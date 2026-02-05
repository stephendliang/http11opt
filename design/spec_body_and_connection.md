# H11 Body & Connection Spec

## S1. Body Framing

### S1.1 Priority (RFC 9112 S6.3)

1. **Transfer-Encoding present** → chunked (TE always overrides CL)
2. **Content-Length present** → identity body of exactly CL bytes
3. **Neither present** → no body

Requests are **never** close-delimited.

### S1.2 State Transitions After Headers

| body_type | content_length | Next State | Setup |
|-----------|---------------|------------|-------|
| `H11_BODY_NONE` | — | COMPLETE | — |
| `H11_BODY_CONTENT_LENGTH` | 0 | COMPLETE | — |
| `H11_BODY_CONTENT_LENGTH` | > 0 | BODY_IDENTITY | `body_remaining = content_length` |
| `H11_BODY_CHUNKED` | — | BODY_CHUNKED_SIZE | — |

### S1.3 Framing Decision Tree

- Has Transfer-Encoding?
  - Yes → Final coding is chunked?
    - Yes → Has Content-Length?
      - Yes → `H11_CFG_REJECT_TE_CL_CONFLICT`?
        - true → `H11_ERR_TE_CL_CONFLICT`
        - false → Use chunked, clear `H11_REQF_KEEP_ALIVE`
      - No → Use chunked
    - No → `H11_ERR_TE_NOT_CHUNKED_FINAL`
  - No → Has Content-Length?
    - Yes → `content_length > max_body_size`?
      - Yes → `H11_ERR_BODY_TOO_LARGE`
      - No → Use identity body
    - No → No body

## S2. Identity Body Reading

### S2.1 h11_read_body() Rules (BODY_IDENTITY state)

1. `to_read = min(len, body_remaining)`
2. Zero-copy: set `*body_out = data`, `*body_len = to_read`
3. `body_remaining -= to_read`, `total_body_read += to_read`
4. If `body_remaining == 0` → transition to COMPLETE
5. Return consumed = `to_read`

### S2.2 max_body_size Enforcement

If `max_body_size != SIZE_MAX` and `total_body_read + to_read > max_body_size` → set ERROR state, return `H11_ERR_BODY_TOO_LARGE`.

## S3. Chunked Encoding

### S3.1 Grammar (RFC 9112 S7.1)

```
chunked-body = *chunk last-chunk trailer-section CRLF
chunk        = chunk-size [ chunk-ext ] CRLF chunk-data CRLF
chunk-size   = 1*HEXDIG
last-chunk   = 1*("0") [ chunk-ext ] CRLF
chunk-ext    = *( BWS ";" BWS chunk-ext-name [ BWS "=" BWS chunk-ext-val ] )
chunk-ext-val = token / quoted-string
trailer-section = *( field-line CRLF )
```

### S3.2 Chunk Size Parsing (BODY_CHUNKED_SIZE state)

1. **Find line**: `find_crlf(data, len)` (spec_architecture:S5.1). If not found and `len > 100` → `H11_ERR_INVALID_CHUNK_SIZE`. If not found → `H11_NEED_MORE_DATA`.
2. **Parse HEXDIG**: scan hex digits, accumulate `chunk_size = chunk_size * 16 + digit`.
3. **Overflow check**: `chunk_size > (UINT64_MAX - digit) / 16` → `H11_ERR_CHUNK_SIZE_OVERFLOW`
4. **No hex digits** → `H11_ERR_INVALID_CHUNK_SIZE`
5. **max_body_size check**: `total_body_read + chunk_size > max_body_size` → `H11_ERR_BODY_TOO_LARGE`
6. **Parse chunk extensions** if remaining bytes on line (see S3.3)
7. Consume `line_len + 2`. If `chunk_size == 0` → TRAILERS. If `chunk_size > 0` → BODY_CHUNKED_DATA with `body_remaining = chunk_size`.

### S3.3 Chunk Extensions

Grammar: `BWS ";" BWS token [ "=" ( token / quoted-string ) ]` repeated.

