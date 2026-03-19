#include <ghostty/vt.h>
#include "emacs-module.h"
#include "elisp.h"
#include <stdlib.h>
#include <string.h>

__attribute__((visibility("default"))) int plugin_is_GPL_compatible;


typedef struct {
  GhosttyTerminal terminal;
  GhosttyKeyEncoder encoder;
} GhosttyTerm;

static void term_finalizer(void *ptr) {
  GhosttyTerm *t = (GhosttyTerm *)ptr;
  if (t) {
    ghostty_key_encoder_free(t->encoder);
    ghostty_terminal_free(t->terminal);
    free(t);
  }
}

static GhosttyTerm *term_get(emacs_env *env, emacs_value arg) {
  return (GhosttyTerm *)env->get_user_ptr(env, arg);
}

/* ghostty-vt--new(rows cols scrollback) -> user-ptr */
static emacs_value Fghostty_vt__new(emacs_env *env, ptrdiff_t nargs,
                                    emacs_value args[], void *data) {
  int rows = (int)env->extract_integer(env, args[0]);
  int cols = (int)env->extract_integer(env, args[1]);
  size_t scrollback = (size_t)env->extract_integer(env, args[2]);

  GhosttyTerm *t = calloc(1, sizeof(GhosttyTerm));
  if (!t) return Qnil;

  GhosttyTerminalOptions opts = {
    .cols = (uint16_t)cols,
    .rows = (uint16_t)rows,
    .max_scrollback = scrollback,
  };
  if (ghostty_terminal_new(NULL, &t->terminal, opts) != GHOSTTY_SUCCESS) {
    free(t);
    return Qnil;
  }
  if (ghostty_key_encoder_new(NULL, &t->encoder) != GHOSTTY_SUCCESS) {
    ghostty_terminal_free(t->terminal);
    free(t);
    return Qnil;
  }
  ghostty_key_encoder_setopt_from_terminal(t->encoder, t->terminal);
  return env->make_user_ptr(env, term_finalizer, t);
}

/* ghostty-vt--write-input(term data) -> nil */
static emacs_value Fghostty_vt__write_input(emacs_env *env, ptrdiff_t nargs,
                                            emacs_value args[], void *data) {
  GhosttyTerm *t = term_get(env, args[0]);
  if (!t) return Qnil;

  ptrdiff_t size = 0;
  env->copy_string_contents(env, args[1], NULL, &size);
  char *buf = malloc((size_t)size);
  if (!buf) return Qnil;
  env->copy_string_contents(env, args[1], buf, &size);
  ghostty_terminal_vt_write(t->terminal, (const uint8_t *)buf, (size_t)(size - 1));
  free(buf);
  ghostty_key_encoder_setopt_from_terminal(t->encoder, t->terminal);
  return Qnil;
}

/* ghostty-vt--render(term) -> string (VT sequences with SGR color/style) */
static emacs_value Fghostty_vt__render(emacs_env *env, ptrdiff_t nargs,
                                       emacs_value args[], void *data) {
  GhosttyTerm *t = term_get(env, args[0]);
  if (!t) return env->make_string(env, "", 0);

  GhosttyFormatterTerminalOptions opts =
    GHOSTTY_INIT_SIZED(GhosttyFormatterTerminalOptions);
  opts.emit = GHOSTTY_FORMATTER_FORMAT_VT;
  opts.trim = true;

  GhosttyFormatter fmt;
  if (ghostty_formatter_terminal_new(NULL, &fmt, t->terminal, opts) !=
      GHOSTTY_SUCCESS)
    return env->make_string(env, "", 0);

  uint8_t *buf = NULL;
  size_t len = 0;
  GhosttyResult r = ghostty_formatter_format_alloc(fmt, NULL, &buf, &len);
  ghostty_formatter_free(fmt);
  if (r != GHOSTTY_SUCCESS) return env->make_string(env, "", 0);

  emacs_value result = env->make_string(env, (const char *)buf, (ptrdiff_t)len);
  free(buf);
  return result;
}

