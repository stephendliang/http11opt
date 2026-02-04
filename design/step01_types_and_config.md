# Step 1: Types, Configuration, and CPU Detection

## Purpose

Define all fundamental types, enumerations, configuration structures, error codes, character classification tables, and **CPU feature detection** that the parser depends on. The SIMD capability level is determined once at startup and governs how the parser's core scanning operations execute.

## RFC References

- **RFC 9110** - HTTP Semantics (general definitions)
- **RFC 9112** - HTTP/1.1 Message Syntax (parsing rules)
- **RFC 3986** - URI Generic Syntax (character classes)

## Files to Modify

- `h11.h` - Public types and API declarations
- `h11_internal.h` - Internal types, macros, SIMD level enum
- `util.c` - Character tables and error message implementations
- `parser.c` - CPU detection and SIMD level initialization

---

## Part 1: SIMD Capability Detection

The parser's core scanning operations (finding CRLF, finding delimiters) use vectorized instructions when available. CPU capability is detected once at process start.

### SIMD Level Enum (h11_internal.h)

```c
typedef enum {
    H11_SIMD_SCALAR = 0,    /* No SIMD, pure C fallback */
    H11_SIMD_SSE42  = 1,    /* SSE4.2 (16-byte vectors) */
    H11_SIMD_AVX2   = 2,    /* AVX2 (32-byte vectors) */
    H11_SIMD_AVX512 = 3     /* AVX-512BW (64-byte vectors) */
} h11_simd_level_t;

/* Global SIMD level, set once at process start */
extern h11_simd_level_t h11_simd_level;
```

### CPU Detection (parser.c)

```c
#include <cpuid.h>

h11_simd_level_t h11_simd_level = H11_SIMD_SCALAR;
static bool h11_initialized = false;

void h11_init(void) {
    if (h11_initialized) return;
    h11_initialized = true;

#ifdef __x86_64__
    uint32_t eax, ebx, ecx, edx;

    /* Basic CPUID */
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        return;  /* CPUID not supported */
    }

    /* Check OSXSAVE (required for AVX state saving) */
    bool osxsave = (ecx & (1 << 27)) != 0;

    /* Check SSE4.2 */
    bool has_sse42 = (ecx & (1 << 20)) != 0;

    /* Get extended features */
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        bool has_avx2 = (ebx & (1 << 5)) != 0;
        bool has_avx512f = (ebx & (1 << 16)) != 0;
        bool has_avx512bw = (ebx & (1 << 30)) != 0;

        /* Check XCR0 for OS support of vector state */
        uint32_t xcr0_lo = 0, xcr0_hi = 0;
        if (osxsave) {
            __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
        }

        bool ymm_enabled = (xcr0_lo & 0x06) == 0x06;      /* XMM + YMM */
        bool zmm_enabled = (xcr0_lo & 0xE6) == 0xE6;      /* XMM + YMM + ZMM + opmask */

        if (has_avx512f && has_avx512bw && zmm_enabled) {
            h11_simd_level = H11_SIMD_AVX512;
        } else if (has_avx2 && ymm_enabled) {
            h11_simd_level = H11_SIMD_AVX2;
        } else if (has_sse42) {
            h11_simd_level = H11_SIMD_SSE42;
        }
    } else if (has_sse42) {
        h11_simd_level = H11_SIMD_SSE42;
    }
#endif

#ifdef __aarch64__
    /* ARM64 always has NEON, map to SSE42 equivalent */
    h11_simd_level = H11_SIMD_SSE42;
#endif
}
```

### Automatic Initialization

The parser calls `h11_init()` automatically on first use:

```c
h11_parser_t *h11_parser_new(const h11_config_t *config) {
    h11_init();  /* Ensure CPU detection has run */
    /* ... rest of initialization ... */
}
```

---

## Part 2: Error Codes (h11.h)

### Error Categories

