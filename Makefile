CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -D_GNU_SOURCE -Isrc
LDFLAGS = -pthread
SRC     = $(filter-out src/main.c, $(wildcard src/*.c))
HDR     = $(wildcard src/*.h)

all: httpd

# Headers are listed as prerequisites so editing a .h triggers a rebuild,
# but only the .c files are passed to the compiler.
httpd: src/main.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS)

asan: CFLAGS  += -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
asan: LDFLAGS += -fsanitize=address,undefined
asan: clean httpd

tsan: CFLAGS  += -g -O1 -fsanitize=thread
tsan: LDFLAGS += -fsanitize=thread
tsan: clean httpd

test: $(SRC) $(HDR) tests/test_http.c
	$(CC) $(CFLAGS) $(filter %.c,$^) -o test_http $(LDFLAGS) && ./test_http

clean:
	rm -f httpd test_http

.PHONY: all asan tsan test clean
