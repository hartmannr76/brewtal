/**
 * Sitronix ST7701S panel controller: init sequence. ESP-IDF APIs only (no Arduino).
 */
#pragma once

#include "brew/drivers/esp32/lcd_init.h"

#include <cstddef>
#include <cstdint>

namespace brew::drivers {

/** ST7701S "type 5" sequence (from Arduino_GFX), used by the Elecrow CrowPanel 2.1". */
extern const LcdInitCmd kSt7701Type5Init[];
extern const size_t kSt7701Type5InitLen;

}  // namespace brew::drivers
