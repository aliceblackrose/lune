#include "compiler.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { LuneSpan name; int depth; uint16_t slot; } Local;

typedef struct {
    const char *source;
    LuneChunk *chunk;
    LuneDiagnosticFn diagnostic;
    void *diagnostic_context;
    bool had_error;
    bool oom;
    int scope_depth;
    Local locals[UINT8_MAX + 1];
    size_t local_count;
    uint16_t *declared_globals;
    size_t declared_global_count;
    size_t declared_global_capacity;
} Compiler;

static void error_at(Compiler *c, LuneSpan span, const char *message) {
    c->had_error = true;
    if (c->diagnostic != NULL) c->diagnostic(c->diagnostic_context, span, message);
}

static bool emit_byte(Compiler *c, uint8_t byte, LuneSpan span) {
    if (lune_chunk_write(c->chunk, byte, span)) return true;
    if (!c->oom) { c->oom = true; error_at(c, span, "out of memory"); }
    return false;
}
static bool emit_op(Compiler *c, LuneOpcode op, LuneSpan span) { return emit_byte(c,(uint8_t)op,span); }
static bool emit_u16(Compiler *c, uint16_t value, LuneSpan span) {
    return emit_byte(c,(uint8_t)(value>>8),span) && emit_byte(c,(uint8_t)(value&0xff),span);
}
static bool emit_indexed(Compiler *c, LuneOpcode op, uint16_t index, LuneSpan span) {
    return emit_op(c,op,span) && emit_u16(c,index,span);
}
static size_t emit_jump(Compiler *c, LuneOpcode op, LuneSpan span) {
    if (!emit_op(c,op,span)) return SIZE_MAX;
    size_t operand=c->chunk->count;
    if (!emit_u16(c,UINT16_MAX,span)) return SIZE_MAX;
    return operand;
}
static bool patch_jump(Compiler *c, size_t operand, LuneSpan span) {
    if (operand==SIZE_MAX || c->chunk->count<operand+2) return false;
    size_t jump=c->chunk->count-operand-2;
    if (jump>UINT16_MAX) { error_at(c,span,"jump is too large"); return false; }
    c->chunk->code[operand]=(uint8_t)(jump>>8);
    c->chunk->code[operand+1]=(uint8_t)(jump&0xff);
    return true;
}
static bool emit_loop(Compiler *c, size_t start, LuneSpan span) {
    if (!emit_op(c,LUNE_OP_LOOP,span)) return false;
    size_t distance=(c->chunk->count+2)-start;
    if (distance>UINT16_MAX) { error_at(c,span,"loop body is too large"); return false; }
    return emit_u16(c,(uint16_t)distance,span);
}

static bool span_equal(const Compiler *c,LuneSpan a,LuneSpan b) {
    return a.length==b.length && memcmp(c->source+a.offset,c->source+b.offset,a.length)==0;
}
static int resolve_local(const Compiler *c,LuneSpan name) {
    for(size_t i=c->local_count;i>0;i--) if(span_equal(c,c->locals[i-1].name,name)) return (int)c->locals[i-1].slot;
    return -1;
}
static bool local_declared_here(const Compiler *c,LuneSpan name) {
    for(size_t i=c->local_count;i>0;i--) {
        const Local *local=&c->locals[i-1];
        if(local->depth<c->scope_depth) break;
        if(span_equal(c,local->name,name)) return true;
    }
    return false;
}
static bool add_local(Compiler *c,LuneSpan name,uint16_t *slot) {
    if(c->local_count>=UINT8_MAX+1u){error_at(c,name,"too many local bindings in scope");return false;}
    *slot=(uint16_t)c->local_count;
    c->locals[c->local_count++]=(Local){name,c->scope_depth,*slot};
    return true;
}
static void begin_scope(Compiler*c){c->scope_depth++;}
static void end_scope(Compiler*c){c->scope_depth--;while(c->local_count>0&&c->locals[c->local_count-1].depth>c->scope_depth)c->local_count--;}

static bool intern_name(Compiler*c,LuneSpan span,uint16_t*index){
    if(lune_chunk_intern_name(c->chunk,c->source+span.offset,span.length,index))return true;
    error_at(c,span,"too many names or out of memory");return false;
}
static bool global_declared(const Compiler*c,uint16_t name){for(size_t i=0;i<c->declared_global_count;i++)if(c->declared_globals[i]==name)return true;return false;}
static bool declare_global(Compiler*c,uint16_t name,LuneSpan span){
    if(global_declared(c,name)){error_at(c,span,"binding already declared in this scope");return false;}
    if(c->declared_global_count==c->declared_global_capacity){
        size_t next=c->declared_global_capacity==0?8:c->declared_global_capacity*2;
        uint16_t*grown=realloc(c->declared_globals,next*sizeof(*grown));
        if(grown==NULL){error_at(c,span,"out of memory");return false;}
        c->declared_globals=grown;c->declared_global_capacity=next;
    }
    c->declared_globals[c->declared_global_count++]=name;return true;
}

