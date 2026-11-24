#include "brew/drivers/esp32/udp_responder.h"
#include "brew/drivers/esp32/log.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <cstdio>
#include <cstring>

namespace brew::drivers {

namespace {
const char * TAG = "brew.udp";

void udp_task(void * arg)
{
    static_cast<EspUdpResponder *>(arg)->run();
}
}  // namespace

bool EspUdpResponder::begin()
{
    if(sock_ >= 0) return true;
    sock_ = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(sock_ < 0) {
        BREW_LOGE(TAG, "socket failed (errno %d)", errno);
        return false;
    }
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port_);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if(lwip_bind(sock_, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0) {
        BREW_LOGE(TAG, "bind to port %u failed (errno %d)", port_, errno);
        lwip_close(sock_);
        sock_ = -1;
        return false;
    }
    xTaskCreate(udp_task, "udp", 4096, this, 3, nullptr);
    BREW_LOGI(TAG, "answering UDP port %u", port_);
    return true;
}

void EspUdpResponder::run()
{
    static uint8_t in[512], out[512];
    for(;;) {
        sockaddr_in from = {};
        socklen_t fl = sizeof(from);
        const int n = lwip_recvfrom(sock_, in, sizeof(in), 0, reinterpret_cast<sockaddr *>(&from), &fl);
        if(n <= 0) continue;
        const uint32_t s = ntohl(from.sin_addr.s_addr);
        const uint8_t src[4] = { static_cast<uint8_t>(s >> 24), static_cast<uint8_t>(s >> 16),
                                 static_cast<uint8_t>(s >> 8), static_cast<uint8_t>(s) };
        const size_t len = answer_(ctx_, in, static_cast<size_t>(n), src, out, sizeof(out));
        if(len > 0) lwip_sendto(sock_, out, len, 0, reinterpret_cast<sockaddr *>(&from), fl);
    }
}

}  // namespace brew::drivers
