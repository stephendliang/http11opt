# Step 10: Connection Management

## Purpose

Implement connection persistence (keep-alive) semantics for HTTP/1.x. The parser determines whether the connection should be kept open after the current request/response exchange.

## RFC References

- **RFC 9112 Section 9.3** - Persistence
- **RFC 9110 Section 7.6.1** - Connection
- **RFC 9112 Section 9.6** - Tear-down

## Files to Modify

- `parser.c` - Connection header processing

---

## Part 1: HTTP/1.x Connection Defaults

### Version-Based Defaults

| Version | Default | Override |
|---------|---------|----------|
| HTTP/1.0 | Close | `Connection: keep-alive` |
| HTTP/1.1 | Keep-alive | `Connection: close` |

### RFC 9112 Section 9.3

> A client that supports persistent connections MAY "pipeline" its requests (i.e., send multiple requests without waiting for each response). A server MUST send its responses to those requests in the same order that the requests were received.

---

## Part 2: Connection Header Processing

### Grammar (RFC 9110)

```
Connection = #connection-option
connection-option = token
```

### Recognized Options

- `close` - Connection will close after this message
- `keep-alive` - Connection should persist (HTTP/1.0 compatibility)
- Other tokens - Names of hop-by-hop headers

### Implementation

```c
static void process_connection_options(h11_parser_t *p) {
    /* Set default based on HTTP version */
    if (p->request.version_major == 1 && p->request.version_minor >= 1) {
        p->request.keep_alive = true;   /* HTTP/1.1+ default */
    } else {
        p->request.keep_alive = false;  /* HTTP/1.0 default */
    }

    /* No Connection header? Use default */
    if (p->request.connection_index < 0) {
        return;
    }

    h11_slice_t value = p->request.headers[p->request.connection_index].value;

    /* Parse comma-separated tokens */
    const char *ptr = value.data;
    const char *end = value.data + value.len;

    while (ptr < end) {
        /* Skip OWS and commas */
        while (ptr < end && (*ptr == ' ' || *ptr == '\t' || *ptr == ',')) {
            ptr++;
        }

        if (ptr >= end) break;

        /* Extract token */
        const char *token_start = ptr;
        while (ptr < end && *ptr != ',' && *ptr != ' ' && *ptr != '\t') {
            ptr++;
        }

        size_t token_len = ptr - token_start;

        /* Check for close */
        if (token_len == 5 && strncasecmp(token_start, "close", 5) == 0) {
            p->request.keep_alive = false;
        }
        /* Check for keep-alive */
        else if (token_len == 10 && strncasecmp(token_start, "keep-alive", 10) == 0) {
            p->request.keep_alive = true;
        }
        /* Other tokens are hop-by-hop header names */
        else {
            /* Could store these for the application layer */
        }
    }
}
```

---

## Part 3: Connection State Tracking

### Parser State Structure Additions

```c
struct h11_parser {
    /* ... existing fields ... */

    /* Connection state */
    bool keep_alive;            /* Connection should persist */
    bool upgrade_requested;     /* Upgrade header present */
    bool expect_continue;       /* Expect: 100-continue */
};
```

### Request Structure Output

```c
typedef struct h11_request {
    /* ... existing fields ... */

    /* Connection semantics */
    bool keep_alive;           /* Should keep connection open */
    bool expect_continue;      /* Client expects 100 Continue */
    bool has_upgrade;          /* Upgrade header present */
} h11_request_t;
```

---

## Part 4: Conditions That Force Close

### Must Close After Response

1. **`Connection: close`** in request or response
2. **HTTP/1.0** without `Connection: keep-alive`
3. **Transfer-Encoding + Content-Length conflict** (if tolerated)
4. **Parse error** occurred
5. **Upgrade** was requested and accepted

### Implementation

```c
static void determine_connection_persistence(h11_parser_t *p) {
    /* Start with version default */
    process_connection_options(p);

    /* Force close on TE+CL conflict (if we tolerated it) */
    if (p->seen_transfer_encoding && p->seen_content_length &&
        !p->config.reject_te_cl_conflict) {
        p->request.keep_alive = false;
    }

    /* Upgrade requests typically close or switch protocols */
    if (p->request.has_upgrade) {
        /* Application decides whether to accept upgrade */
        /* If accepted, connection is upgraded, not kept alive normally */
    }
}
```

---

## Part 5: Multiple Requests (Pipelining)

### How Pipelining Works

```
Client                          Server
  |                               |
  | Request 1 ---------------->   |
  | Request 2 ---------------->   |
  | Request 3 ---------------->   |
  |                               |
  |   <---------------- Response 1|
  |   <---------------- Response 2|
  |   <---------------- Response 3|
```

### Parser Support

