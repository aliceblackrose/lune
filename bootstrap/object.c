#include "object.h"

#include "bytecode.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_GC_THRESHOLD (1024u * 1024u)

static void account_add(
    LuneHeap *heap,
    size_t bytes
) {
    if (SIZE_MAX - heap->bytes_allocated < bytes) {
        heap->bytes_allocated = SIZE_MAX;
    } else {
        heap->bytes_allocated += bytes;
    }
}

static void account_remove(
    LuneHeap *heap,
    size_t bytes
) {
    if (bytes > heap->bytes_allocated) {
        heap->bytes_allocated = 0;
    } else {
        heap->bytes_allocated -= bytes;
    }
}

static size_t object_bytes(
    const LuneObj *object
) {
    switch (object->kind) {
        case LUNE_OBJ_STRING: {
            const LuneObjString *string =
                (const LuneObjString *)object;
            return sizeof(*string) +
                string->length + 1;
        }

        case LUNE_OBJ_LIST: {
            const LuneObjList *list =
                (const LuneObjList *)object;
            return sizeof(*list) +
                list->capacity *
                    sizeof(*list->items);
        }

        case LUNE_OBJ_MAP: {
            const LuneObjMap *map =
                (const LuneObjMap *)object;
            return sizeof(*map) +
                map->capacity *
                    sizeof(*map->entries);
        }

        case LUNE_OBJ_CLOSURE: {
            const LuneObjClosure *closure =
                (const LuneObjClosure *)object;
            return sizeof(*closure) +
                closure->upvalue_count *
                    sizeof(*closure->upvalues);
        }

        case LUNE_OBJ_UPVALUE:
            return sizeof(LuneObjUpvalue);

        case LUNE_OBJ_NATIVE:
            return sizeof(LuneObjNative);
    }

    return 0;
}

static void free_object(
    LuneHeap *heap,
    LuneObj *object
) {
    size_t bytes = object_bytes(object);

    switch (object->kind) {
        case LUNE_OBJ_STRING:
            free(
                ((LuneObjString *)object)->chars
            );
            break;

        case LUNE_OBJ_LIST:
            free(
                ((LuneObjList *)object)->items
            );
            break;

        case LUNE_OBJ_MAP:
            free(
                ((LuneObjMap *)object)->entries
            );
            break;

        case LUNE_OBJ_CLOSURE:
            free(
                ((LuneObjClosure *)object)
                    ->upvalues
            );
            break;

        case LUNE_OBJ_UPVALUE:
        case LUNE_OBJ_NATIVE:
            break;
    }

    account_remove(heap, bytes);
    free(object);
}

static bool gray_push(
    LuneHeap *heap,
    LuneObj *object
) {
    if (
        heap->gray_count ==
        heap->gray_capacity
    ) {
        size_t next =
            heap->gray_capacity == 0
            ? 32
            : heap->gray_capacity * 2;

        if (
            next < heap->gray_capacity ||
            next >
                SIZE_MAX /
                sizeof(*heap->gray)
        ) {
            heap->mark_failed = true;
            return false;
        }

        LuneObj **grown = realloc(
            heap->gray,
            next * sizeof(*heap->gray)
        );

        if (grown == NULL) {
            heap->mark_failed = true;
            return false;
        }

        heap->gray = grown;
        heap->gray_capacity = next;
    }

    heap->gray[heap->gray_count++] =
        object;
    return true;
}

void lune_heap_mark_object(
    LuneHeap *heap,
    LuneObj *object
) {
    if (
        object == NULL ||
        object->marked
    ) {
        return;
    }

    object->marked = true;
    (void)gray_push(heap, object);
}

void lune_heap_mark_value(
    LuneHeap *heap,
    LuneValue value
) {
    if (value.kind == LUNE_VALUE_OBJ) {
        lune_heap_mark_object(
            heap, value.as.object
        );
    }
}

