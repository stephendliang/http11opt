# H11 Parsing Spec

## S1. Request Line

### S1.1 Grammar (RFC 9112 S3)

```
request-line = method SP request-target SP HTTP-version CRLF
method       = 1*tchar
HTTP-version = "HTTP/" DIGIT "." DIGIT
```

### S1.2 Parse Algorithm

1. **Find line boundary**: `find_crlf(data, len)` → `crlf_pos` (spec_architecture:S5.1). If not found and `len >= max_request_line_len` → `H11_ERR_REQUEST_LINE_TOO_LONG`. If not found → `H11_NEED_MORE_DATA`.
2. **Parse method**: scan tchar characters until SP. Empty method → `H11_ERR_INVALID_METHOD`. If `H11_CFG_TOLERATE_SPACES` is set, consume multiple SP/HTAB after method.
3. **Parse request-target**: scan until SP, reject CTL (≤0x20) and DEL (0x7F). Empty target → `H11_ERR_INVALID_TARGET`. Validate all characters: no CTL, no SP. Call `h11_determine_target_form()` (spec_parsing:S2.2).
4. **Parse HTTP-version**: expect exactly `HTTP/DIGIT.DIGIT` (8 bytes). Verify `H`/`T`/`T`/`P`/`/` prefix (case-sensitive). Parse major/minor digits. Reject major ≠ 1. In strict mode (`H11_CFG_TOLERATE_SPACES` not set), reject trailing characters.
5. **Store results**: set `request.method`, `request.target` as spans; set `request.target_form`; set `request.version` to `(major << 8) | minor`; set `H11_REQF_KEEP_ALIVE` in `request.flags` if minor ≥ 1. Consume `line_len + 2` bytes.

### S1.3 Tolerant Mode

- `H11_CFG_TOLERATE_SPACES`: allow multiple SP/HTAB between method, target, and version; allow trailing whitespace before CRLF
- `H11_CFG_STRICT_CRLF` not set: use `find_line_ending()` which accepts bare LF (spec_architecture:S5.3)
- `H11_CFG_ALLOW_LEADING_CRLF`: IDLE state skips leading `\r\n` (or bare `\n` if tolerant) before request-line

### S1.4 Edge Cases

| Input | Error | Condition |
|-------|-------|-----------|
| ` /path HTTP/1.1\r\n` | `H11_ERR_INVALID_METHOD` | Empty method (starts with SP) |
| `GET@POST /path HTTP/1.1\r\n` | `H11_ERR_INVALID_METHOD` | Non-tchar `@` in method |
| `GET/path HTTP/1.1\r\n` | `H11_ERR_INVALID_METHOD` | Missing SP (slash is not tchar after token) |
| `GET /path HTTP/2.0\r\n` | `H11_ERR_INVALID_VERSION` | Major version ≠ 1 |
| `GET /path http/1.1\r\n` | `H11_ERR_INVALID_VERSION` | Lowercase "HTTP" |
| `GET /path\r\n` | `H11_ERR_INVALID_VERSION` | Missing version |
| `GET /path HTTP/1.1 \r\n` | `H11_ERR_INVALID_VERSION` | Trailing SP (strict) |
| `GET  /path HTTP/1.1\r\n` | `H11_ERR_INVALID_METHOD` (strict) / OK (tolerant) | Double SP |
| `GET /vvv...long HTTP/1.1\r\n` | `H11_ERR_REQUEST_LINE_TOO_LONG` | Exceeds limit |
| `GET /path HTTP/1.1` (no CRLF) | `H11_NEED_MORE_DATA` | Incomplete |

## S2. Request Target

### S2.1 Four Forms (RFC 9112 S3.2)

| Form | Detection | Grammar | Example |
|------|-----------|---------|---------|
| origin | Starts with `/` | `absolute-path [ "?" query ]` | `/index.html?q=1` |
| absolute | Contains `://` after scheme | `scheme "://" authority path [ "?" query ]` | `http://host/path` |
| authority | No `/` prefix, no `://`; contains `:` | `uri-host ":" port` | `example.com:443` |
| asterisk | Exactly `*` | `"*"` | `*` |

### S2.2 h11_determine_target_form() Decision Tree

1. If `len==1 && data[0]=='*'` → ASTERISK
2. If `data[0]=='/'` → ORIGIN, validate via `validate_origin_form()`
3. Find first `:` — if followed by `//` → ABSOLUTE, validate via `validate_absolute_form()`
4. Otherwise → AUTHORITY, validate via `validate_authority_form()`

