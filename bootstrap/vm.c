#include "vm.h"

#include "compiler.h"
#include "object.h"
#include "parser.h"
#include "platform.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STACK_MAX 1024
#define LOCAL_MAX 256
#define FRAME_MAX 64

typedef struct {
    const LuneChunk *chunk;
    size_t ip;
    size_t stack_base;

    LuneObjClosure *closure;
    const char *module_path;

    LuneValue locals[LOCAL_MAX];
    bool local_defined[LOCAL_MAX];
} CallFrame;

typedef struct {
    char *key;
    bool loading;
    LuneValue value;
    LuneChunk *chunk;
} ModuleEntry;

struct LuneVM {
    LuneHeap heap;

    LuneValue stack[STACK_MAX];
    size_t stack_count;

    LuneValue *native_roots;
    size_t native_root_count;
    size_t native_root_capacity;

    CallFrame frames[FRAME_MAX];
    size_t frame_count;

    LuneObjMap *globals;
    LuneObjUpvalue *open_upvalues;

    LuneValue last_result;
    bool has_result;

    int process_argc;
    const char *const *process_argv;
    const char *script_path;

    ModuleEntry *modules;
    size_t module_count;
    size_t module_capacity;

    bool exit_requested;
    int exit_status;
    LuneSpan native_span;

    LuneDiagnosticFn diagnostic;
    void *diagnostic_context;
};

static bool native_error(
    LuneVM *vm,
    const char *message
);

static bool run_until(
    LuneVM *vm,
    LuneValue *result,
    size_t stop_depth
);

static bool invoke_callable(
    LuneVM *vm,
    LuneValue callee,
    int argc,
    const LuneValue *args,
    LuneValue *result
);

static bool runtime_error(
    LuneVM *vm,
    LuneSpan span,
    const char *message
) {
    if (vm->diagnostic != NULL) {
        vm->diagnostic(
            vm->diagnostic_context,
            span,
            message
        );
    }
    return false;
}

static void mark_vm_roots(
    void *context,
    LuneHeap *heap
) {
    LuneVM *vm = context;

    for (
        size_t i = 0;
        i < vm->stack_count;
        i++
    ) {
        lune_heap_mark_value(
            heap, vm->stack[i]
        );
    }

    for (
        size_t i = 0;
        i < vm->native_root_count;
        i++
    ) {
        lune_heap_mark_value(
            heap,
            vm->native_roots[i]
        );
    }

    if (vm->globals != NULL) {
        lune_heap_mark_object(
            heap,
            (LuneObj *)vm->globals
        );
    }

    for (
        size_t i = 0;
        i < vm->module_count;
        i++
    ) {
        if (!vm->modules[i].loading) {
            lune_heap_mark_value(
                heap,
                vm->modules[i].value
            );
        }
    }

    for (
        size_t frame_index = 0;
        frame_index < vm->frame_count;
        frame_index++
    ) {
        CallFrame *frame =
            &vm->frames[frame_index];

        if (frame->closure != NULL) {
            lune_heap_mark_object(
                heap,
                (LuneObj *)frame->closure
            );
        }

        for (
            size_t local = 0;
            local < LOCAL_MAX;
            local++
        ) {
            if (frame->local_defined[local]) {
                lune_heap_mark_value(
                    heap,
                    frame->locals[local]
                );
            }
        }
    }

    for (
        LuneObjUpvalue *upvalue =
            vm->open_upvalues;
        upvalue != NULL;
        upvalue = upvalue->next_open
    ) {
        lune_heap_mark_object(
            heap,
            (LuneObj *)upvalue
        );
    }

    if (vm->has_result) {
        lune_heap_mark_value(
            heap, vm->last_result
        );
    }
}

static CallFrame *current_frame(
    LuneVM *vm
) {
    if (vm->frame_count == 0) {
        return NULL;
    }

    return &vm->frames[
        vm->frame_count - 1
    ];
}

static bool push(
    LuneVM *vm,
    LuneValue value,
    LuneSpan span
) {
    if (
        vm->stack_count >= STACK_MAX
    ) {
        return runtime_error(
            vm,
            span,
            "runtime stack overflow"
        );
    }

    vm->stack[vm->stack_count++] =
        value;
    return true;
}

static bool native_root_push(
    LuneVM *vm,
    LuneValue value
) {
    if (
        vm->native_root_count ==
        vm->native_root_capacity
    ) {
        size_t next =
            vm->native_root_capacity == 0
            ? 16
            : vm->native_root_capacity * 2;

        if (
            next <
                vm->native_root_capacity ||
            next >
                SIZE_MAX /
                sizeof(*vm->native_roots)
        ) {
            return native_error(
                vm,
                "native root set is too large"
            );
        }

        LuneValue *grown =
            realloc(
                vm->native_roots,
                next *
                    sizeof(*vm->native_roots)
            );

        if (grown == NULL) {
            return native_error(
                vm, "out of memory"
            );
        }

        vm->native_roots = grown;
        vm->native_root_capacity =
            next;
    }

    vm->native_roots[
        vm->native_root_count++
    ] = value;

    return true;
}

static void native_roots_restore(
    LuneVM *vm,
    size_t count
) {
    if (
        count <=
        vm->native_root_count
    ) {
        vm->native_root_count =
            count;
    }
}

static bool pop(
    LuneVM *vm,
    LuneValue *value,
    LuneSpan span
) {
    if (vm->stack_count == 0) {
        return runtime_error(
            vm,
            span,
            "internal runtime stack underflow"
        );
    }

    *value =
        vm->stack[--vm->stack_count];
    return true;
}

static bool peek_value(
    LuneVM *vm,
    LuneValue *value,
    LuneSpan span
) {
    if (vm->stack_count == 0) {
        return runtime_error(
            vm,
            span,
            "internal runtime stack underflow"
        );
    }

    *value =
        vm->stack[
            vm->stack_count - 1
        ];
    return true;
}

static bool read_u16(
    LuneVM *vm,
    CallFrame *frame,
    uint16_t *value,
    LuneSpan span
) {
    if (
        frame->ip + 1 >=
        frame->chunk->count
    ) {
        return runtime_error(
            vm,
            span,
            "truncated bytecode operand"
        );
    }

    *value = (uint16_t)(
        ((uint16_t)
            frame->chunk->code[
                frame->ip
            ] << 8) |
        frame->chunk->code[
            frame->ip + 1
        ]
    );

    frame->ip += 2;
    return true;
}

static bool read_name(
    LuneVM *vm,
    CallFrame *frame,
    uint16_t index,
    LuneSpan span,
    const LuneName **name
) {
    if (
        index >=
        frame->chunk->names_count
    ) {
        return runtime_error(
            vm,
            span,
            "invalid string/name constant"
        );
    }

    *name =
        &frame->chunk->names[index];
    return true;
}

static bool numeric(
    LuneValue value
) {
    return value.kind ==
        LUNE_VALUE_INT ||
        value.kind ==
        LUNE_VALUE_FLOAT;
}

static long double as_number(
    LuneValue value
) {
    return value.kind ==
        LUNE_VALUE_INT
        ? (long double)
            value.as.integer
        : (long double)
            value.as.floating;
}

static LuneObjString *as_string(
    LuneValue value
) {
    if (
        value.kind !=
            LUNE_VALUE_OBJ ||
        !lune_obj_is_string(
            value.as.object
        )
    ) {
        return NULL;
    }

    return (LuneObjString *)
        value.as.object;
}

static LuneObjList *as_list(
    LuneValue value
) {
    if (
        value.kind !=
            LUNE_VALUE_OBJ ||
        !lune_obj_is_list(
            value.as.object
        )
    ) {
        return NULL;
    }

    return (LuneObjList *)
        value.as.object;
}

static LuneObjMap *as_map(
    LuneValue value
) {
    if (
        value.kind !=
            LUNE_VALUE_OBJ ||
        !lune_obj_is_map(
            value.as.object
        )
    ) {
        return NULL;
    }

    return (LuneObjMap *)
        value.as.object;
}

static LuneObjClosure *as_closure(
    LuneValue value
) {
    if (
        value.kind !=
            LUNE_VALUE_OBJ ||
        !lune_obj_is_closure(
            value.as.object
        )
    ) {
        return NULL;
    }

    return (LuneObjClosure *)
        value.as.object;
}

static LuneObjNative *as_native(
    LuneValue value
) {
    if (
        value.kind !=
            LUNE_VALUE_OBJ ||
        !lune_obj_is_native(
            value.as.object
        )
    ) {
        return NULL;
    }

    return (LuneObjNative *)
        value.as.object;
}

static bool int_add(
    int64_t a,
    int64_t b,
    int64_t *out
) {
    if (
        (b > 0 &&
            a > INT64_MAX - b) ||
        (b < 0 &&
            a < INT64_MIN - b)
    ) {
        return false;
    }

    *out = a + b;
    return true;
}

static bool int_sub(
    int64_t a,
    int64_t b,
    int64_t *out
) {
    if (
        (b < 0 &&
            a > INT64_MAX + b) ||
        (b > 0 &&
            a < INT64_MIN + b)
    ) {
        return false;
    }

    *out = a - b;
    return true;
}

static bool int_mul(
    int64_t a,
    int64_t b,
    int64_t *out
) {
    if (a == 0 || b == 0) {
        *out = 0;
        return true;
    }

    if (
        (a == -1 &&
            b == INT64_MIN) ||
        (b == -1 &&
            a == INT64_MIN)
    ) {
        return false;
    }

    if (a > 0) {
        if (
            (b > 0 &&
                a > INT64_MAX / b) ||
            (b < 0 &&
                b < INT64_MIN / a)
        ) {
            return false;
        }
    } else {
        if (
            (b > 0 &&
                a < INT64_MIN / b) ||
            (b < 0 &&
                b < INT64_MAX / a)
        ) {
            return false;
        }
    }

    *out = a * b;
    return true;
}

static bool arithmetic(
    LuneVM *vm,
    LuneOpcode opcode,
    LuneSpan span
) {
    if (
        opcode == LUNE_OP_ADD &&
        vm->stack_count >= 2
    ) {
        LuneValue left =
            vm->stack[
                vm->stack_count - 2
            ];

        LuneValue right =
            vm->stack[
                vm->stack_count - 1
            ];

        LuneObjString *a =
            as_string(left);

        LuneObjString *b =
            as_string(right);

        if (a != NULL || b != NULL) {
            if (
                a == NULL ||
                b == NULL
            ) {
                return runtime_error(
                    vm,
                    span,
                    "addition operands must both be strings or both be numbers"
                );
            }

            /*
             * Keep both operands on the VM stack
             * while allocating. Stress-GC may run
             * inside lune_string_concat().
             */
            LuneObjString *joined =
                lune_string_concat(
                    &vm->heap,
                    a,
                    b
                );

            if (joined == NULL) {
                return runtime_error(
                    vm,
                    span,
                    "out of memory"
                );
            }

            vm->stack_count -= 2;

            return push(
                vm,
                lune_value_obj(
                    (LuneObj *)joined
                ),
                span
            );
        }
    }

    LuneValue right;
    LuneValue left;

    if (
        !pop(vm, &right, span) ||
        !pop(vm, &left, span)
    ) {
        return false;
    }

    if (
        !numeric(left) ||
        !numeric(right)
    ) {
        return runtime_error(
            vm,
            span,
            "arithmetic operands must be numbers"
        );
    }

    if (
        opcode ==
        LUNE_OP_DIVIDE
    ) {
        long double divisor =
            as_number(right);

        if (divisor == 0.0L) {
            return runtime_error(
                vm,
                span,
                "division by zero"
            );
        }

        return push(
            vm,
            lune_value_float(
                (double)(
                    as_number(left) /
                    divisor
                )
            ),
            span
        );
    }

    if (
        opcode ==
        LUNE_OP_MODULO
    ) {
        if (
            left.kind !=
                LUNE_VALUE_INT ||
            right.kind !=
                LUNE_VALUE_INT
        ) {
            return runtime_error(
                vm,
                span,
                "modulo operands must be integers"
            );
        }

        if (
            right.as.integer == 0
        ) {
            return runtime_error(
                vm,
                span,
                "modulo by zero"
            );
        }

        if (
            left.as.integer ==
                INT64_MIN &&
            right.as.integer == -1
        ) {
            return push(
                vm,
                lune_value_int(0),
                span
            );
        }

        return push(
            vm,
            lune_value_int(
                left.as.integer %
                right.as.integer
            ),
            span
        );
    }

    if (
        left.kind ==
            LUNE_VALUE_FLOAT ||
        right.kind ==
            LUNE_VALUE_FLOAT
    ) {
        double a =
            left.kind ==
                LUNE_VALUE_FLOAT
            ? left.as.floating
            : (double)
                left.as.integer;

        double b =
            right.kind ==
                LUNE_VALUE_FLOAT
            ? right.as.floating
            : (double)
                right.as.integer;

        double result =
            opcode ==
                LUNE_OP_ADD
            ? a + b
            : opcode ==
                LUNE_OP_SUBTRACT
                ? a - b
                : a * b;

        return push(
            vm,
            lune_value_float(
                result
            ),
            span
        );
    }

    int64_t result = 0;

    bool ok =
        opcode ==
            LUNE_OP_ADD
        ? int_add(
            left.as.integer,
            right.as.integer,
            &result
        )
        : opcode ==
            LUNE_OP_SUBTRACT
            ? int_sub(
                left.as.integer,
                right.as.integer,
                &result
            )
            : int_mul(
                left.as.integer,
                right.as.integer,
                &result
            );

    if (!ok) {
        return runtime_error(
            vm,
            span,
            "integer overflow"
        );
    }

    return push(
        vm,
        lune_value_int(result),
        span
    );
}

static bool compare(
    LuneVM *vm,
    LuneOpcode opcode,
    LuneSpan span
) {
    LuneValue right;
    LuneValue left;

    if (
        !pop(vm, &right, span) ||
        !pop(vm, &left, span)
    ) {
        return false;
    }

    if (
        !numeric(left) ||
        !numeric(right)
    ) {
        return runtime_error(
            vm,
            span,
            "ordering operands must be numbers"
        );
    }

    long double a =
        as_number(left);

    long double b =
        as_number(right);

    bool result =
        opcode ==
            LUNE_OP_LESS
        ? a < b
        : opcode ==
            LUNE_OP_LESS_EQUAL
            ? a <= b
            : opcode ==
                LUNE_OP_GREATER
                ? a > b
                : a >= b;

    return push(
        vm,
        lune_value_bool(result),
        span
    );
}

static bool get_index(
    LuneVM *vm,
    LuneSpan span
) {
    LuneValue index;
    LuneValue object;

    if (
        !pop(vm, &index, span) ||
        !pop(vm, &object, span)
    ) {
        return false;
    }

    LuneObjList *list =
        as_list(object);

    if (list != NULL) {
        if (
            index.kind !=
                LUNE_VALUE_INT
        ) {
            return runtime_error(
                vm,
                span,
                "list index must be an integer"
            );
        }

        if (
            index.as.integer < 0 ||
            (uint64_t)
                index.as.integer >=
                list->count
        ) {
            return runtime_error(
                vm,
                span,
                "list index is out of range"
            );
        }

        return push(
            vm,
            list->items[
                (size_t)
                    index.as.integer
            ],
            span
        );
    }

    LuneObjMap *map =
        as_map(object);

    if (map != NULL) {
        LuneObjString *key =
            as_string(index);

        if (key == NULL) {
            return runtime_error(
                vm,
                span,
                "map index must be a string"
            );
        }

        LuneValue value;

        if (!lune_map_get(
            map, key, &value
        )) {
            value =
                lune_value_null();
        }

        return push(
            vm, value, span
        );
    }

    return runtime_error(
        vm,
        span,
        "value does not support indexing"
    );
}

