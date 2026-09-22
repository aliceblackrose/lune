#include "parser.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
typedef struct { int count; } Diagnostics;

static void diagnostic(void *context, LuneSpan span, const char *message) {
    (void)span; (void)message;
    Diagnostics *diagnostics = context;
    diagnostics->count++;
}

static void fail(const char *test, const char *message) {
    fprintf(stderr, "FAIL %s: %s\n", test, message);
    failures++;
}

static LuneAst *parse(const char *source, LuneParser *parser, Diagnostics *diagnostics) {
    lune_parser_init(parser, source, strlen(source), diagnostic, diagnostics);
    return lune_parse_program(parser);
}

static void test_precedence(void) {
    const char *source = "x := 1 + 2 * 3\n";
    LuneParser parser; Diagnostics diagnostics = {0};
    LuneAst *program = parse(source, &parser, &diagnostics);
    if (parser.had_error || program == NULL || program->as.sequence.statements.count != 1) {
        fail("precedence", "program did not parse"); lune_ast_free(program); return;
    }
    LuneAst *decl = program->as.sequence.statements.items[0];
    LuneAst *plus = decl->as.declare.value;
    if (decl->kind != LUNE_AST_DECLARE || plus->kind != LUNE_AST_BINARY ||
        plus->as.binary.op != LUNE_TOKEN_PLUS || plus->as.binary.right->kind != LUNE_AST_BINARY ||
        plus->as.binary.right->as.binary.op != LUNE_TOKEN_STAR)
        fail("precedence", "expected multiplication to bind tighter than addition");
    lune_ast_free(program);
}

static void test_postfix_chain(void) {
    const char *source = "x := factory()(value).items[0]\n";
    LuneParser parser; Diagnostics diagnostics = {0};
    LuneAst *program = parse(source, &parser, &diagnostics);
    if (parser.had_error || program == NULL) {
        fail("postfix-chain", "program did not parse"); lune_ast_free(program); return;
    }
    LuneAst *value = program->as.sequence.statements.items[0]->as.declare.value;
    if (value->kind != LUNE_AST_INDEX || value->as.index.object->kind != LUNE_AST_MEMBER ||
        value->as.index.object->as.member.object->kind != LUNE_AST_CALL)
        fail("postfix-chain", "postfix operations did not compose left-to-right");
    lune_ast_free(program);
}

static void test_assignment_target(void) {
    const char *source = "(x + 1) = 2\n";
    LuneParser parser; Diagnostics diagnostics = {0};
    LuneAst *program = parse(source, &parser, &diagnostics);
    if (!parser.had_error || diagnostics.count == 0)
        fail("assignment-target", "invalid assignment target was accepted");
    lune_ast_free(program);
}

static void test_recovery(void) {
    const char *source = "x :=\ny :=\nz := 1\n";
    LuneParser parser; Diagnostics diagnostics = {0};
    LuneAst *program = parse(source, &parser, &diagnostics);
    if (!parser.had_error || diagnostics.count < 2)
        fail("recovery", "parser did not report multiple independent errors");
    if (program == NULL || program->as.sequence.statements.count != 1)
        fail("recovery", "parser did not recover to parse the final declaration");
    lune_ast_free(program);
}

int main(void) {
    test_precedence();
    test_postfix_chain();
    test_assignment_target();
    test_recovery();
    if (failures != 0) {
        fprintf(stderr, "%d parser test(s) failed\n", failures);
        return 1;
    }
    puts("parser tests passed");
    return 0;
}
