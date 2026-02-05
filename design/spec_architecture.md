# H11 Architecture Spec

## S1. Project Layout

| File | Purpose |
|------|---------|
| `h11.h` | Public API: enums, structs, function declarations |
| `h11_internal.h` | Internal types, SIMD level enum, character table externs, parser struct, macros |
| `parser.c` | CPU detection, SIMD scanners (find_crlf/find_char), state machine, all parsing logic |
| `util.c` | Character classification tables, error messages, string utilities |

**Build**: `cc -std=c11 -O3 -march=native -fPIC parser.c util.c` — SIMD enabled via `-march=native`; cross-compile with `-mavx2` or `-mavx512bw`. Debug: `-g -O0 -DDEBUG`.

## S2. Public API

### S2.1 Enums

**h11_error_t**

| Value | Category |
|-------|----------|
| `H11_OK` | Success |
| `H11_NEED_MORE_DATA` | Incomplete input |
| `H11_ERR_INVALID_METHOD` | Request line |
| `H11_ERR_INVALID_TARGET` | Request line |
| `H11_ERR_INVALID_VERSION` | Request line |
| `H11_ERR_REQUEST_LINE_TOO_LONG` | Request line |
| `H11_ERR_INVALID_CRLF` | Request line |
| `H11_ERR_INVALID_HEADER_NAME` | Header syntax |
| `H11_ERR_INVALID_HEADER_VALUE` | Header syntax |
| `H11_ERR_HEADER_LINE_TOO_LONG` | Header syntax |
| `H11_ERR_TOO_MANY_HEADERS` | Header limit |
| `H11_ERR_HEADERS_TOO_LARGE` | Header limit |
| `H11_ERR_OBS_FOLD_REJECTED` | Header syntax |
| `H11_ERR_LEADING_WHITESPACE` | Header syntax |
| `H11_ERR_MISSING_HOST` | Semantic |
| `H11_ERR_MULTIPLE_HOST` | Semantic |
| `H11_ERR_INVALID_HOST` | Semantic |
| `H11_ERR_INVALID_CONTENT_LENGTH` | Semantic |
| `H11_ERR_MULTIPLE_CONTENT_LENGTH` | Semantic |
| `H11_ERR_CONTENT_LENGTH_OVERFLOW` | Semantic |
| `H11_ERR_INVALID_TRANSFER_ENCODING` | Semantic |
| `H11_ERR_TE_NOT_CHUNKED_FINAL` | Semantic |
| `H11_ERR_TE_CL_CONFLICT` | Semantic |
| `H11_ERR_UNKNOWN_TRANSFER_CODING` | Semantic |
| `H11_ERR_BODY_TOO_LARGE` | Body/chunked |
| `H11_ERR_INVALID_CHUNK_SIZE` | Body/chunked |
| `H11_ERR_CHUNK_SIZE_OVERFLOW` | Body/chunked |
| `H11_ERR_INVALID_CHUNK_EXT` | Body/chunked |
| `H11_ERR_CHUNK_EXT_TOO_LONG` | Body/chunked |
| `H11_ERR_INVALID_CHUNK_DATA` | Body/chunked |
| `H11_ERR_INVALID_TRAILER` | Body/chunked |
| `H11_ERR_CONNECTION_CLOSED` | Fatal |
| `H11_ERR_INTERNAL` | Fatal |

**h11_state_t**

| Value | Meaning |
|-------|---------|
| `H11_STATE_IDLE` | Ready for new request |
| `H11_STATE_REQUEST_LINE` | Parsing request line |
| `H11_STATE_HEADERS` | Parsing header fields |
| `H11_STATE_BODY_IDENTITY` | Reading Content-Length body |
| `H11_STATE_BODY_CHUNKED_SIZE` | Reading chunk size line |
| `H11_STATE_BODY_CHUNKED_DATA` | Reading chunk data |
| `H11_STATE_BODY_CHUNKED_CRLF` | Expecting CRLF after chunk |
| `H11_STATE_TRAILERS` | Parsing trailer fields |
| `H11_STATE_COMPLETE` | Request fully parsed |
| `H11_STATE_ERROR` | Unrecoverable error |

