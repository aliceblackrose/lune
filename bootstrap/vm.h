#ifndef LUNE_VM_H
#define LUNE_VM_H

#include "bytecode.h"
#include "source.h"
#include "value.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct LuneVM LuneVM;

typedef bool (*LuneSourceCompilerFn)(
    void *context,
    const char *path,
    bool module,
    LuneChunk *chunk,
    char *error,
    size_t error_capacity
);

LuneVM *lune_vm_new(
    LuneDiagnosticFn diagnostic,
    void *diagnostic_context
);
void lune_vm_free(
    LuneVM *vm
);

void lune_vm_set_gc_stress(
    LuneVM *vm,
    bool enabled
);
void lune_vm_collect_garbage(
    LuneVM *vm
);
size_t lune_vm_heap_bytes(
    const LuneVM *vm
);

void lune_vm_set_process_args(
    LuneVM *vm,
    int argc,
    const char *const *argv
);
void lune_vm_set_script_path(
    LuneVM *vm,
    const char *path
);
void lune_vm_set_source_compiler(
    LuneVM *vm,
    LuneSourceCompilerFn compiler,
    void *context
);
bool lune_vm_exit_status(
    const LuneVM *vm,
    int *status
);

bool lune_vm_run(
    LuneVM *vm,
    const LuneChunk *chunk,
    LuneValue *result
);

#endif
