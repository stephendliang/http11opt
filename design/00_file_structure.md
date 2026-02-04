# HTTP/1.1 Parser - File Structure

## Overview

This document describes the complete file structure for the h11 HTTP/1.1 parser implementation. The design prioritizes:
- Minimal file count for simplicity
- Clear separation between public API and internal implementation
- **SIMD-first scanning as the core parsing primitive**
- Zero external dependencies beyond libc

## Directory Layout

```
h11/
├── h11.h            # Public API
├── h11_internal.h   # Internal declarations
├── parser.c         # State machine, parsing logic, SIMD-accelerated scanners
├── util.c           # Character tables and utilities
└── Makefile         # Build configuration
```

**Note**: There is no separate `simd.c`. The SIMD scanning functions are the parser's core operations and live in `parser.c` where they are used. Separating them would create a false abstraction.

---

## File: h11.h (Public API)

**Purpose**: Single header that users include. Contains all public types, enums, configuration structures, and function declarations.

### Contents

```c
#ifndef H11_H
#define H11_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Version Information
 *===========================================================================*/
#define H11_VERSION_MAJOR 1
#define H11_VERSION_MINOR 0
#define H11_VERSION_PATCH 0

/*============================================================================
 * Error Codes
 *===========================================================================*/
typedef enum h11_error {
    H11_OK = 0,
    H11_NEED_MORE_DATA,          /* Incomplete input, call again with more */

    /* Request line errors */
    H11_ERR_INVALID_METHOD,
    H11_ERR_INVALID_TARGET,
    H11_ERR_INVALID_VERSION,
    H11_ERR_REQUEST_LINE_TOO_LONG,
    H11_ERR_INVALID_CRLF,

    /* Header errors */
    H11_ERR_INVALID_HEADER_NAME,
    H11_ERR_INVALID_HEADER_VALUE,
    H11_ERR_HEADER_LINE_TOO_LONG,
    H11_ERR_TOO_MANY_HEADERS,
    H11_ERR_HEADERS_TOO_LARGE,
    H11_ERR_OBS_FOLD_REJECTED,
    H11_ERR_LEADING_WHITESPACE,

    /* Semantic errors */
    H11_ERR_MISSING_HOST,
    H11_ERR_MULTIPLE_HOST,
    H11_ERR_INVALID_HOST,
    H11_ERR_INVALID_CONTENT_LENGTH,
    H11_ERR_MULTIPLE_CONTENT_LENGTH,
    H11_ERR_CONTENT_LENGTH_OVERFLOW,
    H11_ERR_INVALID_TRANSFER_ENCODING,
    H11_ERR_TE_NOT_CHUNKED_FINAL,
    H11_ERR_TE_CL_CONFLICT,
    H11_ERR_UNKNOWN_TRANSFER_CODING,

    /* Body/chunked errors */
    H11_ERR_BODY_TOO_LARGE,
    H11_ERR_INVALID_CHUNK_SIZE,
    H11_ERR_CHUNK_SIZE_OVERFLOW,
    H11_ERR_INVALID_CHUNK_EXT,
    H11_ERR_CHUNK_EXT_TOO_LONG,
    H11_ERR_INVALID_CHUNK_DATA,
    H11_ERR_INVALID_TRAILER,

    /* Connection errors */
    H11_ERR_CONNECTION_CLOSED,
    H11_ERR_INTERNAL
} h11_error_t;

/*============================================================================
 * Parser States
 *===========================================================================*/
typedef enum h11_state {
    H11_STATE_IDLE,              /* Ready for new request */
    H11_STATE_REQUEST_LINE,      /* Parsing request line */
    H11_STATE_HEADERS,           /* Parsing header fields */
    H11_STATE_BODY_IDENTITY,     /* Reading Content-Length body */
    H11_STATE_BODY_CHUNKED_SIZE, /* Reading chunk size line */
    H11_STATE_BODY_CHUNKED_DATA, /* Reading chunk data */
    H11_STATE_BODY_CHUNKED_CRLF, /* Expecting CRLF after chunk */
    H11_STATE_TRAILERS,          /* Parsing trailer fields */
    H11_STATE_COMPLETE,          /* Request fully parsed */
    H11_STATE_ERROR              /* Unrecoverable error */
} h11_state_t;

/*============================================================================
 * Request Target Forms (RFC 9112 Section 3.2)
 *===========================================================================*/
typedef enum h11_target_form {
    H11_TARGET_ORIGIN,           /* /path?query */
    H11_TARGET_ABSOLUTE,         /* http://host/path */
    H11_TARGET_AUTHORITY,        /* host:port (CONNECT) */
    H11_TARGET_ASTERISK          /* * (OPTIONS) */
} h11_target_form_t;

/*============================================================================
 * Body Framing Type
 *===========================================================================*/
typedef enum h11_body_type {
    H11_BODY_NONE,               /* No body */
    H11_BODY_CONTENT_LENGTH,     /* Content-Length specified */
    H11_BODY_CHUNKED             /* Transfer-Encoding: chunked */
} h11_body_type_t;

/*============================================================================
 * String Slice (zero-copy reference into buffer)
 *===========================================================================*/
typedef struct h11_slice {
    const char *data;
    size_t len;
} h11_slice_t;

/*============================================================================
 * Header Field
 *===========================================================================*/
typedef struct h11_header {
    h11_slice_t name;
    h11_slice_t value;
} h11_header_t;

/*============================================================================
 * Configuration
 *===========================================================================*/
typedef struct h11_config {
    /* Size limits */
    size_t max_request_line_len;    /* Default: 8192 */
    size_t max_header_line_len;     /* Default: 8192 */
    size_t max_headers_size;        /* Default: 65536 */
    size_t max_header_count;        /* Default: 100 */
    size_t max_body_size;           /* Default: SIZE_MAX (unlimited) */
    size_t max_chunk_ext_len;       /* Default: 1024 */

    /* Tolerance flags */
    bool strict_crlf;               /* Reject bare LF (default: true) */
    bool reject_obs_fold;           /* Reject obs-fold (default: true) */
    bool allow_obs_text;            /* Allow 0x80-0xFF in values (default: true) */
    bool allow_leading_crlf;        /* Ignore leading empty lines (default: true) */
    bool tolerate_spaces;           /* Allow lax SP handling (default: false) */

    /* Conflict handling */
    bool reject_te_cl_conflict;     /* Reject TE+CL (default: true) */
} h11_config_t;

/*============================================================================
 * Parsed Request (output structure)
 *===========================================================================*/
typedef struct h11_request {
    /* Request line */
    h11_slice_t method;
    h11_slice_t target;
    h11_target_form_t target_form;
    uint8_t version_major;
    uint8_t version_minor;

    /* Headers (array of h11_header_t) */
    h11_header_t *headers;
    size_t header_count;
    size_t header_capacity;

    /* Key semantic headers (indices into headers array, -1 if not present) */
    int host_index;
    int content_length_index;
    int transfer_encoding_index;
    int connection_index;
    int expect_index;

    /* Body framing */
    h11_body_type_t body_type;
    uint64_t content_length;        /* Valid if body_type == H11_BODY_CONTENT_LENGTH */

    /* Connection semantics */
    bool keep_alive;
    bool expect_continue;
    bool has_upgrade;

    /* Trailers (populated after chunked body) */
    h11_header_t *trailers;
    size_t trailer_count;
    size_t trailer_capacity;
} h11_request_t;

/*============================================================================
 * Parser Context
 *===========================================================================*/
typedef struct h11_parser h11_parser_t;

/*============================================================================
 * Public API Functions
 *===========================================================================*/

/* Initialization */
h11_config_t h11_config_default(void);
h11_parser_t *h11_parser_new(const h11_config_t *config);
void h11_parser_free(h11_parser_t *parser);
void h11_parser_reset(h11_parser_t *parser);

/* Parsing */
h11_error_t h11_parse(h11_parser_t *parser, const char *data, size_t len,
                      size_t *consumed);

/* State inspection */
h11_state_t h11_get_state(const h11_parser_t *parser);
const h11_request_t *h11_get_request(const h11_parser_t *parser);

/* Body reading */
h11_error_t h11_read_body(h11_parser_t *parser, const char *data, size_t len,
                          size_t *consumed, const char **body_out, size_t *body_len);

/* Error handling */
const char *h11_error_name(h11_error_t error);
const char *h11_error_message(h11_error_t error);
size_t h11_error_offset(const h11_parser_t *parser);

/* Utility */
bool h11_header_name_eq(h11_slice_t name, const char *cmp);
int h11_find_header(const h11_request_t *req, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* H11_H */
```

