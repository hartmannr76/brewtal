/**
 * A small HTTP server that hands its requests to the caller instead of calling it back: the
 * caller polls pending() from its own task, answers with respond(), and so never shares its
 * state with the server's thread. Meant for a page or two on a local network (the WiFi setup
 * page), one request at a time.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace brew::hal {

struct HttpRequest {
    const char * method = "GET";
    const char * path = "/";      // without the query string
    const char * body = "";       // e.g. application/x-www-form-urlencoded for POSTs
    size_t body_len = 0;
};

struct HttpResponse {
    int status = 200;
    const char * content_type = "text/plain";
    const char * body = "";
    size_t body_len = 0;
    const char * location = nullptr;   // for redirects
};

class HttpServer {
public:
    virtual ~HttpServer() = default;
    virtual bool begin() = 0;
    /** A request waiting for an answer, if any. Its strings stay valid until respond(). */
    virtual bool pending(HttpRequest & out) = 0;
    /** Answers the request from pending(). The response is copied before this returns. */
    virtual void respond(const HttpResponse & res) = 0;
};

}  // namespace brew::hal