/* Scan buf for the first CSI CUP sequence (\033[row;colH). */
static bool parse_cup(const uint8_t *buf, size_t len, int *row, int *col) {
  for (size_t i = 0; i + 2 < len; i++) {
    if (buf[i] != 0x1b || buf[i + 1] != '[') continue;
    size_t j = i + 2;
    int r = 0, c = 0;
    bool has_r = false, has_c = false;
    while (j < len && buf[j] >= '0' && buf[j] <= '9') { r = r * 10 + (buf[j++] - '0'); has_r = true; }
    if (j >= len || buf[j] != ';') continue;
    j++;
    while (j < len && buf[j] >= '0' && buf[j] <= '9') { c = c * 10 + (buf[j++] - '0'); has_c = true; }
    if (j >= len || buf[j] != 'H') continue;
    *row = has_r ? r : 1;
    *col = has_c ? c : 1;
    return true;
  }
  return false;
}

/* ghostty-vt--cursor-pos(term) -> (row . col) 1-indexed, or nil */
static emacs_value Fghostty_vt__cursor_pos(emacs_env *env, ptrdiff_t nargs,
                                           emacs_value args[], void *data) {
  GhosttyTerm *t = term_get(env, args[0]);
  if (!t) return Qnil;

  GhosttyFormatterTerminalOptions opts =
    GHOSTTY_INIT_SIZED(GhosttyFormatterTerminalOptions);
  opts.emit = GHOSTTY_FORMATTER_FORMAT_VT;
  opts.extra.screen.size = sizeof(GhosttyFormatterScreenExtra);
  opts.extra.screen.cursor = true;

  GhosttyFormatter fmt;
  if (ghostty_formatter_terminal_new(NULL, &fmt, t->terminal, opts) !=
      GHOSTTY_SUCCESS)
    return Qnil;

  uint8_t *buf = NULL;
  size_t len = 0;
  GhosttyResult r = ghostty_formatter_format_alloc(fmt, NULL, &buf, &len);
  ghostty_formatter_free(fmt);
  if (r != GHOSTTY_SUCCESS) return Qnil;

  int row = 1, col = 1;
  bool found = parse_cup(buf, len, &row, &col);
  free(buf);
  if (!found) return Qnil;

  emacs_value erow = env->make_integer(env, row);
  emacs_value ecol = env->make_integer(env, col);
  return env->funcall(env, env->intern(env, "cons"), 2,
                      (emacs_value[]){erow, ecol});
}

static struct {
  const char *name;
  GhosttyKey key;
} key_table[] = {
  {"<return>",    GHOSTTY_KEY_ENTER},
  {"RET",         GHOSTTY_KEY_ENTER},
  {"<backspace>", GHOSTTY_KEY_BACKSPACE},
  {"DEL",         GHOSTTY_KEY_BACKSPACE},
  {"<tab>",       GHOSTTY_KEY_TAB},
  {"TAB",         GHOSTTY_KEY_TAB},
  {"<escape>",    GHOSTTY_KEY_ESCAPE},
  {"ESC",         GHOSTTY_KEY_ESCAPE},
  {"<up>",        GHOSTTY_KEY_ARROW_UP},
  {"<down>",      GHOSTTY_KEY_ARROW_DOWN},
  {"<left>",      GHOSTTY_KEY_ARROW_LEFT},
  {"<right>",     GHOSTTY_KEY_ARROW_RIGHT},
  {"<home>",      GHOSTTY_KEY_HOME},
  {"<end>",       GHOSTTY_KEY_END},
  {"<prior>",     GHOSTTY_KEY_PAGE_UP},
  {"<next>",      GHOSTTY_KEY_PAGE_DOWN},
  {"<insert>",    GHOSTTY_KEY_INSERT},
  {"<delete>",    GHOSTTY_KEY_DELETE},
  {"<f1>",        GHOSTTY_KEY_F1},
  {"<f2>",        GHOSTTY_KEY_F2},
  {"<f3>",        GHOSTTY_KEY_F3},
  {"<f4>",        GHOSTTY_KEY_F4},
  {"<f5>",        GHOSTTY_KEY_F5},
  {"<f6>",        GHOSTTY_KEY_F6},
  {"<f7>",        GHOSTTY_KEY_F7},
  {"<f8>",        GHOSTTY_KEY_F8},
  {"<f9>",        GHOSTTY_KEY_F9},
  {"<f10>",       GHOSTTY_KEY_F10},
  {"<f11>",       GHOSTTY_KEY_F11},
  {"<f12>",       GHOSTTY_KEY_F12},
  {NULL, 0}
};

