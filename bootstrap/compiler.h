#ifndef LUNE_COMPILER_H
#define LUNE_COMPILER_H

#include "ast.h"
#include "bytecode.h"

bool lune_compile(
    const LuneAst *program,
    const char *source,
    LuneChunk *chunk,
    LuneDiagnosticFn diagnostic,
    void *diagnostic_context
);

#endif
