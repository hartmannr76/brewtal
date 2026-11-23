#include "brew/drivers/esp32/uart.h"

#include "driver/uart.h"
#include "esp_log.h"

#include <cstdio>
#include <cstring>

namespace brew::drivers {

bool EspUartStream::begin()
{
    const auto port = static_cast<uart_port_t>(cfg_.port);
    uart_config_t c = {};
    c.baud_rate = static_cast<int>(cfg_.baud);
    c.data_bits = UART_DATA_8_BITS;
    c.parity = UART_PARITY_DISABLE;
    c.stop_bits = UART_STOP_BITS_1;
    c.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    c.source_clk = UART_SCLK_DEFAULT;
    if(uart_driver_install(port, static_cast<int>(cfg_.rx_buffer), static_cast<int>(cfg_.rx_buffer), 0, nullptr, 0) != ESP_OK)
        return false;
    if(uart_param_config(port, &c) != ESP_OK) return false;
    if(uart_set_pin(port, cfg_.tx, cfg_.rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) return false;
    ready_ = true;
    return true;
}

size_t EspUartStream::write(const uint8_t * d, size_t n)
{
    if(!ready_) return 0;
    const int w = uart_write_bytes(static_cast<uart_port_t>(cfg_.port), d, n);
    return w < 0 ? 0 : static_cast<size_t>(w);
}

size_t EspUartStream::read(uint8_t * buf, size_t max)
{
    if(!ready_) return 0;
    const int r = uart_read_bytes(static_cast<uart_port_t>(cfg_.port), buf, static_cast<uint32_t>(max), 0);
    return r < 0 ? 0 : static_cast<size_t>(r);
}

}  // namespace brew::drivers