static bool set_index(
    LuneVM *vm,
    LuneSpan span
) {
    LuneValue value;
    LuneValue index;
    LuneValue object;

    if (
        !pop(vm, &value, span) ||
        !pop(vm, &index, span) ||
        !pop(vm, &object, span)
    ) {
        return false;
    }

    LuneObjList *list =
        as_list(object);

    if (list != NULL) {
        if (
            index.kind !=
                LUNE_VALUE_INT
        ) {
            return runtime_error(
                vm,
                span,
                "list index must be an integer"
            );
        }

        if (
            index.as.integer < 0 ||
            (uint64_t)
                index.as.integer >=
                list->count
        ) {
            return runtime_error(
                vm,
                span,
                "list index is out of range"
            );
        }

        list->items[
            (size_t)
                index.as.integer
        ] = value;

        return push(
            vm, value, span
        );
    }

    LuneObjMap *map =
        as_map(object);

    if (map != NULL) {
        LuneObjString *key =
            as_string(index);

        if (key == NULL) {
            return runtime_error(
                vm,
                span,
                "map index must be a string"
            );
        }

        if (!lune_map_set(
            &vm->heap,
            map,
            key,
            value
        )) {
            return runtime_error(
                vm,
                span,
                "out of memory"
            );
        }

        return push(
            vm, value, span
        );
    }

    return runtime_error(
        vm,
        span,
        "value does not support indexed assignment"
    );
}

static bool get_field(
    LuneVM *vm,
    CallFrame *frame,
    uint16_t index,
    LuneSpan span
) {
    const LuneName *name;

    if (!read_name(
        vm,
        frame,
        index,
        span,
        &name
    )) {
        return false;
    }

    LuneValue object;

    if (!pop(
        vm, &object, span
    )) {
        return false;
    }

    LuneObjMap *map =
        as_map(object);

    if (map == NULL) {
        return runtime_error(
            vm,
            span,
            "member access requires a map"
        );
    }

    LuneValue value;

    if (!lune_map_get_chars(
        map,
        name->chars,
        name->length,
        &value
    )) {
        value =
            lune_value_null();
    }

    return push(
        vm, value, span
    );
}

static bool set_field(
    LuneVM *vm,
    CallFrame *frame,
    uint16_t index,
    LuneSpan span
) {
    const LuneName *name;

    if (!read_name(
        vm,
        frame,
        index,
        span,
        &name
    )) {
        return false;
    }

    if (vm->stack_count < 2) {
        return runtime_error(
            vm,
            span,
            "internal stack underflow during member assignment"
        );
    }

    LuneValue object =
        vm->stack[
            vm->stack_count - 2
        ];

    LuneValue value =
        vm->stack[
            vm->stack_count - 1
        ];

    LuneObjMap *map =
        as_map(object);

    if (map == NULL) {
        return runtime_error(
            vm,
            span,
            "member assignment requires a map"
        );
    }

    /*
     * Keep object and value on the stack
     * while a new string key may allocate.
     */
    if (!lune_map_set_chars(
        &vm->heap,
        map,
        name->chars,
        name->length,
        value
    )) {
        return runtime_error(
            vm,
            span,
            "out of memory"
        );
    }

    vm->stack_count -= 2;

    return push(
        vm, value, span
    );
}

static LuneObjUpvalue *capture_upvalue(
    LuneVM *vm,
    LuneValue *location
) {
    for (
        LuneObjUpvalue *upvalue =
            vm->open_upvalues;
        upvalue != NULL;
        upvalue =
            upvalue->next_open
    ) {
        if (
            upvalue->location ==
            location
        ) {
            return upvalue;
        }
    }

    LuneObjUpvalue *upvalue =
        lune_upvalue_new(
            &vm->heap,
            location
        );

    if (upvalue == NULL) {
        return NULL;
    }

    upvalue->next_open =
        vm->open_upvalues;

    vm->open_upvalues =
        upvalue;

    return upvalue;
}

static bool upvalue_belongs_to_frame(
    const LuneObjUpvalue *upvalue,
    const CallFrame *frame
) {
    for (
        size_t i = 0;
        i < LOCAL_MAX;
        i++
    ) {
        if (
            upvalue->location ==
            &frame->locals[i]
        ) {
            return true;
        }
    }

    return false;
}

static void close_frame_upvalues(
    LuneVM *vm,
    CallFrame *frame
) {
    LuneObjUpvalue **cursor =
        &vm->open_upvalues;

    while (*cursor != NULL) {
        LuneObjUpvalue *upvalue =
            *cursor;

        if (upvalue_belongs_to_frame(
            upvalue, frame
        )) {
            upvalue->closed =
                *upvalue->location;

            upvalue->location =
                &upvalue->closed;

            *cursor =
                upvalue->next_open;

            upvalue->next_open =
                NULL;
        } else {
            cursor =
                &upvalue->next_open;
        }
    }
}

static void module_entries_clear(
    LuneVM *vm
) {
    for (
        size_t i = 0;
        i < vm->module_count;
        i++
    ) {
        free(vm->modules[i].key);

        if (
            vm->modules[i].chunk !=
            NULL
        ) {
            lune_chunk_free(
                vm->modules[i].chunk
            );
            free(
                vm->modules[i].chunk
            );
        }
    }

    free(vm->modules);
    vm->modules = NULL;
    vm->module_count = 0;
    vm->module_capacity = 0;
}

static ModuleEntry *module_find(
    LuneVM *vm,
    const char *key
) {
    for (
        size_t i = 0;
        i < vm->module_count;
        i++
    ) {
        if (
            strcmp(
                vm->modules[i].key,
                key
            ) == 0
        ) {
            return &vm->modules[i];
        }
    }

    return NULL;
}

static ModuleEntry *module_add(
    LuneVM *vm,
    char *owned_key
) {
    if (
        vm->module_count ==
        vm->module_capacity
    ) {
        size_t next =
            vm->module_capacity == 0
            ? 8
            : vm->module_capacity * 2;

        if (
            next <
                vm->module_capacity ||
            next >
                SIZE_MAX /
                    sizeof(*vm->modules)
        ) {
            free(owned_key);
            return NULL;
        }

        ModuleEntry *grown =
            realloc(
                vm->modules,
                next *
                    sizeof(*vm->modules)
            );

        if (grown == NULL) {
            free(owned_key);
            return NULL;
        }

        vm->modules = grown;
        vm->module_capacity = next;
    }

    ModuleEntry *entry =
        &vm->modules[
            vm->module_count++
        ];

    *entry = (ModuleEntry){
        .key = owned_key,
        .loading = true,
        .value = lune_value_null(),
    };

    return entry;
}

static char *copy_chars(
    const char *chars,
    size_t length
) {
    if (length == SIZE_MAX) {
        return NULL;
    }

    char *copy =
        malloc(length + 1);

    if (copy == NULL) {
        return NULL;
    }

    memcpy(
        copy,
        chars,
        length
    );

    copy[length] = '\0';
    return copy;
}

static char *module_base_directory(
    const char *path
) {
    if (
        path == NULL ||
        path[0] == '\0'
    ) {
        return copy_chars(".", 1);
    }

    const char *slash =
        strrchr(path, '/');

    if (slash == NULL) {
        return copy_chars(".", 1);
    }

    if (slash == path) {
        return copy_chars("/", 1);
    }

    return copy_chars(
        path,
        (size_t)(slash - path)
    );
}

static bool module_file_key(
    LuneVM *vm,
    const LuneObjString *specifier,
    char **key
) {
    if (
        strlen(specifier->chars) !=
        specifier->length
    ) {
        return native_error(
            vm,
            "import path contains NUL"
        );
    }

    const char *current =
        vm->script_path;

    CallFrame *frame =
        current_frame(vm);

    if (
        frame != NULL &&
        frame->module_path != NULL
    ) {
        current =
            frame->module_path;
    }

    char *base =
        module_base_directory(
            current
        );

    if (base == NULL) {
        return native_error(
            vm, "out of memory"
        );
    }

    bool absolute =
        specifier->length > 0 &&
        specifier->chars[0] == '/';

    size_t base_length =
        strlen(base);

    size_t extension =
        (
            specifier->length >= 5 &&
            memcmp(
                specifier->chars +
                    specifier->length - 5,
                ".lune",
                5
            ) == 0
        )
        ? 0
        : 5;

    size_t separator =
        absolute ||
        base_length == 0 ||
        base[
            base_length - 1
        ] == '/'
        ? 0
        : 1;

    size_t joined_length =
        (
            absolute
            ? 0
            : base_length +
                separator
        ) +
        specifier->length +
        extension;

    if (
        joined_length <
        specifier->length
    ) {
        free(base);
        return native_error(
            vm,
            "import path is too large"
        );
    }

    char *joined =
        malloc(
            joined_length + 1
        );

    if (joined == NULL) {
        free(base);
        return native_error(
            vm, "out of memory"
        );
    }

    size_t offset = 0;

    if (!absolute) {
        memcpy(
            joined,
            base,
            base_length
        );

        offset = base_length;

        if (separator != 0) {
            joined[offset++] = '/';
        }
    }

    memcpy(
        joined + offset,
        specifier->chars,
        specifier->length
    );

    offset += specifier->length;

    if (extension != 0) {
        memcpy(
            joined + offset,
            ".lune",
            5
        );

        offset += 5;
    }

    joined[offset] = '\0';
    free(base);

    char error[256];
    char *canonical = NULL;

    if (!lune_platform_canonical_path(
        joined,
        &canonical,
        error,
        sizeof(error)
    )) {
        free(joined);
        return native_error(
            vm, error
        );
    }

    free(joined);
    *key = canonical;
    return true;
}

static bool native_error(
    LuneVM *vm,
    const char *message
) {
    return runtime_error(
        vm,
        vm->native_span,
        message
    );
}

static bool make_string_value(
    LuneVM *vm,
    const char *chars,
    size_t length,
    LuneValue *result
) {
    LuneObjString *string =
        lune_string_new(
            &vm->heap,
            chars,
            length
        );

    if (string == NULL) {
        return native_error(
            vm, "out of memory"
        );
    }

    *result = lune_value_obj(
        (LuneObj *)string
    );
    return true;
}

static bool native_print(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)vm;

    for (
        int i = 0;
        i < argc;
        i++
    ) {
        if (i != 0) {
            fputc(' ', stdout);
        }

        lune_value_print(
            stdout, args[i]
        );
    }

    fputc('\n', stdout);
    *result = lune_value_null();
    return true;
}

static const char *value_type_name(
    LuneValue value
) {
    switch (value.kind) {
        case LUNE_VALUE_NULL:
            return "null";
        case LUNE_VALUE_BOOL:
            return "bool";
        case LUNE_VALUE_INT:
            return "int";
        case LUNE_VALUE_FLOAT:
            return "float";
        case LUNE_VALUE_OBJ:
            break;
    }

    if (lune_obj_is_string(value.as.object)) {
        return "string";
    }
    if (lune_obj_is_list(value.as.object)) {
        return "list";
    }
    if (lune_obj_is_map(value.as.object)) {
        return "map";
    }
    if (
        lune_obj_is_closure(value.as.object) ||
        lune_obj_is_native(value.as.object)
    ) {
        return "function";
    }

    return "unknown";
}

static bool native_type(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;
    const char *name =
        value_type_name(args[0]);

    return make_string_value(
        vm,
        name,
        strlen(name),
        result
    );
}

static bool native_len(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    if (
        args[0].kind ==
            LUNE_VALUE_OBJ &&
        lune_obj_is_string(
            args[0].as.object
        )
    ) {
        LuneObjString *string =
            (LuneObjString *)
                args[0].as.object;

        if (
            string->length >
            (size_t)INT64_MAX
        ) {
            return native_error(
                vm,
                "string length is too large"
            );
        }

        *result = lune_value_int(
            (int64_t)string->length
        );
        return true;
    }

    if (
        args[0].kind ==
            LUNE_VALUE_OBJ &&
        lune_obj_is_list(
            args[0].as.object
        )
    ) {
        LuneObjList *list =
            (LuneObjList *)
                args[0].as.object;

        if (
            list->count >
            (size_t)INT64_MAX
        ) {
            return native_error(
                vm,
                "list length is too large"
            );
        }

        *result = lune_value_int(
            (int64_t)list->count
        );
        return true;
    }

    if (
        args[0].kind ==
            LUNE_VALUE_OBJ &&
        lune_obj_is_map(
            args[0].as.object
        )
    ) {
        LuneObjMap *map =
            (LuneObjMap *)
                args[0].as.object;

        if (
            map->count >
            (size_t)INT64_MAX
        ) {
            return native_error(
                vm,
                "map length is too large"
            );
        }

        *result = lune_value_int(
            (int64_t)map->count
        );
        return true;
    }

    return native_error(
        vm,
        "len() expects a string, list, or map"
    );
}

static bool native_push(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjList *list =
        as_list(args[0]);

    if (list == NULL) {
        return native_error(
            vm,
            "push() expects a list"
        );
    }

    if (!lune_list_push(
        &vm->heap,
        list,
        args[1]
    )) {
        return native_error(
            vm, "out of memory"
        );
    }

    *result = args[0];
    return true;
}

static bool native_pop(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjList *list =
        as_list(args[0]);

    if (list == NULL) {
        return native_error(
            vm,
            "pop() expects a list"
        );
    }

    if (!lune_list_pop(
        list, result
    )) {
        return native_error(
            vm,
            "pop() cannot remove from an empty list"
        );
    }

    return true;
}

static bool native_byte_at(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjString *string =
        as_string(args[0]);

    if (
        string == NULL ||
        args[1].kind !=
            LUNE_VALUE_INT
    ) {
        return native_error(
            vm,
            "byte_at() expects a string and integer index"
        );
    }

    if (
        args[1].as.integer < 0 ||
        (uint64_t)
            args[1].as.integer >=
            string->length
    ) {
        return native_error(
            vm,
            "byte_at() index is out of range"
        );
    }

    unsigned char byte =
        (unsigned char)
            string->chars[
                (size_t)
                    args[1].as.integer
            ];

    *result = lune_value_int(
        (int64_t)byte
    );
    return true;
}

static bool slice_bounds(
    LuneVM *vm,
    LuneValue start_value,
    LuneValue end_value,
    size_t length,
    size_t *start,
    size_t *end
) {
    if (
        start_value.kind !=
            LUNE_VALUE_INT ||
        end_value.kind !=
            LUNE_VALUE_INT
    ) {
        return native_error(
            vm,
            "slice() bounds must be integers"
        );
    }

    if (
        start_value.as.integer < 0 ||
        end_value.as.integer < 0
    ) {
        return native_error(
            vm,
            "slice() bounds cannot be negative"
        );
    }

    uint64_t start_u =
        (uint64_t)
            start_value.as.integer;
    uint64_t end_u =
        (uint64_t)
            end_value.as.integer;

    if (
        start_u > end_u ||
        end_u > length
    ) {
        return native_error(
            vm,
            "slice() bounds are out of range"
        );
    }

    *start = (size_t)start_u;
    *end = (size_t)end_u;
    return true;
}

