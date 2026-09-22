CC ?= cc
CFLAGS ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -Werror

BUILD := build
FRONTEND := lexer.c ast.c parser.c
RUNTIME := value.c object.c bytecode.c bytecode_image.c compiler.c platform.c vm.c
COMMON := $(FRONTEND) $(RUNTIME)

.PHONY: all test test-syntax test-examples test-selfhost test-bootstrap bench bench-build clean

all: $(BUILD)/lune-bootstrap

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/lune-bootstrap: main.c $(COMMON) lexer.h ast.h parser.h value.h object.h bytecode.h bytecode_image.h compiler.h platform.h vm.h | $(BUILD)
	$(CC) $(CFLAGS) main.c $(COMMON) -o $@

$(BUILD)/test-lexer: test_lexer.c lexer.c lexer.h | $(BUILD)
	$(CC) $(CFLAGS) test_lexer.c lexer.c -o $@

$(BUILD)/test-parser: test_parser.c $(FRONTEND) lexer.h ast.h parser.h | $(BUILD)
	$(CC) $(CFLAGS) test_parser.c $(FRONTEND) -o $@

$(BUILD)/test-vm: test_vm.c $(COMMON) lexer.h ast.h parser.h value.h object.h bytecode.h bytecode_image.h compiler.h platform.h vm.h | $(BUILD)
	$(CC) $(CFLAGS) test_vm.c $(COMMON) -o $@

$(BUILD)/bench: ../bench/bench.c $(COMMON) lexer.h ast.h parser.h value.h object.h bytecode.h bytecode_image.h compiler.h platform.h vm.h | $(BUILD)
	$(CC) $(CFLAGS) -I. ../bench/bench.c $(COMMON) -o $@

bench-build: $(BUILD)/bench

bench: $(BUILD)/bench
	./$(BUILD)/bench

test-syntax: $(BUILD)/lune-bootstrap
	@for f in ../tests/syntax/valid/*.lune; do echo "check $$$f"; ./$(BUILD)/lune-bootstrap check "$$$f" || exit 1; done
	@for f in ../tests/syntax/invalid/*.lune; do echo "reject $$$f"; if ./$(BUILD)/lune-bootstrap check "$$$f" >/dev/null 2>&1; then echo "FAIL: expected $$$f to fail"; exit 1; fi; done

test-examples: $(BUILD)/lune-bootstrap
	@for f in ../examples/*.lune; do echo "run $$$f"; ./$(BUILD)/lune-bootstrap run "$$$f" || exit 1; done

test-selfhost: $(BUILD)/lune-bootstrap
	./$(BUILD)/lune-bootstrap run ../compiler/test_lexer.lune
	./$(BUILD)/lune-bootstrap run ../compiler/test_parser.lune
	./$(BUILD)/lune-bootstrap run ../compiler/test_compiler.lune
	@for f in ../tests/syntax/valid/*.lune; do echo "selfhost check $$f"; ./$(BUILD)/lune-bootstrap run ../compiler/check_parser.lune "$$f" || exit 1; done
	@for f in ../tests/syntax/invalid/*.lune; do echo "selfhost reject $$f"; if ./$(BUILD)/lune-bootstrap run ../compiler/check_parser.lune "$$f" >/dev/null 2>&1; then echo "FAIL: self-host parser accepted $$f"; exit 1; fi; done

test-bootstrap: $(BUILD)/lune-bootstrap
	rm -rf $(BUILD)/selfhost
	mkdir -p $(BUILD)/selfhost
	./$(BUILD)/lune-bootstrap run ../compiler/stage.lune ../compiler/lexer.lune $(BUILD)/selfhost/lexer.lbc module
	./$(BUILD)/lune-bootstrap run ../compiler/stage.lune ../compiler/parser.lune $(BUILD)/selfhost/parser.lbc module
	./$(BUILD)/lune-bootstrap run ../compiler/stage.lune ../compiler/compiler.lune $(BUILD)/selfhost/compiler.lbc module
	./$(BUILD)/lune-bootstrap run ../compiler/stage.lune ../compiler/bytecode.lune $(BUILD)/selfhost/bytecode.lbc module
	./$(BUILD)/lune-bootstrap run ../compiler/stage.lune ../compiler/stage.lune $(BUILD)/selfhost/stage.lbc
	./$(BUILD)/lune-bootstrap runbc $(BUILD)/selfhost/stage.lbc ../compiler/lexer.lune $(BUILD)/selfhost/lexer2.lbc module
	./$(BUILD)/lune-bootstrap runbc $(BUILD)/selfhost/stage.lbc ../compiler/parser.lune $(BUILD)/selfhost/parser2.lbc module
	./$(BUILD)/lune-bootstrap runbc $(BUILD)/selfhost/stage.lbc ../compiler/compiler.lune $(BUILD)/selfhost/compiler2.lbc module
	./$(BUILD)/lune-bootstrap runbc $(BUILD)/selfhost/stage.lbc ../compiler/bytecode.lune $(BUILD)/selfhost/bytecode2.lbc module
	./$(BUILD)/lune-bootstrap runbc $(BUILD)/selfhost/stage.lbc ../compiler/stage.lune $(BUILD)/selfhost/stage2.lbc
	cmp $(BUILD)/selfhost/lexer.lbc $(BUILD)/selfhost/lexer2.lbc
	cmp $(BUILD)/selfhost/parser.lbc $(BUILD)/selfhost/parser2.lbc
	cmp $(BUILD)/selfhost/compiler.lbc $(BUILD)/selfhost/compiler2.lbc
	cmp $(BUILD)/selfhost/bytecode.lbc $(BUILD)/selfhost/bytecode2.lbc
	cmp $(BUILD)/selfhost/stage.lbc $(BUILD)/selfhost/stage2.lbc
	./$(BUILD)/lune-bootstrap runbc $(BUILD)/selfhost/stage.lbc ../examples/hello.lune $(BUILD)/selfhost/hello.lbc
	./$(BUILD)/lune-bootstrap runbc $(BUILD)/selfhost/hello.lbc

test: $(BUILD)/test-lexer $(BUILD)/test-parser $(BUILD)/test-vm test-syntax test-examples test-selfhost test-bootstrap
	./$(BUILD)/test-lexer
	./$(BUILD)/test-parser
	./$(BUILD)/test-vm

clean:
	rm -rf $(BUILD)
