#if defined(ESP_PLATFORM)

#include "brew/app/runtime.h"

#include "brew/app/bench.h"
#include "brew/app/lvgl_port.h"
#include "brew/app/ui_bridge.h"
#include "brew/local_bus.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstdlib>

/* ESP_LOGI is compiled out in this framework build (error level only): print directly. */
#define RT_LOG(lvl, fmt, ...) \
    std::printf("[%7u][" lvl "][brewdial] " fmt "\n", static_cast<unsigned>(esp_timer_get_time() / 1000), ##__VA_ARGS__)

namespace brew::app {

namespace {


constexpr uint32_t kInputPeriodMs = 5;
constexpr uint32_t kTouchEvery = 2;   // touch sampled every 10 ms

uint32_t millis32() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

/* Logs every command on its way out, like the old sketch's Serial prints. */
class LoggingUiPort final : public UiPort {
public:
    explicit LoggingUiPort(UiPort & inner) : in_(inner) {}
    void send(const Command & c) override
    {
        switch(c.kind) {
            case Command::Kind::SetSetpoint:
                RT_LOG("I", "setpoint[%d] = %.2f C (%.1f F)", static_cast<int>(c.mode), c.setpoint_c, c_to_f(c.setpoint_c));
                break;
            case Command::Kind::SetMode:
                RT_LOG("I", "mode=%d paused=%d", static_cast<int>(c.mode), c.paused);
                break;
            case Command::Kind::SetUnits:
                RT_LOG("I", "celsius=%d", c.celsius);
                break;
            case Command::Kind::StartDayCount:
                RT_LOG("I", "day count restarted");
                break;
            case Command::Kind::SetProfile:
            case Command::Kind::SetStep:
            case Command::Kind::SetStepCount:
                RT_LOG("I", "schedule command %d", static_cast<int>(c.kind));
                break;
            case Command::Kind::SetTuning:
                RT_LOG("I", "setting %d = %ld", static_cast<int>(c.tuning), static_cast<long>(c.value));
                break;
            case Command::Kind::SetClock:
                if(!clock_logged_) RT_LOG("I", "sending the controller the time (%lu)", static_cast<unsigned long>(static_cast<uint32_t>(c.value)));
                clock_logged_ = true;
                break;
            case Command::Kind::RequestSync:
                break;
        }
        in_.send(c);
    }
    bool pollState(State & s) override { return in_.pollState(s); }
    bool pollHistory(HistoryChunk & h) override { return in_.pollHistory(h); }
    bool pollSettings(Settings & s) override { return in_.pollSettings(s); }
    void service(uint32_t now) override { in_.service(now); }

private:
    UiPort & in_;
    bool clock_logged_ = false;
};

struct DisplayCtx {
    DisplayConfig cfg;
    LvglPort lvgl;
    UiBridge * bridge = nullptr;
    LoggingUiPort * port = nullptr;
    KnobInput * knob = nullptr;
    TouchTracker touch;
    SpscRing<InputEvent, 64> events;   // input task -> ui task
    FrameMeter meter;
    FrameWindow beat_frames;           // the heartbeat's frame statistics
    Bench * bench = nullptr;           // cfg.bench: the scripted UI benchmark
};

DisplayCtx * s_disp;

void input_task(void *)
{
    DisplayCtx & d = *s_disp;
    hal::TouchPoint tp;
    uint32_t n = 0;
    TickType_t wake = xTaskGetTickCount();
    for(;;) {
        const uint32_t now = millis32();
        if(d.knob) d.knob->poll(now, [&](const InputEvent & e) { d.events.push(e); });
        if(d.cfg.touch && ++n % kTouchEvery == 0 && d.cfg.touch->read(tp))
            d.touch.step(tp, [&](const InputEvent & e) { d.events.push(e); });
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(kInputPeriodMs));
    }
}

int64_t micros64() { return esp_timer_get_time(); }
void bench_log(const char * line) { RT_LOG("I", "%s", line); }

const char * phase_name(int p)
{
    static const char * const n[] = { "off", "setup network up", "phone joined", "connecting", "connected", "offline" };
    return p >= 0 && p < 6 ? n[p] : "?";
}

void ui_task(void *)
{
    DisplayCtx & d = *s_disp;
    uint32_t beat = 0, loops = 0, presses = 0;
    for(;;) {
        InputEvent e;
        while(d.events.pop(e)) {
            if(e.kind == InputEvent::Kind::Press) {
                presses++;
                RT_LOG("I", "knob press");
            }
            if(e.kind == InputEvent::Kind::Rotate || e.kind == InputEvent::Kind::Press) d.bridge->handle(e);
            else d.lvgl.pushTouch(e);
        }
        loops++;
        if(millis32() - beat >= 5000) {   // heartbeat: if these stop, the UI task is stuck
            beat = millis32();
            const FrameWindow & fs = d.beat_frames;
            const unsigned avg_ms = fs.frames ? static_cast<unsigned>(fs.sum_us / fs.frames / 1000u) : 0u;
            if(d.cfg.wifi) {
                const net::SetupStatus & w = d.cfg.wifi->status();
                RT_LOG("I", "ui alive (%u loops, %u presses, %u frames: avg %u ms, max %u ms); wifi: %s, %d phone(s), ssid '%s'",
                       static_cast<unsigned>(loops), static_cast<unsigned>(presses), static_cast<unsigned>(fs.frames), avg_ms,
                       static_cast<unsigned>(fs.max_us / 1000u), phase_name(static_cast<int>(w.phase)), w.clients, w.ssid);
            }
            else {
                RT_LOG("I", "ui alive (%u loops, %u presses, %u frames: avg %u ms, max %u ms)", static_cast<unsigned>(loops),
                       static_cast<unsigned>(presses), static_cast<unsigned>(fs.frames), avg_ms,
                       static_cast<unsigned>(fs.max_us / 1000u));
            }
            loops = 0;
            d.beat_frames.clear();
        }
        const uint32_t now = millis32();
        if(d.cfg.wifi) d.cfg.wifi->tick(now);
        if(d.cfg.portal && d.cfg.http) d.cfg.portal->serve(*d.cfg.http, now);
        d.bridge->poll(now);
        if(d.bench) d.bench->step(now);
        const uint32_t wait = lv_timer_handler();
        /* run at least every 5 ms so knob events are applied promptly */
        vTaskDelay(pdMS_TO_TICKS(wait > 5 ? 5 : (wait < 1 ? 1 : wait)));
    }
}

uint32_t lv_tick_cb() { return millis32(); }

/* Internal RAM left: task stacks, DMA buffers and the WiFi driver all need it, and running out
 * fails quietly (a task that is never created), so startup reports it at each stage. */
void log_ram(const char * stage)
{
    RT_LOG("I", "internal RAM after %s: %u KB free, largest block %u KB", stage,
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}

[[noreturn]] void fatal(const char * msg)
{
    RT_LOG("E", "FATAL: %s", msg);
    for(;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

}  // namespace

void startDisplay(const DisplayConfig & cfg, UiPort & port)
{
    if(!cfg.panel) fatal("startDisplay: no panel");
    s_disp = new DisplayCtx();
    DisplayCtx & d = *s_disp;
    d.cfg = cfg;
    log_ram("start");

    if(!cfg.panel->begin()) RT_LOG("E", "display panel begin() failed");
    if(cfg.touch && !cfg.touch->begin()) RT_LOG("W", "touch controller not found");
    if(cfg.encoder && !cfg.encoder->begin()) RT_LOG("W", "encoder init failed");

    lv_init();
    lv_tick_set_cb(lv_tick_cb);

    /* Render into small buffers in internal SRAM (much faster than PSRAM for LVGL's blending);
     * the panel driver copies each dirty band out. */
    const size_t bytes = static_cast<size_t>(cfg.panel->width()) * cfg.panel->drawBufferLines() * 2;
    void * b1 = heap_caps_malloc(bytes, cfg.panel->drawBufferCaps());
    void * b2 = heap_caps_malloc(bytes, cfg.panel->drawBufferCaps());
    if(!b1 || !b2) fatal("draw buffer allocation failed");
    lv_display_t * disp = d.lvgl.begin(*cfg.panel, b1, b2, bytes, cfg.refresh_ms);
    d.meter.attach(disp, micros64);
    d.meter.add(&d.beat_frames);
    if(cfg.touch) d.lvgl.addTouch();
    log_ram("panel + draw buffers");

    d.port = new LoggingUiPort(port);
    d.bridge = new UiBridge(*d.port);
    d.bridge->create(lv_screen_active());
    if(cfg.bench) d.bench = new Bench(*d.bridge, d.meter, bench_log, cfg.wifi);
    log_ram("screens");
    if(cfg.wifi) {
        cfg.wifi->begin(millis32());   // starts the radio; opens setup if nothing is saved
        d.bridge->attachWifi(cfg.wifi);
        d.bridge->attachClock(cfg.clock);
        if(cfg.http && !cfg.http->begin()) RT_LOG("E", "setup page server failed to start");
        log_ram("WiFi + setup page server");
    }

    /* First frame before the backlight, so there is no flash of garbage. Give the controller
     * a moment to answer the sync request so the first frame already shows real values. */
    for(int i = 0; i < 20; i++) {
        d.bridge->poll(millis32());
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    lv_refr_now(nullptr);
    vTaskDelay(pdMS_TO_TICKS(50));
    if(cfg.backlight) cfg.backlight->set(cfg.backlight_level);
    if(cfg.after_first_frame) cfg.after_first_frame();

    if(cfg.encoder || cfg.button) {
        d.knob = new KnobInput(cfg.encoder, cfg.button, cfg.knob);
        d.knob->begin();
    }

    if(xTaskCreatePinnedToCore(input_task, "input", 4096, nullptr, 5, nullptr, 0) != pdPASS)
        fatal("no RAM for the input task");
    if(xTaskCreatePinnedToCore(ui_task, "ui", 16384, nullptr, 2, nullptr, 1) != pdPASS)
        fatal("no RAM for the UI task: the screen will not respond");
    RT_LOG("I", "display ready (%ux%u, PSRAM free %u KB, internal free %u KB, largest block %u KB)",
             cfg.panel->width(), cfg.panel->height(),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}

}  // namespace brew::app

#endif  // ESP_PLATFORM