**h11_target_form_t** (RFC 9112 S3.2)

| Value | Form |
|-------|------|
| `H11_TARGET_ORIGIN` | `/path?query` |
| `H11_TARGET_ABSOLUTE` | `http://host/path` |
| `H11_TARGET_AUTHORITY` | `host:port` (CONNECT) |
| `H11_TARGET_ASTERISK` | `*` (OPTIONS) |

**h11_body_type_t**

| Value | Framing |
|-------|---------|
| `H11_BODY_NONE` | No body |
| `H11_BODY_CONTENT_LENGTH` | Content-Length specified |
| `H11_BODY_CHUNKED` | Transfer-Encoding: chunked |

**h11_known_header_t** — indices into `known_idx[]` array

| Value | Header |
|-------|--------|
| `H11_KHDR_HOST` (0) | Host |
| `H11_KHDR_CONTENT_LENGTH` (1) | Content-Length |
| `H11_KHDR_TRANSFER_ENCODING` (2) | Transfer-Encoding |
| `H11_KHDR_CONNECTION` (3) | Connection |
| `H11_KHDR_EXPECT` (4) | Expect |
| `H11_KHDR_UPGRADE` (5) | Upgrade |
| `H11_KHDR_COUNT` (6) | Sentinel (array size) |

Sentinel: `#define H11_INDEX_NONE UINT16_C(0xFFFF)` — stored in `known_idx[k]` when header `k` is not present.

**Config flags** (anonymous enum, used in `h11_config_t.flags`)

| Constant | Bit | Purpose |
|----------|-----|---------|
| `H11_CFG_STRICT_CRLF` | `1 << 0` | Reject bare LF |
| `H11_CFG_REJECT_OBS_FOLD` | `1 << 1` | Reject obs-fold |
| `H11_CFG_ALLOW_OBS_TEXT` | `1 << 2` | Allow 0x80-0xFF in values |
| `H11_CFG_ALLOW_LEADING_CRLF` | `1 << 3` | Ignore leading empty lines |
| `H11_CFG_TOLERATE_SPACES` | `1 << 4` | Lax SP in request-line |
| `H11_CFG_REJECT_TE_CL_CONFLICT` | `1 << 5` | Reject TE+CL presence |

**Request flags** (anonymous enum, used in `h11_request_t.flags`)

| Constant | Bit | Purpose |
|----------|-----|---------|
| `H11_REQF_KEEP_ALIVE` | `1 << 0` | Connection persistence |
| `H11_REQF_EXPECT_CONTINUE` | `1 << 1` | Expect: 100-continue |
| `H11_REQF_HAS_UPGRADE` | `1 << 2` | Upgrade header present |
| `H11_REQF_HAS_HOST` | `1 << 3` | Host header present |
| `H11_REQF_HAS_CONTENT_LENGTH` | `1 << 4` | Content-Length present |
| `H11_REQF_HAS_TRANSFER_ENCODING` | `1 << 5` | Transfer-Encoding present |
| `H11_REQF_IS_CHUNKED` | `1 << 6` | TE validated as chunked |

**Header flags** (anonymous enum, used in `h11_header_t.flags`)

| Constant | Bit | Purpose |
|----------|-----|---------|
| `H11_HEADER_F_KNOWN_NAME` | `1 << 0` | Name matches a known header |

### S2.2 Structs

**h11_span_t** — offset-based reference into parser's input buffer (replaces pointer-based slices)

| Field | Type | Notes |
|-------|------|-------|
| `off` | `uint32_t` | Byte offset into buffer |
| `len` | `uint32_t` | Length in bytes |

To resolve a span to a pointer: `const char *str = base + span.off`. The `base` pointer is the `data` argument passed to `h11_parse()`. `_Static_assert(sizeof(h11_span_t) == 8)`.

**h11_header_t**

