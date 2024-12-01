#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "compiler.h"
#include "config.h"
#include "debug.h"
#include "memory.h"
#include "scanner.h"

typedef struct Parser Parser;

struct Parser {
  Token current;
  Token previous;
  bool hadError;
  bool panicMode;
  Scanner scanner;
  Parser* stack;
};

typedef enum {
  PREC_NONE,
  PREC_ASSIGNMENT,  // =
  PREC_OR,          // or
  PREC_AND,         // and
  PREC_EQUALITY,    // == !=
  PREC_COMPARISON,  // < > <= >=
  PREC_TERM,        // + - #
  PREC_FACTOR,      // * /
  PREC_UNARY,       // ! - #
  PREC_CALL,        // . () []
  PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(bool canAssign);

typedef struct {
  ParseFn prefix;
  ParseFn infix;
  Precedence precedence;
} ParseRule;

typedef struct {
  Token name;
  int depth;
  bool isCaptured;
} Local;

typedef struct {
  Byte index;
  bool isLocal;
} Upvalue;

typedef enum {
  TYPE_FUNCTION,
  TYPE_INITIALIZER,
  TYPE_METHOD,
  TYPE_SCRIPT
} FunctionType;

typedef struct Compiler {
  struct Compiler* enclosing;
  ObjFunction* function;
  FunctionType type;

  Local locals[BYTE_HEIGHT];
  int localCount;
  Upvalue upvalues[BYTE_HEIGHT];
  int scopeDepth;
} Compiler;

typedef struct ClassCompiler {
  struct ClassCompiler* enclosing;
  bool hasSuperclass;
} ClassCompiler;

Parser* parser_ = NULL;
Compiler* current_ = NULL;
ClassCompiler* currentClass_ = NULL;

static void expression();
static void statement();
static void declaration();
static ParseRule* getRule(TokenType type);
static void parsePrecedence(Precedence precedence);

static Chunk* currentChunk() {
  return &current_->function->chunk;
}

static void errorAt(Token* token, const char* message) {
  if (parser_->panicMode) {
    return;
  }
  parser_->panicMode = true;
  fprintf(stderr, "[line %d] Error", token->line);

  if (token->type == TOKEN_EOF) {
    fprintf(stderr, " at end");
  }
  else if (token->type == TOKEN_ERROR) {
    // Nothing.
  }
  else {
    fprintf(stderr, " at '%.*s'", token->length, token->start);
  }

  fprintf(stderr, ": %s\n", message);
  parser_->hadError = true;
}

static void error(const char* message) {
  errorAt(&parser_->previous, message);
}

static void errorAtCurrent(const char* message) {
  errorAt(&parser_->current, message);
}

static void advance() {
  parser_->previous = parser_->current;
  for (;;) {
    parser_->current = scanToken(&parser_->scanner);
    if (parser_->current.type != TOKEN_ERROR) {
      break;
    }
    errorAtCurrent(parser_->current.start);
  }
}

static void consume(TokenType type, const char* message) {
  if (parser_->current.type == type) {
    advance();
    return;
  }
  errorAtCurrent(message);
}

static bool check(TokenType type) {
  return parser_->current.type == type;
}

static bool match(TokenType type) {
  if (!check(type)) {
    return false;
  }
  advance();
  return true;
}

static void emitByte(Byte byte) {
  writeChunk(currentChunk(), byte, parser_->previous.line);
}

static void emitBytes(Byte byte1, Byte byte2) {
  emitByte(byte1);
  emitByte(byte2);
}

static void emitLoop(int loopStart) {
  emitByte(OP_LOOP);

  int offset = currentChunk()->count - loopStart + 2;
  if (offset > UINT16_MAX) error("Loop body too large.");

  emitByte((offset >> BYTE_WIDTH) & BYTE_MASK);
  emitByte(offset & BYTE_MASK);
}

static int emitJump(Byte instruction) {
  emitByte(instruction);
  emitByte(BYTE_MASK);
  emitByte(BYTE_MASK);
  return currentChunk()->count - 2;
}

static void emitReturn() {
  if (current_->type == TYPE_INITIALIZER) {
    emitBytes(OP_LOCAL_GET, 0);
  }
  else {
    emitByte(OP_NIL);
  }
  emitByte(OP_RETURN);
}

static Byte makeConstant(Value value) {
  int constant = addConstant(currentChunk(), value);
  if (constant > BYTE_MAX) {
    error("Too many constants in one chunk.");
    return 0;
  }
  return (Byte)constant;
}

static void emitConstant(Value value) {
  emitBytes(OP_CONSTANT, makeConstant(value));
}

static void patchJump(int offset) {
  // -2 to adjust for the bytecode for the jump offset itself.
  int jump = currentChunk()->count - offset - 2;

  if (jump > UINT16_MAX) {
    error("Too much code to jump over.");
  }

  currentChunk()->code[offset] = (jump >> BYTE_WIDTH) & BYTE_MASK;
  currentChunk()->code[offset + 1] = jump & BYTE_MASK;
}

static void initCompiler(Compiler* compiler, FunctionType type) {
  compiler->enclosing = current_;
  compiler->function = NULL;
  compiler->type = type;
  compiler->localCount = 0;
  compiler->scopeDepth = 0;
  compiler->function = newFunction();
  current_ = compiler;
  if (type != TYPE_SCRIPT) {
    current_->function->name = copyString(parser_->previous.start, parser_->previous.length);
  }

  Local* local = &current_->locals[current_->localCount++];
  local->depth = 0;
  local->isCaptured = false;
  if (type != TYPE_FUNCTION) {
    local->name.start = "this";
    local->name.length = 4;
  }
  else {
    local->name.start = "";
    local->name.length = 0;
  }
}

static ObjFunction* endCompiler() {
  emitReturn();
  ObjFunction* function = current_->function;

  if (config_.dbg_code) {
    if (!parser_->hadError) {
      disassembleChunk(currentChunk(), function->name != NULL
		       ? function->name->chars : "<script>");
    }
  }

  current_ = current_->enclosing;
  return function;
}

static void beginScope() {
  current_->scopeDepth++;
}

static void endScope() {
  current_->scopeDepth--;

  while ((current_->localCount > 0) &&
         (current_->locals[current_->localCount - 1].depth > current_->scopeDepth)) {
    if (current_->locals[current_->localCount - 1].isCaptured) {
      emitByte(OP_UPVALUE_CLOSE);
    }
    else {
      emitByte(OP_POP);
    }
    current_->localCount--;
  }
}

static Byte identifierConstant(Token* name) {
  return makeConstant(OBJ_VAL(copyString(name->start, name->length)));
}

static bool identifiersEqual(Token* a, Token* b) {
  if (a->length != b->length) {
    return false;
  }
  return memcmp(a->start, b->start, a->length) == 0;
}

static int resolveLocal(Compiler* compiler, Token* name) {
  for (int i = compiler->localCount - 1; i >= 0; i--) {
    Local* local = &compiler->locals[i];
    if (identifiersEqual(name, &local->name)) {
      if (local->depth == -1) {
        error("Can't read local variable in its own initializer.");
      }
      return i;
    }
  }

  return -1;
}

static int addUpvalue(Compiler* compiler, Byte index, bool isLocal) {
  int upvalueCount = compiler->function->upvalueCount;

  for (int i = 0; i < upvalueCount; i++) {
    Upvalue* upvalue = &compiler->upvalues[i];
    if (upvalue->index == index && upvalue->isLocal == isLocal) {
      return i;
    }
  }

  if (upvalueCount == BYTE_HEIGHT) {
    error("Too many closure variables in function.");
    return 0;
  }

  compiler->upvalues[upvalueCount].isLocal = isLocal;
  compiler->upvalues[upvalueCount].index = index;
  return compiler->function->upvalueCount++;
}

static int resolveUpvalue(Compiler* compiler, Token* name) {
  if (compiler->enclosing == NULL) {
    return -1;
  }

  int local = resolveLocal(compiler->enclosing, name);
  if (local != -1) {
    compiler->enclosing->locals[local].isCaptured = true;
    return addUpvalue(compiler, (Byte)local, true);
  }

  int upvalue = resolveUpvalue(compiler->enclosing, name);
  if (upvalue != -1) {
    return addUpvalue(compiler, (Byte)upvalue, false);
  }

  return -1;
}

static void addLocal(Token name) {
  if (current_->localCount == BYTE_HEIGHT) {
    error("Too many local variables in function.");
    return;
  }

  Local* local = &current_->locals[current_->localCount++];
  local->name = name;
  local->depth = -1;
  local->isCaptured = false;
}

static void declareVariable() {
  if (current_->scopeDepth == 0) {
    return;
  }

  Token* name = &parser_->previous;
  for (int i = current_->localCount - 1; i >= 0; i--) {
    Local* local = &current_->locals[i];
    if ((local->depth != -1) && (local->depth < current_->scopeDepth)) {
      break;
    }

    if (identifiersEqual(name, &local->name)) {
      error("Already a variable with this name in this scope.");
    }
  }

  addLocal(*name);
}

static Byte parseVariable(const char* errorMessage) {
  consume(TOKEN_IDENTIFIER, errorMessage);
  declareVariable();
  if (current_->scopeDepth > 0) {
    return 0;
  }
  return identifierConstant(&parser_->previous);
}

static void markInitialized() {
  if (current_->scopeDepth == 0) {
    return;
  }
  current_->locals[current_->localCount - 1].depth = current_->scopeDepth;
}

static void defineVariable(Byte global) {
  if (current_->scopeDepth > 0) {
    markInitialized();
    return;
  }
  emitBytes(OP_GLOBAL_DEFINE, global);
}

static Byte expressionList(TokenType end, const char* missingEnd, bool pair) {
  Byte argCount = 0;
  if (!check(end)) {
    do {
      expression();
      if (argCount == BYTE_MAX) {
        error("Expression list can't have more than 255 items.");
      }
      if (pair) {
	consume(TOKEN_COLON, "Expect ':' to join entries.");
	expression();
      }
      argCount++;
    } while (match(TOKEN_COMMA));
  }
  consume(end, missingEnd);
  return argCount;
}

static Byte argumentList() {
  return expressionList(TOKEN_RIGHT_PAREN, "Expect ')' to end argument list.", false);
}

static void and_(bool canAssign) {
  int endJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP);
  parsePrecedence(PREC_AND);
  patchJump(endJump);
}

