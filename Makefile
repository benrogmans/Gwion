ifeq (,$(wildcard util/config.mk))
$(shell cp util/config.mk.orig util/config.mk)
endif
ifeq (,$(wildcard config.mk))
$(shell cp config.mk.orig config.mk)
endif
include util/config.mk
include config.mk

GWION_PACKAGE=gwion
CFLAGS += -DGWION_PACKAGE='"${GWION_PACKAGE}"'

GIT_BRANCH=$(shell git branch | grep "*" | cut -d" " -f2)

src := $(wildcard src/*.c)
src += $(wildcard src/*/*.c)

test_dir_all := $(wildcard tests/*)
test_ignore = tests/import
test_dir := $(filter-out $(test_ignore), $(test_dir_all))
test_dir += examples

ifeq (${DEBUG_STACK}, 1)
CFLAGS += -DDEBUG_STACK
endif

ifeq (${BUILD_ON_WINDOWS}, 1)
CFLAGS += -DBUILD_ON_WINDOWS -D_XOPEN_SOURCE=700
else
LDFLAGS += -ldl -lpthread
endif

src_obj := $(src:.c=.o)
lib_obj := $(filter-out src/main.o, ${src_obj})

CFLAGS  += -Iinclude

ifeq (${BUILD_ON_WINDOWS}, 1)
ifeq (${CC}, gcc)
LDFLAGS += -lm
endif
else
LDFLAGS += -rdynamic
LDFLAGS += -lm
endif

ifeq ($(shell uname), Linux)
LDFLAGS += -lrt
endif

CFLAGS += -DGWION_BUILTIN

#GWLIBS = libgwion.a ast/libgwion_ast.a ast/libgwion_grammar.a util/libgwion_util.a
GWLIBS = libgwion.a ast/libgwion_ast.a util/libgwion_util.a
_LDFLAGS = ${GWLIBS} ${LDFLAGS}

all: options-show util/libgwion_util.a astlib libgwion.a src/main.o
	$(info link ${PRG})
	@${CC} src/main.o -o ${PRG} ${_LDFLAGS} ${LIBS}

options-show:
	@$(call _options)

libgwion.a: ${lib_obj}
	@${AR} ${AR_OPT}

util/libgwion_util.a:
	@+GWION_PACKAGE= make -s -C util

util: util/libgwion_util.a
	@(info build util)

astlib:
	@+GWION_PACKAGE= make -s -C ast

ast: ast/libgwion_ast.a
	@(info build ast)

clean:
	$(info cleaning ...)
	@rm -f */*.o */*/*.o */*.gw.* */*/*.gw.* */*/*.gcda */*/*.gcno gwion libgwion.a src/*.gcno src/*.gcda

install: ${PRG}
	$(info installing ${GWION_PACKAGE} in ${PREFIX})
	@install ${PRG} ${DESTDIR}/${PREFIX}/bin
	@sed 's/PREFIX/$\{PREFIX\}/g' scripts/gwion-config > gwion-config
	@install gwion-config ${DESTDIR}/${PREFIX}/bin/gwion-config
	@install scripts/gwion-pkg ${DESTDIR}/${PREFIX}/bin/gwion-pkg
	@rm gwion-config
	@mkdir -p ${DESTDIR}/${PREFIX}/include/gwion
	@cp include/*.h ${DESTDIR}/${PREFIX}/include/gwion

uninstall:
	$(info uninstalling ${GWION_PACKAGE} from ${PREFIX})
	@rm -rf ${DESTDIR}/${PREFIX}/bin/${PRG}
	@rm -rf ${DESTDIR}/${PREFIX}/include/gwion

test:
	@bash scripts/test.sh ${test_dir}

coverity:
	[ -z "$(git ls-remote --heads $(git remote get-url origin) coverity_scan)" ] || git push origin :coverity_scan
	git show-ref --verify --quiet refs/heads/master && git branch -D coverity_scan || echo ""
	git checkout -b coverity_scan
	git push --set-upstream origin coverity_scan
	git checkout ${GIT_BRANCH}

include $(wildcard .d/*.d)
include util/intl.mk
