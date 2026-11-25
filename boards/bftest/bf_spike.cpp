/**
 * Brewfather board test (env crowpanel21_bftest): once WiFi is up, makes the calls the Brewfather
 * feature will need and logs what happens, with internal RAM before, at its lowest during, and
 * after each call:
 *   1. GET  api.brewfather.app/v2/batches?status=Fermenting     (HTTPS, Basic auth)
 *   2. GET  the first batch, whole (field names: steps, start date, timestamps)
 *   3. GET  that batch's last reading (does it name the device?)
 *   4. POST log.brewfather.net/stream  (device "BrewDial test", temp 20 C, not attached to a batch)
 * If HTTPS fails with TLS memory in internal RAM (the framework's setting), it retries with mbedTLS
 * allocating from PSRAM. Throwaway: a measurement, not the feature. Credentials come from
 * secrets/brewfather.env via tools/pio_secrets.py and are never printed.
 */
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform.h"
#include "cJSON.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

#define LOG(fmt, ...) std::printf("[%7u][I][bftest] " fmt "\n", static_cast<unsigned>(esp_timer_get_time() / 1000), ##__VA_ARGS__)

unsigned int_free() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024; }
unsigned int_block() { return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024; }

/* lowest internal RAM seen while a call runs, sampled by a small task */
volatile bool s_sampling = false;
volatile size_t s_min_free = 0, s_min_block = 0;
void sampler(void *)
{
    for(;;) {
        if(s_sampling) {
            const size_t f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            const size_t b = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            if(f < s_min_free) s_min_free = f;
            if(b < s_min_block) s_min_block = b;
        }
        vTaskDelay(1);
    }
}

