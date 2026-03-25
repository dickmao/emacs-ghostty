#include <ghostty/vt.h>
#include "emacs-module.h"
#include "elisp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((visibility("default"))) int plugin_is_GPL_compatible;

typedef struct {
  GhosttyTerminal terminal;
  GhosttyKeyEncoder encoder;
  GhosttyRenderState rs;
  GhosttyRenderStateRowIterator iter;
  GhosttyRenderStateRowCells cells;
} GhosttyTerm;

static void term_finalizer(void *ptr) {
  GhosttyTerm *t = (GhosttyTerm *)ptr;
  if (t) {
    ghostty_render_state_row_cells_free(t->cells);
    ghostty_render_state_row_iterator_free(t->iter);
    ghostty_render_state_free(t->rs);
    ghostty_key_encoder_free(t->encoder);
    ghostty_terminal_free(t->terminal);
    free(t);
  }
}

static GhosttyTerm *term_get(emacs_env *env, emacs_value arg) {
  return (GhosttyTerm *)env->get_user_ptr(env, arg);
}

/* --- dynamic byte buffer --- */
typedef struct { char *d; size_t n, cap; } Buf;

static void buf_grow(Buf *b, size_t extra) {
  if (b->n + extra <= b->cap) return;
  size_t nc = b->cap ? b->cap * 2 : 256;
  while (nc < b->n + extra) nc *= 2;
  char *nd = realloc(b->d, nc);
  if (nd) { b->d = nd; b->cap = nc; }
  else { free(b->d); b->d = NULL; b->n = b->cap = 0; }
}
static void buf_push(Buf *b, const char *s, size_t n) {
  buf_grow(b, n + 1);
  if (b->d) { memcpy(b->d + b->n, s, n); b->n += n; b->d[b->n] = '\0'; }
}

/* --- UTF-8 encoding --- */
static size_t cp_to_utf8(uint32_t cp, char out[4]) {
  if      (cp < 0x80)    { out[0] = (char)cp; return 1; }
  else if (cp < 0x800)   { out[0] = (char)(0xC0|(cp>>6));  out[1] = (char)(0x80|(cp&0x3F)); return 2; }
  else if (cp < 0x10000) { out[0] = (char)(0xE0|(cp>>12)); out[1] = (char)(0x80|((cp>>6)&0x3F));  out[2] = (char)(0x80|(cp&0x3F)); return 3; }
  else                   { out[0] = (char)(0xF0|(cp>>18));  out[1] = (char)(0x80|((cp>>12)&0x3F)); out[2] = (char)(0x80|((cp>>6)&0x3F)); out[3] = (char)(0x80|(cp&0x3F)); return 4; }
}

/* --- run: accumulated text + attrs pending insertion --- */
typedef struct {
  Buf text;
  bool has_fg, has_bg, bold, italic;
  GhosttyColorRgb fg, bg;
} Run;

static void flush_run(emacs_env *env, Run *r) {
  if (r->text.n == 0) return;
  emacs_value start = env->funcall(env, Fpoint, 0, NULL);
  emacs_value str   = env->make_string(env, r->text.d, (ptrdiff_t)r->text.n);
  env->funcall(env, Finsert, 1, &str);
  if (r->bold || r->italic || r->has_fg || r->has_bg) {
    emacs_value end = env->funcall(env, Fpoint, 0, NULL);
    emacs_value pargs[8]; int pn = 0;
    if (r->has_fg) {
      char hex[8]; snprintf(hex, sizeof(hex), "#%02x%02x%02x", r->fg.r, r->fg.g, r->fg.b);
      pargs[pn++] = Sforeground; pargs[pn++] = env->make_string(env, hex, 7);
    }
    if (r->has_bg) {
      char hex[8]; snprintf(hex, sizeof(hex), "#%02x%02x%02x", r->bg.r, r->bg.g, r->bg.b);
      pargs[pn++] = Sbackground; pargs[pn++] = env->make_string(env, hex, 7);
    }
    if (r->bold)   { pargs[pn++] = Sweight; pargs[pn++] = Sbold; }
    if (r->italic) { pargs[pn++] = Sslant;  pargs[pn++] = Sitalic; }
    emacs_value face = env->funcall(env, Flist, pn, pargs);
    env->funcall(env, Fput_text_property, 4, (emacs_value[]){start, end, Qface, face});
  }
  r->text.n = 0;
}

