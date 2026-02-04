# Step 8: Chunked Transfer Coding

## Purpose

Parse chunked transfer-encoded message bodies according to RFC 9112. Each chunk size line is located using the **SIMD CRLF scanner** - the same core primitive used throughout the parser.

## RFC References

- **RFC 9112 Section 7.1** - Chunked Transfer Coding
- **RFC 9112 Section 7.1.1** - Chunk Extensions
- **RFC 9112 Section 7.1.2** - Chunked Trailer Section
- **RFC 9112 Section 7.1.3** - Decoding Chunked

## Files to Modify

- `parser.c` - Chunked body parsing with integrated SIMD scanning

---

## Part 1: Chunked Message Format

### Grammar (RFC 9112)

```
chunked-body   = *chunk
                 last-chunk
                 trailer-section
                 CRLF

chunk          = chunk-size [ chunk-ext ] CRLF
                 chunk-data CRLF

chunk-size     = 1*HEXDIG
chunk-ext      = *( BWS ";" BWS chunk-ext-name [ BWS "=" BWS chunk-ext-val ] )

last-chunk     = 1*("0") [ chunk-ext ] CRLF
trailer-section = *( field-line CRLF )
```

### Visual Example

```
7\r\n                    ; chunk-size = 7 (found via SIMD CRLF scan)
Mozilla\r\n              ; chunk-data (7 bytes) + CRLF
9\r\n                    ; chunk-size = 9
Developer\r\n            ; chunk-data (9 bytes) + CRLF
0\r\n                    ; last-chunk (size = 0)
\r\n                     ; end of chunked body
```

---

## Part 2: Chunk Size Parsing

### Using the SIMD CRLF Scanner

```c
h11_error_t h11_parse_chunk_size(h11_parser_t *p, const char *data,
                                  size_t len, size_t *consumed) {
    *consumed = 0;

    /*=========================================================================
     * STEP 1: Find line boundary using SIMD CRLF scanner
     *
     * This is the same find_crlf() used for request line and headers.
     * The parser has ONE scanning primitive, used everywhere.
     *========================================================================*/
    ssize_t crlf_pos = find_crlf(data, len);

    if (crlf_pos < 0) {
        /* Chunk size lines should be short - reject if too long */
        if (len > 100) {
            p->error_offset = p->total_consumed;
            return H11_ERR_INVALID_CHUNK_SIZE;
        }
        return H11_NEED_MORE_DATA;
    }

    size_t line_len = (size_t)crlf_pos;

    /*=========================================================================
     * STEP 2: Parse hex digits
     *========================================================================*/
    size_t pos = 0;
    uint64_t chunk_size = 0;
    bool has_digits = false;

    while (pos < line_len && H11_IS_HEXDIG(data[pos])) {
        has_digits = true;
        int digit = h11_hexval(data[pos]);

        /* Overflow check */
        if (chunk_size > (UINT64_MAX - digit) / 16) {
            p->error_offset = p->total_consumed + pos;
            return H11_ERR_CHUNK_SIZE_OVERFLOW;
        }

        chunk_size = chunk_size * 16 + digit;
        pos++;
    }

    if (!has_digits) {
        p->error_offset = p->total_consumed;
        return H11_ERR_INVALID_CHUNK_SIZE;
    }

    /*=========================================================================
     * STEP 3: Parse chunk extensions (optional)
     *========================================================================*/
    if (pos < line_len) {
        h11_error_t err = parse_chunk_extensions(p, data + pos, line_len - pos,
                                                  p->total_consumed + pos);
        if (err != H11_OK) return err;
    }

    /*=========================================================================
     * STEP 4: Check body size limit
     *========================================================================*/
    if (p->config.max_body_size != SIZE_MAX) {
        if (p->total_body_read + chunk_size > p->config.max_body_size) {
            p->error_offset = p->total_consumed;
            return H11_ERR_BODY_TOO_LARGE;
        }
    }

    /*=========================================================================
     * STEP 5: Transition state
     *========================================================================*/
    *consumed = line_len + 2;  /* Line + CRLF */

    if (chunk_size == 0) {
        /* Last chunk - proceed to trailers */
        p->state = H11_STATE_TRAILERS;
    } else {
        /* Regular chunk - proceed to data */
        p->state = H11_STATE_BODY_CHUNKED_DATA;
        p->body_remaining = chunk_size;
    }

    return H11_OK;
}
```

---

## Part 3: Chunk Extension Parsing