| Field | Type | Notes |
|-------|------|-------|
| `name` | `h11_span_t` | Header field name (offset into buffer) |
| `value` | `h11_span_t` | Header field value (offset into buffer) |
| `name_id` | `uint16_t` | `h11_known_header_t` value if known, else `H11_INDEX_NONE` |
| `flags` | `uint16_t` | Bitfield: `H11_HEADER_F_KNOWN_NAME` if name matches a known header |

`_Static_assert(sizeof(h11_header_t) <= 24)`.

**h11_config_t**

| Field | Type | Default | Purpose |
|-------|------|---------|---------|
| `max_body_size` | `uint64_t` | `UINT64_MAX` | Max body bytes (unlimited) |
| `max_request_line_len` | `uint32_t` | 8192 | Max request-line bytes |
| `max_header_line_len` | `uint32_t` | 8192 | Max single header line |
| `max_headers_size` | `uint32_t` | 65536 | Total header section bytes |
| `max_header_count` | `uint32_t` | 100 | Max number of headers |
| `max_chunk_ext_len` | `uint32_t` | 1024 | Max chunk extension bytes |
| `flags` | `uint32_t` | see below | Bitfield of `H11_CFG_*` constants |
| `reserved0` | `uint32_t` | 0 | Reserved for future use |

Default flags: `H11_CFG_STRICT_CRLF | H11_CFG_REJECT_OBS_FOLD | H11_CFG_ALLOW_OBS_TEXT | H11_CFG_ALLOW_LEADING_CRLF | H11_CFG_REJECT_TE_CL_CONFLICT`. Total size: 40 bytes.

**h11_request_t** — parsed request output

| Field | Type | Notes |
|-------|------|-------|
| `method` | `h11_span_t` | Case-sensitive token |
| `target` | `h11_span_t` | Raw request-target |
| `content_length` | `uint64_t` | Valid if body_type=CL |
| `header_count` | `uint32_t` | Number of parsed headers |
| `trailer_count` | `uint32_t` | Number of parsed trailers |
| `version` | `uint16_t` | Packed: `(major << 8) | minor` (e.g. `0x0101` for HTTP/1.1) |
| `target_form` | `uint8_t` | `h11_target_form_t` value |
| `body_type` | `uint8_t` | `h11_body_type_t` value |
| `flags` | `uint16_t` | Bitfield of `H11_REQF_*` constants |
| `reserved0` | `uint16_t` | Reserved |
| `known_idx` | `uint16_t[H11_KHDR_COUNT]` | Index into `headers[]` for each known header, or `H11_INDEX_NONE` |
| `reserved1` | `uint16_t` | Reserved |
| `headers` | `h11_header_t *` | Dynamic array |
| `trailers` | `h11_header_t *` | Dynamic array (chunked only) |

Boolean state is encoded in `flags`: `H11_REQF_KEEP_ALIVE`, `H11_REQF_EXPECT_CONTINUE`, `H11_REQF_HAS_UPGRADE`, etc. Capacity fields are internal to the parser and not exposed. `_Static_assert(sizeof(h11_request_t) <= 96)`.

### S2.3 Functions

| Signature | Semantics |
|-----------|-----------|
| `h11_config_t h11_config_default(void)` | Returns config with defaults above |
| `h11_parser_t *h11_parser_new(const h11_config_t *config)` | Allocate parser; NULL config uses defaults; calls h11_init() |
| `void h11_parser_free(h11_parser_t *parser)` | Free parser and dynamic arrays |
| `void h11_parser_reset(h11_parser_t *parser)` | Reset for next request (pipelining); keeps allocated arrays |
| `h11_error_t h11_parse(h11_parser_t *p, const char *data, size_t len, size_t *consumed)` | Drive state machine; returns OK/NEED_MORE_DATA/error |
| `h11_state_t h11_get_state(const h11_parser_t *p)` | Current parser state |
| `const h11_request_t *h11_get_request(const h11_parser_t *p)` | Access parsed request |
| `h11_error_t h11_read_body(h11_parser_t *p, const char *data, size_t len, size_t *consumed, const char **body_out, size_t *body_len)` | Zero-copy body read; valid in BODY\_IDENTITY or BODY\_CHUNKED\_DATA |
| `const char *h11_error_name(h11_error_t error)` | Enum name as string |
| `const char *h11_error_message(h11_error_t error)` | Human-readable message |
| `size_t h11_error_offset(const h11_parser_t *p)` | Byte offset of error |
| `bool h11_header_name_eq(const char *base, h11_span_t name, const char *cmp)` | Case-insensitive name comparison; `base` is the input buffer |
| `int h11_find_header(const h11_request_t *req, const char *base, const char *name)` | Find header index by name, -1 if absent; `base` is the input buffer |

