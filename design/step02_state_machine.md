# Step 2: Parser State Machine

## Purpose

Implement the core state machine that drives incremental parsing of HTTP/1.1 requests. The parser must handle partial data, maintain state between calls, and support pipelining.

## RFC References

- **RFC 9112 Section 2** - Message Format
- **RFC 9112 Section 2.1** - Message Framing
- **RFC 9112 Section 2.2** - Message Parsing

## Files to Modify

- `parser.c` - State machine implementation, parser lifecycle functions

---

## Part 1: Parser Context Structure (h11_internal.h)

```c
struct h11_parser {
    /*=== Configuration ===*/
    h11_config_t config;

    /*=== State Machine ===*/
    h11_state_t state;
    h11_error_t last_error;
    size_t error_offset;         /* Byte offset where error occurred */

    /*=== Current Request ===*/
    h11_request_t request;

    /*=== Position Tracking ===*/
    size_t total_consumed;       /* Total bytes consumed for this request */
    size_t line_start;           /* Offset of current line start */
    size_t headers_size;         /* Accumulated header section bytes */

    /*=== Body Parsing ===*/
    uint64_t body_remaining;     /* Bytes remaining in body/chunk */
    uint64_t total_body_read;    /* Total body bytes delivered */

    /*=== Chunk State ===*/
    bool in_chunk_ext;           /* Currently in chunk extension */
    size_t chunk_ext_len;        /* Length of current chunk extension */

    /*=== Semantic Flags ===*/
    bool seen_host;
    bool seen_content_length;
    bool seen_transfer_encoding;
    bool is_chunked;
    bool leading_crlf_consumed;
};
```

---

## Part 2: Parser Lifecycle Functions (parser.c)

### Constructor

```c
h11_parser_t *h11_parser_new(const h11_config_t *config) {
    h11_parser_t *p = calloc(1, sizeof(h11_parser_t));
    if (!p) return NULL;

    /* Apply configuration (use defaults if NULL) */
    if (config) {
        p->config = *config;
    } else {
        p->config = h11_config_default();
    }

    /* Initialize state */
    p->state = H11_STATE_IDLE;
    p->last_error = H11_OK;

    /* Initialize request structure */
    p->request.headers = NULL;
    p->request.header_count = 0;
    p->request.header_capacity = 0;
    p->request.trailers = NULL;
    p->request.trailer_count = 0;
    p->request.trailer_capacity = 0;

    /* Initialize header indices to "not found" */
    p->request.host_index = -1;
    p->request.content_length_index = -1;
    p->request.transfer_encoding_index = -1;
    p->request.connection_index = -1;
    p->request.expect_index = -1;

    return p;
}
```

### Destructor

```c
void h11_parser_free(h11_parser_t *parser) {
    if (!parser) return;

    /* Free dynamic allocations */
    free(parser->request.headers);
    free(parser->request.trailers);
    free(parser);
}
```

### Reset (for pipelining)

```c
void h11_parser_reset(h11_parser_t *parser) {
    if (!parser) return;

    /* Reset state machine */
    parser->state = H11_STATE_IDLE;
    parser->last_error = H11_OK;
    parser->error_offset = 0;

    /* Reset position tracking */
    parser->total_consumed = 0;
    parser->line_start = 0;
    parser->headers_size = 0;

    /* Reset body state */
    parser->body_remaining = 0;
    parser->total_body_read = 0;
    parser->in_chunk_ext = false;
    parser->chunk_ext_len = 0;

    /* Reset semantic flags */
    parser->seen_host = false;
    parser->seen_content_length = false;
    parser->seen_transfer_encoding = false;
    parser->is_chunked = false;
    parser->leading_crlf_consumed = false;

    /* Reset request (keep allocated arrays, just reset counts) */
    parser->request.method = (h11_slice_t){0};
    parser->request.target = (h11_slice_t){0};
    parser->request.header_count = 0;
    parser->request.trailer_count = 0;
    parser->request.host_index = -1;
    parser->request.content_length_index = -1;
    parser->request.transfer_encoding_index = -1;
    parser->request.connection_index = -1;
    parser->request.expect_index = -1;
    parser->request.body_type = H11_BODY_NONE;
    parser->request.content_length = 0;
    parser->request.keep_alive = true;  /* HTTP/1.1 default */
    parser->request.expect_continue = false;
    parser->request.has_upgrade = false;
}
```

---

## Part 3: Main Parse Function (parser.c)

