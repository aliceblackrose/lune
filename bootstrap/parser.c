#include "parser.h"

#include <stdlib.h>

static LuneSpan merge_span(LuneSpan a, LuneSpan b) {
    size_t end = b.offset + b.length;
    LuneSpan span = a;
    span.length = end > a.offset ? end - a.offset : a.length;
    return span;
}

static void report(LuneParser *parser, LuneSpan span, const char *message) {
    parser->had_error = true;
    if (parser->diagnostic != NULL)
        parser->diagnostic(parser->diagnostic_context, span, message);
}

static void report_oom(LuneParser *parser, LuneSpan span) {
    if (parser->oom) return;
    parser->oom = true;
    report(parser, span, "out of memory");
}

static LuneAst *new_node(LuneParser *parser, LuneAstKind kind, LuneSpan span) {
    LuneAst *node = lune_ast_new(kind, span);
    if (node == NULL) report_oom(parser, span);
    return node;
}

static void advance(LuneParser *parser) {
    parser->current = parser->next;
    parser->next = lune_lexer_next(&parser->lexer);
    if (parser->current.kind == LUNE_TOKEN_ERROR || parser->lexer.had_error)
        parser->had_error = true;
}

static bool match(LuneParser *parser, LuneTokenKind kind) {
    if (parser->current.kind != kind) return false;
    advance(parser);
    return true;
}

static bool expect(LuneParser *parser, LuneTokenKind kind, const char *message) {
    if (match(parser, kind)) return true;
    report(parser, parser->current.span, message);
    return false;
}

static void skip_newlines(LuneParser *parser) {
    while (match(parser, LUNE_TOKEN_NEWLINE)) {}
}

static void synchronize(LuneParser *parser) {
    while (parser->current.kind != LUNE_TOKEN_EOF &&
           parser->current.kind != LUNE_TOKEN_NEWLINE &&
           parser->current.kind != LUNE_TOKEN_RBRACE) {
        advance(parser);
    }
}

static bool is_assignment_target(const LuneAst *node) {
    return node != NULL && (node->kind == LUNE_AST_NAME ||
                            node->kind == LUNE_AST_INDEX ||
                            node->kind == LUNE_AST_MEMBER);
}

static LuneAst *parse_expression_bp(LuneParser *parser, int min_bp);
static LuneAst *parse_statement(LuneParser *parser);
static LuneAst *parse_block(LuneParser *parser);

static LuneAst *parse_list(LuneParser *parser) {
    LuneToken open = parser->current;
    advance(parser);
    skip_newlines(parser);

    LuneAst *node = new_node(parser, LUNE_AST_LIST, open.span);
    if (node == NULL) return NULL;

    if (parser->current.kind == LUNE_TOKEN_RBRACK) {
        LuneToken close = parser->current;
        advance(parser);
        node->span = merge_span(open.span, close.span);
        return node;
    }

    for (;;) {
        LuneAst *element = parse_expression_bp(parser, 0);
        if (element == NULL) break;
        if (!lune_ast_list_push(&node->as.list.elements, element)) {
            report_oom(parser, element->span);
            lune_ast_free(element);
            break;
        }
        skip_newlines(parser);
        if (match(parser, LUNE_TOKEN_COMMA)) {
            skip_newlines(parser);
            if (parser->current.kind == LUNE_TOKEN_RBRACK) break;
            continue;
        }
        break;
    }

    LuneToken close = parser->current;
    if (!expect(parser, LUNE_TOKEN_RBRACK, "expected ']' after list")) {
        lune_ast_free(node);
        return NULL;
    }
    node->span = merge_span(open.span, close.span);
    return node;
}

