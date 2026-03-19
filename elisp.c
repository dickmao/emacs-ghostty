#include "elisp.h"

emacs_value Qt;
emacs_value Qnil;
emacs_value Finsert;
emacs_value Ferase_buffer;

void bind_function(emacs_env *env, const char *name, emacs_value Sfun) {
  emacs_value Qfset = env->intern(env, "fset");
  emacs_value Qsym = env->intern(env, name);
  env->funcall(env, Qfset, 2, (emacs_value[]){Qsym, Sfun});
}

void provide(emacs_env *env, const char *feature) {
  emacs_value Qfeat = env->intern(env, feature);
  emacs_value Qprovide = env->intern(env, "provide");
  env->funcall(env, Qprovide, 1, (emacs_value[]){Qfeat});
}

void insert(emacs_env *env, emacs_value string) {
  env->funcall(env, Finsert, 1, (emacs_value[]){string});
}

void erase_buffer(emacs_env *env) {
  env->funcall(env, Ferase_buffer, 0, NULL);
}
