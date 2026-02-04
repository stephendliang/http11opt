# Step 4: Request Target Validation

## Purpose

Validate and classify the request-target into one of four forms defined by RFC 9112. The request-target form affects how the parser validates the target and how the server interprets the request.

## RFC References

- **RFC 9112 Section 3.2** - Request Target
- **RFC 3986** - URI Generic Syntax
- **RFC 9110 Section 7.1** - Determining Target URI

## Files to Modify

- `parser.c` - Request target validation and form detection

---

## Part 1: Request Target Forms

### The Four Forms (RFC 9112 Section 3.2)

```
request-target = origin-form
               / absolute-form
               / authority-form
               / asterisk-form

origin-form    = absolute-path [ "?" query ]
absolute-form  = absolute-URI
authority-form = uri-host ":" port
asterisk-form  = "*"
```

### Visual Examples

```
origin-form:     /path/to/resource?query=value
                 │                │
                 absolute-path    query

absolute-form:   http://example.com/path?query
                 │      │          │
                 scheme authority  path+query

authority-form:  example.com:443
                 │           │
                 host        port

asterisk-form:   *
```

---

## Part 2: Form Detection Function

### Signature

```c
h11_error_t h11_determine_target_form(h11_slice_t target, h11_target_form_t *form);
```

### Implementation

```c
h11_error_t h11_determine_target_form(h11_slice_t target, h11_target_form_t *form) {
    if (target.len == 0) {
        return H11_ERR_INVALID_TARGET;
    }

    const char *data = target.data;
    size_t len = target.len;

    /* Asterisk form: exactly "*" */
    if (len == 1 && data[0] == '*') {
        *form = H11_TARGET_ASTERISK;
        return H11_OK;
    }

    /* Origin form: starts with "/" */
    if (data[0] == '/') {
        *form = H11_TARGET_ORIGIN;
        return validate_origin_form(target);
    }

    /* Check for scheme (absolute-form starts with scheme "://") */
    size_t colon_pos = 0;
    while (colon_pos < len && data[colon_pos] != ':') {
        colon_pos++;
    }

    if (colon_pos < len && colon_pos + 2 < len) {
        if (data[colon_pos + 1] == '/' && data[colon_pos + 2] == '/') {
            /* Looks like absolute-form: scheme://... */
            *form = H11_TARGET_ABSOLUTE;
            return validate_absolute_form(target);
        }
    }

    /* Otherwise, authority-form (for CONNECT method) */
    /* host:port format */
    *form = H11_TARGET_AUTHORITY;
    return validate_authority_form(target);
}
```

---

## Part 3: Origin Form Validation

### Grammar

```
origin-form    = absolute-path [ "?" query ]
absolute-path  = 1*( "/" segment )
segment        = *pchar
query          = *( pchar / "/" / "?" )
pchar          = unreserved / pct-encoded / sub-delims / ":" / "@"
```

### Implementation

```c
static h11_error_t validate_origin_form(h11_slice_t target) {
    const char *data = target.data;
    size_t len = target.len;

    /* Must start with "/" */
    if (len == 0 || data[0] != '/') {
        return H11_ERR_INVALID_TARGET;
    }

    bool in_query = false;

    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)data[i];

        /* Check for query start */
        if (c == '?') {
            in_query = true;
            continue;
        }

        /* Fragment (#) is NEVER allowed in request-target */
        if (c == '#') {
            return H11_ERR_INVALID_TARGET;
        }

        /* CTL and SP are not allowed */
        if (c <= 0x20 || c == 0x7F) {
            return H11_ERR_INVALID_TARGET;
        }

        /* Percent-encoded sequences */
        if (c == '%') {
            if (i + 2 >= len) {
                return H11_ERR_INVALID_TARGET;
            }
            if (!H11_IS_HEXDIG(data[i + 1]) || !H11_IS_HEXDIG(data[i + 2])) {
                return H11_ERR_INVALID_TARGET;
            }
            i += 2;  /* Skip hex digits */
            continue;
        }

        /* Path characters (unreserved + sub-delims + : @ /) */
        if (!is_path_char(c) && !in_query) {
            return H11_ERR_INVALID_TARGET;
        }

        /* Query allows additional: / ? */
        if (in_query && !is_query_char(c)) {
            return H11_ERR_INVALID_TARGET;
        }
    }

    return H11_OK;
}

static bool is_path_char(uint8_t c) {
    /* unreserved / sub-delims / : / @ / / */
    return H11_IS_URI(c);
}

static bool is_query_char(uint8_t c) {
    /* pchar / "/" / "?" */
    return H11_IS_URI(c) || c == '/' || c == '?';
}
```

