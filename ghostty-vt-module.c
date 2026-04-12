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

/* --- UTF-8 encoding --- */
static size_t cp_to_utf8(uint32_t cp, char out[4]) {
  if      (cp < 0x80)    { out[0] = (char)cp; return 1; }
  else if (cp < 0x800)   { out[0] = (char)(0xC0|(cp>>6));  out[1] = (char)(0x80|(cp&0x3F)); return 2; }
  else if (cp < 0x10000) { out[0] = (char)(0xE0|(cp>>12)); out[1] = (char)(0x80|((cp>>6)&0x3F));  out[2] = (char)(0x80|(cp&0x3F)); return 3; }
  else                   { out[0] = (char)(0xF0|(cp>>18));  out[1] = (char)(0x80|((cp>>12)&0x3F)); out[2] = (char)(0x80|((cp>>6)&0x3F)); out[3] = (char)(0x80|(cp&0x3F)); return 4; }
}

#define MAX_ROW_BYTES (512 * 64)

static size_t encode_cps(const uint32_t *cps, size_t ncp, char *buf, size_t cap) {
  size_t n = 0;
  for (size_t i = 0; i < ncp; i++) {
    char utf8[4]; size_t k = cp_to_utf8(cps[i], utf8);
    if (n + k <= cap) { memcpy(buf + n, utf8, k); n += k; }
  }
  return n;
}

static void flush_padding(char *buf, size_t *buf_n, size_t buf_cap, int padding) {
  for (int i = 0; i < padding; i++) {
    if (*buf_n < buf_cap) buf[(*buf_n)++] = ' ';
  }
}

static void flush_default(emacs_env *env, char *buf, size_t n) {
  if (n == 0) return;
  emacs_value str = env->make_string(env, buf, (ptrdiff_t)n);
  env->funcall(env, Finsert, 1, &str);
}

static void insert_styled(emacs_env *env, const char *text, size_t n,
                          const GhosttyStyle *style,
                          GhosttyColorRgb fg, GhosttyColorRgb bg) {
  emacs_value start = env->funcall(env, Fpoint, 0, NULL);
  emacs_value str = env->make_string(env, text, (ptrdiff_t)n);
  env->funcall(env, Finsert, 1, &str);
  emacs_value end = env->funcall(env, Fpoint, 0, NULL);
  emacs_value pargs[20]; int pn = 0;
  if (style->fg_color.tag != GHOSTTY_STYLE_COLOR_NONE) {
    char hex[8]; snprintf(hex, sizeof(hex), "#%02x%02x%02x", fg.r, fg.g, fg.b);
    pargs[pn++] = Sforeground; pargs[pn++] = env->make_string(env, hex, 7);
  }
  if (style->bg_color.tag != GHOSTTY_STYLE_COLOR_NONE) {
    char hex[8]; snprintf(hex, sizeof(hex), "#%02x%02x%02x", bg.r, bg.g, bg.b);
    pargs[pn++] = Sbackground; pargs[pn++] = env->make_string(env, hex, 7);
  }
  if (style->bold)        { pargs[pn++] = Sweight; pargs[pn++] = Sbold; }
  else if (style->faint)  { pargs[pn++] = Sweight; pargs[pn++] = Slight; }
  if (style->italic)      { pargs[pn++] = Sslant;  pargs[pn++] = Sitalic; }
  if (style->inverse)     { pargs[pn++] = Sinverse_video;  pargs[pn++] = Qt; }
  if (style->strikethrough){ pargs[pn++] = Sstrike_through; pargs[pn++] = Qt; }
  if (style->overline)    { pargs[pn++] = Soverline;        pargs[pn++] = Qt; }
  if (style->underline != GHOSTTY_SGR_UNDERLINE_NONE) { pargs[pn++] = Sunderline; pargs[pn++] = Qt; }
  emacs_value face = env->funcall(env, Flist, pn, pargs);
  env->funcall(env, Fput_text_property, 4, (emacs_value[]){start, end, Qface, face});
}

