#ifndef runtests_h
#define runtests_h

void check(bool condition, const char* fmt, ...);

typedef void (*TestFn)();

#endif
