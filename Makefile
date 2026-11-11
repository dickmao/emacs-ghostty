include epkg.mk
epkg.mk:
	emacs --batch -l package -f package-initialize -l epkg -f epkg-copy-mk

SHELL := /bin/bash
, := ,
EMACS ?= emacs
CC    ?= cc
ELSRC := $(shell git ls-files *.el)
TESTSRC := $(shell git ls-files test/*.el)

EPKG_FILES := ghostty-vt-module.so $(ELSRC)
EPKG_MAIN := ghostty-vt.el
EPKG_TEST_EL := $(TESTSRC)

CSRC  := $(shell git ls-files '*.c' 2>/dev/null)

BEAR := $(shell command -v bear 2>/dev/null)
ifneq ($(BEAR),)
	BEAR := $(BEAR) --
endif

PKG_CONFIG ?= pkg-config
GHOSTTY_LIBDIR := $(shell $(PKG_CONFIG) --variable=libdir libghostty-vt)
CFLAGS := -std=c99 -Werror -fvisibility=hidden -fPIC -g \
          $(shell $(PKG_CONFIG) --cflags libghostty-vt)
LDFLAGS := $(shell $(PKG_CONFIG) --libs libghostty-vt) \
           $(if $(GHOSTTY_LIBDIR),-Wl$(,)-rpath$(,)$(GHOSTTY_LIBDIR))

.DEFAULT_GOAL := compile

.PHONY: compile
compile: ghostty-vt-module.so epkg-compile

ghostty-vt-module.so: $(CSRC)
	$(BEAR) $(CC) $(CFLAGS) -shared -o $@ $(CSRC) $(LDFLAGS)

.PHONY: run
run: compile
	$(if $(DEBUG),DEBUGINFOD_URLS= gdb --args) $(EMACS) -Q -L $(CURDIR) -l ghostty-vt --eval "(setq debug-on-error t)" -f ghostty-vt

.PHONY: clean
clean:
	git clean -dfX

.PHONY: install
install: ghostty-vt-module.so epkg-install

.PHONY: test
test: compile epkg-test
