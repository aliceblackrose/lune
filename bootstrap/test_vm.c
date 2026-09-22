#include "compiler.h"
#include "parser.h"
#include "vm.h"

#include <stdio.h>
#include <string.h>

typedef struct { int count; } Diagnostics;
static int failures = 0;

static void diagnostic(void *context, LuneSpan span, const char *message) {
    (void)span; (void)message;
    ((Diagnostics *)context)->count++;
}
static void fail(const char *name,const char *message){fprintf(stderr,"FAIL %s: %s\n",name,message);failures++;}

static bool eval(const char*source,LuneValue*result,int*diagnostic_count){
    Diagnostics d={0};LuneParser parser;lune_parser_init(&parser,source,strlen(source),diagnostic,&d);LuneAst*ast=lune_parse_program(&parser);
    if(ast==NULL||parser.had_error){lune_ast_free(ast);*diagnostic_count=d.count;return false;}
    LuneChunk chunk;lune_chunk_init(&chunk);bool ok=lune_compile(ast,source,&chunk,diagnostic,&d);if(ok)ok=lune_vm_run(&chunk,diagnostic,&d,result);
    lune_chunk_free(&chunk);lune_ast_free(ast);*diagnostic_count=d.count;return ok;
}
static void expect_int(const char*name,const char*source,int64_t expected){LuneValue result;int diagnostics=0;if(!eval(source,&result,&diagnostics)){fail(name,"evaluation failed");return;}if(result.kind!=LUNE_VALUE_INT||result.as.integer!=expected)fail(name,"unexpected result");}

int main(void){
    expect_int("arithmetic","x := 2 + 3 * 4\nx = x + 1\nx\n",15);
    expect_int("if","x := 3\nif x > 2 {\n  x * 10\n} else {\n  0\n}\n",30);
    expect_int("while","x := 0\nwhile x < 5 {\n  x = x + 1\n}\nx\n",5);
    expect_int("scope","x := 1\nif true {\n  x := 5\n  x\n}\nx\n",1);

    LuneValue result;int diagnostics=0;
    if(!eval("false and missing\n",&result,&diagnostics)||result.kind!=LUNE_VALUE_BOOL||result.as.boolean)fail("and-short-circuit","rhs did not short-circuit");
    diagnostics=0;if(!eval("true or missing\n",&result,&diagnostics)||result.kind!=LUNE_VALUE_BOOL||!result.as.boolean)fail("or-short-circuit","rhs did not short-circuit");
    diagnostics=0;if(eval("missing = 1\n",&result,&diagnostics)||diagnostics==0)fail("unknown-assignment","unknown assignment did not fail");
    diagnostics=0;if(eval("x := 1\nx := 2\n",&result,&diagnostics)||diagnostics==0)fail("redeclaration","same-scope redeclaration did not fail");

    if(failures){fprintf(stderr,"%d VM test(s) failed\n",failures);return 1;}puts("VM tests passed");return 0;
}
