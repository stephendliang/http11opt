#ifndef H11_INTERNAL_H
#define H11_INTERNAL_H

#include "h11.h"

typedef enum {
    H11_SIMD_SCALAR = 0,
    H11_SIMD_SSE42  = 1,
    H11_SIMD_AVX2   = 2,
    H11_SIMD_AVX512 = 3
} h11_simd_level_t;

extern h11_simd_level_t h11_simd_level;

#if defined(__GNUC__) || defined(__clang__)
#define H11_LIKELY(x) __builtin_expect(!!(x), 1)
#define H11_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define H11_INLINE static inline __attribute__((always_inline))
#define H11_NOINLINE __attribute__((noinline))
#else
#define H11_LIKELY(x) (x)
#define H11_UNLIKELY(x) (x)
#define H11_INLINE static inline
#define H11_NOINLINE
#endif

#define H11_ARRAY_LEN(x) (sizeof(x) / sizeof((x)[0]))

extern const u8 h11_tchar_table[256];
extern const u8 h11_vchar_table[256];
extern const u8 h11_digit_table[256];
extern const u8 h11_hexdig_table[256];
extern const u8 h11_uri_table[256];

H11_INLINE bool h11_is_tchar(char c)  { return h11_tchar_table[(u8)c]; }
H11_INLINE bool h11_is_vchar(char c)  { return h11_vchar_table[(u8)c]; }
H11_INLINE bool h11_is_digit(char c)  { return h11_digit_table[(u8)c]; }
H11_INLINE bool h11_is_hexdig(char c) { return h11_hexdig_table[(u8)c]; }
H11_INLINE bool h11_is_uri(char c)    { return h11_uri_table[(u8)c]; }
H11_INLINE bool h11_is_sp(char c)     { return (u8)c == 0x20; }
H11_INLINE bool h11_is_htab(char c)   { return (u8)c == 0x09; }
H11_INLINE bool h11_is_ows(char c)    { return h11_is_sp(c) || h11_is_htab(c); }
H11_INLINE bool h11_is_cr(char c)     { return (u8)c == 0x0D; }
H11_INLINE bool h11_is_lf(char c)     { return (u8)c == 0x0A; }

struct h11_parser {
    h11_config_t  config;
    h11_state_t   state;
    h11_error_t   last_error;
    usize         error_offset;
    h11_request_t request;
    usize         total_consumed;
    usize         line_start;
    usize         headers_size;
    u64           body_remaining;
    u64           total_body_read;
    bool          in_chunk_ext;
    usize         chunk_ext_len;
    bool          seen_host;
    bool          seen_content_length;
    bool          seen_transfer_encoding;
    bool          is_chunked;
    bool          leading_crlf_consumed;
};

bool h11_span_eq_case(const char *base, h11_span_t a, const char *b, usize blen);
int h11_hexval(char c);
void h11_init(void);

#endif