```c
h11_error_t h11_parse(h11_parser_t *parser, const char *data, size_t len,
                      size_t *consumed) {
    if (!parser || !data || !consumed) {
        return H11_ERR_INTERNAL;
    }

    *consumed = 0;

    /* Cannot parse if already in error state */
    if (parser->state == H11_STATE_ERROR) {
        return parser->last_error;
    }

    h11_error_t err;
    size_t bytes_consumed;

    while (*consumed < len) {
        const char *buf = data + *consumed;
        size_t remaining = len - *consumed;
        bytes_consumed = 0;

        switch (parser->state) {
        case H11_STATE_IDLE:
            /* Handle leading CRLF (RFC 9112 Section 2.2) */
            if (parser->config.allow_leading_crlf && !parser->leading_crlf_consumed) {
                if (remaining >= 2 && buf[0] == '\r' && buf[1] == '\n') {
                    *consumed += 2;
                    continue;
                }
                if (remaining >= 1 && buf[0] == '\n' && !parser->config.strict_crlf) {
                    *consumed += 1;
                    continue;
                }
            }
            parser->leading_crlf_consumed = true;
            parser->state = H11_STATE_REQUEST_LINE;
            /* Fall through to request line parsing */

        case H11_STATE_REQUEST_LINE:
            err = h11_parse_request_line(parser, buf, remaining, &bytes_consumed);
            if (err == H11_NEED_MORE_DATA) {
                return H11_NEED_MORE_DATA;
            }
            if (err != H11_OK) {
                parser->state = H11_STATE_ERROR;
                parser->last_error = err;
                return err;
            }
            *consumed += bytes_consumed;
            parser->state = H11_STATE_HEADERS;
            break;

        case H11_STATE_HEADERS:
            err = h11_parse_headers(parser, buf, remaining, &bytes_consumed);
            if (err == H11_NEED_MORE_DATA) {
                return H11_NEED_MORE_DATA;
            }
            if (err != H11_OK) {
                parser->state = H11_STATE_ERROR;
                parser->last_error = err;
                return err;
            }
            *consumed += bytes_consumed;
            /* h11_parse_headers sets next state based on body type */
            break;

        case H11_STATE_BODY_IDENTITY:
            /* Identity body reading handled by h11_read_body */
            return H11_OK;

        case H11_STATE_BODY_CHUNKED_SIZE:
            err = h11_parse_chunk_size(parser, buf, remaining, &bytes_consumed);
            if (err == H11_NEED_MORE_DATA) {
                return H11_NEED_MORE_DATA;
            }
            if (err != H11_OK) {
                parser->state = H11_STATE_ERROR;
                parser->last_error = err;
                return err;
            }
            *consumed += bytes_consumed;
            /* State transition handled in parse_chunk_size */
            break;

        case H11_STATE_BODY_CHUNKED_DATA:
            /* Chunk data reading handled by h11_read_body */
            return H11_OK;

        case H11_STATE_BODY_CHUNKED_CRLF:
            /* Expect CRLF after chunk data */
            if (remaining < 2) {
                return H11_NEED_MORE_DATA;
            }
            if (buf[0] != '\r' || buf[1] != '\n') {
                parser->state = H11_STATE_ERROR;
                parser->last_error = H11_ERR_INVALID_CHUNK_DATA;
                parser->error_offset = parser->total_consumed + *consumed;
                return H11_ERR_INVALID_CHUNK_DATA;
            }
            *consumed += 2;
            parser->state = H11_STATE_BODY_CHUNKED_SIZE;
            break;

        case H11_STATE_TRAILERS:
            err = h11_parse_trailers(parser, buf, remaining, &bytes_consumed);
            if (err == H11_NEED_MORE_DATA) {
                return H11_NEED_MORE_DATA;
            }
            if (err != H11_OK) {
                parser->state = H11_STATE_ERROR;
                parser->last_error = err;
                return err;
            }
            *consumed += bytes_consumed;
            /* State transition to COMPLETE handled in parse_trailers */
            break;

        case H11_STATE_COMPLETE:
            /* Request complete, return to caller */
            return H11_OK;

        case H11_STATE_ERROR:
            return parser->last_error;
        }
    }

    return H11_OK;
}
```

---

## Part 4: State Inspection Functions (parser.c)

```c
h11_state_t h11_get_state(const h11_parser_t *parser) {
    return parser ? parser->state : H11_STATE_ERROR;
}

const h11_request_t *h11_get_request(const h11_parser_t *parser) {
    return parser ? &parser->request : NULL;
}

size_t h11_error_offset(const h11_parser_t *parser) {
    return parser ? parser->error_offset : 0;
}
```

---

## Part 5: State Transition Rules

### Valid Transitions

| From State | To State | Condition |
|------------|----------|-----------|
| IDLE | REQUEST_LINE | Data available |
| REQUEST_LINE | HEADERS | Request line complete |
| HEADERS | COMPLETE | No body (no CL, no TE) |
| HEADERS | BODY_IDENTITY | Content-Length present |
| HEADERS | BODY_CHUNKED_SIZE | Transfer-Encoding: chunked |
| BODY_IDENTITY | COMPLETE | All bytes consumed |
| BODY_CHUNKED_SIZE | BODY_CHUNKED_DATA | Chunk size > 0 |
| BODY_CHUNKED_SIZE | TRAILERS | Chunk size = 0 (last chunk) |
| BODY_CHUNKED_DATA | BODY_CHUNKED_CRLF | All chunk bytes consumed |
| BODY_CHUNKED_CRLF | BODY_CHUNKED_SIZE | CRLF consumed |
| TRAILERS | COMPLETE | Empty line parsed |
| Any | ERROR | Parse error |
| COMPLETE | IDLE | h11_parser_reset() called |

