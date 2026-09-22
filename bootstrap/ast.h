#ifndef LUNE_AST_H
#define LUNE_AST_H

#include "lexer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct LuneAst LuneAst;

typedef enum {
    LUNE_AST_PROGRAM,
    LUNE_AST_NULL,
    LUNE_AST_BOOL,
    LUNE_AST_NUMBER,
    LUNE_AST_STRING,
    LUNE_AST_NAME,
    LUNE_AST_UNARY,
    LUNE_AST_BINARY,
    LUNE_AST_CALL,
    LUNE_AST_INDEX,
    LUNE_AST_MEMBER,
    LUNE_AST_FUNCTION,
    LUNE_AST_BLOCK,
    LUNE_AST_IF,
    LUNE_AST_WHILE,
    LUNE_AST_DECLARE,
    LUNE_AST_ASSIGN,
    LUNE_AST_LIST,
    LUNE_AST_MAP
} LuneAstKind;

typedef struct {
    LuneAst **items;
    size_t count;
    size_t capacity;
} LuneAstList;

typedef struct {
    LuneSpan *items;
    size_t count;
    size_t capacity;
} LuneSpanList;

typedef struct {
    LuneSpan key;
    bool quoted;
    LuneAst *value;
} LuneMapEntry;

typedef struct {
    LuneMapEntry *items;
    size_t count;
    size_t capacity;
} LuneMapEntryList;

struct LuneAst {
    LuneAstKind kind;
    LuneSpan span;
    union {
        bool boolean;
        struct { LuneTokenKind op; LuneAst *operand; } unary;
        struct { LuneTokenKind op; LuneAst *left; LuneAst *right; } binary;
        struct { LuneAst *callee; LuneAstList arguments; } call;
        struct { LuneAst *object; LuneAst *index; } index;
        struct { LuneAst *object; LuneSpan name; } member;
        struct { LuneSpanList parameters; LuneAst *body; } function;
        struct { LuneAstList statements; } sequence;
        struct { LuneAst *condition; LuneAst *then_branch; LuneAst *else_branch; } if_expr;
        struct { LuneAst *condition; LuneAst *body; } while_stmt;
        struct { LuneSpan name; LuneAst *value; } declare;
        struct { LuneAst *target; LuneAst *value; } assign;
        struct { LuneAstList elements; } list;
        struct { LuneMapEntryList entries; } map;
    } as;
};

LuneAst *lune_ast_new(LuneAstKind kind, LuneSpan span);
bool lune_ast_list_push(LuneAstList *list, LuneAst *node);
bool lune_span_list_push(LuneSpanList *list, LuneSpan span);
bool lune_map_entry_list_push(LuneMapEntryList *list, LuneMapEntry entry);
void lune_ast_free(LuneAst *node);
void lune_ast_dump(FILE *out, const LuneAst *node, const char *source);
const char *lune_ast_kind_name(LuneAstKind kind);

#endif