---

## File: h11_internal.h (Internal Declarations)

**Purpose**: Shared internal types, macros, and declarations. Includes SIMD capability level as core parser state.

### Contents

```c
#ifndef H11_INTERNAL_H
#define H11_INTERNAL_H

#include "h11.h"

/*============================================================================
 * Compiler Attributes
 *===========================================================================*/
#ifdef __GNUC__
#define H11_LIKELY(x)   __builtin_expect(!!(x), 1)
#define H11_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define H11_INLINE      static inline __attribute__((always_inline))
#define H11_NOINLINE    __attribute__((noinline))
#else
#define H11_LIKELY(x)   (x)
#define H11_UNLIKELY(x) (x)
#define H11_INLINE      static inline
#define H11_NOINLINE
#endif

/*============================================================================
 * SIMD Capability Level (detected at startup)
 *===========================================================================*/
typedef enum {
    H11_SIMD_SCALAR = 0,    /* No SIMD, pure C fallback */
    H11_SIMD_SSE42  = 1,    /* SSE4.2 (16-byte vectors) */
    H11_SIMD_AVX2   = 2,    /* AVX2 (32-byte vectors) */
    H11_SIMD_AVX512 = 3     /* AVX-512BW (64-byte vectors) */
} h11_simd_level_t;

/* Global SIMD level, set once at process start */
extern h11_simd_level_t h11_simd_level;

/* Initialize SIMD detection (called automatically) */
void h11_init(void);

/*============================================================================
 * Character Classification Tables (defined in util.c)
 *===========================================================================*/
extern const uint8_t h11_tchar_table[256];    /* Token characters */
extern const uint8_t h11_vchar_table[256];    /* Visible + SP + HTAB + obs-text */
extern const uint8_t h11_digit_table[256];    /* 0-9 */
extern const uint8_t h11_hexdig_table[256];   /* 0-9, A-F, a-f */
extern const uint8_t h11_uri_table[256];      /* Valid URI characters */

/* Character class macros */
#define H11_IS_TCHAR(c)   (h11_tchar_table[(uint8_t)(c)])
#define H11_IS_VCHAR(c)   (h11_vchar_table[(uint8_t)(c)])
#define H11_IS_DIGIT(c)   (h11_digit_table[(uint8_t)(c)])
#define H11_IS_HEXDIG(c)  (h11_hexdig_table[(uint8_t)(c)])
#define H11_IS_URI(c)     (h11_uri_table[(uint8_t)(c)])
#define H11_IS_SP(c)      ((c) == ' ')
#define H11_IS_HTAB(c)    ((c) == '\t')
#define H11_IS_OWS(c)     ((c) == ' ' || (c) == '\t')
#define H11_IS_CR(c)      ((c) == '\r')
#define H11_IS_LF(c)      ((c) == '\n')

/*============================================================================
 * Parser Structure (internal definition)
 *===========================================================================*/
struct h11_parser {
    /* Configuration */
    h11_config_t config;

    /* Current state */
    h11_state_t state;
    h11_error_t last_error;
    size_t error_offset;

    /* Current request being parsed */
    h11_request_t request;

    /* Parsing position tracking */
    size_t total_consumed;      /* Total bytes consumed this request */
    size_t line_start;          /* Start of current line */
    size_t headers_size;        /* Accumulated header bytes */

    /* Body parsing state */
    uint64_t body_remaining;    /* Bytes remaining in current body/chunk */
    uint64_t total_body_read;   /* Total body bytes read */

    /* Chunk parsing state */
    bool in_chunk_ext;          /* Currently parsing chunk extension */
    size_t chunk_ext_len;       /* Current chunk extension length */

    /* Flags */
    bool seen_host;
    bool seen_content_length;
    bool seen_transfer_encoding;
    bool is_chunked;
    bool leading_crlf_consumed;
};

/*============================================================================
 * Utility Functions (defined in util.c)
 *===========================================================================*/

/* String comparison (case-insensitive for header names) */
bool h11_slice_eq_case(h11_slice_t a, const char *b, size_t blen);

/* Hex digit value */
int h11_hexval(char c);

#endif /* H11_INTERNAL_H */
```