```c
typedef enum h11_error {
    /* Success / incomplete */
    H11_OK = 0,                      // Parsing succeeded
    H11_NEED_MORE_DATA,              // Need more input bytes

    /* Request line errors (400 Bad Request) */
    H11_ERR_INVALID_METHOD,          // Method contains non-tchar
    H11_ERR_INVALID_TARGET,          // Request-target invalid
    H11_ERR_INVALID_VERSION,         // HTTP-version malformed
    H11_ERR_REQUEST_LINE_TOO_LONG,   // Exceeds max_request_line_len
    H11_ERR_INVALID_CRLF,            // Missing/invalid line ending

    /* Header errors (400 Bad Request or 431 Request Header Fields Too Large) */
    H11_ERR_INVALID_HEADER_NAME,     // Name contains non-tchar
    H11_ERR_INVALID_HEADER_VALUE,    // Value contains invalid chars
    H11_ERR_HEADER_LINE_TOO_LONG,    // Single header exceeds limit
    H11_ERR_TOO_MANY_HEADERS,        // Exceeds max_header_count
    H11_ERR_HEADERS_TOO_LARGE,       // Total size exceeds limit
    H11_ERR_OBS_FOLD_REJECTED,       // obs-fold when reject_obs_fold=true
    H11_ERR_LEADING_WHITESPACE,      // SP/HTAB before first header

    /* Semantic errors (400 Bad Request) */
    H11_ERR_MISSING_HOST,            // HTTP/1.1 missing Host
    H11_ERR_MULTIPLE_HOST,           // More than one Host header
    H11_ERR_INVALID_HOST,            // Host value malformed
    H11_ERR_INVALID_CONTENT_LENGTH,  // CL not valid integer
    H11_ERR_MULTIPLE_CONTENT_LENGTH, // Conflicting CL values
    H11_ERR_CONTENT_LENGTH_OVERFLOW, // CL exceeds uint64_t
    H11_ERR_INVALID_TRANSFER_ENCODING, // TE syntax error
    H11_ERR_TE_NOT_CHUNKED_FINAL,    // TE doesn't end with chunked
    H11_ERR_TE_CL_CONFLICT,          // Both TE and CL present
    H11_ERR_UNKNOWN_TRANSFER_CODING, // Unknown coding (501)

    /* Body errors (400 Bad Request or 413 Content Too Large) */
    H11_ERR_BODY_TOO_LARGE,          // Exceeds max_body_size
    H11_ERR_INVALID_CHUNK_SIZE,      // Chunk size not valid hex
    H11_ERR_CHUNK_SIZE_OVERFLOW,     // Chunk size exceeds uint64_t
    H11_ERR_INVALID_CHUNK_EXT,       // Chunk extension syntax error
    H11_ERR_CHUNK_EXT_TOO_LONG,      // Chunk ext exceeds limit
    H11_ERR_INVALID_CHUNK_DATA,      // Chunk data/CRLF error
    H11_ERR_INVALID_TRAILER,         // Trailer field error

    /* Fatal errors */
    H11_ERR_CONNECTION_CLOSED,       // Unexpected EOF
    H11_ERR_INTERNAL                 // Internal parser error
} h11_error_t;
```

### Error-to-HTTP Status Mapping

| Error Code | HTTP Status | Description |
|------------|-------------|-------------|
| `H11_ERR_INVALID_*` (request line) | 400 | Bad Request |
| `H11_ERR_*_HEADER_*` | 400 or 431 | Header errors |
| `H11_ERR_MISSING_HOST` | 400 | Bad Request |
| `H11_ERR_BODY_TOO_LARGE` | 413 | Content Too Large |
| `H11_ERR_HEADERS_TOO_LARGE` | 431 | Request Header Fields Too Large |
| `H11_ERR_TOO_MANY_HEADERS` | 431 | Request Header Fields Too Large |
| `H11_ERR_UNKNOWN_TRANSFER_CODING` | 501 | Not Implemented |

---

## Part 3: Parser States (h11.h)

```c
typedef enum h11_state {
    H11_STATE_IDLE,              // Ready for new request
    H11_STATE_REQUEST_LINE,      // Parsing request-line
    H11_STATE_HEADERS,           // Parsing header fields
    H11_STATE_BODY_IDENTITY,     // Reading Content-Length body
    H11_STATE_BODY_CHUNKED_SIZE, // Reading chunk-size line
    H11_STATE_BODY_CHUNKED_DATA, // Reading chunk octets
    H11_STATE_BODY_CHUNKED_CRLF, // Expecting CRLF after chunk
    H11_STATE_TRAILERS,          // Parsing trailer section
    H11_STATE_COMPLETE,          // Request fully parsed
    H11_STATE_ERROR              // Unrecoverable error state
} h11_state_t;
```