static void namedVariable(Token name, bool canAssign) {
  Byte getOp, setOp;
  int arg = resolveLocal(current_, &name);
  if (arg != -1) {
    getOp = OP_LOCAL_GET;
    setOp = OP_LOCAL_SET;
  }
  else if ((arg = resolveUpvalue(current_, &name)) != -1) {
    getOp = OP_UPVALUE_GET;
    setOp = OP_UPVALUE_SET;
  }
  else {
    arg = identifierConstant(&name);
    getOp = OP_GLOBAL_GET;
    setOp = OP_GLOBAL_SET;
  }
  if (canAssign && match(TOKEN_EQUAL)) {
    expression();
    emitBytes(setOp, (Byte)arg);
  }
  else {
    emitBytes(getOp, (Byte)arg);
  }
}

static Token syntheticToken(const char* text) {
  Token token;
  token.start = text;
  token.length = (int)strlen(text);
  return token;
}

static void binaryConcat() {
  namedVariable(syntheticToken("concat"), false);
  emitBytes(OP_CALL_POSTFIX, 2);
}

static void binary(bool canAssign) {
  TokenType operatorType = parser_->previous.type;
  ParseRule* rule = getRule(operatorType);
  parsePrecedence((Precedence)(rule->precedence + 1));

  switch (operatorType) {
    case TOKEN_BANG_EQUAL:    emitBytes(OP_EQUAL, OP_NOT); break;
    case TOKEN_EQUAL_EQUAL:   emitByte(OP_EQUAL); break;
    case TOKEN_GREATER:       emitByte(OP_GREATER); break;
    case TOKEN_GREATER_EQUAL: emitBytes(OP_LESS, OP_NOT); break;
    case TOKEN_HASH:          binaryConcat(); break;
    case TOKEN_LESS:          emitByte(OP_LESS); break;
    case TOKEN_LESS_EQUAL:    emitBytes(OP_GREATER, OP_NOT); break;
    case TOKEN_MINUS:         emitByte(OP_SUBTRACT); break;
    case TOKEN_PLUS:          emitByte(OP_ADD); break;
    case TOKEN_SLASH:         emitByte(OP_DIVIDE); break;
    case TOKEN_STAR:          emitByte(OP_MULTIPLY); break;
    default: return; // Unreachable.
  }
}

