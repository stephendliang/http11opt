/*
 * test_util.c — Tests for character tables, error strings, and string utilities
 */
#include "h11_internal.h"
#include <stdio.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  %-50s ", #name); } while (0)
#define PASS() do { printf("OK\n"); tests_passed++; } while (0)
#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL (%s:%d: %s)\n", __FILE__, __LINE__, #cond); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define ASSERT_RANGE_TRUE(pred, lo, hi) do { \
    for (int c = (lo); c <= (hi); c++) ASSERT(pred((char)c)); \
} while (0)
#define ASSERT_CSTR_TRUE(pred, s) do { \
    for (const unsigned char *p = (const unsigned char *)(s); *p; p++) ASSERT(pred((char)*p)); \
} while (0)
#define ASSERT_LIST(pred, expect, ...) do { \
    const int vals[] = { __VA_ARGS__ }; \
    for (size_t i = 0; i < H11_ARRAY_LEN(vals); i++) { \
        int got = pred((char)vals[i]) != 0; \
        ASSERT(got == ((expect) != 0)); \
    } \
} while (0)

static void test_tchar_special_symbols(void) {
    TEST(tchar_special_symbols);
    ASSERT_CSTR_TRUE(h11_is_tchar, "!#$%&'*+-.^_`|~");
    PASS();
}

static void test_tchar_digits(void) {
    TEST(tchar_digits);
    ASSERT_RANGE_TRUE(h11_is_tchar, '0', '9');
    PASS();
}

static void test_tchar_alpha(void) {
    TEST(tchar_alpha);
    ASSERT_RANGE_TRUE(h11_is_tchar, 'A', 'Z');
    ASSERT_RANGE_TRUE(h11_is_tchar, 'a', 'z');
    PASS();
}

static void test_tchar_non_members(void) {
    TEST(tchar_non_members);
    ASSERT_LIST(h11_is_tchar, 0, ' ', '\t', '(', ')', '"', '\\', '<', '>', '@',
                '{', '}', 0, 0x7f, 0x80, ',', '/', ':', ';', '=', '?', '[', ']');
    PASS();
}

static void test_vchar_boundaries(void) {
    TEST(vchar_boundaries);
    ASSERT_LIST(h11_is_vchar, 0, 0x00, 0x08, 0x0A, 0x1F, 0x7F);
    ASSERT_LIST(h11_is_vchar, 1, 0x09, 0x20, 0x21, 0x7E, 0x80, 0xFF);
    PASS();
}

static void test_digit_boundaries(void) {
    TEST(digit_boundaries);
    ASSERT_LIST(h11_is_digit, 1, '0', '9');
    ASSERT_LIST(h11_is_digit, 0, '/', ':', 'A', ' ');
    PASS();
}

static void test_hexdig_boundaries(void) {
    TEST(hexdig_boundaries);
    ASSERT_LIST(h11_is_hexdig, 1, '0', '9', 'A', 'F', 'a', 'f');
    ASSERT_LIST(h11_is_hexdig, 0, 'G', 'g', '/', ':', '@', '`');
    PASS();
}

static void test_uri_unreserved(void) {
    TEST(uri_unreserved);
    ASSERT_RANGE_TRUE(h11_is_uri, 'A', 'Z');
    ASSERT_RANGE_TRUE(h11_is_uri, 'a', 'z');
    ASSERT_RANGE_TRUE(h11_is_uri, '0', '9');
    ASSERT_LIST(h11_is_uri, 1, '-', '.', '_', '~');
    PASS();
}

static void test_uri_sub_delims(void) {
    TEST(uri_sub_delims);
    ASSERT_CSTR_TRUE(h11_is_uri, "!$&'()*+,;=");
    PASS();
}

static void test_uri_extra_allowed(void) {
    TEST(uri_extra_allowed);
    ASSERT_LIST(h11_is_uri, 1, ':', '@', '/', '%');
    PASS();
}

static void test_uri_non_members(void) {
    TEST(uri_non_members);
    ASSERT_LIST(h11_is_uri, 0, '?', '#', '[', ']', ' ', 0, 0x80, '"', '<',
                '>', '\\', '^', '`', '{', '}', '|');
    PASS();
}

static void test_char_macros(void) {
    TEST(char_macros_sp_htab_ows_cr_lf);
    ASSERT(h11_is_sp(' '));
    ASSERT(!h11_is_sp('\t'));
    ASSERT(!h11_is_sp('a'));
    ASSERT(h11_is_htab('\t'));
    ASSERT(!h11_is_htab(' '));
    ASSERT(h11_is_ows(' '));
    ASSERT(h11_is_ows('\t'));
    ASSERT(!h11_is_ows('x'));
    ASSERT(h11_is_cr('\r'));
    ASSERT(!h11_is_cr('\n'));
    ASSERT(h11_is_lf('\n'));
    ASSERT(!h11_is_lf('\r'));
    PASS();
}

