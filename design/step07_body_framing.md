# Step 7: Body Framing Determination

## Purpose

Determine how the message body is framed based on headers and HTTP version. Requests use Content-Length or Transfer-Encoding; they are never close-delimited.

## RFC References

- **RFC 9112 Section 6** - Message Body
- **RFC 9112 Section 6.1** - Transfer-Encoding
- **RFC 9112 Section 6.2** - Content-Length
- **RFC 9112 Section 6.3** - Message Body Length

## Files to Modify

- `parser.c` - Body framing logic in finalize_headers()

---

## Part 1: Body Framing Rules

### RFC 9112 Section 6.3 - Message Body Length

For **requests**, the message body length is determined by:

1. **Transfer-Encoding present** → Body is chunked encoded (final coding must be chunked)
2. **Content-Length present** → Body is exactly that many octets
3. **Neither present** → No body (message ends after headers)

### Key Points

- Requests are **never** close-delimited
- Transfer-Encoding **overrides** Content-Length if both present
- If TE + CL conflict: either reject or ignore CL

---

## Part 2: Body Type Enum

```c
typedef enum h11_body_type {
    H11_BODY_NONE,              /* No body */
    H11_BODY_CONTENT_LENGTH,    /* Fixed length via Content-Length */
    H11_BODY_CHUNKED            /* Chunked transfer coding */
} h11_body_type_t;
```

---

## Part 3: Framing Decision Logic

```c
static h11_error_t determine_body_framing(h11_parser_t *p) {
    /* Already set if TE was validated */
    if (p->request.body_type != H11_BODY_NONE) {
        return H11_OK;
    }

    /* Priority 1: Transfer-Encoding (always overrides CL) */
    if (p->seen_transfer_encoding) {
        /* Validation already ensured final coding is chunked */
        p->request.body_type = H11_BODY_CHUNKED;

        /* If CL also present, that's a conflict */
        if (p->seen_content_length) {
            if (p->config.reject_te_cl_conflict) {
                return H11_ERR_TE_CL_CONFLICT;
            }
            /* Tolerant: use chunked, mark for connection close */
            p->request.keep_alive = false;
        }

        return H11_OK;
    }

    /* Priority 2: Content-Length */
    if (p->seen_content_length) {
        p->request.body_type = H11_BODY_CONTENT_LENGTH;
        /* content_length already parsed and stored */
        return H11_OK;
    }

    /* Priority 3: No body */
    p->request.body_type = H11_BODY_NONE;
    return H11_OK;
}
```

---

## Part 4: State Transitions After Headers

```c
/* In finalize_headers(), after determine_body_framing(): */

switch (p->request.body_type) {
case H11_BODY_NONE:
    /* No body - request is complete */
    p->state = H11_STATE_COMPLETE;
    break;

case H11_BODY_CONTENT_LENGTH:
    /* Fixed-length body */
    if (p->request.content_length == 0) {
        /* Zero-length body is effectively no body */
        p->state = H11_STATE_COMPLETE;
    } else {
        p->state = H11_STATE_BODY_IDENTITY;
        p->body_remaining = p->request.content_length;
    }
    break;

case H11_BODY_CHUNKED:
    /* Chunked body - start reading chunk sizes */
    p->state = H11_STATE_BODY_CHUNKED_SIZE;
    break;
}
```

---

## Part 5: Identity Body Reading

For Content-Length bodies, reading is straightforward:

```c
h11_error_t h11_read_body(h11_parser_t *parser, const char *data, size_t len,
                          size_t *consumed, const char **body_out, size_t *body_len) {
    if (!parser || !consumed || !body_out || !body_len) {
        return H11_ERR_INTERNAL;
    }

    *consumed = 0;
    *body_out = NULL;
    *body_len = 0;

    if (parser->state != H11_STATE_BODY_IDENTITY &&
        parser->state != H11_STATE_BODY_CHUNKED_DATA) {
        return H11_ERR_INTERNAL;  /* Not in body reading state */
    }

    if (parser->state == H11_STATE_BODY_IDENTITY) {
        return read_identity_body(parser, data, len, consumed, body_out, body_len);
    } else {
        return read_chunked_body(parser, data, len, consumed, body_out, body_len);
    }
}

static h11_error_t read_identity_body(h11_parser_t *p, const char *data, size_t len,
                                       size_t *consumed, const char **body_out,
                                       size_t *body_len) {
    /* Read up to body_remaining bytes */
    size_t to_read = len;
    if (to_read > p->body_remaining) {
        to_read = (size_t)p->body_remaining;
    }

    /* Return pointer to body data */
    *body_out = data;
    *body_len = to_read;
    *consumed = to_read;

    /* Update state */
    p->body_remaining -= to_read;
    p->total_body_read += to_read;

    /* Check if body complete */
    if (p->body_remaining == 0) {
        p->state = H11_STATE_COMPLETE;
    }

    return H11_OK;
}
```

---

## Part 6: Body Reading Flow

### For Content-Length Body

