#include "vm.h"

#include "object.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#define STACK_MAX 1024
#define LOCAL_MAX 256

struct LuneVM {
    LuneHeap heap;

    const LuneChunk *chunk;
    size_t ip;

    LuneValue stack[STACK_MAX];
    size_t stack_count;

    LuneValue locals[LOCAL_MAX];
    bool local_defined[LOCAL_MAX];

    LuneValue *globals;
    bool *global_defined;

    LuneDiagnosticFn diagnostic;
    void *diagnostic_context;
};

static bool runtime_error(
    LuneVM *vm,
    LuneSpan span,
    const char *message
) {
    if (vm->diagnostic != NULL) {
        vm->diagnostic(vm->diagnostic_context, span, message);
    }
    return false;
}

static bool push(LuneVM *vm, LuneValue value, LuneSpan span) {
    if (vm->stack_count >= STACK_MAX) {
        return runtime_error(vm, span, "runtime stack overflow");
    }
    vm->stack[vm->stack_count++] = value;
    return true;
}

static bool pop(LuneVM *vm, LuneValue *value, LuneSpan span) {
    if (vm->stack_count == 0) {
        return runtime_error(
            vm, span, "internal runtime stack underflow"
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
            vm, span, "internal runtime stack underflow"
        );
    }
    *value = vm->stack[vm->stack_count - 1];
    return true;
}

static bool read_u16(
    LuneVM *vm,
    uint16_t *value,
    LuneSpan span
) {
    if (vm->ip + 1 >= vm->chunk->count) {
        return runtime_error(
            vm, span, "truncated bytecode operand"
        );
    }

    *value = (uint16_t)(
        ((uint16_t)vm->chunk->code[vm->ip] << 8) |
        vm->chunk->code[vm->ip + 1]
    );
    vm->ip += 2;
    return true;
}

static bool read_name(
    LuneVM *vm,
    uint16_t index,
    LuneSpan span,
    const LuneName **name
) {
    if (index >= vm->chunk->names_count) {
        return runtime_error(
            vm, span, "invalid string/name constant"
        );
    }
    *name = &vm->chunk->names[index];
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
    ) return NULL;
    return (LuneObjString *)value.as.object;
}

static LuneObjList *as_list(LuneValue value) {
    if (
        value.kind != LUNE_VALUE_OBJ ||
        !lune_obj_is_list(value.as.object)
    ) return NULL;
    return (LuneObjList *)value.as.object;
}

static LuneObjMap *as_map(LuneValue value) {
    if (
        value.kind != LUNE_VALUE_OBJ ||
        !lune_obj_is_map(value.as.object)
    ) return NULL;
    return (LuneObjMap *)value.as.object;
}

static bool int_add(int64_t a, int64_t b, int64_t *out) {
    if (
        (b > 0 && a > INT64_MAX - b) ||
        (b < 0 && a < INT64_MIN - b)
    ) return false;
    *out = a + b;
    return true;
}

static bool int_sub(int64_t a, int64_t b, int64_t *out) {
    if (
        (b < 0 && a > INT64_MAX + b) ||
        (b > 0 && a < INT64_MIN + b)
    ) return false;
    *out = a - b;
    return true;
}

