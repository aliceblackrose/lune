#ifndef LUNE_VM_H
#define LUNE_VM_H

#include "bytecode.h"
#include "lexer.h"
#include "value.h"

#include <stdbool.h>

typedef struct LuneVM LuneVM;

LuneVM *lune_vm_new(
    LuneDiagnosticFn diagnostic,
    void *diagnostic_context
);
void lune_vm_free(LuneVM *vm);

bool lune_vm_run(
    LuneVM *vm,
    const LuneChunk *chunk,
    LuneValue *result
);

#endif
