# Step 6: Header Semantic Validation

## Purpose

Validate the semantic correctness of key HTTP headers after parsing. This includes Host requirements, Content-Length parsing, Transfer-Encoding parsing, and detecting conflicts.

## RFC References

- **RFC 9110 Section 7.2** - Host and :authority
- **RFC 9110 Section 8.6** - Content-Length
- **RFC 9112 Section 6.1** - Transfer-Encoding
- **RFC 9110 Section 7.6.1** - Connection
- **RFC 9110 Section 10.1.1** - Expect

## Files to Modify

- `parser.c` - Semantic validation functions

---

## Part 1: Header Finalization

Called after all headers are parsed (empty line found):

```c
static h11_error_t finalize_headers(h11_parser_t *p) {
    h11_error_t err;

    /* 1. Validate Host header */
    err = validate_host_header(p);
    if (err != H11_OK) return err;

    /* 2. Validate Content-Length if present */
    if (p->seen_content_length) {
        err = validate_content_length_header(p);
        if (err != H11_OK) return err;
    }

    /* 3. Validate Transfer-Encoding if present */
    if (p->seen_transfer_encoding) {
        err = validate_transfer_encoding_header(p);
        if (err != H11_OK) return err;
    }

    /* 4. Check for TE + CL conflict */
    if (p->seen_transfer_encoding && p->seen_content_length) {
        if (p->config.reject_te_cl_conflict) {
            return H11_ERR_TE_CL_CONFLICT;
        }
        /* Tolerant mode: TE takes precedence, ignore CL */
        p->request.body_type = H11_BODY_CHUNKED;
        p->request.keep_alive = false;  /* Must close after response */
    }

    /* 5. Determine body framing (if not already set by TE) */
    if (p->request.body_type == H11_BODY_NONE) {
        if (p->is_chunked) {
            p->request.body_type = H11_BODY_CHUNKED;
        } else if (p->seen_content_length) {
            p->request.body_type = H11_BODY_CONTENT_LENGTH;
        }
        /* else: no body */
    }

    /* 6. Validate method-target compatibility */
    err = validate_method_target_compatibility(p);
    if (err != H11_OK) return err;

    return H11_OK;
}
```

---

## Part 2: Host Header Validation

### RFC Requirements (RFC 9110 Section 7.2)

- HTTP/1.1 **MUST** send Host in all requests
- Server **MUST** respond 400 if:
  - Host is missing in HTTP/1.1
  - More than one Host header
  - Host value is invalid

### Implementation

```c
static h11_error_t validate_host_header(h11_parser_t *p) {
    /* HTTP/1.0 doesn't require Host */
    if (p->request.version_major == 1 && p->request.version_minor == 0) {
        return H11_OK;
    }

    /* HTTP/1.1 requires exactly one Host header */
    if (!p->seen_host) {
        p->error_offset = p->total_consumed;
        return H11_ERR_MISSING_HOST;
    }

    /* Check for multiple Host headers */
    int host_count = 0;
    for (size_t i = 0; i < p->request.header_count; i++) {
        if (h11_slice_eq_case(p->request.headers[i].name, "Host", 4)) {
            host_count++;
        }
    }

    if (host_count > 1) {
        return H11_ERR_MULTIPLE_HOST;
    }

    /* Validate Host value */
    h11_slice_t host_value = p->request.headers[p->request.host_index].value;
    return h11_validate_host(host_value);
}
```

### Host Value Validation

```c
h11_error_t h11_validate_host(h11_slice_t value) {
    /* Host = uri-host [ ":" port ] */

    if (value.len == 0) {
        /* Empty Host is valid only when target has no authority */
        return H11_OK;
    }

    const char *data = value.data;
    size_t len = value.len;
    size_t pos = 0;

    /* Handle IPv6: [address] */
    if (data[0] == '[') {
        pos++;
        while (pos < len && data[pos] != ']') {
            char c = data[pos];
            if (!H11_IS_HEXDIG(c) && c != ':' && c != '.') {
                return H11_ERR_INVALID_HOST;
            }
            pos++;
        }
        if (pos >= len || data[pos] != ']') {
            return H11_ERR_INVALID_HOST;
        }
        pos++;  /* Skip ] */
    } else {
        /* reg-name or IPv4 */
        while (pos < len && data[pos] != ':') {
            uint8_t c = (uint8_t)data[pos];
            /* Valid: unreserved / sub-delims */
            if (c <= 0x20 || c == 0x7F) {
                return H11_ERR_INVALID_HOST;
            }
            pos++;
        }
    }

    /* Optional port */
    if (pos < len) {
        if (data[pos] != ':') {
            return H11_ERR_INVALID_HOST;
        }
        pos++;  /* Skip : */

        /* Port must be 1*DIGIT */
        if (pos >= len) {
            return H11_ERR_INVALID_HOST;
        }

        uint64_t port = 0;
        while (pos < len) {
            if (!H11_IS_DIGIT(data[pos])) {
                return H11_ERR_INVALID_HOST;
            }
            port = port * 10 + (data[pos] - '0');
            if (port > 65535) {
                return H11_ERR_INVALID_HOST;
            }
            pos++;
        }
    }

    return H11_OK;
}
```

