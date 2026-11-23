/* Host DisplayPanel: renders into an in-memory RGB565 framebuffer (headless sim + bench). */
#pragma once

#include "brew/drivers/interfaces/display.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace brew::sim {

class SimPanel final : public hal::DisplayPanel {
public:
    SimPanel(uint16_t w, uint16_t h) : w_(w), h_(h), fb_(static_cast<size_t>(w) * h, 0) {}

    bool begin() override { return true; }
    uint16_t width() const override { return w_; }
    uint16_t height() const override { return h_; }
    uint32_t drawBufferCaps() const override { return 0; }
    uint16_t drawBufferLines() const override { return 60; }

    void flush(const hal::Area & a, uint16_t * px, FlushDone done, void * ctx) override
    {
        const int w = a.x2 - a.x1 + 1;
        for(int y = a.y1; y <= a.y2; y++) {
            std::memcpy(&fb_[static_cast<size_t>(y) * w_ + a.x1], px, static_cast<size_t>(w) * 2);
            px += w;
        }
        flushed_px += static_cast<uint64_t>(w) * static_cast<uint64_t>(a.y2 - a.y1 + 1);
        flushes++;
        done(ctx);
    }

    const uint16_t * framebuffer() const { return fb_.data(); }

    uint64_t flushed_px = 0;
    uint32_t flushes = 0;

private:
    uint16_t w_, h_;
    std::vector<uint16_t> fb_;
};

}  // namespace brew::sim
