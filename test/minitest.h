/* Dependency-free test harness: TEST(name) { CHECK(...); CHECK_EQ(a, b); } */
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace mt {

struct Case {
    const char * name;
    void (*fn)();
};

inline std::vector<Case> & registry()
{
    static std::vector<Case> r;
    return r;
}

inline int & failures()
{
    static int f = 0;
    return f;
}

struct Reg {
    Reg(const char * n, void (*f)()) { registry().push_back({ n, f }); }
};

template<typename T>
std::string show(const T & v)
{
    std::ostringstream o;
    o << v;
    return o.str();
}
inline std::string show(uint8_t v) { return show(static_cast<int>(v)); }
inline std::string show(int8_t v) { return show(static_cast<int>(v)); }

inline void fail(const char * file, int line, const std::string & msg)
{
    std::printf("    %s:%d: %s\n", file, line, msg.c_str());
    failures()++;
}

inline int run_all()
{
    int failed_cases = 0;
    for(const Case & c : registry()) {
        const int before = failures();
        c.fn();
        const bool ok = failures() == before;
        if(!ok) failed_cases++;
        std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", c.name);
        std::fflush(stdout);   /* so a crash still shows the cases before it */
    }
    std::printf("\n%d/%d test cases passed\n", static_cast<int>(registry().size()) - failed_cases,
                static_cast<int>(registry().size()));
    return failed_cases ? 1 : 0;
}

}  // namespace mt

#define MT_CAT2(a, b) a##b
#define MT_CAT(a, b) MT_CAT2(a, b)
#define TEST(name)                                              \
    static void name();                                         \
    static mt::Reg MT_CAT(mt_reg_, name)(#name, &name);         \
    static void name()

#define CHECK(cond) \
    do { if(!(cond)) mt::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); } while(0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        const auto & mt_a = (a);                                                                    \
        const auto & mt_b = (b);                                                                    \
        if(!(mt_a == mt_b))                                                                         \
            mt::fail(__FILE__, __LINE__, "CHECK_EQ(" #a ", " #b "): " + mt::show(mt_a) + " != " + mt::show(mt_b)); \
    } while(0)

#define CHECK_NEAR(a, b, eps)                                                                       \
    do {                                                                                            \
        const double mt_a = (a), mt_b = (b);                                                        \
        if(std::fabs(mt_a - mt_b) > (eps))                                                          \
            mt::fail(__FILE__, __LINE__, "CHECK_NEAR(" #a ", " #b "): " + mt::show(mt_a) + " vs " + mt::show(mt_b)); \
    } while(0)