---

## Part 3: Content-Length Validation

### RFC Requirements (RFC 9110 Section 8.6)

- Value is `1*DIGIT` (one or more digits)
- No sign, no decimals
- Multiple values in comma-list must all be identical
- Must detect overflow

### Implementation

```c
static h11_error_t validate_content_length_header(h11_parser_t *p) {
    uint64_t content_length = 0;
    bool first = true;

    /* Check all Content-Length headers */
    for (size_t i = 0; i < p->request.header_count; i++) {
        if (!h11_slice_eq_case(p->request.headers[i].name, "Content-Length", 14)) {
            continue;
        }

        h11_slice_t value = p->request.headers[i].value;
        uint64_t parsed;

        h11_error_t err = parse_content_length_value(value, &parsed);
        if (err != H11_OK) {
            return err;
        }

        if (first) {
            content_length = parsed;
            first = false;
        } else {
            /* Multiple CL headers must have same value */
            if (parsed != content_length) {
                return H11_ERR_MULTIPLE_CONTENT_LENGTH;
            }
        }
    }

    /* Check body size limit */
    if (content_length > p->config.max_body_size) {
        return H11_ERR_BODY_TOO_LARGE;
    }

    p->request.content_length = content_length;
    return H11_OK;
}

static h11_error_t parse_content_length_value(h11_slice_t value, uint64_t *out) {
    *out = 0;

    if (value.len == 0) {
        return H11_ERR_INVALID_CONTENT_LENGTH;
    }

    const char *data = value.data;
    size_t len = value.len;
    size_t pos = 0;

    /* Skip leading OWS (shouldn't be there, but tolerate) */
    while (pos < len && (data[pos] == ' ' || data[pos] == '\t')) {
        pos++;
    }

    if (pos >= len) {
        return H11_ERR_INVALID_CONTENT_LENGTH;
    }

    /* Check for sign (not allowed) */
    if (data[pos] == '+' || data[pos] == '-') {
        return H11_ERR_INVALID_CONTENT_LENGTH;
    }

    /* Parse digits */
    bool has_digits = false;
    uint64_t result = 0;

    while (pos < len && H11_IS_DIGIT(data[pos])) {
        has_digits = true;
        uint64_t digit = data[pos] - '0';

        /* Check for overflow */
        if (result > (UINT64_MAX - digit) / 10) {
            return H11_ERR_CONTENT_LENGTH_OVERFLOW;
        }

        result = result * 10 + digit;
        pos++;
    }

    if (!has_digits) {
        return H11_ERR_INVALID_CONTENT_LENGTH;
    }

    /* Skip trailing OWS */
    while (pos < len && (data[pos] == ' ' || data[pos] == '\t')) {
        pos++;
    }

    /* Check for comma-separated values in single header */
    if (pos < len && data[pos] == ',') {
        /* Parse remaining values, must all be identical */
        while (pos < len) {
            pos++;  /* Skip comma */

            /* Skip OWS */
            while (pos < len && (data[pos] == ' ' || data[pos] == '\t')) {
                pos++;
            }

            /* Parse next value */
            uint64_t next = 0;
            bool next_has_digits = false;

            while (pos < len && H11_IS_DIGIT(data[pos])) {
                next_has_digits = true;
                uint64_t digit = data[pos] - '0';
                if (next > (UINT64_MAX - digit) / 10) {
                    return H11_ERR_CONTENT_LENGTH_OVERFLOW;
                }
                next = next * 10 + digit;
                pos++;
            }

            if (!next_has_digits) {
                return H11_ERR_INVALID_CONTENT_LENGTH;
            }

            if (next != result) {
                return H11_ERR_MULTIPLE_CONTENT_LENGTH;
            }

            /* Skip OWS */
            while (pos < len && (data[pos] == ' ' || data[pos] == '\t')) {
                pos++;
            }

            if (pos < len && data[pos] != ',') {
                return H11_ERR_INVALID_CONTENT_LENGTH;
            }
        }
    }

    /* Should have consumed entire value */
    if (pos != len) {
        return H11_ERR_INVALID_CONTENT_LENGTH;
    }

    *out = result;
    return H11_OK;
}
```

