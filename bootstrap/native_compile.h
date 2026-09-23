#ifndef LUNE_NATIVE_COMPILE_H
#define LUNE_NATIVE_COMPILE_H

#include "bytecode.h"
#include "source.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    LuneDiagnosticFn diagnostic;
    void *diagnostic_context;
} LuneNativeCompiler;

bool lune_native_compile_file(
    void *context,
    const char *path,
    bool module,
    LuneChunk *chunk,
    char *error,
    size_t error_capacity
);

#endif
