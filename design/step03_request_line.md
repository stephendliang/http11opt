# Step 3: Request Line Parsing

## Purpose

Parse the HTTP request line according to RFC 9112. The request-line is the first line of an HTTP request and contains the method, request-target, and HTTP version. **Parsing begins with the SIMD-accelerated CRLF scanner** - the fundamental operation that locates the line boundary.

## RFC References

- **RFC 9112 Section 3** - Request Line
- **RFC 9112 Section 3.1** - Method
- **RFC 9112 Section 3.2** - Request Target
- **RFC 9112 Section 2.3** - HTTP Version

## Files to Modify

- `parser.c` - Request line parsing with integrated SIMD scanning

---

## Part 1: The CRLF Scanner - Core Parsing Primitive

Before any parsing logic executes, we must find where the line ends. This is the parser's most critical operation.

### find_crlf() - The Heart of the Parser

```c
/*
 * Find CRLF sequence in buffer.
 * Returns offset of '\r' in CRLF, or -1 if not found.
 *
 * This function is called for EVERY line parsed:
 * - Request line
 * - Each header line
 * - Each chunk size line
 * - Each trailer line
 *
 * Performance here directly determines parser throughput.
 */
static ssize_t find_crlf(const char *data, size_t len) {
    if (len < 2) return -1;

    switch (h11_simd_level) {
#ifdef __AVX512BW__
    case H11_SIMD_AVX512:
        return find_crlf_avx512(data, len);
#endif
#ifdef __AVX2__
    case H11_SIMD_AVX2:
        return find_crlf_avx2(data, len);
#endif
#ifdef __SSE4_2__
    case H11_SIMD_SSE42:
        return find_crlf_sse42(data, len);
#endif
    default:
        return find_crlf_scalar(data, len);
    }
}
```

### AVX-512 Implementation (64 bytes/iteration)

```c
#ifdef __AVX512BW__
static ssize_t find_crlf_avx512(const char *data, size_t len) {
    const __m512i cr = _mm512_set1_epi8('\r');
    size_t i = 0;

    while (i + 64 <= len) {
        __m512i chunk = _mm512_loadu_si512(data + i);
        __mmask64 mask = _mm512_cmpeq_epi8_mask(chunk, cr);

        while (mask) {
            int pos = __builtin_ctzll(mask);
            if (i + pos + 1 < len && data[i + pos + 1] == '\n') {
                return i + pos;
            }
            mask &= mask - 1;
        }
        i += 64;
    }

    /* Scalar tail */
    for (; i + 1 < len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            return i;
        }
    }
    return -1;
}
#endif
```

### AVX2 Implementation (32 bytes/iteration)

```c
#ifdef __AVX2__
static ssize_t find_crlf_avx2(const char *data, size_t len) {
    const __m256i cr = _mm256_set1_epi8('\r');
    size_t i = 0;

    while (i + 32 <= len) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(data + i));
        __m256i cmp = _mm256_cmpeq_epi8(chunk, cr);
        int mask = _mm256_movemask_epi8(cmp);

        while (mask) {
            int pos = __builtin_ctz(mask);
            if (i + pos + 1 < len && data[i + pos + 1] == '\n') {
                return i + pos;
            }
            mask &= mask - 1;
        }
        i += 32;
    }

    for (; i + 1 < len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            return i;
        }
    }
    return -1;
}
#endif
```

### SSE4.2 Implementation (16 bytes/iteration)

```c
#ifdef __SSE4_2__
static ssize_t find_crlf_sse42(const char *data, size_t len) {
    const __m128i cr = _mm_set1_epi8('\r');
    size_t i = 0;

    while (i + 16 <= len) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(data + i));
        __m128i cmp = _mm_cmpeq_epi8(chunk, cr);
        int mask = _mm_movemask_epi8(cmp);

        while (mask) {
            int pos = __builtin_ctz(mask);
            if (i + pos + 1 < len && data[i + pos + 1] == '\n') {
                return i + pos;
            }
            mask &= mask - 1;
        }
        i += 16;
    }

    for (; i + 1 < len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            return i;
        }
    }
    return -1;
}
#endif
```

### Scalar Fallback

```c
static ssize_t find_crlf_scalar(const char *data, size_t len) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            return i;
        }
    }
    return -1;
}
```

---

## Part 2: Request Line Format

### Grammar (RFC 9112)

```
request-line = method SP request-target SP HTTP-version CRLF

method       = token
token        = 1*tchar

HTTP-version = HTTP-name "/" DIGIT "." DIGIT
HTTP-name    = %x48.54.54.50 ; "HTTP"
```

### Example