static bool compile_node(Compiler*c,const LuneAst*node);
static bool compile_sequence(Compiler*c,const LuneAstList*statements,LuneSpan span){
    if(statements->count==0)return emit_op(c,LUNE_OP_NULL,span);
    for(size_t i=0;i<statements->count;i++){
        if(!compile_node(c,statements->items[i]))return false;
        if(i+1<statements->count&&!emit_op(c,LUNE_OP_POP,statements->items[i]->span))return false;
    }
    return true;
}
static bool compile_number(Compiler*c,const LuneAst*node){
    char*text=malloc(node->span.length+1);if(text==NULL){error_at(c,node->span,"out of memory");return false;}
    memcpy(text,c->source+node->span.offset,node->span.length);text[node->span.length]='\0';errno=0;LuneValue value;
    if(strchr(text,'.')!=NULL){char*end=NULL;double number=strtod(text,&end);if(errno==ERANGE||end==text||*end!='\0'){free(text);error_at(c,node->span,"invalid floating-point literal");return false;}value=lune_value_float(number);}
    else{char*end=NULL;long long number=strtoll(text,&end,10);if(errno==ERANGE||end==text||*end!='\0'){free(text);error_at(c,node->span,"integer literal is out of range");return false;}value=lune_value_int((int64_t)number);}
    free(text);uint16_t index;if(!lune_chunk_add_constant(c->chunk,value,&index)){error_at(c,node->span,"too many constants or out of memory");return false;}return emit_indexed(c,LUNE_OP_CONSTANT,index,node->span);
}
static bool compile_name(Compiler*c,const LuneAst*node){
    int local=resolve_local(c,node->span);if(local>=0)return emit_indexed(c,LUNE_OP_GET_LOCAL,(uint16_t)local,node->span);
    uint16_t name;return intern_name(c,node->span,&name)&&emit_indexed(c,LUNE_OP_GET_GLOBAL,name,node->span);
}
static bool compile_binary(Compiler*c,const LuneAst*node){
    LuneTokenKind op=node->as.binary.op;
    if(op==LUNE_TOKEN_AND){if(!compile_node(c,node->as.binary.left))return false;size_t end=emit_jump(c,LUNE_OP_JUMP_IF_FALSE,node->span);if(!emit_op(c,LUNE_OP_POP,node->span)||!compile_node(c,node->as.binary.right))return false;return patch_jump(c,end,node->span);}
    if(op==LUNE_TOKEN_OR){if(!compile_node(c,node->as.binary.left))return false;size_t rhs=emit_jump(c,LUNE_OP_JUMP_IF_FALSE,node->span);size_t end=emit_jump(c,LUNE_OP_JUMP,node->span);if(!patch_jump(c,rhs,node->span)||!emit_op(c,LUNE_OP_POP,node->span)||!compile_node(c,node->as.binary.right))return false;return patch_jump(c,end,node->span);}
    if(!compile_node(c,node->as.binary.left)||!compile_node(c,node->as.binary.right))return false;
    LuneOpcode code;switch(op){
        case LUNE_TOKEN_PLUS:code=LUNE_OP_ADD;break;case LUNE_TOKEN_MINUS:code=LUNE_OP_SUBTRACT;break;case LUNE_TOKEN_STAR:code=LUNE_OP_MULTIPLY;break;case LUNE_TOKEN_SLASH:code=LUNE_OP_DIVIDE;break;case LUNE_TOKEN_PERCENT:code=LUNE_OP_MODULO;break;
        case LUNE_TOKEN_EQ:code=LUNE_OP_EQUAL;break;case LUNE_TOKEN_NE:code=LUNE_OP_NOT_EQUAL;break;case LUNE_TOKEN_LT:code=LUNE_OP_LESS;break;case LUNE_TOKEN_LE:code=LUNE_OP_LESS_EQUAL;break;case LUNE_TOKEN_GT:code=LUNE_OP_GREATER;break;case LUNE_TOKEN_GE:code=LUNE_OP_GREATER_EQUAL;break;
        default:error_at(c,node->span,"unsupported binary operator");return false;}
    return emit_op(c,code,node->span);
}
static bool compile_if(Compiler*c,const LuneAst*node){
    if(!compile_node(c,node->as.if_expr.condition))return false;size_t else_jump=emit_jump(c,LUNE_OP_JUMP_IF_FALSE,node->span);
    if(!emit_op(c,LUNE_OP_POP,node->span)||!compile_node(c,node->as.if_expr.then_branch))return false;size_t end_jump=emit_jump(c,LUNE_OP_JUMP,node->span);
    if(!patch_jump(c,else_jump,node->span)||!emit_op(c,LUNE_OP_POP,node->span))return false;
    if(node->as.if_expr.else_branch!=NULL){if(!compile_node(c,node->as.if_expr.else_branch))return false;}else if(!emit_op(c,LUNE_OP_NULL,node->span))return false;
    return patch_jump(c,end_jump,node->span);
}
static bool compile_while(Compiler*c,const LuneAst*node){
    size_t loop_start=c->chunk->count;if(!compile_node(c,node->as.while_stmt.condition))return false;size_t exit_jump=emit_jump(c,LUNE_OP_JUMP_IF_FALSE,node->span);
    if(!emit_op(c,LUNE_OP_POP,node->span)||!compile_node(c,node->as.while_stmt.body)||!emit_op(c,LUNE_OP_POP,node->span)||!emit_loop(c,loop_start,node->span))return false;
    if(!patch_jump(c,exit_jump,node->span)||!emit_op(c,LUNE_OP_POP,node->span))return false;return emit_op(c,LUNE_OP_NULL,node->span);
}
static bool compile_declare(Compiler*c,const LuneAst*node){
    LuneSpan name_span=node->as.declare.name;
    if(c->scope_depth>0){if(local_declared_here(c,name_span)){error_at(c,name_span,"binding already declared in this scope");return false;}if(!compile_node(c,node->as.declare.value))return false;uint16_t slot;return add_local(c,name_span,&slot)&&emit_indexed(c,LUNE_OP_SET_LOCAL,slot,name_span);}
    uint16_t name;if(!intern_name(c,name_span,&name)||!declare_global(c,name,name_span)||!compile_node(c,node->as.declare.value))return false;return emit_indexed(c,LUNE_OP_DEFINE_GLOBAL,name,name_span);
}
static bool compile_assign(Compiler*c,const LuneAst*node){
    const LuneAst*target=node->as.assign.target;if(target->kind!=LUNE_AST_NAME){error_at(c,target->span,"index and member assignment are not implemented in the bootstrap VM yet");return false;}
    if(!compile_node(c,node->as.assign.value))return false;int local=resolve_local(c,target->span);if(local>=0)return emit_indexed(c,LUNE_OP_SET_LOCAL,(uint16_t)local,target->span);
    uint16_t name;return intern_name(c,target->span,&name)&&emit_indexed(c,LUNE_OP_SET_GLOBAL,name,target->span);
}
static bool compile_node(Compiler*c,const LuneAst*node){
    if(node==NULL)return false;switch(node->kind){
        case LUNE_AST_PROGRAM:return compile_sequence(c,&node->as.sequence.statements,node->span);
        case LUNE_AST_BLOCK:{begin_scope(c);bool ok=compile_sequence(c,&node->as.sequence.statements,node->span);end_scope(c);return ok;}
        case LUNE_AST_NULL:return emit_op(c,LUNE_OP_NULL,node->span);case LUNE_AST_BOOL:return emit_op(c,node->as.boolean?LUNE_OP_TRUE:LUNE_OP_FALSE,node->span);case LUNE_AST_NUMBER:return compile_number(c,node);case LUNE_AST_NAME:return compile_name(c,node);
        case LUNE_AST_UNARY:if(!compile_node(c,node->as.unary.operand))return false;if(node->as.unary.op==LUNE_TOKEN_MINUS)return emit_op(c,LUNE_OP_NEGATE,node->span);if(node->as.unary.op==LUNE_TOKEN_NOT)return emit_op(c,LUNE_OP_NOT,node->span);error_at(c,node->span,"unsupported unary operator");return false;
        case LUNE_AST_BINARY:return compile_binary(c,node);case LUNE_AST_IF:return compile_if(c,node);case LUNE_AST_WHILE:return compile_while(c,node);case LUNE_AST_DECLARE:return compile_declare(c,node);case LUNE_AST_ASSIGN:return compile_assign(c,node);
        case LUNE_AST_STRING:error_at(c,node->span,"strings are not implemented in the bootstrap VM yet");return false;
        case LUNE_AST_LIST:case LUNE_AST_MAP:case LUNE_AST_INDEX:case LUNE_AST_MEMBER:error_at(c,node->span,"collections are not implemented in the bootstrap VM yet");return false;
        case LUNE_AST_FUNCTION:case LUNE_AST_CALL:error_at(c,node->span,"functions are not implemented in the bootstrap VM yet");return false;
    }return false;
}
bool lune_compile(const LuneAst*program,const char*source,LuneChunk*chunk,LuneDiagnosticFn diagnostic,void*diagnostic_context){
    Compiler c={.source=source,.chunk=chunk,.diagnostic=diagnostic,.diagnostic_context=diagnostic_context};
    bool ok=compile_node(&c,program);if(ok)ok=emit_op(&c,LUNE_OP_RETURN,program->span);free(c.declared_globals);return ok&&!c.had_error;
}
