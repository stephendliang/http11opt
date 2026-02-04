# HTTP/1.1 Request Parser — Complete Feature Checklist

This checklist enumerates the **required** and **recommended** features for a correct and complete HTTP/1.1 request parser (RFC 9110/9112 semantics). It is written as implementation‑agnostic requirements plus performance/robustness expectations. Where a feature is optional or configurable, it is stated explicitly.

---

## 1) Streaming & State Machine Core
- **Incremental parsing**: accept partial buffers; resume parsing when more data arrives.
- **Pipelining support**: parse multiple requests back‑to‑back on the same connection without losing state.
- **Clear state transitions**: start‑line → headers → body (or no body) → trailers (if chunked) → next request.
- **Error state**: detect invalid syntax and surface a clear error (with position if possible) without UB.
- **Deterministic consumption**: never over‑consume bytes belonging to the next request.

## 2) Request Line (Start‑Line) Parsing
- **Form**: `method SP request-target SP HTTP-version CRLF`.
- **Method**:
  - Parse as a **token** (tchar+) and preserve case (case‑sensitive by spec; commonly uppercase).
  - Accept extension methods (unknown tokens) without failing.
- **SP handling**:
  - Exactly one SP between elements (strict mode).
  - Optional tolerant mode may parse on whitespace-delimited boundaries (SP / HTAB / VT / FF / bare CR) while ignoring leading/trailing whitespace; default should be strict due to smuggling risk.
- **HTTP-version**:
  - Parse `HTTP/1.1` and `HTTP/1.0` (reject others unless explicitly supported).
  - Validate `HTTP/` prefix and `DIGIT.DIGIT` version format.
- **Line ending**:
  - Require CRLF in strict mode.
  - Optional tolerant mode to accept bare LF (flag as non-compliant).
  - Bare CR not followed by LF is invalid; tolerant mode may replace it with SP and continue.
- **Trailing whitespace**:
  - No extra OWS before CRLF in strict mode.
- **Length limits**:
  - Configurable maximum start‑line length (default to a safe value to avoid DoS).

## 3) Request Target Forms
Support all four HTTP/1.1 request‑target forms:
- **origin-form**: `absolute-path [ "?" query ]` (most common).
- **absolute-form**: `absolute-URI` (for proxies).
- **authority-form**: `authority` (for CONNECT).
- **asterisk-form**: `*` (for OPTIONS).

Validation requirements:
- **No SP/CTL** characters in request-target.
- **Request-target must be valid** for its form and URI grammar; reject invalid characters (including `#` unless percent-encoded), SP, and CTL.
- For origin-form: path must start with `/` (absolute-path); if the target URI path is empty, the client sends `/`.
- Percent-encoding **may appear**; parser should preserve raw bytes and leave normalization/decoding to higher layers.
- **Absolute-form** is an `absolute-URI` (no fragment). Preserve raw bytes or parse into components (scheme/authority/path/query) as needed.
- **authority-form** is only for CONNECT; **asterisk-form** is only for server-wide OPTIONS.
- If the request-target includes an authority component, it supplies the target URI authority; otherwise the `Host` field supplies the authority.
- For absolute-form, proxies and origin servers MUST ignore the received `Host` field and use the authority from the request-target; proxies must replace `Host` when forwarding.

## 4) Header Field Parsing (General)
- **Header section ends** on a CRLF alone (empty line).
- **No whitespace between start-line and first header field**: if a line begins with SP/HTAB before any header field, reject or ignore that line (and any following whitespace-prefixed lines) until a valid header field or the end of headers.
- **Field name**:
  - Parse as `token` (tchar+), **case‑insensitive** for comparison; must be non-empty.
  - The colon must appear immediately after the field-name (no OWS before `:`).
  - Reject if it contains separators/CTL/space.
- **Field value**:
  - Parse after `:` with optional OWS (SP / HTAB).
  - Allow visible chars, SP, HTAB, and obs‑text (0x80‑0xFF).
  - Reject CTL (0x00‑0x1F, 0x7F) except HTAB within OWS.
  - Reject any raw CR or LF within a field line/value.
- **Header line length / count limits**:
  - Configurable maximum header line length.
  - Configurable maximum total header size and header count.

### Obsolete line folding (obs‑fold)
- Detect header lines beginning with SP/HTAB as obs‑fold.
- **Default behavior**: reject as invalid per RFC 9112.
- **Optional tolerant mode**: replace each obs‑fold with a single SP and continue (mark request as non‑compliant).

