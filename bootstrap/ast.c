#include "ast.h"

#include <stdlib.h>

static bool grow(void **items, size_t *capacity, size_t item_size) {
    size_t next = *capacity == 0 ? 4 : *capacity * 2;
    void *grown = realloc(*items, next * item_size);
    if (grown == NULL) return false;
    *items = grown;
    *capacity = next;
    return true;
}

LuneAst *lune_ast_new(LuneAstKind kind, LuneSpan span) {
    LuneAst *node = calloc(1, sizeof(*node));
    if (node == NULL) return NULL;
    node->kind = kind;
    node->span = span;
    return node;
}

bool lune_ast_list_push(LuneAstList *list, LuneAst *node) {
    if (list->count == list->capacity &&
        !grow((void **)&list->items, &list->capacity, sizeof(*list->items))) return false;
    list->items[list->count++] = node;
    return true;
}

bool lune_span_list_push(LuneSpanList *list, LuneSpan span) {
    if (list->count == list->capacity &&
        !grow((void **)&list->items, &list->capacity, sizeof(*list->items))) return false;
    list->items[list->count++] = span;
    return true;
}

bool lune_map_entry_list_push(LuneMapEntryList *list, LuneMapEntry entry) {
    if (list->count == list->capacity &&
        !grow((void **)&list->items, &list->capacity, sizeof(*list->items))) return false;
    list->items[list->count++] = entry;
    return true;
}

void lune_ast_free(LuneAst *node) {
    if (node == NULL) return;
    switch (node->kind) {
        case LUNE_AST_PROGRAM:
        case LUNE_AST_BLOCK:
            for (size_t i = 0; i < node->as.sequence.statements.count; i++)
                lune_ast_free(node->as.sequence.statements.items[i]);
            free(node->as.sequence.statements.items);
            break;
        case LUNE_AST_UNARY:
            lune_ast_free(node->as.unary.operand);
            break;
        case LUNE_AST_BINARY:
            lune_ast_free(node->as.binary.left);
            lune_ast_free(node->as.binary.right);
            break;
        case LUNE_AST_CALL:
            lune_ast_free(node->as.call.callee);
            for (size_t i = 0; i < node->as.call.arguments.count; i++)
                lune_ast_free(node->as.call.arguments.items[i]);
            free(node->as.call.arguments.items);
            break;
        case LUNE_AST_INDEX:
            lune_ast_free(node->as.index.object);
            lune_ast_free(node->as.index.index);
            break;
        case LUNE_AST_MEMBER:
            lune_ast_free(node->as.member.object);
            break;
        case LUNE_AST_FUNCTION:
            free(node->as.function.parameters.items);
            lune_ast_free(node->as.function.body);
            break;
        case LUNE_AST_IF:
            lune_ast_free(node->as.if_expr.condition);
            lune_ast_free(node->as.if_expr.then_branch);
            lune_ast_free(node->as.if_expr.else_branch);
            break;
        case LUNE_AST_WHILE:
            lune_ast_free(node->as.while_stmt.condition);
            lune_ast_free(node->as.while_stmt.body);
            break;
        case LUNE_AST_DECLARE:
            lune_ast_free(node->as.declare.value);
            break;
        case LUNE_AST_ASSIGN:
            lune_ast_free(node->as.assign.target);
            lune_ast_free(node->as.assign.value);
            break;
        case LUNE_AST_LIST:
            for (size_t i = 0; i < node->as.list.elements.count; i++)
                lune_ast_free(node->as.list.elements.items[i]);
            free(node->as.list.elements.items);
            break;
        case LUNE_AST_MAP:
            for (size_t i = 0; i < node->as.map.entries.count; i++)
                lune_ast_free(node->as.map.entries.items[i].value);
            free(node->as.map.entries.items);
            break;
        case LUNE_AST_NULL:
        case LUNE_AST_BOOL:
        case LUNE_AST_NUMBER:
        case LUNE_AST_STRING:
        case LUNE_AST_NAME:
            break;
    }
    free(node);
}

const char *lune_ast_kind_name(LuneAstKind kind) {
    static const char *names[] = {
        "Program", "Null", "Bool", "Number", "String", "Name", "Unary", "Binary",
        "Call", "Index", "Member", "Function", "Block", "If", "While", "Declare",
        "Assign", "List", "Map"
    };
    return names[(size_t)kind];
}

