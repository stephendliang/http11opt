/*
 * util.c — Character tables, error strings, and string utilities
 */
#include "h11_internal.h"
#include <limits.h>
#include <string.h>

#define Z16 \
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
#define O16 \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
#define R16(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) \
    a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p

const u8 h11_tchar_table[256] = {
    Z16, Z16,
    R16(0, 1, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0),
    R16(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0),
    R16(0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),
    R16(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1),
    R16(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),
    R16(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0),
    Z16, Z16, Z16, Z16, Z16, Z16, Z16, Z16,
};

const u8 h11_vchar_table[256] = {
    R16(0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0),
    Z16,
    O16, O16, O16, O16, O16,
    R16(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0),
    O16, O16, O16, O16, O16, O16, O16, O16,
};

const u8 h11_digit_table[256] = {
    Z16, Z16, Z16,
    R16(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0),
    Z16, Z16, Z16, Z16, Z16, Z16, Z16, Z16, Z16, Z16, Z16, Z16,
};

const u8 h11_hexdig_table[256] = {
    Z16, Z16, Z16,
    R16(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0),
    R16(0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0),
    Z16,
    R16(0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0),
    Z16, Z16, Z16, Z16, Z16, Z16, Z16, Z16, Z16,
};

const u8 h11_uri_table[256] = {
    Z16, Z16,
    R16(0, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),
    R16(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0),
    O16,
    R16(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1),
    R16(0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),
    R16(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0),
    Z16, Z16, Z16, Z16, Z16, Z16, Z16, Z16,
};

#undef R16
#undef O16
#undef Z16

int h11_hexval(char c) {
    unsigned u = (unsigned char)c;
    if (u - '0' <= 9u)
        return (int)(u - '0');
    u |= 0x20u;
    if (u - 'a' <= 5u)
        return (int)(u - 'a' + 10u);
    return -1;
}

#define H11_ERROR_LIST(X) \
    X(H11_OK, "H11_OK", "Success") \
    X(H11_NEED_MORE_DATA, "H11_NEED_MORE_DATA", "Need more data") \
    X(H11_ERR_INVALID_METHOD, "H11_ERR_INVALID_METHOD", "Invalid HTTP method") \
    X(H11_ERR_INVALID_TARGET, "H11_ERR_INVALID_TARGET", "Invalid request target") \
    X(H11_ERR_INVALID_VERSION, "H11_ERR_INVALID_VERSION", "Invalid HTTP version") \
    X(H11_ERR_REQUEST_LINE_TOO_LONG, "H11_ERR_REQUEST_LINE_TOO_LONG", "Request line too long") \
    X(H11_ERR_INVALID_CRLF, "H11_ERR_INVALID_CRLF", "Invalid line ending") \
    X(H11_ERR_INVALID_HEADER_NAME, "H11_ERR_INVALID_HEADER_NAME", "Invalid header name") \
    X(H11_ERR_INVALID_HEADER_VALUE, "H11_ERR_INVALID_HEADER_VALUE", "Invalid header value") \
    X(H11_ERR_HEADER_LINE_TOO_LONG, "H11_ERR_HEADER_LINE_TOO_LONG", "Header line too long") \
    X(H11_ERR_TOO_MANY_HEADERS, "H11_ERR_TOO_MANY_HEADERS", "Too many headers") \
    X(H11_ERR_HEADERS_TOO_LARGE, "H11_ERR_HEADERS_TOO_LARGE", "Headers section too large") \
    X(H11_ERR_OBS_FOLD_REJECTED, "H11_ERR_OBS_FOLD_REJECTED", "Obsolete line folding rejected") \
    X(H11_ERR_LEADING_WHITESPACE, "H11_ERR_LEADING_WHITESPACE", "Leading whitespace in header section") \
    X(H11_ERR_MISSING_HOST, "H11_ERR_MISSING_HOST", "Missing Host header") \
    X(H11_ERR_MULTIPLE_HOST, "H11_ERR_MULTIPLE_HOST", "Multiple Host headers") \
    X(H11_ERR_INVALID_HOST, "H11_ERR_INVALID_HOST", "Invalid Host header value") \
    X(H11_ERR_INVALID_CONTENT_LENGTH, "H11_ERR_INVALID_CONTENT_LENGTH", "Invalid Content-Length value") \
    X(H11_ERR_MULTIPLE_CONTENT_LENGTH, "H11_ERR_MULTIPLE_CONTENT_LENGTH", "Conflicting Content-Length values") \
    X(H11_ERR_CONTENT_LENGTH_OVERFLOW, "H11_ERR_CONTENT_LENGTH_OVERFLOW", "Content-Length value overflow") \
    X(H11_ERR_INVALID_TRANSFER_ENCODING, "H11_ERR_INVALID_TRANSFER_ENCODING", "Invalid Transfer-Encoding") \
    X(H11_ERR_TE_NOT_CHUNKED_FINAL, "H11_ERR_TE_NOT_CHUNKED_FINAL", "Transfer-Encoding final coding is not chunked") \
    X(H11_ERR_TE_CL_CONFLICT, "H11_ERR_TE_CL_CONFLICT", "Transfer-Encoding and Content-Length both present") \
    X(H11_ERR_UNKNOWN_TRANSFER_CODING, "H11_ERR_UNKNOWN_TRANSFER_CODING", "Unknown transfer coding") \
    X(H11_ERR_BODY_TOO_LARGE, "H11_ERR_BODY_TOO_LARGE", "Body exceeds maximum size") \
    X(H11_ERR_INVALID_CHUNK_SIZE, "H11_ERR_INVALID_CHUNK_SIZE", "Invalid chunk size") \
    X(H11_ERR_CHUNK_SIZE_OVERFLOW, "H11_ERR_CHUNK_SIZE_OVERFLOW", "Chunk size overflow") \
    X(H11_ERR_INVALID_CHUNK_EXT, "H11_ERR_INVALID_CHUNK_EXT", "Invalid chunk extension") \
    X(H11_ERR_CHUNK_EXT_TOO_LONG, "H11_ERR_CHUNK_EXT_TOO_LONG", "Chunk extension too long") \
    X(H11_ERR_INVALID_CHUNK_DATA, "H11_ERR_INVALID_CHUNK_DATA", "Invalid chunk data") \
    X(H11_ERR_INVALID_TRAILER, "H11_ERR_INVALID_TRAILER", "Invalid trailer field") \
    X(H11_ERR_CONNECTION_CLOSED, "H11_ERR_CONNECTION_CLOSED", "Connection closed") \
    X(H11_ERR_INTERNAL, "H11_ERR_INTERNAL", "Internal error")

