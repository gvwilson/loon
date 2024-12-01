#ifndef debug_h
#define debug_h

#include "chunk.h"
#include "object.h"

void disassembleChunk(Chunk* chunk, const char* name);
int disassembleInstruction(Chunk* chunk, int offset);
void printAllObjects();
void traceExecution(ObjFiber* fiber, CallFrame* frame);

#endif