## S3. Internal Types

### S3.1 SIMD Level

| Level | Enum Value | Vector Width |
|-------|------------|-------------|
| Scalar | `H11_SIMD_SCALAR = 0` | 1 byte |
| SSE4.2 | `H11_SIMD_SSE42 = 1` | 16 bytes |
| AVX2 | `H11_SIMD_AVX2 = 2` | 32 bytes |
| AVX-512BW | `H11_SIMD_AVX512 = 3` | 64 bytes |

Global `h11_simd_level_t h11_simd_level` — set once by `h11_init()`.

### S3.2 Compiler Macros

| Macro | Expansion (GCC/Clang) | Fallback |
|-------|----------------------|----------|
| `H11_LIKELY(x)` | `__builtin_expect(!!(x), 1)` | `(x)` |
| `H11_UNLIKELY(x)` | `__builtin_expect(!!(x), 0)` | `(x)` |
| `H11_INLINE` | `static inline __attribute__((always_inline))` | `static inline` |
| `H11_NOINLINE` | `__attribute__((noinline))` | (empty) |

### S3.3 Character Tables & Macros

Five `extern const uint8_t [256]` tables in `util.c`:

| Table | Membership |
|-------|-----------|
| `h11_tchar_table` | `! # $ % & ' * + - . ^ _ \` \| ~ DIGIT ALPHA` (RFC 9110 S5.6.2) |
| `h11_vchar_table` | VCHAR (0x21-0x7E) + SP + HTAB + obs-text (0x80-0xFF) |
| `h11_digit_table` | `0-9` |
| `h11_hexdig_table` | `0-9 A-F a-f` |
| `h11_uri_table` | unreserved + sub-delims + `:` `@` `/` `%` (RFC 3986) |

Character macros: `H11_IS_TCHAR(c)`, `H11_IS_VCHAR(c)`, `H11_IS_DIGIT(c)`, `H11_IS_HEXDIG(c)`, `H11_IS_URI(c)`, `H11_IS_SP(c)`, `H11_IS_HTAB(c)`, `H11_IS_OWS(c)`, `H11_IS_CR(c)`, `H11_IS_LF(c)` — all index into tables via `(uint8_t)(c)`.

Hex value lookup: `int h11_hexval(char c)` — returns 0-15 or -1 via `h11_hexval_table[256]` (int8_t, -1 sentinels for non-hex).

### S3.4 Parser Struct (h11_parser)

| Field | Type | Purpose |
|-------|------|---------|
| `config` | `h11_config_t` | Immutable after init |
| `state` | `h11_state_t` | Current state |
| `last_error` | `h11_error_t` | Stored error code |
| `error_offset` | `size_t` | Byte offset of error |
| `request` | `h11_request_t` | Current parsed request |
| `total_consumed` | `size_t` | Total bytes consumed this request |
| `line_start` | `size_t` | Start of current line |
| `headers_size` | `size_t` | Accumulated header bytes |
| `body_remaining` | `uint64_t` | Bytes left in body/chunk |
| `total_body_read` | `uint64_t` | Total body bytes delivered |
| `in_chunk_ext` | `bool` | In chunk extension |
| `chunk_ext_len` | `size_t` | Current ext length |
| `seen_host` | `bool` | Host header encountered |
| `seen_content_length` | `bool` | CL header encountered |
| `seen_transfer_encoding` | `bool` | TE header encountered |
| `is_chunked` | `bool` | TE validated as chunked |
| `leading_crlf_consumed` | `bool` | Leading empty lines consumed |