```c
static h11_error_t parse_chunk_extensions(h11_parser_t *p, const char *data,
                                           size_t len, size_t base_offset) {
    size_t pos = 0;
    size_t ext_len = 0;

    while (pos < len) {
        /* Skip BWS */
        while (pos < len && (data[pos] == ' ' || data[pos] == '\t')) {
            pos++; ext_len++;
        }

        if (pos >= len) break;

        /* Expect semicolon */
        if (data[pos] != ';') {
            p->error_offset = base_offset + pos;
            return H11_ERR_INVALID_CHUNK_EXT;
        }
        pos++; ext_len++;

        /* Skip BWS */
        while (pos < len && (data[pos] == ' ' || data[pos] == '\t')) {
            pos++; ext_len++;
        }

        /* Extension name (token) */
        size_t name_start = pos;
        while (pos < len && H11_IS_TCHAR(data[pos])) {
            pos++; ext_len++;
        }

        if (pos == name_start) {
            p->error_offset = base_offset + pos;
            return H11_ERR_INVALID_CHUNK_EXT;
        }

        /* Skip BWS */
        while (pos < len && (data[pos] == ' ' || data[pos] == '\t')) {
            pos++; ext_len++;
        }

        /* Optional: = value */
        if (pos < len && data[pos] == '=') {
            pos++; ext_len++;

            /* Skip BWS */
            while (pos < len && (data[pos] == ' ' || data[pos] == '\t')) {
                pos++; ext_len++;
            }

            if (pos >= len) {
                p->error_offset = base_offset + pos;
                return H11_ERR_INVALID_CHUNK_EXT;
            }

            /* Value: token or quoted-string */
            if (data[pos] == '"') {
                pos++; ext_len++;
                while (pos < len && data[pos] != '"') {
                    if (data[pos] == '\\' && pos + 1 < len) {
                        pos += 2; ext_len += 2;
                    } else {
                        pos++; ext_len++;
                    }
                }
                if (pos >= len || data[pos] != '"') {
                    p->error_offset = base_offset + pos;
                    return H11_ERR_INVALID_CHUNK_EXT;
                }
                pos++; ext_len++;
            } else {
                while (pos < len && H11_IS_TCHAR(data[pos])) {
                    pos++; ext_len++;
                }
            }
        }

        /* Check extension length limit */
        if (ext_len > p->config.max_chunk_ext_len) {
            p->error_offset = base_offset;
            return H11_ERR_CHUNK_EXT_TOO_LONG;
        }
    }

    return H11_OK;
}
```

---

## Part 4: Chunk Data Reading

```c
static h11_error_t read_chunked_body(h11_parser_t *p, const char *data, size_t len,
                                      size_t *consumed, const char **body_out,
                                      size_t *body_len) {
    /* Read up to body_remaining bytes from current chunk */
    size_t to_read = len;
    if (to_read > p->body_remaining) {
        to_read = (size_t)p->body_remaining;
    }

    *body_out = data;
    *body_len = to_read;
    *consumed = to_read;

    p->body_remaining -= to_read;
    p->total_body_read += to_read;

    /* Chunk exhausted? Move to CRLF state */
    if (p->body_remaining == 0) {
        p->state = H11_STATE_BODY_CHUNKED_CRLF;
    }

    return H11_OK;
}
```

---

## Part 5: Chunk CRLF Handling

```c
/* In main parse loop, H11_STATE_BODY_CHUNKED_CRLF case */

case H11_STATE_BODY_CHUNKED_CRLF:
    /* Expect CRLF after chunk data */
    if (remaining < 2) {
        return H11_NEED_MORE_DATA;
    }

    if (buf[0] != '\r' || buf[1] != '\n') {
        p->state = H11_STATE_ERROR;
        p->last_error = H11_ERR_INVALID_CHUNK_DATA;
        p->error_offset = p->total_consumed + *consumed;
        return H11_ERR_INVALID_CHUNK_DATA;
    }

    *consumed += 2;
    p->state = H11_STATE_BODY_CHUNKED_SIZE;  /* Back to size parsing */
    break;
```

---

## Part 6: Trailer Parsing (Reuses Header Scanning)

```c
h11_error_t h11_parse_trailers(h11_parser_t *p, const char *data,
                                size_t len, size_t *consumed) {
    *consumed = 0;

    while (*consumed < len) {
        const char *line = data + *consumed;
        size_t remaining = len - *consumed;

        /*=====================================================================
         * Find line using SIMD CRLF scanner (same as headers)
         *====================================================================*/
        ssize_t crlf_pos = find_crlf(line, remaining);

        if (crlf_pos < 0) {
            return H11_NEED_MORE_DATA;
        }

        size_t line_len = (size_t)crlf_pos;

        /* Empty line ends trailers */
        if (line_len == 0) {
            *consumed += 2;
            p->state = H11_STATE_COMPLETE;
            return H11_OK;
        }

        /* Parse trailer field (reuse header parsing logic) */
        h11_error_t err = parse_trailer_line(p, line, line_len,
                                              p->total_consumed + *consumed);
        if (err != H11_OK) return err;

        *consumed += line_len + 2;
    }

    return H11_NEED_MORE_DATA;
}

static h11_error_t parse_trailer_line(h11_parser_t *p, const char *line,
                                        size_t len, size_t base_offset) {
    /*=========================================================================
     * Find colon using SIMD character scanner (same as headers)
     *========================================================================*/
    ssize_t colon_pos = find_char(line, len, ':');

    if (colon_pos <= 0) {
        p->error_offset = base_offset;
        return H11_ERR_INVALID_TRAILER;
    }

    /* Validate name */
    for (size_t i = 0; i < (size_t)colon_pos; i++) {
        if (!H11_IS_TCHAR(line[i])) {
            p->error_offset = base_offset + i;
            return H11_ERR_INVALID_TRAILER;
        }
    }

    h11_slice_t name = {line, (size_t)colon_pos};

    /* Parse value */
    size_t pos = colon_pos + 1;
    while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) pos++;

    size_t value_start = pos;
    size_t value_end = len;

    while (value_end > value_start &&
           (line[value_end-1] == ' ' || line[value_end-1] == '\t')) {
        value_end--;
    }

    h11_slice_t value = {line + value_start, value_end - value_start};

    return h11_add_trailer(&p->request, name, value);
}
```