static void test_hexval_valid(void) {
    TEST(hexval_valid_digits);
    for (int c = '0'; c <= '9'; c++)
        ASSERT(h11_hexval((char)c) == c - '0');
    for (int i = 0; i < 6; i++) {
        ASSERT(h11_hexval((char)('A' + i)) == 10 + i);
        ASSERT(h11_hexval((char)('a' + i)) == 10 + i);
    }
    PASS();
}

static void test_hexval_invalid(void) {
    TEST(hexval_invalid_chars);
    const int vals[] = { 'G', 'g', '/', ':', '@', '`', 0, 0xFF };
    for (size_t i = 0; i < H11_ARRAY_LEN(vals); i++)
        ASSERT(h11_hexval((char)vals[i]) == -1);
    PASS();
}

static void test_config_default(void) {
    TEST(config_default_values);
    h11_config_t cfg = h11_config_default();
    ASSERT(cfg.max_body_size == UINT64_MAX);
    ASSERT(cfg.max_request_line_len == 8192);
    ASSERT(cfg.max_header_line_len == 8192);
    ASSERT(cfg.max_headers_size == 65536);
    ASSERT(cfg.max_header_count == 100);
    ASSERT(cfg.max_chunk_ext_len == 1024);
    ASSERT((cfg.flags & H11_CFG_STRICT_CRLF) != 0);
    ASSERT((cfg.flags & H11_CFG_REJECT_OBS_FOLD) != 0);
    ASSERT((cfg.flags & H11_CFG_ALLOW_OBS_TEXT) != 0);
    ASSERT((cfg.flags & H11_CFG_ALLOW_LEADING_CRLF) != 0);
    ASSERT((cfg.flags & H11_CFG_TOLERATE_SPACES) == 0);
    ASSERT((cfg.flags & H11_CFG_REJECT_TE_CL_CONFLICT) != 0);
    PASS();
}

static void test_slice_eq_case_empty(void) {
    TEST(slice_eq_case_empty);
    const char *base = "";
    h11_span_t s = { .off = 0, .len = 0 };
    ASSERT(h11_span_eq_case(base, s, "", 0) == true);
    PASS();
}

static void test_slice_eq_case_match(void) {
    TEST(slice_eq_case_insensitive_match);
    const char *base = "Content-Type";
    h11_span_t s = { .off = 0, .len = 12 };
    ASSERT(h11_span_eq_case(base, s, "content-type", 12) == true);
    ASSERT(h11_span_eq_case(base, s, "CONTENT-TYPE", 12) == true);
    ASSERT(h11_span_eq_case(base, s, "Content-Type", 12) == true);
    PASS();
}

static void test_slice_eq_case_length_mismatch(void) {
    TEST(slice_eq_case_length_mismatch);
    const char *base = "Host";
    h11_span_t s = { .off = 0, .len = 4 };
    ASSERT(h11_span_eq_case(base, s, "Hos", 3) == false);
    ASSERT(h11_span_eq_case(base, s, "Hostt", 5) == false);
    PASS();
}

static void test_slice_eq_case_exact(void) {
    TEST(slice_eq_case_exact_match);
    const char *base = "abc123";
    h11_span_t s = { .off = 0, .len = 6 };
    ASSERT(h11_span_eq_case(base, s, "abc123", 6) == true);
    PASS();
}

static void test_slice_eq_case_non_alpha(void) {
    TEST(slice_eq_case_non_alpha_chars);
    const char *base1 = "x-my-1";
    h11_span_t s1 = { .off = 0, .len = 6 };
    ASSERT(h11_span_eq_case(base1, s1, "X-MY-1", 6) == true);
    const char *base2 = "a-b";
    h11_span_t s2 = { .off = 0, .len = 3 };
    ASSERT(h11_span_eq_case(base2, s2, "a_b", 3) == false);
    ASSERT(h11_span_eq_case(NULL, s2, "a_b", 3) == false);
    PASS();
}

static void test_header_name_eq(void) {
    TEST(header_name_eq);
    const char *base = "Content-Length";
    h11_span_t name = { .off = 0, .len = 14 };
    ASSERT(h11_header_name_eq(base, name, "content-length") == true);
    ASSERT(h11_header_name_eq(base, name, "CONTENT-LENGTH") == true);
    ASSERT(h11_header_name_eq(base, name, "content-type") == false);
    ASSERT(h11_header_name_eq(base, name, NULL) == false);
    PASS();
}

static void test_find_header(void) {
    TEST(find_header);
    const char base[] =
        "Host\0"
        "example.com\0"
        "Content-Type\0"
        "text/html\0"
        "Connection\0"
        "keep-alive\0";
    h11_header_t hdrs[3] = {
        { .name = { .off = 0, .len = 4 }, .value = { .off = 5, .len = 11 },
          .name_id = H11_KHDR_HOST, .flags = H11_HEADER_F_KNOWN_NAME },
        { .name = { .off = 17, .len = 12 }, .value = { .off = 30, .len = 9 },
          .name_id = H11_INDEX_NONE, .flags = 0 },
        { .name = { .off = 40, .len = 10 }, .value = { .off = 51, .len = 10 },
          .name_id = H11_KHDR_CONNECTION, .flags = H11_HEADER_F_KNOWN_NAME },
    };
    h11_request_t req = { .headers = hdrs, .header_count = 3 };
    ASSERT(h11_find_header(&req, base, "host") == 0);
    ASSERT(h11_find_header(&req, base, "HOST") == 0);
    ASSERT(h11_find_header(&req, base, "content-type") == 1);
    ASSERT(h11_find_header(&req, base, "connection") == 2);
    ASSERT(h11_find_header(&req, base, "x-missing") == -1);
    ASSERT(h11_find_header(&req, base, "Accept") == -1);
    PASS();
}

