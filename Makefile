CC      := gcc
CFLAGS  := -O3 -march=native -mavx512f -mavx512bw -Wall -Wextra -Werror
LDFLAGS :=

TARGET  := http_scan
SRC     := http_scan.c

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test: $(TARGET)
	./$(TARGET) sample_requests/01_simple_get.txt
	./$(TARGET) sample_requests/04_post_json.txt

clean:
	rm -f $(TARGET)
