SHELL := /bin/bash
EMACS ?= emacs
CC    ?= cc
ELSRC := $(shell git ls-files *.el)
TESTSRC := $(shell git ls-files test/*.el)

GHOSTTY_SRC := vendor/ghostty
GHOSTTY_OUT := $(GHOSTTY_SRC)/zig-out

CSRC  := $(shell git ls-files '*.[ch]' 2>/dev/null || echo ghostty-vt-module.c elisp.c)
ZIGSRC := $(shell find $(GHOSTTY_SRC)/src -name '*.zig' 2>/dev/null)

BEAR := $(shell command -v bear 2>/dev/null)
ifneq ($(BEAR),)
	BEAR := $(BEAR) --
endif

CFLAGS := -std=c99 -Werror -fvisibility=hidden -fPIC \
          -I$(GHOSTTY_OUT)/include
LDFLAGS := -L$(GHOSTTY_OUT)/lib -lghostty-vt \
           -Wl,-rpath,$(abspath $(GHOSTTY_OUT)/lib)

.PHONY: compile
compile: ghostty-vt-module.so
	$(EMACS) -batch \
	  --eval "(setq byte-compile-error-on-warn t)" \
	  --eval "(setq package-user-dir (expand-file-name \"deps\"))" \
	  -f package-initialize \
	  -L . -L test \
	  -f batch-byte-compile $(ELSRC) $(TESTSRC); \
	  (ret=$$? ; rm -f $(ELSRC:.el=.elc) $(TESTSRC:.el=.elc) && exit $$ret)

$(GHOSTTY_SRC)/.git:
	git submodule update --init --recursive $(GHOSTTY_SRC)

$(GHOSTTY_OUT)/lib/libghostty-vt.so: $(GHOSTTY_SRC)/.git $(ZIGSRC)
	cd $(GHOSTTY_SRC) && zig build -Demit-lib-vt=true

ghostty-vt-module.so: $(GHOSTTY_OUT)/lib/libghostty-vt.so $(CSRC)
	$(BEAR) $(CC) $(CFLAGS) -shared -o $@ $(CSRC) $(LDFLAGS)

.PHONY: run
run: ghostty-vt-module.so
	$(EMACS) -Q -L $(CURDIR) -l ghostty-vt -f ghostty-vt

.PHONY: debug
debug: ghostty-vt-module.so
	gdb --args $(EMACS) -Q -L $(CURDIR) -l ghostty-vt

.PHONY: clean
clean:
	git clean -dfX

.PHONY: distclean
distclean: clean
	git -C $(GHOSTTY_SRC) clean -dfX