static bool int_mul(int64_t a, int64_t b, int64_t *out) {
    if (a == 0 || b == 0) {
        *out = 0;
        return true;
    }
    if (
        (a == -1 && b == INT64_MIN) ||
        (b == -1 && a == INT64_MIN)
    ) return false;

    if (a > 0) {
        if (
            (b > 0 && a > INT64_MAX / b) ||
            (b < 0 && b < INT64_MIN / a)
        ) return false;
    } else {
        if (
            (b > 0 && a < INT64_MIN / b) ||
            (b < 0 && b < INT64_MAX / a)
        ) return false;
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
    ) return false;

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

            LuneObjString *joined = lune_string_concat(
                &vm->heap, a, b
            );
            if (joined == NULL) {
                return runtime_error(vm, span, "out of memory");
            }
            return push(
                vm,
                lune_value_obj((LuneObj *)joined),
                span
            );
        }
    }

    if (!numeric(left) || !numeric(right)) {
        return runtime_error(
            vm, span, "arithmetic operands must be numbers"
        );
    }

    if (opcode == LUNE_OP_DIVIDE) {
        long double divisor = as_number(right);
        if (divisor == 0.0L) {
            return runtime_error(vm, span, "division by zero");
        }
        return push(
            vm,
            lune_value_float(
                (double)(as_number(left) / divisor)
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
                vm, span, "modulo operands must be integers"
            );
        }
        if (right.as.integer == 0) {
            return runtime_error(vm, span, "modulo by zero");
        }
        if (
            left.as.integer == INT64_MIN &&
            right.as.integer == -1
        ) {
            return push(vm, lune_value_int(0), span);
        }
        return push(
            vm,
            lune_value_int(
                left.as.integer % right.as.integer
            ),
            span
        );
    }

    if (
        left.kind == LUNE_VALUE_FLOAT ||
        right.kind == LUNE_VALUE_FLOAT
    ) {
        double a = left.kind == LUNE_VALUE_FLOAT
            ? left.as.floating
            : (double)left.as.integer;
        double b = right.kind == LUNE_VALUE_FLOAT
            ? right.as.floating
            : (double)right.as.integer;
        double result = opcode == LUNE_OP_ADD
            ? a + b
            : opcode == LUNE_OP_SUBTRACT
                ? a - b
                : a * b;
        return push(vm, lune_value_float(result), span);
    }

    int64_t result = 0;
    bool ok = opcode == LUNE_OP_ADD
        ? int_add(
            left.as.integer, right.as.integer, &result
        )
        : opcode == LUNE_OP_SUBTRACT
            ? int_sub(
                left.as.integer, right.as.integer, &result
            )
            : int_mul(
                left.as.integer, right.as.integer, &result
            );

    if (!ok) {
        return runtime_error(vm, span, "integer overflow");
    }
    return push(vm, lune_value_int(result), span);
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
    ) return false;

    if (!numeric(left) || !numeric(right)) {
        return runtime_error(
            vm, span, "ordering operands must be numbers"
        );
    }

    long double a = as_number(left);
    long double b = as_number(right);
    bool result = opcode == LUNE_OP_LESS
        ? a < b
        : opcode == LUNE_OP_LESS_EQUAL
            ? a <= b
            : opcode == LUNE_OP_GREATER
                ? a > b
                : a >= b;

    return push(vm, lune_value_bool(result), span);
}

static bool get_index(LuneVM *vm, LuneSpan span) {
    LuneValue index;
    LuneValue object;
    if (
        !pop(vm, &index, span) ||
        !pop(vm, &object, span)
    ) return false;

    LuneObjList *list = as_list(object);
    if (list != NULL) {
        if (index.kind != LUNE_VALUE_INT) {
            return runtime_error(
                vm, span, "list index must be an integer"
            );
        }
        if (
            index.as.integer < 0 ||
            (uint64_t)index.as.integer >= list->count
        ) {
            return runtime_error(
                vm, span, "list index is out of range"
            );
        }
        return push(
            vm, list->items[(size_t)index.as.integer], span
        );
    }

    LuneObjMap *map = as_map(object);
    if (map != NULL) {
        LuneObjString *key = as_string(index);
        if (key == NULL) {
            return runtime_error(
                vm, span, "map index must be a string"
            );
        }

        LuneValue value;
        if (!lune_map_get(map, key, &value)) {
            value = lune_value_null();
        }
        return push(vm, value, span);
    }

    return runtime_error(
        vm, span, "value does not support indexing"
    );
}

static bool set_index(LuneVM *vm, LuneSpan span) {
    LuneValue value;
    LuneValue index;
    LuneValue object;
    if (
        !pop(vm, &value, span) ||
        !pop(vm, &index, span) ||
        !pop(vm, &object, span)
    ) return false;

    LuneObjList *list = as_list(object);
    if (list != NULL) {
        if (index.kind != LUNE_VALUE_INT) {
            return runtime_error(
                vm, span, "list index must be an integer"
            );
        }
        if (
            index.as.integer < 0 ||
            (uint64_t)index.as.integer >= list->count
        ) {
            return runtime_error(
                vm, span, "list index is out of range"
            );
        }
        list->items[(size_t)index.as.integer] = value;
        return push(vm, value, span);
    }

    LuneObjMap *map = as_map(object);
    if (map != NULL) {
        LuneObjString *key = as_string(index);
        if (key == NULL) {
            return runtime_error(
                vm, span, "map index must be a string"
            );
        }
        if (!lune_map_set(&vm->heap, map, key, value)) {
            return runtime_error(vm, span, "out of memory");
        }
        return push(vm, value, span);
    }

    return runtime_error(
        vm, span, "value does not support indexed assignment"
    );
}

