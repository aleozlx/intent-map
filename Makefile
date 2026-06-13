# intent-map — single-binary SQLite-backed symbol→meaning store.
# See DESIGN-intent-map.md for the full design.

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
# Link the system SQLite (must include FTS5; most distro builds do).
SQLITE_CFLAGS := $(shell pkg-config --cflags sqlite3 2>/dev/null)
SQLITE_LIBS   := $(shell pkg-config --libs sqlite3 2>/dev/null || echo -lsqlite3)

BIN := intent-map
SRC := src/intent-map.c

.PHONY: all build run test clean

all: build

build: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(SQLITE_CFLAGS) -o $@ $(SRC) $(SQLITE_LIBS)

# Run the binary: `make run ARGS="search foo"`
run: $(BIN)
	./$(BIN) $(ARGS)

# CI gate: build the binary, then drive it as a black box from pytest.
test: $(BIN)
	pytest -q tests

clean:
	rm -f $(BIN)