static bool native_slice(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    size_t start = 0;
    size_t end = 0;

    LuneObjString *string =
        as_string(args[0]);

    if (string != NULL) {
        if (!slice_bounds(
            vm,
            args[1],
            args[2],
            string->length,
            &start,
            &end
        )) {
            return false;
        }

        return make_string_value(
            vm,
            string->chars + start,
            end - start,
            result
        );
    }

    LuneObjList *list =
        as_list(args[0]);

    if (list != NULL) {
        if (!slice_bounds(
            vm,
            args[1],
            args[2],
            list->count,
            &start,
            &end
        )) {
            return false;
        }

        LuneObjList *copy =
            lune_list_new(
                &vm->heap,
                list->items + start,
                end - start
            );

        if (copy == NULL) {
            return native_error(
                vm, "out of memory"
            );
        }

        *result = lune_value_obj(
            (LuneObj *)copy
        );
        return true;
    }

    return native_error(
        vm,
        "slice() expects a string or list"
    );
}

static bool native_bytes(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjList *list =
        as_list(args[0]);

    if (list == NULL) {
        return native_error(
            vm,
            "bytes() expects a list"
        );
    }

    char *buffer = NULL;

    if (list->count > 0) {
        buffer = malloc(
            list->count
        );

        if (buffer == NULL) {
            return native_error(
                vm, "out of memory"
            );
        }
    }

    for (
        size_t i = 0;
        i < list->count;
        i++
    ) {
        LuneValue value =
            list->items[i];

        if (
            value.kind !=
                LUNE_VALUE_INT ||
            value.as.integer < 0 ||
            value.as.integer > 255
        ) {
            free(buffer);
            return native_error(
                vm,
                "bytes() elements must be integers from 0 through 255"
            );
        }

        buffer[i] =
            (char)(unsigned char)
                value.as.integer;
    }

    bool ok = make_string_value(
        vm,
        buffer,
        list->count,
        result
    );

    free(buffer);
    return ok;
}

static bool scalar_text(
    LuneVM *vm,
    LuneValue value,
    char buffer[64],
    const char **chars,
    size_t *length
) {
    if (
        value.kind ==
            LUNE_VALUE_OBJ &&
        lune_obj_is_string(
            value.as.object
        )
    ) {
        LuneObjString *string =
            (LuneObjString *)
                value.as.object;

        *chars = string->chars;
        *length = string->length;
        return true;
    }

    switch (value.kind) {
        case LUNE_VALUE_NULL:
            *chars = "null";
            *length = 4;
            return true;

        case LUNE_VALUE_BOOL:
            *chars = value.as.boolean
                ? "true"
                : "false";
            *length = value.as.boolean
                ? 4
                : 5;
            return true;

        case LUNE_VALUE_INT: {
            int written = snprintf(
                buffer,
                64,
                "%lld",
                (long long)
                    value.as.integer
            );

            if (
                written < 0 ||
                written >= 64
            ) {
                return native_error(
                    vm,
                    "integer conversion failed"
                );
            }

            *chars = buffer;
            *length = (size_t)written;
            return true;
        }

        case LUNE_VALUE_FLOAT: {
            int written = snprintf(
                buffer,
                64,
                "%.17g",
                value.as.floating
            );

            if (
                written < 0 ||
                written >= 64
            ) {
                return native_error(
                    vm,
                    "float conversion failed"
                );
            }

            *chars = buffer;
            *length = (size_t)written;
            return true;
        }

        case LUNE_VALUE_OBJ:
            return native_error(
                vm,
                "value cannot be converted to text"
            );
    }

    return native_error(
        vm,
        "value cannot be converted to text"
    );
}

static bool native_str(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    if (
        args[0].kind ==
            LUNE_VALUE_OBJ &&
        lune_obj_is_string(
            args[0].as.object
        )
    ) {
        *result = args[0];
        return true;
    }

    char buffer[64];
    const char *chars = NULL;
    size_t length = 0;

    if (!scalar_text(
        vm,
        args[0],
        buffer,
        &chars,
        &length
    )) {
        return false;
    }

    return make_string_value(
        vm,
        chars,
        length,
        result
    );
}

static bool string_to_int(
    LuneVM *vm,
    LuneObjString *string,
    LuneValue *result
) {
    errno = 0;
    char *end = NULL;

    long long value =
        strtoll(
            string->chars,
            &end,
            10
        );

    if (
        errno == ERANGE ||
        end == string->chars ||
        end !=
            string->chars +
                string->length
    ) {
        return native_error(
            vm,
            "int() could not parse string"
        );
    }

    *result = lune_value_int(
        (int64_t)value
    );
    return true;
}

static bool native_int(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    if (
        args[0].kind ==
        LUNE_VALUE_INT
    ) {
        *result = args[0];
        return true;
    }

    if (
        args[0].kind ==
        LUNE_VALUE_FLOAT
    ) {
        long double value =
            (long double)
                args[0].as.floating;

        if (
            !isfinite(
                args[0].as.floating
            ) ||
            value <
                (long double)INT64_MIN ||
            value >
                (long double)INT64_MAX
        ) {
            return native_error(
                vm,
                "int() value is out of range"
            );
        }

        *result = lune_value_int(
            (int64_t)
                args[0].as.floating
        );
        return true;
    }

    if (
        args[0].kind ==
            LUNE_VALUE_OBJ &&
        lune_obj_is_string(
            args[0].as.object
        )
    ) {
        return string_to_int(
            vm,
            (LuneObjString *)
                args[0].as.object,
            result
        );
    }

    return native_error(
        vm,
        "int() expects an int, float, or string"
    );
}

static bool native_float(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    if (
        args[0].kind ==
        LUNE_VALUE_FLOAT
    ) {
        *result = args[0];
        return true;
    }

    if (
        args[0].kind ==
        LUNE_VALUE_INT
    ) {
        *result = lune_value_float(
            (double)args[0].as.integer
        );
        return true;
    }

    if (
        args[0].kind ==
            LUNE_VALUE_OBJ &&
        lune_obj_is_string(
            args[0].as.object
        )
    ) {
        LuneObjString *string =
            (LuneObjString *)
                args[0].as.object;

        errno = 0;
        char *end = NULL;
        double value = strtod(
            string->chars,
            &end
        );

        if (
            errno == ERANGE ||
            end == string->chars ||
            end !=
                string->chars +
                    string->length
        ) {
            return native_error(
                vm,
                "float() could not parse string"
            );
        }

        *result =
            lune_value_float(value);
        return true;
    }

    return native_error(
        vm,
        "float() expects a number or string"
    );
}

static bool native_bool(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)vm;
    (void)argc;

    *result = lune_value_bool(
        lune_value_truthy(args[0])
    );
    return true;
}

static bool string_find_bytes(
    const LuneObjString *haystack,
    const LuneObjString *needle,
    size_t start,
    size_t *index
) {
    if (start > haystack->length) {
        return false;
    }

    if (needle->length == 0) {
        *index = start;
        return true;
    }

    if (
        needle->length >
        haystack->length - start
    ) {
        return false;
    }

    size_t last =
        haystack->length -
        needle->length;

    for (
        size_t i = start;
        i <= last;
        i++
    ) {
        if (
            memcmp(
                haystack->chars + i,
                needle->chars,
                needle->length
            ) == 0
        ) {
            *index = i;
            return true;
        }
    }

    return false;
}

static bool native_contains(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjString *haystack =
        as_string(args[0]);
    LuneObjString *needle =
        as_string(args[1]);

    if (
        haystack == NULL ||
        needle == NULL
    ) {
        return native_error(
            vm,
            "contains() expects two strings"
        );
    }

    size_t index = 0;

    *result = lune_value_bool(
        string_find_bytes(
            haystack,
            needle,
            0,
            &index
        )
    );

    return true;
}

static bool native_find(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjString *haystack =
        as_string(args[0]);
    LuneObjString *needle =
        as_string(args[1]);

    if (
        haystack == NULL ||
        needle == NULL
    ) {
        return native_error(
            vm,
            "find() expects two strings"
        );
    }

    size_t index = 0;

    if (!string_find_bytes(
        haystack,
        needle,
        0,
        &index
    )) {
        *result = lune_value_null();
        return true;
    }

    if (
        index >
        (size_t)INT64_MAX
    ) {
        return native_error(
            vm,
            "string index is too large"
        );
    }

    *result = lune_value_int(
        (int64_t)index
    );
    return true;
}

static bool native_split(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjString *text =
        as_string(args[0]);
    LuneObjString *separator =
        as_string(args[1]);

    if (
        text == NULL ||
        separator == NULL
    ) {
        return native_error(
            vm,
            "split() expects two strings"
        );
    }

    if (separator->length == 0) {
        return native_error(
            vm,
            "split() separator must not be empty"
        );
    }

    size_t count = 1;
    size_t scan = 0;
    size_t found = 0;

    while (string_find_bytes(
        text,
        separator,
        scan,
        &found
    )) {
        if (count == SIZE_MAX) {
            return native_error(
                vm,
                "split() result is too large"
            );
        }

        count++;
        scan =
            found +
            separator->length;
    }

    if (
        count >
        SIZE_MAX /
            sizeof(LuneValue)
    ) {
        return native_error(
            vm,
            "split() result is too large"
        );
    }

    LuneValue *items =
        calloc(
            count,
            sizeof(*items)
        );

    if (
        items == NULL &&
        count != 0
    ) {
        return native_error(
            vm, "out of memory"
        );
    }

    LuneObjList *list =
        lune_list_new(
            &vm->heap,
            items,
            count
        );

    free(items);

    if (list == NULL) {
        return native_error(
            vm, "out of memory"
        );
    }

    LuneValue list_value =
        lune_value_obj(
            (LuneObj *)list
        );

    if (!push(
        vm,
        list_value,
        vm->native_span
    )) {
        return false;
    }

    scan = 0;
    size_t item = 0;

    while (
        item + 1 < count &&
        string_find_bytes(
            text,
            separator,
            scan,
            &found
        )
    ) {
        LuneObjString *piece =
            lune_string_new(
                &vm->heap,
                text->chars + scan,
                found - scan
            );

        if (piece == NULL) {
            return native_error(
                vm, "out of memory"
            );
        }

        list->items[item++] =
            lune_value_obj(
                (LuneObj *)piece
            );

        scan =
            found +
            separator->length;
    }

    LuneObjString *piece =
        lune_string_new(
            &vm->heap,
            text->chars + scan,
            text->length - scan
        );

    if (piece == NULL) {
        return native_error(
            vm, "out of memory"
        );
    }

    list->items[item] =
        lune_value_obj(
            (LuneObj *)piece
        );

    LuneValue rooted;

    if (!pop(
        vm,
        &rooted,
        vm->native_span
    )) {
        return false;
    }

    *result = rooted;
    return true;
}

static bool native_join(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjList *items =
        as_list(args[0]);
    LuneObjString *separator =
        as_string(args[1]);

    if (
        items == NULL ||
        separator == NULL
    ) {
        return native_error(
            vm,
            "join() expects a list and a string separator"
        );
    }

    size_t length = 0;

    for (
        size_t i = 0;
        i < items->count;
        i++
    ) {
        LuneObjString *item =
            as_string(
                items->items[i]
            );

        if (item == NULL) {
            return native_error(
                vm,
                "join() list items must be strings"
            );
        }

        if (
            item->length >
            SIZE_MAX - length
        ) {
            return native_error(
                vm,
                "join() result is too large"
            );
        }

        length += item->length;

        if (
            i + 1 < items->count
        ) {
            if (
                separator->length >
                SIZE_MAX - length
            ) {
                return native_error(
                    vm,
                    "join() result is too large"
                );
            }

            length +=
                separator->length;
        }
    }

    char *buffer =
        malloc(length);

    if (
        buffer == NULL &&
        length != 0
    ) {
        return native_error(
            vm, "out of memory"
        );
    }

    size_t offset = 0;

    for (
        size_t i = 0;
        i < items->count;
        i++
    ) {
        LuneObjString *item =
            (LuneObjString *)
                items->items[i]
                    .as.object;

        memcpy(
            buffer + offset,
            item->chars,
            item->length
        );

        offset += item->length;

        if (
            i + 1 < items->count
        ) {
            memcpy(
                buffer + offset,
                separator->chars,
                separator->length
            );

            offset +=
                separator->length;
        }
    }

    bool ok = make_string_value(
        vm,
        buffer,
        length,
        result
    );

    free(buffer);
    return ok;
}

static bool native_format(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjString *template =
        as_string(args[0]);
    LuneObjList *values =
        as_list(args[1]);

    if (
        template == NULL ||
        values == NULL
    ) {
        return native_error(
            vm,
            "format() expects a string template and a list"
        );
    }

    size_t output_length = 0;
    size_t value_index = 0;
    size_t i = 0;

    while (i < template->length) {
        if (
            i + 1 <
                template->length &&
            template->chars[i] == '{' &&
            template->chars[i + 1] == '}'
        ) {
            if (
                value_index >=
                values->count
            ) {
                return native_error(
                    vm,
                    "format() has more placeholders than values"
                );
            }

            char scalar_buffer[64];
            const char *chars = NULL;
            size_t length = 0;

            if (!scalar_text(
                vm,
                values->items[
                    value_index
                ],
                scalar_buffer,
                &chars,
                &length
            )) {
                return false;
            }

            (void)chars;

            if (
                length >
                SIZE_MAX -
                    output_length
            ) {
                return native_error(
                    vm,
                    "format() result is too large"
                );
            }

            output_length += length;
            value_index++;
            i += 2;
            continue;
        }

        if (
            output_length ==
            SIZE_MAX
        ) {
            return native_error(
                vm,
                "format() result is too large"
            );
        }

        output_length++;
        i++;
    }

    if (
        value_index !=
        values->count
    ) {
        return native_error(
            vm,
            "format() has more values than placeholders"
        );
    }

    char *buffer =
        malloc(output_length);

    if (
        buffer == NULL &&
        output_length != 0
    ) {
        return native_error(
            vm, "out of memory"
        );
    }

    size_t output = 0;
    value_index = 0;
    i = 0;

    while (i < template->length) {
        if (
            i + 1 <
                template->length &&
            template->chars[i] == '{' &&
            template->chars[i + 1] == '}'
        ) {
            char scalar_buffer[64];
            const char *chars = NULL;
            size_t length = 0;

            if (!scalar_text(
                vm,
                values->items[
                    value_index++
                ],
                scalar_buffer,
                &chars,
                &length
            )) {
                free(buffer);
                return false;
            }

            memcpy(
                buffer + output,
                chars,
                length
            );

            output += length;
            i += 2;
            continue;
        }

        buffer[output++] =
            template->chars[i++];
    }

    bool ok = make_string_value(
        vm,
        buffer,
        output_length,
        result
    );

    free(buffer);
    return ok;
}

static bool native_each(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjList *items =
        as_list(args[0]);

    if (items == NULL) {
        return native_error(
            vm,
            "each() expects a list and a function"
        );
    }

    for (
        size_t i = 0;
        i < items->count;
        i++
    ) {
        LuneValue call_result =
            lune_value_null();

        if (!invoke_callable(
            vm,
            args[1],
            1,
            &items->items[i],
            &call_result
        )) {
            return false;
        }

        if (vm->exit_requested) {
            *result = lune_value_null();
            return true;
        }
    }

    *result = args[0];
    return true;
}