static bool get_field(
    LuneVM *vm,
    uint16_t index,
    LuneSpan span
) {
    const LuneName *name;
    if (!read_name(vm, index, span, &name)) return false;

    LuneValue object;
    if (!pop(vm, &object, span)) return false;

    LuneObjMap *map = as_map(object);
    if (map == NULL) {
        return runtime_error(
            vm, span, "member access requires a map"
        );
    }

    LuneValue value;
    if (!lune_map_get_chars(
        map, name->chars, name->length, &value
    )) {
        value = lune_value_null();
    }
    return push(vm, value, span);
}

static bool set_field(
    LuneVM *vm,
    uint16_t index,
    LuneSpan span
) {
    const LuneName *name;
    if (!read_name(vm, index, span, &name)) return false;

    LuneValue value;
    LuneValue object;
    if (
        !pop(vm, &value, span) ||
        !pop(vm, &object, span)
    ) return false;

    LuneObjMap *map = as_map(object);
    if (map == NULL) {
        return runtime_error(
            vm, span, "member assignment requires a map"
        );
    }

    if (!lune_map_set_chars(
        &vm->heap,
        map,
        name->chars,
        name->length,
        value
    )) {
        return runtime_error(vm, span, "out of memory");
    }
    return push(vm, value, span);
}

LuneVM *lune_vm_new(
    LuneDiagnosticFn diagnostic,
    void *diagnostic_context
) {
    LuneVM *vm = calloc(1, sizeof(*vm));
    if (vm == NULL) return NULL;
    lune_heap_init(&vm->heap);
    vm->diagnostic = diagnostic;
    vm->diagnostic_context = diagnostic_context;
    return vm;
}

void lune_vm_free(LuneVM *vm) {
    if (vm == NULL) return;
    lune_heap_free(&vm->heap);
    free(vm->globals);
    free(vm->global_defined);
    free(vm);
}

static bool prepare_run(LuneVM *vm, const LuneChunk *chunk) {
    lune_heap_free(&vm->heap);
    lune_heap_init(&vm->heap);

    free(vm->globals);
    free(vm->global_defined);
    vm->globals = NULL;
    vm->global_defined = NULL;

    vm->chunk = chunk;
    vm->ip = 0;
    vm->stack_count = 0;

    for (size_t i = 0; i < LOCAL_MAX; i++) {
        vm->local_defined[i] = false;
    }

    if (chunk->names_count == 0) return true;

    vm->globals = calloc(
        chunk->names_count, sizeof(*vm->globals)
    );
    vm->global_defined = calloc(
        chunk->names_count, sizeof(*vm->global_defined)
    );
    if (
        vm->globals == NULL ||
        vm->global_defined == NULL
    ) {
        free(vm->globals);
        free(vm->global_defined);
        vm->globals = NULL;
        vm->global_defined = NULL;
        return false;
    }
    return true;
}

