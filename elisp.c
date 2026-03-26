#include "elisp.h"

emacs_value Qt;
emacs_value Qnil;
emacs_value Qface;
emacs_value Finsert;
emacs_value Ferase_buffer;
emacs_value Fgoto_char;
emacs_value Fpoint;
emacs_value Fpoint_min;
emacs_value Fforward_line;
emacs_value Fforward_char;
emacs_value Fdelete_region;
emacs_value Fline_end_position;
emacs_value Fput_text_property;
emacs_value Flist;
emacs_value Sforeground;
emacs_value Sbackground;
emacs_value Sweight;
emacs_value Sslant;
emacs_value Sbold;
emacs_value Sitalic;

void bind_function(emacs_env *env, const char *name, emacs_value Sfun) {
  emacs_value Qfset = env->intern(env, "fset");
  emacs_value Qsym  = env->intern(env, name);
  env->funcall(env, Qfset, 2, (emacs_value[]){Qsym, Sfun});
}

void provide(emacs_env *env, const char *feature) {
  emacs_value Qfeat    = env->intern(env, feature);
  emacs_value Qprovide = env->intern(env, "provide");
  env->funcall(env, Qprovide, 1, (emacs_value[]){Qfeat});
}

void elisp_init(emacs_env *env) {
#define G(var, sym) var = env->make_global_ref(env, env->intern(env, sym))
  G(Qt,                 "t");
  G(Qnil,               "nil");
  G(Qface,              "face");
  G(Finsert,            "insert");
  G(Ferase_buffer,      "erase-buffer");
  G(Fgoto_char,         "goto-char");
  G(Fpoint,             "point");
  G(Fpoint_min,         "point-min");
  G(Fforward_line,      "forward-line");
  G(Fforward_char,      "forward-char");
  G(Fdelete_region,     "delete-region");
  G(Fline_end_position, "line-end-position");
  G(Fput_text_property, "put-text-property");
  G(Flist,              "list");
  G(Sforeground,        ":foreground");
  G(Sbackground,        ":background");
  G(Sweight,            ":weight");
  G(Sslant,             ":slant");
  G(Sbold,              "bold");
  G(Sitalic,            "italic");
#undef G
}