### Invalid Transitions

- ERROR -> Any (must reset first)
- COMPLETE -> Any (must reset first)
- Backward transitions (except via reset)

---

## Part 6: Error Handling Patterns

### Setting Error State

```c
static h11_error_t set_error(h11_parser_t *p, h11_error_t err, size_t offset) {
    p->state = H11_STATE_ERROR;
    p->last_error = err;
    p->error_offset = offset;
    return err;
}
```

### Error Recovery

The parser does not support error recovery. Once in ERROR state:
1. `h11_parse()` returns the stored error
2. `h11_get_state()` returns H11_STATE_ERROR
3. The caller should close the connection
4. `h11_parser_reset()` can be called to reuse the parser

---

## Part 7: Pipelining Support

HTTP pipelining allows multiple requests on one connection:

```
Request 1 | Request 2 | Request 3
[parsed]  | [buffered]| [buffered]
```

### Pipelining Workflow

```c
while (has_data) {
    size_t consumed;
    h11_error_t err = h11_parse(parser, buffer, len, &consumed);

    if (err == H11_NEED_MORE_DATA) {
        /* Wait for more data */
        break;
    }

    if (err != H11_OK) {
        /* Error, close connection */
        break;
    }

    if (h11_get_state(parser) == H11_STATE_COMPLETE) {
        /* Process complete request */
        handle_request(h11_get_request(parser));

        /* Reset for next request */
        h11_parser_reset(parser);

        /* Advance buffer past consumed bytes */
        buffer += consumed;
        len -= consumed;
    }
}
```

---

## Part 8: Streaming Body API

### Body Reading Function

```c
h11_error_t h11_read_body(h11_parser_t *parser, const char *data, size_t len,
                          size_t *consumed, const char **body_out, size_t *body_len) {
    if (!parser || !consumed || !body_out || !body_len) {
        return H11_ERR_INTERNAL;
    }

    *consumed = 0;
    *body_out = NULL;
    *body_len = 0;

    switch (parser->state) {
    case H11_STATE_BODY_IDENTITY: {
        /* Read up to body_remaining bytes */
        size_t to_read = len;
        if (to_read > parser->body_remaining) {
            to_read = (size_t)parser->body_remaining;
        }

        *body_out = data;
        *body_len = to_read;
        *consumed = to_read;

        parser->body_remaining -= to_read;
        parser->total_body_read += to_read;

        if (parser->body_remaining == 0) {
            parser->state = H11_STATE_COMPLETE;
        }
        return H11_OK;
    }

    case H11_STATE_BODY_CHUNKED_DATA: {
        /* Read up to body_remaining bytes (current chunk) */
        size_t to_read = len;
        if (to_read > parser->body_remaining) {
            to_read = (size_t)parser->body_remaining;
        }

        *body_out = data;
        *body_len = to_read;
        *consumed = to_read;

        parser->body_remaining -= to_read;
        parser->total_body_read += to_read;

        if (parser->body_remaining == 0) {
            parser->state = H11_STATE_BODY_CHUNKED_CRLF;
        }
        return H11_OK;
    }

    default:
        return H11_ERR_INTERNAL;
    }
}
```

---

## Edge Cases

### 1. Empty Input
```c
h11_parse(parser, "", 0, &consumed);
/* Returns H11_NEED_MORE_DATA, consumed=0 */
```

### 2. Partial Request Line
```c
h11_parse(parser, "GET /path", 9, &consumed);
/* Returns H11_NEED_MORE_DATA, consumed=0 */
```

### 3. Multiple Requests in Buffer
```c
const char *buf = "GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /2 HTTP/1.1\r\n...";
/* First parse returns with consumed pointing to start of second request */
```

### 4. Interleaved Body Reads
```c
/* Parse headers, then alternately call h11_parse and h11_read_body */
/* State machine handles transitions correctly */
```

---

## Implementation Checklist

- [ ] Implement h11_parser_new()
- [ ] Implement h11_parser_free()
- [ ] Implement h11_parser_reset()
- [ ] Implement h11_parse() main loop
- [ ] Implement h11_get_state()
- [ ] Implement h11_get_request()
- [ ] Implement h11_error_offset()
- [ ] Implement h11_read_body()
- [ ] Handle leading CRLF in IDLE state
- [ ] Test pipelining scenarios
- [ ] Test partial data scenarios
- [ ] Test error state handling