static void test_find_header_empty(void) {
    TEST(find_header_empty_list);
    h11_request_t req = { .headers = NULL, .header_count = 0 };
    ASSERT(h11_find_header(&req, "", "Host") == -1);
    ASSERT(h11_find_header(NULL, "", "Host") == -1);
    ASSERT(h11_find_header(&req, NULL, "Host") == -1);
    ASSERT(h11_find_header(&req, "", NULL) == -1);
    PASS();
}

static void test_error_name_ok(void) {
    TEST(error_name_ok);
    ASSERT(strcmp(h11_error_name(H11_OK), "H11_OK") == 0);
    PASS();
}

static void test_error_message_ok(void) {
    TEST(error_message_ok);
    ASSERT(strcmp(h11_error_message(H11_OK), "Success") == 0);
    PASS();
}

static void test_error_name_categories(void) {
    TEST(error_name_categories);
    const h11_error_t errs[] = {
        H11_NEED_MORE_DATA,
        H11_ERR_INVALID_METHOD,
        H11_ERR_INVALID_HEADER_NAME,
        H11_ERR_MISSING_HOST,
        H11_ERR_BODY_TOO_LARGE,
        H11_ERR_CONNECTION_CLOSED,
        H11_ERR_INTERNAL,
    };
    const char *names[] = {
        "H11_NEED_MORE_DATA",
        "H11_ERR_INVALID_METHOD",
        "H11_ERR_INVALID_HEADER_NAME",
        "H11_ERR_MISSING_HOST",
        "H11_ERR_BODY_TOO_LARGE",
        "H11_ERR_CONNECTION_CLOSED",
        "H11_ERR_INTERNAL",
    };
    for (size_t i = 0; i < H11_ARRAY_LEN(errs); i++)
        ASSERT(strcmp(h11_error_name(errs[i]), names[i]) == 0);
    PASS();
}

static void test_error_message_categories(void) {
    TEST(error_message_categories);
    ASSERT(strcmp(h11_error_message(H11_NEED_MORE_DATA), "Need more data") == 0);
    ASSERT(strcmp(h11_error_message(H11_ERR_INVALID_METHOD), "Invalid HTTP method") == 0);
    ASSERT(strcmp(h11_error_message(H11_ERR_INTERNAL), "Internal error") == 0);
    PASS();
}

static void test_error_out_of_range(void) {
    TEST(error_out_of_range);
    ASSERT(strcmp(h11_error_name((h11_error_t)-1), "UNKNOWN") == 0);
    ASSERT(strcmp(h11_error_name((h11_error_t)999), "UNKNOWN") == 0);
    ASSERT(strcmp(h11_error_message((h11_error_t)-1), "UNKNOWN") == 0);
    ASSERT(strcmp(h11_error_message((h11_error_t)999), "UNKNOWN") == 0);
    PASS();
}

static void test_error_all_non_null(void) {
    TEST(error_all_entries_non_null);
    for (int i = 0; i < H11_ERR__COUNT; i++) {
        ASSERT(h11_error_name((h11_error_t)i) != NULL);
        ASSERT(h11_error_message((h11_error_t)i) != NULL);
    }
    PASS();
}

int main(void) {
    printf("=== tchar table ===\n");
    test_tchar_special_symbols();
    test_tchar_digits();
    test_tchar_alpha();
    test_tchar_non_members();

    printf("=== vchar table ===\n");
    test_vchar_boundaries();

    printf("=== digit table ===\n");
    test_digit_boundaries();

    printf("=== hexdig table ===\n");
    test_hexdig_boundaries();

    printf("=== uri table ===\n");
    test_uri_unreserved();
    test_uri_sub_delims();
    test_uri_extra_allowed();
    test_uri_non_members();

    printf("=== char macros ===\n");
    test_char_macros();

    printf("=== hexval ===\n");
    test_hexval_valid();
    test_hexval_invalid();

    printf("=== config default ===\n");
    test_config_default();

    printf("=== slice_eq_case ===\n");
    test_slice_eq_case_empty();
    test_slice_eq_case_match();
    test_slice_eq_case_length_mismatch();
    test_slice_eq_case_exact();
    test_slice_eq_case_non_alpha();

    printf("=== header_name_eq ===\n");
    test_header_name_eq();

    printf("=== find_header ===\n");
    test_find_header();
    test_find_header_empty();

    printf("=== error_name / error_message ===\n");
    test_error_name_ok();
    test_error_message_ok();
    test_error_name_categories();
    test_error_message_categories();
    test_error_out_of_range();
    test_error_all_non_null();

    printf("\n--- Results: %d passed, %d failed ---\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