## 5) Header Semantics & Validation
- **Host**:
  - HTTP/1.1 clients MUST send `Host` in all requests. A server MUST respond with 400 if `Host` is missing, if there is more than one `Host` field line, or if the field value is invalid for the request-target (empty is only valid when the authority is missing/undefined).
  - If the target URI includes an authority component, the `Host` field value MUST be identical to that authority (excluding any userinfo). If the authority is missing/undefined, the `Host` field value MUST be empty.
  - For absolute-form: origin servers and proxies MUST ignore the received `Host` and use the authority from the request-target; proxies must replace `Host` when forwarding.
  - Field value is `host [ ":" port ]` where `host` is `uri-host` (reg-name / IPv4 / "[" IPv6 "]") and `port` is 1*DIGIT (validate numeric range per policy).
- **Content-Length**:
  - Field value is 1+ digits, with optional OWS around the value; reject signs, decimals, or empty.
  - If the field value is a comma-separated list, it is invalid unless all members are valid and identical; you MAY accept by replacing it with a single value or reject it.
  - Detect integer overflow / precision loss and treat as invalid.
- **Transfer-Encoding**:
  - Parse a comma-separated list of codings in order (OWS around commas); merge multiple header lines into one list; tokens are case-insensitive.
  - If present, **must override** Content-Length for message framing.
  - The final coding for requests MUST be `chunked`; otherwise respond 400 and close.
  - `chunked` has no parameters; their presence is an error.
  - If unknown coding is present, respond 501 (Not Implemented).
  - If `Transfer-Encoding` appears in an HTTP/1.0 message, treat framing as faulty and close after processing.
- **Connection**:
  - Parse comma‑separated tokens.
  - Identify hop‑by‑hop headers named in `Connection` for later handling by higher layers.
  - Hop-by-hop fields received without a corresponding `Connection` option should be ignored as improperly forwarded.
- **Expect**:
  - Parse comma-separated expectations; only `100-continue` is defined.
  - Any other expectation should yield 417 (Expectation Failed) or 400.
  - In HTTP/1.0 requests, ignore `100-continue`.
- **TE**:
  - Describes transfer codings the client is willing to accept in the response; a `trailers` member indicates the client accepts trailer fields.
  - Grammar: `#t-codings` where each member is `trailers` or a transfer-coding token with optional weight/parameters.
  - The sender MUST include `TE` as a connection option in `Connection`; if missing, treat TE as improperly forwarded.
- **Trailer**:
  - `Trailer` lists field names likely to appear in the trailer section; sender SHOULD include it when trailers will be sent.
  - Trailer fields must be stored separately and MUST NOT be merged unless the field definition explicitly permits it.
- **Upgrade**:
  - Parse as per header grammar; surface for protocol upgrade decisions.
- **Set-Cookie**: do **not** merge multiple `Set-Cookie` headers; preserve as distinct fields.
- **Field list merging**: for other headers, allow merging into a single comma‑separated value **only** if the field is defined as a list; otherwise preserve duplicates.

## 6) Message Body Determination
- **Requests are never close-delimited**: if neither `Content-Length` nor `Transfer-Encoding` is present, there is no body and the message ends after the header section.
- **Content-Length**:
  - Body length is exactly the parsed Content-Length.
  - Reject if length exceeds configured maximum.
- **Transfer-Encoding: chunked**:
  - Body length is defined by chunk framing (see below).
- **Invalid combinations**:
  - `Transfer-Encoding` plus `Content-Length`: TE overrides CL; the server MAY reject or process using TE, but MUST close the connection after the response.
  - If `Transfer-Encoding` is present and the final coding is not `chunked`, respond 400 and close.

## 7) Chunked Transfer Coding Parsing
- **Chunk size line**: `1*HEXDIG [ chunk-ext ] CRLF`.
- **Chunk data**: exactly `chunk-size` bytes followed by `CRLF`.
- **Last chunk**: size `0` (1*'0') followed by `CRLF` (extensions allowed).
- **Chunk extensions**:
  - Grammar: `*( OWS ";" OWS token [ "=" ( token / quoted-string ) ] )`.
  - Must be able to parse/skip; ignore unrecognized extensions by default.
  - Apply a configurable limit to total chunk-extension length.
- **Trailers**:
  - After last chunk, parse trailer header fields until an empty CRLF.
  - Trailer fields must be stored separately and MUST NOT be merged unless the field definition explicitly permits it.