static LuneAst *parse_map(LuneParser *parser) {
    LuneToken open = parser->current;
    advance(parser);
    skip_newlines(parser);

    LuneAst *node = new_node(parser, LUNE_AST_MAP, open.span);
    if (node == NULL) return NULL;

    if (parser->current.kind == LUNE_TOKEN_RBRACE) {
        LuneToken close = parser->current;
        advance(parser);
        node->span = merge_span(open.span, close.span);
        return node;
    }

    for (;;) {
        bool quoted = parser->current.kind == LUNE_TOKEN_STRING;
        if (parser->current.kind != LUNE_TOKEN_IDENTIFIER && !quoted) {
            report(parser, parser->current.span, "expected map key");
            break;
        }
        LuneSpan key = parser->current.span;
        advance(parser);
        skip_newlines(parser);
        if (!expect(parser, LUNE_TOKEN_COLON, "expected ':' after map key")) break;
        skip_newlines(parser);
        LuneAst *value = parse_expression_bp(parser, 0);
        if (value == NULL) break;
        LuneMapEntry entry = {.key = key, .quoted = quoted, .value = value};
        if (!lune_map_entry_list_push(&node->as.map.entries, entry)) {
            report_oom(parser, value->span);
            lune_ast_free(value);
            break;
        }
        skip_newlines(parser);
        if (match(parser, LUNE_TOKEN_COMMA)) {
            skip_newlines(parser);
            if (parser->current.kind == LUNE_TOKEN_RBRACE) break;
            continue;
        }
        break;
    }

    LuneToken close = parser->current;
    if (!expect(parser, LUNE_TOKEN_RBRACE, "expected '}' after map")) {
        lune_ast_free(node);
        return NULL;
    }
    node->span = merge_span(open.span, close.span);
    return node;
}

static LuneAst *parse_function(LuneParser *parser) {
    LuneToken start = parser->current;
    advance(parser);
    if (!expect(parser, LUNE_TOKEN_LPAREN, "expected '(' after 'fn'")) return NULL;
    skip_newlines(parser);

    LuneAst *node = new_node(parser, LUNE_AST_FUNCTION, start.span);
    if (node == NULL) return NULL;

    if (parser->current.kind != LUNE_TOKEN_RPAREN) {
        for (;;) {
            if (parser->current.kind != LUNE_TOKEN_IDENTIFIER) {
                report(parser, parser->current.span, "expected parameter name");
                lune_ast_free(node);
                return NULL;
            }
            if (!lune_span_list_push(&node->as.function.parameters, parser->current.span)) {
                report_oom(parser, parser->current.span);
                lune_ast_free(node);
                return NULL;
            }
            advance(parser);
            skip_newlines(parser);
            if (!match(parser, LUNE_TOKEN_COMMA)) break;
            skip_newlines(parser);
            if (parser->current.kind == LUNE_TOKEN_RPAREN) break;
        }
    }

    if (!expect(parser, LUNE_TOKEN_RPAREN, "expected ')' after parameters") ||
        !expect(parser, LUNE_TOKEN_ARROW, "expected '=>' after function parameters")) {
        lune_ast_free(node);
        return NULL;
    }
    skip_newlines(parser);

    node->as.function.body = parser->current.kind == LUNE_TOKEN_LBRACE
        ? parse_block(parser)
        : parse_expression_bp(parser, 0);
    if (node->as.function.body == NULL) {
        lune_ast_free(node);
        return NULL;
    }
    node->span = merge_span(start.span, node->as.function.body->span);
    return node;
}

static LuneAst *parse_if(LuneParser *parser) {
    LuneToken start = parser->current;
    advance(parser);
    LuneAst *condition = parse_expression_bp(parser, 0);
    if (condition == NULL) return NULL;
    skip_newlines(parser);
    if (parser->current.kind != LUNE_TOKEN_LBRACE) {
        report(parser, parser->current.span, "expected block after if condition");
        lune_ast_free(condition);
        return NULL;
    }
    LuneAst *then_branch = parse_block(parser);
    if (then_branch == NULL) {
        lune_ast_free(condition);
        return NULL;
    }

    LuneAst *else_branch = NULL;
    if (match(parser, LUNE_TOKEN_ELSE)) {
        if (parser->current.kind == LUNE_TOKEN_IF)
            else_branch = parse_if(parser);
        else if (parser->current.kind == LUNE_TOKEN_LBRACE)
            else_branch = parse_block(parser);
        else
            report(parser, parser->current.span, "expected 'if' or block after 'else'");
    }

    LuneAst *node = new_node(parser, LUNE_AST_IF, start.span);
    if (node == NULL) {
        lune_ast_free(condition); lune_ast_free(then_branch); lune_ast_free(else_branch);
        return NULL;
    }
    node->as.if_expr.condition = condition;
    node->as.if_expr.then_branch = then_branch;
    node->as.if_expr.else_branch = else_branch;
    node->span = merge_span(start.span, else_branch != NULL ? else_branch->span : then_branch->span);
    return node;
}

