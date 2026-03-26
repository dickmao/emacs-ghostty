#ifndef ELISP_H
#define ELISP_H

#include "emacs-module.h"

extern emacs_value Qt;
extern emacs_value Qnil;
extern emacs_value Qface;
extern emacs_value Finsert;
extern emacs_value Ferase_buffer;
extern emacs_value Fgoto_char;
extern emacs_value Fpoint;
extern emacs_value Fpoint_min;
extern emacs_value Fforward_line;
extern emacs_value Fforward_char;
extern emacs_value Fdelete_region;
extern emacs_value Fline_end_position;
extern emacs_value Fput_text_property;
extern emacs_value Flist;
extern emacs_value Sforeground;
extern emacs_value Sbackground;
extern emacs_value Sweight;
extern emacs_value Sslant;
extern emacs_value Sbold;
extern emacs_value Sitalic;

void bind_function(emacs_env *env, const char *name, emacs_value Sfun);
void provide(emacs_env *env, const char *feature);
void elisp_init(emacs_env *env);

#endif /* ELISP_H */
