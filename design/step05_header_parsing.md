# Step 5: Header Field Parsing

## Purpose

Parse HTTP header fields according to RFC 9110/9112. Each header line is located using the **SIMD CRLF scanner**, then the colon delimiter is found using the **SIMD character scanner**. These vectorized operations are the core of header parsing.

## RFC References

- **RFC 9110 Section 5** - Fields
- **RFC 9110 Section 5.1** - Field Names
- **RFC 9110 Section 5.5** - Field Values
- **RFC 9112 Section 5** - Field Syntax

## Files to Modify

- `parser.c` - Header parsing with integrated SIMD scanning

---

## Part 1: SIMD Scanners for Header Parsing

Header parsing uses two SIMD primitives:
1. **find_crlf()** - Locate end of each header line (already defined in Step 3)
2. **find_char()** - Locate the colon separating name from value

### find_char() - Delimiter Scanner

```c
/*
 * Find first occurrence of character in buffer.
 * Returns offset or -1 if not found.
 *
 * Used to find ':' in header lines for name/value split.
 */
static ssize_t find_char(const char *data, size_t len, char target) {
    switch (h11_simd_level) {
#ifdef __AVX2__
    case H11_SIMD_AVX512:
    case H11_SIMD_AVX2:
        return find_char_avx2(data, len, target);
#endif
#ifdef __SSE4_2__
    case H11_SIMD_SSE42:
        return find_char_sse42(data, len, target);
#endif
    default:
        return find_char_scalar(data, len, target);
    }
}
```

### AVX2 Character Search

```c
#ifdef __AVX2__
static ssize_t find_char_avx2(const char *data, size_t len, char target) {
    const __m256i needle = _mm256_set1_epi8(target);
    size_t i = 0;

    while (i + 32 <= len) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(data + i));
        __m256i cmp = _mm256_cmpeq_epi8(chunk, needle);
        int mask = _mm256_movemask_epi8(cmp);

        if (mask) {
            return i + __builtin_ctz(mask);
        }
        i += 32;
    }

    for (; i < len; i++) {
        if (data[i] == target) return i;
    }
    return -1;
}
#endif
```

### SSE4.2 Character Search

```c
#ifdef __SSE4_2__
static ssize_t find_char_sse42(const char *data, size_t len, char target) {
    const __m128i needle = _mm_set1_epi8(target);
    size_t i = 0;

    while (i + 16 <= len) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(data + i));
        __m128i cmp = _mm_cmpeq_epi8(chunk, needle);
        int mask = _mm_movemask_epi8(cmp);

        if (mask) {
            return i + __builtin_ctz(mask);
        }
        i += 16;
    }

    for (; i < len; i++) {
        if (data[i] == target) return i;
    }
    return -1;
}
#endif
```

### Scalar Character Search

```c
static ssize_t find_char_scalar(const char *data, size_t len, char target) {
    for (size_t i = 0; i < len; i++) {
        if (data[i] == target) return i;
    }
    return -1;
}
```

---

## Part 2: Header Field Grammar

### RFC 9112 Syntax

```
header-section = *( field-line CRLF )
field-line     = field-name ":" OWS field-value OWS

field-name     = token
field-value    = *field-content
field-content  = field-vchar
                 [ 1*( SP / HTAB / field-vchar ) field-vchar ]
field-vchar    = VCHAR / obs-text

OWS            = *( SP / HTAB )      ; optional whitespace
VCHAR          = %x21-7E             ; visible ASCII
obs-text       = %x80-FF             ; high-byte extension
```

### Visual Example

```
Content-Type: text/html; charset=utf-8\r\n
│            │ │                      │
field-name   │ │                      CRLF
             : OWS
               field-value
```

---

## Part 3: Main Header Parsing Function

