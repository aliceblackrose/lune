#include "vm.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#define STACK_MAX 1024
#define LOCAL_MAX 256

typedef struct {
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
} VM;

static bool runtime_error(VM *vm,LuneSpan span,const char*message){if(vm->diagnostic)vm->diagnostic(vm->diagnostic_context,span,message);return false;}
static bool push(VM*vm,LuneValue value,LuneSpan span){if(vm->stack_count>=STACK_MAX)return runtime_error(vm,span,"runtime stack overflow");vm->stack[vm->stack_count++]=value;return true;}
static bool pop(VM*vm,LuneValue*value,LuneSpan span){if(vm->stack_count==0)return runtime_error(vm,span,"internal runtime stack underflow");*value=vm->stack[--vm->stack_count];return true;}
static bool peek_value(VM*vm,LuneValue*value,LuneSpan span){if(vm->stack_count==0)return runtime_error(vm,span,"internal runtime stack underflow");*value=vm->stack[vm->stack_count-1];return true;}
static bool read_u16(VM*vm,uint16_t*value,LuneSpan span){if(vm->ip+1>=vm->chunk->count)return runtime_error(vm,span,"truncated bytecode operand");*value=(uint16_t)(((uint16_t)vm->chunk->code[vm->ip]<<8)|vm->chunk->code[vm->ip+1]);vm->ip+=2;return true;}
static bool numeric(LuneValue v){return v.kind==LUNE_VALUE_INT||v.kind==LUNE_VALUE_FLOAT;}
static long double as_number(LuneValue v){return v.kind==LUNE_VALUE_INT?(long double)v.as.integer:(long double)v.as.floating;}
static bool int_add(int64_t a,int64_t b,int64_t*out){if((b>0&&a>INT64_MAX-b)||(b<0&&a<INT64_MIN-b))return false;*out=a+b;return true;}
static bool int_sub(int64_t a,int64_t b,int64_t*out){if((b<0&&a>INT64_MAX+b)||(b>0&&a<INT64_MIN+b))return false;*out=a-b;return true;}
static bool int_mul(int64_t a,int64_t b,int64_t*out){if(a==0||b==0){*out=0;return true;}if((a==-1&&b==INT64_MIN)||(b==-1&&a==INT64_MIN))return false;if(a>0){if((b>0&&a>INT64_MAX/b)||(b<0&&b<INT64_MIN/a))return false;}else{if((b>0&&a<INT64_MIN/b)||(b<0&&b<INT64_MAX/a))return false;}*out=a*b;return true;}

static bool arithmetic(VM*vm,LuneOpcode op,LuneSpan span){
    LuneValue b,a;if(!pop(vm,&b,span)||!pop(vm,&a,span))return false;if(!numeric(a)||!numeric(b))return runtime_error(vm,span,"arithmetic operands must be numbers");
    if(op==LUNE_OP_DIVIDE){long double d=as_number(b);if(d==0.0L)return runtime_error(vm,span,"division by zero");return push(vm,lune_value_float((double)(as_number(a)/d)),span);}
    if(op==LUNE_OP_MODULO){if(a.kind!=LUNE_VALUE_INT||b.kind!=LUNE_VALUE_INT)return runtime_error(vm,span,"modulo operands must be integers");if(b.as.integer==0)return runtime_error(vm,span,"modulo by zero");if(a.as.integer==INT64_MIN&&b.as.integer==-1)return push(vm,lune_value_int(0),span);return push(vm,lune_value_int(a.as.integer%b.as.integer),span);}
    if(a.kind==LUNE_VALUE_FLOAT||b.kind==LUNE_VALUE_FLOAT){double x=a.kind==LUNE_VALUE_FLOAT?a.as.floating:(double)a.as.integer;double y=b.kind==LUNE_VALUE_FLOAT?b.as.floating:(double)b.as.integer;double r=op==LUNE_OP_ADD?x+y:op==LUNE_OP_SUBTRACT?x-y:x*y;return push(vm,lune_value_float(r),span);}
    int64_t r=0;bool ok=op==LUNE_OP_ADD?int_add(a.as.integer,b.as.integer,&r):op==LUNE_OP_SUBTRACT?int_sub(a.as.integer,b.as.integer,&r):int_mul(a.as.integer,b.as.integer,&r);if(!ok)return runtime_error(vm,span,"integer overflow");return push(vm,lune_value_int(r),span);
}
static bool compare(VM*vm,LuneOpcode op,LuneSpan span){LuneValue b,a;if(!pop(vm,&b,span)||!pop(vm,&a,span))return false;if(!numeric(a)||!numeric(b))return runtime_error(vm,span,"ordering operands must be numbers");long double x=as_number(a),y=as_number(b);bool r=op==LUNE_OP_LESS?x<y:op==LUNE_OP_LESS_EQUAL?x<=y:op==LUNE_OP_GREATER?x>y:x>=y;return push(vm,lune_value_bool(r),span);}