static void process_cell(emacs_env *env,
                         char *buf, size_t *buf_n, size_t buf_cap, int *padding,
                         const GhosttyStyle *style,
                         const uint32_t *cps, size_t ncp,
                         GhosttyColorRgb fg, GhosttyColorRgb bg) {
  /* no glyph, no background -> pure padding, defer */
  if (ncp == 0 && style->bg_color.tag == GHOSTTY_STYLE_COLOR_NONE) {
    (*padding)++;
    return;
  }
  flush_padding(buf, buf_n, buf_cap, *padding); *padding = 0;
  if (ghostty_style_is_default(style)) {
    if (ncp == 0) {
      if (*buf_n < buf_cap) buf[(*buf_n)++] = ' ';
    } else {
      *buf_n += encode_cps(cps, ncp, buf + *buf_n, buf_cap - *buf_n);
    }
  } else {
    flush_default(env, buf, *buf_n); *buf_n = 0;
    char cell[64]; size_t cell_n;
    if (ncp == 0) { cell[0] = ' '; cell_n = 1; }
    else          { cell_n = encode_cps(cps, ncp, cell, sizeof cell); }
    insert_styled(env, cell, cell_n, style, fg, bg);
  }
}

static void render_row(emacs_env *env, GhosttyRenderStateRowCells cells) {
  char buf[MAX_ROW_BYTES]; size_t buf_n = 0;
  int padding = 0;
  while (ghostty_render_state_row_cells_next(cells)) {
    GhosttyCell raw_cell = 0;
    ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw_cell);
    int wide = GHOSTTY_CELL_WIDE_NARROW;
    ghostty_cell_get(raw_cell, GHOSTTY_CELL_DATA_WIDE, &wide);
    if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL || wide == GHOSTTY_CELL_WIDE_SPACER_HEAD)
      continue;
    uint32_t grapheme_len = 0;
    ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &grapheme_len);
    GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
    ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);
    uint32_t cps[16] = {0};
    size_t ncp = grapheme_len < 16 ? grapheme_len : 16;
    if (ncp > 0)
      ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, cps);
    GhosttyColorRgb fg = {0}, bg = {0};
    ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fg);
    ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg);
    process_cell(env, buf, &buf_n, sizeof buf, &padding, &style, cps, ncp, fg, bg);
  }
  flush_default(env, buf, buf_n); buf_n = 0;
  /* padding discarded — trailing cells are pure padding */
}

static void resolve_style_color(GhosttyStyleColor sc, GhosttyColorRgb *out, bool *has,
				const GhosttyRenderStateColors *colors) {
  switch (sc.tag) {
  case GHOSTTY_STYLE_COLOR_PALETTE:
    *out = colors->palette[sc.value.palette];
    *has = true;
    break;
  case GHOSTTY_STYLE_COLOR_RGB:
    *out = sc.value.rgb;
    *has = true;
    break;
  default:
    *has = false;
    break;
  }
}

static void render_sb_row(emacs_env *env, GhosttyTerminal terminal,
			  const size_t row, const uint16_t cols,
			  const GhosttyRenderStateColors *colors) {
  char buf[MAX_ROW_BYTES]; size_t buf_n = 0;
  int padding = 0;
  for (uint16_t col = 0; col < cols; col++) {
    GhosttyPoint pt = {
      .tag = GHOSTTY_POINT_TAG_HISTORY,
      .value = { .coordinate = { .x = col, .y = (uint32_t)row } }
    };
    GhosttyGridRef ref = GHOSTTY_INIT_SIZED(GhosttyGridRef);
    if (ghostty_terminal_grid_ref(terminal, pt, &ref) != GHOSTTY_SUCCESS)
      continue;
    GhosttyCell cell = 0;
    ghostty_grid_ref_cell(&ref, &cell);
    int wide = GHOSTTY_CELL_WIDE_NARROW;
    ghostty_cell_get(cell, GHOSTTY_CELL_DATA_WIDE, &wide);
    if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL || wide == GHOSTTY_CELL_WIDE_SPACER_HEAD)
      continue;
    GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
    ghostty_grid_ref_style(&ref, &style);
    uint32_t cps[16]; size_t ncp = 0;
    ghostty_grid_ref_graphemes(&ref, cps, 16, &ncp);
    GhosttyColorRgb fg = {0}, bg = {0};
    bool unused = false;
    resolve_style_color(style.fg_color, &fg, &unused, colors);
    resolve_style_color(style.bg_color, &bg, &unused, colors);
    process_cell(env, buf, &buf_n, sizeof buf, &padding, &style, cps, ncp, fg, bg);
  }
  flush_default(env, buf, buf_n); buf_n = 0;
  /* padding discarded — trailing cells are pure padding */
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
  emacs_value one = env->make_integer(env, 1);
  emacs_value overlay = env->funcall(env, Fmake_overlay, 2, (emacs_value[]){one, one});
  env->funcall(env, Fset, 2, (emacs_value[]){Qghostty_vt__cursor_overlay, overlay});
  return env->make_user_ptr(env, term_finalizer, t);
}