---

## Part 4: Transfer-Encoding Validation

### RFC Requirements (RFC 9112 Section 6.1)

- Comma-separated list of transfer codings
- Final coding **MUST** be `chunked` for requests
- `chunked` has no parameters
- Unknown codings yield 501

### Implementation

```c
static h11_error_t validate_transfer_encoding_header(h11_parser_t *p) {
    /* Collect all Transfer-Encoding headers */
    h11_slice_t last_coding = {0};
    bool found_chunked = false;

    for (size_t i = 0; i < p->request.header_count; i++) {
        if (!h11_slice_eq_case(p->request.headers[i].name, "Transfer-Encoding", 17)) {
            continue;
        }

        h11_slice_t value = p->request.headers[i].value;
        h11_error_t err = parse_transfer_encoding(value, &last_coding, &found_chunked);
        if (err != H11_OK) {
            return err;
        }
    }

    /* For requests, final coding must be chunked */
    if (!h11_slice_eq_case(last_coding, "chunked", 7)) {
        return H11_ERR_TE_NOT_CHUNKED_FINAL;
    }

    p->is_chunked = true;
    return H11_OK;
}

static h11_error_t parse_transfer_encoding(h11_slice_t value,
                                            h11_slice_t *last_coding,
                                            bool *found_chunked) {
    const char *data = value.data;
    size_t len = value.len;
    size_t pos = 0;

    while (pos < len) {
        /* Skip OWS */
        while (pos < len && (data[pos] == ' ' || data[pos] == '\t')) {
            pos++;
        }

        if (pos >= len) break;

        /* Skip comma if present */
        if (data[pos] == ',') {
            pos++;
            continue;
        }

        /* Parse coding token */
        size_t coding_start = pos;
        while (pos < len && H11_IS_TCHAR(data[pos])) {
            pos++;
        }

        if (pos == coding_start) {
            return H11_ERR_INVALID_TRANSFER_ENCODING;
        }

        h11_slice_t coding = {data + coding_start, pos - coding_start};
        *last_coding = coding;

        /* Check for chunked */
        if (h11_slice_eq_case(coding, "chunked", 7)) {
            *found_chunked = true;

            /* chunked must not have parameters */
            while (pos < len && (data[pos] == ' ' || data[pos] == '\t')) {
                pos++;
            }
            if (pos < len && data[pos] == ';') {
                return H11_ERR_INVALID_TRANSFER_ENCODING;
            }
        }
        /* Check for known codings */
        else if (h11_slice_eq_case(coding, "gzip", 4) ||
                 h11_slice_eq_case(coding, "deflate", 7) ||
                 h11_slice_eq_case(coding, "compress", 8) ||
                 h11_slice_eq_case(coding, "identity", 8)) {
            /* Known coding, skip any parameters */
            while (pos < len && data[pos] != ',') {
                pos++;
            }
        }
        else {
            /* Unknown coding */
            return H11_ERR_UNKNOWN_TRANSFER_CODING;
        }
    }

    return H11_OK;
}
```

---

## Part 5: Connection Header Processing

### RFC 9110 Section 7.6.1

```
Connection = #connection-option
connection-option = token
```

### Key Tokens

- `close` - Connection will be closed after response
- `keep-alive` - Connection should be kept open
- Other tokens name hop-by-hop headers

### Implementation

```c
static void process_connection_header(h11_parser_t *p) {
    if (p->request.connection_index < 0) {
        return;
    }

    h11_slice_t value = p->request.headers[p->request.connection_index].value;

    /* Already processed in process_semantic_header for keep-alive/close */

    /* Could also extract hop-by-hop header names for higher layers */
}
```

---

## Part 6: Expect Header

### RFC 9110 Section 10.1.1

```
Expect = #expectation
expectation = token [ "=" ( token / quoted-string ) parameters ]
```

### 100-continue