static bool native_map(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjList *items =
        as_list(args[0]);

    if (items == NULL) {
        return native_error(
            vm,
            "map() expects a list and a function"
        );
    }

    if (
        items->count >
        SIZE_MAX /
            sizeof(LuneValue)
    ) {
        return native_error(
            vm,
            "map() result is too large"
        );
    }

    LuneValue *empty = NULL;

    if (items->count > 0) {
        empty = calloc(
            items->count,
            sizeof(*empty)
        );

        if (empty == NULL) {
            return native_error(
                vm, "out of memory"
            );
        }
    }

    LuneObjList *mapped =
        lune_list_new(
            &vm->heap,
            empty,
            items->count
        );

    free(empty);

    if (mapped == NULL) {
        return native_error(
            vm, "out of memory"
        );
    }

    LuneValue mapped_value =
        lune_value_obj(
            (LuneObj *)mapped
        );

    if (!push(
        vm,
        mapped_value,
        vm->native_span
    )) {
        return false;
    }

    for (
        size_t i = 0;
        i < items->count;
        i++
    ) {
        LuneValue call_result =
            lune_value_null();

        if (!invoke_callable(
            vm,
            args[1],
            1,
            &items->items[i],
            &call_result
        )) {
            return false;
        }

        if (vm->exit_requested) {
            *result = lune_value_null();
            return true;
        }

        mapped->items[i] =
            call_result;
    }

    LuneValue rooted;

    if (!pop(
        vm,
        &rooted,
        vm->native_span
    )) {
        return false;
    }

    *result = rooted;
    return true;
}

static bool native_filter(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjList *items =
        as_list(args[0]);

    if (items == NULL) {
        return native_error(
            vm,
            "filter() expects a list and a function"
        );
    }

    LuneValue *kept = NULL;

    if (items->count > 0) {
        if (
            items->count >
            SIZE_MAX /
                sizeof(*kept)
        ) {
            return native_error(
                vm,
                "filter() result is too large"
            );
        }

        kept = malloc(
            items->count *
            sizeof(*kept)
        );

        if (kept == NULL) {
            return native_error(
                vm, "out of memory"
            );
        }
    }

    size_t kept_count = 0;

    for (
        size_t i = 0;
        i < items->count;
        i++
    ) {
        LuneValue predicate =
            lune_value_null();

        if (!invoke_callable(
            vm,
            args[1],
            1,
            &items->items[i],
            &predicate
        )) {
            free(kept);
            return false;
        }

        if (vm->exit_requested) {
            free(kept);
            *result = lune_value_null();
            return true;
        }

        if (lune_value_truthy(predicate)) {
            kept[kept_count++] =
                items->items[i];
        }
    }

    LuneObjList *filtered =
        lune_list_new(
            &vm->heap,
            kept,
            kept_count
        );

    free(kept);

    if (filtered == NULL) {
        return native_error(
            vm, "out of memory"
        );
    }

    *result = lune_value_obj(
        (LuneObj *)filtered
    );
    return true;
}

static bool native_reduce(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjList *items =
        as_list(args[0]);

    if (items == NULL) {
        return native_error(
            vm,
            "reduce() expects a list, initial value, and function"
        );
    }

    LuneValue accumulator =
        args[1];

    for (
        size_t i = 0;
        i < items->count;
        i++
    ) {
        LuneValue callback_args[2] = {
            accumulator,
            items->items[i],
        };

        LuneValue next =
            lune_value_null();

        if (!invoke_callable(
            vm,
            args[2],
            2,
            callback_args,
            &next
        )) {
            return false;
        }

        if (vm->exit_requested) {
            *result = lune_value_null();
            return true;
        }

        accumulator = next;
    }

    *result = accumulator;
    return true;
}


typedef struct {
    char *data;
    size_t count;
    size_t capacity;
} ByteBuffer;

static void byte_buffer_free(
    ByteBuffer *buffer
) {
    free(buffer->data);
    *buffer = (ByteBuffer){0};
}

static bool byte_buffer_reserve(
    ByteBuffer *buffer,
    size_t extra
) {
    if (
        extra >
        SIZE_MAX - buffer->count
    ) {
        return false;
    }

    size_t needed =
        buffer->count + extra;

    if (
        needed <=
        buffer->capacity
    ) {
        return true;
    }

    size_t next =
        buffer->capacity == 0
        ? 64
        : buffer->capacity;

    while (next < needed) {
        if (next > SIZE_MAX / 2) {
            next = needed;
            break;
        }

        next *= 2;
    }

    char *grown = realloc(
        buffer->data,
        next
    );

    if (grown == NULL) {
        return false;
    }

    buffer->data = grown;
    buffer->capacity = next;
    return true;
}

static bool byte_buffer_append(
    ByteBuffer *buffer,
    const char *data,
    size_t length
) {
    if (!byte_buffer_reserve(
        buffer, length
    )) {
        return false;
    }

    memcpy(
        buffer->data +
            buffer->count,
        data,
        length
    );

    buffer->count += length;
    return true;
}

static bool byte_buffer_byte(
    ByteBuffer *buffer,
    char byte
) {
    return byte_buffer_append(
        buffer, &byte, 1
    );
}

typedef struct {
    LuneVM *vm;
    const char *chars;
    size_t length;
    size_t position;
    unsigned depth;
} JsonParser;

static bool json_error(
    JsonParser *parser,
    const char *message
) {
    char diagnostic[256];

    (void)snprintf(
        diagnostic,
        sizeof(diagnostic),
        "json_parse(): %s at byte %zu",
        message,
        parser->position
    );

    return native_error(
        parser->vm,
        diagnostic
    );
}

static void json_skip_space(
    JsonParser *parser
) {
    while (
        parser->position <
        parser->length
    ) {
        char c =
            parser->chars[
                parser->position
            ];

        if (
            c != ' ' &&
            c != '\t' &&
            c != '\r' &&
            c != '\n'
        ) {
            break;
        }

        parser->position++;
    }
}

static int json_hex(char c) {
    if (
        c >= '0' &&
        c <= '9'
    ) {
        return c - '0';
    }

    if (
        c >= 'a' &&
        c <= 'f'
    ) {
        return 10 + c - 'a';
    }

    if (
        c >= 'A' &&
        c <= 'F'
    ) {
        return 10 + c - 'A';
    }

    return -1;
}

static bool json_append_utf8(
    ByteBuffer *buffer,
    uint32_t codepoint
) {
    char encoded[4];
    size_t count = 0;

    if (codepoint <= 0x7f) {
        encoded[0] =
            (char)codepoint;
        count = 1;
    } else if (
        codepoint <= 0x7ff
    ) {
        encoded[0] = (char)(
            0xc0u |
            (codepoint >> 6)
        );

        encoded[1] = (char)(
            0x80u |
            (codepoint & 0x3fu)
        );

        count = 2;
    } else if (
        codepoint <= 0xffff
    ) {
        if (
            codepoint >= 0xd800 &&
            codepoint <= 0xdfff
        ) {
            return false;
        }

        encoded[0] = (char)(
            0xe0u |
            (codepoint >> 12)
        );

        encoded[1] = (char)(
            0x80u |
            ((codepoint >> 6) &
                0x3fu)
        );

        encoded[2] = (char)(
            0x80u |
            (codepoint & 0x3fu)
        );

        count = 3;
    } else if (
        codepoint <= 0x10ffff
    ) {
        encoded[0] = (char)(
            0xf0u |
            (codepoint >> 18)
        );

        encoded[1] = (char)(
            0x80u |
            ((codepoint >> 12) &
                0x3fu)
        );

        encoded[2] = (char)(
            0x80u |
            ((codepoint >> 6) &
                0x3fu)
        );

        encoded[3] = (char)(
            0x80u |
            (codepoint & 0x3fu)
        );

        count = 4;
    } else {
        return false;
    }

    return byte_buffer_append(
        buffer,
        encoded,
        count
    );
}

static bool json_read_u16(
    JsonParser *parser,
    uint32_t *value
) {
    if (
        parser->position + 4 >
        parser->length
    ) {
        return json_error(
            parser,
            "truncated unicode escape"
        );
    }

    uint32_t code = 0;

    for (int i = 0; i < 4; i++) {
        int hex = json_hex(
            parser->chars[
                parser->position++
            ]
        );

        if (hex < 0) {
            return json_error(
                parser,
                "invalid unicode escape"
            );
        }

        code =
            code * 16u +
            (uint32_t)hex;
    }

    *value = code;
    return true;
}

static bool json_parse_string_bytes(
    JsonParser *parser,
    ByteBuffer *buffer
) {
    if (
        parser->position >=
            parser->length ||
        parser->chars[
            parser->position
        ] != '"'
    ) {
        return json_error(
            parser,
            "expected string"
        );
    }

    parser->position++;

    while (
        parser->position <
        parser->length
    ) {
        unsigned char c =
            (unsigned char)
                parser->chars[
                    parser->position++
                ];

        if (c == '"') {
            return true;
        }

        if (c < 0x20u) {
            return json_error(
                parser,
                "control byte in string"
            );
        }

        if (c != '\\') {
            if (!byte_buffer_byte(
                buffer, (char)c
            )) {
                return native_error(
                    parser->vm,
                    "out of memory"
                );
            }

            continue;
        }

        if (
            parser->position >=
            parser->length
        ) {
            return json_error(
                parser,
                "truncated escape"
            );
        }

        char escape =
            parser->chars[
                parser->position++
            ];

        switch (escape) {
            case '"':
            case '\\':
            case '/':
                if (!byte_buffer_byte(
                    buffer, escape
                )) {
                    return native_error(
                        parser->vm,
                        "out of memory"
                    );
                }
                break;

            case 'b':
                if (!byte_buffer_byte(
                    buffer, '\b'
                )) {
                    return native_error(
                        parser->vm,
                        "out of memory"
                    );
                }
                break;

            case 'f':
                if (!byte_buffer_byte(
                    buffer, '\f'
                )) {
                    return native_error(
                        parser->vm,
                        "out of memory"
                    );
                }
                break;

            case 'n':
                if (!byte_buffer_byte(
                    buffer, '\n'
                )) {
                    return native_error(
                        parser->vm,
                        "out of memory"
                    );
                }
                break;

            case 'r':
                if (!byte_buffer_byte(
                    buffer, '\r'
                )) {
                    return native_error(
                        parser->vm,
                        "out of memory"
                    );
                }
                break;

            case 't':
                if (!byte_buffer_byte(
                    buffer, '\t'
                )) {
                    return native_error(
                        parser->vm,
                        "out of memory"
                    );
                }
                break;

            case 'u': {
                uint32_t codepoint = 0;

                if (!json_read_u16(
                    parser,
                    &codepoint
                )) {
                    return false;
                }

                if (
                    codepoint >= 0xd800u &&
                    codepoint <= 0xdbffu
                ) {
                    if (
                        parser->position + 2 >
                            parser->length ||
                        parser->chars[
                            parser->position
                        ] != '\\' ||
                        parser->chars[
                            parser->position + 1
                        ] != 'u'
                    ) {
                        return json_error(
                            parser,
                            "missing low surrogate"
                        );
                    }

                    parser->position += 2;

                    uint32_t low = 0;

                    if (!json_read_u16(
                        parser, &low
                    )) {
                        return false;
                    }

                    if (
                        low < 0xdc00u ||
                        low > 0xdfffu
                    ) {
                        return json_error(
                            parser,
                            "invalid low surrogate"
                        );
                    }

                    codepoint =
                        0x10000u +
                        ((codepoint -
                            0xd800u) << 10) +
                        (low - 0xdc00u);
                } else if (
                    codepoint >= 0xdc00u &&
                    codepoint <= 0xdfffu
                ) {
                    return json_error(
                        parser,
                        "unexpected low surrogate"
                    );
                }

                if (!json_append_utf8(
                    buffer,
                    codepoint
                )) {
                    return native_error(
                        parser->vm,
                        "out of memory"
                    );
                }
                break;
            }

            default:
                return json_error(
                    parser,
                    "invalid string escape"
                );
        }
    }

    return json_error(
        parser,
        "unterminated string"
    );
}

static bool json_parse_value(
    JsonParser *parser,
    LuneValue *result
);

static bool json_parse_array(
    JsonParser *parser,
    LuneValue *result
) {
    if (parser->depth >= 128) {
        return json_error(
            parser,
            "nesting is too deep"
        );
    }

    parser->depth++;
    parser->position++;

    size_t roots =
        parser->vm->native_root_count;

    json_skip_space(parser);

    if (
        parser->position <
            parser->length &&
        parser->chars[
            parser->position
        ] == ']'
    ) {
        parser->position++;

        LuneObjList *list =
            lune_list_new(
                &parser->vm->heap,
                NULL,
                0
            );

        parser->depth--;

        if (list == NULL) {
            return native_error(
                parser->vm,
                "out of memory"
            );
        }

        *result = lune_value_obj(
            (LuneObj *)list
        );
        return true;
    }

    for (;;) {
        LuneValue value =
            lune_value_null();

        if (!json_parse_value(
            parser, &value
        )) {
            native_roots_restore(
                parser->vm, roots
            );
            parser->depth--;
            return false;
        }

        if (!native_root_push(
            parser->vm, value
        )) {
            native_roots_restore(
                parser->vm, roots
            );
            parser->depth--;
            return false;
        }

        json_skip_space(parser);

        if (
            parser->position >=
            parser->length
        ) {
            native_roots_restore(
                parser->vm, roots
            );
            parser->depth--;
            return json_error(
                parser,
                "unterminated array"
            );
        }

        char c =
            parser->chars[
                parser->position++
            ];

        if (c == ']') {
            break;
        }

        if (c != ',') {
            native_roots_restore(
                parser->vm, roots
            );
            parser->depth--;
            return json_error(
                parser,
                "expected ',' or ']'"
            );
        }

        json_skip_space(parser);
    }

    size_t count =
        parser->vm->native_root_count -
        roots;

    const LuneValue *items =
        count == 0
        ? NULL
        : parser->vm->native_roots +
            roots;

    LuneObjList *list =
        lune_list_new(
            &parser->vm->heap,
            items,
            count
        );

    native_roots_restore(
        parser->vm, roots
    );

    parser->depth--;

    if (list == NULL) {
        return native_error(
            parser->vm,
            "out of memory"
        );
    }

    *result = lune_value_obj(
        (LuneObj *)list
    );

    return true;
}