static void blacken_object(
    LuneHeap *heap,
    LuneObj *object
) {
    switch (object->kind) {
        case LUNE_OBJ_STRING:
        case LUNE_OBJ_NATIVE:
            break;

        case LUNE_OBJ_LIST: {
            LuneObjList *list =
                (LuneObjList *)object;

            for (
                size_t i = 0;
                i < list->count;
                i++
            ) {
                lune_heap_mark_value(
                    heap, list->items[i]
                );
            }
            break;
        }

        case LUNE_OBJ_MAP: {
            LuneObjMap *map =
                (LuneObjMap *)object;

            for (
                size_t i = 0;
                i < map->count;
                i++
            ) {
                lune_heap_mark_object(
                    heap,
                    (LuneObj *)
                        map->entries[i].key
                );

                lune_heap_mark_value(
                    heap,
                    map->entries[i].value
                );
            }
            break;
        }

        case LUNE_OBJ_CLOSURE: {
            LuneObjClosure *closure =
                (LuneObjClosure *)object;

            for (
                size_t i = 0;
                i <
                    closure->upvalue_count;
                i++
            ) {
                lune_heap_mark_object(
                    heap,
                    (LuneObj *)
                        closure->upvalues[i]
                );
            }
            break;
        }

        case LUNE_OBJ_UPVALUE: {
            LuneObjUpvalue *upvalue =
                (LuneObjUpvalue *)object;

            if (upvalue->location != NULL) {
                lune_heap_mark_value(
                    heap,
                    *upvalue->location
                );
            }
            break;
        }
    }
}

static void trace_references(
    LuneHeap *heap
) {
    while (
        heap->gray_count > 0 &&
        !heap->mark_failed
    ) {
        LuneObj *object =
            heap->gray[
                --heap->gray_count
            ];

        blacken_object(heap, object);
    }
}

static void clear_marks(
    LuneHeap *heap
) {
    for (
        LuneObj *object = heap->objects;
        object != NULL;
        object = object->next
    ) {
        object->marked = false;
    }

    heap->gray_count = 0;
}

static void sweep(
    LuneHeap *heap
) {
    LuneObj **cursor =
        &heap->objects;

    while (*cursor != NULL) {
        LuneObj *object = *cursor;

        if (object->marked) {
            object->marked = false;
            cursor = &object->next;
        } else {
            *cursor = object->next;
            free_object(heap, object);
        }
    }
}

void lune_heap_collect(
    LuneHeap *heap
) {
    heap->mark_failed = false;
    heap->gray_count = 0;

    if (heap->mark_roots != NULL) {
        heap->mark_roots(
            heap->mark_context,
            heap
        );
    }

    trace_references(heap);

    if (heap->mark_failed) {
        /*
         * If the gray stack cannot grow, retaining
         * garbage is safer than sweeping reachable
         * children that we could not trace.
         */
        clear_marks(heap);

        if (
            heap->bytes_allocated >
            SIZE_MAX / 2
        ) {
            heap->next_gc = SIZE_MAX;
        } else {
            heap->next_gc =
                heap->bytes_allocated * 2;
        }

        if (
            heap->next_gc <
            INITIAL_GC_THRESHOLD
        ) {
            heap->next_gc =
                INITIAL_GC_THRESHOLD;
        }
        return;
    }

    sweep(heap);

    if (
        heap->bytes_allocated >
        SIZE_MAX / 2
    ) {
        heap->next_gc = SIZE_MAX;
    } else {
        heap->next_gc =
            heap->bytes_allocated * 2;
    }

    if (
        heap->next_gc <
        INITIAL_GC_THRESHOLD
    ) {
        heap->next_gc =
            INITIAL_GC_THRESHOLD;
    }
}

static LuneObj *allocate_object(
    LuneHeap *heap,
    size_t size,
    LuneObjKind kind
) {
    if (
        heap->stress_gc ||
        heap->bytes_allocated + size >
            heap->next_gc
    ) {
        lune_heap_collect(heap);
    }

    LuneObj *object = calloc(1, size);
    if (object == NULL) return NULL;

    object->kind = kind;
    object->next = heap->objects;
    heap->objects = object;

    account_add(heap, size);
    return object;
}

void lune_heap_init(
    LuneHeap *heap,
    LuneMarkRootsFn mark_roots,
    void *mark_context
) {
    *heap = (LuneHeap){
        .next_gc =
            INITIAL_GC_THRESHOLD,
        .mark_roots = mark_roots,
        .mark_context = mark_context,
    };
}

