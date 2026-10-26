CC ?= cc
PYTHON ?= python3
CFLAGS := -O2 -std=c11 -Wall -Wextra -Werror
SOURCES := plugin.c settings.c events.c json.c rewrite.c timezone.c platform.c
HEADERS := settings.h events.h json.h rewrite.h timezone.h platform.h dashboard_bundle.h

ifeq ($(OS),Windows_NT)
LIB := codex-timezone-sync-local.dll
LDFLAGS := -shared -static-libgcc -lws2_32
else
CFLAGS += -fPIC -fstack-protector-strong
ifeq ($(shell uname -s),Darwin)
LIB := codex-timezone-sync-local.dylib
# dladdr lives in libSystem on macOS.
LDFLAGS := -dynamiclib -pthread
else
LIB := codex-timezone-sync-local.so
# glibc 2.34+ ships dladdr in libc, but older toolchains still need libdl.
LDFLAGS := -pthread -shared -ldl
endif
endif

all: $(LIB)

dashboard_bundle.h: dashboard.html embed_ui.py
	$(PYTHON) embed_ui.py

$(LIB): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LDFLAGS)

test: all
	$(PYTHON) test_plugin.py

# Validates the committed timezone table against the reference tzdata database.
test-tz:
	$(PYTHON) test_timezone_rules.py

clean:
	rm -f codex-timezone-sync-local.so codex-timezone-sync-local.dylib codex-timezone-sync-local.dll dashboard_bundle.h

.PHONY: all clean test test-tz