static bool json_parse_object(
    JsonParser *parser,
    LuneValue *result
) {
    if (parser->depth >= 128) {
        return json_error(
            parser,
            "nesting is too deep"
        );
    }

    parser->depth++;
    parser->position++;

    LuneObjMap *map =
        lune_map_new(
            &parser->vm->heap
        );

    if (map == NULL) {
        parser->depth--;
        return native_error(
            parser->vm,
            "out of memory"
        );
    }

    size_t roots =
        parser->vm->native_root_count;

    if (!native_root_push(
        parser->vm,
        lune_value_obj(
            (LuneObj *)map
        )
    )) {
        parser->depth--;
        return false;
    }

    json_skip_space(parser);

    if (
        parser->position <
            parser->length &&
        parser->chars[
            parser->position
        ] == '}'
    ) {
        parser->position++;

        native_roots_restore(
            parser->vm, roots
        );

        parser->depth--;

        *result = lune_value_obj(
            (LuneObj *)map
        );

        return true;
    }

    for (;;) {
        ByteBuffer key = {0};

        if (!json_parse_string_bytes(
            parser, &key
        )) {
            byte_buffer_free(&key);
            native_roots_restore(
                parser->vm, roots
            );
            parser->depth--;
            return false;
        }

        json_skip_space(parser);

        if (
            parser->position >=
                parser->length ||
            parser->chars[
                parser->position
            ] != ':'
        ) {
            byte_buffer_free(&key);
            native_roots_restore(
                parser->vm, roots
            );
            parser->depth--;
            return json_error(
                parser,
                "expected ':'"
            );
        }

        parser->position++;
        json_skip_space(parser);

        LuneValue value =
            lune_value_null();

        if (!json_parse_value(
            parser, &value
        )) {
            byte_buffer_free(&key);
            native_roots_restore(
                parser->vm, roots
            );
            parser->depth--;
            return false;
        }

        if (!native_root_push(
            parser->vm, value
        )) {
            byte_buffer_free(&key);
            native_roots_restore(
                parser->vm, roots
            );
            parser->depth--;
            return false;
        }

        bool set = lune_map_set_chars(
            &parser->vm->heap,
            map,
            key.data == NULL
                ? ""
                : key.data,
            key.count,
            value
        );

        byte_buffer_free(&key);

        native_roots_restore(
            parser->vm,
            roots + 1
        );

        if (!set) {
            native_roots_restore(
                parser->vm, roots
            );
            parser->depth--;
            return native_error(
                parser->vm,
                "out of memory"
            );
        }

        json_skip_space(parser);

        if (
            parser->position >=
            parser->length
        ) {
            native_roots_restore(
                parser->vm, roots
            );
            parser->depth--;
            return json_error(
                parser,
                "unterminated object"
            );
        }

        char c =
            parser->chars[
                parser->position++
            ];

        if (c == '}') {
            break;
        }

        if (c != ',') {
            native_roots_restore(
                parser->vm, roots
            );
            parser->depth--;
            return json_error(
                parser,
                "expected ',' or '}'"
            );
        }

        json_skip_space(parser);
    }

    native_roots_restore(
        parser->vm, roots
    );

    parser->depth--;

    *result = lune_value_obj(
        (LuneObj *)map
    );

    return true;
}

static bool json_parse_number(
    JsonParser *parser,
    LuneValue *result
) {
    size_t start =
        parser->position;

    if (
        parser->chars[
            parser->position
        ] == '-'
    ) {
        parser->position++;

        if (
            parser->position >=
            parser->length
        ) {
            return json_error(
                parser,
                "invalid number"
            );
        }
    }

    if (
        parser->chars[
            parser->position
        ] == '0'
    ) {
        parser->position++;
    } else if (
        parser->chars[
            parser->position
        ] >= '1' &&
        parser->chars[
            parser->position
        ] <= '9'
    ) {
        while (
            parser->position <
                parser->length &&
            parser->chars[
                parser->position
            ] >= '0' &&
            parser->chars[
                parser->position
            ] <= '9'
        ) {
            parser->position++;
        }
    } else {
        return json_error(
            parser,
            "invalid number"
        );
    }

    bool floating = false;

    if (
        parser->position <
            parser->length &&
        parser->chars[
            parser->position
        ] == '.'
    ) {
        floating = true;
        parser->position++;

        size_t digits =
            parser->position;

        while (
            parser->position <
                parser->length &&
            parser->chars[
                parser->position
            ] >= '0' &&
            parser->chars[
                parser->position
            ] <= '9'
        ) {
            parser->position++;
        }

        if (
            parser->position ==
            digits
        ) {
            return json_error(
                parser,
                "invalid fraction"
            );
        }
    }

    if (
        parser->position <
            parser->length &&
        (
            parser->chars[
                parser->position
            ] == 'e' ||
            parser->chars[
                parser->position
            ] == 'E'
        )
    ) {
        floating = true;
        parser->position++;

        if (
            parser->position <
                parser->length &&
            (
                parser->chars[
                    parser->position
                ] == '+' ||
                parser->chars[
                    parser->position
                ] == '-'
            )
        ) {
            parser->position++;
        }

        size_t digits =
            parser->position;

        while (
            parser->position <
                parser->length &&
            parser->chars[
                parser->position
            ] >= '0' &&
            parser->chars[
                parser->position
            ] <= '9'
        ) {
            parser->position++;
        }

        if (
            parser->position ==
            digits
        ) {
            return json_error(
                parser,
                "invalid exponent"
            );
        }
    }

    size_t length =
        parser->position - start;

    char *text =
        malloc(length + 1);

    if (text == NULL) {
        return native_error(
            parser->vm,
            "out of memory"
        );
    }

    memcpy(
        text,
        parser->chars + start,
        length
    );

    text[length] = '\0';

    if (!floating) {
        errno = 0;
        char *end = NULL;

        long long integer =
            strtoll(
                text,
                &end,
                10
            );

        if (
            errno != ERANGE &&
            end != text &&
            *end == '\0'
        ) {
            free(text);

            *result = lune_value_int(
                (int64_t)integer
            );

            return true;
        }
    }

    errno = 0;
    char *end = NULL;
    double number =
        strtod(text, &end);

    bool valid =
        errno != ERANGE &&
        end != text &&
        *end == '\0' &&
        isfinite(number);

    free(text);

    if (!valid) {
        return json_error(
            parser,
            "number is out of range"
        );
    }

    *result =
        lune_value_float(number);

    return true;
}

static bool json_match_literal(
    JsonParser *parser,
    const char *literal
) {
    size_t length =
        strlen(literal);

    if (
        parser->position + length >
        parser->length
    ) {
        return false;
    }

    if (
        memcmp(
            parser->chars +
                parser->position,
            literal,
            length
        ) != 0
    ) {
        return false;
    }

    parser->position += length;
    return true;
}

static bool json_parse_value(
    JsonParser *parser,
    LuneValue *result
) {
    json_skip_space(parser);

    if (
        parser->position >=
        parser->length
    ) {
        return json_error(
            parser,
            "expected value"
        );
    }

    char c =
        parser->chars[
            parser->position
        ];

    if (c == '"') {
        ByteBuffer string = {0};

        if (!json_parse_string_bytes(
            parser, &string
        )) {
            byte_buffer_free(&string);
            return false;
        }

        bool ok = make_string_value(
            parser->vm,
            string.data == NULL
                ? ""
                : string.data,
            string.count,
            result
        );

        byte_buffer_free(&string);
        return ok;
    }

    if (c == '[') {
        return json_parse_array(
            parser, result
        );
    }

    if (c == '{') {
        return json_parse_object(
            parser, result
        );
    }

    if (
        c == '-' ||
        (c >= '0' && c <= '9')
    ) {
        return json_parse_number(
            parser, result
        );
    }

    if (json_match_literal(
        parser, "true"
    )) {
        *result =
            lune_value_bool(true);
        return true;
    }

    if (json_match_literal(
        parser, "false"
    )) {
        *result =
            lune_value_bool(false);
        return true;
    }

    if (json_match_literal(
        parser, "null"
    )) {
        *result =
            lune_value_null();
        return true;
    }

    return json_error(
        parser,
        "invalid value"
    );
}

static bool native_json_parse(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjString *json =
        as_string(args[0]);

    if (json == NULL) {
        return native_error(
            vm,
            "json_parse() expects a string"
        );
    }

    JsonParser parser = {
        .vm = vm,
        .chars = json->chars,
        .length = json->length,
    };

    size_t roots =
        vm->native_root_count;

    if (!json_parse_value(
        &parser, result
    )) {
        native_roots_restore(
            vm, roots
        );
        return false;
    }

    if (!native_root_push(
        vm, *result
    )) {
        native_roots_restore(
            vm, roots
        );
        return false;
    }

    json_skip_space(&parser);

    bool complete =
        parser.position ==
        parser.length;

    native_roots_restore(
        vm, roots
    );

    if (!complete) {
        return json_error(
            &parser,
            "trailing data"
        );
    }

    return true;
}

typedef struct {
    LuneVM *vm;
    ByteBuffer output;
    LuneObj **path;
    size_t path_count;
    size_t path_capacity;
    unsigned depth;
} JsonWriter;

static bool json_writer_error(
    JsonWriter *writer,
    const char *message
) {
    return native_error(
        writer->vm,
        message
    );
}

static bool json_writer_append(
    JsonWriter *writer,
    const char *data,
    size_t length
) {
    if (!byte_buffer_append(
        &writer->output,
        data,
        length
    )) {
        return json_writer_error(
            writer,
            "out of memory"
        );
    }

    return true;
}

static bool json_writer_byte(
    JsonWriter *writer,
    char byte
) {
    return json_writer_append(
        writer, &byte, 1
    );
}

static bool json_write_string(
    JsonWriter *writer,
    const char *chars,
    size_t length
) {
    if (!json_writer_byte(
        writer, '"'
    )) {
        return false;
    }

    static const char hex[] =
        "0123456789abcdef";

    for (
        size_t i = 0;
        i < length;
        i++
    ) {
        unsigned char c =
            (unsigned char)chars[i];

        switch (c) {
            case '"':
                if (!json_writer_append(
                    writer, "\\\"", 2
                )) {
                    return false;
                }
                break;

            case '\\':
                if (!json_writer_append(
                    writer, "\\\\", 2
                )) {
                    return false;
                }
                break;

            case '\b':
                if (!json_writer_append(
                    writer, "\\b", 2
                )) {
                    return false;
                }
                break;

            case '\f':
                if (!json_writer_append(
                    writer, "\\f", 2
                )) {
                    return false;
                }
                break;

            case '\n':
                if (!json_writer_append(
                    writer, "\\n", 2
                )) {
                    return false;
                }
                break;

            case '\r':
                if (!json_writer_append(
                    writer, "\\r", 2
                )) {
                    return false;
                }
                break;

            case '\t':
                if (!json_writer_append(
                    writer, "\\t", 2
                )) {
                    return false;
                }
                break;

            default:
                if (c < 0x20u) {
                    char escaped[6] = {
                        '\\', 'u', '0', '0',
                        hex[c >> 4],
                        hex[c & 0x0fu],
                    };

                    if (!json_writer_append(
                        writer,
                        escaped,
                        sizeof(escaped)
                    )) {
                        return false;
                    }
                } else if (!json_writer_byte(
                    writer, (char)c
                )) {
                    return false;
                }
                break;
        }
    }

    return json_writer_byte(
        writer, '"'
    );
}

static bool json_writer_path_push(
    JsonWriter *writer,
    LuneObj *object
) {
    for (
        size_t i = 0;
        i < writer->path_count;
        i++
    ) {
        if (
            writer->path[i] ==
            object
        ) {
            return json_writer_error(
                writer,
                "json_stringify(): cyclic value"
            );
        }
    }

    if (
        writer->path_count ==
        writer->path_capacity
    ) {
        size_t next =
            writer->path_capacity == 0
            ? 16
            : writer->path_capacity * 2;

        if (
            next <
                writer->path_capacity ||
            next >
                SIZE_MAX /
                    sizeof(*writer->path)
        ) {
            return json_writer_error(
                writer,
                "json_stringify(): nesting is too deep"
            );
        }

        LuneObj **grown =
            realloc(
                writer->path,
                next *
                    sizeof(*writer->path)
            );

        if (grown == NULL) {
            return json_writer_error(
                writer,
                "out of memory"
            );
        }

        writer->path = grown;
        writer->path_capacity = next;
    }

    writer->path[
        writer->path_count++
    ] = object;

    return true;
}

static bool json_write_value(
    JsonWriter *writer,
    LuneValue value
) {
    switch (value.kind) {
        case LUNE_VALUE_NULL:
            return json_writer_append(
                writer, "null", 4
            );

        case LUNE_VALUE_BOOL:
            return value.as.boolean
                ? json_writer_append(
                    writer, "true", 4
                )
                : json_writer_append(
                    writer, "false", 5
                );

        case LUNE_VALUE_INT: {
            char number[64];
            int written = snprintf(
                number,
                sizeof(number),
                "%lld",
                (long long)
                    value.as.integer
            );

            if (
                written < 0 ||
                (size_t)written >=
                    sizeof(number)
            ) {
                return json_writer_error(
                    writer,
                    "json_stringify(): integer conversion failed"
                );
            }

            return json_writer_append(
                writer,
                number,
                (size_t)written
            );
        }

        case LUNE_VALUE_FLOAT: {
            if (!isfinite(
                value.as.floating
            )) {
                return json_writer_error(
                    writer,
                    "json_stringify(): non-finite float"
                );
            }

            char number[64];
            int written = snprintf(
                number,
                sizeof(number),
                "%.17g",
                value.as.floating
            );

            if (
                written < 0 ||
                (size_t)written >=
                    sizeof(number)
            ) {
                return json_writer_error(
                    writer,
                    "json_stringify(): float conversion failed"
                );
            }

            return json_writer_append(
                writer,
                number,
                (size_t)written
            );
        }

        case LUNE_VALUE_OBJ:
            break;
    }

    if (lune_obj_is_string(
        value.as.object
    )) {
        LuneObjString *string =
            (LuneObjString *)
                value.as.object;

        return json_write_string(
            writer,
            string->chars,
            string->length
        );
    }

    if (
        writer->depth >= 128
    ) {
        return json_writer_error(
            writer,
            "json_stringify(): nesting is too deep"
        );
    }

    if (lune_obj_is_list(
        value.as.object
    )) {
        if (!json_writer_path_push(
            writer,
            value.as.object
        )) {
            return false;
        }

        writer->depth++;

        LuneObjList *list =
            (LuneObjList *)
                value.as.object;

        if (!json_writer_byte(
            writer, '['
        )) {
            writer->depth--;
            writer->path_count--;
            return false;
        }

        for (
            size_t i = 0;
            i < list->count;
            i++
        ) {
            if (
                i != 0 &&
                !json_writer_byte(
                    writer, ','
                )
            ) {
                writer->depth--;
                writer->path_count--;
                return false;
            }

            if (!json_write_value(
                writer,
                list->items[i]
            )) {
                writer->depth--;
                writer->path_count--;
                return false;
            }
        }

        bool ok = json_writer_byte(
            writer, ']'
        );

        writer->depth--;
        writer->path_count--;
        return ok;
    }

    if (lune_obj_is_map(
        value.as.object
    )) {
        if (!json_writer_path_push(
            writer,
            value.as.object
        )) {
            return false;
        }

        writer->depth++;

        LuneObjMap *map =
            (LuneObjMap *)
                value.as.object;

        if (!json_writer_byte(
            writer, '{'
        )) {
            writer->depth--;
            writer->path_count--;
            return false;
        }

        for (
            size_t i = 0;
            i < map->count;
            i++
        ) {
            if (
                i != 0 &&
                !json_writer_byte(
                    writer, ','
                )
            ) {
                writer->depth--;
                writer->path_count--;
                return false;
            }

            if (
                !json_write_string(
                    writer,
                    map->entries[i]
                        .key->chars,
                    map->entries[i]
                        .key->length
                ) ||
                !json_writer_byte(
                    writer, ':'
                ) ||
                !json_write_value(
                    writer,
                    map->entries[i]
                        .value
                )
            ) {
                writer->depth--;
                writer->path_count--;
                return false;
            }
        }

        bool ok = json_writer_byte(
            writer, '}'
        );

        writer->depth--;
        writer->path_count--;
        return ok;
    }

    return json_writer_error(
        writer,
        "json_stringify(): unsupported value type"
    );
}

