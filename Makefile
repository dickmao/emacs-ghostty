SHELL := /bin/bash
EMACS ?= emacs
CC    ?= cc

GHOSTTY_SRC := vendor/ghostty
GHOSTTY_OUT := $(GHOSTTY_SRC)/zig-out

CSRC  := $(shell git ls-files '*.[ch]' 2>/dev/null || echo ghostty-vt-module.c elisp.c)
ZIGSRC := $(shell find $(GHOSTTY_SRC)/src -name '*.zig' 2>/dev/null)

BEAR := $(shell command -v bear 2>/dev/null)
ifneq ($(BEAR),)
	BEAR := $(BEAR) --
endif

CFLAGS := -std=c99 -Werror -fvisibility=hidden -fPIC \
          -I$(GHOSTTY_SRC)/include
LDFLAGS := -L$(GHOSTTY_OUT)/lib -lghostty-vt \
           -Wl,-rpath,$(abspath $(GHOSTTY_OUT)/lib)

.PHONY: compile
compile: ghostty-vt-module.so

$(GHOSTTY_OUT)/lib/libghostty-vt.so: $(ZIGSRC)
	cd $(GHOSTTY_SRC) && zig build lib-vt

ghostty-vt-module.so: $(GHOSTTY_OUT)/lib/libghostty-vt.so $(CSRC)
	$(BEAR) $(CC) $(CFLAGS) -shared -o $@ $(CSRC) $(LDFLAGS)

.PHONY: run
run: ghostty-vt-module.so
	$(EMACS) -Q -L $(CURDIR) --eval "(require 'ghostty-vt)" --eval "(ghostty-vt)"

.PHONY: clean
clean:
	git clean -dfX

.PHONY: distclean
distclean: clean
	git -C $(GHOSTTY_SRC) clean -dfX
