#include "brew/network/brewfather.h"

#include "brew/network/json_scan.h"

#include <cstdio>
#include <cstring>

namespace brew::net {

namespace {
constexpr const char * kKeyUser = "bf_user";
constexpr const char * kKeyKey = "bf_key";
constexpr const char * kKeyStream = "bf_stream";
constexpr const char * kKeyBatch = "bf_batch";     // the imported batch's id
constexpr const char * kKeyBatchMod = "bf_bmod";   // ...and its _timestamp_ms then (decimal text)

/* Has `t` been reached? Unsigned millis: right across the 49-day wrap. */
bool reached(uint32_t now, uint32_t t) { return static_cast<int32_t>(now - t) >= 0; }

void copy(char * d, size_t n, const char * s) { std::snprintf(d, n, "%s", s ? s : ""); }
}  // namespace

const char * accountName(BfAccount a)
{
    switch(a) {
        case BfAccount::NotSetUp:    return "not_set_up";
        case BfAccount::Checking:    return "checking";
        case BfAccount::Connected:   return "connected";
        case BfAccount::Rejected:    return "rejected";
        case BfAccount::Unreachable: return "unreachable";
    }
    return "not_set_up";
}

bool Brewfather::base64(const char * in, size_t len, char * out, size_t n)
{
    static const char kAbc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t need = (len + 2) / 3 * 4 + 1;
    if(!out || n < need) return false;
    size_t o = 0;
    for(size_t i = 0; i < len; i += 3) {
        const uint32_t b = (static_cast<uint32_t>(static_cast<unsigned char>(in[i])) << 16) |
                           (i + 1 < len ? static_cast<uint32_t>(static_cast<unsigned char>(in[i + 1])) << 8 : 0u) |
                           (i + 2 < len ? static_cast<uint32_t>(static_cast<unsigned char>(in[i + 2])) : 0u);
        out[o++] = kAbc[(b >> 18) & 63];
        out[o++] = kAbc[(b >> 12) & 63];
        out[o++] = i + 1 < len ? kAbc[(b >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? kAbc[b & 63] : '=';
    }
    out[o] = '\0';
    return true;
}

Brewfather::Brewfather(hal::HttpClient & http, hal::SettingsStore * store, const Config & cfg)
    : http_(http), store_(store), cfg_(cfg)
{
}

void Brewfather::begin(const char * device, uint32_t now)
{
    /* Brewfather keys a stream device by its name in capitals, letters and digits only
     * ("BrewDial test" became BREWDIALTEST); keep the name to those so the key is predictable. */
    size_t d = 0, k = 0;
    for(const char * p = device ? device : ""; *p && d + 1 < sizeof(st_.device); p++) {
        const char c = *p;
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if(!alnum) continue;
        st_.device[d++] = c;
        if(k + 1 < sizeof(key_upper_)) key_upper_[k++] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
    }
    st_.device[d] = '\0';
    key_upper_[k] = '\0';
    if(store_) {
        if(!store_->getStr(kKeyUser, user_, sizeof(user_))) user_[0] = '\0';
        if(!store_->getStr(kKeyKey, key_, sizeof(key_))) key_[0] = '\0';
        if(!store_->getStr(kKeyStream, stream_, sizeof(stream_))) stream_[0] = '\0';
        if(!store_->getStr(kKeyBatch, imported_id_, sizeof(imported_id_))) imported_id_[0] = '\0';
    }
    st_.user_set = user_[0] != '\0';
    st_.key_set = key_[0] != '\0';
    st_.stream_set = stream_[0] != '\0';
    st_.account = haveAccount() ? BfAccount::Checking : BfAccount::NotSetUp;
    check_at_ = now;
    post_at_ = now + cfg_.first_post_ms;
}

void Brewfather::save()
{
    if(!store_) return;
    store_->setStr(kKeyUser, user_);
    store_->setStr(kKeyKey, key_);
    store_->setStr(kKeyStream, stream_);
    store_->commit();
}

void Brewfather::setCredentials(const char * user, const char * key, const char * stream, uint32_t now)
{
    if(user && user[0]) copy(user_, sizeof(user_), user);
    if(key && key[0]) copy(key_, sizeof(key_), key);
    if(stream && stream[0]) {
        /* the whole logging URL may be pasted: keep the id */
        const char * id = std::strstr(stream, "id=");
        copy(stream_, sizeof(stream_), id ? id + 3 : stream);
        if(char * amp = std::strchr(stream_, '&')) *amp = '\0';
    }
    save();
    st_.user_set = user_[0] != '\0';
    st_.key_set = key_[0] != '\0';
    st_.stream_set = stream_[0] != '\0';
    st_.account = haveAccount() ? BfAccount::Checking : BfAccount::NotSetUp;
    st_.attached = false;
    st_.batch[0] = '\0';
    st_.http = 0;
    check_at_ = now;
    post_at_ = now + cfg_.first_post_ms;
}

void Brewfather::forget()
{
    user_[0] = key_[0] = stream_[0] = '\0';
    save();
    const BrewfatherStatus fresh;
    char device[sizeof(st_.device)];
    copy(device, sizeof(device), st_.device);
    st_ = fresh;
    copy(st_.device, sizeof(st_.device), device);
}

void Brewfather::checkNow(uint32_t now) { check_at_ = now; }

void Brewfather::fetchBatch(int i, uint32_t now)
{
    (void)now;
    BfPreview & p = st_.preview;
    p = BfPreview{};
    if(i < 0 || i >= st_.nbatches || !haveAccount()) {
        p.state = BfPreview::State::Failed;
        st_.rev++;
        return;
    }
    copy(p.id, sizeof(p.id), st_.batches[i].id);
    copy(p.label, sizeof(p.label), st_.batches[i].label);
    p.state = BfPreview::State::Loading;
    fetch_due_ = true;
    st_.rev++;
}

void Brewfather::imported()
{
    const BfPreview & p = st_.preview;
    if(p.state != BfPreview::State::Ready) return;
    copy(imported_id_, sizeof(imported_id_), p.id);
    if(!store_) return;
    char mod[24];
    std::snprintf(mod, sizeof(mod), "%lld", static_cast<long long>(p.modified_unix_ms));
    store_->setStr(kKeyBatch, imported_id_);
    store_->setStr(kKeyBatchMod, mod);
    store_->commit();
}

void Brewfather::shortName(const char * batch_name, const char * recipe_name, char * out, size_t n)
{
    if(!out || n == 0) return;
    const char * s = batch_name && batch_name[0] && std::strcmp(batch_name, "Batch") != 0 ? batch_name : recipe_name;
    if(!s) s = "";
    if(std::strncmp(s, "(Modified) ", 11) == 0) s += 11;
    size_t k = 0;
    while(s[k] && k + 1 < n) k++;
    if(s[k])   /* cut short: don't leave half a UTF-8 character, nor a trailing space */
        while(k > 0 && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80) k--;
    while(k > 0 && s[k - 1] == ' ') k--;
    std::memcpy(out, s, k);
    out[k] = '\0';
}

void Brewfather::setReading(bool valid, float temp_c, float target_c, const char * state)
{
    reading_valid_ = valid;
    temp_c_ = temp_c;
    target_c_ = target_c;
    copy(state_, sizeof(state_), state ? state : "idle");
}

int64_t Brewfather::lastLogAgeS(int64_t unix_ms) const
{
    if(!st_.last_log_unix_ms || unix_ms <= 0) return -1;
    const int64_t d = unix_ms - st_.last_log_unix_ms;
    return d < 0 ? 0 : d / 1000;
}

void Brewfather::tick(uint32_t now, bool online, int64_t unix_ms)
{
    if(unix_ms > 0) unix_ms_ = unix_ms;
    if(pending_ != Pending::None) {   /* one request at a time: wait for its result */
        hal::HttpClientResult r;
        if(!http_.poll(r)) return;
        const Pending done = pending_;
        pending_ = Pending::None;
        if(done == Pending::Check) finishCheck(r, now);
        else if(done == Pending::Fetch) finishFetch(r);
        else finishPost(r, now);
        return;
    }
    if(!online) return;
    if(fetch_due_ && haveAccount()) {   /* the user is waiting on this one */
        char url[sizeof(url_)];
        std::snprintf(url, sizeof(url), "%s/v2/batches/%s", cfg_.api, st_.preview.id);
        if(startGet(url)) {
            pending_ = Pending::Fetch;
            fetch_due_ = false;
        }
    }
    else if(haveAccount() && reached(now, check_at_)) startCheck(now);
    else if(stream_[0] && reading_valid_ && reached(now, post_at_)) startPost(now);
}

bool Brewfather::startGet(const char * url)
{
    char cred[sizeof(user_) + sizeof(key_) + 1];
    std::snprintf(cred, sizeof(cred), "%s:%s", user_, key_);
    char b64[sizeof(auth_) - 8];
    if(!base64(cred, std::strlen(cred), b64, sizeof(b64))) return false;
    std::snprintf(auth_, sizeof(auth_), "Basic %s", b64);
    copy(url_, sizeof(url_), url);
    hal::HttpClientRequest req;
    req.method = "GET";
    req.url = url_;
    req.authorization = auth_;
    return http_.start(req);
}

void Brewfather::finishFetch(const hal::HttpClientResult & r)
{
    BfPreview & p = st_.preview;
    st_.rev++;
    if(p.state != BfPreview::State::Loading) return;   // a newer pick, or forgotten
    st_.http = r.status;
    const json::Value b = r.status == 200 ? json::parse(r.body, r.body_len) : json::Value{};
    const json::Value steps = json::get(json::get(json::get(b, "recipe"), "fermentation"), "steps");
    if(!json::isArray(steps)) {
        p.state = BfPreview::State::Failed;
        return;
    }
    char batch_name[48] = { 0 }, recipe_name[48] = { 0 };
    json::string(json::get(b, "name"), batch_name, sizeof(batch_name));
    json::string(json::get(json::get(b, "recipe"), "name"), recipe_name, sizeof(recipe_name));
    shortName(batch_name, recipe_name, p.name, sizeof(p.name));
    p.total_steps = static_cast<int>(json::size(steps));
    p.nsteps = 0;
    for(int i = 0; i < p.total_steps && p.nsteps < BfPreview::kMaxSteps; i++) {
        const json::Value s = json::at(steps, static_cast<size_t>(i));
        BfStep & o = p.steps[p.nsteps++];
        json::string(json::get(s, "type"), o.type, sizeof(o.type));
        o.temp_c = static_cast<float>(json::number(json::get(s, "stepTemp"), 20.0));
        /* days -> whole hours (ramp null = a jump) */
        const double ramp = json::number(json::get(s, "ramp"), 0.0) * 24.0, hold = json::number(json::get(s, "stepTime"), 0.0) * 24.0;
        o.ramp_h = static_cast<uint16_t>(ramp <= 0 ? 0 : ramp > 65535 ? 65535 : ramp + 0.5);
        o.hold_h = static_cast<uint16_t>(hold <= 0 ? 0 : hold > 65535 ? 65535 : hold + 0.5);
    }
    p.start_unix_ms = static_cast<int64_t>(json::number(json::get(b, "fermentationStartDate"), 0.0));
    p.modified_unix_ms = static_cast<int64_t>(json::number(json::get(b, "_timestamp_ms"), 0.0));
    p.state = p.nsteps > 0 ? BfPreview::State::Ready : BfPreview::State::Failed;
}

void Brewfather::startCheck(uint32_t now)
{
    char url[sizeof(url_)];
    std::snprintf(url, sizeof(url), "%s/v2/batches?status=Fermenting&include=devices&limit=50", cfg_.api);
    if(startGet(url)) pending_ = Pending::Check;   // else the client is busy: next tick
    (void)now;
}

void Brewfather::finishCheck(const hal::HttpClientResult & r, uint32_t now)
{
    if(!haveAccount()) return;   // forgotten while it ran
    st_.http = r.status;
    if(r.status == 401 || r.status == 403) {
        st_.account = BfAccount::Rejected;
        st_.attached = false;
        st_.batch[0] = '\0';
        check_at_ = now + cfg_.check_every_ms;
        return;
    }
    if(r.status != 200) {   /* no answer or a server problem: what we knew still stands */
        st_.account = BfAccount::Unreachable;
        check_at_ = now + cfg_.retry_ms;
        return;
    }
    st_.account = BfAccount::Connected;
    check_at_ = now + cfg_.check_every_ms;
    st_.attached = false;
    st_.batch[0] = '\0';
    const json::Value list = json::parse(r.body, r.body_len);
    /* the Fermenting batches, for Profile › Brewfather */
    st_.nbatches = 0;
    for(size_t i = 0, n = json::size(list); i < n && st_.nbatches < BrewfatherStatus::kMaxBatches; i++) {
        const json::Value b = json::at(list, i);
        BfBatch & o = st_.batches[st_.nbatches];
        if(!json::string(json::get(b, "_id"), o.id, sizeof(o.id)) || !o.id[0]) continue;
        char batch_name[48] = { 0 }, recipe_name[48] = { 0 }, name[40];
        json::string(json::get(b, "name"), batch_name, sizeof(batch_name));
        json::string(json::get(json::get(b, "recipe"), "name"), recipe_name, sizeof(recipe_name));
        shortName(batch_name, recipe_name, name, sizeof(name));
        const json::Value no = json::get(b, "batchNo");
        if(json::isNumber(no)) std::snprintf(o.label, sizeof(o.label), "#%d %s", static_cast<int>(json::number(no)), name);
        else copy(o.label, sizeof(o.label), name);
        st_.nbatches++;
    }
    st_.rev++;
    for(size_t i = 0, n = json::size(list); i < n && !st_.attached; i++) {
        const json::Value b = json::at(list, i);
        const json::Value items = json::get(json::get(json::get(b, "devices"), "stream"), "items");
        for(size_t j = 0, m = json::size(items); j < m; j++) {
            const json::Value dev = json::at(items, j);
            if(!json::equals(json::get(dev, "key"), key_upper_)) continue;
            st_.attached = true;
            char batch_name[48] = { 0 }, recipe_name[48] = { 0 }, name[48];
            json::string(json::get(b, "name"), batch_name, sizeof(batch_name));
            json::string(json::get(json::get(b, "recipe"), "name"), recipe_name, sizeof(recipe_name));
            shortName(batch_name, recipe_name, name, sizeof(name));
            const json::Value no = json::get(b, "batchNo");
            if(json::isNumber(no)) std::snprintf(st_.batch, sizeof(st_.batch), "#%d %s", static_cast<int>(json::number(no)), name);
            else copy(st_.batch, sizeof(st_.batch), name);
            const double last = json::number(json::get(dev, "lastLog"), 0.0);
            if(last > 0.0) st_.last_log_unix_ms = static_cast<int64_t>(last);
            break;
        }
    }
}

void Brewfather::startPost(uint32_t now)
{
    std::snprintf(url_, sizeof(url_), "%s/stream?id=%s", cfg_.stream, stream_);
    const int n = std::snprintf(body_, sizeof(body_),
                                "{\"name\":\"%s\",\"temp\":%.2f,\"temp_unit\":\"C\",\"temp_target\":%.2f,\"device_state\":\"%s\"}",
                                st_.device, static_cast<double>(temp_c_), static_cast<double>(target_c_), state_);
    hal::HttpClientRequest req;
    req.method = "POST";
    req.url = url_;
    req.content_type = "application/json";
    req.body = body_;
    req.body_len = n > 0 ? static_cast<size_t>(n) : 0;
    if(http_.start(req)) pending_ = Pending::Post;
    (void)now;
}

void Brewfather::finishPost(const hal::HttpClientResult & r, uint32_t now)
{
    st_.last_post_ms = now;
    st_.last_post_http = r.status;
    const bool ok = r.status >= 200 && r.status < 300;
    post_at_ = now + (ok ? cfg_.post_every_ms : cfg_.retry_ms);
    /* see whether it landed: Brewfather lists our device on the batch it's attached to */
    if(ok && haveAccount()) {
        const uint32_t soon = now + cfg_.check_after_post_ms;
        if(static_cast<int32_t>(check_at_ - soon) > 0) check_at_ = soon;
    }
}

}  // namespace brew::net