```c
h11_error_t h11_parse_headers(h11_parser_t *p, const char *data,
                              size_t len, size_t *consumed) {
    *consumed = 0;

    while (*consumed < len) {
        const char *line = data + *consumed;
        size_t remaining = len - *consumed;

        /*=====================================================================
         * STEP 1: Find line boundary using SIMD CRLF scanner
         *====================================================================*/
        ssize_t crlf_pos = find_crlf(line, remaining);

        if (crlf_pos < 0) {
            /* Check limits before waiting */
            if (remaining >= p->config.max_header_line_len) {
                p->error_offset = p->total_consumed + *consumed;
                return H11_ERR_HEADER_LINE_TOO_LONG;
            }
            if (p->headers_size + remaining > p->config.max_headers_size) {
                p->error_offset = p->total_consumed + *consumed;
                return H11_ERR_HEADERS_TOO_LARGE;
            }
            return H11_NEED_MORE_DATA;
        }

        size_t line_len = (size_t)crlf_pos;

        /* Check header line length */
        if (line_len > p->config.max_header_line_len) {
            p->error_offset = p->total_consumed + *consumed;
            return H11_ERR_HEADER_LINE_TOO_LONG;
        }

        /*=====================================================================
         * STEP 2: Empty line marks end of headers
         *====================================================================*/
        if (line_len == 0) {
            *consumed += 2;
            p->headers_size += 2;

            /* Finalize and determine body framing */
            h11_error_t err = finalize_headers(p);
            if (err != H11_OK) return err;

            /* Transition to next state */
            switch (p->request.body_type) {
            case H11_BODY_NONE:
                p->state = H11_STATE_COMPLETE;
                break;
            case H11_BODY_CONTENT_LENGTH:
                p->state = H11_STATE_BODY_IDENTITY;
                p->body_remaining = p->request.content_length;
                break;
            case H11_BODY_CHUNKED:
                p->state = H11_STATE_BODY_CHUNKED_SIZE;
                break;
            }
            return H11_OK;
        }

        /*=====================================================================
         * STEP 3: Check for obs-fold (line starting with SP/HTAB)
         *====================================================================*/
        if (line[0] == ' ' || line[0] == '\t') {
            if (p->request.header_count == 0) {
                p->error_offset = p->total_consumed + *consumed;
                return H11_ERR_LEADING_WHITESPACE;
            }

            if (p->config.reject_obs_fold) {
                p->error_offset = p->total_consumed + *consumed;
                return H11_ERR_OBS_FOLD_REJECTED;
            }

            /* Tolerant: skip obs-fold line */
            *consumed += line_len + 2;
            p->headers_size += line_len + 2;
            continue;
        }

        /*=====================================================================
         * STEP 4: Parse header field using SIMD colon scanner
         *====================================================================*/
        h11_error_t err = parse_header_line(p, line, line_len,
                                            p->total_consumed + *consumed);
        if (err != H11_OK) return err;

        *consumed += line_len + 2;
        p->headers_size += line_len + 2;

        /* Check limits */
        if (p->request.header_count > p->config.max_header_count) {
            p->error_offset = p->total_consumed + *consumed;
            return H11_ERR_TOO_MANY_HEADERS;
        }
        if (p->headers_size > p->config.max_headers_size) {
            p->error_offset = p->total_consumed + *consumed;
            return H11_ERR_HEADERS_TOO_LARGE;
        }
    }

    return H11_NEED_MORE_DATA;
}
```

---

## Part 4: Single Header Line Parsing

```c
static h11_error_t parse_header_line(h11_parser_t *p, const char *line,
                                      size_t len, size_t base_offset) {
    /*=========================================================================
     * STEP 1: Find colon using SIMD character scanner
     *========================================================================*/
    ssize_t colon_pos = find_char(line, len, ':');

    if (colon_pos <= 0) {
        /* No colon, or colon at position 0 (empty name) */
        p->error_offset = base_offset + (colon_pos < 0 ? len : 0);
        return H11_ERR_INVALID_HEADER_NAME;
    }

    size_t name_len = (size_t)colon_pos;

    /*=========================================================================
     * STEP 2: Validate field-name (must be all tchar)
     *========================================================================*/
    for (size_t i = 0; i < name_len; i++) {
        if (!H11_IS_TCHAR(line[i])) {
            p->error_offset = base_offset + i;
            return H11_ERR_INVALID_HEADER_NAME;
        }
    }

    /*=========================================================================
     * STEP 3: Skip OWS after colon, find field-value bounds
     *========================================================================*/
    size_t pos = name_len + 1;  /* Skip colon */

    /* Skip leading OWS */
    while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) {
        pos++;
    }

    size_t value_start = pos;
    size_t value_end = len;

    /* Trim trailing OWS */
    while (value_end > value_start &&
           (line[value_end - 1] == ' ' || line[value_end - 1] == '\t')) {
        value_end--;
    }

    /*=========================================================================
     * STEP 4: Validate field-value characters
     *========================================================================*/
    for (size_t i = value_start; i < value_end; i++) {
        uint8_t c = (uint8_t)line[i];

        /* SP and HTAB allowed */
        if (c == ' ' || c == '\t') continue;

        /* VCHAR (0x21-0x7E) */
        if (c >= 0x21 && c <= 0x7E) continue;

        /* obs-text (0x80-0xFF) if configured */
        if (c >= 0x80 && p->config.allow_obs_text) continue;

        /* Invalid */
        p->error_offset = base_offset + i;
        return H11_ERR_INVALID_HEADER_VALUE;
    }

    /*=========================================================================
     * STEP 5: Store header and track semantics
     *========================================================================*/
    h11_slice_t name = {line, name_len};
    h11_slice_t value = {line + value_start, value_end - value_start};

    h11_error_t err = h11_add_header(&p->request, name, value);
    if (err != H11_OK) return err;

    /* Track semantic headers */
    int idx = (int)p->request.header_count - 1;
    process_semantic_header(p, name, value, idx);

    return H11_OK;
}
```

---

## Part 5: Header Storage