```
Application                         Parser
    |                                  |
    |  h11_parse(data) ---------------→|
    |                                  |  Parse request line + headers
    |  ←--- H11_OK (state=BODY_IDENTITY)
    |                                  |
    |  h11_read_body(data) -----------→|
    |                                  |  Return body chunk
    |  ←--- H11_OK, body_out, body_len |
    |                                  |
    |  (repeat until state=COMPLETE)   |
    |                                  |
    |  h11_get_state() ---------------→|
    |  ←--- H11_STATE_COMPLETE         |
```

### For No Body

```
Application                         Parser
    |                                  |
    |  h11_parse(data) ---------------→|
    |                                  |  Parse request line + headers
    |  ←--- H11_OK (state=COMPLETE)    |
    |                                  |
    |  (no body to read)               |
```

---

## Part 7: Edge Cases

### 1. Content-Length: 0

```
POST / HTTP/1.1\r\n
Host: example.com\r\n
Content-Length: 0\r\n
\r\n
```
**Result**: `body_type=H11_BODY_CONTENT_LENGTH`, `content_length=0`, immediate COMPLETE

### 2. GET with No Body Headers

```
GET / HTTP/1.1\r\n
Host: example.com\r\n
\r\n
```
**Result**: `body_type=H11_BODY_NONE`, immediate COMPLETE

### 3. POST with No Body Headers

```
POST / HTTP/1.1\r\n
Host: example.com\r\n
\r\n
```
**Result**: `body_type=H11_BODY_NONE`, immediate COMPLETE
(This is valid - POST without body is allowed, server may reject semantically)

### 4. TE + CL (reject mode)

```
POST / HTTP/1.1\r\n
Host: example.com\r\n
Content-Length: 100\r\n
Transfer-Encoding: chunked\r\n
\r\n
```
**Error**: `H11_ERR_TE_CL_CONFLICT`

### 5. TE + CL (tolerant mode)

Same request with `reject_te_cl_conflict=false`:
**Result**: Use chunked, set `keep_alive=false`

### 6. Transfer-Encoding Without Chunked Final

```
POST / HTTP/1.1\r\n
Host: example.com\r\n
Transfer-Encoding: gzip\r\n
\r\n
```
**Error**: `H11_ERR_TE_NOT_CHUNKED_FINAL`

### 7. Body Larger Than Limit

During body reading, if `total_body_read + len > max_body_size`:
**Error**: `H11_ERR_BODY_TOO_LARGE`

---

## Part 8: Body Size Enforcement

```c
static h11_error_t read_identity_body(h11_parser_t *p, const char *data, size_t len,
                                       size_t *consumed, const char **body_out,
                                       size_t *body_len) {
    size_t to_read = len;
    if (to_read > p->body_remaining) {
        to_read = (size_t)p->body_remaining;
    }

    /* Check max body size */
    if (p->config.max_body_size != SIZE_MAX) {
        if (p->total_body_read + to_read > p->config.max_body_size) {
            p->state = H11_STATE_ERROR;
            p->last_error = H11_ERR_BODY_TOO_LARGE;
            return H11_ERR_BODY_TOO_LARGE;
        }
    }

    *body_out = data;
    *body_len = to_read;
    *consumed = to_read;

    p->body_remaining -= to_read;
    p->total_body_read += to_read;

    if (p->body_remaining == 0) {
        p->state = H11_STATE_COMPLETE;
    }

    return H11_OK;
}
```

---

## Part 9: Configuration Options

| Option | Effect |
|--------|--------|
| `max_body_size` | Maximum allowed body size (default: unlimited) |
| `reject_te_cl_conflict` | Error on TE+CL (default: true) |

---

## Part 10: Decision Tree

```
Has Transfer-Encoding?
├── Yes
│   └── Final coding is chunked?
│       ├── Yes
│       │   └── Has Content-Length?
│       │       ├── Yes
│       │       │   └── reject_te_cl_conflict?
│       │       │       ├── Yes → ERROR: H11_ERR_TE_CL_CONFLICT
│       │       │       └── No  → Use chunked, keep_alive=false
│       │       └── No → Use chunked
│       └── No → ERROR: H11_ERR_TE_NOT_CHUNKED_FINAL
└── No
    └── Has Content-Length?
        ├── Yes
        │   └── content_length > max_body_size?
        │       ├── Yes → ERROR: H11_ERR_BODY_TOO_LARGE
        │       └── No  → Use identity body
        └── No → No body
```

---

## Implementation Checklist

- [ ] Implement determine_body_framing()
- [ ] Handle TE overriding CL
- [ ] Handle TE+CL conflict based on config
- [ ] Set state to BODY_IDENTITY for CL
- [ ] Set state to BODY_CHUNKED_SIZE for TE
- [ ] Set state to COMPLETE for no body
- [ ] Handle Content-Length: 0
- [ ] Implement read_identity_body()
- [ ] Enforce max_body_size during reads
- [ ] Track body_remaining and total_body_read
- [ ] Transition to COMPLETE when body done
- [ ] Test all edge cases
