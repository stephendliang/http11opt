#ifndef H11_H
#define H11_H

#ifdef __cplusplus
extern "C" {
#endif

#include "h11_types.h"

enum {
    H11_VERSION_MAJOR = 1,
    H11_VERSION_MINOR = 0,
    H11_VERSION_PATCH = 0,
};

typedef enum {
    H11_OK = 0,
    H11_NEED_MORE_DATA,
    H11_ERR_INVALID_METHOD,
    H11_ERR_INVALID_TARGET,
    H11_ERR_INVALID_VERSION,
    H11_ERR_REQUEST_LINE_TOO_LONG,
    H11_ERR_INVALID_CRLF,
    H11_ERR_INVALID_HEADER_NAME,
    H11_ERR_INVALID_HEADER_VALUE,
    H11_ERR_HEADER_LINE_TOO_LONG,
    H11_ERR_TOO_MANY_HEADERS,
    H11_ERR_HEADERS_TOO_LARGE,
    H11_ERR_OBS_FOLD_REJECTED,
    H11_ERR_LEADING_WHITESPACE,
    H11_ERR_MISSING_HOST,
    H11_ERR_MULTIPLE_HOST,
    H11_ERR_INVALID_HOST,
    H11_ERR_INVALID_CONTENT_LENGTH,
    H11_ERR_MULTIPLE_CONTENT_LENGTH,
    H11_ERR_CONTENT_LENGTH_OVERFLOW,
    H11_ERR_INVALID_TRANSFER_ENCODING,
    H11_ERR_TE_NOT_CHUNKED_FINAL,
    H11_ERR_TE_CL_CONFLICT,
    H11_ERR_UNKNOWN_TRANSFER_CODING,
    H11_ERR_BODY_TOO_LARGE,
    H11_ERR_INVALID_CHUNK_SIZE,
    H11_ERR_CHUNK_SIZE_OVERFLOW,
    H11_ERR_INVALID_CHUNK_EXT,
    H11_ERR_CHUNK_EXT_TOO_LONG,
    H11_ERR_INVALID_CHUNK_DATA,
    H11_ERR_INVALID_TRAILER,
    H11_ERR_CONNECTION_CLOSED,
    H11_ERR_INTERNAL,
    H11_ERR__COUNT
} h11_error_t;

typedef enum {
    H11_STATE_IDLE = 0,
    H11_STATE_REQUEST_LINE,
    H11_STATE_HEADERS,
    H11_STATE_BODY_IDENTITY,
    H11_STATE_BODY_CHUNKED_SIZE,
    H11_STATE_BODY_CHUNKED_DATA,
    H11_STATE_BODY_CHUNKED_CRLF,
    H11_STATE_TRAILERS,
    H11_STATE_COMPLETE,
    H11_STATE_ERROR
} h11_state_t;

typedef enum {
    H11_TARGET_ORIGIN = 0,
    H11_TARGET_ABSOLUTE,
    H11_TARGET_AUTHORITY,
    H11_TARGET_ASTERISK
} h11_target_form_t;

typedef enum {
    H11_BODY_NONE = 0,
    H11_BODY_CONTENT_LENGTH,
    H11_BODY_CHUNKED
} h11_body_type_t;

typedef enum {
    H11_KHDR_HOST = 0,
    H11_KHDR_CONTENT_LENGTH,
    H11_KHDR_TRANSFER_ENCODING,
    H11_KHDR_CONNECTION,
    H11_KHDR_EXPECT,
    H11_KHDR_UPGRADE,
    H11_KHDR_COUNT
} h11_known_header_t;

enum {
    H11_CFG_STRICT_CRLF           = 1u << 0,
    H11_CFG_REJECT_OBS_FOLD       = 1u << 1,
    H11_CFG_ALLOW_OBS_TEXT        = 1u << 2,
    H11_CFG_ALLOW_LEADING_CRLF    = 1u << 3,
    H11_CFG_TOLERATE_SPACES       = 1u << 4,
    H11_CFG_REJECT_TE_CL_CONFLICT = 1u << 5,
};

enum {
    H11_REQF_KEEP_ALIVE            = 1u << 0,
    H11_REQF_EXPECT_CONTINUE       = 1u << 1,
    H11_REQF_HAS_UPGRADE           = 1u << 2,
    H11_REQF_HAS_HOST              = 1u << 3,
    H11_REQF_HAS_CONTENT_LENGTH    = 1u << 4,
    H11_REQF_HAS_TRANSFER_ENCODING = 1u << 5,
    H11_REQF_IS_CHUNKED            = 1u << 6,
};

enum {
    H11_HEADER_F_KNOWN_NAME = 1u << 0,
};

enum { H11_INDEX_NONE = 0xFFFF };

typedef struct {
    u32 off;
    u32 len;
} h11_span_t;

typedef struct {
    h11_span_t name;
    h11_span_t value;
    u16        name_id;
    u16        flags;
} h11_header_t;

typedef struct {
    u64 max_body_size;
    u32 max_request_line_len;
    u32 max_header_line_len;
    u32 max_headers_size;
    u32 max_header_count;
    u32 max_chunk_ext_len;
    u32 flags;
    u32 reserved0;
} h11_config_t;

typedef struct {
    h11_span_t    method;
    h11_span_t    target;
    u64           content_length;
    u32           header_count;
    u32           trailer_count;
    u16           version;
    u8            target_form;
    u8            body_type;
    u16           flags;
    u16           reserved0;
    u16           known_idx[H11_KHDR_COUNT];
    u16           reserved1;
    h11_header_t *headers;
    h11_header_t *trailers;
} h11_request_t;

typedef struct h11_parser h11_parser_t;

h11_config_t h11_config_default(void);
h11_parser_t *h11_parser_new(const h11_config_t *config);
void h11_parser_free(h11_parser_t *parser);
void h11_parser_reset(h11_parser_t *parser);
h11_error_t h11_parse(h11_parser_t *p, const char *data, usize len, usize *consumed);
h11_state_t h11_get_state(const h11_parser_t *p);
const h11_request_t *h11_get_request(const h11_parser_t *p);
h11_error_t h11_read_body(h11_parser_t *p, const char *data, usize len, usize *consumed,
                          const char **body_out, usize *body_len);
const char *h11_error_name(h11_error_t error);
const char *h11_error_message(h11_error_t error);
usize h11_error_offset(const h11_parser_t *p);
bool h11_header_name_eq(const char *base, h11_span_t name, const char *cmp);
int h11_find_header(const h11_request_t *req, const char *base, const char *name);

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(h11_span_t) == 8, "h11_span_t must stay compact");
_Static_assert(sizeof(h11_header_t) <= 24, "h11_header_t exceeded target size");
_Static_assert(sizeof(h11_request_t) <= 96, "h11_request_t exceeded target size");
#endif

#ifdef __cplusplus
}
#endif

#endif
