#ifndef LUNE_BYTECODE_H
#define LUNE_BYTECODE_H

#include "lexer.h"
#include "value.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LUNE_OP_CONSTANT,
    LUNE_OP_NULL,
    LUNE_OP_TRUE,
    LUNE_OP_FALSE,
    LUNE_OP_STRING,
    LUNE_OP_LIST,
    LUNE_OP_MAP,
    LUNE_OP_POP,
    LUNE_OP_GET_LOCAL,
    LUNE_OP_SET_LOCAL,
    LUNE_OP_GET_GLOBAL,
    LUNE_OP_DEFINE_GLOBAL,
    LUNE_OP_SET_GLOBAL,
    LUNE_OP_GET_INDEX,
    LUNE_OP_SET_INDEX,
    LUNE_OP_GET_FIELD,
    LUNE_OP_SET_FIELD,
    LUNE_OP_ADD,
    LUNE_OP_SUBTRACT,
    LUNE_OP_MULTIPLY,
    LUNE_OP_DIVIDE,
    LUNE_OP_MODULO,
    LUNE_OP_EQUAL,
    LUNE_OP_NOT_EQUAL,
    LUNE_OP_LESS,
    LUNE_OP_LESS_EQUAL,
    LUNE_OP_GREATER,
    LUNE_OP_GREATER_EQUAL,
    LUNE_OP_NOT,
    LUNE_OP_NEGATE,
    LUNE_OP_JUMP,
    LUNE_OP_JUMP_IF_FALSE,
    LUNE_OP_LOOP,
    LUNE_OP_RETURN
} LuneOpcode;

typedef struct {
    char *chars;
    size_t length;
} LuneName;

typedef struct {
    uint8_t *code;
    LuneSpan *spans;
    size_t count;
    size_t capacity;

    LuneValue *constants;
    size_t constants_count;
    size_t constants_capacity;

    LuneName *names;
    size_t names_count;
    size_t names_capacity;
} LuneChunk;

void lune_chunk_init(LuneChunk *chunk);
void lune_chunk_free(LuneChunk *chunk);
bool lune_chunk_write(LuneChunk *chunk, uint8_t byte, LuneSpan span);
bool lune_chunk_add_constant(
    LuneChunk *chunk,
    LuneValue value,
    uint16_t *index
);
bool lune_chunk_intern_name(
    LuneChunk *chunk,
    const char *chars,
    size_t length,
    uint16_t *index
);
const char *lune_opcode_name(LuneOpcode opcode);

#endif