static void call(bool canAssign) {
  Byte argCount = argumentList();
  emitBytes(OP_CALL, argCount);
}

static void index_(bool canAssign) {
  expression();
  consume(TOKEN_RIGHT_SQUARE, "Expect ']' after index.");

  if (canAssign && match(TOKEN_EQUAL)) {
    expression();
    Token setAt = syntheticToken("setAt");
    Byte name = identifierConstant(&setAt);
    emitBytes(OP_INVOKE, name);
    emitByte(2);
  }
  else {
    Token getAt = syntheticToken("getAt");
    Byte name = identifierConstant(&getAt);
    emitBytes(OP_INVOKE, name);
    emitByte(1);
  }
}

static void dot(bool canAssign) {
  consume(TOKEN_IDENTIFIER, "Expect property name after '.'.");
  Byte name = identifierConstant(&parser_->previous);

  if (canAssign && match(TOKEN_EQUAL)) {
    expression();
    emitBytes(OP_PROPERTY_SET, name);
  }
  else if (match(TOKEN_LEFT_PAREN)) {
    Byte argCount = argumentList();
    emitBytes(OP_INVOKE, name);
    emitByte(argCount);
  }
  else {
    emitBytes(OP_PROPERTY_GET, name);
  }
}

static void literal(bool canAssign) {
  switch (parser_->previous.type) {
    case TOKEN_FALSE: emitByte(OP_FALSE); break;
    case TOKEN_NIL: emitByte(OP_NIL); break;
    case TOKEN_TRUE: emitByte(OP_TRUE); break;
    default: return; // Unreachable.
  }
}