---

## Part 4: Absolute Form Validation

### Grammar

```
absolute-form  = absolute-URI
absolute-URI   = scheme ":" hier-part [ "?" query ]
hier-part      = "//" authority path-abempty
               / path-absolute
               / path-rootless
               / path-empty
scheme         = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
```

### Key Constraints

- **No fragment**: Absolute-URI cannot contain `#fragment`
- **Scheme required**: Must start with scheme (http, https, etc.)
- **Authority required**: Must have `//authority` after scheme

### Implementation

```c
static h11_error_t validate_absolute_form(h11_slice_t target) {
    const char *data = target.data;
    size_t len = target.len;
    size_t pos = 0;

    /* Parse scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) */
    if (len == 0 || !is_alpha(data[0])) {
        return H11_ERR_INVALID_TARGET;
    }
    pos++;

    while (pos < len && is_scheme_char(data[pos])) {
        pos++;
    }

    /* Expect "://" */
    if (pos + 3 > len || data[pos] != ':' || data[pos + 1] != '/' || data[pos + 2] != '/') {
        return H11_ERR_INVALID_TARGET;
    }
    pos += 3;

    /* Parse authority (ends at / ? or end of string) */
    size_t authority_start = pos;
    while (pos < len && data[pos] != '/' && data[pos] != '?') {
        uint8_t c = (uint8_t)data[pos];

        /* Fragment not allowed */
        if (c == '#') {
            return H11_ERR_INVALID_TARGET;
        }

        /* CTL and SP not allowed */
        if (c <= 0x20 || c == 0x7F) {
            return H11_ERR_INVALID_TARGET;
        }

        pos++;
    }

    /* Authority must not be empty */
    if (pos == authority_start) {
        return H11_ERR_INVALID_TARGET;
    }

    /* Rest is path + optional query */
    while (pos < len) {
        uint8_t c = (uint8_t)data[pos];

        /* Fragment not allowed */
        if (c == '#') {
            return H11_ERR_INVALID_TARGET;
        }

        /* CTL and SP not allowed */
        if (c <= 0x20 || c == 0x7F) {
            return H11_ERR_INVALID_TARGET;
        }

        /* Validate percent-encoding */
        if (c == '%') {
            if (pos + 2 >= len) {
                return H11_ERR_INVALID_TARGET;
            }
            if (!H11_IS_HEXDIG(data[pos + 1]) || !H11_IS_HEXDIG(data[pos + 2])) {
                return H11_ERR_INVALID_TARGET;
            }
            pos += 3;
            continue;
        }

        pos++;
    }

    return H11_OK;
}

static bool is_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool is_scheme_char(char c) {
    return is_alpha(c) || H11_IS_DIGIT(c) || c == '+' || c == '-' || c == '.';
}
```

---

## Part 5: Authority Form Validation

### Grammar

```
authority-form = uri-host ":" port
uri-host       = IP-literal / IPv4address / reg-name
port           = *DIGIT
```

### Usage

- Only valid for CONNECT method
- Represents the target host:port for tunneling

### Implementation

