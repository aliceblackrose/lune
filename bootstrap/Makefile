CC ?= cc
CFLAGS ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -Werror

BUILD := build
SELFHOST := $(BUILD)/selfhost
FRONTEND := lexer.c ast.c parser.c
BOOTSTRAP_COMPILER := compiler.c native_compile.c
CORE := value.c object.c bytecode.c bytecode_image.c platform.c vm.c
COMMON := $(FRONTEND) $(BOOTSTRAP_COMPILER) $(CORE)
SELFHOST_SOURCES := \
	../compiler/lexer.lune \
	../compiler/parser.lune \
	../compiler/compiler.lune \
	../compiler/bytecode.lune \
	../compiler/formatter.lune \
	../compiler/fmt.lune \
	../compiler/stage.lune

.PHONY: all bootstrap selfhost-images test test-syntax test-examples test-selfhost test-bootstrap test-production bench bench-build clean

all: $(BUILD)/lune

bootstrap: $(BUILD)/lune-bootstrap

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/lune-bootstrap: main.c $(COMMON) source.h lexer.h ast.h parser.h value.h object.h bytecode.h bytecode_image.h compiler.h native_compile.h platform.h vm.h | $(BUILD)
	$(CC) $(CFLAGS) main.c $(COMMON) -o $@

$(SELFHOST)/.stamp: $(BUILD)/lune-bootstrap $(SELFHOST_SOURCES)
	rm -rf $(SELFHOST)
	mkdir -p $(SELFHOST)
	./$(BUILD)/lune-bootstrap run ../compiler/stage.lune ../compiler/lexer.lune $(SELFHOST)/lexer.lbc module
	./$(BUILD)/lune-bootstrap run ../compiler/stage.lune ../compiler/parser.lune $(SELFHOST)/parser.lbc module
	./$(BUILD)/lune-bootstrap run ../compiler/stage.lune ../compiler/compiler.lune $(SELFHOST)/compiler.lbc module
	./$(BUILD)/lune-bootstrap run ../compiler/stage.lune ../compiler/bytecode.lune $(SELFHOST)/bytecode.lbc module
	./$(BUILD)/lune-bootstrap run ../compiler/stage.lune ../compiler/formatter.lune $(SELFHOST)/formatter.lbc module
	./$(BUILD)/lune-bootstrap run ../compiler/stage.lune ../compiler/fmt.lune $(SELFHOST)/fmt.lbc
	./$(BUILD)/lune-bootstrap run ../compiler/stage.lune ../compiler/stage.lune $(SELFHOST)/stage.lbc
	touch $@

selfhost-images: $(SELFHOST)/.stamp

$(BUILD)/lune: production.c $(CORE) source.h value.h object.h bytecode.h bytecode_image.h platform.h vm.h $(SELFHOST)/.stamp | $(BUILD)
	$(CC) $(CFLAGS) production.c $(CORE) -o $@

$(BUILD)/test-lexer: test_lexer.c lexer.c lexer.h | $(BUILD)
	$(CC) $(CFLAGS) test_lexer.c lexer.c -o $@

$(BUILD)/test-parser: test_parser.c $(FRONTEND) lexer.h ast.h parser.h | $(BUILD)
	$(CC) $(CFLAGS) test_parser.c $(FRONTEND) -o $@

$(BUILD)/test-vm: test_vm.c $(COMMON) source.h lexer.h ast.h parser.h value.h object.h bytecode.h bytecode_image.h compiler.h native_compile.h platform.h vm.h | $(BUILD)
	$(CC) $(CFLAGS) test_vm.c $(COMMON) -o $@

$(BUILD)/bench: ../bench/bench.c $(COMMON) source.h lexer.h ast.h parser.h value.h object.h bytecode.h bytecode_image.h compiler.h native_compile.h platform.h vm.h | $(BUILD)
	$(CC) $(CFLAGS) -I. ../bench/bench.c $(COMMON) -o $@

bench-build: $(BUILD)/bench

bench: $(BUILD)/bench
	./$(BUILD)/bench

