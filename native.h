#ifndef native_h
#define native_h

#include "object.h"
#include "value.h"

void freeCoreList(ObjList* list);
void markCoreList(ObjList* list);
void printCoreList(ObjList* list);
void nativeCoreList();

void freeCoreTable(ObjTable* table);
void markCoreTable(ObjTable* table);
void printCoreTable(ObjTable* table);
void nativeCoreTable();

void initNative();

#endif