```c
static h11_error_t validate_authority_form(h11_slice_t target) {
    const char *data = target.data;
    size_t len = target.len;

    /* Must contain exactly one colon (between host and port) */
    ssize_t colon_pos = -1;
    size_t colon_count = 0;

    /* Handle IPv6 addresses: [::1]:port */
    bool in_ipv6 = false;
    if (len > 0 && data[0] == '[') {
        in_ipv6 = true;
    }

    for (size_t i = 0; i < len; i++) {
        if (data[i] == '[') {
            in_ipv6 = true;
        } else if (data[i] == ']') {
            in_ipv6 = false;
        } else if (data[i] == ':' && !in_ipv6) {
            colon_count++;
            colon_pos = i;
        }
    }

    /* For non-IPv6, must have exactly one colon */
    if (colon_pos < 0) {
        return H11_ERR_INVALID_TARGET;
    }

    /* Validate host portion */
    h11_slice_t host = {data, (size_t)colon_pos};
    if (!validate_host(host)) {
        return H11_ERR_INVALID_TARGET;
    }

    /* Validate port portion */
    size_t port_start = colon_pos + 1;
    size_t port_len = len - port_start;

    if (port_len == 0) {
        return H11_ERR_INVALID_TARGET;  /* Port required for authority-form */
    }

    /* Port must be all digits */
    for (size_t i = port_start; i < len; i++) {
        if (!H11_IS_DIGIT(data[i])) {
            return H11_ERR_INVALID_TARGET;
        }
    }

    /* Optional: validate port range (0-65535) */
    /* Parsing as uint64 to check for overflow */
    uint64_t port = 0;
    for (size_t i = port_start; i < len; i++) {
        port = port * 10 + (data[i] - '0');
        if (port > 65535) {
            return H11_ERR_INVALID_TARGET;
        }
    }

    return H11_OK;
}

static bool validate_host(h11_slice_t host) {
    if (host.len == 0) {
        return false;
    }

    /* IPv6: [address] */
    if (host.data[0] == '[') {
        if (host.len < 4 || host.data[host.len - 1] != ']') {
            return false;
        }
        /* Validate IPv6 content */
        for (size_t i = 1; i < host.len - 1; i++) {
            char c = host.data[i];
            if (!H11_IS_HEXDIG(c) && c != ':' && c != '.') {
                return false;
            }
        }
        return true;
    }

    /* IPv4 or reg-name */
    for (size_t i = 0; i < host.len; i++) {
        uint8_t c = (uint8_t)host.data[i];
        /* unreserved / sub-delims characters */
        if (c <= 0x20 || c == 0x7F) {
            return false;
        }
    }

    return true;
}
```

---

## Part 6: Method-Form Compatibility

### RFC Requirements

| Method | Valid Forms |
|--------|-------------|
| CONNECT | authority-form only |
| OPTIONS | origin-form or asterisk-form |
| All others | origin-form or absolute-form |

### Validation (Post-Parse)

```c
/* Called after headers are complete, when we know the method */
static h11_error_t validate_method_target_compatibility(h11_parser_t *p) {
    h11_slice_t method = p->request.method;
    h11_target_form_t form = p->request.target_form;

    /* CONNECT must use authority-form */
    if (h11_slice_eq_case(method, "CONNECT", 7)) {
        if (form != H11_TARGET_AUTHORITY) {
            return H11_ERR_INVALID_TARGET;
        }
    }

    /* Authority-form is only for CONNECT */
    if (form == H11_TARGET_AUTHORITY) {
        if (!h11_slice_eq_case(method, "CONNECT", 7)) {
            return H11_ERR_INVALID_TARGET;
        }
    }

    /* Asterisk-form is only for OPTIONS */
    if (form == H11_TARGET_ASTERISK) {
        if (!h11_slice_eq_case(method, "OPTIONS", 7)) {
            return H11_ERR_INVALID_TARGET;
        }
    }

    return H11_OK;
}
```

---

## Part 7: Character Sets

### Unreserved Characters (RFC 3986)

```
unreserved = ALPHA / DIGIT / "-" / "." / "_" / "~"
```

### Sub-delimiters

```
sub-delims = "!" / "$" / "&" / "'" / "(" / ")" / "*" / "+" / "," / ";" / "="
```