static bool native_json_stringify(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    JsonWriter writer = {
        .vm = vm,
    };

    bool ok = json_write_value(
        &writer, args[0]
    );

    if (ok) {
        ok = make_string_value(
            vm,
            writer.output.data == NULL
                ? ""
                : writer.output.data,
            writer.output.count,
            result
        );
    }

    byte_buffer_free(
        &writer.output
    );

    free(writer.path);
    return ok;
}

static bool module_compile_wrapper(
    LuneVM *vm,
    ModuleEntry *entry,
    const char *source,
    size_t source_length
) {
    static const char prefix[] =
        "fn() => {\n";
    static const char suffix[] =
        "\n}\n";

    if (
        source_length >
        SIZE_MAX -
            (sizeof(prefix) - 1) -
            (sizeof(suffix) - 1) -
            1
    ) {
        return native_error(
            vm,
            "module source is too large"
        );
    }

    size_t wrapped_length =
        (sizeof(prefix) - 1) +
        source_length +
        (sizeof(suffix) - 1);

    char *wrapped =
        malloc(
            wrapped_length + 1
        );

    if (wrapped == NULL) {
        return native_error(
            vm, "out of memory"
        );
    }

    size_t offset = 0;

    memcpy(
        wrapped + offset,
        prefix,
        sizeof(prefix) - 1
    );

    offset += sizeof(prefix) - 1;

    memcpy(
        wrapped + offset,
        source,
        source_length
    );

    offset += source_length;

    memcpy(
        wrapped + offset,
        suffix,
        sizeof(suffix) - 1
    );

    offset += sizeof(suffix) - 1;
    wrapped[offset] = '\0';

    LuneParser parser;
    lune_parser_init(
        &parser,
        wrapped,
        wrapped_length,
        vm->diagnostic,
        vm->diagnostic_context
    );

    LuneAst *ast =
        lune_parse_program(
            &parser
        );

    if (
        ast == NULL ||
        parser.had_error
    ) {
        lune_ast_free(ast);
        free(wrapped);
        return false;
    }

    LuneChunk *chunk =
        malloc(sizeof(*chunk));

    if (chunk == NULL) {
        lune_ast_free(ast);
        free(wrapped);
        return native_error(
            vm, "out of memory"
        );
    }

    lune_chunk_init(chunk);

    bool compiled = lune_compile(
        ast,
        wrapped,
        chunk,
        vm->diagnostic,
        vm->diagnostic_context
    );

    lune_ast_free(ast);
    free(wrapped);

    if (!compiled) {
        lune_chunk_free(chunk);
        free(chunk);
        return false;
    }

    entry->chunk = chunk;
    return true;
}

static bool module_execute_entry(
    LuneVM *vm,
    ModuleEntry *entry,
    LuneValue *result
) {
    size_t base_depth =
        vm->frame_count;
    size_t base_stack =
        vm->stack_count;

    if (
        vm->frame_count >=
        FRAME_MAX
    ) {
        return native_error(
            vm,
            "maximum module depth exceeded"
        );
    }

    CallFrame *frame =
        &vm->frames[
            vm->frame_count++
        ];

    *frame = (CallFrame){
        .chunk = entry->chunk,
        .stack_base = base_stack,
        .closure = NULL,
        .module_path = entry->key,
    };

    LuneValue factory =
        lune_value_null();

    if (!run_until(
        vm,
        &factory,
        base_depth
    )) {
        return false;
    }

    if (vm->exit_requested) {
        *result = lune_value_null();
        return true;
    }

    if (!invoke_callable(
        vm,
        factory,
        0,
        NULL,
        result
    )) {
        return false;
    }

    return true;
}

static bool module_native_json(
    LuneVM *vm,
    ModuleEntry *entry,
    LuneValue *result
) {
    size_t roots =
        vm->native_root_count;

    LuneObjMap *module =
        lune_map_new(
            &vm->heap
        );

    if (module == NULL) {
        return native_error(
            vm, "out of memory"
        );
    }

    if (!native_root_push(
        vm,
        lune_value_obj(
            (LuneObj *)module
        )
    )) {
        return false;
    }

    LuneValue parse;
    LuneValue stringify;

    if (
        !lune_map_get_chars(
            vm->globals,
            "json_parse",
            strlen("json_parse"),
            &parse
        ) ||
        !lune_map_get_chars(
            vm->globals,
            "json_stringify",
            strlen("json_stringify"),
            &stringify
        ) ||
        !lune_map_set_chars(
            &vm->heap,
            module,
            "parse",
            strlen("parse"),
            parse
        ) ||
        !lune_map_set_chars(
            &vm->heap,
            module,
            "stringify",
            strlen("stringify"),
            stringify
        )
    ) {
        native_roots_restore(
            vm, roots
        );

        return native_error(
            vm,
            "unable to initialize json module"
        );
    }

    *result = lune_value_obj(
        (LuneObj *)module
    );

    entry->loading = false;
    entry->value = *result;

    native_roots_restore(
        vm, roots
    );

    return true;
}

static bool native_import(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjString *specifier =
        as_string(args[0]);

    if (specifier == NULL) {
        return native_error(
            vm,
            "import() expects a string"
        );
    }

    bool file_module =
        specifier->length > 0 &&
        (
            specifier->chars[0] == '.' ||
            specifier->chars[0] == '/' ||
            memchr(
                specifier->chars,
                '/',
                specifier->length
            ) != NULL
        );

    if (!file_module) {
        if (
            specifier->length !=
                strlen("json") ||
            memcmp(
                specifier->chars,
                "json",
                strlen("json")
            ) != 0
        ) {
            return native_error(
                vm,
                "unknown native module"
            );
        }

        ModuleEntry *cached =
            module_find(vm, "json");

        if (cached != NULL) {
            if (cached->loading) {
                return native_error(
                    vm,
                    "cyclic native module import"
                );
            }

            *result = cached->value;
            return true;
        }

        char *key =
            copy_chars(
                "json",
                strlen("json")
            );

        if (key == NULL) {
            return native_error(
                vm, "out of memory"
            );
        }

        ModuleEntry *entry =
            module_add(vm, key);

        if (entry == NULL) {
            return native_error(
                vm, "out of memory"
            );
        }

        return module_native_json(
            vm,
            entry,
            result
        );
    }

    char *key = NULL;

    if (!module_file_key(
        vm,
        specifier,
        &key
    )) {
        return false;
    }

    ModuleEntry *cached =
        module_find(vm, key);

    if (cached != NULL) {
        free(key);

        if (cached->loading) {
            return native_error(
                vm,
                "cyclic module import"
            );
        }

        *result = cached->value;
        return true;
    }

    ModuleEntry *entry =
        module_add(vm, key);

    if (entry == NULL) {
        return native_error(
            vm, "out of memory"
        );
    }

    char *source = NULL;
    size_t source_length = 0;
    char error[256];

    if (!lune_platform_read_file(
        entry->key,
        &source,
        &source_length,
        error,
        sizeof(error)
    )) {
        return native_error(
            vm, error
        );
    }

    bool compiled =
        module_compile_wrapper(
            vm,
            entry,
            source,
            source_length
        );

    free(source);

    if (!compiled) {
        return false;
    }

    LuneValue value =
        lune_value_null();

    if (!module_execute_entry(
        vm,
        entry,
        &value
    )) {
        return false;
    }

    if (vm->exit_requested) {
        *result = lune_value_null();
        return true;
    }

    entry->value = value;
    entry->loading = false;
    *result = value;
    return true;
}

static bool native_env(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjString *name =
        as_string(args[0]);

    if (name == NULL) {
        return native_error(
            vm,
            "env() expects a string name"
        );
    }

    if (
        strlen(name->chars) !=
        name->length
    ) {
        return native_error(
            vm,
            "environment variable name contains NUL"
        );
    }

    const char *value =
        getenv(name->chars);

    if (value == NULL) {
        *result = lune_value_null();
        return true;
    }

    return make_string_value(
        vm,
        value,
        strlen(value),
        result
    );
}

static bool native_exit(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    if (
        args[0].kind !=
        LUNE_VALUE_INT
    ) {
        return native_error(
            vm,
            "exit() expects an integer status"
        );
    }

    if (
        args[0].as.integer < 0 ||
        args[0].as.integer > 255
    ) {
        return native_error(
            vm,
            "exit() status must be between 0 and 255"
        );
    }

    vm->exit_requested = true;
    vm->exit_status =
        (int)args[0].as.integer;

    *result = lune_value_null();
    return true;
}

static LuneObjString *path_string(
    LuneVM *vm,
    LuneValue value,
    const char *function_name
) {
    LuneObjString *string =
        as_string(value);

    if (string == NULL) {
        char message[96];

        (void)snprintf(
            message,
            sizeof(message),
            "%s expects string paths",
            function_name
        );

        (void)native_error(
            vm, message
        );
        return NULL;
    }

    if (
        strlen(string->chars) !=
        string->length
    ) {
        (void)native_error(
            vm,
            "path contains NUL"
        );
        return NULL;
    }

    return string;
}

static bool native_read_file(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjString *path =
        path_string(
            vm,
            args[0],
            "read_file()"
        );

    if (path == NULL) {
        return false;
    }

    char *data = NULL;
    size_t length = 0;
    char error[256];

    if (!lune_platform_read_file(
        path->chars,
        &data,
        &length,
        error,
        sizeof(error)
    )) {
        return native_error(
            vm, error
        );
    }

    bool ok = make_string_value(
        vm,
        data,
        length,
        result
    );

    free(data);
    return ok;
}

static bool native_write_file(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjString *path =
        path_string(
            vm,
            args[0],
            "write_file()"
        );

    if (path == NULL) {
        return false;
    }

    LuneObjString *data =
        as_string(args[1]);

    if (data == NULL) {
        return native_error(
            vm,
            "write_file() expects string data"
        );
    }

    char error[256];

    if (!lune_platform_write_file(
        path->chars,
        data->chars,
        data->length,
        error,
        sizeof(error)
    )) {
        return native_error(
            vm, error
        );
    }

    if (
        data->length >
        (size_t)INT64_MAX
    ) {
        return native_error(
            vm,
            "written byte count is too large"
        );
    }

    *result = lune_value_int(
        (int64_t)data->length
    );
    return true;
}

static bool native_path_join(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjString *left =
        path_string(
            vm,
            args[0],
            "path_join()"
        );

    if (left == NULL) {
        return false;
    }

    LuneObjString *right =
        path_string(
            vm,
            args[1],
            "path_join()"
        );

    if (right == NULL) {
        return false;
    }

    if (
        right->length > 0 &&
        right->chars[0] == '/'
    ) {
        return make_string_value(
            vm,
            right->chars,
            right->length,
            result
        );
    }

    if (left->length == 0) {
        return make_string_value(
            vm,
            right->chars,
            right->length,
            result
        );
    }

    if (right->length == 0) {
        return make_string_value(
            vm,
            left->chars,
            left->length,
            result
        );
    }

    bool left_slash =
        left->chars[
            left->length - 1
        ] == '/';

    bool right_slash =
        right->chars[0] == '/';

    size_t separator =
        left_slash ||
        right_slash
        ? 0
        : 1;

    size_t right_offset =
        left_slash &&
        right_slash
        ? 1
        : 0;

    if (
        left->length >
        SIZE_MAX -
            separator -
            (right->length -
                right_offset)
    ) {
        return native_error(
            vm,
            "joined path is too large"
        );
    }

    size_t length =
        left->length +
        separator +
        right->length -
        right_offset;

    char *joined =
        malloc(length);

    if (
        joined == NULL &&
        length != 0
    ) {
        return native_error(
            vm, "out of memory"
        );
    }

    size_t offset = 0;

    memcpy(
        joined + offset,
        left->chars,
        left->length
    );
    offset += left->length;

    if (separator != 0) {
        joined[offset++] = '/';
    }

    memcpy(
        joined + offset,
        right->chars +
            right_offset,
        right->length -
            right_offset
    );

    bool ok = make_string_value(
        vm,
        joined,
        length,
        result
    );

    free(joined);
    return ok;
}

static size_t path_trim_end(
    const LuneObjString *path
) {
    size_t end = path->length;

    while (
        end > 1 &&
        path->chars[end - 1] == '/'
    ) {
        end--;
    }

    return end;
}

static bool native_path_base(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjString *path =
        path_string(
            vm,
            args[0],
            "path_base()"
        );

    if (path == NULL) {
        return false;
    }

    size_t end =
        path_trim_end(path);

    size_t start = end;

    while (
        start > 0 &&
        path->chars[start - 1] != '/'
    ) {
        start--;
    }

    return make_string_value(
        vm,
        path->chars + start,
        end - start,
        result
    );
}

static bool native_path_dir(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjString *path =
        path_string(
            vm,
            args[0],
            "path_dir()"
        );

    if (path == NULL) {
        return false;
    }

    size_t end =
        path_trim_end(path);

    size_t slash = end;

    while (
        slash > 0 &&
        path->chars[slash - 1] != '/'
    ) {
        slash--;
    }

    if (slash == 0) {
        return make_string_value(
            vm, ".", 1, result
        );
    }

    while (
        slash > 1 &&
        path->chars[slash - 1] == '/'
    ) {
        slash--;
    }

    return make_string_value(
        vm,
        path->chars,
        slash,
        result
    );
}

static bool map_set_native(
    LuneVM *vm,
    LuneObjMap *map,
    const char *name,
    LuneValue value
) {
    if (!push(
        vm,
        value,
        vm->native_span
    )) {
        return false;
    }

    bool ok =
        lune_map_set_chars(
            &vm->heap,
            map,
            name,
            strlen(name),
            value
        );

    LuneValue ignored;

    if (!pop(
        vm,
        &ignored,
        vm->native_span
    )) {
        return false;
    }

    if (!ok) {
        return native_error(
            vm, "out of memory"
        );
    }

    return true;
}

static bool native_exec(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    (void)argc;

    LuneObjString *program =
        path_string(
            vm,
            args[0],
            "exec()"
        );

    if (program == NULL) {
        return false;
    }

    LuneObjList *arguments =
        as_list(args[1]);

    if (arguments == NULL) {
        return native_error(
            vm,
            "exec() expects a list of string arguments"
        );
    }

    if (
        arguments->count >
        SIZE_MAX /
            sizeof(char *) -
            2
    ) {
        return native_error(
            vm,
            "exec() argument list is too large"
        );
    }

    char **argv = calloc(
        arguments->count + 2,
        sizeof(*argv)
    );

    if (argv == NULL) {
        return native_error(
            vm, "out of memory"
        );
    }

    argv[0] = program->chars;

    for (
        size_t i = 0;
        i < arguments->count;
        i++
    ) {
        LuneObjString *argument =
            as_string(
                arguments->items[i]
            );

        if (
            argument == NULL ||
            strlen(argument->chars) !=
                argument->length
        ) {
            free(argv);

            return native_error(
                vm,
                "exec() arguments must be strings without NUL"
            );
        }

        argv[i + 1] =
            argument->chars;
    }

    LuneProcessResult process;
    char error[256];

    bool ran = lune_platform_exec(
        program->chars,
        argv,
        &process,
        error,
        sizeof(error)
    );

    free(argv);

    if (!ran) {
        return native_error(
            vm, error
        );
    }

    LuneObjMap *map =
        lune_map_new(&vm->heap);

    if (map == NULL) {
        lune_platform_process_result_free(
            &process
        );
        return native_error(
            vm, "out of memory"
        );
    }

    LuneValue map_value =
        lune_value_obj(
            (LuneObj *)map
        );

    if (!push(
        vm,
        map_value,
        vm->native_span
    )) {
        lune_platform_process_result_free(
            &process
        );
        return false;
    }

    bool ok = map_set_native(
        vm,
        map,
        "status",
        lune_value_int(
            (int64_t)process.status
        )
    );

    LuneValue stdout_value =
        lune_value_null();

    if (
        ok &&
        !make_string_value(
            vm,
            process.stdout_data,
            process.stdout_length,
            &stdout_value
        )
    ) {
        ok = false;
    }

    if (
        ok &&
        !map_set_native(
            vm,
            map,
            "stdout",
            stdout_value
        )
    ) {
        ok = false;
    }

    LuneValue stderr_value =
        lune_value_null();

    if (
        ok &&
        !make_string_value(
            vm,
            process.stderr_data,
            process.stderr_length,
            &stderr_value
        )
    ) {
        ok = false;
    }

    if (
        ok &&
        !map_set_native(
            vm,
            map,
            "stderr",
            stderr_value
        )
    ) {
        ok = false;
    }

    lune_platform_process_result_free(
        &process
    );

    LuneValue rooted_map;

    if (!pop(
        vm,
        &rooted_map,
        vm->native_span
    )) {
        return false;
    }

    if (!ok) {
        return false;
    }

    *result = rooted_map;
    return true;
}

