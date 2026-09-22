grammar lune;

// Executable reference grammar for the Lune v0 contract.
// The production frontend is intended to become a hand-written Pratt parser.

program
    : separators? (statement (separators statement)*)? separators? EOF
    ;

statement
    : declaration
    | assignment
    | whileStatement
    | expression
    ;

declaration
    : IDENTIFIER DECLARE expression
    ;

assignment
    : assignTarget ASSIGN expression
    ;

assignTarget
    : IDENTIFIER ((DOT IDENTIFIER) | (LBRACK nl* expression nl* RBRACK))*
    ;

whileStatement
    : WHILE expression block
    ;

expression : logicOr ;
logicOr    : logicAnd (nl* OR nl* logicAnd)* ;
logicAnd   : equality (nl* AND nl* equality)* ;
equality   : comparison (nl* (EQ | NE) nl* comparison)* ;
comparison : term (nl* (LE | GE | LT | GT) nl* term)* ;
term       : factor (nl* (PLUS | MINUS) nl* factor)* ;
factor     : unary (nl* (STAR | SLASH | PERCENT) nl* unary)* ;

unary
    : (MINUS | NOT) nl* unary
    | postfix
    ;

postfix
    : primary postfixPart*
    ;

postfixPart
    : LPAREN nl* arguments? nl* RPAREN
    | LBRACK nl* expression nl* RBRACK
    | DOT IDENTIFIER
    ;

primary
    : NUMBER
    | STRING
    | TRUE
    | FALSE
    | NULL
    | IDENTIFIER
    | functionExpr
    | ifExpr
    | listLiteral
    | mapLiteral
    | LPAREN nl* expression nl* RPAREN
    ;

functionExpr
    : FN LPAREN nl* parameters? nl* RPAREN ARROW nl* (block | expression)
    ;

parameters
    : IDENTIFIER (nl* COMMA nl* IDENTIFIER)* nl* COMMA?
    ;

ifExpr
    : IF expression block (ELSE (ifExpr | block))?
    ;

block
    : LBRACE separators?
      (statement (separators statement)*)?
      separators? RBRACE
    ;

arguments
    : expression (nl* COMMA nl* expression)* nl* COMMA?
    ;

listLiteral
    : LBRACK nl* arguments? nl* RBRACK
    ;

mapLiteral
    : LBRACE nl* mapEntries? nl* RBRACE
    ;

mapEntries
    : mapEntry (nl* COMMA nl* mapEntry)* nl* COMMA?
    ;

mapEntry
    : (IDENTIFIER | STRING) nl* COLON nl* expression
    ;

separators : NEWLINE+ ;
nl : NEWLINE ;

FN      : 'fn';
IF      : 'if';
ELSE    : 'else';
WHILE   : 'while';

TRUE    : 'true';
FALSE   : 'false';
NULL    : 'null';

AND     : 'and';
OR      : 'or';
NOT     : 'not';

DECLARE : ':=';
ARROW   : '=>';

EQ      : '==';
NE      : '!=';
LE      : '<=';
GE      : '>=';

ASSIGN  : '=';
LT      : '<';
GT      : '>';

PLUS    : '+';
MINUS   : '-';
STAR    : '*';
SLASH   : '/';
PERCENT : '%';

LPAREN  : '(';
RPAREN  : ')';
LBRACE  : '{';
RBRACE  : '}';
LBRACK  : '[';
RBRACK  : ']';
COMMA   : ',';
DOT     : '.';
COLON   : ':';

NUMBER : [0-9]+ ('.' [0-9]+)? ;

STRING
    : '"' (ESCAPE | ~["\\\r\n])* '"'
    ;

fragment ESCAPE
    : '\\' (["\\nrt0] | 'u{' HEX+ '}')
    ;

fragment HEX : [0-9a-fA-F] ;

IDENTIFIER : [A-Za-z_] [A-Za-z0-9_]* ;

LINE_COMMENT : '//' ~[\r\n]* -> skip ;
NEWLINE : '\r'? '\n' ;
WS : [ \t\u000B\u000C]+ -> skip ;