void * psram_calloc(size_t n, size_t sz)
{
    void * p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_calloc(n, sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
void * psram_malloc(size_t sz) { return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

struct Resp {
    int status = -1;
    esp_err_t err = ESP_FAIL;
    std::string body;
    unsigned ms = 0;
};

Resp request(const char * url, esp_http_client_method_t method, const char * auth, const char * post_body)
{
    Resp r;
    esp_http_client_config_t c = {};
    c.url = url;
    c.method = method;
    c.crt_bundle_attach = esp_crt_bundle_attach;
    c.timeout_ms = 20000;
    c.buffer_size = 2048;
    c.buffer_size_tx = 1024;
    s_min_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_min_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    LOG("  before: internal %u KB free, largest block %u KB", int_free(), int_block());
    s_sampling = true;
    const int64_t t0 = esp_timer_get_time();
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if(!h) {
        s_sampling = false;
        LOG("  esp_http_client_init failed");
        return r;
    }
    if(auth) esp_http_client_set_header(h, "authorization", auth);
    const int len = post_body ? static_cast<int>(std::strlen(post_body)) : 0;
    if(post_body) esp_http_client_set_header(h, "Content-Type", "application/json");
    r.err = esp_http_client_open(h, len);
    if(r.err == ESP_OK) {
        if(post_body) esp_http_client_write(h, post_body, len);
        esp_http_client_fetch_headers(h);
        r.status = esp_http_client_get_status_code(h);
        char buf[1024];
        int n;
        while((n = esp_http_client_read(h, buf, sizeof(buf))) > 0) r.body.append(buf, static_cast<size_t>(n));
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    r.ms = static_cast<unsigned>((esp_timer_get_time() - t0) / 1000);
    s_sampling = false;
    LOG("  result: %s, HTTP %d, %u bytes, %u ms", esp_err_to_name(r.err), r.status, static_cast<unsigned>(r.body.size()), r.ms);
    LOG("  lowest during: internal %u KB free, largest block %u KB; after: %u KB free, largest %u KB",
        static_cast<unsigned>(s_min_free / 1024), static_cast<unsigned>(s_min_block / 1024), int_free(), int_block());
    return r;
}

void print_chunked(const char * label, const std::string & s, size_t max)
{
    LOG("  %s (%u of %u bytes):", label, static_cast<unsigned>(s.size() < max ? s.size() : max), static_cast<unsigned>(s.size()));
    for(size_t i = 0; i < s.size() && i < max; i += 200) std::printf("    | %.*s\n", static_cast<int>(s.size() - i < 200 ? s.size() - i : 200), s.c_str() + i);
}

void list_keys(const char * label, const cJSON * o)
{
    std::string keys;
    for(const cJSON * c = o ? o->child : nullptr; c; c = c->next) {
        if(!keys.empty()) keys += ", ";
        keys += c->string ? c->string : "?";
    }
    LOG("  %s keys: %s", label, keys.c_str());
}

void spike(void *)
{
    /* wait for an IP on the home network */
    esp_netif_t * sta = nullptr;
    esp_netif_ip_info_t ip = {};
    for(;;) {
        if(!sta) sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if(sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    vTaskDelay(pdMS_TO_TICKS(5000));   /* let boot and the first frames settle */
    LOG("online; starting the Brewfather board test");

    cJSON_Hooks hooks = { psram_malloc, heap_caps_free };   /* JSON trees in PSRAM, not internal RAM */
    cJSON_InitHooks(&hooks);

    /* Basic auth: base64(userid:apikey) */
    const std::string cred = std::string(BF_USER_ID) + ":" + BF_API_KEY;
    unsigned char b64[256];
    size_t olen = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &olen, reinterpret_cast<const unsigned char *>(cred.c_str()), cred.size());
    const std::string auth = "Basic " + std::string(reinterpret_cast<char *>(b64), olen);

    LOG("1. GET /v2/batches?status=Fermenting (TLS memory: framework default, internal RAM)");
    const char * list_url = "https://api.brewfather.app/v2/batches?status=Fermenting&limit=10";
    Resp r = request(list_url, HTTP_METHOD_GET, auth.c_str(), nullptr);
    if(r.err != ESP_OK) {
        LOG("1b. retrying with mbedTLS allocating from PSRAM (mbedtls_platform_set_calloc_free)");
        const int rc = mbedtls_platform_set_calloc_free(psram_calloc, heap_caps_free);
        LOG("  mbedtls_platform_set_calloc_free -> %d", rc);
        r = request(list_url, HTTP_METHOD_GET, auth.c_str(), nullptr);
    }
    print_chunked("batch list", r.body, 1200);

    std::string id;
    auto first_id = [&](const std::string & body, const char * what) {
        if(cJSON * list = cJSON_Parse(body.c_str())) {
            const cJSON * first = cJSON_IsArray(list) ? cJSON_GetArrayItem(list, 0) : nullptr;
            const cJSON * jid = first ? cJSON_GetObjectItem(first, "_id") : nullptr;
            if(cJSON_IsString(jid)) id = jid->valuestring;
            LOG("  %d %s batch(es); first _id %s", cJSON_IsArray(list) ? cJSON_GetArraySize(list) : -1, what,
                id.empty() ? "(none)" : "found");
            cJSON_Delete(list);
        }
    };
    first_id(r.body, "fermenting");
    if(id.empty()) {
        LOG("1c. no Fermenting batch: GET the most recent batch in any status");
        r = request("https://api.brewfather.app/v2/batches?limit=5&order_by=_id&order_by_direction=desc", HTTP_METHOD_GET,
                    auth.c_str(), nullptr);
        print_chunked("recent batches", r.body, 1200);
        first_id(r.body, "recent");
    }

    if(!id.empty()) {
        LOG("2. GET /v2/batches/<first> (whole batch)");
        r = request(("https://api.brewfather.app/v2/batches/" + id).c_str(), HTTP_METHOD_GET, auth.c_str(), nullptr);
        if(cJSON * b = cJSON_Parse(r.body.c_str())) {
            list_keys("batch", b);
            const cJSON * fsd = cJSON_GetObjectItem(b, "fermentationStartDate");
            if(cJSON_IsNumber(fsd)) LOG("  fermentationStartDate = %.0f", fsd->valuedouble);
            for(const cJSON * c = b->child; c; c = c->next)
                if(c->string && std::strstr(c->string, "imestamp")) {
                    char * v = cJSON_PrintUnformatted(c);
                    LOG("  %s = %s", c->string, v ? v : "?");
                    cJSON_free(v);
                }
            const cJSON * recipe = cJSON_GetObjectItem(b, "recipe");
            const cJSON * ferm = recipe ? cJSON_GetObjectItem(recipe, "fermentation") : nullptr;
            if(ferm) {
                char * v = cJSON_PrintUnformatted(ferm);
                print_chunked("recipe.fermentation", v ? std::string(v) : std::string(), 3000);
                cJSON_free(v);
            }
            else {
                LOG("  no recipe.fermentation in the batch");
            }
            cJSON_Delete(b);
        }
        else {
            LOG("  batch JSON did not parse");
        }

        LOG("3. GET /v2/batches/<first>/readings/last");
        r = request(("https://api.brewfather.app/v2/batches/" + id + "/readings/last").c_str(), HTTP_METHOD_GET, auth.c_str(),
                    nullptr);
        print_chunked("last reading", r.body, 1200);
    }

    LOG("4. POST log.brewfather.net/stream (device \"BrewDial test\")");
    const std::string stream = std::string("https://log.brewfather.net/stream?id=") + BF_STREAM_ID;
    r = request(stream.c_str(), HTTP_METHOD_POST, nullptr,
                "{\"name\":\"BrewDial test\",\"temp\":20.0,\"temp_unit\":\"C\",\"comment\":\"BrewDial connectivity test\"}");
    print_chunked("stream response", r.body, 400);

    LOG("Brewfather board test done");
    vTaskDelete(nullptr);
}

}  // namespace

/* Called from boards/crowpanel21/main.cpp when BREW_BF_SPIKE is set (env crowpanel21_bftest). */
void bfSpikeStart()
{
    xTaskCreatePinnedToCore(sampler, "bfsample", 2048, nullptr, 1, nullptr, 0);
    /* the test's own stack in PSRAM: internal RAM is what's being measured */
    if(xTaskCreatePinnedToCoreWithCaps(spike, "bftest", 12288, nullptr, 3, nullptr, 0, MALLOC_CAP_SPIRAM) != pdPASS)
        LOG("could not start the test task");
}