### S2.3 Per-Form Validation

**Origin form**:
- Must start with `/`
- `#` (fragment) never allowed → `H11_ERR_INVALID_TARGET`
- CTL (≤0x20) and DEL (0x7F) not allowed
- `%` must be followed by exactly 2 HEXDIG (pct-encoded)
- Path chars: unreserved + sub-delims + `:` `@` `/` (via `H11_IS_URI`)
- Query allows additional `/` and `?`

**Absolute form**:
- Scheme: `ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )`
- Must have `://` after scheme
- Authority portion ends at `/` or `?` or end; must be non-empty
- No `#` fragment, no CTL/SP
- Percent-encoding validated same as origin

**Authority form**:
- Format: `uri-host ":" port`
- IPv6: `[address]:port` — brackets required, content is HEXDIG + `:` + `.`
- Non-IPv6 host: no CTL/SP
- Port: 1*DIGIT, value 0-65535
- Port required (unlike Host header)

**Asterisk form**: exactly `*`, no further validation.

### S2.4 Method-Form Compatibility

| Method | Allowed Forms |
|--------|--------------|
| CONNECT | authority only |
| OPTIONS | origin or asterisk |
| All others | origin or absolute |

Validated post-headers in `finalize_headers()`. CONNECT with non-authority or non-CONNECT with authority → `H11_ERR_INVALID_TARGET`.

### S2.5 URI Character Sets (RFC 3986)

- **unreserved**: `ALPHA DIGIT - . _ ~`
- **sub-delims**: `! $ & ' ( ) * + , ; =`
- **gen-delims**: `: / ? # [ ] @`
- **pct-encoded**: `% HEXDIG HEXDIG`

### S2.6 Edge Cases

| Input | Error | Reason |
|-------|-------|--------|
| `GET  HTTP/1.1\r\n` | `H11_ERR_INVALID_TARGET` | Empty target |
| `/path#frag` | `H11_ERR_INVALID_TARGET` | Fragment in target |
| `/path with space` | `H11_ERR_INVALID_TARGET` | SP in target |
| `/path%GG` | `H11_ERR_INVALID_TARGET` | Invalid pct-encoding |
| `/path%2` | `H11_ERR_INVALID_TARGET` | Incomplete pct-encoding |
| `?query` | `H11_ERR_INVALID_TARGET` | Missing path before query |
| `CONNECT example.com` | `H11_ERR_INVALID_TARGET` | No port for authority |
| `CONNECT host:99999` | `H11_ERR_INVALID_TARGET` | Port > 65535 |
| `CONNECT [::1]:8080` | OK | Valid IPv6 authority |
| `http:/path` | `H11_ERR_INVALID_TARGET` | Missing `//` |

## S3. Header Parsing

### S3.1 Grammar (RFC 9112 S5)

```
header-section = *( field-line CRLF )
field-line     = field-name ":" OWS field-value OWS
field-name     = token
field-value    = *( field-vchar [ 1*( SP / HTAB / field-vchar ) field-vchar ] )
field-vchar    = VCHAR / obs-text
OWS            = *( SP / HTAB )
```

### S3.2 h11_parse_headers() Loop

1. **Find line**: `find_crlf(line, remaining)` → `crlf_pos`. If not found: check `max_header_line_len` and `max_headers_size` limits, return `H11_NEED_MORE_DATA`.
2. **Empty line** (`crlf_pos==0`): end of headers → call `finalize_headers()`, transition to body state per `body_type`.
3. **Obs-fold check**: if line starts with SP/HTAB:
   - Before any header → `H11_ERR_LEADING_WHITESPACE`
   - If `H11_CFG_REJECT_OBS_FOLD` set → `H11_ERR_OBS_FOLD_REJECTED`
   - If tolerant: skip line, accumulate into `headers_size`
4. **Parse header**: call `parse_header_line()`, accumulate `headers_size`, check `max_header_count` and `max_headers_size`.

### S3.3 parse_header_line() Rules

1. **Find colon**: `find_char(line, len, ':')` (spec_architecture:S5.2). Position ≤ 0 → `H11_ERR_INVALID_HEADER_NAME`.
2. **Validate name**: every byte before colon must be tchar (`H11_IS_TCHAR`). Non-tchar → `H11_ERR_INVALID_HEADER_NAME`.
3. **Trim OWS**: skip SP/HTAB after colon (leading OWS); scan backward from end for trailing OWS.
4. **Validate value**: each byte must be SP, HTAB, VCHAR (0x21-0x7E), or obs-text (0x80-0xFF if `H11_CFG_ALLOW_OBS_TEXT` set). Invalid → `H11_ERR_INVALID_HEADER_VALUE`.
5. **Store**: call `h11_add_header()`, then `process_semantic_header()` for key header tracking.