### S3.5 Internal Functions

| Signature | Purpose |
|-----------|---------|
| `bool h11_span_eq_case(const char *base, h11_span_t a, const char *b, size_t blen)` | Case-insensitive span comparison; `base` is the input buffer |
| `int h11_hexval(char c)` | Hex digit → 0-15, or -1 |
| `void h11_init(void)` | One-time CPU detection |

## S4. SIMD CPU Detection

**Detection hierarchy** (highest to lowest):

| Level | CPUID Check | OS Support (XCR0) |
|-------|------------|-------------------|
| AVX-512BW | EAX=7,ECX=0: EBX bit 16 (AVX512F) + bit 30 (AVX512BW) | `(XCR0 & 0xE6) == 0xE6` (XMM+YMM+ZMM+opmask) |
| AVX2 | EAX=7,ECX=0: EBX bit 5 | `(XCR0 & 0x06) == 0x06` (XMM+YMM) |
| SSE4.2 | EAX=1: ECX bit 20 | (always available if CPU reports it) |
| Scalar | (fallback) | — |

Prerequisites: OSXSAVE (EAX=1: ECX bit 27) required for XCR0 check. ARM64: map to SSE42 equivalent (NEON always available).

`h11_init()` called once — guarded by `static bool h11_initialized`. Called automatically from `h11_parser_new()`.

## S5. Scanner Primitives

### S5.1 find_crlf()

`static ssize_t find_crlf(const char *data, size_t len)` — returns offset of `\r` in `\r\n`, or -1.

Dispatch: switch on `h11_simd_level` → `find_crlf_avx512` / `find_crlf_avx2` / `find_crlf_sse42` / `find_crlf_scalar`.

| Level | Algorithm |
|-------|-----------|
| AVX-512BW | Broadcast `\r` to 64B, `_mm512_cmpeq_epi8_mask`, `__builtin_ctzll` per hit, verify `\n` follows |
| AVX2 | Broadcast `\r` to 32B, `_mm256_cmpeq_epi8` + `_mm256_movemask_epi8`, `__builtin_ctz` per hit, verify `\n` |
| SSE4.2 | Broadcast `\r` to 16B, `_mm_cmpeq_epi8` + `_mm_movemask_epi8`, `__builtin_ctz` per hit, verify `\n` |
| Scalar | Byte-by-byte: `data[i]=='\r' && data[i+1]=='\n'` |

All SIMD variants fall back to scalar for the tail (remaining bytes < vector width).

### S5.2 find_char()

`static ssize_t find_char(const char *data, size_t len, char target)` — returns offset of first `target`, or -1.

Dispatch: AVX512/AVX2 share `find_char_avx2`; SSE42 uses `find_char_sse42`; else scalar. Same broadcast-compare-mask pattern as find_crlf but for arbitrary single character.

### S5.3 find_line_ending() (bare-LF mode)

When `H11_CFG_STRICT_CRLF` is not set: try `find_crlf()` first; if not found, scan for bare `\n`. If bare `\n` preceded by `\r`, treat as CRLF. Returns position and `bool *is_crlf` flag.

## S6. State Machine

### S6.1 Transition Table

| From | To | Condition |
|------|----|-----------|
| IDLE | REQUEST_LINE | Data available (after optional leading CRLF) |
| REQUEST_LINE | HEADERS | Request line parsed |
| HEADERS | COMPLETE | No body (no CL, no TE) |
| HEADERS | BODY_IDENTITY | Content-Length > 0 |
| HEADERS | COMPLETE | Content-Length == 0 |
| HEADERS | BODY_CHUNKED_SIZE | Transfer-Encoding: chunked |
| BODY_IDENTITY | COMPLETE | All CL bytes consumed |
| BODY_CHUNKED_SIZE | BODY_CHUNKED_DATA | chunk_size > 0 |
| BODY_CHUNKED_SIZE | TRAILERS | chunk_size == 0 (last chunk) |
| BODY_CHUNKED_DATA | BODY_CHUNKED_CRLF | All chunk bytes consumed |
| BODY_CHUNKED_CRLF | BODY_CHUNKED_SIZE | CRLF consumed |
| TRAILERS | COMPLETE | Empty line parsed |
| Any | ERROR | Parse error |
| COMPLETE | IDLE | `h11_parser_reset()` called |