```c
/* After completing a request */
if (h11_get_state(parser) == H11_STATE_COMPLETE) {
    /* Process request */
    const h11_request_t *req = h11_get_request(parser);
    handle_request(req);

    if (req->keep_alive) {
        /* Reset parser for next request */
        h11_parser_reset(parser);

        /* Check if more data is already buffered */
        if (buffer_len > consumed) {
            /* Parse next request immediately */
            h11_parse(parser, buffer + consumed, buffer_len - consumed, &new_consumed);
        }
    } else {
        /* Close connection after response */
        close_connection();
    }
}
```

---

## Part 6: Hop-by-Hop Headers

### RFC 9110 Section 7.6.1

The `Connection` header lists headers that are hop-by-hop (not forwarded by proxies):

```
Connection: close, X-Custom-Header
```

This means `X-Custom-Header` should not be forwarded.

### Standard Hop-by-Hop Headers

- Connection
- Keep-Alive
- Proxy-Authenticate
- Proxy-Authorization
- TE
- Trailer
- Transfer-Encoding
- Upgrade

### Implementation (for proxies)

```c
typedef struct {
    const char *name;
    size_t len;
} h11_hop_by_hop_t;

static const h11_hop_by_hop_t standard_hop_by_hop[] = {
    {"Connection", 10},
    {"Keep-Alive", 10},
    {"Proxy-Authenticate", 18},
    {"Proxy-Authorization", 19},
    {"TE", 2},
    {"Trailer", 7},
    {"Transfer-Encoding", 17},
    {"Upgrade", 7}
};

bool h11_is_hop_by_hop(h11_slice_t name) {
    for (size_t i = 0; i < sizeof(standard_hop_by_hop)/sizeof(standard_hop_by_hop[0]); i++) {
        if (h11_slice_eq_case(name, standard_hop_by_hop[i].name,
                               standard_hop_by_hop[i].len)) {
            return true;
        }
    }
    return false;
}
```

---

## Part 7: Expect: 100-continue

### RFC 9110 Section 10.1.1

Client sends `Expect: 100-continue` to check if server will accept the body before sending it.

### Flow

```
Client                               Server
  |                                    |
  | POST /upload HTTP/1.1              |
  | Content-Length: 1000000            |
  | Expect: 100-continue               |
  | ---------------------------------> |
  |                                    | (check headers)
  |   <------ HTTP/1.1 100 Continue    |
  |                                    |
  | [body data] --------------------> |
  |                                    |
  |   <------ HTTP/1.1 200 OK          |
```

### Parser Support

```c
/* In process_semantic_header() */
if (h11_slice_eq_case(name, "Expect", 6)) {
    if (p->request.expect_index < 0) {
        p->request.expect_index = idx;
    }

    /* Check for 100-continue */
    if (contains_token_ci(value, "100-continue", 12)) {
        /* Only for HTTP/1.1+ */
        if (p->request.version_major == 1 && p->request.version_minor >= 1) {
            p->request.expect_continue = true;
        }
    }
}
```

### Application Handling

```c
if (h11_get_state(parser) == H11_STATE_BODY_IDENTITY ||
    h11_get_state(parser) == H11_STATE_BODY_CHUNKED_SIZE) {

    const h11_request_t *req = h11_get_request(parser);

    if (req->expect_continue) {
        /* Don't read body yet - application should send 100 Continue first */
        /* Or send error response if request is rejected */
        send_response("HTTP/1.1 100 Continue\r\n\r\n");
    }

    /* Now read body */
    h11_read_body(parser, ...);
}
```

---

## Part 8: Upgrade Header

### RFC 9110 Section 7.8

The `Upgrade` header requests protocol switching (e.g., to WebSocket).

### Detection

```c
/* In process_semantic_header() */
if (h11_slice_eq_case(name, "Upgrade", 7)) {
    p->request.has_upgrade = true;
}
```

### Application Response

If upgrade is accepted:
```
HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
```

After 101, the connection is no longer HTTP - parser should not be used further.

---

## Part 9: Keep-Alive Header (HTTP/1.0)

### Non-Standard but Common

```
Keep-Alive: timeout=5, max=100
```

Parameters:
- `timeout` - Seconds to keep connection idle
- `max` - Maximum requests on this connection

### Parsing (Optional)