```
GET /index.html HTTP/1.1\r\n
│   │           │
│   │           └── HTTP-version
│   └── request-target
└── method
```

---

## Part 3: Parsing Function

### Signature

```c
h11_error_t h11_parse_request_line(h11_parser_t *p, const char *data,
                                    size_t len, size_t *consumed);
```

### Implementation

```c
h11_error_t h11_parse_request_line(h11_parser_t *p, const char *data,
                                    size_t len, size_t *consumed) {
    *consumed = 0;

    /*=========================================================================
     * STEP 1: Find line boundary using SIMD scanner
     *========================================================================*/
    ssize_t crlf_pos = find_crlf(data, len);

    if (crlf_pos < 0) {
        /* No CRLF found - check if we've exceeded limit */
        if (len >= p->config.max_request_line_len) {
            p->error_offset = p->total_consumed;
            return H11_ERR_REQUEST_LINE_TOO_LONG;
        }
        return H11_NEED_MORE_DATA;
    }

    size_t line_len = (size_t)crlf_pos;

    /* Validate length */
    if (line_len > p->config.max_request_line_len) {
        p->error_offset = p->total_consumed;
        return H11_ERR_REQUEST_LINE_TOO_LONG;
    }

    /*=========================================================================
     * STEP 2: Parse method (token characters until SP)
     *========================================================================*/
    size_t pos = 0;

    while (pos < line_len && H11_IS_TCHAR(data[pos])) {
        pos++;
    }

    if (pos == 0) {
        p->error_offset = p->total_consumed;
        return H11_ERR_INVALID_METHOD;
    }

    size_t method_len = pos;

    /* Expect SP */
    if (pos >= line_len || data[pos] != ' ') {
        p->error_offset = p->total_consumed + pos;
        return H11_ERR_INVALID_METHOD;
    }
    pos++;

    /* Tolerate multiple spaces if configured */
    if (p->config.tolerate_spaces) {
        while (pos < line_len && (data[pos] == ' ' || data[pos] == '\t')) {
            pos++;
        }
    }

    /*=========================================================================
     * STEP 3: Parse request-target (until SP)
     *========================================================================*/
    size_t target_start = pos;

    while (pos < line_len && data[pos] != ' ' && (uint8_t)data[pos] > 0x1F) {
        pos++;
    }

    if (pos == target_start) {
        p->error_offset = p->total_consumed + pos;
        return H11_ERR_INVALID_TARGET;
    }

    size_t target_len = pos - target_start;

    /* Validate request-target characters */
    for (size_t i = target_start; i < pos; i++) {
        uint8_t c = (uint8_t)data[i];
        if (c <= 0x20 || c == 0x7F) {
            p->error_offset = p->total_consumed + i;
            return H11_ERR_INVALID_TARGET;
        }
    }

    /* Expect SP */
    if (pos >= line_len || data[pos] != ' ') {
        p->error_offset = p->total_consumed + pos;
        return H11_ERR_INVALID_TARGET;
    }
    pos++;

    if (p->config.tolerate_spaces) {
        while (pos < line_len && (data[pos] == ' ' || data[pos] == '\t')) {
            pos++;
        }
    }

    /*=========================================================================
     * STEP 4: Parse HTTP-version
     *========================================================================*/
    size_t version_start = pos;
    size_t version_len = line_len - pos;

    if (version_len < 8) {
        p->error_offset = p->total_consumed + version_start;
        return H11_ERR_INVALID_VERSION;
    }

    /* Check "HTTP/" prefix */
    if (data[pos] != 'H' || data[pos+1] != 'T' || data[pos+2] != 'T' ||
        data[pos+3] != 'P' || data[pos+4] != '/') {
        p->error_offset = p->total_consumed + version_start;
        return H11_ERR_INVALID_VERSION;
    }

    /* Major version */
    if (!H11_IS_DIGIT(data[pos+5])) {
        p->error_offset = p->total_consumed + pos + 5;
        return H11_ERR_INVALID_VERSION;
    }
    p->request.version_major = data[pos+5] - '0';

    /* Dot */
    if (data[pos+6] != '.') {
        p->error_offset = p->total_consumed + pos + 6;
        return H11_ERR_INVALID_VERSION;
    }

    /* Minor version */
    if (!H11_IS_DIGIT(data[pos+7])) {
        p->error_offset = p->total_consumed + pos + 7;
        return H11_ERR_INVALID_VERSION;
    }
    p->request.version_minor = data[pos+7] - '0';

    /* Validate version */
    if (p->request.version_major != 1) {
        p->error_offset = p->total_consumed + pos + 5;
        return H11_ERR_INVALID_VERSION;
    }

    /* Strict mode: no trailing characters */
    if (!p->config.tolerate_spaces && version_len != 8) {
        p->error_offset = p->total_consumed + pos + 8;
        return H11_ERR_INVALID_VERSION;
    }

    /*=========================================================================
     * STEP 5: Store parsed values and determine target form
     *========================================================================*/
    p->request.method = (h11_slice_t){data, method_len};
    p->request.target = (h11_slice_t){data + target_start, target_len};

    h11_error_t err = h11_determine_target_form(p->request.target,
                                                 &p->request.target_form);
    if (err != H11_OK) {
        p->error_offset = p->total_consumed + target_start;
        return err;
    }

    /* Set default keep-alive based on version */
    p->request.keep_alive = (p->request.version_minor >= 1);

    *consumed = line_len + 2;  /* Line + CRLF */
    return H11_OK;
}
```