/* ghostty-vt--send-key(term key-string shift alt ctrl) -> string */
static emacs_value Fghostty_vt__send_key(emacs_env *env, ptrdiff_t nargs,
                                         emacs_value args[], void *data) {
  GhosttyTerm *t = term_get(env, args[0]);
  if (!t) return env->make_string(env, "", 0);

  ptrdiff_t keysize = 0;
  env->copy_string_contents(env, args[1], NULL, &keysize);
  char *keystr = malloc((size_t)keysize);
  if (!keystr) return env->make_string(env, "", 0);
  env->copy_string_contents(env, args[1], keystr, &keysize);

  GhosttyMods mods = 0;
  if (env->is_not_nil(env, args[2])) mods |= GHOSTTY_MODS_SHIFT;
  if (env->is_not_nil(env, args[3])) mods |= GHOSTTY_MODS_ALT;
  if (env->is_not_nil(env, args[4])) mods |= GHOSTTY_MODS_CTRL;

  GhosttyKey key = GHOSTTY_KEY_UNIDENTIFIED;
  for (int i = 0; key_table[i].name; i++) {
    if (strcmp(keystr, key_table[i].name) == 0) {
      key = key_table[i].key;
      break;
    }
  }

  emacs_value result = env->make_string(env, "", 0);
  GhosttyKeyEvent event;
  if (ghostty_key_event_new(NULL, &event) == GHOSTTY_SUCCESS) {
    ghostty_key_event_set_action(event, GHOSTTY_KEY_ACTION_PRESS);
    ghostty_key_event_set_key(event, key);
    ghostty_key_event_set_mods(event, mods);
    if (key == GHOSTTY_KEY_UNIDENTIFIED)
      ghostty_key_event_set_utf8(event, keystr, (size_t)(keysize - 1));

    char outbuf[128];
    size_t written = 0;
    GhosttyResult r = ghostty_key_encoder_encode(t->encoder, event, outbuf,
                                                 sizeof(outbuf), &written);
    if (r == GHOSTTY_SUCCESS && written > 0) {
      result = env->make_string(env, outbuf, (ptrdiff_t)written);
    } else if (r == GHOSTTY_OUT_OF_MEMORY) {
      char *dynbuf = malloc(written);
      if (dynbuf) {
        r = ghostty_key_encoder_encode(t->encoder, event, dynbuf, written,
                                       &written);
        if (r == GHOSTTY_SUCCESS && written > 0)
          result = env->make_string(env, dynbuf, (ptrdiff_t)written);
        free(dynbuf);
      }
    }
    ghostty_key_event_free(event);
  }
  free(keystr);
  return result;
}

/* ghostty-vt--resize(term rows cols) -> nil */
static emacs_value Fghostty_vt__resize(emacs_env *env, ptrdiff_t nargs,
                                       emacs_value args[], void *data) {
  GhosttyTerm *t = term_get(env, args[0]);
  if (!t) return Qnil;
  ghostty_terminal_resize(t->terminal,
                          (uint16_t)env->extract_integer(env, args[2]),
                          (uint16_t)env->extract_integer(env, args[1]));
  ghostty_key_encoder_setopt_from_terminal(t->encoder, t->terminal);
  return Qnil;
}

__attribute__((visibility("default")))
int emacs_module_init(struct emacs_runtime *ert) {
  emacs_env *env = ert->get_environment(ert);
  Qt   = env->make_global_ref(env, env->intern(env, "t"));
  Qnil = env->make_global_ref(env, env->intern(env, "nil"));
  Finsert       = env->make_global_ref(env, env->intern(env, "insert"));
  Ferase_buffer = env->make_global_ref(env, env->intern(env, "erase-buffer"));

#define DEFUN(lname, fn, min, max) \
  bind_function(env, lname, env->make_function(env, min, max, fn, NULL, NULL))
  DEFUN("ghostty-vt--new",         Fghostty_vt__new,         3, 3);
  DEFUN("ghostty-vt--write-input", Fghostty_vt__write_input, 2, 2);
  DEFUN("ghostty-vt--render",      Fghostty_vt__render,      1, 1);
  DEFUN("ghostty-vt--send-key",    Fghostty_vt__send_key,    5, 5);
  DEFUN("ghostty-vt--resize",      Fghostty_vt__resize,      3, 3);
  DEFUN("ghostty-vt--cursor-pos",  Fghostty_vt__cursor_pos,  1, 1);
#undef DEFUN

  provide(env, "ghostty-vt-module");
  return 0;
}
