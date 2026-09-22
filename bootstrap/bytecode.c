#include "bytecode.h"

#include <stdlib.h>
#include <string.h>

static bool grow(
    void **items,
    size_t *capacity,
    size_t item_size,
    size_t needed
) {
    if (*capacity >= needed) return true;

    size_t next = *capacity == 0 ? 8 : *capacity;
    while (next < needed) {
        if (next > SIZE_MAX / 2) return false;
        next *= 2;
    }
    if (next > SIZE_MAX / item_size) return false;

    void *grown = realloc(*items, next * item_size);
    if (grown == NULL) return false;
    *items = grown;
    *capacity = next;
    return true;
}

void lune_chunk_init(LuneChunk *chunk) {
    *chunk = (LuneChunk){0};
}

void lune_function_free(LuneFunction *function) {
    if (function == NULL) return;
    lune_chunk_free(&function->chunk);
    free(function->upvalues);
    free(function);
}

void lune_chunk_free(LuneChunk *chunk) {
    free(chunk->code);
    free(chunk->spans);
    free(chunk->constants);

    for (size_t i = 0; i < chunk->names_count; i++) {
        free(chunk->names[i].chars);
    }
    free(chunk->names);

    for (size_t i = 0; i < chunk->functions_count; i++) {
        lune_function_free(chunk->functions[i]);
    }
    free(chunk->functions);

    lune_chunk_init(chunk);
}

bool lune_chunk_write(LuneChunk *chunk, uint8_t byte, LuneSpan span) {
    if (chunk->count == chunk->capacity) {
        size_t next = chunk->capacity == 0 ? 16 : chunk->capacity * 2;

        uint8_t *code = realloc(
            chunk->code, next * sizeof(*chunk->code)
        );
        if (code == NULL) return false;

        LuneSpan *spans = realloc(
            chunk->spans, next * sizeof(*chunk->spans)
        );
        if (spans == NULL) {
            chunk->code = code;
            return false;
        }

        chunk->code = code;
        chunk->spans = spans;
        chunk->capacity = next;
    }

    chunk->code[chunk->count] = byte;
    chunk->spans[chunk->count] = span;
    chunk->count++;
    return true;
}

bool lune_chunk_add_constant(
    LuneChunk *chunk,
    LuneValue value,
    uint16_t *index
) {
    if (chunk->constants_count >= UINT16_MAX) return false;
    if (!grow(
        (void **)&chunk->constants,
        &chunk->constants_capacity,
        sizeof(*chunk->constants),
        chunk->constants_count + 1
    )) return false;

    *index = (uint16_t)chunk->constants_count;
    chunk->constants[chunk->constants_count++] = value;
    return true;
}

bool lune_chunk_intern_name(
    LuneChunk *chunk,
    const char *chars,
    size_t length,
    uint16_t *index
) {
    for (size_t i = 0; i < chunk->names_count; i++) {
        if (
            chunk->names[i].length == length &&
            memcmp(chunk->names[i].chars, chars, length) == 0
        ) {
            *index = (uint16_t)i;
            return true;
        }
    }

    if (chunk->names_count >= UINT16_MAX) return false;
    if (!grow(
        (void **)&chunk->names,
        &chunk->names_capacity,
        sizeof(*chunk->names),
        chunk->names_count + 1
    )) return false;

    char *copy = malloc(length + 1);
    if (copy == NULL) return false;
    memcpy(copy, chars, length);
    copy[length] = '\0';

    chunk->names[chunk->names_count] = (LuneName){
        .chars = copy,
        .length = length,
    };
    *index = (uint16_t)chunk->names_count++;
    return true;
}

bool lune_chunk_add_function(
    LuneChunk *chunk,
    LuneFunction *function,
    uint16_t *index
) {
    if (chunk->functions_count >= UINT16_MAX) return false;
    if (!grow(
        (void **)&chunk->functions,
        &chunk->functions_capacity,
        sizeof(*chunk->functions),
        chunk->functions_count + 1
    )) return false;

    *index = (uint16_t)chunk->functions_count;
    chunk->functions[chunk->functions_count++] = function;
    return true;
}

LuneFunction *lune_function_new(uint16_t arity) {
    LuneFunction *function = calloc(1, sizeof(*function));
    if (function == NULL) return NULL;
    function->arity = arity;
    lune_chunk_init(&function->chunk);
    return function;
}

bool lune_function_set_upvalues(
    LuneFunction *function,
    const LuneUpvalueDesc *upvalues,
    size_t count
) {
    if (count == 0) {
        function->upvalue_count = 0;
        return true;
    }

    if (count > SIZE_MAX / sizeof(*function->upvalues)) {
        return false;
    }
    LuneUpvalueDesc *copy = malloc(
        count * sizeof(*copy)
    );
    if (copy == NULL) return false;

    memcpy(copy, upvalues, count * sizeof(*copy));
    function->upvalues = copy;
    function->upvalue_count = count;
    return true;
}

const char *lune_opcode_name(LuneOpcode opcode) {
    static const char *names[] = {
        "CONSTANT",
        "NULL",
        "TRUE",
        "FALSE",
        "STRING",
        "LIST",
        "MAP",
        "POP",
        "GET_LOCAL",
        "SET_LOCAL",
        "GET_UPVALUE",
        "SET_UPVALUE",
        "GET_GLOBAL",
        "DEFINE_GLOBAL",
        "SET_GLOBAL",
        "GET_INDEX",
        "SET_INDEX",
        "GET_FIELD",
        "SET_FIELD",
        "ADD",
        "SUBTRACT",
        "MULTIPLY",
        "DIVIDE",
        "MODULO",
        "EQUAL",
        "NOT_EQUAL",
        "LESS",
        "LESS_EQUAL",
        "GREATER",
        "GREATER_EQUAL",
        "NOT",
        "NEGATE",
        "JUMP",
        "JUMP_IF_FALSE",
        "LOOP",
        "CLOSURE",
        "CALL",
        "RETURN",
    };

    return (size_t)opcode < sizeof(names) / sizeof(names[0])
        ? names[opcode]
        : "UNKNOWN";
}
