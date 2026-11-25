/**
 * A small read-only JSON scanner for API responses (Brewfather): finds values inside the text in
 * place, no allocation, no tree. Lookups rescan from the start of the object or array, which is
 * fine for responses of a few tens of KB read a handful of times.
 *
 *   Value root = json::parse(body, len);
 *   Value step = json::at(json::get(json::get(root, "fermentation"), "steps"), 0);
 *   double c = json::number(json::get(step, "stepTemp"));
 *
 * Anything missing or malformed gives an empty Value; reading an empty Value gives the fallback.
 */
#pragma once

#include <cstddef>

namespace brew::net::json {

/** One value inside the text: [p, p + n). n == 0: not found / not valid. */
struct Value {
    const char * p = nullptr;
    size_t n = 0;
    explicit operator bool() const { return n > 0; }
};

/** The document's top-level value (whitespace around it ignored); empty if it isn't valid JSON. */
Value parse(const char * text, size_t len);

/** An object's member `key` (keys compared as written, without escapes). */
Value get(Value object, const char * key);
/** An array's element `i`. */
Value at(Value array, size_t i);
/** How many elements an array, or members an object, has (0 for anything else). */
size_t size(Value container);

bool isNull(Value v);
bool isString(Value v);
bool isNumber(Value v);
bool isArray(Value v);
bool isObject(Value v);

double number(Value v, double fallback = 0.0);
/** A string's text, unescaped (\u escapes become UTF-8), cut to fit `n` (never mid-character).
 *  False (and "") if `v` isn't a string. */
bool string(Value v, char * out, size_t n);
/** Whether `v` is the string `s`. */
bool equals(Value v, const char * s);

}  // namespace brew::net::json