---

## File: parser.c (Core Parser with Integrated SIMD)

**Purpose**: Contains ALL parsing logic including the SIMD-accelerated scanning primitives that are fundamental to the parser's operation. The SIMD scanners are not helpers - they ARE the parser.

### Structure

```c
/*============================================================================
 * SIMD SCANNING CORE
 *
 * These functions are the heart of the parser. Every parsing operation
 * begins with finding a delimiter (CRLF for lines, colon for headers).
 * The SIMD implementations are not optimizations bolted onto a scalar
 * parser - they ARE the parser's fundamental operations.
 *===========================================================================*/

#include "h11.h"
#include "h11_internal.h"

#include <stdlib.h>
#include <string.h>

#ifdef __x86_64__
#include <cpuid.h>
#ifdef __AVX512BW__
#include <immintrin.h>
#endif
#ifdef __AVX2__
#include <immintrin.h>
#endif
#ifdef __SSE4_2__
#include <nmmintrin.h>
#endif
#endif

/* Global SIMD capability level */
h11_simd_level_t h11_simd_level = H11_SIMD_SCALAR;
static bool h11_initialized = false;

/*----------------------------------------------------------------------------
 * CPU Detection (runs once at startup)
 *---------------------------------------------------------------------------*/
void h11_init(void) { ... }

/*----------------------------------------------------------------------------
 * CRLF Scanner - The Fundamental Parsing Primitive
 *
 * This is the most critical function in the entire parser. Every line-based
 * parse (request line, headers, chunk sizes) depends on this.
 *---------------------------------------------------------------------------*/
static ssize_t find_crlf(const char *data, size_t len) { ... }

/*----------------------------------------------------------------------------
 * Character Scanner - For delimiter detection
 *---------------------------------------------------------------------------*/
static ssize_t find_char(const char *data, size_t len, char c) { ... }

/*============================================================================
 * PARSER STATE MACHINE
 *===========================================================================*/

/* Uses find_crlf() and find_char() throughout */

h11_error_t h11_parse_request_line(...) {
    ssize_t crlf = find_crlf(data, len);  /* SIMD scan */
    ...
}

h11_error_t h11_parse_headers(...) {
    ssize_t crlf = find_crlf(data, len);  /* SIMD scan */
    ssize_t colon = find_char(line, line_len, ':');  /* SIMD scan */
    ...
}

h11_error_t h11_parse_chunk_size(...) {
    ssize_t crlf = find_crlf(data, len);  /* SIMD scan */
    ...
}
```

