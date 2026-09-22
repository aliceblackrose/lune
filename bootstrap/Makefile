CC ?= cc
CFLAGS ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -Werror

BUILD := build
COMMON := lexer.c ast.c parser.c

.PHONY: all test test-syntax clean

all: $(BUILD)/lune-bootstrap

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/lune-bootstrap: main.c $(COMMON) lexer.h ast.h parser.h | $(BUILD)
	$(CC) $(CFLAGS) main.c $(COMMON) -o $@

$(BUILD)/test-lexer: test_lexer.c lexer.c lexer.h | $(BUILD)
	$(CC) $(CFLAGS) test_lexer.c lexer.c -o $@

$(BUILD)/test-parser: test_parser.c $(COMMON) lexer.h ast.h parser.h | $(BUILD)
	$(CC) $(CFLAGS) test_parser.c $(COMMON) -o $@

test-syntax: $(BUILD)/lune-bootstrap
	@for f in ../tests/syntax/valid/*.lune; do echo "check $$f"; ./$(BUILD)/lune-bootstrap check "$$f" || exit 1; done
	@for f in ../tests/syntax/invalid/*.lune; do echo "reject $$f"; if ./$(BUILD)/lune-bootstrap check "$$f" >/dev/null 2>&1; then echo "FAIL: expected $$f to fail"; exit 1; fi; done

test: $(BUILD)/test-lexer $(BUILD)/test-parser test-syntax
	./$(BUILD)/test-lexer
	./$(BUILD)/test-parser

clean:
	rm -rf $(BUILD)
