# Override CC, CFLAGS, etc. via environment or local.mk
-include local.mk

CC      ?= cc
CFLAGS  ?= -std=c11 -O3 -Wall -Wextra -Werror -pedantic
HEADERS := h11_types.h h11.h h11_internal.h

# Library
LIB_SRCS := util.c
LIB_OBJS := $(LIB_SRCS:.c=.o)

libh11.a: $(LIB_OBJS)
	ar rcs $@ $^

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Tests
test_util: test_util.c util.o $(HEADERS)
	$(CC) $(CFLAGS) -o $@ test_util.c util.o

test: test_util
	./test_util

# Legacy http_scan target (separate flags for SIMD)
SCAN_CFLAGS := -O3 -march=native -mavx512f -mavx512bw -Wall -Wextra -Werror

http_scan: http_scan.c
	$(CC) $(SCAN_CFLAGS) -o $@ $<

.PHONY: all test clean
all: libh11.a

clean:
	rm -f $(LIB_OBJS) libh11.a test_util http_scan
