CC=cc
CCFLAGS=-g

HDR=$(wildcard *.h)
SRC=$(patsubst %.h,%.c,$(filter-out common.h,${HDR}))
OBJDIR=./obj
OBJ=$(patsubst %.c,${OBJDIR}/%.o,${SRC})
LIB_OBJ=$(filter-out ${OBJDIR}/main.o,${OBJ})
LIB=libloon.a
LIBFLAGS=-L. -lloon
EXE=./loon
TESTER=tests/runtests

all: commands

## commands: show available commands
commands:
	@grep -h -E '^##' ${MAKEFILE_LIST} | sed -e 's/## //g' | column -t -s ':'

## loon: make executable
loon: ${OBJDIR}/main.o ${LIB}
	${CC} ${LIBFLAGS} -o $@ $<

## lib: make library
.PHONY: lib
lib: ${LIB}

## clean: clean up
.PHONY: clean
clean:
	@find . -name '*~' -exec rm {} \;
	@find . -name '*.o' -exec rm {} \;
	@rm -r -f ${OBJDIR}
	@rm -f ${EXE} ${LIB} ${TESTER} core.loon.c

## test: run tests
.PHONY: test
test: ${TESTER}
	@${TESTER}

## defs: variable definitions
.PHONY: defs
defs:
	@echo CCFLAGS ${CCFLAGS}
	@echo EXE ${EXE}
	@echo HDR ${HDR}
	@echo LIB ${LIB}
	@echo LIB_OBJ ${LIB_OBJ}
	@echo LIBFLAGS ${LIBFLAGS}
	@echo OBJ ${OBJ}
	@echo OBJDIR ${OBJDIR}
	@echo SRC ${SRC}
	@echo TESTER ${TESTER}

# ----------------------------------------------------------------------

# Compile C files
${OBJDIR}/%.o: %.c ${HDR}
	@mkdir -p ${OBJDIR}
	${CC} ${CCFLAGS} -c $< -o $@

# Make the library
${LIB}: ${LIB_OBJ}
	ar -r -u $@ ${LIB_OBJ}
	ranlib $@

# Translate standard library into C
core.loon.c: core.loon
	xxd -i $< > $@

# Inclusions
constants.c: constants.inc
	touch $@
constants.h: constants.inc
	touch $@

# Make the test runner
${TESTER}: tests/runtests.o ${LIB}
	${CC} ${LIBFLAGS} -o $@ $<

tests/runtests.c: tests/runtests.h
	touch $@

# Suppress automatic rule to try to build %.ltl.
# https://stackoverflow.com/questions/3674019/makefile-circular-dependency
%.loon:;