---

## File: util.c (Utilities)

**Purpose**: Character classification tables and string utilities.

### Key Sections

1. **Character Tables** - 256-byte lookup tables for tchar, vchar, digit, hexdig, URI chars
2. **Error Messages** - Human-readable error strings
3. **String Utilities** - Case-insensitive comparison, hex conversion

---

## File: Makefile

**Purpose**: Build configuration with SIMD as mandatory, not optional.

### Contents

```makefile
CC      ?= gcc
CFLAGS  := -Wall -Wextra -Werror -std=c11 -fPIC -O3

# SIMD is not optional - these flags enable the compiler to use
# vector instructions that are fundamental to parser operation
CFLAGS  += -march=native

# For cross-compilation or specific targets:
# CFLAGS += -mavx2        # Minimum recommended
# CFLAGS += -mavx512bw    # Best performance

# Build type
DEBUG   ?= 0
ifeq ($(DEBUG),1)
    CFLAGS := $(filter-out -O3,$(CFLAGS)) -g -O0 -DDEBUG
endif

# Sources (note: no separate simd.c - it's part of parser.c)
SRCS    := parser.c util.c
OBJS    := $(SRCS:.c=.o)
HEADERS := h11.h h11_internal.h

# Targets
LIB_STATIC := libh11.a
LIB_SHARED := libh11.so

.PHONY: all clean static shared

all: static shared

static: $(LIB_STATIC)

shared: $(LIB_SHARED)

$(LIB_STATIC): $(OBJS)
	ar rcs $@ $^

$(LIB_SHARED): $(OBJS)
	$(CC) -shared -o $@ $^ $(LDFLAGS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(LIB_STATIC) $(LIB_SHARED)
```

---

## Implementation Order

Files should be implemented in this order:

1. **util.c** - No dependencies, provides character tables
2. **h11.h** - Public types
3. **h11_internal.h** - Internal types including SIMD level
4. **parser.c** - Everything: CPU detection, SIMD scanners, state machine, all parsing

---

## Memory Layout

```
h11_parser_t
├── config (h11_config_t)           ~48 bytes
├── state/error/offset              ~16 bytes
├── request (h11_request_t)
│   ├── slices (method, target)     ~48 bytes
│   ├── headers array pointer        ~8 bytes
│   ├── header indices               ~20 bytes
│   ├── body info                    ~24 bytes
│   └── trailers                     ~24 bytes
└── parsing state                    ~64 bytes
─────────────────────────────────────
Total parser context:              ~250 bytes (excluding dynamic allocations)
```

Headers and trailers are dynamically allocated arrays that grow as needed.
