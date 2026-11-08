# Host build of the protocol/decoding core, for testing without an iPad.
#   ../fetch_dlpspec.sh && make test
CC      ?= cc
SPEC    := ../ThirdParty/dlpspec
CFLAGS  += -std=gnu11 -O1 -g -Wall -Wextra -I. -I$(SPEC)
SPECFLAGS := -std=gnu11 -O1 -g -w -I$(SPEC)

SPEC_SRC := $(wildcard $(SPEC)/*.c)
SPEC_OBJ := $(patsubst $(SPEC)/%.c,build/spec_%.o,$(SPEC_SRC))

build/%.o: %.c dfcore.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/spec_%.o: $(SPEC)/%.c | build
	$(CC) $(SPECFLAGS) -c $< -o $@

build/test_dfcore: build/dfcore.o build/test_dfcore.o $(SPEC_OBJ)
	$(CC) $^ -lm -o $@

build/test_dfcore.o: test/test_dfcore.c dfcore.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

test: build/test_dfcore
	./build/test_dfcore

clean:
	rm -rf build

.PHONY: test clean