```c
#define HEADER_INITIAL_CAPACITY 16
#define HEADER_GROWTH_FACTOR 2

h11_error_t h11_add_header(h11_request_t *req, h11_slice_t name, h11_slice_t value) {
    if (req->header_count >= req->header_capacity) {
        size_t new_cap = req->header_capacity == 0 ?
                         HEADER_INITIAL_CAPACITY :
                         req->header_capacity * HEADER_GROWTH_FACTOR;

        h11_header_t *new_hdrs = realloc(req->headers, new_cap * sizeof(h11_header_t));
        if (!new_hdrs) return H11_ERR_INTERNAL;

        req->headers = new_hdrs;
        req->header_capacity = new_cap;
    }

    req->headers[req->header_count].name = name;
    req->headers[req->header_count].value = value;
    req->header_count++;

    return H11_OK;
}
```

---

## Part 6: Semantic Header Tracking

```c
static void process_semantic_header(h11_parser_t *p, h11_slice_t name,
                                     h11_slice_t value, int idx) {
    if (h11_slice_eq_case(name, "Host", 4)) {
        if (p->request.host_index < 0) p->request.host_index = idx;
        p->seen_host = true;
        return;
    }

    if (h11_slice_eq_case(name, "Content-Length", 14)) {
        if (p->request.content_length_index < 0) p->request.content_length_index = idx;
        p->seen_content_length = true;
        return;
    }

    if (h11_slice_eq_case(name, "Transfer-Encoding", 17)) {
        if (p->request.transfer_encoding_index < 0) p->request.transfer_encoding_index = idx;
        p->seen_transfer_encoding = true;
        return;
    }

    if (h11_slice_eq_case(name, "Connection", 10)) {
        if (p->request.connection_index < 0) p->request.connection_index = idx;
        if (contains_token_ci(value, "close", 5)) {
            p->request.keep_alive = false;
        } else if (contains_token_ci(value, "keep-alive", 10)) {
            p->request.keep_alive = true;
        }
        return;
    }

    if (h11_slice_eq_case(name, "Expect", 6)) {
        if (p->request.expect_index < 0) p->request.expect_index = idx;
        if (contains_token_ci(value, "100-continue", 12)) {
            p->request.expect_continue = true;
        }
        return;
    }

    if (h11_slice_eq_case(name, "Upgrade", 7)) {
        p->request.has_upgrade = true;
    }
}
```

---

## Part 7: Edge Cases

### 1. No Colon in Header

```
InvalidHeader\r\n
```
**Error**: `H11_ERR_INVALID_HEADER_NAME`
**Scanner**: `find_char(line, len, ':')` returns -1

### 2. Space Before Colon

```
Header Name: value\r\n
```
**Error**: `H11_ERR_INVALID_HEADER_NAME` (space is not tchar)

### 3. Empty Header Name

```
: value\r\n
```
**Error**: `H11_ERR_INVALID_HEADER_NAME`
**Scanner**: `find_char()` returns 0, which is invalid

### 4. Empty Header Value

```
X-Empty:\r\n
```
**Success**: Valid header with empty value

### 5. Header Value with OWS

```
Content-Type:   text/html   \r\n
```
**Success**: Value is "text/html" (OWS trimmed)

### 6. Control Character in Value

```
X-Header: value\x01here\r\n
```
**Error**: `H11_ERR_INVALID_HEADER_VALUE`

### 7. obs-fold (reject_obs_fold=true)

```
X-Header: value\r\n
 continued\r\n
```
**Error**: `H11_ERR_OBS_FOLD_REJECTED`
**Detection**: Line starts with SP/HTAB

### 8. Too Many Headers

```
Header1: value\r\n
... (exceeds max_header_count)
```
**Error**: `H11_ERR_TOO_MANY_HEADERS`

---

## Part 8: Performance Characteristics

### Scanning Throughput

| Operation | AVX2 | SSE4.2 | Scalar |
|-----------|------|--------|--------|
| find_crlf (per line) | ~1 cycle/byte | ~2 cycles/byte | ~4 cycles/byte |
| find_char (colon) | ~1 cycle/byte | ~2 cycles/byte | ~4 cycles/byte |

### Typical Header Line

```
Content-Type: application/json\r\n
```
- Length: 32 bytes
- AVX2: ~32 cycles for CRLF + ~12 cycles for colon = ~44 cycles
- Scalar: ~128 cycles for CRLF + ~48 cycles for colon = ~176 cycles

---

## Implementation Checklist

- [ ] Implement find_char_avx2()
- [ ] Implement find_char_sse42()
- [ ] Implement find_char_scalar()
- [ ] Implement find_char() dispatcher
- [ ] Implement h11_parse_headers()
- [ ] Implement parse_header_line()
- [ ] Implement h11_add_header() with dynamic growth
- [ ] Implement process_semantic_header()
- [ ] Use find_crlf() for line boundaries
- [ ] Use find_char() for colon detection
- [ ] Validate field-name (tchar only)
- [ ] Validate field-value characters
- [ ] Trim OWS from field-value
- [ ] Handle obs-fold based on config
- [ ] Check max_header_line_len
- [ ] Check max_header_count
- [ ] Check max_headers_size
- [ ] Detect end of headers (empty line)
- [ ] Call finalize_headers()