---

## Part 4: Bare LF Handling (Tolerant Mode)

When `strict_crlf=false`, the parser can accept bare LF:

```c
static ssize_t find_line_ending(h11_parser_t *p, const char *data, size_t len,
                                 bool *is_crlf) {
    /* Try CRLF first */
    ssize_t pos = find_crlf(data, len);
    if (pos >= 0) {
        *is_crlf = true;
        return pos;
    }

    /* If tolerant, try bare LF */
    if (!p->config.strict_crlf) {
        for (size_t i = 0; i < len; i++) {
            if (data[i] == '\n') {
                *is_crlf = false;
                /* Check for preceding CR (treat as CRLF) */
                if (i > 0 && data[i-1] == '\r') {
                    *is_crlf = true;
                    return i - 1;
                }
                return i;
            }
        }
    }

    return -1;
}
```

---

## Part 5: Edge Cases

### 1. Empty Method

```
 /path HTTP/1.1\r\n
```
**Error**: `H11_ERR_INVALID_METHOD` at offset 0

### 2. Invalid Method Character

```
GET@POST /path HTTP/1.1\r\n
```
**Error**: `H11_ERR_INVALID_METHOD` at position of '@'

### 3. Missing SP Between Elements

```
GET/path HTTP/1.1\r\n
```
**Error**: `H11_ERR_INVALID_METHOD` at position of '/'

### 4. Invalid HTTP Version

```
GET /path HTTP/2.0\r\n
```
**Error**: `H11_ERR_INVALID_VERSION`

### 5. Lowercase HTTP

```
GET /path http/1.1\r\n
```
**Error**: `H11_ERR_INVALID_VERSION` (case-sensitive)

### 6. Missing Version

```
GET /path\r\n
```
**Error**: `H11_ERR_INVALID_VERSION` (incomplete)

### 7. Trailing Whitespace (Strict Mode)

```
GET /path HTTP/1.1 \r\n
```
**Error**: `H11_ERR_INVALID_VERSION` when `tolerate_spaces=false`

### 8. Multiple Spaces (Strict Mode)

```
GET  /path HTTP/1.1\r\n
```
**Error**: `H11_ERR_INVALID_METHOD` when `tolerate_spaces=false`
**Success**: When `tolerate_spaces=true`

### 9. Request Line Too Long

```
GET /vvvvery...long...path HTTP/1.1\r\n (>8192 bytes)
```
**Error**: `H11_ERR_REQUEST_LINE_TOO_LONG`

### 10. No CRLF in Buffer

```
GET /path HTTP/1.1 (no line ending)
```
**Return**: `H11_NEED_MORE_DATA` (waiting for more input)

---

## Part 6: Performance Characteristics

| SIMD Level | Bytes/Iteration | Typical Throughput |
|------------|-----------------|-------------------|
| AVX-512 | 64 | ~20 GB/s |
| AVX2 | 32 | ~12 GB/s |
| SSE4.2 | 16 | ~6 GB/s |
| Scalar | 1 | ~1 GB/s |

The CRLF scan dominates request line parsing time for typical requests.

---

## Implementation Checklist

- [ ] Implement find_crlf_avx512()
- [ ] Implement find_crlf_avx2()
- [ ] Implement find_crlf_sse42()
- [ ] Implement find_crlf_scalar()
- [ ] Implement find_crlf() dispatcher
- [ ] Implement h11_parse_request_line()
- [ ] Validate method token characters
- [ ] Validate request-target characters
- [ ] Parse HTTP-version strictly
- [ ] Handle strict_crlf mode
- [ ] Handle tolerate_spaces mode
- [ ] Check max_request_line_len
- [ ] Set version_major/version_minor
- [ ] Set default keep_alive based on version
- [ ] Call h11_determine_target_form()
- [ ] Store method and target slices
- [ ] Return accurate error offsets
