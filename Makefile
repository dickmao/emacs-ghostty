SHELL := /bin/bash
EMACS ?= emacs
CC    ?= cc
ELSRC := $(shell git ls-files *.el)
TESTSRC := $(shell git ls-files test/*.el)

GHOSTTY_SRC := vendor/ghostty
GHOSTTY_OUT := $(GHOSTTY_SRC)/zig-out

CSRC  := $(shell git ls-files '*.c' 2>/dev/null)
ZIGSRC := $(shell find $(GHOSTTY_SRC)/src -name '*.zig' 2>/dev/null)

BEAR := $(shell command -v bear 2>/dev/null)
ifneq ($(BEAR),)
	BEAR := $(BEAR) --
endif

CFLAGS := -std=c99 -Werror -fvisibility=hidden -fPIC -g \
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
	cd $(GHOSTTY_SRC) && zig build -Demit-lib-vt=true -Doptimize=ReleaseFast

ghostty-vt-module.so: $(GHOSTTY_OUT)/lib/libghostty-vt.so $(CSRC)
	$(BEAR) $(CC) $(CFLAGS) -shared -o $@ $(CSRC) $(LDFLAGS)

.PHONY: run
run: compile
	$(if $(DEBUG),DEBUGINFOD_URLS= gdb --args) $(EMACS) -Q -L $(CURDIR) -l ghostty-vt --eval "(setq debug-on-error t)" -f ghostty-vt

.PHONY: clean
clean:
	git clean -dfX
	git -C $(GHOSTTY_SRC) clean -dfX

.PHONY: dist-clean
dist-clean:
	( \
	set -e; \
	PKG_NAME=`$(EMACS) -batch -L . -l ghostty-vt-package --eval "(princ (ghostty-vt-package-name))"`; \
	rm -rf $${PKG_NAME}; \
	rm -rf $${PKG_NAME}.tar; \
	)

.PHONY: dist
dist: dist-clean ghostty-vt-module.so
	$(EMACS) -batch -L . -l ghostty-vt-package -f ghostty-vt-package-inception
	( \
	set -e; \
	PKG_NAME=`$(EMACS) -batch -L . -l ghostty-vt-package --eval "(princ (ghostty-vt-package-name))"`; \
	rsync -R ghostty-vt-module.so $(ELSRC) $${PKG_NAME} && \
	tar cf $${PKG_NAME}.tar $${PKG_NAME}; \
	)

.PHONY: install
install:
	$(call install-recipe,package-user-dir)

define install-recipe
	$(MAKE) dist
	( \
	set -e; \
	INSTALL_PATH=$(1); \
	if [[ "$${INSTALL_PATH}" == /* ]]; then INSTALL_PATH=\"$${INSTALL_PATH}\"; fi; \
	PKG_NAME=`$(EMACS) -batch -L . -l ghostty-vt-package --eval "(princ (ghostty-vt-package-name))"`; \
	$(EMACS) --batch -l package --eval "(setq package-user-dir (expand-file-name $${INSTALL_PATH}))" \
	  -f package-initialize \
	  --eval "(ignore-errors (apply (function package-delete) (alist-get (quote ghostty-vt) package-alist)))" \
	  -f package-refresh-contents \
	  --eval "(package-install-file \"$${PKG_NAME}.tar\")"; \
	PKG_DIR=`$(EMACS) -batch -l package --eval "(setq package-user-dir (expand-file-name $${INSTALL_PATH}))" -f package-initialize --eval "(princ (package-desc-dir (car (alist-get 'ghostty-vt package-alist))))"`; \
	)
	$(MAKE) dist-clean
endef

.PHONY: test
test: compile
	$(EMACS) --batch -L . -L test $(patsubst %.el,-l %,$(notdir $(TESTSRC))) -f ert-run-tests-batch
