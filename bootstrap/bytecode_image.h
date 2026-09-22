#ifndef LUNE_BYTECODE_IMAGE_H
#define LUNE_BYTECODE_IMAGE_H

#include "bytecode.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool lune_bytecode_load(
    const uint8_t *data,
    size_t length,
    LuneChunk *chunk,
    char *error,
    size_t error_capacity
);

#endif