static LuneAst *parse_prefix(LuneParser *parser) {
    LuneToken token = parser->current;
    switch (token.kind) {
        case LUNE_TOKEN_NULL: {
            advance(parser);
            return new_node(parser, LUNE_AST_NULL, token.span);
        }
        case LUNE_TOKEN_TRUE:
        case LUNE_TOKEN_FALSE: {
            advance(parser);
            LuneAst *node = new_node(parser, LUNE_AST_BOOL, token.span);
            if (node != NULL) node->as.boolean = token.kind == LUNE_TOKEN_TRUE;
            return node;
        }
        case LUNE_TOKEN_NUMBER: advance(parser); return new_node(parser, LUNE_AST_NUMBER, token.span);
        case LUNE_TOKEN_STRING: advance(parser); return new_node(parser, LUNE_AST_STRING, token.span);
        case LUNE_TOKEN_IDENTIFIER: advance(parser); return new_node(parser, LUNE_AST_NAME, token.span);
        case LUNE_TOKEN_MINUS:
        case LUNE_TOKEN_NOT: {
            advance(parser);
            skip_newlines(parser);
            LuneAst *operand = parse_expression_bp(parser, 13);
            if (operand == NULL) return NULL;
            LuneAst *node = new_node(parser, LUNE_AST_UNARY, merge_span(token.span, operand->span));
            if (node == NULL) { lune_ast_free(operand); return NULL; }
            node->as.unary.op = token.kind;
            node->as.unary.operand = operand;
            return node;
        }
        case LUNE_TOKEN_LPAREN: {
            advance(parser);
            skip_newlines(parser);
            LuneAst *node = parse_expression_bp(parser, 0);
            skip_newlines(parser);
            if (!expect(parser, LUNE_TOKEN_RPAREN, "expected ')' after expression")) {
                lune_ast_free(node);
                return NULL;
            }
            return node;
        }
        case LUNE_TOKEN_LBRACK: return parse_list(parser);
        case LUNE_TOKEN_LBRACE: return parse_map(parser);
        case LUNE_TOKEN_FN: return parse_function(parser);
        case LUNE_TOKEN_IF: return parse_if(parser);
        case LUNE_TOKEN_ERROR:
            advance(parser);
            return NULL;
        default:
            report(parser, token.span, "expected expression");
            return NULL;
    }
}

static bool infix_bp(LuneTokenKind kind, int *left, int *right) {
    int bp;
    switch (kind) {
        case LUNE_TOKEN_OR: bp = 1; break;
        case LUNE_TOKEN_AND: bp = 3; break;
        case LUNE_TOKEN_EQ:
        case LUNE_TOKEN_NE: bp = 5; break;
        case LUNE_TOKEN_LT:
        case LUNE_TOKEN_LE:
        case LUNE_TOKEN_GT:
        case LUNE_TOKEN_GE: bp = 7; break;
        case LUNE_TOKEN_PLUS:
        case LUNE_TOKEN_MINUS: bp = 9; break;
        case LUNE_TOKEN_STAR:
        case LUNE_TOKEN_SLASH:
        case LUNE_TOKEN_PERCENT: bp = 11; break;
        default: return false;
    }
    *left = bp;
    *right = bp + 1;
    return true;
}

static LuneAst *finish_call(LuneParser *parser, LuneAst *callee) {
    advance(parser);
    skip_newlines(parser);
    LuneAst *node = new_node(parser, LUNE_AST_CALL, callee->span);
    if (node == NULL) { lune_ast_free(callee); return NULL; }
    node->as.call.callee = callee;

    if (parser->current.kind != LUNE_TOKEN_RPAREN) {
        for (;;) {
            LuneAst *argument = parse_expression_bp(parser, 0);
            if (argument == NULL) { lune_ast_free(node); return NULL; }
            if (!lune_ast_list_push(&node->as.call.arguments, argument)) {
                report_oom(parser, argument->span); lune_ast_free(argument); lune_ast_free(node); return NULL;
            }
            skip_newlines(parser);
            if (!match(parser, LUNE_TOKEN_COMMA)) break;
            skip_newlines(parser);
            if (parser->current.kind == LUNE_TOKEN_RPAREN) break;
        }
    }
    LuneToken close = parser->current;
    if (!expect(parser, LUNE_TOKEN_RPAREN, "expected ')' after arguments")) {
        lune_ast_free(node); return NULL;
    }
    node->span = merge_span(callee->span, close.span);
    return node;
}

