#include "vm.h"

#include "object.h"

#include <limits.h>
#include <stdint.h>
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
    LuneValue locals[LOCAL_MAX];
    bool local_defined[LOCAL_MAX];
} CallFrame;

struct LuneVM {
    LuneHeap heap;

    LuneValue stack[STACK_MAX];
    size_t stack_count;

    CallFrame frames[FRAME_MAX];
    size_t frame_count;

    LuneObjMap *globals;
    LuneObjUpvalue *open_upvalues;

    LuneDiagnosticFn diagnostic;
    void *diagnostic_context;
};

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

static CallFrame *current_frame(LuneVM *vm) {
    if (vm->frame_count == 0) return NULL;
    return &vm->frames[vm->frame_count - 1];
}

static bool push(
    LuneVM *vm,
    LuneValue value,
    LuneSpan span
) {
    if (vm->stack_count >= STACK_MAX) {
        return runtime_error(
            vm, span, "runtime stack overflow"
        );
    }

    vm->stack[vm->stack_count++] = value;
    return true;
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

    *value = vm->stack[--vm->stack_count];
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

    *value = vm->stack[vm->stack_count - 1];
    return true;
}

static bool read_u16(
    LuneVM *vm,
    CallFrame *frame,
    uint16_t *value,
    LuneSpan span
) {
    if (frame->ip + 1 >= frame->chunk->count) {
        return runtime_error(
            vm, span, "truncated bytecode operand"
        );
    }

    *value = (uint16_t)(
        ((uint16_t)frame->chunk->code[frame->ip] << 8) |
        frame->chunk->code[frame->ip + 1]
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
    if (index >= frame->chunk->names_count) {
        return runtime_error(
            vm, span, "invalid string/name constant"
        );
    }

    *name = &frame->chunk->names[index];
    return true;
}

static bool numeric(LuneValue value) {
    return value.kind == LUNE_VALUE_INT ||
        value.kind == LUNE_VALUE_FLOAT;
}

static long double as_number(LuneValue value) {
    return value.kind == LUNE_VALUE_INT
        ? (long double)value.as.integer
        : (long double)value.as.floating;
}

static LuneObjString *as_string(LuneValue value) {
    if (
        value.kind != LUNE_VALUE_OBJ ||
        !lune_obj_is_string(value.as.object)
    ) {
        return NULL;
    }

    return (LuneObjString *)value.as.object;
}

static LuneObjList *as_list(LuneValue value) {
    if (
        value.kind != LUNE_VALUE_OBJ ||
        !lune_obj_is_list(value.as.object)
    ) {
        return NULL;
    }

    return (LuneObjList *)value.as.object;
}

static LuneObjMap *as_map(LuneValue value) {
    if (
        value.kind != LUNE_VALUE_OBJ ||
        !lune_obj_is_map(value.as.object)
    ) {
        return NULL;
    }

    return (LuneObjMap *)value.as.object;
}

static LuneObjClosure *as_closure(LuneValue value) {
    if (
        value.kind != LUNE_VALUE_OBJ ||
        !lune_obj_is_closure(value.as.object)
    ) {
        return NULL;
    }

    return (LuneObjClosure *)value.as.object;
}

static LuneObjNative *as_native(LuneValue value) {
    if (
        value.kind != LUNE_VALUE_OBJ ||
        !lune_obj_is_native(value.as.object)
    ) {
        return NULL;
    }

    return (LuneObjNative *)value.as.object;
}

static bool int_add(
    int64_t a,
    int64_t b,
    int64_t *out
) {
    if (
        (b > 0 && a > INT64_MAX - b) ||
        (b < 0 && a < INT64_MIN - b)
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
        (b < 0 && a > INT64_MAX + b) ||
        (b > 0 && a < INT64_MIN + b)
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
        (a == -1 && b == INT64_MIN) ||
        (b == -1 && a == INT64_MIN)
    ) {
        return false;
    }

    if (a > 0) {
        if (
            (b > 0 && a > INT64_MAX / b) ||
            (b < 0 && b < INT64_MIN / a)
        ) {
            return false;
        }
    } else {
        if (
            (b > 0 && a < INT64_MIN / b) ||
            (b < 0 && b < INT64_MAX / a)
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
    LuneValue right;
    LuneValue left;

    if (
        !pop(vm, &right, span) ||
        !pop(vm, &left, span)
    ) {
        return false;
    }

    if (opcode == LUNE_OP_ADD) {
        LuneObjString *a = as_string(left);
        LuneObjString *b = as_string(right);

        if (a != NULL || b != NULL) {
            if (a == NULL || b == NULL) {
                return runtime_error(
                    vm,
                    span,
                    "addition operands must both be strings or both be numbers"
                );
            }

            LuneObjString *joined =
                lune_string_concat(
                    &vm->heap, a, b
                );

            if (joined == NULL) {
                return runtime_error(
                    vm, span, "out of memory"
                );
            }

            return push(
                vm,
                lune_value_obj(
                    (LuneObj *)joined
                ),
                span
            );
        }
    }

    if (!numeric(left) || !numeric(right)) {
        return runtime_error(
            vm,
            span,
            "arithmetic operands must be numbers"
        );
    }

    if (opcode == LUNE_OP_DIVIDE) {
        long double divisor = as_number(right);

        if (divisor == 0.0L) {
            return runtime_error(
                vm, span, "division by zero"
            );
        }

        return push(
            vm,
            lune_value_float(
                (double)(
                    as_number(left) / divisor
                )
            ),
            span
        );
    }

    if (opcode == LUNE_OP_MODULO) {
        if (
            left.kind != LUNE_VALUE_INT ||
            right.kind != LUNE_VALUE_INT
        ) {
            return runtime_error(
                vm,
                span,
                "modulo operands must be integers"
            );
        }

        if (right.as.integer == 0) {
            return runtime_error(
                vm, span, "modulo by zero"
            );
        }

        if (
            left.as.integer == INT64_MIN &&
            right.as.integer == -1
        ) {
            return push(
                vm, lune_value_int(0), span
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
        left.kind == LUNE_VALUE_FLOAT ||
        right.kind == LUNE_VALUE_FLOAT
    ) {
        double a =
            left.kind == LUNE_VALUE_FLOAT
            ? left.as.floating
            : (double)left.as.integer;

        double b =
            right.kind == LUNE_VALUE_FLOAT
            ? right.as.floating
            : (double)right.as.integer;

        double result =
            opcode == LUNE_OP_ADD
            ? a + b
            : opcode == LUNE_OP_SUBTRACT
                ? a - b
                : a * b;

        return push(
            vm,
            lune_value_float(result),
            span
        );
    }

    int64_t result = 0;
    bool ok =
        opcode == LUNE_OP_ADD
        ? int_add(
            left.as.integer,
            right.as.integer,
            &result
        )
        : opcode == LUNE_OP_SUBTRACT
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
            vm, span, "integer overflow"
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

    if (!numeric(left) || !numeric(right)) {
        return runtime_error(
            vm,
            span,
            "ordering operands must be numbers"
        );
    }

    long double a = as_number(left);
    long double b = as_number(right);

    bool result =
        opcode == LUNE_OP_LESS
        ? a < b
        : opcode == LUNE_OP_LESS_EQUAL
            ? a <= b
            : opcode == LUNE_OP_GREATER
                ? a > b
                : a >= b;

    return push(
        vm, lune_value_bool(result), span
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

    LuneObjList *list = as_list(object);
    if (list != NULL) {
        if (index.kind != LUNE_VALUE_INT) {
            return runtime_error(
                vm,
                span,
                "list index must be an integer"
            );
        }

        if (
            index.as.integer < 0 ||
            (uint64_t)index.as.integer >=
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
                (size_t)index.as.integer
            ],
            span
        );
    }

    LuneObjMap *map = as_map(object);
    if (map != NULL) {
        LuneObjString *key = as_string(index);

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
            value = lune_value_null();
        }

        return push(vm, value, span);
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

    LuneObjList *list = as_list(object);
    if (list != NULL) {
        if (index.kind != LUNE_VALUE_INT) {
            return runtime_error(
                vm,
                span,
                "list index must be an integer"
            );
        }

        if (
            index.as.integer < 0 ||
            (uint64_t)index.as.integer >=
                list->count
        ) {
            return runtime_error(
                vm,
                span,
                "list index is out of range"
            );
        }

        list->items[
            (size_t)index.as.integer
        ] = value;

        return push(vm, value, span);
    }

    LuneObjMap *map = as_map(object);
    if (map != NULL) {
        LuneObjString *key = as_string(index);

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
                vm, span, "out of memory"
            );
        }

        return push(vm, value, span);
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
        vm, frame, index, span, &name
    )) {
        return false;
    }

    LuneValue object;
    if (!pop(vm, &object, span)) {
        return false;
    }

    LuneObjMap *map = as_map(object);
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
        value = lune_value_null();
    }

    return push(vm, value, span);
}

static bool set_field(
    LuneVM *vm,
    CallFrame *frame,
    uint16_t index,
    LuneSpan span
) {
    const LuneName *name;

    if (!read_name(
        vm, frame, index, span, &name
    )) {
        return false;
    }

    LuneValue value;
    LuneValue object;

    if (
        !pop(vm, &value, span) ||
        !pop(vm, &object, span)
    ) {
        return false;
    }

    LuneObjMap *map = as_map(object);

    if (map == NULL) {
        return runtime_error(
            vm,
            span,
            "member assignment requires a map"
        );
    }

    if (!lune_map_set_chars(
        &vm->heap,
        map,
        name->chars,
        name->length,
        value
    )) {
        return runtime_error(
            vm, span, "out of memory"
        );
    }

    return push(vm, value, span);
}

static LuneObjUpvalue *capture_upvalue(
    LuneVM *vm,
    LuneValue *location
) {
    for (
        LuneObjUpvalue *upvalue =
            vm->open_upvalues;
        upvalue != NULL;
        upvalue = upvalue->next_open
    ) {
        if (upvalue->location == location) {
            return upvalue;
        }
    }

    LuneObjUpvalue *upvalue =
        lune_upvalue_new(
            &vm->heap, location
        );

    if (upvalue == NULL) return NULL;

    upvalue->next_open = vm->open_upvalues;
    vm->open_upvalues = upvalue;
    return upvalue;
}

static bool upvalue_belongs_to_frame(
    const LuneObjUpvalue *upvalue,
    const CallFrame *frame
) {
    for (size_t i = 0; i < LOCAL_MAX; i++) {
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
        LuneObjUpvalue *upvalue = *cursor;

        if (upvalue_belongs_to_frame(
            upvalue, frame
        )) {
            upvalue->closed =
                *upvalue->location;
            upvalue->location =
                &upvalue->closed;
            *cursor = upvalue->next_open;
            upvalue->next_open = NULL;
        } else {
            cursor = &upvalue->next_open;
        }
    }
}

static LuneValue native_print(
    int argc,
    const LuneValue *args
) {
    for (int i = 0; i < argc; i++) {
        if (i != 0) fputc(' ', stdout);
        lune_value_print(stdout, args[i]);
    }
    fputc('\n', stdout);
    return lune_value_null();
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

    if (native == NULL) return false;

    return lune_map_set_chars(
        &vm->heap,
        vm->globals,
        name,
        strlen(name),
        lune_value_obj(
            (LuneObj *)native
        )
    );
}

static bool call_closure(
    LuneVM *vm,
    LuneObjClosure *closure,
    uint16_t argc,
    size_t callee_index,
    LuneSpan span
) {
    if (
        argc != closure->function->arity
    ) {
        return runtime_error(
            vm,
            span,
            "wrong number of arguments"
        );
    }

    if (vm->frame_count >= FRAME_MAX) {
        return runtime_error(
            vm,
            span,
            "maximum call depth exceeded"
        );
    }

    CallFrame *frame =
        &vm->frames[vm->frame_count];

    *frame = (CallFrame){
        .chunk = &closure->function->chunk,
        .stack_base = callee_index,
        .closure = closure,
    };

    for (uint16_t i = 0; i < argc; i++) {
        frame->locals[i] =
            vm->stack[
                callee_index + 1 + i
            ];
        frame->local_defined[i] = true;
    }

    vm->stack_count = callee_index;
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
        argc != (uint16_t)native->arity
    ) {
        return runtime_error(
            vm,
            span,
            "wrong number of arguments"
        );
    }

    LuneValue result = native->function(
        (int)argc,
        vm->stack + callee_index + 1
    );

    vm->stack_count = callee_index;
    return push(vm, result, span);
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
        vm->stack_count - (size_t)argc - 1;

    LuneValue callee =
        vm->stack[callee_index];

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

LuneVM *lune_vm_new(
    LuneDiagnosticFn diagnostic,
    void *diagnostic_context
) {
    LuneVM *vm = calloc(
        1, sizeof(*vm)
    );

    if (vm == NULL) return NULL;

    lune_heap_init(&vm->heap);
    vm->diagnostic = diagnostic;
    vm->diagnostic_context =
        diagnostic_context;
    return vm;
}

void lune_vm_free(LuneVM *vm) {
    if (vm == NULL) return;

    lune_heap_free(&vm->heap);
    free(vm);
}

static bool prepare_run(
    LuneVM *vm,
    const LuneChunk *chunk
) {
    lune_heap_free(&vm->heap);
    lune_heap_init(&vm->heap);

    vm->stack_count = 0;
    vm->frame_count = 0;
    vm->open_upvalues = NULL;

    vm->globals = lune_map_new(
        &vm->heap
    );

    if (vm->globals == NULL) {
        return false;
    }

    if (!define_native(
        vm, "print", -1, native_print
    )) {
        return false;
    }

    CallFrame *root =
        &vm->frames[vm->frame_count++];

    *root = (CallFrame){
        .chunk = chunk,
        .ip = 0,
        .stack_base = 0,
        .closure = NULL,
    };

    return true;
}

bool lune_vm_run(
    LuneVM *vm,
    const LuneChunk *chunk,
    LuneValue *result
) {
    if (!prepare_run(vm, chunk)) {
        return runtime_error(
            vm,
            (LuneSpan){0},
            "out of memory"
        );
    }

    for (;;) {
        CallFrame *frame =
            current_frame(vm);

        if (
            frame == NULL ||
            frame->ip >= frame->chunk->count
        ) {
            return runtime_error(
                vm,
                (LuneSpan){0},
                "bytecode ended without return"
            );
        }

        size_t instruction = frame->ip;
        LuneSpan span =
            frame->chunk->spans[instruction];

        LuneOpcode opcode =
            (LuneOpcode)
            frame->chunk->code[frame->ip++];

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
                        frame->chunk->constants_count
                ) {
                    return runtime_error(
                        vm,
                        span,
                        "invalid constant index"
                    );
                }

                if (!push(
                    vm,
                    frame->chunk->constants[index],
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

                if (string == NULL) {
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

                size_t count = index;

                if (
                    vm->stack_count < count
                ) {
                    return runtime_error(
                        vm,
                        span,
                        "internal stack underflow building list"
                    );
                }

                size_t base =
                    vm->stack_count - count;

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

                vm->stack_count = base;

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

                size_t count = index;

                if (
                    count > SIZE_MAX / 2 ||
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
                    lune_map_new(&vm->heap);

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
                            base + i * 2
                        ];

                    LuneValue value =
                        vm->stack[
                            base + i * 2 + 1
                        ];

                    LuneObjString *key =
                        as_string(key_value);

                    if (key == NULL) {
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

                vm->stack_count = base;

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
                    index >= LOCAL_MAX ||
                    !frame->local_defined[index]
                ) {
                    return runtime_error(
                        vm,
                        span,
                        "unknown local binding"
                    );
                }

                if (!push(
                    vm,
                    frame->locals[index],
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
                    index >= LOCAL_MAX ||
                    !peek_value(
                        vm, &a, span
                    )
                ) {
                    return false;
                }

                frame->locals[index] = a;
                frame->local_defined[index] = true;
                break;

            case LUNE_OP_GET_UPVALUE:
                if (
                    !read_u16(
                        vm,
                        frame,
                        &index,
                        span
                    ) ||
                    frame->closure == NULL ||
                    index >=
                        frame->closure->upvalue_count
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
                    frame->closure == NULL ||
                    index >=
                        frame->closure->upvalue_count ||
                    !peek_value(
                        vm, &a, span
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

                if (!push(vm, a, span)) {
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
                        vm, &a, span
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
                        vm, &a, span
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
                    vm, opcode, span
                )) {
                    return false;
                }
                break;

            case LUNE_OP_EQUAL:
            case LUNE_OP_NOT_EQUAL:
                if (
                    !pop(vm, &b, span) ||
                    !pop(vm, &a, span)
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
                    vm, opcode, span
                )) {
                    return false;
                }
                break;

            case LUNE_OP_NOT:
                if (!pop(
                    vm, &a, span
                )) {
                    return false;
                }

                if (!push(
                    vm,
                    lune_value_bool(
                        !lune_value_truthy(a)
                    ),
                    span
                )) {
                    return false;
                }
                break;

            case LUNE_OP_NEGATE:
                if (!pop(
                    vm, &a, span
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
                    frame->ip + index >
                        frame->chunk->count
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
                        vm, &a, span
                    )
                ) {
                    return false;
                }

                if (!lune_value_truthy(a)) {
                    if (
                        frame->ip + index >
                        frame->chunk->count
                    ) {
                        return runtime_error(
                            vm,
                            span,
                            "invalid conditional jump"
                        );
                    }

                    frame->ip += index;
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
                    index > frame->ip
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
                        function->upvalue_count
                    );

                if (closure == NULL) {
                    return runtime_error(
                        vm,
                        span,
                        "out of memory"
                    );
                }

                for (
                    size_t i = 0;
                    i <
                        function->upvalue_count;
                    i++
                ) {
                    LuneUpvalueDesc desc =
                        function->upvalues[i];

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

                        closure->upvalues[i] =
                            capture_upvalue(
                                vm,
                                &frame->locals[
                                    desc.index
                                ]
                            );

                        if (
                            closure->upvalues[i] ==
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

                        closure->upvalues[i] =
                            frame->closure
                                ->upvalues[
                                    desc.index
                                ];
                    }
                }

                if (!push(
                    vm,
                    lune_value_obj(
                        (LuneObj *)closure
                    ),
                    span
                )) {
                    return false;
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
                    vm, index, span
                )) {
                    return false;
                }
                break;

            case LUNE_OP_RETURN: {
                if (!pop(
                    vm, &a, span
                )) {
                    return false;
                }

                close_frame_upvalues(
                    vm, frame
                );

                size_t stack_base =
                    frame->stack_base;

                if (vm->frame_count == 1) {
                    vm->stack_count =
                        stack_base;

                    if (result != NULL) {
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