static bool define_global_value(
    LuneVM *vm,
    const char *name,
    LuneValue value
) {
    if (!push(
        vm,
        value,
        (LuneSpan){0}
    )) {
        return false;
    }

    bool ok =
        lune_map_set_chars(
            &vm->heap,
            vm->globals,
            name,
            strlen(name),
            vm->stack[
                vm->stack_count - 1
            ]
        );

    LuneValue ignored;
    if (!pop(
        vm,
        &ignored,
        (LuneSpan){0}
    )) {
        return false;
    }

    return ok;
}

static bool define_native(
    LuneVM *vm,
    const char *name,
    int arity,
    LuneNativeFn function
) {
    LuneObjNative *native =
        lune_native_new(
            &vm->heap,
            name,
            arity,
            function
        );

    if (native == NULL) {
        return false;
    }

    return define_global_value(
        vm,
        name,
        lune_value_obj(
            (LuneObj *)native
        )
    );
}

static bool define_process_args(
    LuneVM *vm
) {
    size_t count =
        vm->process_argc < 0
        ? 0
        : (size_t)vm->process_argc;

    LuneValue *items = NULL;

    if (count > 0) {
        if (
            count >
            SIZE_MAX /
                sizeof(*items)
        ) {
            return false;
        }

        items = calloc(
            count,
            sizeof(*items)
        );

        if (items == NULL) {
            return false;
        }
    }

    LuneObjList *list =
        lune_list_new(
            &vm->heap,
            items,
            count
        );

    free(items);

    if (list == NULL) {
        return false;
    }

    if (!define_global_value(
        vm,
        "args",
        lune_value_obj(
            (LuneObj *)list
        )
    )) {
        return false;
    }

    for (
        size_t i = 0;
        i < count;
        i++
    ) {
        const char *argument =
            vm->process_argv[i];

        LuneObjString *string =
            lune_string_new(
                &vm->heap,
                argument,
                strlen(argument)
            );

        if (string == NULL) {
            return false;
        }

        list->items[i] =
            lune_value_obj(
                (LuneObj *)string
            );
    }

    return true;
}

static bool call_closure(
    LuneVM *vm,
    LuneObjClosure *closure,
    uint16_t argc,
    size_t callee_index,
    LuneSpan span
) {
    if (
        argc !=
        closure->function->arity
    ) {
        return runtime_error(
            vm,
            span,
            "wrong number of arguments"
        );
    }

    if (
        vm->frame_count >=
        FRAME_MAX
    ) {
        return runtime_error(
            vm,
            span,
            "maximum call depth exceeded"
        );
    }

    CallFrame *frame =
        &vm->frames[
            vm->frame_count
        ];

    *frame = (CallFrame){
        .chunk =
            &closure->function
                ->chunk,
        .stack_base =
            callee_index,
        .closure =
            closure,
        .module_path =
            closure->module_path,
    };

    for (
        uint16_t i = 0;
        i < argc;
        i++
    ) {
        frame->locals[i] =
            vm->stack[
                callee_index +
                1 + i
            ];

        frame->local_defined[i] =
            true;
    }

    vm->stack_count =
        callee_index;

    vm->frame_count++;
    return true;
}

static bool call_native(
    LuneVM *vm,
    LuneObjNative *native,
    uint16_t argc,
    size_t callee_index,
    LuneSpan span
) {
    if (
        native->arity >= 0 &&
        argc !=
            (uint16_t)
                native->arity
    ) {
        return runtime_error(
            vm,
            span,
            "wrong number of arguments"
        );
    }

    LuneValue result =
        lune_value_null();

    vm->native_span = span;

    if (!native->function(
        vm,
        (int)argc,
        vm->stack +
            callee_index + 1,
        &result
    )) {
        return false;
    }

    vm->stack_count =
        callee_index;

    return push(
        vm, result, span
    );
}

static bool call_value(
    LuneVM *vm,
    uint16_t argc,
    LuneSpan span
) {
    if (
        vm->stack_count <
        (size_t)argc + 1
    ) {
        return runtime_error(
            vm,
            span,
            "internal stack underflow during call"
        );
    }

    size_t callee_index =
        vm->stack_count -
        (size_t)argc - 1;

    LuneValue callee =
        vm->stack[
            callee_index
        ];

    LuneObjClosure *closure =
        as_closure(callee);

    if (closure != NULL) {
        return call_closure(
            vm,
            closure,
            argc,
            callee_index,
            span
        );
    }

    LuneObjNative *native =
        as_native(callee);

    if (native != NULL) {
        return call_native(
            vm,
            native,
            argc,
            callee_index,
            span
        );
    }

    return runtime_error(
        vm,
        span,
        "attempted to call a non-function value"
    );
}

static bool invoke_callable(
    LuneVM *vm,
    LuneValue callee,
    int argc,
    const LuneValue *args,
    LuneValue *result
) {
    if (
        argc < 0 ||
        argc > (int)UINT16_MAX
    ) {
        return native_error(
            vm,
            "callback has too many arguments"
        );
    }

    size_t base_stack =
        vm->stack_count;
    size_t base_depth =
        vm->frame_count;
    LuneSpan saved_span =
        vm->native_span;

    if (!push(
        vm,
        callee,
        saved_span
    )) {
        return false;
    }

    for (
        int i = 0;
        i < argc;
        i++
    ) {
        if (!push(
            vm,
            args[i],
            saved_span
        )) {
            vm->stack_count =
                base_stack;
            return false;
        }
    }

    if (!call_value(
        vm,
        (uint16_t)argc,
        saved_span
    )) {
        vm->stack_count =
            base_stack;
        vm->native_span =
            saved_span;
        return false;
    }

    if (vm->exit_requested) {
        vm->stack_count =
            base_stack;
        vm->native_span =
            saved_span;
        *result = lune_value_null();
        return true;
    }

    if (
        vm->frame_count >
        base_depth
    ) {
        LuneValue call_result =
            lune_value_null();

        if (!run_until(
            vm,
            &call_result,
            base_depth
        )) {
            vm->stack_count =
                base_stack;
            vm->native_span =
                saved_span;
            return false;
        }

        vm->native_span =
            saved_span;

        if (vm->exit_requested) {
            vm->stack_count =
                base_stack;
            *result =
                lune_value_null();
            return true;
        }

        vm->stack_count =
            base_stack;
        *result = call_result;
        return true;
    }

    LuneValue call_result;

    if (!pop(
        vm,
        &call_result,
        saved_span
    )) {
        vm->native_span =
            saved_span;
        return false;
    }

    vm->stack_count =
        base_stack;
    vm->native_span =
        saved_span;
    *result = call_result;
    return true;
}

LuneVM *lune_vm_new(
    LuneDiagnosticFn diagnostic,
    void *diagnostic_context
) {
    LuneVM *vm =
        calloc(1, sizeof(*vm));

    if (vm == NULL) {
        return NULL;
    }

    lune_heap_init(
        &vm->heap,
        mark_vm_roots,
        vm
    );

    vm->diagnostic =
        diagnostic;

    vm->diagnostic_context =
        diagnostic_context;

    return vm;
}

void lune_vm_free(
    LuneVM *vm
) {
    if (vm == NULL) return;

    lune_heap_free(
        &vm->heap
    );

    module_entries_clear(vm);
    free(vm->native_roots);
    free(vm);
}

void lune_vm_set_process_args(
    LuneVM *vm,
    int argc,
    const char *const *argv
) {
    vm->process_argc = argc;
    vm->process_argv = argv;
}

void lune_vm_set_script_path(
    LuneVM *vm,
    const char *path
) {
    vm->script_path = path;
}

bool lune_vm_exit_status(
    const LuneVM *vm,
    int *status
) {
    if (!vm->exit_requested) {
        return false;
    }

    if (status != NULL) {
        *status = vm->exit_status;
    }

    return true;
}

void lune_vm_set_gc_stress(
    LuneVM *vm,
    bool enabled
) {
    lune_heap_set_stress(
        &vm->heap, enabled
    );
}

void lune_vm_collect_garbage(
    LuneVM *vm
) {
    lune_heap_collect(
        &vm->heap
    );
}

size_t lune_vm_heap_bytes(
    const LuneVM *vm
) {
    return lune_heap_bytes(
        &vm->heap
    );
}

static bool prepare_run(
    LuneVM *vm,
    const LuneChunk *chunk
) {
    bool stress =
        vm->heap.stress_gc;

    /*
     * Clear old VM roots before freeing the
     * previous run's heap.
     */
    vm->stack_count = 0;
    vm->native_root_count = 0;
    vm->frame_count = 0;
    vm->globals = NULL;
    vm->open_upvalues = NULL;
    vm->has_result = false;
    vm->last_result =
        lune_value_null();
    vm->exit_requested = false;
    vm->exit_status = 0;
    vm->native_span =
        (LuneSpan){0};

    lune_heap_free(
        &vm->heap
    );

    module_entries_clear(vm);

    lune_heap_init(
        &vm->heap,
        mark_vm_roots,
        vm
    );

    lune_heap_set_stress(
        &vm->heap, stress
    );

    vm->globals =
        lune_map_new(
            &vm->heap
        );

    if (vm->globals == NULL) {
        return false;
    }

    if (!define_process_args(vm)) {
        return false;
    }

    if (
        !define_native(
            vm, "print", -1, native_print
        ) ||
        !define_native(
            vm, "type", 1, native_type
        ) ||
        !define_native(
            vm, "len", 1, native_len
        ) ||
        !define_native(
            vm, "push", 2, native_push
        ) ||
        !define_native(
            vm, "pop", 1, native_pop
        ) ||
        !define_native(
            vm, "byte_at", 2,
            native_byte_at
        ) ||
        !define_native(
            vm, "slice", 3,
            native_slice
        ) ||
        !define_native(
            vm, "bytes", 1,
            native_bytes
        ) ||
        !define_native(
            vm, "str", 1, native_str
        ) ||
        !define_native(
            vm, "int", 1, native_int
        ) ||
        !define_native(
            vm, "float", 1, native_float
        ) ||
        !define_native(
            vm, "bool", 1, native_bool
        ) ||
        !define_native(
            vm, "contains", 2,
            native_contains
        ) ||
        !define_native(
            vm, "find", 2,
            native_find
        ) ||
        !define_native(
            vm, "split", 2,
            native_split
        ) ||
        !define_native(
            vm, "join", 2,
            native_join
        ) ||
        !define_native(
            vm, "format", 2,
            native_format
        ) ||
        !define_native(
            vm, "each", 2,
            native_each
        ) ||
        !define_native(
            vm, "map", 2,
            native_map
        ) ||
        !define_native(
            vm, "filter", 2,
            native_filter
        ) ||
        !define_native(
            vm, "reduce", 3,
            native_reduce
        ) ||
        !define_native(
            vm, "json_parse", 1,
            native_json_parse
        ) ||
        !define_native(
            vm, "json_stringify", 1,
            native_json_stringify
        ) ||
        !define_native(
            vm, "import", 1,
            native_import
        ) ||
        !define_native(
            vm, "env", 1, native_env
        ) ||
        !define_native(
            vm, "exit", 1, native_exit
        ) ||
        !define_native(
            vm, "read_file", 1,
            native_read_file
        ) ||
        !define_native(
            vm, "write_file", 2,
            native_write_file
        ) ||
        !define_native(
            vm, "path_join", 2,
            native_path_join
        ) ||
        !define_native(
            vm, "path_base", 1,
            native_path_base
        ) ||
        !define_native(
            vm, "path_dir", 1,
            native_path_dir
        ) ||
        !define_native(
            vm, "exec", 2,
            native_exec
        )
    ) {
        return false;
    }

    CallFrame *root =
        &vm->frames[
            vm->frame_count++
        ];

    *root = (CallFrame){
        .chunk = chunk,
        .ip = 0,
        .stack_base = 0,
        .closure = NULL,
        .module_path =
            vm->script_path != NULL
            ? vm->script_path
            : ".",
    };

    return true;
}