static LuneAst *finish_index(LuneParser *parser, LuneAst *object) {
    advance(parser);
    skip_newlines(parser);
    LuneAst *index = parse_expression_bp(parser, 0);
    if (index == NULL) { lune_ast_free(object); return NULL; }
    skip_newlines(parser);
    LuneToken close = parser->current;
    if (!expect(parser, LUNE_TOKEN_RBRACK, "expected ']' after index")) {
        lune_ast_free(object); lune_ast_free(index); return NULL;
    }
    LuneAst *node = new_node(parser, LUNE_AST_INDEX, merge_span(object->span, close.span));
    if (node == NULL) { lune_ast_free(object); lune_ast_free(index); return NULL; }
    node->as.index.object = object;
    node->as.index.index = index;
    return node;
}

static LuneAst *finish_member(LuneParser *parser, LuneAst *object) {
    advance(parser);
    if (parser->current.kind != LUNE_TOKEN_IDENTIFIER) {
        report(parser, parser->current.span, "expected member name after '.'");
        lune_ast_free(object);
        return NULL;
    }
    LuneToken name = parser->current;
    advance(parser);
    LuneAst *node = new_node(parser, LUNE_AST_MEMBER, merge_span(object->span, name.span));
    if (node == NULL) { lune_ast_free(object); return NULL; }
    node->as.member.object = object;
    node->as.member.name = name.span;
    return node;
}

static LuneAst *parse_expression_bp(LuneParser *parser, int min_bp) {
    LuneAst *left = parse_prefix(parser);
    if (left == NULL) return NULL;

    for (;;) {
        if (parser->current.kind == LUNE_TOKEN_LPAREN) {
            left = finish_call(parser, left);
            if (left == NULL) return NULL;
            continue;
        }
        if (parser->current.kind == LUNE_TOKEN_LBRACK) {
            left = finish_index(parser, left);
            if (left == NULL) return NULL;
            continue;
        }
        if (parser->current.kind == LUNE_TOKEN_DOT) {
            left = finish_member(parser, left);
            if (left == NULL) return NULL;
            continue;
        }

        int left_bp = 0, right_bp = 0;
        if (!infix_bp(parser->current.kind, &left_bp, &right_bp) || left_bp < min_bp)
            break;
        LuneToken op = parser->current;
        advance(parser);
        skip_newlines(parser);
        LuneAst *right = parse_expression_bp(parser, right_bp);
        if (right == NULL) { lune_ast_free(left); return NULL; }
        LuneAst *binary = new_node(parser, LUNE_AST_BINARY, merge_span(left->span, right->span));
        if (binary == NULL) { lune_ast_free(left); lune_ast_free(right); return NULL; }
        binary->as.binary.op = op.kind;
        binary->as.binary.left = left;
        binary->as.binary.right = right;
        left = binary;
    }
    return left;
}

static LuneAst *parse_block(LuneParser *parser) {
    LuneToken open = parser->current;
    if (!expect(parser, LUNE_TOKEN_LBRACE, "expected '{'")) return NULL;
    LuneAst *block = new_node(parser, LUNE_AST_BLOCK, open.span);
    if (block == NULL) return NULL;
    skip_newlines(parser);

    while (parser->current.kind != LUNE_TOKEN_RBRACE && parser->current.kind != LUNE_TOKEN_EOF) {
        LuneAst *statement = parse_statement(parser);
        if (statement != NULL && !lune_ast_list_push(&block->as.sequence.statements, statement)) {
            report_oom(parser, statement->span); lune_ast_free(statement); lune_ast_free(block); return NULL;
        }
        if (parser->current.kind == LUNE_TOKEN_NEWLINE) {
            skip_newlines(parser);
        } else if (parser->current.kind != LUNE_TOKEN_RBRACE) {
            report(parser, parser->current.span, "expected newline or '}' after statement");
            synchronize(parser);
            skip_newlines(parser);
        }
    }

    LuneToken close = parser->current;
    if (!expect(parser, LUNE_TOKEN_RBRACE, "expected '}' after block")) {
        lune_ast_free(block); return NULL;
    }
    block->span = merge_span(open.span, close.span);
    return block;
}

