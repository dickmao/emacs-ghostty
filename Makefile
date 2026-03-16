SHELL := /bin/bash
EMACS ?= emacs
CSRC := $(shell git ls-files '*.[ch]' 2>/dev/null || echo ghostty-vt-module.c elisp.c)
BEAR := $(shell command -v bear 2>/dev/null)
ifneq ($(BEAR),)
	BEAR := $(BEAR) --
endif

.PHONY: compile
compile: ghostty-vt-module.so

ghostty-vt-module.so: $(CSRC) CMakeLists.txt
	cmake -B build
	$(BEAR) cmake --build build --clean-first --config Release -j8

.PHONY: clean
clean:
	rm -rf build ghostty-vt-module.so