static void grouping(bool canAssign) {
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void number(bool canAssign) {
  double value = strtod(parser_->previous.start, NULL);
  emitConstant(NUMBER_VAL(value));
}

static void or_(bool canAssign) {
  int elseJump = emitJump(OP_JUMP_IF_FALSE);
  int endJump = emitJump(OP_JUMP);

  patchJump(elseJump);
  emitByte(OP_POP);

  parsePrecedence(PREC_OR);
  patchJump(endJump);
}

static void string(bool canAssign) {
  emitConstant(OBJ_VAL(copyString(parser_->previous.start + 1, parser_->previous.length - 2)));
}

static void variable(bool canAssign) {
  namedVariable(parser_->previous, canAssign);
}

static void super_(bool canAssign) {
  if (currentClass_ == NULL) {
    error("Can't use 'super' outside of a class.");
  }
  else if (!currentClass_->hasSuperclass) {
    error("Can't use 'super' in a class with no superclass.");
  }

  consume(TOKEN_DOT, "Expect '.' after 'super'.");
  consume(TOKEN_IDENTIFIER, "Expect superclass method name.");
  Byte name = identifierConstant(&parser_->previous);

  namedVariable(syntheticToken("this"), false);
  if (match(TOKEN_LEFT_PAREN)) {
    Byte argCount = argumentList();
    namedVariable(syntheticToken("super"), false);
    emitBytes(OP_INVOKE_SUPER, name);
    emitByte(argCount);
  }
  else {
    namedVariable(syntheticToken("super"), false);
    emitBytes(UP_SUPER_GET, name);
  }
}

static void this_(bool canAssign) {
  if (currentClass_ == NULL) {
    error("Can't use 'this' outside of a class.");
    return;
  }
  variable(false);
}

static void unaryAsStr(bool canAssign) {
  namedVariable(syntheticToken("str"), canAssign);
  parsePrecedence(PREC_UNARY);
  emitBytes(OP_CALL, 1);
}

static void literalList() {
  Byte argCount = expressionList(TOKEN_RIGHT_SQUARE, "Expect ']' to end list.", false);
  emitBytes(OP_COLLECTION_LIST, argCount);
}

static void literalTable() {
  Byte argCount = expressionList(TOKEN_RIGHT_CURLY, "Expect '}' to end table.", true);
  emitBytes(OP_COLLECTION_TABLE, argCount);
}

static void unary(bool canAssign) {
  TokenType operatorType = parser_->previous.type;
  switch (operatorType) {
    case TOKEN_HASH: {
      unaryAsStr(canAssign);
      break;
    }
    case TOKEN_LEFT_CURLY: {
      literalTable();
      break;
    }
    case TOKEN_LEFT_SQUARE: {
      literalList();
      break;
    }
    case TOKEN_MINUS: {
      parsePrecedence(PREC_UNARY);
      emitByte(OP_NEGATE);
      break;
    }
    case TOKEN_NOT: {
      parsePrecedence(PREC_UNARY);
      emitByte(OP_NOT);
      break;
    }
    default: return; // Unreachable.
  }
}

ParseRule rules[] = {
  [TOKEN_AND]           = {NULL,     and_,   PREC_AND},
  [TOKEN_BANG_EQUAL]    = {NULL,     binary, PREC_EQUALITY},
  [TOKEN_CLASS]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_COMMA]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_DOT]           = {NULL,     dot,    PREC_CALL},
  [TOKEN_ELSE]          = {NULL,     NULL,   PREC_NONE},
  [TOKEN_EOF]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_EQUAL]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_EQUAL_EQUAL]   = {NULL,     binary, PREC_EQUALITY},
  [TOKEN_ERROR]         = {NULL,     NULL,   PREC_NONE},
  [TOKEN_FALSE]         = {literal,  NULL,   PREC_NONE},
  [TOKEN_FOR]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_FUN]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_GREATER]       = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_GREATER_EQUAL] = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_HASH]          = {unary,    binary, PREC_FACTOR},
  [TOKEN_IDENTIFIER]    = {variable, NULL,   PREC_NONE},
  [TOKEN_IF]            = {NULL,     NULL,   PREC_NONE},
  [TOKEN_LEFT_CURLY]    = {unary,    NULL,   PREC_CALL},
  [TOKEN_LEFT_PAREN]    = {grouping, call,   PREC_CALL},
  [TOKEN_LEFT_SQUARE]   = {unary,    index_, PREC_CALL},
  [TOKEN_LESS]          = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_LESS_EQUAL]    = {NULL,     binary, PREC_COMPARISON},
  [TOKEN_MINUS]         = {unary,    binary, PREC_TERM},
  [TOKEN_NIL]           = {literal,  NULL,   PREC_NONE},
  [TOKEN_NOT]           = {unary,    NULL,   PREC_NONE},
  [TOKEN_NUMBER]        = {number,   NULL,   PREC_NONE},
  [TOKEN_OR]            = {NULL,     or_,    PREC_OR},
  [TOKEN_PLUS]          = {NULL,     binary, PREC_TERM},
  [TOKEN_RETURN]        = {NULL,     NULL,   PREC_NONE},
  [TOKEN_RIGHT_CURLY]   = {NULL,     NULL,   PREC_NONE},
  [TOKEN_RIGHT_PAREN]   = {NULL,     NULL,   PREC_NONE},
  [TOKEN_RIGHT_SQUARE]  = {NULL,     NULL,   PREC_NONE},
  [TOKEN_SEMICOLON]     = {NULL,     NULL,   PREC_NONE},
  [TOKEN_SLASH]         = {NULL,     binary, PREC_FACTOR},
  [TOKEN_STAR]          = {NULL,     binary, PREC_FACTOR},
  [TOKEN_STRING]        = {string,   NULL,   PREC_NONE},
  [TOKEN_SUPER]         = {super_,   NULL,   PREC_NONE},
  [TOKEN_THIS]          = {this_,    NULL,   PREC_NONE},
  [TOKEN_TRUE]          = {literal,  NULL,   PREC_NONE},
  [TOKEN_VAR]           = {NULL,     NULL,   PREC_NONE},
  [TOKEN_WHILE]         = {NULL,     NULL,   PREC_NONE}
};