static void render_row(emacs_env *env, GhosttyRenderStateRowCells cells) {
  Run r = {0};
  while (ghostty_render_state_row_cells_next(cells)) {
    GhosttyCell raw_cell = 0;
    ghostty_render_state_row_cells_get(
        cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw_cell);
    int wide = GHOSTTY_CELL_WIDE_NARROW;
    ghostty_cell_get(raw_cell, GHOSTTY_CELL_DATA_WIDE, &wide);
    if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL || wide == GHOSTTY_CELL_WIDE_SPACER_HEAD)
      continue;

    uint32_t grapheme_len = 0;
    ghostty_render_state_row_cells_get(
        cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &grapheme_len);

    GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
    ghostty_render_state_row_cells_get(
        cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);

    GhosttyColorRgb fg, bg;
    bool has_fg = ghostty_render_state_row_cells_get(
        cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fg) == GHOSTTY_SUCCESS;
    bool has_bg = ghostty_render_state_row_cells_get(
        cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg) == GHOSTTY_SUCCESS;

    bool changed = r.bold != style.bold || r.italic != style.italic
      || r.has_fg != has_fg || r.has_bg != has_bg
      || (has_fg && memcmp(&r.fg, &fg, sizeof fg))
      || (has_bg && memcmp(&r.bg, &bg, sizeof bg));
    if (changed) {
      flush_run(env, &r);
      r.bold = style.bold; r.italic = style.italic;
      r.has_fg = has_fg; r.fg = fg;
      r.has_bg = has_bg; r.bg = bg;
    }

    if (grapheme_len == 0) {
      buf_push(&r.text, " ", 1);
    } else {
      uint32_t cps[16];
      uint32_t len = grapheme_len < 16 ? grapheme_len : 16;
      ghostty_render_state_row_cells_get(
          cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, cps);
      for (uint32_t i = 0; i < len; i++) {
        char utf8[4];
        buf_push(&r.text, utf8, cp_to_utf8(cps[i], utf8));
      }
    }
  }
  flush_run(env, &r);
  free(r.text.d);
}

/* ghostty-vt--new(rows cols scrollback) -> user-ptr */
static emacs_value Fghostty_vt__new(emacs_env *env, ptrdiff_t nargs,
                                    emacs_value args[], void *data) {
  (void)nargs; (void)data;
  int rows = (int)env->extract_integer(env, args[0]);
  int cols = (int)env->extract_integer(env, args[1]);
  size_t scrollback = (size_t)env->extract_integer(env, args[2]);

  GhosttyTerm *t = calloc(1, sizeof(GhosttyTerm));
  if (!t) return Qnil;

  GhosttyTerminalOptions opts = {
    .cols = (uint16_t)cols, .rows = (uint16_t)rows, .max_scrollback = scrollback,
  };
  if (ghostty_terminal_new(NULL, &t->terminal, opts) != GHOSTTY_SUCCESS) { free(t); return Qnil; }
  if (ghostty_key_encoder_new(NULL, &t->encoder) != GHOSTTY_SUCCESS) {
    ghostty_terminal_free(t->terminal); free(t); return Qnil;
  }
  if (ghostty_render_state_new(NULL, &t->rs) != GHOSTTY_SUCCESS) {
    ghostty_key_encoder_free(t->encoder); ghostty_terminal_free(t->terminal); free(t); return Qnil;
  }
  if (ghostty_render_state_row_iterator_new(NULL, &t->iter) != GHOSTTY_SUCCESS) {
    ghostty_render_state_free(t->rs);
    ghostty_key_encoder_free(t->encoder); ghostty_terminal_free(t->terminal); free(t); return Qnil;
  }
  if (ghostty_render_state_row_cells_new(NULL, &t->cells) != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_iterator_free(t->iter);
    ghostty_render_state_free(t->rs);
    ghostty_key_encoder_free(t->encoder); ghostty_terminal_free(t->terminal); free(t); return Qnil;
  }
  ghostty_key_encoder_setopt_from_terminal(t->encoder, t->terminal);
  return env->make_user_ptr(env, term_finalizer, t);
}