---

## Part 7: Trailer Storage

```c
h11_error_t h11_add_trailer(h11_request_t *req, h11_slice_t name, h11_slice_t value) {
    if (req->trailer_count >= req->trailer_capacity) {
        size_t new_cap = req->trailer_capacity == 0 ? 8 : req->trailer_capacity * 2;

        h11_header_t *new_trailers = realloc(req->trailers,
                                              new_cap * sizeof(h11_header_t));
        if (!new_trailers) return H11_ERR_INTERNAL;

        req->trailers = new_trailers;
        req->trailer_capacity = new_cap;
    }

    req->trailers[req->trailer_count].name = name;
    req->trailers[req->trailer_count].value = value;
    req->trailer_count++;

    return H11_OK;
}
```

---

## Part 8: Chunked State Flow

```
          ┌───────────────────┐
          │ BODY_CHUNKED_SIZE │ ◄── find_crlf() locates size line
          └─────────┬─────────┘
                    │
            Parse hex size
                    │
           ┌────────┴────────┐
           │                 │
           ▼                 ▼
       size > 0          size == 0
           │                 │
           ▼                 ▼
   ┌───────────────┐  ┌─────────────┐
   │BODY_CHUNKED_  │  │  TRAILERS   │ ◄── find_crlf() for each trailer
   │    DATA       │  └──────┬──────┘
   └───────┬───────┘         │
           │                 ▼
      Read bytes       Empty line?
           │                 │
           ▼            ┌────┴────┐
   ┌───────────────┐    │         │
   │BODY_CHUNKED_  │   No        Yes
   │    CRLF       │    │         │
   └───────┬───────┘    │         ▼
           │            │  ┌──────────┐
       Expect \r\n      │  │ COMPLETE │
           │            │  └──────────┘
           └────────────┘
               │
               ▼
      Back to CHUNKED_SIZE
```

---

## Part 9: Edge Cases

### 1. Empty Chunk (Size 0 Only)

```
0\r\n
\r\n
```
**Result**: No body data, empty trailers, COMPLETE
**Scanner**: `find_crlf()` finds line at offset 0, size parses as 0

### 2. Chunk with Extensions

```
A;ext=value\r\n
0123456789\r\n
0\r\n
\r\n
```
**Result**: 10 bytes of data, extensions ignored

### 3. Chunk Size with Leading Zeros

```
00A\r\n
```
**Result**: Valid, size = 10

### 4. Chunk Size Overflow

```
FFFFFFFFFFFFFFFF1\r\n
```
**Error**: `H11_ERR_CHUNK_SIZE_OVERFLOW`

### 5. Missing CRLF After Chunk Data

```
5\r\n
hello
0\r\n
```
**Error**: `H11_ERR_INVALID_CHUNK_DATA`

### 6. Non-Hex in Chunk Size

```
5G\r\n
```
**Error**: `H11_ERR_INVALID_CHUNK_SIZE` (unless G starts extension)

### 7. Chunk Extension Too Long

```
5;ext=aaaa...aaa\r\n (>1024 chars)
```
**Error**: `H11_ERR_CHUNK_EXT_TOO_LONG`

---

## Part 10: Unified Scanning Architecture

The chunked parser uses the **same SIMD primitives** as the rest of the parser:

| Operation | Scanner | SIMD Levels |
|-----------|---------|-------------|
| Find chunk size line end | `find_crlf()` | AVX-512/AVX2/SSE4.2/Scalar |
| Find trailer line end | `find_crlf()` | AVX-512/AVX2/SSE4.2/Scalar |
| Find colon in trailer | `find_char()` | AVX2/SSE4.2/Scalar |

There is **one parser with one scanning core**. The chunked parser is not a separate subsystem - it's the same vectorized scanning applied to chunk-formatted data.

---

## Implementation Checklist

- [ ] Implement h11_parse_chunk_size()
- [ ] Use find_crlf() for chunk size line boundary
- [ ] Parse hex digits with overflow check
- [ ] Implement parse_chunk_extensions()
- [ ] Validate extension syntax
- [ ] Enforce max_chunk_ext_len
- [ ] Implement read_chunked_body()
- [ ] Handle BODY_CHUNKED_CRLF state
- [ ] Implement h11_parse_trailers()
- [ ] Use find_crlf() for trailer line boundary
- [ ] Use find_char() for trailer colon
- [ ] Implement h11_add_trailer()
- [ ] Check max_body_size across chunks
- [ ] Handle last chunk (size 0)
- [ ] Transition to COMPLETE after trailers
