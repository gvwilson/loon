#ifndef config_h
#define config_h

#include <stdarg.h>
#include <stdbool.h>

typedef void (*PrintFn)(const char* fmt, va_list ap);

typedef struct {
  bool dbg_code;
  bool dbg_exec;
  bool dbg_gc;
  bool dbg_memory;
  const char* filename;
  PrintFn print;
} Config;

extern Config config_;

void initConfig(int argc, const char* argv[]);
void quietPrint();
void restorePrint();
void showLog();
void clearLog();

#endif
