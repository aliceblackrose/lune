CC ?= cc
CFLAGS ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -Werror

BUILD := build
COMMON := lexer.c

.PHONY: all test clean

all: $(BUILD)/lune-bootstrap

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/lune-bootstrap: main.c $(COMMON) lexer.h | $(BUILD)
	$(CC) $(CFLAGS) main.c $(COMMON) -o $@

$(BUILD)/test-lexer: test_lexer.c $(COMMON) lexer.h | $(BUILD)
	$(CC) $(CFLAGS) test_lexer.c $(COMMON) -o $@

test: $(BUILD)/test-lexer
	./$(BUILD)/test-lexer

clean:
	rm -rf $(BUILD)