test-syntax: $(BUILD)/lune-bootstrap
	@for f in ../tests/syntax/valid/*.lune; do echo "check $$f"; ./$(BUILD)/lune-bootstrap check "$$f" || exit 1; done
	@for f in ../tests/syntax/invalid/*.lune; do echo "reject $$f"; if ./$(BUILD)/lune-bootstrap check "$$f" >/dev/null 2>&1; then echo "FAIL: expected $$f to fail"; exit 1; fi; done

test-examples: $(BUILD)/lune-bootstrap
	@for f in ../examples/*.lune; do echo "run $$f"; ./$(BUILD)/lune-bootstrap run "$$f" || exit 1; done

test-selfhost: $(BUILD)/lune-bootstrap
	./$(BUILD)/lune-bootstrap run ../compiler/test_lexer.lune
	./$(BUILD)/lune-bootstrap run ../compiler/test_parser.lune
	./$(BUILD)/lune-bootstrap run ../compiler/test_compiler.lune
	./$(BUILD)/lune-bootstrap run ../compiler/test_formatter.lune
	@for f in ../tests/syntax/valid/*.lune; do echo "selfhost check $$f"; ./$(BUILD)/lune-bootstrap run ../compiler/check_parser.lune "$$f" || exit 1; done
	@for f in ../tests/syntax/invalid/*.lune; do echo "selfhost reject $$f"; if ./$(BUILD)/lune-bootstrap run ../compiler/check_parser.lune "$$f" >/dev/null 2>&1; then echo "FAIL: self-host parser accepted $$f"; exit 1; fi; done

test-bootstrap: $(SELFHOST)/.stamp
	./$(BUILD)/lune-bootstrap runbc $(SELFHOST)/stage.lbc ../compiler/lexer.lune $(SELFHOST)/lexer2.lbc module
	./$(BUILD)/lune-bootstrap runbc $(SELFHOST)/stage.lbc ../compiler/parser.lune $(SELFHOST)/parser2.lbc module
	./$(BUILD)/lune-bootstrap runbc $(SELFHOST)/stage.lbc ../compiler/compiler.lune $(SELFHOST)/compiler2.lbc module
	./$(BUILD)/lune-bootstrap runbc $(SELFHOST)/stage.lbc ../compiler/bytecode.lune $(SELFHOST)/bytecode2.lbc module
	./$(BUILD)/lune-bootstrap runbc $(SELFHOST)/stage.lbc ../compiler/formatter.lune $(SELFHOST)/formatter2.lbc module
	./$(BUILD)/lune-bootstrap runbc $(SELFHOST)/stage.lbc ../compiler/fmt.lune $(SELFHOST)/fmt2.lbc
	./$(BUILD)/lune-bootstrap runbc $(SELFHOST)/stage.lbc ../compiler/stage.lune $(SELFHOST)/stage2.lbc
	cmp $(SELFHOST)/lexer.lbc $(SELFHOST)/lexer2.lbc
	cmp $(SELFHOST)/parser.lbc $(SELFHOST)/parser2.lbc
	cmp $(SELFHOST)/compiler.lbc $(SELFHOST)/compiler2.lbc
	cmp $(SELFHOST)/bytecode.lbc $(SELFHOST)/bytecode2.lbc
	cmp $(SELFHOST)/formatter.lbc $(SELFHOST)/formatter2.lbc
	cmp $(SELFHOST)/fmt.lbc $(SELFHOST)/fmt2.lbc
	cmp $(SELFHOST)/stage.lbc $(SELFHOST)/stage2.lbc
	./$(BUILD)/lune-bootstrap runbc $(SELFHOST)/stage.lbc ../examples/hello.lune $(SELFHOST)/hello.lbc
	./$(BUILD)/lune-bootstrap runbc $(SELFHOST)/hello.lbc

test-production: $(BUILD)/lune
	./$(BUILD)/lune check ../examples/hello.lune
	@if ./$(BUILD)/lune check ../tests/syntax/invalid/missing-arrow.lune >$(BUILD)/diag.out 2>$(BUILD)/diag.err; then echo "FAIL: production compiler accepted invalid syntax"; exit 1; fi
	grep -Eq '^\.\./tests/syntax/invalid/missing-arrow\.lune:[0-9]+:[0-9]+: error: ' $(BUILD)/diag.err
	grep -Fq '  double := fn(x) x * 2' $(BUILD)/diag.err
	grep -Eq '^  +\^+' $(BUILD)/diag.err
	./$(BUILD)/lune run ../examples/hello.lune
	./$(BUILD)/lune ../examples/hello.lune
	cp ../examples/hello.lune $(BUILD)/cli-hello.lune
	./$(BUILD)/lune compile $(BUILD)/cli-hello.lune
	test -f $(BUILD)/cli-hello.lbc
	./$(BUILD)/lune $(BUILD)/cli-hello.lbc
	cp ../tests/fmt/unformatted.lune $(BUILD)/fmt-case.lune
	./$(BUILD)/lune fmt $(BUILD)/fmt-case.lune
	cmp $(BUILD)/fmt-case.lune ../tests/fmt/expected.lune
	./$(BUILD)/lune fmt $(BUILD)/fmt-case.lune
	cmp $(BUILD)/fmt-case.lune ../tests/fmt/expected.lune
	./$(BUILD)/lune check $(BUILD)/fmt-case.lune
	printf '%s\n' 'make := fn(x) => {' '  fn() => x' '}' 'f := make(42)' 'f()' ':quit' | ./$(BUILD)/lune repl > $(BUILD)/repl.out
	tail -n 1 $(BUILD)/repl.out | grep -Eq '^42\r?
	@if ./$(BUILD)/lune ../tests/runtime/stack-trace.lune >$(BUILD)/trace.out 2>$(BUILD)/trace.err; then echo "FAIL: stack trace fixture unexpectedly succeeded"; exit 1; fi
	grep -Eq 'stack-trace\.lune:1:[0-9]+: error: division by zero' $(BUILD)/trace.err
	grep -Fq '  inner := fn() => 1 / 0' $(BUILD)/trace.err
	grep -Fq 'stack trace:' $(BUILD)/trace.err
	grep -Eq '  at .*stack-trace\.lune:2:[0-9]+' $(BUILD)/trace.err
	grep -Eq '  at .*stack-trace\.lune:3:[0-9]+' $(BUILD)/trace.err
	grep -Eq '  at .*stack-trace\.lune:4:[0-9]+' $(BUILD)/trace.err
	@if ./$(BUILD)/lune ../tests/runtime/trace-module/main.lune >$(BUILD)/module-trace.out 2>$(BUILD)/module-trace.err; then echo "FAIL: module trace fixture unexpectedly succeeded"; exit 1; fi
	grep -Eq 'trace-module/child\.lune:1:[0-9]+: error: division by zero' $(BUILD)/module-trace.err
	grep -Fq '  fail := fn() => 1 / 0' $(BUILD)/module-trace.err
	grep -Eq '  at .*trace-module/main\.lune:2:[0-9]+' $(BUILD)/module-trace.err
	@if ./$(BUILD)/lune ../tests/runtime/suggest-global.lune >$(BUILD)/suggest.out 2>$(BUILD)/suggest.err; then echo "FAIL: suggestion fixture unexpectedly succeeded"; exit 1; fi
	grep -Fq "did you mean 'print'?" $(BUILD)/suggest.err
	@if ./$(BUILD)/lune ../tests/runtime/ambiguous-global.lune >$(BUILD)/ambiguous.out 2>$(BUILD)/ambiguous.err; then echo "FAIL: ambiguous suggestion fixture unexpectedly succeeded"; exit 1; fi
	grep -Fq "unknown global 'cit'" $(BUILD)/ambiguous.err
	@if grep -Fq 'did you mean' $(BUILD)/ambiguous.err; then echo "FAIL: ambiguous global emitted a suggestion"; exit 1; fi
	./$(BUILD)/lune run ../tests/runtime/source-import/main.lune
	./$(BUILD)/lune eval ../tests/runtime/source-import/main.lune | grep -Eq '^42\r?

test: $(BUILD)/test-lexer $(BUILD)/test-parser $(BUILD)/test-vm test-syntax test-examples test-selfhost test-bootstrap test-production
	./$(BUILD)/test-lexer
	./$(BUILD)/test-parser
	./$(BUILD)/test-vm

clean:
	rm -rf $(BUILD)

	@if ./$(BUILD)/lune ../tests/runtime/stack-trace.lune >$(BUILD)/trace.out 2>$(BUILD)/trace.err; then echo "FAIL: stack trace fixture unexpectedly succeeded"; exit 1; fi
	grep -Eq 'stack-trace\.lune:1:[0-9]+: error: division by zero' $(BUILD)/trace.err
	grep -Fq '  inner := fn() => 1 / 0' $(BUILD)/trace.err
	grep -Fq 'stack trace:' $(BUILD)/trace.err
	grep -Eq '  at .*stack-trace\.lune:2:[0-9]+' $(BUILD)/trace.err
	grep -Eq '  at .*stack-trace\.lune:3:[0-9]+' $(BUILD)/trace.err
	grep -Eq '  at .*stack-trace\.lune:4:[0-9]+' $(BUILD)/trace.err
	@if ./$(BUILD)/lune ../tests/runtime/trace-module/main.lune >$(BUILD)/module-trace.out 2>$(BUILD)/module-trace.err; then echo "FAIL: module trace fixture unexpectedly succeeded"; exit 1; fi
	grep -Eq 'trace-module/child\.lune:1:[0-9]+: error: division by zero' $(BUILD)/module-trace.err
	grep -Fq '  fail := fn() => 1 / 0' $(BUILD)/module-trace.err
	grep -Eq '  at .*trace-module/main\.lune:2:[0-9]+' $(BUILD)/module-trace.err
	@if ./$(BUILD)/lune ../tests/runtime/suggest-global.lune >$(BUILD)/suggest.out 2>$(BUILD)/suggest.err; then echo "FAIL: suggestion fixture unexpectedly succeeded"; exit 1; fi
	grep -Fq "did you mean 'print'?" $(BUILD)/suggest.err
	@if ./$(BUILD)/lune ../tests/runtime/ambiguous-global.lune >$(BUILD)/ambiguous.out 2>$(BUILD)/ambiguous.err; then echo "FAIL: ambiguous suggestion fixture unexpectedly succeeded"; exit 1; fi
	grep -Fq "unknown global 'cit'" $(BUILD)/ambiguous.err
	@if grep -Fq 'did you mean' $(BUILD)/ambiguous.err; then echo "FAIL: ambiguous global emitted a suggestion"; exit 1; fi
	./$(BUILD)/lune run ../tests/runtime/source-import/main.lune
	./$(BUILD)/lune eval ../tests/runtime/source-import/main.lune | grep -qx '42'

test: $(BUILD)/test-lexer $(BUILD)/test-parser $(BUILD)/test-vm test-syntax test-examples test-selfhost test-bootstrap test-production
	./$(BUILD)/test-lexer
	./$(BUILD)/test-parser
	./$(BUILD)/test-vm

clean:
	rm -rf $(BUILD)


test: $(BUILD)/test-lexer $(BUILD)/test-parser $(BUILD)/test-vm test-syntax test-examples test-selfhost test-bootstrap test-production
	./$(BUILD)/test-lexer
	./$(BUILD)/test-parser
	./$(BUILD)/test-vm

clean:
	rm -rf $(BUILD)

	@if ./$(BUILD)/lune ../tests/runtime/stack-trace.lune >$(BUILD)/trace.out 2>$(BUILD)/trace.err; then echo "FAIL: stack trace fixture unexpectedly succeeded"; exit 1; fi
	grep -Eq 'stack-trace\.lune:1:[0-9]+: error: division by zero' $(BUILD)/trace.err
	grep -Fq '  inner := fn() => 1 / 0' $(BUILD)/trace.err
	grep -Fq 'stack trace:' $(BUILD)/trace.err
	grep -Eq '  at .*stack-trace\.lune:2:[0-9]+' $(BUILD)/trace.err
	grep -Eq '  at .*stack-trace\.lune:3:[0-9]+' $(BUILD)/trace.err
	grep -Eq '  at .*stack-trace\.lune:4:[0-9]+' $(BUILD)/trace.err
	@if ./$(BUILD)/lune ../tests/runtime/trace-module/main.lune >$(BUILD)/module-trace.out 2>$(BUILD)/module-trace.err; then echo "FAIL: module trace fixture unexpectedly succeeded"; exit 1; fi
	grep -Eq 'trace-module/child\.lune:1:[0-9]+: error: division by zero' $(BUILD)/module-trace.err
	grep -Fq '  fail := fn() => 1 / 0' $(BUILD)/module-trace.err
	grep -Eq '  at .*trace-module/main\.lune:2:[0-9]+' $(BUILD)/module-trace.err
	@if ./$(BUILD)/lune ../tests/runtime/suggest-global.lune >$(BUILD)/suggest.out 2>$(BUILD)/suggest.err; then echo "FAIL: suggestion fixture unexpectedly succeeded"; exit 1; fi
	grep -Fq "did you mean 'print'?" $(BUILD)/suggest.err
	@if ./$(BUILD)/lune ../tests/runtime/ambiguous-global.lune >$(BUILD)/ambiguous.out 2>$(BUILD)/ambiguous.err; then echo "FAIL: ambiguous suggestion fixture unexpectedly succeeded"; exit 1; fi
	grep -Fq "unknown global 'cit'" $(BUILD)/ambiguous.err
	@if grep -Fq 'did you mean' $(BUILD)/ambiguous.err; then echo "FAIL: ambiguous global emitted a suggestion"; exit 1; fi
	./$(BUILD)/lune run ../tests/runtime/source-import/main.lune
	./$(BUILD)/lune eval ../tests/runtime/source-import/main.lune | grep -qx '42'

test: $(BUILD)/test-lexer $(BUILD)/test-parser $(BUILD)/test-vm test-syntax test-examples test-selfhost test-bootstrap test-production
	./$(BUILD)/test-lexer
	./$(BUILD)/test-parser
	./$(BUILD)/test-vm

clean:
	rm -rf $(BUILD)