### S3.4 Dynamic Array Storage

Headers and trailers are stored as `h11_header_t *` dynamic arrays managed internally by the parser. Capacity tracking and growth (initial 16 for headers, 8 for trailers, grow ×2 via `realloc`) are parser-internal details not exposed in the public `h11_request_t`.

### S3.5 Semantic Header Tracking

When a header is stored via `h11_add_header()`, its name is compared (case-insensitive) against the `h11_known_header_t` enum values. If it matches, the header's `name_id` is set to the corresponding `H11_KHDR_*` value and `H11_HEADER_F_KNOWN_NAME` is set in `flags`; otherwise `name_id = H11_INDEX_NONE`. Then `process_semantic_header()` updates the request:

| Header (case-insensitive) | Action |
|--------------------------|--------|
| Host | Set `known_idx[H11_KHDR_HOST]` to header index, set `H11_REQF_HAS_HOST` flag |
| Content-Length | Set `known_idx[H11_KHDR_CONTENT_LENGTH]`, set `H11_REQF_HAS_CONTENT_LENGTH` flag |
| Transfer-Encoding | Set `known_idx[H11_KHDR_TRANSFER_ENCODING]`, set `H11_REQF_HAS_TRANSFER_ENCODING` flag |
| Connection | Set `known_idx[H11_KHDR_CONNECTION]`; if contains `close` → clear `H11_REQF_KEEP_ALIVE`; if contains `keep-alive` → set `H11_REQF_KEEP_ALIVE` |
| Expect | Set `known_idx[H11_KHDR_EXPECT]`; if contains `100-continue` → set `H11_REQF_EXPECT_CONTINUE` |
| Upgrade | Set `known_idx[H11_KHDR_UPGRADE]`, set `H11_REQF_HAS_UPGRADE` |

`known_idx[k]` stores the index (into `headers[]`) of the first occurrence; `H11_INDEX_NONE` if not present.

### S3.6 Limits

| Config Field | Check Point | Error |
|-------------|------------|-------|
| `max_header_line_len` | Each line after find_crlf | `H11_ERR_HEADER_LINE_TOO_LONG` |
| `max_header_count` | After each header stored | `H11_ERR_TOO_MANY_HEADERS` |
| `max_headers_size` | Accumulated bytes after each line | `H11_ERR_HEADERS_TOO_LARGE` |

### S3.7 Edge Cases

| Input | Error | Reason |
|-------|-------|--------|
| `InvalidHeader\r\n` | `H11_ERR_INVALID_HEADER_NAME` | No colon |
| `Header Name: val\r\n` | `H11_ERR_INVALID_HEADER_NAME` | SP in name |
| `: value\r\n` | `H11_ERR_INVALID_HEADER_NAME` | Empty name (colon at pos 0) |
| `X-Empty:\r\n` | OK | Empty value is valid |
| `Content-Type:   text/html   \r\n` | OK | Value = `text/html` (OWS trimmed) |
| `X-H: val\x01here\r\n` | `H11_ERR_INVALID_HEADER_VALUE` | CTL in value |
| `X-H: val\r\n continued\r\n` | `H11_ERR_OBS_FOLD_REJECTED` (default) | Obs-fold |
| 101st header | `H11_ERR_TOO_MANY_HEADERS` | Exceeds default 100 |

## S4. Header Semantics

### S4.1 Validation Order

`finalize_headers()` runs after all headers parsed (empty line found):
1. Validate Host header
2. Validate Content-Length (if present)
3. Validate Transfer-Encoding (if present)
4. Check TE+CL conflict
5. Determine body framing (spec_body_and_connection:S1)
6. Validate method-target compatibility (spec_parsing:S2.4)

### S4.2 Host Rules (RFC 9110 S7.2)

- HTTP/1.1 MUST have exactly one Host header; HTTP/1.0 does not require it
- Missing → `H11_ERR_MISSING_HOST`; multiple → `H11_ERR_MULTIPLE_HOST`
- Value format: `uri-host [ ":" port ]`
  - IPv6: `[address]` — brackets required, content is HEXDIG + `:` + `.`
  - reg-name/IPv4: no CTL/SP
  - Port: `1*DIGIT`, optional, 0-65535