/* ghostty-vt--write-input(term data) -> nil */
static emacs_value Fghostty_vt__write_input(emacs_env *env, ptrdiff_t nargs,
                                            emacs_value args[], void *data) {
  (void)nargs; (void)data;
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

/* ghostty-vt--render(term)
   Directly manipulates the current Emacs buffer.
   Returns t if the buffer was updated, nil if nothing was dirty. */
static emacs_value Fghostty_vt__render(emacs_env *env, ptrdiff_t nargs,
                                       emacs_value args[], void *data) {
  (void)nargs; (void)data;
  GhosttyTerm *t = term_get(env, args[0]);
  if (!t) return Qnil;

  ghostty_render_state_update(t->rs, t->terminal);

  GhosttyRenderStateDirty dirty;
  ghostty_render_state_get(t->rs, GHOSTTY_RENDER_STATE_DATA_DIRTY, &dirty);
  if (dirty == GHOSTTY_RENDER_STATE_DIRTY_FALSE) return Qnil;

  ghostty_render_state_get(t->rs, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &t->iter);

  if (dirty == GHOSTTY_RENDER_STATE_DIRTY_FULL) {
    env->funcall(env, Ferase_buffer, 0, NULL);
    uint16_t y = 0;
    while (ghostty_render_state_row_iterator_next(t->iter)) {
      if (y > 0) {
        emacs_value nl = env->make_string(env, "\n", 1);
        env->funcall(env, Finsert, 1, &nl);
      }
      ghostty_render_state_row_get(t->iter, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &t->cells);
      render_row(env, t->cells);
      bool clean = false;
      ghostty_render_state_row_set(t->iter, GHOSTTY_RENDER_STATE_ROW_OPTION_DIRTY, &clean);
      y++;
    }
  } else {
    uint16_t y = 0;
    while (ghostty_render_state_row_iterator_next(t->iter)) {
      bool row_dirty = false;
      ghostty_render_state_row_get(t->iter, GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY, &row_dirty);
      if (row_dirty) {
        emacs_value pm = env->funcall(env, Fpoint_min, 0, NULL);
        env->funcall(env, Fgoto_char, 1, &pm);
        emacs_value yn = env->make_integer(env, y);
        env->funcall(env, Fforward_line, 1, &yn);
        emacs_value ls = env->funcall(env, Fpoint, 0, NULL);
        emacs_value le = env->funcall(env, Fline_end_position, 0, NULL);
        env->funcall(env, Fdelete_region, 2, (emacs_value[]){ls, le});
        ghostty_render_state_row_get(t->iter, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &t->cells);
        render_row(env, t->cells);
        bool clean = false;
        ghostty_render_state_row_set(t->iter, GHOSTTY_RENDER_STATE_ROW_OPTION_DIRTY, &clean);
      }
      y++;
    }
  }

  GhosttyRenderStateDirty clean_state = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
  ghostty_render_state_set(t->rs, GHOSTTY_RENDER_STATE_OPTION_DIRTY, &clean_state);
  return Qt;
}

/* ghostty-vt--cursor-pos(term) -> (row . col) 1-indexed, or nil */
static emacs_value Fghostty_vt__cursor_pos(emacs_env *env, ptrdiff_t nargs,
                                           emacs_value args[], void *data) {
  (void)nargs; (void)data;
  GhosttyTerm *t = term_get(env, args[0]);
  if (!t) return Qnil;
  bool visible = false, in_viewport = false;
  ghostty_render_state_get(t->rs, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE, &visible);
  ghostty_render_state_get(t->rs, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE, &in_viewport);
  if (!visible || !in_viewport) return Qnil;
  uint16_t cx = 0, cy = 0;
  ghostty_render_state_get(t->rs, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &cx);
  ghostty_render_state_get(t->rs, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cy);
  emacs_value erow = env->make_integer(env, cy + 1);
  emacs_value ecol = env->make_integer(env, cx + 1);
  return env->funcall(env, env->intern(env, "cons"), 2, (emacs_value[]){erow, ecol});
}

static struct { const char *name; GhosttyKey key; } key_table[] = {
  {"<return>",    GHOSTTY_KEY_ENTER},      {"RET",         GHOSTTY_KEY_ENTER},
  {"<backspace>", GHOSTTY_KEY_BACKSPACE},  {"DEL",         GHOSTTY_KEY_BACKSPACE},
  {"<tab>",       GHOSTTY_KEY_TAB},        {"TAB",         GHOSTTY_KEY_TAB},
  {"<escape>",    GHOSTTY_KEY_ESCAPE},     {"ESC",         GHOSTTY_KEY_ESCAPE},
  {"SPC",         GHOSTTY_KEY_SPACE},
  {"<up>",        GHOSTTY_KEY_ARROW_UP},   {"<down>",      GHOSTTY_KEY_ARROW_DOWN},
  {"<left>",      GHOSTTY_KEY_ARROW_LEFT}, {"<right>",     GHOSTTY_KEY_ARROW_RIGHT},
  {"<home>",      GHOSTTY_KEY_HOME},       {"<end>",       GHOSTTY_KEY_END},
  {"<prior>",     GHOSTTY_KEY_PAGE_UP},    {"<next>",      GHOSTTY_KEY_PAGE_DOWN},
  {"<insert>",    GHOSTTY_KEY_INSERT},     {"<delete>",    GHOSTTY_KEY_DELETE},
  {"<f1>",        GHOSTTY_KEY_F1},         {"<f2>",        GHOSTTY_KEY_F2},
  {"<f3>",        GHOSTTY_KEY_F3},         {"<f4>",        GHOSTTY_KEY_F4},
  {"<f5>",        GHOSTTY_KEY_F5},         {"<f6>",        GHOSTTY_KEY_F6},
  {"<f7>",        GHOSTTY_KEY_F7},         {"<f8>",        GHOSTTY_KEY_F8},
  {"<f9>",        GHOSTTY_KEY_F9},         {"<f10>",       GHOSTTY_KEY_F10},
  {"<f11>",       GHOSTTY_KEY_F11},        {"<f12>",       GHOSTTY_KEY_F12},
  {"<backtab>",   GHOSTTY_KEY_TAB},        {"<iso-lefttab>", GHOSTTY_KEY_TAB},
  {"<kp-0>",      GHOSTTY_KEY_NUMPAD_0},   {"<kp-1>",      GHOSTTY_KEY_NUMPAD_1},
  {"<kp-2>",      GHOSTTY_KEY_NUMPAD_2},   {"<kp-3>",      GHOSTTY_KEY_NUMPAD_3},
  {"<kp-4>",      GHOSTTY_KEY_NUMPAD_4},   {"<kp-5>",      GHOSTTY_KEY_NUMPAD_5},
  {"<kp-6>",      GHOSTTY_KEY_NUMPAD_6},   {"<kp-7>",      GHOSTTY_KEY_NUMPAD_7},
  {"<kp-8>",      GHOSTTY_KEY_NUMPAD_8},   {"<kp-9>",      GHOSTTY_KEY_NUMPAD_9},
  {"<kp-add>",      GHOSTTY_KEY_NUMPAD_ADD},      {"<kp-subtract>", GHOSTTY_KEY_NUMPAD_SUBTRACT},
  {"<kp-multiply>", GHOSTTY_KEY_NUMPAD_MULTIPLY}, {"<kp-divide>",   GHOSTTY_KEY_NUMPAD_DIVIDE},
  {"<kp-equal>",    GHOSTTY_KEY_NUMPAD_EQUAL},    {"<kp-decimal>",  GHOSTTY_KEY_NUMPAD_DECIMAL},
  {"<kp-separator>",GHOSTTY_KEY_NUMPAD_SEPARATOR},{"<kp-enter>",    GHOSTTY_KEY_NUMPAD_ENTER},
  {NULL, 0}
};

/* ghostty-vt--encode-key(term key-string shift alt ctrl) -> string */
static emacs_value Fghostty_vt__encode_key(emacs_env *env, ptrdiff_t nargs,
                                         emacs_value args[], void *data) {
  (void)nargs; (void)data;
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
    if (strcmp(keystr, key_table[i].name) == 0) { key = key_table[i].key; break; }
  }
  emacs_value result = env->make_string(env, "", 0);
  GhosttyKeyEvent event;
  if (ghostty_key_event_new(NULL, &event) == GHOSTTY_SUCCESS) {
    ghostty_key_event_set_action(event, GHOSTTY_KEY_ACTION_PRESS);
    ghostty_key_event_set_key(event, key);
    ghostty_key_event_set_mods(event, mods);
    if (key == GHOSTTY_KEY_UNIDENTIFIED)
      ghostty_key_event_set_utf8(event, keystr, (size_t)(keysize - 1));
    char outbuf[128]; size_t written = 0;
    GhosttyResult r = ghostty_key_encoder_encode(t->encoder, event, outbuf, sizeof(outbuf), &written);
    if (r == GHOSTTY_SUCCESS && written > 0) {
      result = env->make_string(env, outbuf, (ptrdiff_t)written);
    } else if (r == GHOSTTY_OUT_OF_MEMORY) {
      char *dynbuf = malloc(written);
      if (dynbuf) {
        r = ghostty_key_encoder_encode(t->encoder, event, dynbuf, written, &written);
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

/* ghostty-vt--resize(term rows cols cell-width-px cell-height-px) -> nil */
static emacs_value Fghostty_vt__resize(emacs_env *env, ptrdiff_t nargs,
                                       emacs_value args[], void *data) {
  (void)nargs; (void)data;
  GhosttyTerm *t = term_get(env, args[0]);
  if (!t) return Qnil;
  ghostty_terminal_resize(t->terminal,
                          (uint16_t)env->extract_integer(env, args[2]),
                          (uint16_t)env->extract_integer(env, args[1]),
                          (uint32_t)env->extract_integer(env, args[3]),
                          (uint32_t)env->extract_integer(env, args[4]));
  ghostty_key_encoder_setopt_from_terminal(t->encoder, t->terminal);
  return Qnil;
}

__attribute__((visibility("default")))
int emacs_module_init(struct emacs_runtime *ert) {
  emacs_env *env = ert->get_environment(ert);
  elisp_init(env);
#define DEFUN(lname, fn, min, max) \
  bind_function(env, lname, env->make_function(env, min, max, fn, NULL, NULL))
  DEFUN("ghostty-vt--new",         Fghostty_vt__new,         3, 3);
  DEFUN("ghostty-vt--write-input", Fghostty_vt__write_input, 2, 2);
  DEFUN("ghostty-vt--render",      Fghostty_vt__render,      1, 1);
  DEFUN("ghostty-vt--encode-key",  Fghostty_vt__encode_key,  5, 5);
  DEFUN("ghostty-vt--resize",      Fghostty_vt__resize,      5, 5);
  DEFUN("ghostty-vt--cursor-pos",  Fghostty_vt__cursor_pos,  1, 1);
#undef DEFUN
  provide(env, "ghostty-vt-module");
  return 0;
}
