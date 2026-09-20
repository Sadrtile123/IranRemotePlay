// RemotePlay - tests/test_session_codes.cpp
// Session code generation, normalization, and constant-time verification.
#include "security/SessionCodes.h"

#include "TestHarness.hpp"

#include <set>
#include <string>

using namespace rp::security;

RP_TEST(generated_code_format) {
    for (int i = 0; i < 200; ++i) {
        const std::string code = generateSessionCode();
        CHECK_EQ(code.size(), size_t(9));
        CHECK_EQ(code[4], '-');
        for (size_t j = 0; j < code.size(); ++j) {
            if (j == 4) continue;
            const char c = code[j];
            const bool valid = (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '9');
            CHECK(valid);
        }
    }
}

RP_TEST(generated_codes_are_unique_in_batch) {
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) seen.insert(generateSessionCode());
    CHECK_EQ(seen.size(), size_t(1000));
}

RP_TEST(alphabet_has_no_ambiguous_characters) {
    CHECK_EQ(kCodeAlphabetSize, size_t(32));
    const std::string alphabet(kCodeAlphabet);
    CHECK(alphabet.find('I') == std::string::npos);
    CHECK(alphabet.find('O') == std::string::npos);
    CHECK(alphabet.find('0') == std::string::npos);
    CHECK(alphabet.find('1') == std::string::npos);
    CHECK_EQ(alphabet.size(), kCodeAlphabetSize);
}

RP_TEST(normalization_accepts_user_typos) {
    CHECK_EQ(normalizeSessionCode("abc7-k92p"), std::string("ABC7-K92P"));
    CHECK_EQ(normalizeSessionCode("  ABC7K92P "), std::string("ABC7-K92P"));
    CHECK_EQ(normalizeSessionCode("ABC7 K92P"), std::string("ABC7-K92P"));
    CHECK_EQ(normalizeSessionCode("abc7 k92p"), std::string("ABC7-K92P"));
    CHECK_EQ(normalizeSessionCode("ABC7-K92P"), std::string("ABC7-K92P"));
}

RP_TEST(normalization_rejects_bad_input) {
    CHECK(!normalizeSessionCode("ABC7-K92").has_value());   // 7 chars
    CHECK(!normalizeSessionCode("ABC7-K92PP").has_value()); // 9 chars
    CHECK(!normalizeSessionCode("ABCI-K92P").has_value());  // ambiguous 'I'
    CHECK(!normalizeSessionCode("ABO7-K92P").has_value());  // ambiguous 'O'
    CHECK(!normalizeSessionCode("ABC0-K92P").has_value());  // ambiguous '0'
    CHECK(!normalizeSessionCode("ABC1-K92P").has_value());  // ambiguous '1'
    CHECK(!normalizeSessionCode("").has_value());
    CHECK(!normalizeSessionCode("--------").has_value());
}

RP_TEST(verify_accepts_correct_code) {
    CHECK(verifySessionCode("ABC7-K92P", "ABC7-K92P"));
    CHECK(verifySessionCode("abc7 k92p", "ABC7-K92P")); // sloppy typing still works
}

RP_TEST(verify_rejects_wrong_code) {
    CHECK(!verifySessionCode("ABC7-K92O", "ABC7-K92P")); // 1 char different
    CHECK(!verifySessionCode("ABC7-K92P", "AAAA-AAAA"));
    CHECK(!verifySessionCode("GARBAGE!!", "ABC7-K92P"));
}

RP_TEST(constant_time_equal_basics) {
    CHECK(constantTimeEqual("ABC7-K92P", "ABC7-K92P"));
    CHECK(!constantTimeEqual("ABC7-K92P", "ABC7-K92O"));
    CHECK(!constantTimeEqual("ABC7-K92P", "ABC7-K92"));  // different lengths
    CHECK(constantTimeEqual("", ""));
}

RP_TEST_MAIN("session codes")