- Empty Host is valid only when target authority is absent

### S4.3 Content-Length Rules (RFC 9110 S8.6)

- Value: `1*DIGIT` — no signs, no decimals, no leading whitespace (tolerate trailing OWS)
- Overflow check: `result > (UINT64_MAX - digit) / 10` → `H11_ERR_CONTENT_LENGTH_OVERFLOW`
- Comma-separated list in single header: all values must be identical; differing → `H11_ERR_MULTIPLE_CONTENT_LENGTH`
- Multiple CL headers: all must have same parsed value → `H11_ERR_MULTIPLE_CONTENT_LENGTH`
- Check `content_length > max_body_size` → `H11_ERR_BODY_TOO_LARGE`

### S4.4 Transfer-Encoding Rules (RFC 9112 S6.1)

- Comma-separated tokens, case-insensitive; merge multiple TE headers into one list
- Final coding MUST be `chunked` → else `H11_ERR_TE_NOT_CHUNKED_FINAL`
- `chunked` MUST NOT have parameters (`;` after `chunked`) → `H11_ERR_INVALID_TRANSFER_ENCODING`
- Known codings: `chunked`, `gzip`, `deflate`, `compress`, `identity`
- Unknown coding → `H11_ERR_UNKNOWN_TRANSFER_CODING`
- TE in HTTP/1.0 → treat as faulty, force close

### S4.5 TE+CL Conflict

- If both present and `H11_CFG_REJECT_TE_CL_CONFLICT` set in config flags → `H11_ERR_TE_CL_CONFLICT`
- If tolerant: TE takes precedence, set `body_type=CHUNKED`, clear `H11_REQF_KEEP_ALIVE`

### S4.6 Connection Token Parsing

- Grammar: `Connection = #connection-option` (comma-separated tokens)
- `close` → clear `H11_REQF_KEEP_ALIVE`; `keep-alive` → set `H11_REQF_KEEP_ALIVE`
- Other tokens name hop-by-hop headers (for proxy use)

### S4.7 Expect 100-continue (RFC 9110 S10.1.1)

- If `Expect` contains `100-continue` token and version ≥ HTTP/1.1 → set `H11_REQF_EXPECT_CONTINUE` in `request.flags`
- HTTP/1.0: ignore 100-continue
- Unknown expectations: could yield 417 (surfaced via flag, not parser error)

### S4.8 Validation Summary

| Header | Requirement | Error |
|--------|-------------|-------|
| Host | Required for HTTP/1.1, exactly one | `MISSING_HOST` / `MULTIPLE_HOST` |
| Host | Valid uri-host[:port] | `INVALID_HOST` |
| Content-Length | 1*DIGIT, no overflow | `INVALID_CONTENT_LENGTH` / `CONTENT_LENGTH_OVERFLOW` |
| Content-Length | All values identical | `MULTIPLE_CONTENT_LENGTH` |
| Transfer-Encoding | Final = chunked | `TE_NOT_CHUNKED_FINAL` |
| Transfer-Encoding | Known codings | `UNKNOWN_TRANSFER_CODING` |
| Transfer-Encoding | chunked has no params | `INVALID_TRANSFER_ENCODING` |
| TE + CL | Conflict | `TE_CL_CONFLICT` |

### S4.9 Edge Cases

| Scenario | Error |
|----------|-------|
| HTTP/1.1 missing Host | `H11_ERR_MISSING_HOST` |
| Two Host headers | `H11_ERR_MULTIPLE_HOST` |
| `Host: example .com` | `H11_ERR_INVALID_HOST` |
| `Content-Length: +100` | `H11_ERR_INVALID_CONTENT_LENGTH` |
| `Content-Length: 99999999999999999999` | `H11_ERR_CONTENT_LENGTH_OVERFLOW` |
| CL: 100 + CL: 200 | `H11_ERR_MULTIPLE_CONTENT_LENGTH` |
| `Transfer-Encoding: gzip` (no chunked final) | `H11_ERR_TE_NOT_CHUNKED_FINAL` |
| `Transfer-Encoding: chunked;q=1` | `H11_ERR_INVALID_TRANSFER_ENCODING` |
| `Transfer-Encoding: unknown, chunked` | `H11_ERR_UNKNOWN_TRANSFER_CODING` |
| CL + TE present (reject mode) | `H11_ERR_TE_CL_CONFLICT` |