### Reserved Characters

```
reserved = gen-delims / sub-delims
gen-delims = ":" / "/" / "?" / "#" / "[" / "]" / "@"
```

### Percent-Encoding

```
pct-encoded = "%" HEXDIG HEXDIG
```

### URI Character Table Construction

```c
/* Build in util.c */
const uint8_t h11_uri_table[256] = {
    /* Initialize to 0 */
};

void init_uri_table(void) {
    /* Unreserved */
    for (char c = 'A'; c <= 'Z'; c++) h11_uri_table[(uint8_t)c] = 1;
    for (char c = 'a'; c <= 'z'; c++) h11_uri_table[(uint8_t)c] = 1;
    for (char c = '0'; c <= '9'; c++) h11_uri_table[(uint8_t)c] = 1;
    h11_uri_table['-'] = 1;
    h11_uri_table['.'] = 1;
    h11_uri_table['_'] = 1;
    h11_uri_table['~'] = 1;

    /* Sub-delims */
    h11_uri_table['!'] = 1;
    h11_uri_table['$'] = 1;
    h11_uri_table['&'] = 1;
    h11_uri_table['\''] = 1;
    h11_uri_table['('] = 1;
    h11_uri_table[')'] = 1;
    h11_uri_table['*'] = 1;
    h11_uri_table['+'] = 1;
    h11_uri_table[','] = 1;
    h11_uri_table[';'] = 1;
    h11_uri_table['='] = 1;

    /* Gen-delims used in paths */
    h11_uri_table[':'] = 1;
    h11_uri_table['@'] = 1;
    h11_uri_table['/'] = 1;

    /* Percent for percent-encoding (validated specially) */
    h11_uri_table['%'] = 1;
}
```

---

## Part 8: Edge Cases

### 1. Empty Target
```
GET  HTTP/1.1\r\n
```
**Error**: `H11_ERR_INVALID_TARGET`

### 2. Fragment in Target
```
GET /path#fragment HTTP/1.1\r\n
```
**Error**: `H11_ERR_INVALID_TARGET` (fragments never allowed)

### 3. Space in Target
```
GET /path with space HTTP/1.1\r\n
```
**Error**: `H11_ERR_INVALID_TARGET`

### 4. Invalid Percent-Encoding
```
GET /path%GG HTTP/1.1\r\n
```
**Error**: `H11_ERR_INVALID_TARGET`

### 5. Incomplete Percent-Encoding
```
GET /path%2 HTTP/1.1\r\n
```
**Error**: `H11_ERR_INVALID_TARGET`

### 6. Missing Path in Origin Form
```
GET ? HTTP/1.1\r\n
```
**Error**: `H11_ERR_INVALID_TARGET` (path required before query)

### 7. Authority Without Port
```
CONNECT example.com HTTP/1.1\r\n
```
**Error**: `H11_ERR_INVALID_TARGET` (port required for authority-form)

### 8. Invalid Port Number
```
CONNECT example.com:99999 HTTP/1.1\r\n
```
**Error**: `H11_ERR_INVALID_TARGET` (port > 65535)

### 9. IPv6 Authority
```
CONNECT [::1]:8080 HTTP/1.1\r\n
```
**Success**: Valid authority-form with IPv6

### 10. Absolute Form Without Authority
```
GET http:/path HTTP/1.1\r\n
```
**Error**: `H11_ERR_INVALID_TARGET` (missing //)

---

## Implementation Checklist

- [ ] Implement h11_determine_target_form()
- [ ] Implement validate_origin_form()
- [ ] Implement validate_absolute_form()
- [ ] Implement validate_authority_form()
- [ ] Implement validate_host() for IPv4/IPv6/reg-name
- [ ] Build URI character table
- [ ] Validate percent-encoding sequences
- [ ] Reject fragments in all forms
- [ ] Validate method-form compatibility
- [ ] Handle IPv6 addresses in brackets
- [ ] Validate port numbers
- [ ] Test all edge cases
