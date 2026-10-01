include epkg.mk
epkg.mk:
	emacs --batch -l package -f package-initialize -l epkg -f epkg-copy-mk

SHELL := /bin/bash
EMACS ?= emacs
CC    ?= cc
ELSRC := $(shell git ls-files *.el)
TESTSRC := $(shell git ls-files test/*.el)

GHOSTTY_SRC := vendor/ghostty
GHOSTTY_OUT := $(GHOSTTY_SRC)/zig-out

EPKG_FILES := ghostty-vt-module.so $(ELSRC)
EPKG_EL := $(ELSRC) $(TESTSRC)
EPKG_MAIN := ghostty-vt.el
EPKG_TEST_EL := $(TESTSRC)

CSRC  := $(shell git ls-files '*.c' 2>/dev/null)
ZIGSRC := $(shell find $(GHOSTTY_SRC)/src -name '*.zig' 2>/dev/null)

BEAR := $(shell command -v bear 2>/dev/null)
ifneq ($(BEAR),)
	BEAR := $(BEAR) --
endif

CFLAGS := -std=c99 -Werror -fvisibility=hidden -fPIC -g \
          -I$(GHOSTTY_OUT)/include
LDFLAGS := $(GHOSTTY_OUT)/lib/libghostty-vt.a

.PHONY: compile
compile: ghostty-vt-module.so epkg-compile

$(GHOSTTY_SRC)/.git:
	git submodule update --init --recursive $(GHOSTTY_SRC)

$(GHOSTTY_OUT)/lib/libghostty-vt.a: $(ZIGSRC) | $(GHOSTTY_SRC)/.git
	cd $(GHOSTTY_SRC) && zig build -Demit-lib-vt=true -Doptimize=ReleaseFast
	touch $@

ghostty-vt-module.so: $(GHOSTTY_OUT)/lib/libghostty-vt.a $(CSRC)
	$(BEAR) $(CC) $(CFLAGS) -shared -o $@ $(CSRC) $(LDFLAGS)

.PHONY: bump-mitch
bump-mitch:
	git -C $(GHOSTTY_SRC) fetch --tags --force origin
	git -C $(GHOSTTY_SRC) checkout tip
	git add $(GHOSTTY_SRC)
	git commit -m "bump ghostty to tip"

.PHONY: run
run: compile
	$(if $(DEBUG),DEBUGINFOD_URLS= gdb --args) $(EMACS) -Q -L $(CURDIR) -l ghostty-vt --eval "(setq debug-on-error t)" -f ghostty-vt

.PHONY: clean
clean:
	git clean -dfX

.PHONY: veryclean
veryclean: clean
	git -C $(GHOSTTY_SRC) clean -dfX

.PHONY: dist-clean
dist-clean: epkg-dist-clean

.PHONY: dist
dist: ghostty-vt-module.so epkg-dist

.PHONY: install
install: epkg-install

.PHONY: test
test: compile epkg-test