---

## Part 4: Configuration Structure (h11.h)

```c
typedef struct h11_config {
    /* Size limits (bytes) */
    size_t max_request_line_len;    // Max request-line length
    size_t max_header_line_len;     // Max single header line
    size_t max_headers_size;        // Total header section size
    size_t max_header_count;        // Maximum number of headers
    size_t max_body_size;           // Maximum body size
    size_t max_chunk_ext_len;       // Max chunk extension length

    /* Tolerance flags */
    bool strict_crlf;               // Require CRLF, reject bare LF
    bool reject_obs_fold;           // Reject obs-fold (line folding)
    bool allow_obs_text;            // Allow 0x80-0xFF in field values
    bool allow_leading_crlf;        // Ignore leading empty lines
    bool tolerate_spaces;           // Lax whitespace in request-line

    /* Conflict resolution */
    bool reject_te_cl_conflict;     // Reject Transfer-Encoding + Content-Length
} h11_config_t;
```

### Default Configuration Function

```c
h11_config_t h11_config_default(void) {
    return (h11_config_t){
        .max_request_line_len = 8192,
        .max_header_line_len  = 8192,
        .max_headers_size     = 65536,   // 64KB
        .max_header_count     = 100,
        .max_body_size        = SIZE_MAX, // Unlimited by default
        .max_chunk_ext_len    = 1024,

        .strict_crlf          = true,
        .reject_obs_fold      = true,
        .allow_obs_text       = true,
        .allow_leading_crlf   = true,
        .tolerate_spaces      = false,

        .reject_te_cl_conflict = true
    };
}
```

---

## Part 5: Character Classification Tables (util.c)

### Token Characters (tchar)

RFC 9110 Section 5.6.2:
```
token = 1*tchar
tchar = "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+" / "-" / "." /
        "^" / "_" / "`" / "|" / "~" / DIGIT / ALPHA
```

```c
const uint8_t h11_tchar_table[256] = {
    /*       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
    /* 0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 1 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 2 */ 0, 1, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0,
    /* 3 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,
    /* 4 */ 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 5 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1,
    /* 6 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 7 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0,
    /* 8+ all zeros */
};
```

### Field Value Characters (vchar + OWS + obs-text)

```c
const uint8_t h11_vchar_table[256] = {
    /*       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
    /* 0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, /* HTAB=0x09 */
    /* 1 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 2 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* SP + visible */
    /* 3-7: all visible ASCII = 1 */
    /* 7F */ 0,  /* DEL invalid */
    /* 80-FF: obs-text = 1 if allow_obs_text */
};
```

### Hex Value Table

```c
static const int8_t h11_hexval_table[256] = {
    /* 0-9 = 0-9, A-F = 10-15, a-f = 10-15, rest = -1 */
    ['0'] = 0, ['1'] = 1, ['2'] = 2, ['3'] = 3, ['4'] = 4,
    ['5'] = 5, ['6'] = 6, ['7'] = 7, ['8'] = 8, ['9'] = 9,
    ['A'] = 10, ['B'] = 11, ['C'] = 12, ['D'] = 13, ['E'] = 14, ['F'] = 15,
    ['a'] = 10, ['b'] = 11, ['c'] = 12, ['d'] = 13, ['e'] = 14, ['f'] = 15,
    /* All other entries implicitly -1 via designated initializers */
};