void lune_heap_free(
    LuneHeap *heap
) {
    LuneObj *object = heap->objects;

    while (object != NULL) {
        LuneObj *next = object->next;
        free_object(heap, object);
        object = next;
    }

    free(heap->gray);
    *heap = (LuneHeap){0};
}

void lune_heap_set_stress(
    LuneHeap *heap,
    bool enabled
) {
    heap->stress_gc = enabled;
}

size_t lune_heap_bytes(
    const LuneHeap *heap
) {
    return heap->bytes_allocated;
}

LuneObjString *lune_string_new(
    LuneHeap *heap,
    const char *chars,
    size_t length
) {
    LuneObjString *string =
        (LuneObjString *)allocate_object(
            heap,
            sizeof(*string),
            LUNE_OBJ_STRING
        );

    if (string == NULL) return NULL;

    string->chars = malloc(length + 1);

    if (string->chars == NULL) {
        return NULL;
    }

    if (length > 0) {
        memcpy(
            string->chars,
            chars,
            length
        );
    }

    string->chars[length] = '\0';
    string->length = length;

    account_add(heap, length + 1);
    return string;
}

LuneObjString *lune_string_concat(
    LuneHeap *heap,
    const LuneObjString *a,
    const LuneObjString *b
) {
    if (
        a->length >
        SIZE_MAX - b->length
    ) {
        return NULL;
    }

    size_t length =
        a->length + b->length;

    char *chars =
        malloc(length + 1);

    if (chars == NULL) {
        return NULL;
    }

    memcpy(
        chars,
        a->chars,
        a->length
    );

    memcpy(
        chars + a->length,
        b->chars,
        b->length
    );

    chars[length] = '\0';

    LuneObjString *string =
        (LuneObjString *)allocate_object(
            heap,
            sizeof(*string),
            LUNE_OBJ_STRING
        );

    if (string == NULL) {
        free(chars);
        return NULL;
    }

    string->chars = chars;
    string->length = length;
    account_add(heap, length + 1);
    return string;
}

LuneObjList *lune_list_new(
    LuneHeap *heap,
    const LuneValue *items,
    size_t count
) {
    LuneObjList *list =
        (LuneObjList *)allocate_object(
            heap,
            sizeof(*list),
            LUNE_OBJ_LIST
        );

    if (list == NULL) return NULL;

    if (count > 0) {
        if (
            count >
            SIZE_MAX /
                sizeof(*list->items)
        ) {
            return NULL;
        }

        size_t bytes =
            count * sizeof(*list->items);

        list->items = malloc(bytes);

        if (list->items == NULL) {
            return NULL;
        }

        memcpy(
            list->items,
            items,
            bytes
        );

        account_add(heap, bytes);
    }

    list->count = count;
    list->capacity = count;
    return list;
}

bool lune_list_push(
    LuneHeap *heap,
    LuneObjList *list,
    LuneValue value
) {
    if (
        list->count ==
        list->capacity
    ) {
        size_t next =
            list->capacity == 0
            ? 8
            : list->capacity * 2;

        if (
            next < list->capacity ||
            next >
                SIZE_MAX /
                    sizeof(*list->items)
        ) {
            return false;
        }

        size_t old_bytes =
            list->capacity *
            sizeof(*list->items);

        size_t new_bytes =
            next *
            sizeof(*list->items);

        LuneValue *grown =
            realloc(
                list->items,
                new_bytes
            );

        if (grown == NULL) {
            return false;
        }

        list->items = grown;
        list->capacity = next;

        account_add(
            heap,
            new_bytes - old_bytes
        );
    }

    list->items[list->count++] =
        value;

    return true;
}

bool lune_list_pop(
    LuneObjList *list,
    LuneValue *value
) {
    if (list->count == 0) {
        return false;
    }

    list->count--;
    *value = list->items[
        list->count
    ];
    list->items[list->count] =
        lune_value_null();

    return true;
}

LuneObjMap *lune_map_new(
    LuneHeap *heap
) {
    return (LuneObjMap *)allocate_object(
        heap,
        sizeof(LuneObjMap),
        LUNE_OBJ_MAP
    );
}

