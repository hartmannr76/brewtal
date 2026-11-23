/**
 * Panel init sequences: one command per entry. ESP-IDF APIs only (no Arduino).
 */
#pragma once


#include <cstddef>
#include <cstdint>

namespace brew::drivers {

/** One entry of a panel init sequence. */
struct LcdInitCmd {
    uint8_t  cmd;
    uint8_t  len;
    uint8_t  data[16];
    uint16_t delay_ms;
};

}  // namespace brew::drivers