static LuneAst *parse_while(LuneParser *parser) {
    LuneToken start = parser->current;
    advance(parser);
    LuneAst *condition = parse_expression_bp(parser, 0);
    if (condition == NULL) return NULL;
    skip_newlines(parser);
    if (parser->current.kind != LUNE_TOKEN_LBRACE) {
        report(parser, parser->current.span, "expected block after while condition");
        lune_ast_free(condition);
        return NULL;
    }
    LuneAst *body = parse_block(parser);
    if (body == NULL) { lune_ast_free(condition); return NULL; }
    LuneAst *node = new_node(parser, LUNE_AST_WHILE, merge_span(start.span, body->span));
    if (node == NULL) { lune_ast_free(condition); lune_ast_free(body); return NULL; }
    node->as.while_stmt.condition = condition;
    node->as.while_stmt.body = body;
    return node;
}

static LuneAst *parse_declaration(LuneParser *parser) {
    LuneToken name = parser->current;
    advance(parser);
    advance(parser);
    LuneAst *value = parse_expression_bp(parser, 0);
    if (value == NULL) return NULL;
    LuneAst *node = new_node(parser, LUNE_AST_DECLARE, merge_span(name.span, value->span));
    if (node == NULL) { lune_ast_free(value); return NULL; }
    node->as.declare.name = name.span;
    node->as.declare.value = value;
    return node;
}

static LuneAst *parse_statement(LuneParser *parser) {
    if (parser->current.kind == LUNE_TOKEN_WHILE)
        return parse_while(parser);
    if (parser->current.kind == LUNE_TOKEN_IDENTIFIER && parser->next.kind == LUNE_TOKEN_DECLARE)
        return parse_declaration(parser);

    LuneAst *left = parse_expression_bp(parser, 0);
    if (left == NULL) return NULL;
    if (!match(parser, LUNE_TOKEN_ASSIGN)) return left;

    if (!is_assignment_target(left))
        report(parser, left->span, "invalid assignment target");
    LuneAst *value = parse_expression_bp(parser, 0);
    if (value == NULL) { lune_ast_free(left); return NULL; }
    LuneAst *node = new_node(parser, LUNE_AST_ASSIGN, merge_span(left->span, value->span));
    if (node == NULL) { lune_ast_free(left); lune_ast_free(value); return NULL; }
    node->as.assign.target = left;
    node->as.assign.value = value;
    return node;
}

void lune_parser_init(
    LuneParser *parser,
    const char *source,
    size_t length,
    LuneDiagnosticFn diagnostic,
    void *diagnostic_context
) {
    *parser = (LuneParser){0};
    parser->diagnostic = diagnostic;
    parser->diagnostic_context = diagnostic_context;
    lune_lexer_init(&parser->lexer, source, length, diagnostic, diagnostic_context);
    parser->current = lune_lexer_next(&parser->lexer);
    parser->next = lune_lexer_next(&parser->lexer);
    if (parser->current.kind == LUNE_TOKEN_ERROR || parser->next.kind == LUNE_TOKEN_ERROR || parser->lexer.had_error)
        parser->had_error = true;
}

LuneAst *lune_parse_program(LuneParser *parser) {
    LuneSpan start = parser->current.span;
    LuneAst *program = new_node(parser, LUNE_AST_PROGRAM, start);
    if (program == NULL) return NULL;
    skip_newlines(parser);

    while (parser->current.kind != LUNE_TOKEN_EOF) {
        LuneAst *statement = parse_statement(parser);
        if (statement != NULL && !lune_ast_list_push(&program->as.sequence.statements, statement)) {
            report_oom(parser, statement->span); lune_ast_free(statement); lune_ast_free(program); return NULL;
        }

        if (parser->current.kind == LUNE_TOKEN_NEWLINE) {
            skip_newlines(parser);
        } else if (parser->current.kind != LUNE_TOKEN_EOF) {
            report(parser, parser->current.span, "expected newline after statement");
            synchronize(parser);
            skip_newlines(parser);
        }
    }

    parser->had_error = parser->had_error || parser->lexer.had_error;
    program->span = merge_span(start, parser->current.span);
    return program;
}