static bool string_equal_chars(
    const LuneObjString *string,
    const char *chars,
    size_t length
) {
    return string->length == length &&
        memcmp(
            string->chars,
            chars,
            length
        ) == 0;
}

bool lune_map_get_chars(
    const LuneObjMap *map,
    const char *chars,
    size_t length,
    LuneValue *value
) {
    for (
        size_t i = 0;
        i < map->count;
        i++
    ) {
        if (string_equal_chars(
            map->entries[i].key,
            chars,
            length
        )) {
            *value =
                map->entries[i].value;
            return true;
        }
    }

    return false;
}

bool lune_map_get(
    const LuneObjMap *map,
    const LuneObjString *key,
    LuneValue *value
) {
    return lune_map_get_chars(
        map,
        key->chars,
        key->length,
        value
    );
}

static bool map_append(
    LuneHeap *heap,
    LuneObjMap *map,
    LuneObjString *key,
    LuneValue value
) {
    if (
        map->count ==
        map->capacity
    ) {
        size_t next =
            map->capacity == 0
            ? 8
            : map->capacity * 2;

        if (
            next < map->capacity ||
            next >
                SIZE_MAX /
                sizeof(*map->entries)
        ) {
            return false;
        }

        size_t old_bytes =
            map->capacity *
            sizeof(*map->entries);

        size_t new_bytes =
            next *
            sizeof(*map->entries);

        LuneMapValue *grown =
            realloc(
                map->entries,
                new_bytes
            );

        if (grown == NULL) {
            return false;
        }

        map->entries = grown;
        map->capacity = next;

        account_add(
            heap,
            new_bytes - old_bytes
        );
    }

    map->entries[map->count++] =
        (LuneMapValue){
            .key = key,
            .value = value,
        };

    return true;
}

bool lune_map_set(
    LuneHeap *heap,
    LuneObjMap *map,
    LuneObjString *key,
    LuneValue value
) {
    for (
        size_t i = 0;
        i < map->count;
        i++
    ) {
        if (string_equal_chars(
            map->entries[i].key,
            key->chars,
            key->length
        )) {
            map->entries[i].value =
                value;
            return true;
        }
    }

    return map_append(
        heap, map, key, value
    );
}

bool lune_map_set_chars(
    LuneHeap *heap,
    LuneObjMap *map,
    const char *chars,
    size_t length,
    LuneValue value
) {
    for (
        size_t i = 0;
        i < map->count;
        i++
    ) {
        if (string_equal_chars(
            map->entries[i].key,
            chars,
            length
        )) {
            map->entries[i].value =
                value;
            return true;
        }
    }

    LuneObjString *key =
        lune_string_new(
            heap, chars, length
        );

    if (key == NULL) return false;

    return map_append(
        heap, map, key, value
    );
}

LuneObjClosure *lune_closure_new(
    LuneHeap *heap,
    const LuneFunction *function,
    size_t upvalue_count
) {
    LuneObjClosure *closure =
        (LuneObjClosure *)
        allocate_object(
            heap,
            sizeof(*closure),
            LUNE_OBJ_CLOSURE
        );

    if (closure == NULL) {
        return NULL;
    }

    if (upvalue_count > 0) {
        if (
            upvalue_count >
            SIZE_MAX /
                sizeof(
                    *closure->upvalues
                )
        ) {
            return NULL;
        }

        size_t bytes =
            upvalue_count *
            sizeof(*closure->upvalues);

        closure->upvalues =
            calloc(
                upvalue_count,
                sizeof(*closure->upvalues)
            );

        if (
            closure->upvalues == NULL
        ) {
            return NULL;
        }

        account_add(heap, bytes);
    }

    closure->function = function;
    closure->upvalue_count =
        upvalue_count;
    return closure;
}

LuneObjUpvalue *lune_upvalue_new(
    LuneHeap *heap,
    LuneValue *location
) {
    LuneObjUpvalue *upvalue =
        (LuneObjUpvalue *)
        allocate_object(
            heap,
            sizeof(*upvalue),
            LUNE_OBJ_UPVALUE
        );

    if (upvalue == NULL) {
        return NULL;
    }

    upvalue->location = location;
    upvalue->closed =
        lune_value_null();
    return upvalue;
}