static void parsePrecedence(Precedence precedence) {
  advance();
  ParseFn prefixRule = getRule(parser_->previous.type)->prefix;
  if (prefixRule == NULL) {
    error("Expect expression.");
    return;
  }

  bool canAssign = precedence <= PREC_ASSIGNMENT;
  prefixRule(canAssign);

  while (precedence <= getRule(parser_->current.type)->precedence) {
    advance();
    ParseFn infixRule = getRule(parser_->previous.type)->infix;
    infixRule(canAssign);
  }

  if (canAssign && match(TOKEN_EQUAL)) {
    error("Invalid assignment target.");
  }
}

static ParseRule* getRule(TokenType type) {
  return &rules[type];
}

static void expression() {
  parsePrecedence(PREC_ASSIGNMENT);
}

static void block() {
  while (!check(TOKEN_RIGHT_CURLY) && !check(TOKEN_EOF)) {
    declaration();
  }

  consume(TOKEN_RIGHT_CURLY, "Expect '}' after block.");
}

static void function(FunctionType type) {
  Compiler compiler;
  initCompiler(&compiler, type);
  beginScope();

  consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");
  if (!check(TOKEN_RIGHT_PAREN)) {
    do {
      current_->function->arity++;
      if (current_->function->arity > BYTE_MAX) {
        errorAtCurrent("Can't have more than 255 parameters.");
      }
      Byte constant = parseVariable("Expect parameter name.");
      defineVariable(constant);
    } while (match(TOKEN_COMMA));
  }
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
  consume(TOKEN_LEFT_CURLY, "Expect '{' before function body.");
  block();

  ObjFunction* function = endCompiler();
  emitBytes(OP_CLOSURE, makeConstant(OBJ_VAL(function)));

  for (int i = 0; i < function->upvalueCount; i++) {
    emitByte(compiler.upvalues[i].isLocal ? 1 : 0);
    emitByte(compiler.upvalues[i].index);
  }
}

