#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "memory.h"
#include "vm.h"

static const char* USAGE = "usage: loon [-c] [-g] [-l] [-m] [-x] [filename]";

typedef struct LogMessage LogMessage;

struct LogMessage {
  char* text;
  LogMessage* link;
};

struct LogMessage* log_ = NULL;

static void printLog(const char* fmt, va_list ap) {
  LogMessage *entry = ALLOCATE(LogMessage, 1);
  vasprintf(&entry->text, fmt, ap);
  entry->link = log_;
  log_ = entry;
}

static void showLogRecursive(LogMessage* current) {
  if (current != NULL) {
    showLogRecursive(current->link);
    printf("%s", current->text);
  }
}

static void printImmediate(const char* fmt, va_list ap) {
  vprintf(fmt, ap);
}

static void printQuiet(const char* fmt, va_list ap) {}

Config config_ = {
  .dbg_code = false,
  .dbg_exec = false,
  .dbg_gc = false,
  .dbg_memory = false,
  .filename = NULL,
  .print = printImmediate
};

void initConfig(int argc, const char* argv[]) {
  for (int i=1; i<argc; i++) {
    if (strcmp(argv[i], "-c") == 0) {
      config_.dbg_code = true;
    }
    else if (strcmp(argv[i], "-g") == 0) {
      config_.dbg_gc = true;
    }
    else if (strcmp(argv[i], "-l") == 0) {
      config_.print = printLog;
    }
    else if (strcmp(argv[i], "-m") == 0) {
      config_.dbg_memory = true;
    }
    else if (strcmp(argv[i], "-x") == 0) {
      config_.dbg_exec = true;
    }
    else if (argv[i][0] == '-') {
      fprintf(stderr, "Unrecognized flag '%s'\n", argv[i]);
      fprintf(stderr, "%s", USAGE);
      exit(64);
    }
    else if (config_.filename != NULL) {
      fprintf(stderr, "Can only provide one filename\n");
      fprintf(stderr, "%s", USAGE);
      exit(64);
    }
    else {
      config_.filename = argv[i];
    }
  }
}

static PrintFn previousPrint_;

void print(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  config_.print(fmt, args);
  va_end(args);
}

void quietPrint() {
  previousPrint_ = config_.print;
  config_.print = printQuiet;
}

void restorePrint() {
  config_.print = previousPrint_;
}

void showLog() {
  showLogRecursive(log_);
}

void clearLog() {
  while (log_ != NULL) {
    LogMessage* temp = log_->link;
    free(log_->text);
    FREE(LogMessage, log_);
    log_ = temp;
  }
}