LuneObjNative *lune_native_new(
    LuneHeap *heap,
    const char *name,
    int arity,
    LuneNativeFn function
) {
    LuneObjNative *native =
        (LuneObjNative *)
        allocate_object(
            heap,
            sizeof(*native),
            LUNE_OBJ_NATIVE
        );

    if (native == NULL) {
        return NULL;
    }

    native->name = name;
    native->arity = arity;
    native->function = function;
    return native;
}

bool lune_obj_is_string(
    const LuneObj *object
) {
    return object != NULL &&
        object->kind ==
            LUNE_OBJ_STRING;
}

bool lune_obj_is_list(
    const LuneObj *object
) {
    return object != NULL &&
        object->kind ==
            LUNE_OBJ_LIST;
}

bool lune_obj_is_map(
    const LuneObj *object
) {
    return object != NULL &&
        object->kind ==
            LUNE_OBJ_MAP;
}

bool lune_obj_is_closure(
    const LuneObj *object
) {
    return object != NULL &&
        object->kind ==
            LUNE_OBJ_CLOSURE;
}

bool lune_obj_is_native(
    const LuneObj *object
) {
    return object != NULL &&
        object->kind ==
            LUNE_OBJ_NATIVE;
}

static void print_object(
    FILE *out,
    const LuneObj *object,
    unsigned depth
);

static void print_value(
    FILE *out,
    LuneValue value,
    unsigned depth
) {
    switch (value.kind) {
        case LUNE_VALUE_NULL:
            fputs("null", out);
            break;

        case LUNE_VALUE_BOOL:
            fputs(
                value.as.boolean
                    ? "true"
                    : "false",
                out
            );
            break;

        case LUNE_VALUE_INT:
            fprintf(
                out,
                "%lld",
                (long long)
                    value.as.integer
            );
            break;

        case LUNE_VALUE_FLOAT:
            fprintf(
                out,
                "%.17g",
                value.as.floating
            );
            break;

        case LUNE_VALUE_OBJ:
            print_object(
                out,
                value.as.object,
                depth
            );
            break;
    }
}

static void print_object(
    FILE *out,
    const LuneObj *object,
    unsigned depth
) {
    if (depth > 8) {
        fputs("...", out);
        return;
    }

    switch (object->kind) {
        case LUNE_OBJ_STRING: {
            const LuneObjString *string =
                (const LuneObjString *)
                    object;

            fwrite(
                string->chars,
                1,
                string->length,
                out
            );
            break;
        }

        case LUNE_OBJ_LIST: {
            const LuneObjList *list =
                (const LuneObjList *)
                    object;

            fputc('[', out);

            for (
                size_t i = 0;
                i < list->count;
                i++
            ) {
                if (i != 0) {
                    fputs(", ", out);
                }

                print_value(
                    out,
                    list->items[i],
                    depth + 1
                );
            }

            fputc(']', out);
            break;
        }

        case LUNE_OBJ_MAP: {
            const LuneObjMap *map =
                (const LuneObjMap *)
                    object;

            fputc('{', out);

            for (
                size_t i = 0;
                i < map->count;
                i++
            ) {
                if (i != 0) {
                    fputs(", ", out);
                }

                fwrite(
                    map->entries[i]
                        .key->chars,
                    1,
                    map->entries[i]
                        .key->length,
                    out
                );

                fputs(": ", out);

                print_value(
                    out,
                    map->entries[i]
                        .value,
                    depth + 1
                );
            }

            fputc('}', out);
            break;
        }

        case LUNE_OBJ_CLOSURE: {
            const LuneObjClosure *closure =
                (const LuneObjClosure *)
                    object;

            fprintf(
                out,
                "<fn/%u>",
                (unsigned)
                    closure->function
                        ->arity
            );
            break;
        }

        case LUNE_OBJ_UPVALUE:
            fputs("<upvalue>", out);
            break;

        case LUNE_OBJ_NATIVE: {
            const LuneObjNative *native =
                (const LuneObjNative *)
                    object;

            fprintf(
                out,
                "<native %s>",
                native->name
            );
            break;
        }
    }
}

void lune_object_print(
    FILE *out,
    const LuneObj *object
) {
    print_object(out, object, 0);
}