static void method() {
  consume(TOKEN_IDENTIFIER, "Expect method name.");
  Byte constant = identifierConstant(&parser_->previous);

  FunctionType type = TYPE_METHOD;
  if ((parser_->previous.length == 4) && (memcmp(parser_->previous.start, "init", 4) == 0)) {
    type = TYPE_INITIALIZER;
  }

  function(type);
  emitBytes(OP_METHOD, constant);
}

static void classDeclaration() {
  consume(TOKEN_IDENTIFIER, "Expect class name.");
  Token className = parser_->previous;
  Byte nameConstant = identifierConstant(&parser_->previous);
  declareVariable();

  emitBytes(OP_CLASS, nameConstant);
  defineVariable(nameConstant);

  ClassCompiler classCompiler;
  classCompiler.hasSuperclass = false;
  classCompiler.enclosing = currentClass_;
  currentClass_ = &classCompiler;

  if (match(TOKEN_LESS)) {
    consume(TOKEN_IDENTIFIER, "Expect superclass name.");
    variable(false);

    if (identifiersEqual(&className, &parser_->previous)) {
      error("A class can't inherit from itself.");
    }

    beginScope();
    addLocal(syntheticToken("super"));
    defineVariable(0);

    namedVariable(className, false);
    emitByte(OP_INHERIT);
    classCompiler.hasSuperclass = true;
  }

  namedVariable(className, false);
  consume(TOKEN_LEFT_CURLY, "Expect '{' before class body.");
  while (!check(TOKEN_RIGHT_CURLY) && !check(TOKEN_EOF)) {
    method();
  }
  consume(TOKEN_RIGHT_CURLY, "Expect '}' after class body.");
  emitByte(OP_POP);

  if (classCompiler.hasSuperclass) {
    endScope();
  }

  currentClass_ = currentClass_->enclosing;
}

static void funDeclaration() {
  Byte global = parseVariable("Expect function name.");
  markInitialized();
  function(TYPE_FUNCTION);
  defineVariable(global);
}

static void varDeclaration() {
  Byte global = parseVariable("Expect variable name.");

  if (match(TOKEN_EQUAL)) {
    expression();
  }
  else {
    emitByte(OP_NIL);
  }
  consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");

  defineVariable(global);
}

