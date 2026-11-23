CC      ?= cc
CFLAGS  ?= -O2 -g
override CFLAGS += -std=c11 -Wall -Wextra -Werror -Iinclude -Isrc
BUILD   ?= build

SRC   := $(wildcard src/*.c src/kernels/*.c src/front/*.c)
OBJ   := $(SRC:src/%.c=$(BUILD)/obj/%.o)
TSRC  := $(wildcard tests/*.c)
TOBJ  := $(TSRC:tests/%.c=$(BUILD)/obj/tests/%.o)
HDR   := $(wildcard include/quad/*.h) $(wildcard src/front/*.h) $(wildcard tests/*.h) tests/tests.def

.PHONY: all test asan clean

all: $(BUILD)/libquad.a $(BUILD)/test $(BUILD)/quad

$(BUILD)/obj/%.o: src/%.c $(HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/tests/%.o: tests/%.c $(HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests -c $< -o $@

$(BUILD)/libquad.a: $(OBJ)
	ar rcs $@ $^

$(BUILD)/test: $(TOBJ) $(BUILD)/libquad.a
	$(CC) $(CFLAGS) $(TOBJ) $(BUILD)/libquad.a -o $@

# The command-line tool: quad check, quad ir, quad run.
$(BUILD)/quad: tools/quad.c $(BUILD)/libquad.a $(HDR)
	$(CC) $(CFLAGS) tools/quad.c $(BUILD)/libquad.a -o $@

test: $(BUILD)/test
	./$(BUILD)/test

# The same tests with the address and undefined-behaviour sanitizers.
asan:
	$(MAKE) BUILD=build/asan CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" test

clean:
	rm -rf build
