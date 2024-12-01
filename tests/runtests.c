#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "../config.h"
#include "../vm.h"

#include "runtests.h"

typedef struct TestMessage TestMessage;

struct TestMessage {
  char* msg;
  TestMessage* prev;
};

typedef struct {
  int pass;
  int fail;
  TestMessage* msg;
} Outcomes;

static TestMessage* messages_ = NULL;
static Outcomes outcomes_;

void check(bool condition, const char* fmt, ...) {
  if (condition) {
    outcomes_.pass++;
    return;
  }
  outcomes_.fail++;
  TestMessage* msg = malloc(sizeof(TestMessage));
  msg->prev = messages_;
  messages_ = msg;
  va_list ap;
  va_start(ap, fmt);
  vasprintf(&msg->msg, fmt, ap);
  va_end(ap);
}

static void test_alwaysSucceed() {
  check(1 > 0, "This should have worked.");
}

static void test_alwaysFail() {
  check(1 < 0, "This failed as it should.");
}

static TestFn tests[] = {
  test_alwaysSucceed,
  test_alwaysFail,
  NULL
};

int main(int argc, const char* argv[]) {
  initConfig(argc, argv);
  for (int i=0; tests[i] != NULL; i++) {
    initVM();
    tests[i]();
    freeVM();
  }

  printf("pass %d\n", outcomes_.pass);
  printf("fail %d\n", outcomes_.fail);
  while (messages_ != NULL) {
    printf("%s", messages_->msg);
    TestMessage* temp = messages_->prev;
    free(messages_->msg);
    free(messages_);
    messages_ = temp;
  }

  return 0;
}
