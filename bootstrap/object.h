#ifndef LUNE_OBJECT_H
#define LUNE_OBJECT_H

#include "value.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct LuneFunction LuneFunction;
typedef struct LuneObjUpvalue LuneObjUpvalue;
typedef struct LuneHeap LuneHeap;
typedef struct LuneVM LuneVM;

typedef bool (*LuneNativeFn)(
    LuneVM *vm,
    int argc,
    const LuneValue *args,
    LuneValue *result
);

typedef void (*LuneMarkRootsFn)(
    void *context,
    LuneHeap *heap
);

typedef enum {
    LUNE_OBJ_STRING,
    LUNE_OBJ_LIST,
    LUNE_OBJ_MAP,
    LUNE_OBJ_CLOSURE,
    LUNE_OBJ_UPVALUE,
    LUNE_OBJ_NATIVE
} LuneObjKind;

struct LuneObj {
    LuneObjKind kind;
    bool marked;
    LuneObj *next;
};

typedef struct {
    LuneObj obj;
    size_t length;
    char *chars;
} LuneObjString;

typedef struct {
    LuneObj obj;
    size_t count;
    LuneValue *items;
} LuneObjList;

typedef struct {
    LuneObjString *key;
    LuneValue value;
} LuneMapValue;

typedef struct {
    LuneObj obj;
    size_t count;
    size_t capacity;
    LuneMapValue *entries;
} LuneObjMap;

typedef struct {
    LuneObj obj;
    const LuneFunction *function;
    LuneObjUpvalue **upvalues;
    size_t upvalue_count;
    const char *module_path;
} LuneObjClosure;

struct LuneObjUpvalue {
    LuneObj obj;
    LuneValue *location;
    LuneValue closed;
    LuneObjUpvalue *next_open;
};

typedef struct {
    LuneObj obj;
    const char *name;
    int arity;
    LuneNativeFn function;
} LuneObjNative;

struct LuneHeap {
    LuneObj *objects;

    size_t bytes_allocated;
    size_t next_gc;
    bool stress_gc;

    LuneObj **gray;
    size_t gray_count;
    size_t gray_capacity;
    bool mark_failed;

    LuneMarkRootsFn mark_roots;
    void *mark_context;
};

void lune_heap_init(
    LuneHeap *heap,
    LuneMarkRootsFn mark_roots,
    void *mark_context
);
void lune_heap_free(LuneHeap *heap);
void lune_heap_set_stress(
    LuneHeap *heap,
    bool enabled
);
void lune_heap_collect(LuneHeap *heap);
size_t lune_heap_bytes(
    const LuneHeap *heap
);

void lune_heap_mark_value(
    LuneHeap *heap,
    LuneValue value
);
void lune_heap_mark_object(
    LuneHeap *heap,
    LuneObj *object
);

LuneObjString *lune_string_new(
    LuneHeap *heap,
    const char *chars,
    size_t length
);
LuneObjString *lune_string_concat(
    LuneHeap *heap,
    const LuneObjString *a,
    const LuneObjString *b
);
LuneObjList *lune_list_new(
    LuneHeap *heap,
    const LuneValue *items,
    size_t count
);
LuneObjMap *lune_map_new(
    LuneHeap *heap
);

bool lune_map_get(
    const LuneObjMap *map,
    const LuneObjString *key,
    LuneValue *value
);
bool lune_map_get_chars(
    const LuneObjMap *map,
    const char *chars,
    size_t length,
    LuneValue *value
);
bool lune_map_set(
    LuneHeap *heap,
    LuneObjMap *map,
    LuneObjString *key,
    LuneValue value
);
bool lune_map_set_chars(
    LuneHeap *heap,
    LuneObjMap *map,
    const char *chars,
    size_t length,
    LuneValue value
);

LuneObjClosure *lune_closure_new(
    LuneHeap *heap,
    const LuneFunction *function,
    size_t upvalue_count
);
LuneObjUpvalue *lune_upvalue_new(
    LuneHeap *heap,
    LuneValue *location
);
LuneObjNative *lune_native_new(
    LuneHeap *heap,
    const char *name,
    int arity,
    LuneNativeFn function
);

bool lune_obj_is_string(
    const LuneObj *object
);
bool lune_obj_is_list(
    const LuneObj *object
);
bool lune_obj_is_map(
    const LuneObj *object
);
bool lune_obj_is_closure(
    const LuneObj *object
);
bool lune_obj_is_native(
    const LuneObj *object
);

void lune_object_print(
    FILE *out,
    const LuneObj *object
);

#endif