#define H11_ERR_NAME(e, n, m) [e] = n,
static const char *const error_names[H11_ERR__COUNT] = {
    H11_ERROR_LIST(H11_ERR_NAME)
};
#undef H11_ERR_NAME

#define H11_ERR_MSG(e, n, m) [e] = m,
static const char *const error_messages[H11_ERR__COUNT] = {
    H11_ERROR_LIST(H11_ERR_MSG)
};
#undef H11_ERR_MSG
#undef H11_ERROR_LIST

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(H11_ARRAY_LEN(error_names) == H11_ERR__COUNT,
               "error_names must match h11_error_t");
_Static_assert(H11_ARRAY_LEN(error_messages) == H11_ERR__COUNT,
               "error_messages must match h11_error_t");
#endif

const char *h11_error_name(h11_error_t error) {
    if ((unsigned)error >= H11_ERR__COUNT)
        return "UNKNOWN";
    return error_names[error];
}

const char *h11_error_message(h11_error_t error) {
    if ((unsigned)error >= H11_ERR__COUNT)
        return "UNKNOWN";
    return error_messages[error];
}

h11_config_t h11_config_default(void) {
    return (h11_config_t){
        .max_body_size = UINT64_MAX,
        .max_request_line_len = 8192,
        .max_header_line_len = 8192,
        .max_headers_size = 65536,
        .max_header_count = 100,
        .max_chunk_ext_len = 1024,
        .flags = H11_CFG_STRICT_CRLF | H11_CFG_REJECT_OBS_FOLD |
                 H11_CFG_ALLOW_OBS_TEXT | H11_CFG_ALLOW_LEADING_CRLF |
                 H11_CFG_REJECT_TE_CL_CONFLICT,
        .reserved0 = 0,
    };
}

H11_INLINE u8 h11_ascii_fold(u8 c) {
    return (c >= 'A' && c <= 'Z') ? (u8)(c | 0x20u) : c;
}

bool h11_span_eq_case(const char *base, h11_span_t a, const char *b, usize blen) {
    if (base == NULL || b == NULL || a.len != blen)
        return false;
    const u8 *ap = (const u8 *)(base + a.off);
    const u8 *bp = (const u8 *)b;
    for (usize i = 0; i < blen; i++) {
        if (h11_ascii_fold(ap[i]) != h11_ascii_fold(bp[i]))
            return false;
    }
    return true;
}

bool h11_header_name_eq(const char *base, h11_span_t name, const char *cmp) {
    return cmp != NULL && h11_span_eq_case(base, name, cmp, strlen(cmp));
}

int h11_find_header(const h11_request_t *req, const char *base, const char *name) {
    if (req == NULL || base == NULL || name == NULL || req->headers == NULL)
        return -1;
    if (req->header_count > (u32)INT_MAX)
        return -1;
    for (u32 i = 0; i < req->header_count; i++) {
        if (h11_header_name_eq(base, req->headers[i].name, name))
            return (int)i;
    }
    return -1;
}
