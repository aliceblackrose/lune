#include "ast.h"
#include "bytecode.h"
#include "compiler.h"
#include "lexer.h"
#include "parser.h"
#include "value.h"
#include "vm.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path,size_t *length){FILE*file=fopen(path,"rb");if(!file)return NULL;if(fseek(file,0,SEEK_END)!=0){fclose(file);return NULL;}long size=ftell(file);if(size<0||fseek(file,0,SEEK_SET)!=0){fclose(file);return NULL;}char*buffer=malloc((size_t)size+1);if(!buffer){fclose(file);return NULL;}size_t read=fread(buffer,1,(size_t)size,file);fclose(file);if(read!=(size_t)size){free(buffer);return NULL;}buffer[read]='\0';*length=read;return buffer;}
static void print_diagnostic(void*context,LuneSpan span,const char*message){fprintf(stderr,"%s:%zu:%zu: error: %s\n",(const char*)context,span.line,span.column,message);}
static int lex_file(const char*path,const char*source,size_t length){LuneLexer lexer;lune_lexer_init(&lexer,source,length,print_diagnostic,(void*)path);for(;;){LuneToken token=lune_lexer_next(&lexer);printf("%zu:%zu %-12s",token.span.line,token.span.column,lune_token_kind_name(token.kind));if(token.span.length>0&&token.kind!=LUNE_TOKEN_NEWLINE)printf(" %.*s",(int)token.span.length,source+token.span.offset);putchar('\n');if(token.kind==LUNE_TOKEN_EOF)break;}return lexer.had_error?1:0;}
static LuneAst*parse_source(const char*path,const char*source,size_t length,bool*ok){LuneParser parser;lune_parser_init(&parser,source,length,print_diagnostic,(void*)path);LuneAst*ast=lune_parse_program(&parser);*ok=ast!=NULL&&!parser.had_error;return ast;}
static int parse_file(const char*path,const char*source,size_t length,bool dump){bool ok=false;LuneAst*ast=parse_source(path,source,length,&ok);if(ok&&dump)lune_ast_dump(stdout,ast,source);lune_ast_free(ast);return ok?0:1;}
static int execute_file(const char*path,const char*source,size_t length,bool print_result){bool ok=false;LuneAst*ast=parse_source(path,source,length,&ok);if(!ok){lune_ast_free(ast);return 1;}LuneChunk chunk;lune_chunk_init(&chunk);ok=lune_compile(ast,source,&chunk,print_diagnostic,(void*)path);LuneValue result=lune_value_null();if(ok)ok=lune_vm_run(&chunk,print_diagnostic,(void*)path,&result);if(ok&&print_result){lune_value_print(stdout,result);putchar('\n');}lune_chunk_free(&chunk);lune_ast_free(ast);return ok?0:1;}
static void usage(const char*program){fprintf(stderr,"usage: %s <lex|check|parse|run|eval> FILE\n",program);}
int main(int argc,char**argv){if(argc!=3){usage(argv[0]);return 2;}size_t length=0;char*source=read_file(argv[2],&length);if(!source){fprintf(stderr,"lune-bootstrap: unable to read %s\n",argv[2]);return 1;}int result;if(!strcmp(argv[1],"lex"))result=lex_file(argv[2],source,length);else if(!strcmp(argv[1],"check"))result=parse_file(argv[2],source,length,false);else if(!strcmp(argv[1],"parse"))result=parse_file(argv[2],source,length,true);else if(!strcmp(argv[1],"run"))result=execute_file(argv[2],source,length,false);else if(!strcmp(argv[1],"eval"))result=execute_file(argv[2],source,length,true);else{usage(argv[0]);result=2;}free(source);return result;}