int h11_hexval(char c) {
    int v = h11_hexval_table[(uint8_t)c];
    return v ? v : (c == '0' ? 0 : -1);
}
```

---

## Part 6: Error Messages (util.c)

```c
static const char *error_messages[] = {
    [H11_OK]                        = "Success",
    [H11_NEED_MORE_DATA]            = "Incomplete input, need more data",
    [H11_ERR_INVALID_METHOD]        = "Invalid character in HTTP method",
    [H11_ERR_INVALID_TARGET]        = "Invalid request-target",
    [H11_ERR_INVALID_VERSION]       = "Invalid HTTP version",
    [H11_ERR_REQUEST_LINE_TOO_LONG] = "Request-line exceeds maximum length",
    [H11_ERR_INVALID_CRLF]          = "Invalid or missing CRLF",
    [H11_ERR_INVALID_HEADER_NAME]   = "Invalid character in header name",
    [H11_ERR_INVALID_HEADER_VALUE]  = "Invalid character in header value",
    [H11_ERR_HEADER_LINE_TOO_LONG]  = "Header line exceeds maximum length",
    [H11_ERR_TOO_MANY_HEADERS]      = "Too many header fields",
    [H11_ERR_HEADERS_TOO_LARGE]     = "Header section exceeds maximum size",
    [H11_ERR_OBS_FOLD_REJECTED]     = "Obsolete line folding not allowed",
    [H11_ERR_LEADING_WHITESPACE]    = "Invalid leading whitespace",
    [H11_ERR_MISSING_HOST]          = "Missing Host header in HTTP/1.1 request",
    [H11_ERR_MULTIPLE_HOST]         = "Multiple Host headers",
    [H11_ERR_INVALID_HOST]          = "Invalid Host header value",
    [H11_ERR_INVALID_CONTENT_LENGTH] = "Invalid Content-Length value",
    [H11_ERR_MULTIPLE_CONTENT_LENGTH] = "Multiple conflicting Content-Length values",
    [H11_ERR_CONTENT_LENGTH_OVERFLOW] = "Content-Length value overflow",
    [H11_ERR_INVALID_TRANSFER_ENCODING] = "Invalid Transfer-Encoding header",
    [H11_ERR_TE_NOT_CHUNKED_FINAL]  = "Transfer-Encoding must end with chunked",
    [H11_ERR_TE_CL_CONFLICT]        = "Both Transfer-Encoding and Content-Length present",
    [H11_ERR_UNKNOWN_TRANSFER_CODING] = "Unknown transfer coding",
    [H11_ERR_BODY_TOO_LARGE]        = "Request body exceeds maximum size",
    [H11_ERR_INVALID_CHUNK_SIZE]    = "Invalid chunk size",
    [H11_ERR_CHUNK_SIZE_OVERFLOW]   = "Chunk size overflow",
    [H11_ERR_INVALID_CHUNK_EXT]     = "Invalid chunk extension",
    [H11_ERR_CHUNK_EXT_TOO_LONG]    = "Chunk extension exceeds maximum length",
    [H11_ERR_INVALID_CHUNK_DATA]    = "Invalid chunk data or terminator",
    [H11_ERR_INVALID_TRAILER]       = "Invalid trailer field",
    [H11_ERR_CONNECTION_CLOSED]     = "Unexpected connection close",
    [H11_ERR_INTERNAL]              = "Internal parser error"
};

const char *h11_error_message(h11_error_t error) {
    if (error >= 0 && error < sizeof(error_messages)/sizeof(error_messages[0])) {
        return error_messages[error];
    }
    return "Unknown error";
}
```

---

## Part 7: Slice Type and Utilities

```c
typedef struct h11_slice {
    const char *data;   // Pointer into source buffer (NOT null-terminated)
    size_t len;         // Length in bytes
} h11_slice_t;

/* Case-insensitive comparison for header names */
bool h11_slice_eq_case(h11_slice_t a, const char *b, size_t blen) {
    if (a.len != blen) return false;
    for (size_t i = 0; i < blen; i++) {
        char ca = a.data[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
    }
    return true;
}
```

---

## Implementation Checklist

- [ ] Define h11_simd_level_t enum
- [ ] Implement h11_init() with CPUID detection
- [ ] Handle x86_64 AVX-512, AVX2, SSE4.2 detection
- [ ] Handle ARM64 NEON detection
- [ ] Define all error enums in h11.h
- [ ] Define h11_config_t with defaults
- [ ] Define h11_slice_t
- [ ] Define h11_header_t
- [ ] Define h11_request_t
- [ ] Create character tables in util.c
- [ ] Implement h11_config_default()
- [ ] Implement error name/message functions
- [ ] Implement slice comparison utilities
- [ ] Call h11_init() from h11_parser_new()