- **Strict CRLF** for chunk boundaries; tolerant bare-LF mode is strongly discouraged.
- **Numeric safety**: detect chunk-size overflow and enforce a max total decoded body size.

## 8) Character and Byte Handling
- **No implicit charset conversions**: treat input as bytes.
- **Preserve raw octets** of request-target and header values (higher layers handle decoding).
- **Validate ASCII rules** for method token and header names.
- **Support obs‑text** in header field values (0x80–0xFF).

## 9) Connection & Persistence Semantics
- **HTTP/1.1 default is keep‑alive** unless `Connection: close` present.
- **HTTP/1.0 default is close** unless `Connection: keep-alive` present.
- Parser should surface connection intent for connection manager.

## 10) Robustness & Security
- **Strict parsing by default**, optional tolerant mode behind an explicit flag.
- **Leading empty lines**: ignore at least one empty CRLF line before the request-line (SHOULD).
- **Defensive limits**:
  - Start‑line length, header line length, total header size, header count, body size, chunk count.
- **Invalid byte detection** to avoid request smuggling / splitting.
- **Consistent CRLF handling** to prevent ambiguity.
- **Reject ambiguous framing** (e.g., invalid `Content-Length` + `Transfer-Encoding`).

## 11) Output Structure (What Parser Should Emit)
- **Method** (token string).
- **Request-target** (raw string + parsed form identifier: origin/absolute/authority/asterisk).
- **HTTP version** (major/minor).
- **Header map** (case‑insensitive lookup) plus ordered list of raw header fields.
- **Body framing info**: none | content-length(n) | chunked.
- **Trailer fields** (if chunked).
- **Connection hints** (keep‑alive / close), **Expect** value, and any **Upgrade** request.

## 12) Performance Features (Including AVX512)
- **SIMD scanning** for delimiters (CRLF, `:`) and ASCII validation when CPU supports it.
  - Use **AVX2/AVX512** for wide scans of `\r` and `\n` to accelerate line detection.
  - Fallback to scalar scanning when SIMD is unavailable.
- **Zero‑copy slices** into the input buffer for method, target, and header values when safe.
- **Fast path** for common case:
  - `HTTP/1.1` + origin-form + small headers + no body or Content-Length.

## 13) Compliance Toggles (Recommended)
Provide explicit flags to control behavior:
- **strict_crlf** (reject bare LF).
- **reject_obs_fold** (default true).
- **tolerate_obs_text** (default true).
- **max_* limits** (line length, header count/size, body size).
- **allow_leading_crlf** (default true; ignore at least one empty line).
- **tolerate_request_line_whitespace** (default false).
- **allow_bare_lf_in_chunked** (default false; not recommended).
- **on_te_cl_conflict** (`reject` | `ignore_cl_and_close`, default reject).

## 14) Clear Error Reporting
- Error type + offset (byte index) for:
  - Invalid method token
  - Invalid request‑target
  - Invalid HTTP‑version
  - Malformed header line
  - Invalid `Content-Length`
  - Invalid `Transfer-Encoding`
  - Invalid chunk framing
- Provide enough detail for callers to map to HTTP 400/431/413 responses.

---

## Minimal Compliance Summary (Must-Haves)
If you implement **only the essentials**, the parser must still:
- Parse request line strictly per RFC 9112.
- Support all request‑target forms (origin/absolute/authority/asterisk) with method/form constraints and no fragments.
- Parse headers with token names and proper OWS handling.
- Enforce `Host` requirements for HTTP/1.1 and validate `host[:port]`.
- Determine body framing using `Transfer-Encoding` and/or `Content-Length` (requests are never close-delimited; TE must end in `chunked`).
- Parse chunked transfer coding including chunk extensions and trailers.
- Reject obs‑fold and invalid CRLF usage by default.
- Provide deterministic, streaming‑safe parsing with limits.

---

## Appendix A) Optional Compatibility / Hardening (Nice-to-Have)
- **HTTP/0.9 compatibility mode**: accept a request-line without an HTTP-version for legacy traffic (off by default).
- **`Proxy-Connection` compatibility**: if configured, treat `Proxy-Connection` like `Connection` for legacy clients.
- **Extra leading empty lines**: tolerate more than one empty CRLF line before the request-line if you must interoperate with broken clients.
- **Request-target normalization**: optional dot-segment removal and percent-decoding (with strict security review); otherwise pass raw bytes.
- **Non-URI character policy**: if you choose not to fully validate URI characters, provide a strict option to reject or normalize `\\` and other non-URI bytes.