- Client expects 100 Continue before sending body
- HTTP/1.0 servers should ignore

### Implementation

```c
static h11_error_t validate_expect_header(h11_parser_t *p) {
    if (p->request.expect_index < 0) {
        return H11_OK;
    }

    h11_slice_t value = p->request.headers[p->request.expect_index].value;

    /* Check for 100-continue */
    if (contains_token_ci(value, "100-continue", 12)) {
        /* HTTP/1.0 should ignore 100-continue */
        if (p->request.version_major == 1 && p->request.version_minor >= 1) {
            p->request.expect_continue = true;
        }
    }

    /* Unknown expectations could be rejected with 417 */
    /* For now, just surface expect_continue flag */

    return H11_OK;
}
```

---

## Part 7: Edge Cases

### 1. Missing Host in HTTP/1.1

```
GET / HTTP/1.1\r\n
Content-Length: 0\r\n
\r\n
```
**Error**: `H11_ERR_MISSING_HOST`

### 2. Multiple Host Headers

```
GET / HTTP/1.1\r\n
Host: example.com\r\n
Host: other.com\r\n
\r\n
```
**Error**: `H11_ERR_MULTIPLE_HOST`

### 3. Invalid Host (space)

```
Host: example .com\r\n
```
**Error**: `H11_ERR_INVALID_HOST`

### 4. Content-Length with Sign

```
Content-Length: +100\r\n
```
**Error**: `H11_ERR_INVALID_CONTENT_LENGTH`

### 5. Content-Length Overflow

```
Content-Length: 99999999999999999999\r\n
```
**Error**: `H11_ERR_CONTENT_LENGTH_OVERFLOW`

### 6. Multiple Different Content-Lengths

```
Content-Length: 100\r\n
Content-Length: 200\r\n
```
**Error**: `H11_ERR_MULTIPLE_CONTENT_LENGTH`

### 7. Transfer-Encoding Not Ending with Chunked

```
Transfer-Encoding: gzip\r\n
```
**Error**: `H11_ERR_TE_NOT_CHUNKED_FINAL`

### 8. Chunked with Parameters

```
Transfer-Encoding: chunked;q=1\r\n
```
**Error**: `H11_ERR_INVALID_TRANSFER_ENCODING`

### 9. Unknown Transfer Coding

```
Transfer-Encoding: unknown, chunked\r\n
```
**Error**: `H11_ERR_UNKNOWN_TRANSFER_CODING`

### 10. Both TE and CL (reject_te_cl_conflict=true)

```
Content-Length: 100\r\n
Transfer-Encoding: chunked\r\n
```
**Error**: `H11_ERR_TE_CL_CONFLICT`

---

## Part 8: Validation Summary Table

| Header | Requirement | Error if Invalid |
|--------|-------------|------------------|
| Host | Required for HTTP/1.1 | H11_ERR_MISSING_HOST |
| Host | Exactly one | H11_ERR_MULTIPLE_HOST |
| Host | Valid uri-host[:port] | H11_ERR_INVALID_HOST |
| Content-Length | 1*DIGIT | H11_ERR_INVALID_CONTENT_LENGTH |
| Content-Length | No overflow | H11_ERR_CONTENT_LENGTH_OVERFLOW |
| Content-Length | All values same | H11_ERR_MULTIPLE_CONTENT_LENGTH |
| Transfer-Encoding | Ends with chunked | H11_ERR_TE_NOT_CHUNKED_FINAL |
| Transfer-Encoding | Known codings | H11_ERR_UNKNOWN_TRANSFER_CODING |
| Transfer-Encoding | chunked has no params | H11_ERR_INVALID_TRANSFER_ENCODING |
| TE + CL | Conflict | H11_ERR_TE_CL_CONFLICT |

---

## Implementation Checklist

- [ ] Implement finalize_headers()
- [ ] Implement validate_host_header()
- [ ] Implement h11_validate_host()
- [ ] Implement validate_content_length_header()
- [ ] Implement parse_content_length_value()
- [ ] Implement validate_transfer_encoding_header()
- [ ] Implement parse_transfer_encoding()
- [ ] Handle IPv6 in Host header
- [ ] Handle comma-separated Content-Length
- [ ] Detect Content-Length overflow
- [ ] Check TE+CL conflict
- [ ] Check final coding is chunked
- [ ] Reject chunked with parameters
- [ ] Reject unknown codings
- [ ] Set body_type based on validation
- [ ] Test all edge cases
