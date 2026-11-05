CC ?= cc
CPPFLAGS += -Iinclude
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wformat=2 -pthread
LDLIBS += -lmicrohttpd -lsqlite3 -ljson-c -lcrypto -pthread
SOURCES = src/main.c src/db.c src/util.c src/service.c src/auth.c src/rides.c src/admin.c

.PHONY: all clean test sanitize
all: build/ride_server

build/ride_server: $(SOURCES) include/app.h src/schema.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

test: all
	python3 -m unittest discover -s tests -v

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O1 -g -std=c11 -Wall -Wextra -pthread -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'

clean:
	rm -f build/ride_server