static void expressionStatement() {
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
  emitByte(OP_POP);
}

static void forStatement() {
  beginScope();
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");
  if (match(TOKEN_SEMICOLON)) {
    // No initializer.
  }
  else if (match(TOKEN_VAR)) {
    varDeclaration();
  }
  else {
    expressionStatement();
  }

  int loopStart = currentChunk()->count;
  int exitJump = -1;
  if (!match(TOKEN_SEMICOLON)) {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");

    // Jump out of the loop if the condition is false.
    exitJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP); // Condition.
  }

  if (!match(TOKEN_RIGHT_PAREN)) {
    int bodyJump = emitJump(OP_JUMP);
    int incrementStart = currentChunk()->count;
    expression();
    emitByte(OP_POP);
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

    emitLoop(loopStart);
    loopStart = incrementStart;
    patchJump(bodyJump);
  }

  statement();
  emitLoop(loopStart);

  if (exitJump != -1) {
    patchJump(exitJump);
    emitByte(OP_POP); // Condition.
  }

  endScope();
}

static void ifStatement() {
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

  int thenJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP);
  statement();

  int elseJump = emitJump(OP_JUMP);

  patchJump(thenJump);
  emitByte(OP_POP);

  if (match(TOKEN_ELSE)) statement();
  patchJump(elseJump);
}

static void returnStatement() {
  if (current_->type == TYPE_SCRIPT) {
    error("Can't return from top-level code.");
  }

  if (match(TOKEN_SEMICOLON)) {
    emitReturn();
  }
  else {
    if (current_->type == TYPE_INITIALIZER) {
      error("Can't return a value from an initializer.");
    }

    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after return value.");
    emitByte(OP_RETURN);
  }
}

static void whileStatement() {
  int loopStart = currentChunk()->count;
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

  int exitJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP);
  statement();
  emitLoop(loopStart);

  patchJump(exitJump);
  emitByte(OP_POP);
}

static void synchronize() {
  parser_->panicMode = false;

  while (parser_->current.type != TOKEN_EOF) {
    if (parser_->previous.type == TOKEN_SEMICOLON) {
      return;
    }
    switch (parser_->current.type) {
      case TOKEN_CLASS:
      case TOKEN_FOR:
      case TOKEN_FUN:
      case TOKEN_IF:
      case TOKEN_RETURN:
      case TOKEN_VAR:
      case TOKEN_WHILE:
        return;

      default:
        ; // Do nothing.
    }

    advance();
  }
}

static void declaration() {
  if (match(TOKEN_CLASS)) {
    classDeclaration();
  }
  else if (match(TOKEN_FUN)) {
    funDeclaration();
  }
  else if (match(TOKEN_VAR)) {
    varDeclaration();
  }
  else {
    statement();
  }

  if (parser_->panicMode) {
    synchronize();
  }
}

static void statement() {
  if (match(TOKEN_FOR)) {
    forStatement();
  }
  else if (match(TOKEN_IF)) {
    ifStatement();
  }
  else if (match(TOKEN_RETURN)) {
    returnStatement();
  }
  else if (match(TOKEN_WHILE)) {
    whileStatement();
  }
  else if (match(TOKEN_LEFT_CURLY)) {
    beginScope();
    block();
    endScope();
  }
  else {
    expressionStatement();
  }
}

static void initParser(Parser* parser, const char* source) {
  initScanner(&parser->scanner, source);
  parser->hadError = false;
  parser->panicMode = false;
  parser->stack = NULL;
}

ObjFunction* compile(const char* source) {
  Parser p;
  initParser(&p, source);
  parser_ = &p;
  Compiler compiler;
  initCompiler(&compiler, TYPE_SCRIPT);

  advance();

  while (!match(TOKEN_EOF)) {
    declaration();
  }

  ObjFunction* function = endCompiler();

  return parser_->hadError ? NULL : function;
}

void markCompilerRoots() {
  Compiler* compiler = current_;
  while (compiler != NULL) {
    markObject((Obj*)compiler->function);
    compiler = compiler->enclosing;
  }
}