### S6.2 h11_parse() Dispatch

1. Return stored error if state==ERROR
2. Loop while `*consumed < len`:
   - IDLE: consume optional leading CRLF if configured, transition to REQUEST_LINE
   - REQUEST_LINE: call `h11_parse_request_line()`, on success → HEADERS
   - HEADERS: call `h11_parse_headers()`, on success → body state per framing
   - BODY_IDENTITY / BODY_CHUNKED_DATA: return H11_OK (caller uses `h11_read_body()`)
   - BODY_CHUNKED_SIZE: call `h11_parse_chunk_size()`
   - BODY_CHUNKED_CRLF: expect `\r\n`, then → BODY_CHUNKED_SIZE
   - TRAILERS: call `h11_parse_trailers()`, on empty line → COMPLETE
   - COMPLETE: return H11_OK

### S6.3 Error Handling

`set_error(p, err, offset)` sets state=ERROR, stores error code and byte offset. No recovery from ERROR — caller must close connection or call `h11_parser_reset()`.

### S6.4 Lifecycle

`h11_parser_new()` → `h11_parse()` (+ `h11_read_body()` for bodies) → `h11_parser_reset()` (pipelining) → `h11_parser_free()`

## S7. Character Tables

| Table | Members |
|-------|---------|
| tchar | `!#$%&'*+-.^_\`\|~` + `0-9` + `A-Za-z` |
| vchar | 0x09 (HTAB), 0x20 (SP), 0x21-0x7E (visible ASCII), 0x80-0xFF (obs-text) |
| digit | `0-9` |
| hexdig | `0-9`, `A-F`, `a-f` |
| uri | unreserved (`A-Za-z0-9-._~`) + sub-delims (`!$&'()*+,;=`) + `:@/%` |

**Hex value table**: `int8_t h11_hexval_table[256]` — `'0'-'9'` → 0-9, `'A'-'F'/'a'-'f'` → 10-15, all others → -1.

## S8. Error-to-HTTP Mapping

| Error Pattern | HTTP Status |
|--------------|-------------|
| `H11_ERR_INVALID_METHOD/TARGET/VERSION/CRLF` | 400 Bad Request |
| `H11_ERR_INVALID_HEADER_NAME/VALUE`, `H11_ERR_OBS_FOLD_REJECTED`, `H11_ERR_LEADING_WHITESPACE` | 400 Bad Request |
| `H11_ERR_MISSING_HOST`, `H11_ERR_MULTIPLE_HOST`, `H11_ERR_INVALID_HOST` | 400 Bad Request |
| `H11_ERR_INVALID_CONTENT_LENGTH`, `H11_ERR_MULTIPLE_CONTENT_LENGTH`, `H11_ERR_CONTENT_LENGTH_OVERFLOW` | 400 Bad Request |
| `H11_ERR_INVALID_TRANSFER_ENCODING`, `H11_ERR_TE_NOT_CHUNKED_FINAL`, `H11_ERR_TE_CL_CONFLICT` | 400 Bad Request |
| `H11_ERR_REQUEST_LINE_TOO_LONG`, `H11_ERR_HEADER_LINE_TOO_LONG` | 400 Bad Request |
| `H11_ERR_HEADERS_TOO_LARGE`, `H11_ERR_TOO_MANY_HEADERS` | 431 Request Header Fields Too Large |
| `H11_ERR_BODY_TOO_LARGE` | 413 Content Too Large |
| `H11_ERR_UNKNOWN_TRANSFER_CODING` | 501 Not Implemented |
| `H11_ERR_INVALID_CHUNK_*`, `H11_ERR_INVALID_TRAILER` | 400 Bad Request |

## Non-Goals

- HTTP/0.9 compatibility
- `Proxy-Connection` header handling
- Request-target normalization (dot-segment removal, percent-decoding)