- BWS = optional SP/HTAB
- Extension name: token (tchar+)
- Extension value: token or quoted-string (with `\` escaping inside quotes)
- Content is discarded (not stored)
- `ext_len` accumulated; if `> max_chunk_ext_len` → `H11_ERR_CHUNK_EXT_TOO_LONG`
- Invalid syntax → `H11_ERR_INVALID_CHUNK_EXT`

### S3.4 Chunk Data Reading (BODY_CHUNKED_DATA state)

Same pattern as identity body:
1. `to_read = min(len, body_remaining)`
2. Zero-copy: `*body_out = data`, `*body_len = to_read`
3. `body_remaining -= to_read`, `total_body_read += to_read`
4. If `body_remaining == 0` → transition to BODY_CHUNKED_CRLF

### S3.5 Post-Chunk CRLF (BODY_CHUNKED_CRLF state)

1. Need ≥ 2 bytes; if `< 2` → `H11_NEED_MORE_DATA`
2. Expect `\r\n`; if mismatch → `H11_ERR_INVALID_CHUNK_DATA`
3. Consume 2 bytes → transition to BODY_CHUNKED_SIZE

## S4. Trailers

### S4.1 Parsing

Reuses header parsing logic via `h11_parse_trailers()`:
1. Find line with `find_crlf()`
2. Empty line → transition to COMPLETE
3. Non-empty: find colon with `find_char()`, validate name (tchar), trim OWS, validate value
4. Store in `request.trailers` (NOT merged with `request.headers`)

### S4.2 Storage

Trailers stored as `h11_header_t` array in `request.trailers` with `trailer_count`. Capacity tracking is internal to the parser (initial 8, grow ×2).

## S5. Connection Management

### S5.1 Keep-Alive Defaults

| Version | Default | Override |
|---------|---------|----------|
| HTTP/1.0 | close | `Connection: keep-alive` |
| HTTP/1.1 | keep-alive | `Connection: close` |

Set in request-line parsing: `H11_REQF_KEEP_ALIVE` flag set if minor version ≥ 1 (i.e. `(version & 0xFF) >= 1`). Overridden by Connection header tokens.

### S5.2 Connection Token Parsing (RFC 9110 S7.6.1)

Grammar: `Connection = #connection-option` (comma-separated tokens).

- `close` → clear `H11_REQF_KEEP_ALIVE` flag
- `keep-alive` → set `H11_REQF_KEEP_ALIVE` flag
- Other tokens → hop-by-hop header names (for proxy forwarding)

### S5.3 Force-Close Conditions

1. `Connection: close` in request (`H11_REQF_KEEP_ALIVE` cleared)
2. HTTP/1.0 without `Connection: keep-alive` (`H11_REQF_KEEP_ALIVE` not set)
3. TE+CL conflict tolerated (not rejected) — `H11_REQF_KEEP_ALIVE` cleared
4. Parse error occurred (state=ERROR)
5. Upgrade requested and accepted (connection switches protocol)
6. TE in HTTP/1.0 message

### S5.4 Hop-by-Hop Headers

Static list (always hop-by-hop per RFC 9110): Connection, Keep-Alive, Proxy-Authenticate, Proxy-Authorization, TE, Trailer, Transfer-Encoding, Upgrade.

`bool h11_is_hop_by_hop(const char *base, h11_span_t name)` — checks against static list plus any tokens named in `Connection` header.

### S5.5 Expect 100-continue (RFC 9110 S10.1.1)

1. Parser sets `H11_REQF_EXPECT_CONTINUE` in `request.flags` if Expect header contains `100-continue` and version ≥ 1.1
2. Application checks `request.flags & H11_REQF_EXPECT_CONTINUE` after headers are complete (state is BODY_IDENTITY or BODY_CHUNKED_SIZE)
3. Application sends `100 Continue` response (or error) before reading body

### S5.6 Upgrade Detection

`H11_REQF_HAS_UPGRADE` flag set in `request.flags` when Upgrade header present. After 101 response, connection is no longer HTTP — parser must not be used further.

### S5.7 Pipelining

After COMPLETE: call `h11_parser_reset()`, then parse next request from remaining buffer. `reset()` preserves allocated arrays (just zeros counts), sets state to IDLE.

### S5.8 Keep-Alive Parameters (HTTP/1.0)

Non-standard `Keep-Alive` header: `timeout=N, max=N`. Parser can optionally parse for application use. `timeout` = idle seconds, `max` = max requests on connection.

## S6. Edge Cases

| Scenario | Result |
|----------|--------|
| `Content-Length: 0` | `body_type=CL`, immediate COMPLETE |
| GET with no body headers | `body_type=NONE`, immediate COMPLETE |
| POST with no body headers | `body_type=NONE`, immediate COMPLETE (valid at parser level) |
| TE+CL reject mode | `H11_ERR_TE_CL_CONFLICT` |
| TE+CL tolerant mode | Use chunked, `H11_REQF_KEEP_ALIVE` cleared |
| TE without chunked final | `H11_ERR_TE_NOT_CHUNKED_FINAL` |
| Body exceeds max_body_size during read | `H11_ERR_BODY_TOO_LARGE` |
| Chunk size `0\r\n\r\n` (no data, no trailers) | No body data, empty trailers, COMPLETE |
| Chunk with extensions `A;ext=value\r\n` | 10 bytes data, extensions discarded |
| Chunk size leading zeros `00A\r\n` | Valid, size = 10 |
| Chunk size overflow `FFFFFFFFFFFFFFFF1\r\n` | `H11_ERR_CHUNK_SIZE_OVERFLOW` |
| Missing CRLF after chunk data | `H11_ERR_INVALID_CHUNK_DATA` |
| Non-hex in chunk size | `H11_ERR_INVALID_CHUNK_SIZE` |
| Chunk extension > 1024 bytes | `H11_ERR_CHUNK_EXT_TOO_LONG` |
| HTTP/1.0 no Connection header | `H11_REQF_KEEP_ALIVE` not set |
| HTTP/1.0 + `Connection: keep-alive` | `H11_REQF_KEEP_ALIVE` set |
| HTTP/1.1 no Connection header | `H11_REQF_KEEP_ALIVE` set |
| HTTP/1.1 + `Connection: close` | `H11_REQF_KEEP_ALIVE` cleared |
| Multiple Connection options `keep-alive, upgrade` | `H11_REQF_KEEP_ALIVE` set, tokens recorded |
| TE+CL tolerant mode connection | `H11_REQF_KEEP_ALIVE` cleared (force close) |