/* ghostty-vt--write(term data) -> nil */
static emacs_value Fghostty_vt__write(emacs_env *env, ptrdiff_t nargs,
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
   Renders viewport into current Emacs buffer, accounting for any
   scrollback lines prepended above the viewport.
   Returns t if the buffer was updated, nil if nothing was dirty. */
static emacs_value Fghostty_vt__render(emacs_env *env, ptrdiff_t nargs,
                                       emacs_value args[], void *data) {
  (void)nargs; (void)data;
  GhosttyTerm *t = term_get(env, args[0]);
  if (!t) return Qnil;

  ghostty_render_state_update(t->rs, t->terminal);
  ghostty_render_state_get(t->rs, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &t->iter);

  bool cursor_visible = false, cursor_in_viewport = false;
  ghostty_render_state_get(t->rs, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE, &cursor_visible);
  ghostty_render_state_get(t->rs, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE, &cursor_in_viewport);

  uint16_t cx = 0, cy = 0;
  if (cursor_visible && cursor_in_viewport
      && GHOSTTY_SUCCESS != ghostty_render_state_get
      (t->rs, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &cx)
      || GHOSTTY_SUCCESS != ghostty_render_state_get
      (t->rs, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cy)) {
    cx = cy = -1;
  }

  emacs_value pm = env->funcall(env, Fpoint_min, 0, NULL);
  env->funcall(env, Fgoto_char, 1, &pm);

  emacs_value overlay = env->funcall(env, Fsymbol_value, 1, (emacs_value[]){Qghostty_vt__cursor_overlay});
  env->funcall(env, Fdelete_overlay, 1, &overlay);

  intmax_t window_width = env->extract_integer(env, env->funcall(env, Fwindow_width, 0, NULL));
  for (int y = 0; ghostty_render_state_row_iterator_next(t->iter); ++y) {
    bool row_dirty = false;
    ghostty_render_state_row_get(t->iter, GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY, &row_dirty);
    emacs_value beg = env->funcall(env, Fpoint, 0, NULL);
    if (!row_dirty) {
      env->funcall(env, Fvertical_motion, 1, (emacs_value[]){env->make_integer(env, 1)});
    } else {
      intmax_t logical = env->extract_integer(env, env->funcall(env, Fline_end_position, 0, NULL));
      intmax_t physical = env->extract_integer(env, beg) + window_width;
      emacs_value end;
      if (logical <= physical) {
	/* +1 for newline */
	end = env->make_integer(env, logical + 1);
      } else {
	end = env->make_integer(env, physical);;
      }
      emacs_value pm = env->funcall(env, Fpoint_max, 0, NULL);
      if (env->extract_integer(env, end) <= env->extract_integer(env, pm))
	env->funcall(env, Fdelete_region, 2, (emacs_value[]){beg, end});
      ghostty_render_state_row_get(t->iter, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &t->cells);
      render_row(env, t->cells);

      GhosttyRow raw_row;
      ghostty_render_state_row_get(t->iter, GHOSTTY_RENDER_STATE_ROW_DATA_RAW, &raw_row);
      bool wrap = false;
      ghostty_row_get(raw_row, GHOSTTY_ROW_DATA_WRAP, &wrap);
      if (!wrap) {
	emacs_value nl = env->make_string(env, "\n", 1);
	env->funcall(env, Finsert, 1, &nl);
      }

      bool clean = false;
      ghostty_render_state_row_set(t->iter, GHOSTTY_RENDER_STATE_ROW_OPTION_DIRTY, &clean);
    }

    if (y == cy) {
      emacs_value restore = env->funcall(env, Fpoint, 0, NULL);
      env->funcall(env, Fgoto_char, 1, &beg);

      GhosttyColorRgb color;
      bool has_cursor_color = false;
      ghostty_render_state_get(t->rs, GHOSTTY_RENDER_STATE_DATA_COLOR_CURSOR_HAS_VALUE, &has_cursor_color);
      ghostty_render_state_get(t->rs, has_cursor_color
			       ? GHOSTTY_RENDER_STATE_DATA_COLOR_CURSOR
			       : GHOSTTY_RENDER_STATE_DATA_COLOR_FOREGROUND, &color);
      char hex[8];
      snprintf(hex, sizeof(hex), "#%02x%02x%02x", color.r, color.g, color.b);
      emacs_value end = env->funcall(env, Fline_end_position, 0, NULL);
      intmax_t len = env->extract_integer(env, end) - env->extract_integer(env, beg);
      if (len <= (intmax_t)cx) {
	env->funcall(env, Fgoto_char, 1, &end);
	intmax_t needed = (intmax_t)cx + 1 - len;
	if (needed > 512) needed = 512;
	char spaces[512];
	memset(spaces, ' ', (size_t)needed);
	emacs_value sp = env->make_string(env, spaces, (ptrdiff_t)needed);
	env->funcall(env, Finsert, 1, &sp);
env->funcall(env, Fgoto_char, 1, &beg);
      }
      env->funcall(env, Fforward_char, 1, (emacs_value[]){env->make_integer(env, cx)});
      emacs_value cs = env->funcall(env, Fpoint, 0, NULL);
      emacs_value ce = env->make_integer(env, env->extract_integer(env, cs) + 1);
      emacs_value face = env->funcall(env, Flist, 2,
				      (emacs_value[]){Sbackground,
						      env->make_string(env, hex, 7)});
      env->funcall(env, Fmove_overlay, 3, (emacs_value[]){overlay, cs, ce});
      env->funcall(env, Foverlay_put, 3, (emacs_value[]){overlay, Qface, face});
      env->funcall(env, Fgoto_char, 1, &restore);
    }
  }
  GhosttyRenderStateDirty clean_state = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
  ghostty_render_state_set(t->rs, GHOSTTY_RENDER_STATE_OPTION_DIRTY, &clean_state);
  return Qt;
}

/* ghostty-vt--prepend-history(term)
   Reads all scrollback rows from ghostty and inserts them at the top of
   the current Emacs buffer, oldest row first.
   Returns the number of lines prepended. */
static emacs_value Fghostty_vt__prepend_history(emacs_env *env, ptrdiff_t nargs,
						emacs_value args[], void *data) {
  (void)nargs; (void)data;
  GhosttyTerm *t = term_get(env, args[0]);
  if (!t) return env->make_integer(env, 0);

  uint16_t cols = 0;
  ghostty_terminal_get(t->terminal, GHOSTTY_TERMINAL_DATA_COLS, &cols);

  size_t sb_rows = 0;
  ghostty_terminal_get(t->terminal, GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS, &sb_rows);

  GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
  ghostty_render_state_colors_get(t->rs, &colors);

  emacs_value pm = env->funcall(env, Fpoint_min, 0, NULL);
  env->funcall(env, Fgoto_char, 1, &pm);

  for (size_t row = 0; row < sb_rows; row++) {
    render_sb_row(env, t->terminal, row, cols, &colors);
    emacs_value nl = env->make_string(env, "\n", 1);
    env->funcall(env, Finsert, 1, &nl);
  }

  return env->make_integer(env, (intmax_t)sb_rows);
}

/* ghostty-vt--discard-history(term)
   Removes the scrollback lines from the top of the current Emacs buffer.
   Returns nil. */
static emacs_value Fghostty_vt__discard_history(emacs_env *env, ptrdiff_t nargs,
						emacs_value args[], void *data) {
  (void)nargs; (void)data;
  GhosttyTerm *t = term_get(env, args[0]);
  if (!t) return Qnil;

  emacs_value vp_start = env->funcall(env, Fpoint, 0, NULL);
  emacs_value pm = env->funcall(env, Fpoint_min, 0, NULL);
  env->funcall(env, Fdelete_region, 2, (emacs_value[]){pm, vp_start});
  return Qnil;
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
  DEFUN("ghostty-vt--new",              Fghostty_vt__new,             3, 3);
  DEFUN("ghostty-vt--write",            Fghostty_vt__write,           2, 2);
  DEFUN("ghostty-vt--render",           Fghostty_vt__render,          1, 1);
  DEFUN("ghostty-vt--encode-key",       Fghostty_vt__encode_key,      5, 5);
  DEFUN("ghostty-vt--resize",           Fghostty_vt__resize,          5, 5);
  DEFUN("ghostty-vt--prepend-history",  Fghostty_vt__prepend_history, 1, 1);
  DEFUN("ghostty-vt--discard-history",  Fghostty_vt__discard_history, 1, 1);
#undef DEFUN
  provide(env, "ghostty-vt-module");
  return 0;
}