bool lune_vm_run(const LuneChunk*chunk,LuneDiagnosticFn diagnostic,void*diagnostic_context,LuneValue*result){
    VM vm={.chunk=chunk,.diagnostic=diagnostic,.diagnostic_context=diagnostic_context};
    if(chunk->names_count>0){vm.globals=calloc(chunk->names_count,sizeof(*vm.globals));vm.global_defined=calloc(chunk->names_count,sizeof(*vm.global_defined));if(vm.globals==NULL||vm.global_defined==NULL){free(vm.globals);free(vm.global_defined);if(diagnostic)diagnostic(diagnostic_context,(LuneSpan){0},"out of memory");return false;}}
    bool ok=true;while(vm.ip<chunk->count){size_t instruction=vm.ip;LuneSpan span=chunk->spans[instruction];LuneOpcode op=(LuneOpcode)chunk->code[vm.ip++];uint16_t index;LuneValue a,b;
        switch(op){
            case LUNE_OP_CONSTANT:if(!read_u16(&vm,&index,span)||index>=chunk->constants_count){ok=false;break;}ok=push(&vm,chunk->constants[index],span);break;
            case LUNE_OP_NULL:ok=push(&vm,lune_value_null(),span);break;case LUNE_OP_TRUE:ok=push(&vm,lune_value_bool(true),span);break;case LUNE_OP_FALSE:ok=push(&vm,lune_value_bool(false),span);break;case LUNE_OP_POP:ok=pop(&vm,&a,span);break;
            case LUNE_OP_GET_LOCAL:if(!read_u16(&vm,&index,span)||index>=LOCAL_MAX||!vm.local_defined[index]){ok=runtime_error(&vm,span,"unknown local binding");break;}ok=push(&vm,vm.locals[index],span);break;
            case LUNE_OP_SET_LOCAL:if(!read_u16(&vm,&index,span)||index>=LOCAL_MAX||!peek_value(&vm,&a,span)){ok=false;break;}vm.locals[index]=a;vm.local_defined[index]=true;break;
            case LUNE_OP_GET_GLOBAL:if(!read_u16(&vm,&index,span)||index>=chunk->names_count||!vm.global_defined[index]){ok=runtime_error(&vm,span,"unknown global binding");break;}ok=push(&vm,vm.globals[index],span);break;
            case LUNE_OP_DEFINE_GLOBAL:if(!read_u16(&vm,&index,span)||index>=chunk->names_count||!peek_value(&vm,&a,span)){ok=false;break;}if(vm.global_defined[index]){ok=runtime_error(&vm,span,"binding already declared in this scope");break;}vm.globals[index]=a;vm.global_defined[index]=true;break;
            case LUNE_OP_SET_GLOBAL:if(!read_u16(&vm,&index,span)||index>=chunk->names_count||!peek_value(&vm,&a,span)){ok=false;break;}if(!vm.global_defined[index]){ok=runtime_error(&vm,span,"assignment to unknown binding");break;}vm.globals[index]=a;break;
            case LUNE_OP_ADD:case LUNE_OP_SUBTRACT:case LUNE_OP_MULTIPLY:case LUNE_OP_DIVIDE:case LUNE_OP_MODULO:ok=arithmetic(&vm,op,span);break;
            case LUNE_OP_EQUAL:case LUNE_OP_NOT_EQUAL:if(!pop(&vm,&b,span)||!pop(&vm,&a,span)){ok=false;break;}ok=push(&vm,lune_value_bool(op==LUNE_OP_EQUAL?lune_value_equal(a,b):!lune_value_equal(a,b)),span);break;
            case LUNE_OP_LESS:case LUNE_OP_LESS_EQUAL:case LUNE_OP_GREATER:case LUNE_OP_GREATER_EQUAL:ok=compare(&vm,op,span);break;
            case LUNE_OP_NOT:if(!pop(&vm,&a,span)){ok=false;break;}ok=push(&vm,lune_value_bool(!lune_value_truthy(a)),span);break;
            case LUNE_OP_NEGATE:if(!pop(&vm,&a,span)){ok=false;break;}if(a.kind==LUNE_VALUE_INT){if(a.as.integer==INT64_MIN){ok=runtime_error(&vm,span,"integer overflow");break;}ok=push(&vm,lune_value_int(-a.as.integer),span);}else if(a.kind==LUNE_VALUE_FLOAT)ok=push(&vm,lune_value_float(-a.as.floating),span);else ok=runtime_error(&vm,span,"negation operand must be a number");break;
            case LUNE_OP_JUMP:if(!read_u16(&vm,&index,span)||vm.ip+index>chunk->count){ok=runtime_error(&vm,span,"invalid jump");break;}vm.ip+=index;break;
            case LUNE_OP_JUMP_IF_FALSE:if(!read_u16(&vm,&index,span)||!peek_value(&vm,&a,span)){ok=false;break;}if(!lune_value_truthy(a)){if(vm.ip+index>chunk->count){ok=runtime_error(&vm,span,"invalid conditional jump");break;}vm.ip+=index;}break;
            case LUNE_OP_LOOP:if(!read_u16(&vm,&index,span)||index>vm.ip){ok=runtime_error(&vm,span,"invalid loop jump");break;}vm.ip-=index;break;
            case LUNE_OP_RETURN:if(!pop(&vm,&a,span)){ok=false;break;}if(result)*result=a;free(vm.globals);free(vm.global_defined);return true;
            default:ok=runtime_error(&vm,span,"unknown bytecode instruction");break;
        }if(!ok)break;
    }free(vm.globals);free(vm.global_defined);return false;
}