static bool run_until(
    LuneVM *vm,
    LuneValue *result,
    size_t stop_depth
) {
    for (;;) {
        CallFrame *frame =
            current_frame(vm);

        if (
            frame == NULL ||
            frame->ip >=
                frame->chunk->count
        ) {
            return runtime_error(
                vm,
                (LuneSpan){0},
                "bytecode ended without return"
            );
        }

        size_t instruction =
            frame->ip;

        LuneSpan span =
            frame->chunk
                ->spans[instruction];

        LuneOpcode opcode =
            (LuneOpcode)
                frame->chunk
                    ->code[
                        frame->ip++
                    ];

        uint16_t index;
        LuneValue a;
        LuneValue b;

        switch (opcode) {
            case LUNE_OP_CONSTANT:
                if (
                    !read_u16(
                        vm,
                        frame,
                        &index,
                        span
                    ) ||
                    index >=
                        frame->chunk
                            ->constants_count
                ) {
                    return runtime_error(
                        vm,
                        span,
                        "invalid constant index"
                    );
                }

                if (!push(
                    vm,
                    frame->chunk
                        ->constants[index],
                    span
                )) {
                    return false;
                }
                break;

            case LUNE_OP_NULL:
                if (!push(
                    vm,
                    lune_value_null(),
                    span
                )) {
                    return false;
                }
                break;

            case LUNE_OP_TRUE:
                if (!push(
                    vm,
                    lune_value_bool(true),
                    span
                )) {
                    return false;
                }
                break;

            case LUNE_OP_FALSE:
                if (!push(
                    vm,
                    lune_value_bool(false),
                    span
                )) {
                    return false;
                }
                break;

            case LUNE_OP_STRING: {
                const LuneName *name;

                if (
                    !read_u16(
                        vm,
                        frame,
                        &index,
                        span
                    ) ||
                    !read_name(
                        vm,
                        frame,
                        index,
                        span,
                        &name
                    )
                ) {
                    return false;
                }

                LuneObjString *string =
                    lune_string_new(
                        &vm->heap,
                        name->chars,
                        name->length
                    );

                if (
                    string == NULL
                ) {
                    return runtime_error(
                        vm,
                        span,
                        "out of memory"
                    );
                }

                if (!push(
                    vm,
                    lune_value_obj(
                        (LuneObj *)string
                    ),
                    span
                )) {
                    return false;
                }
                break;
            }

            case LUNE_OP_LIST: {
                if (!read_u16(
                    vm,
                    frame,
                    &index,
                    span
                )) {
                    return false;
                }

                size_t count =
                    index;

                if (
                    vm->stack_count <
                    count
                ) {
                    return runtime_error(
                        vm,
                        span,
                        "internal stack underflow building list"
                    );
                }

                size_t base =
                    vm->stack_count -
                    count;

                LuneObjList *list =
                    lune_list_new(
                        &vm->heap,
                        vm->stack + base,
                        count
                    );

                if (list == NULL) {
                    return runtime_error(
                        vm,
                        span,
                        "out of memory"
                    );
                }

                vm->stack_count =
                    base;

                if (!push(
                    vm,
                    lune_value_obj(
                        (LuneObj *)list
                    ),
                    span
                )) {
                    return false;
                }
                break;
            }

            case LUNE_OP_MAP: {
                if (!read_u16(
                    vm,
                    frame,
                    &index,
                    span
                )) {
                    return false;
                }

                size_t count =
                    index;

                if (
                    count >
                        SIZE_MAX / 2 ||
                    vm->stack_count <
                        count * 2
                ) {
                    return runtime_error(
                        vm,
                        span,
                        "internal stack underflow building map"
                    );
                }

                size_t base =
                    vm->stack_count -
                    count * 2;

                LuneObjMap *map =
                    lune_map_new(
                        &vm->heap
                    );

                if (map == NULL) {
                    return runtime_error(
                        vm,
                        span,
                        "out of memory"
                    );
                }

                for (
                    size_t i = 0;
                    i < count;
                    i++
                ) {
                    LuneValue key_value =
                        vm->stack[
                            base +
                            i * 2
                        ];

                    LuneValue value =
                        vm->stack[
                            base +
                            i * 2 + 1
                        ];

                    LuneObjString *key =
                        as_string(
                            key_value
                        );

                    if (
                        key == NULL
                    ) {
                        return runtime_error(
                            vm,
                            span,
                            "internal map key is not a string"
                        );
                    }

                    if (!lune_map_set(
                        &vm->heap,
                        map,
                        key,
                        value
                    )) {
                        return runtime_error(
                            vm,
                            span,
                            "out of memory"
                        );
                    }
                }

                vm->stack_count =
                    base;

                if (!push(
                    vm,
                    lune_value_obj(
                        (LuneObj *)map
                    ),
                    span
                )) {
                    return false;
                }
                break;
            }

            case LUNE_OP_POP:
                if (!pop(
                    vm, &a, span
                )) {
                    return false;
                }
                break;

            case LUNE_OP_GET_LOCAL:
                if (!read_u16(
                    vm,
                    frame,
                    &index,
                    span
                )) {
                    return false;
                }

                if (
                    index >=
                        LOCAL_MAX ||
                    !frame
                        ->local_defined[
                            index
                        ]
                ) {
                    return runtime_error(
                        vm,
                        span,
                        "unknown local binding"
                    );
                }

                if (!push(
                    vm,
                    frame->locals[
                        index
                    ],
                    span
                )) {
                    return false;
                }
                break;

            case LUNE_OP_SET_LOCAL:
                if (
                    !read_u16(
                        vm,
                        frame,
                        &index,
                        span
                    ) ||
                    index >=
                        LOCAL_MAX ||
                    !peek_value(
                        vm,
                        &a,
                        span
                    )
                ) {
                    return false;
                }

                frame->locals[
                    index
                ] = a;

                frame->local_defined[
                    index
                ] = true;
                break;

            case LUNE_OP_GET_UPVALUE:
                if (
                    !read_u16(
                        vm,
                        frame,
                        &index,
                        span
                    ) ||
                    frame->closure ==
                        NULL ||
                    index >=
                        frame->closure
                            ->upvalue_count
                ) {
                    return runtime_error(
                        vm,
                        span,
                        "invalid captured binding"
                    );
                }

                if (!push(
                    vm,
                    *frame->closure
                        ->upvalues[index]
                        ->location,
                    span
                )) {
                    return false;
                }
                break;

            case LUNE_OP_SET_UPVALUE:
                if (
                    !read_u16(
                        vm,
                        frame,
                        &index,
                        span
                    ) ||
                    frame->closure ==
                        NULL ||
                    index >=
                        frame->closure
                            ->upvalue_count ||
                    !peek_value(
                        vm,
                        &a,
                        span
                    )
                ) {
                    return false;
                }

                *frame->closure
                    ->upvalues[index]
                    ->location = a;
                break;

            case LUNE_OP_GET_GLOBAL: {
                const LuneName *name;

                if (
                    !read_u16(
                        vm,
                        frame,
                        &index,
                        span
                    ) ||
                    !read_name(
                        vm,
                        frame,
                        index,
                        span,
                        &name
                    )
                ) {
                    return false;
                }

                if (!lune_map_get_chars(
                    vm->globals,
                    name->chars,
                    name->length,
                    &a
                )) {
                    return runtime_error(
                        vm,
                        span,
                        "unknown global binding"
                    );
                }

                if (!push(
                    vm, a, span
                )) {
                    return false;
                }
                break;
            }

            case LUNE_OP_DEFINE_GLOBAL: {
                const LuneName *name;

                if (
                    !read_u16(
                        vm,
                        frame,
                        &index,
                        span
                    ) ||
                    !read_name(
                        vm,
                        frame,
                        index,
                        span,
                        &name
                    ) ||
                    !peek_value(
                        vm,
                        &a,
                        span
                    )
                ) {
                    return false;
                }

                if (lune_map_get_chars(
                    vm->globals,
                    name->chars,
                    name->length,
                    &b
                )) {
                    return runtime_error(
                        vm,
                        span,
                        "binding already declared in this scope"
                    );
                }

                if (!lune_map_set_chars(
                    &vm->heap,
                    vm->globals,
                    name->chars,
                    name->length,
                    a
                )) {
                    return runtime_error(
                        vm,
                        span,
                        "out of memory"
                    );
                }
                break;
            }

            case LUNE_OP_SET_GLOBAL: {
                const LuneName *name;

                if (
                    !read_u16(
                        vm,
                        frame,
                        &index,
                        span
                    ) ||
                    !read_name(
                        vm,
                        frame,
                        index,
                        span,
                        &name
                    ) ||
                    !peek_value(
                        vm,
                        &a,
                        span
                    )
                ) {
                    return false;
                }

                if (!lune_map_get_chars(
                    vm->globals,
                    name->chars,
                    name->length,
                    &b
                )) {
                    return runtime_error(
                        vm,
                        span,
                        "assignment to unknown binding"
                    );
                }

                if (!lune_map_set_chars(
                    &vm->heap,
                    vm->globals,
                    name->chars,
                    name->length,
                    a
                )) {
                    return runtime_error(
                        vm,
                        span,
                        "out of memory"
                    );
                }
                break;
            }

            case LUNE_OP_GET_INDEX:
                if (!get_index(
                    vm, span
                )) {
                    return false;
                }
                break;

            case LUNE_OP_SET_INDEX:
                if (!set_index(
                    vm, span
                )) {
                    return false;
                }
                break;

            case LUNE_OP_GET_FIELD:
                if (
                    !read_u16(
                        vm,
                        frame,
                        &index,
                        span
                    ) ||
                    !get_field(
                        vm,
                        frame,
                        index,
                        span
                    )
                ) {
                    return false;
                }
                break;

            case LUNE_OP_SET_FIELD:
                if (
                    !read_u16(
                        vm,
                        frame,
                        &index,
                        span
                    ) ||
                    !set_field(
                        vm,
                        frame,
                        index,
                        span
                    )
                ) {
                    return false;
                }
                break;

            case LUNE_OP_ADD:
            case LUNE_OP_SUBTRACT:
            case LUNE_OP_MULTIPLY:
            case LUNE_OP_DIVIDE:
            case LUNE_OP_MODULO:
                if (!arithmetic(
                    vm,
                    opcode,
                    span
                )) {
                    return false;
                }
                break;

            case LUNE_OP_EQUAL:
            case LUNE_OP_NOT_EQUAL:
                if (
                    !pop(
                        vm,
                        &b,
                        span
                    ) ||
                    !pop(
                        vm,
                        &a,
                        span
                    )
                ) {
                    return false;
                }

                if (!push(
                    vm,
                    lune_value_bool(
                        opcode ==
                            LUNE_OP_EQUAL
                        ? lune_value_equal(
                            a, b
                        )
                        : !lune_value_equal(
                            a, b
                        )
                    ),
                    span
                )) {
                    return false;
                }
                break;

            case LUNE_OP_LESS:
            case LUNE_OP_LESS_EQUAL:
            case LUNE_OP_GREATER:
            case LUNE_OP_GREATER_EQUAL:
                if (!compare(
                    vm,
                    opcode,
                    span
                )) {
                    return false;
                }
                break;

            case LUNE_OP_NOT:
                if (!pop(
                    vm,
                    &a,
                    span
                )) {
                    return false;
                }

                if (!push(
                    vm,
                    lune_value_bool(
                        !lune_value_truthy(
                            a
                        )
                    ),
                    span
                )) {
                    return false;
                }
                break;

            case LUNE_OP_NEGATE:
                if (!pop(
                    vm,
                    &a,
                    span
                )) {
                    return false;
                }

                if (
                    a.kind ==
                    LUNE_VALUE_INT
                ) {
                    if (
                        a.as.integer ==
                        INT64_MIN
                    ) {
                        return runtime_error(
                            vm,
                            span,
                            "integer overflow"
                        );
                    }

                    if (!push(
                        vm,
                        lune_value_int(
                            -a.as.integer
                        ),
                        span
                    )) {
                        return false;
                    }
                } else if (
                    a.kind ==
                    LUNE_VALUE_FLOAT
                ) {
                    if (!push(
                        vm,
                        lune_value_float(
                            -a.as.floating
                        ),
                        span
                    )) {
                        return false;
                    }
                } else {
                    return runtime_error(
                        vm,
                        span,
                        "negation operand must be a number"
                    );
                }
                break;

            case LUNE_OP_JUMP:
                if (
                    !read_u16(
                        vm,
                        frame,
                        &index,
                        span
                    ) ||
                    frame->ip +
                        index >
                        frame->chunk
                            ->count
                ) {
                    return runtime_error(
                        vm,
                        span,
                        "invalid jump"
                    );
                }

                frame->ip += index;
                break;

            case LUNE_OP_JUMP_IF_FALSE:
                if (
                    !read_u16(
                        vm,
                        frame,
                        &index,
                        span
                    ) ||
                    !peek_value(
                        vm,
                        &a,
                        span
                    )
                ) {
                    return false;
                }

                if (!lune_value_truthy(
                    a
                )) {
                    if (
                        frame->ip +
                            index >
                        frame->chunk
                            ->count
                    ) {
                        return runtime_error(
                            vm,
                            span,
                            "invalid conditional jump"
                        );
                    }

                    frame->ip +=
                        index;
                }
                break;

            case LUNE_OP_LOOP:
                if (
                    !read_u16(
                        vm,
                        frame,
                        &index,
                        span
                    ) ||
                    index >
                        frame->ip
                ) {
                    return runtime_error(
                        vm,
                        span,
                        "invalid loop jump"
                    );
                }

                frame->ip -= index;
                break;

            case LUNE_OP_CLOSURE: {
                if (
                    !read_u16(
                        vm,
                        frame,
                        &index,
                        span
                    ) ||
                    index >=
                        frame->chunk
                            ->functions_count
                ) {
                    return runtime_error(
                        vm,
                        span,
                        "invalid function constant"
                    );
                }

                const LuneFunction *function =
                    frame->chunk
                        ->functions[index];

                LuneObjClosure *closure =
                    lune_closure_new(
                        &vm->heap,
                        function,
                        function
                            ->upvalue_count
                    );

                if (
                    closure == NULL
                ) {
                    return runtime_error(
                        vm,
                        span,
                        "out of memory"
                    );
                }

                closure->module_path =
                    frame->module_path;

                /*
                 * Root the closure before capture_upvalue()
                 * performs any further allocations.
                 */
                if (!push(
                    vm,
                    lune_value_obj(
                        (LuneObj *)closure
                    ),
                    span
                )) {
                    return false;
                }

                for (
                    size_t i = 0;
                    i <
                        function
                            ->upvalue_count;
                    i++
                ) {
                    LuneUpvalueDesc desc =
                        function
                            ->upvalues[i];

                    if (desc.is_local) {
                        if (
                            desc.index >=
                            LOCAL_MAX
                        ) {
                            return runtime_error(
                                vm,
                                span,
                                "invalid local capture"
                            );
                        }

                        closure
                            ->upvalues[i] =
                            capture_upvalue(
                                vm,
                                &frame
                                    ->locals[
                                        desc.index
                                    ]
                            );

                        if (
                            closure
                                ->upvalues[i] ==
                            NULL
                        ) {
                            return runtime_error(
                                vm,
                                span,
                                "out of memory"
                            );
                        }
                    } else {
                        if (
                            frame->closure ==
                                NULL ||
                            desc.index >=
                                frame->closure
                                    ->upvalue_count
                        ) {
                            return runtime_error(
                                vm,
                                span,
                                "invalid outer capture"
                            );
                        }

                        closure
                            ->upvalues[i] =
                            frame->closure
                                ->upvalues[
                                    desc.index
                                ];
                    }
                }

                break;
            }

            case LUNE_OP_CALL:
                if (!read_u16(
                    vm,
                    frame,
                    &index,
                    span
                )) {
                    return false;
                }

                if (!call_value(
                    vm,
                    index,
                    span
                )) {
                    return false;
                }

                if (vm->exit_requested) {
                    vm->last_result =
                        lune_value_null();
                    vm->has_result = true;

                    if (result != NULL) {
                        *result =
                            vm->last_result;
                    }

                    return true;
                }
                break;

            case LUNE_OP_RETURN: {
                if (!pop(
                    vm,
                    &a,
                    span
                )) {
                    return false;
                }

                close_frame_upvalues(
                    vm, frame
                );

                size_t stack_base =
                    frame->stack_base;

                if (
                    vm->frame_count ==
                    stop_depth + 1
                ) {
                    vm->frame_count--;
                    vm->stack_count =
                        stack_base;

                    if (stop_depth == 0) {
                        vm->last_result = a;
                        vm->has_result = true;
                    }

                    if (
                        result != NULL
                    ) {
                        *result = a;
                    }

                    return true;
                }

                vm->frame_count--;

                vm->stack_count =
                    stack_base;

                if (!push(
                    vm, a, span
                )) {
                    return false;
                }
                break;
            }

            default:
                return runtime_error(
                    vm,
                    span,
                    "unknown bytecode instruction"
                );
        }
    }
}

bool lune_vm_run(
    LuneVM *vm,
    const LuneChunk *chunk,
    LuneValue *result
) {
    if (!prepare_run(
        vm, chunk
    )) {
        return runtime_error(
            vm,
            (LuneSpan){0},
            "out of memory"
        );
    }

    return run_until(
        vm,
        result,
        0
    );
}
