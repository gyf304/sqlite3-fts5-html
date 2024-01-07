EXT = .so
CFLAGS ?= -Os -Wall -Wextra -Werror

ifeq ($(OS),Windows_NT)
	EXT = .dll
else
	UNAME_S := $(shell uname -s)
	ifeq ($(UNAME_S),Darwin)
		EXT = .dylib
	endif
endif

.PHONY: all clean test

all: fts5html$(EXT) htmlentity.h

clean:
	rm -f fts5html$(EXT)
	rm -rf fts5html$(EXT).dSYM
	rm -f test.sql.*

fts5html$(EXT): fts5html.c
	$(CC) $(CFLAGS) -g -shared -fPIC -o $@ $<

test.sql.expected: test.sql
	sed -rn 's/.*-- expected: (.*)$$/\1/p' $< > $@

test.sql.output: fts5html$(EXT) test.sql
	sqlite3 :memory: ".load ./fts5html$(EXT)" ".read test.sql" .exit > $@

test: test.sql.expected test.sql.output
	diff -u test.sql.expected test.sql.output
