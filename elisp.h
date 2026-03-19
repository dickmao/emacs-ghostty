#ifndef ELISP_H
#define ELISP_H

#include "emacs-module.h"

extern emacs_value Qt;
extern emacs_value Qnil;
extern emacs_value Finsert;
extern emacs_value Ferase_buffer;

void bind_function(emacs_env *env, const char *name, emacs_value Sfun);
void provide(emacs_env *env, const char *feature);
void insert(emacs_env *env, emacs_value string);
void erase_buffer(emacs_env *env);

#endif /* ELISP_H */