```c
static void parse_keep_alive_params(h11_slice_t value,
                                     int *timeout, int *max) {
    *timeout = -1;
    *max = -1;

    /* Parse "param=value, param=value" format */
    const char *ptr = value.data;
    const char *end = value.data + value.len;

    while (ptr < end) {
        /* Skip whitespace and commas */
        while (ptr < end && (*ptr == ' ' || *ptr == '\t' || *ptr == ',')) {
            ptr++;
        }

        if (ptr >= end) break;

        /* Parse "name=value" */
        const char *name_start = ptr;
        while (ptr < end && *ptr != '=' && *ptr != ',' && *ptr != ' ') {
            ptr++;
        }

        size_t name_len = ptr - name_start;

        if (ptr < end && *ptr == '=') {
            ptr++;  /* Skip = */

            /* Parse value */
            int value_int = 0;
            while (ptr < end && *ptr >= '0' && *ptr <= '9') {
                value_int = value_int * 10 + (*ptr - '0');
                ptr++;
            }

            if (name_len == 7 && strncasecmp(name_start, "timeout", 7) == 0) {
                *timeout = value_int;
            } else if (name_len == 3 && strncasecmp(name_start, "max", 3) == 0) {
                *max = value_int;
            }
        }
    }
}
```

---

## Part 10: Connection Lifecycle

### State Diagram

```
               ┌─────────────┐
               │    IDLE     │
               └──────┬──────┘
                      │ Data arrives
                      ▼
               ┌─────────────┐
               │   PARSING   │
               └──────┬──────┘
                      │ Request complete
                      ▼
               ┌─────────────┐
               │  COMPLETE   │
               └──────┬──────┘
                      │
           ┌──────────┴──────────┐
           │                     │
           ▼                     ▼
    keep_alive=true       keep_alive=false
           │                     │
           ▼                     ▼
    ┌─────────────┐       ┌─────────────┐
    │    RESET    │       │    CLOSE    │
    │   (idle)    │       │ connection  │
    └─────────────┘       └─────────────┘
```

### Application Code Example

```c
void connection_loop(int socket, h11_parser_t *parser) {
    char buffer[8192];
    size_t buffer_len = 0;

    while (1) {
        /* Read more data */
        ssize_t n = read(socket, buffer + buffer_len, sizeof(buffer) - buffer_len);
        if (n <= 0) break;

        buffer_len += n;

        /* Parse */
        size_t consumed = 0;
        h11_error_t err = h11_parse(parser, buffer, buffer_len, &consumed);

        if (err == H11_NEED_MORE_DATA) {
            /* Need more data */
            continue;
        }

        if (err != H11_OK) {
            /* Parse error - send 400, close */
            send_error_response(socket, 400);
            break;
        }

        if (h11_get_state(parser) == H11_STATE_COMPLETE) {
            /* Handle complete request */
            const h11_request_t *req = h11_get_request(parser);

            send_response(socket, req);

            /* Check persistence */
            if (!req->keep_alive) {
                break;  /* Close connection */
            }

            /* Reset for next request */
            h11_parser_reset(parser);

            /* Shift buffer */
            if (consumed < buffer_len) {
                memmove(buffer, buffer + consumed, buffer_len - consumed);
                buffer_len -= consumed;
            } else {
                buffer_len = 0;
            }
        }
    }

    close(socket);
}
```

---

## Part 11: Edge Cases

### 1. HTTP/1.0 No Connection Header

```
GET / HTTP/1.0\r\n
Host: example.com\r\n
\r\n
```
**Result**: `keep_alive = false`

### 2. HTTP/1.0 with Keep-Alive

```
GET / HTTP/1.0\r\n
Connection: keep-alive\r\n
Host: example.com\r\n
\r\n
```
**Result**: `keep_alive = true`

### 3. HTTP/1.1 No Connection Header

```
GET / HTTP/1.1\r\n
Host: example.com\r\n
\r\n
```
**Result**: `keep_alive = true`

### 4. HTTP/1.1 with Close

```
GET / HTTP/1.1\r\n
Host: example.com\r\n
Connection: close\r\n
\r\n
```
**Result**: `keep_alive = false`

### 5. Multiple Connection Options

```
Connection: keep-alive, upgrade\r\n
```
**Result**: `keep_alive = true`, custom tokens recorded

### 6. Mixed Case

```
Connection: Keep-Alive\r\n
Connection: CLOSE\r\n
```
**Result**: Last one wins, `keep_alive = false`

### 7. TE+CL Conflict (tolerant mode)

```
Content-Length: 100\r\n
Transfer-Encoding: chunked\r\n
```
**Result**: Use chunked, `keep_alive = false` (force close)

---

## Implementation Checklist

- [ ] Set default keep_alive based on HTTP version
- [ ] Implement process_connection_options()
- [ ] Parse Connection header tokens
- [ ] Handle `close` token
- [ ] Handle `keep-alive` token
- [ ] Track hop-by-hop header names
- [ ] Implement h11_is_hop_by_hop()
- [ ] Handle Expect: 100-continue
- [ ] Detect Upgrade header
- [ ] Force close on TE+CL conflict
- [ ] Test all edge cases
- [ ] Document pipelining support
