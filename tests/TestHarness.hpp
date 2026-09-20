// RemotePlay - tests/TestHarness.hpp
// Minimal zero-dependency test harness used by the ctest suite.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace rptest {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

inline int& failureCount() {
    static int c = 0;
    return c;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back(TestCase{name, std::move(fn)});
    }
};

inline int runAll(const char* suiteName) {
    int failed = 0;
    std::printf("== %s ==\n", suiteName);
    for (const TestCase& tc : registry()) {
        const int before = failureCount();
        std::printf("[ RUN  ] %s\n", tc.name);
        tc.fn();
        if (failureCount() > before) {
            ++failed;
            std::printf("[ FAIL ] %s\n", tc.name);
        } else {
            std::printf("[  OK  ] %s\n", tc.name);
        }
    }
    std::printf("== %s: %zu test(s), %d failed ==\n", suiteName, registry().size(), failed);
    return failed == 0 ? 0 : 1;
}

} // namespace rptest

#define RP_TEST(name)                                                                  \
    static void rptest_##name();                                                       \
    static const rptest::Registrar rptest_registrar_##name(#name, rptest_##name);      \
    static void rptest_##name()

#define RP_TEST_MAIN(suite)                                                            \
    int main() { return rptest::runAll(suite); }

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            ++rptest::failureCount();                                                  \
            std::printf("       CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                 \
    do {                                                                               \
        if (!((a) == (b))) {                                                           \
            ++rptest::failureCount();                                                  \
            std::printf("       CHECK_EQ failed at %s:%d: %s == %s\n", __FILE__,       \
                        __LINE__, #a, #b);                                             \
        }                                                                              \
    } while (0)