bool lune_vm_run(
    LuneVM *vm,
    const LuneChunk *chunk,
    LuneValue *result
) {
    if (!prepare_run(vm, chunk)) {
        return runtime_error(
            vm, (LuneSpan){0}, "out of memory"
        );
    }

    while (vm->ip < chunk->count) {
        size_t instruction = vm->ip;
        LuneSpan span = chunk->spans[instruction];
        LuneOpcode opcode = (LuneOpcode)chunk->code[vm->ip++];

        uint16_t index;
        LuneValue a;
        LuneValue b;

        switch (opcode) {
            case LUNE_OP_CONSTANT:
                if (
                    !read_u16(vm, &index, span) ||
                    index >= chunk->constants_count
                ) {
                    return runtime_error(
                        vm, span, "invalid constant index"
                    );
                }
                if (!push(vm, chunk->constants[index], span)) {
                    return false;
                }
                break;

            case LUNE_OP_NULL:
                if (!push(vm, lune_value_null(), span)) return false;
                break;

            case LUNE_OP_TRUE:
                if (!push(vm, lune_value_bool(true), span)) return false;
                break;

            case LUNE_OP_FALSE:
                if (!push(vm, lune_value_bool(false), span)) return false;
                break;

            case LUNE_OP_STRING: {
                const LuneName *name;
                if (
                    !read_u16(vm, &index, span) ||
                    !read_name(vm, index, span, &name)
                ) return false;

                LuneObjString *string = lune_string_new(
                    &vm->heap, name->chars, name->length
                );
                if (string == NULL) {
                    return runtime_error(
                        vm, span, "out of memory"
                    );
                }
                if (!push(
                    vm,
                    lune_value_obj((LuneObj *)string),
                    span
                )) return false;
                break;
            }

            case LUNE_OP_LIST: {
                if (!read_u16(vm, &index, span)) return false;
                size_t count = index;
                if (vm->stack_count < count) {
                    return runtime_error(
                        vm,
                        span,
                        "internal stack underflow building list"
                    );
                }

                size_t base = vm->stack_count - count;
                LuneObjList *list = lune_list_new(
                    &vm->heap, vm->stack + base, count
                );
                if (list == NULL) {
                    return runtime_error(
                        vm, span, "out of memory"
                    );
                }

                vm->stack_count = base;
                if (!push(
                    vm,
                    lune_value_obj((LuneObj *)list),
                    span
                )) return false;
                break;
            }

            case LUNE_OP_MAP: {
                if (!read_u16(vm, &index, span)) return false;
                size_t count = index;
                if (
                    count > SIZE_MAX / 2 ||
                    vm->stack_count < count * 2
                ) {
                    return runtime_error(
                        vm,
                        span,
                        "internal stack underflow building map"
                    );
                }

                size_t base = vm->stack_count - count * 2;
                LuneObjMap *map = lune_map_new(&vm->heap);
                if (map == NULL) {
                    return runtime_error(
                        vm, span, "out of memory"
                    );
                }

                for (size_t i = 0; i < count; i++) {
                    LuneValue key_value =
                        vm->stack[base + i * 2];
                    LuneValue value =
                        vm->stack[base + i * 2 + 1];
                    LuneObjString *key = as_string(key_value);
                    if (key == NULL) {
                        return runtime_error(
                            vm,
                            span,
                            "internal map key is not a string"
                        );
                    }
                    if (!lune_map_set(
                        &vm->heap, map, key, value
                    )) {
                        return runtime_error(
                            vm, span, "out of memory"
                        );
                    }
                }

                vm->stack_count = base;
                if (!push(
                    vm,
                    lune_value_obj((LuneObj *)map),
                    span
                )) return false;
                break;
            }

            case LUNE_OP_POP:
                if (!pop(vm, &a, span)) return false;
                break;

            case LUNE_OP_GET_LOCAL:
                if (!read_u16(vm, &index, span)) return false;
                if (
                    index >= LOCAL_MAX ||
                    !vm->local_defined[index]
                ) {
                    return runtime_error(
                        vm, span, "unknown local binding"
                    );
                }
                if (!push(vm, vm->locals[index], span)) {
                    return false;
                }
                break;

            case LUNE_OP_SET_LOCAL:
                if (
                    !read_u16(vm, &index, span) ||
                    index >= LOCAL_MAX ||
                    !peek_value(vm, &a, span)
                ) return false;
                vm->locals[index] = a;
                vm->local_defined[index] = true;
                break;

            case LUNE_OP_GET_GLOBAL:
                if (!read_u16(vm, &index, span)) return false;
                if (
                    index >= chunk->names_count ||
                    !vm->global_defined[index]
                ) {
                    return runtime_error(
                        vm, span, "unknown global binding"
                    );
                }
                if (!push(vm, vm->globals[index], span)) {
                    return false;
                }
                break;

            case LUNE_OP_DEFINE_GLOBAL:
                if (
                    !read_u16(vm, &index, span) ||
                    index >= chunk->names_count ||
                    !peek_value(vm, &a, span)
                ) return false;
                if (vm->global_defined[index]) {
                    return runtime_error(
                        vm,
                        span,
                        "binding already declared in this scope"
                    );
                }
                vm->globals[index] = a;
                vm->global_defined[index] = true;
                break;

            case LUNE_OP_SET_GLOBAL:
                if (
                    !read_u16(vm, &index, span) ||
                    index >= chunk->names_count ||
                    !peek_value(vm, &a, span)
                ) return false;
                if (!vm->global_defined[index]) {
                    return runtime_error(
                        vm,
                        span,
                        "assignment to unknown binding"
                    );
                }
                vm->globals[index] = a;
                break;

            case LUNE_OP_GET_INDEX:
                if (!get_index(vm, span)) return false;
                break;

            case LUNE_OP_SET_INDEX:
                if (!set_index(vm, span)) return false;
                break;

            case LUNE_OP_GET_FIELD:
                if (
                    !read_u16(vm, &index, span) ||
                    !get_field(vm, index, span)
                ) return false;
                break;

            case LUNE_OP_SET_FIELD:
                if (
                    !read_u16(vm, &index, span) ||
                    !set_field(vm, index, span)
                ) return false;
                break;

            case LUNE_OP_ADD:
            case LUNE_OP_SUBTRACT:
            case LUNE_OP_MULTIPLY:
            case LUNE_OP_DIVIDE:
            case LUNE_OP_MODULO:
                if (!arithmetic(vm, opcode, span)) return false;
                break;

            case LUNE_OP_EQUAL:
            case LUNE_OP_NOT_EQUAL:
                if (
                    !pop(vm, &b, span) ||
                    !pop(vm, &a, span)
                ) return false;
                if (!push(
                    vm,
                    lune_value_bool(
                        opcode == LUNE_OP_EQUAL
                            ? lune_value_equal(a, b)
                            : !lune_value_equal(a, b)
                    ),
                    span
                )) return false;
                break;

            case LUNE_OP_LESS:
            case LUNE_OP_LESS_EQUAL:
            case LUNE_OP_GREATER:
            case LUNE_OP_GREATER_EQUAL:
                if (!compare(vm, opcode, span)) return false;
                break;

            case LUNE_OP_NOT:
                if (!pop(vm, &a, span)) return false;
                if (!push(
                    vm,
                    lune_value_bool(!lune_value_truthy(a)),
                    span
                )) return false;
                break;

            case LUNE_OP_NEGATE:
                if (!pop(vm, &a, span)) return false;
                if (a.kind == LUNE_VALUE_INT) {
                    if (a.as.integer == INT64_MIN) {
                        return runtime_error(
                            vm, span, "integer overflow"
                        );
                    }
                    if (!push(
                        vm,
                        lune_value_int(-a.as.integer),
                        span
                    )) return false;
                } else if (a.kind == LUNE_VALUE_FLOAT) {
                    if (!push(
                        vm,
                        lune_value_float(-a.as.floating),
                        span
                    )) return false;
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
                    !read_u16(vm, &index, span) ||
                    vm->ip + index > chunk->count
                ) {
                    return runtime_error(
                        vm, span, "invalid jump"
                    );
                }
                vm->ip += index;
                break;

            case LUNE_OP_JUMP_IF_FALSE:
                if (
                    !read_u16(vm, &index, span) ||
                    !peek_value(vm, &a, span)
                ) return false;
                if (!lune_value_truthy(a)) {
                    if (vm->ip + index > chunk->count) {
                        return runtime_error(
                            vm,
                            span,
                            "invalid conditional jump"
                        );
                    }
                    vm->ip += index;
                }
                break;

            case LUNE_OP_LOOP:
                if (
                    !read_u16(vm, &index, span) ||
                    index > vm->ip
                ) {
                    return runtime_error(
                        vm, span, "invalid loop jump"
                    );
                }
                vm->ip -= index;
                break;

            case LUNE_OP_RETURN:
                if (!pop(vm, &a, span)) return false;
                if (result != NULL) *result = a;
                return true;

            default:
                return runtime_error(
                    vm, span, "unknown bytecode instruction"
                );
        }
    }

    return runtime_error(
        vm, (LuneSpan){0}, "bytecode ended without return"
    );
}
