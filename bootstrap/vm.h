#ifndef LUNE_VM_H
#define LUNE_VM_H

#include "bytecode.h"
#include "lexer.h"
#include "value.h"

bool lune_vm_run(
    const LuneChunk *chunk,
    LuneDiagnosticFn diagnostic,
    void *diagnostic_context,
    LuneValue *result
);

#endif