static void indent(FILE *out, int depth) {
    for (int i = 0; i < depth; i++) fputs("  ", out);
}

static void span_text(FILE *out, LuneSpan span, const char *source) {
    fprintf(out, "%.*s", (int)span.length, source + span.offset);
}

static void dump(FILE *out, const LuneAst *node, const char *source, int depth) {
    if (node == NULL) {
        indent(out, depth);
        fputs("<null>\n", out);
        return;
    }
    indent(out, depth);
    fputs(lune_ast_kind_name(node->kind), out);
    switch (node->kind) {
        case LUNE_AST_NUMBER:
        case LUNE_AST_STRING:
        case LUNE_AST_NAME:
            fputc(' ', out); span_text(out, node->span, source); fputc('\n', out); return;
        case LUNE_AST_BOOL:
            fprintf(out, " %s\n", node->as.boolean ? "true" : "false"); return;
        case LUNE_AST_NULL:
            fputc('\n', out); return;
        case LUNE_AST_UNARY:
            fprintf(out, " %s\n", lune_token_kind_name(node->as.unary.op));
            dump(out, node->as.unary.operand, source, depth + 1); return;
        case LUNE_AST_BINARY:
            fprintf(out, " %s\n", lune_token_kind_name(node->as.binary.op));
            dump(out, node->as.binary.left, source, depth + 1);
            dump(out, node->as.binary.right, source, depth + 1); return;
        case LUNE_AST_MEMBER:
            fputc(' ', out); span_text(out, node->as.member.name, source); fputc('\n', out);
            dump(out, node->as.member.object, source, depth + 1); return;
        case LUNE_AST_DECLARE:
            fputc(' ', out); span_text(out, node->as.declare.name, source); fputc('\n', out);
            dump(out, node->as.declare.value, source, depth + 1); return;
        default:
            fputc('\n', out); break;
    }

    switch (node->kind) {
        case LUNE_AST_PROGRAM:
        case LUNE_AST_BLOCK:
            for (size_t i = 0; i < node->as.sequence.statements.count; i++)
                dump(out, node->as.sequence.statements.items[i], source, depth + 1);
            break;
        case LUNE_AST_CALL:
            dump(out, node->as.call.callee, source, depth + 1);
            for (size_t i = 0; i < node->as.call.arguments.count; i++)
                dump(out, node->as.call.arguments.items[i], source, depth + 1);
            break;
        case LUNE_AST_INDEX:
            dump(out, node->as.index.object, source, depth + 1);
            dump(out, node->as.index.index, source, depth + 1);
            break;
        case LUNE_AST_FUNCTION:
            indent(out, depth + 1); fputs("Params", out);
            for (size_t i = 0; i < node->as.function.parameters.count; i++) {
                fputc(' ', out); span_text(out, node->as.function.parameters.items[i], source);
            }
            fputc('\n', out);
            dump(out, node->as.function.body, source, depth + 1);
            break;
        case LUNE_AST_IF:
            dump(out, node->as.if_expr.condition, source, depth + 1);
            dump(out, node->as.if_expr.then_branch, source, depth + 1);
            if (node->as.if_expr.else_branch != NULL)
                dump(out, node->as.if_expr.else_branch, source, depth + 1);
            break;
        case LUNE_AST_WHILE:
            dump(out, node->as.while_stmt.condition, source, depth + 1);
            dump(out, node->as.while_stmt.body, source, depth + 1);
            break;
        case LUNE_AST_ASSIGN:
            dump(out, node->as.assign.target, source, depth + 1);
            dump(out, node->as.assign.value, source, depth + 1);
            break;
        case LUNE_AST_LIST:
            for (size_t i = 0; i < node->as.list.elements.count; i++)
                dump(out, node->as.list.elements.items[i], source, depth + 1);
            break;
        case LUNE_AST_MAP:
            for (size_t i = 0; i < node->as.map.entries.count; i++) {
                indent(out, depth + 1);
                fputs("Entry ", out);
                span_text(out, node->as.map.entries.items[i].key, source);
                fputc('\n', out);
                dump(out, node->as.map.entries.items[i].value, source, depth + 2);
            }
            break;
        default:
            break;
    }
}

void lune_ast_dump(FILE *out, const LuneAst *node, const char *source) {
    dump(out, node, source, 0);
}
