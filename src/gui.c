/* gui.c — X11 window, event loop and rendering.
 *
 * This is the only module that calls Xlib. The event loop multiplexes the X
 * connection and every tab's worker pipe with poll(), so the GUI never blocks
 * while a command runs and hidden tabs keep collecting output.
 */
#include "gui.h"
#include "completion.h"
#include "config.h"
#include "history.h"
#include "tab.h"
#include "util.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Wrapped-line model                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    char   **lines;
    size_t  *offs;   /* absolute offset of each line's first character */
    size_t   n;
    size_t   cap;
} lines_t;

static void lines_add(lines_t *l, const char *s, size_t n, size_t off)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->lines = xrealloc(l->lines, l->cap * sizeof(*l->lines));
        l->offs = xrealloc(l->offs, l->cap * sizeof(*l->offs));
    }
    l->lines[l->n] = xstrndup(s, n);
    l->offs[l->n] = off;
    l->n++;
}

static void lines_free(lines_t *l)
{
    for (size_t i = 0; i < l->n; i++)
        free(l->lines[i]);
    free(l->lines);
    free(l->offs);
}

/* Split `text` on newlines and wrap each segment at `cols` columns. */
static void wrap_text(const char *text, size_t len, int cols, lines_t *out)
{
    if (cols < 1)
        cols = 1;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            size_t seglen = i - start;
            if (seglen == 0) {
                lines_add(out, "", 0, start);
            } else {
                /* Wrap by Unicode codepoints so multi-byte UTF-8 characters
                 * are never split across lines. */
                size_t off = 0;
                while (off < seglen) {
                    size_t take = 0;
                    int seen = 0;
                    while (off + take < seglen && seen < cols) {
                        if (((unsigned char)text[start + off + take] & 0xC0)
                            != 0x80)
                            seen++;
                        take++;
                    }
                    while (off + take < seglen &&
                           ((unsigned char)text[start + off + take] & 0xC0)
                           == 0x80)
                        take++;
                    lines_add(out, text + start + off, take, start + off);
                    off += take;
                }
            }
            start = i + 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* GUI state                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    app_t       *app;
    Display     *dpy;
    Window       win;
    GC           gc;           /* filled rectangles only */
    XftFont     *xft;          /* antialiased monospace font */
    XftDraw     *draw;
    int          cw, chh;      /* cell width / height */
    int          font_size;   /* fontconfig point size */
    int          W, H;         /* window size */
    int          rows;         /* visible text rows */
    int          scroll;       /* lines scrolled up from the bottom */
    int          redraw;
    int          hover_tab;    /* tab under the pointer, or -1 */
    int          cursor_visible;
    int          focused;
    unsigned long long cursor_blink_at_ms;

    /* Theme colors. */
    XftColor     c_bg, c_fg, c_accent, c_green, c_sky, c_sky_dim, c_yellow;
    XftColor     c_dim, c_surface, c_bar;
    XftColor     ansi[16];

    /* Ctrl+R search prompt. */
    int          search_mode;
    char         search[256];
    size_t       search_len;

    /* Clipboard. */
    Atom         clipboard;
    Atom         utf8;
    Atom         paste_prop;
    char        *clip_data;

    Atom         wm_delete;
} gui_t;

/* ------------------------------------------------------------------ */
/* Drawing helpers                                                    */
/* ------------------------------------------------------------------ */

static int text_width(gui_t *g, const char *s, size_t n)
{
    if (n == 0)
        return 0;
    XGlyphInfo ext;
    XftTextExtentsUtf8(g->dpy, g->xft, (const FcChar8 *)s, (int)n, &ext);
    return ext.xOff;
}

static void draw_text(gui_t *g, const XftColor *c, int x, int y,
                      const char *s, size_t n)
{
    if (n == 0)
        return;
    XftDrawStringUtf8(g->draw, c, g->xft, x, y, (const FcChar8 *)s, (int)n);
}

static XftFont *open_gui_font(Display *dpy, int screen, int size)
{
    char name[64];
    snprintf(name, sizeof(name), "DejaVu Sans Mono-%d", size);
    XftFont *font = XftFontOpenName(dpy, screen, name);
    if (font)
        return font;
    snprintf(name, sizeof(name), "Ubuntu Mono-%d", size);
    font = XftFontOpenName(dpy, screen, name);
    if (font)
        return font;
    snprintf(name, sizeof(name), "monospace:size=%d", size);
    return XftFontOpenName(dpy, screen, name);
}

static void update_cell_metrics(gui_t *g)
{
    g->cw = g->xft->max_advance_width;
    if (g->cw < 1)
        g->cw = 6;
    g->chh = g->xft->ascent + g->xft->descent + 2;
    if (g->chh < 1)
        g->chh = 13;
}

static void change_font_size(gui_t *g, int delta)
{
    int size = g->font_size + delta;
    if (size < 7 || size > 32 || size == g->font_size)
        return;
    XftFont *font = open_gui_font(g->dpy, DefaultScreen(g->dpy), size);
    if (!font)
        return;
    XftFontClose(g->dpy, g->xft);
    g->xft = font;
    g->font_size = size;
    update_cell_metrics(g);
    g->redraw = 1;
}

/* Height of the tab bar; shared by drawing and click hit-testing. */
static int tabbar_height(gui_t *g)
{
    return g->chh + 10;
}

/* Format one visible tab label from its stable ID, not its current array
 * position. Closing a different tab shifts array indices but must not rename
 * the tabs that remain. The same helper feeds drawing and hit-test sizing. */
static void tab_label(gui_t *g, int i, char *label, size_t label_size)
{
    tab_t *t = app_tab(g->app, i);
    int id = t ? tab_id(t) : i + 1;
    snprintf(label, label_size, "%s %d",
             i == app_active_index(g->app) ? "●" : "○", id);
}

/* Width of tab `i`'s button; shared by drawing and click hit-testing so the
 * two can never drift apart. */
static int tab_tab_width(gui_t *g, int i)
{
    char label[32];
    tab_label(g, i, label, sizeof(label));
    return text_width(g, label, strlen(label)) + 36;
}

/* Place + directly after the tabs that fit in the tab strip. */
static int tabbar_plus_x(gui_t *g)
{
    int x = 8;
    int nt = app_tab_count(g->app);
    for (int i = 0; i < nt; i++) {
        int w = tab_tab_width(g, i);
        if (x + w + 6 + 26 > g->W - 8)
            break;
        x += w + 6;
    }
    return x;
}

static int tabbar_tab_at(gui_t *g, int px, int py)
{
    if (py < 0 || py >= tabbar_height(g))
        return -1;
    int x = 8;
    int nt = app_tab_count(g->app);
    for (int i = 0; i < nt; i++) {
        int w = tab_tab_width(g, i);
        if (x + w > tabbar_plus_x(g) - 6)
            break;
        if (px >= x && px < x + w)
            return i;
        x += w + 6;
    }
    return -1;
}

/* Strip basic ANSI SGR color sequences while recording the foreground color
 * for each displayed byte. Color 16 means the normal theme foreground. */
static void visible_output(const char *src, size_t n, buf_t *out,
                           unsigned char **colors)
{
    *colors = xmalloc(n ? n : 1);
    int color = 16;
    size_t i = 0;
    while (i < n) {
        if ((unsigned char)src[i] == 0x1b && i + 2 < n && src[i + 1] == '[') {
            size_t end = i + 2;
            while (end < n &&
                   ((src[end] >= '0' && src[end] <= '9') || src[end] == ';'))
                end++;
            if (end < n && src[end] == 'm') {
                size_t p = i + 2;
                do {
                    int code = 0, have = 0;
                    while (p < end && src[p] >= '0' && src[p] <= '9') {
                        have = 1;
                        code = code * 10 + src[p++] - '0';
                    }
                    if (!have || code == 0 || code == 39) color = 16;
                    else if (code >= 30 && code <= 37) color = code - 30;
                    else if (code >= 90 && code <= 97) color = code - 90 + 8;
                    if (p < end && src[p] == ';') p++;
                    else break;
                } while (p <= end);
                i = end + 1;
                continue;
            }
        }
        buf_append_char(out, src[i++]);
        (*colors)[out->len - 1] = (unsigned char)color;
    }
}

/* The echoed command in scrollback starts with the same prompt as the live
 * input line. Mark those recorded prompt spans so the cwd stays easy to spot
 * after the command has been submitted. */
static void mark_echoed_prompts(const char *text, size_t len,
                               unsigned char *colors)
{
    size_t line = 0;
    while (line < len) {
        size_t end = line;
        while (end < len && text[end] != '\n')
            end++;

        const char *start = text + line;
        size_t line_len = end - line;
        const char *colon = memchr(start, ':', line_len);
        if (colon && colon > start && memchr(start, '@',
                                              (size_t)(colon - start))) {
            const char *dollar = memchr(colon + 1, '$',
                                         (size_t)(text + end - colon - 1));
            if (dollar && dollar > colon + 1 && dollar + 1 < text + end &&
                dollar[1] == ' ') {
                size_t green_end = (size_t)(colon - text) + 1;
                size_t cyan_end = (size_t)(dollar - text);
                for (size_t i = line; i < green_end; i++)
                    colors[i] = 17;
                for (size_t i = green_end; i < cyan_end; i++)
                    colors[i] = 18;
            }
        }
        line = end + (end < len ? 1 : 0);
    }
}

/* Color class for a character in the output, live prompt, or input. */
static const XftColor *color_at(gui_t *g, size_t off, size_t base,
                                size_t prompt_len, size_t colon_off,
                                size_t dollar_off,
                                const unsigned char *ansi)
{
    if (off < base) {
        if (ansi && ansi[off] == 17)
            return &g->c_green;
        if (ansi && ansi[off] == 18)
            return &g->c_sky_dim;
        return ansi && ansi[off] < 16 ? &g->ansi[ansi[off]] : &g->c_fg;
    }
    if (off >= base + prompt_len)
        return &g->c_fg;
    size_t k = off - base;
    if (colon_off >= prompt_len)
        return &g->c_yellow;          /* "> " continuation prompt */
    if (k <= colon_off)
        return &g->c_green;           /* user@host: */
    if (k < dollar_off)
        return &g->c_sky;             /* cwd */
    return &g->c_fg;                  /* "$ " */
}

/* ------------------------------------------------------------------ */
/* Key naming                                                         */
/* ------------------------------------------------------------------ */

/* Produce the canonical key name used by the config file, e.g. "Left",
 * "Ctrl+A", "Ctrl+Shift+C", "Tab", "a". */
static void key_name(gui_t *g, XKeyEvent *ev, char *out, size_t n)
{
    int ctrl = (ev->state & ControlMask) != 0;
    int shift = (ev->state & ShiftMask) != 0;
    /* XLookupKeysym(column 0/1) ignores the Lock modifier. Ask Xlib to
     * translate the full event instead so Caps Lock and the active keyboard
     * layout both affect the character delivered to the editor. */
    unsigned int lookup_mods = ev->state & ~ControlMask;
    char translated[32];
    KeySym ks = NoSymbol;
    XkbLookupKeySym(g->dpy, ev->keycode, lookup_mods, NULL, &ks);
    /* Some XKB layouts report the navigation keypad names KP_Home/KP_End
     * instead of Home/End. Normalize the active symbol (and the base level
     * for dedicated navigation keys), without stealing KP_7/KP_1 digits when
     * Num Lock is on. */
    KeySym level0 = XLookupKeysym(ev, 0);
    if (ks == XK_KP_Home)
        ks = XK_Home;
    else if (ks == XK_KP_End)
        ks = XK_End;
    else if (ks != XK_Home && ks != XK_End) {
        if (level0 == XK_Home)
            ks = XK_Home;
        else if (level0 == XK_End)
            ks = XK_End;
    }
    XKeyEvent lookup = *ev;
    lookup.state = lookup_mods;
    int translated_len = XLookupString(&lookup, translated,
                                       sizeof(translated) - 1, NULL, NULL);
    if (ks == XK_ISO_Left_Tab)
        ks = XK_Tab;

    out[0] = '\0';

    const char *special = NULL;
    switch (ks) {
    case XK_Left:      special = "Left"; break;
    case XK_Right:     special = "Right"; break;
    case XK_Up:        special = "Up"; break;
    case XK_Down:      special = "Down"; break;
    case XK_Tab:       special = "Tab"; break;
    case XK_Page_Up:   special = "PageUp"; break;
    case XK_Page_Down: special = "PageDown"; break;
    case XK_Home:      special = "Home"; break;
    case XK_End:       special = "End"; break;
    case XK_Insert:    special = "Insert"; break;
    case XK_BackSpace: special = "BackSpace"; break;
    case XK_Return:    special = "Return"; break;
    case XK_Escape:    special = "Escape"; break;
    case XK_Delete:    special = "Delete"; break;
    default: break;
    }
    if (special) {
        if (ctrl && shift)      snprintf(out, n, "Ctrl+Shift+%s", special);
        else if (ctrl)          snprintf(out, n, "Ctrl+%s", special);
        else if (shift)         snprintf(out, n, "Shift+%s", special);
        else                    snprintf(out, n, "%s", special);
        return;
    }

    if (ks >= XK_a && ks <= XK_z) {
        char c = (char)('A' + (ks - XK_a));
        if (ctrl && shift)      snprintf(out, n, "Ctrl+Shift+%c", c);
        else if (ctrl)          snprintf(out, n, "Ctrl+%c", c);
        else if (shift)         snprintf(out, n, "%c", c);
        else                    snprintf(out, n, "%c", (char)ks);
        return;
    }
    if (ks >= XK_A && ks <= XK_Z) {
        char c = (char)ks;
        if (ctrl && shift)      snprintf(out, n, "Ctrl+Shift+%c", c);
        else if (ctrl)          snprintf(out, n, "Ctrl+%c", c);
        else                    snprintf(out, n, "%c", c);
        return;
    }

    /* Control can turn printable keysyms into control bytes. Look up the
     * printable character without Control, then keep the modifier in its name. */
    if (translated_len > 0 && translated_len < (int)sizeof(translated)) {
        translated[translated_len] = '\0';
        if (ctrl && shift)
            snprintf(out, n, "Ctrl+Shift+%s", translated);
        else if (ctrl)
            snprintf(out, n, "Ctrl+%s", translated);
        else
            snprintf(out, n, "%s", translated);
    }
}

/* ------------------------------------------------------------------ */
/* Clipboard                                                          */
/* ------------------------------------------------------------------ */

static void do_copy(gui_t *g)
{
    tab_t *t = app_active_tab(g->app);
    if (!t)
        return;
    const char *in = tab_input(t);
    if (!in || !*in)
        return;
    free(g->clip_data);
    g->clip_data = xstrdup(in);
    XSetSelectionOwner(g->dpy, g->clipboard, g->win, CurrentTime);
}

static void do_paste(gui_t *g)
{
    g->paste_prop = XInternAtom(g->dpy, "TERMINATOR_PASTE", False);
    XConvertSelection(g->dpy, g->clipboard, XA_STRING, g->paste_prop,
                      g->win, CurrentTime);
}

static unsigned long long monotonic_millis(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)ts.tv_nsec / 1000000ULL;
}

static void handle_selection_request(gui_t *g, XSelectionRequestEvent *req)
{
    XSelectionEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SelectionNotify;
    ev.display = req->display;
    ev.requestor = req->requestor;
    ev.selection = req->selection;
    ev.target = req->target;
    ev.property = None;
    ev.time = req->time;

    if (req->target == XA_STRING || req->target == g->utf8) {
        Atom prop = req->property ? req->property : req->target;
        const char *data = g->clip_data ? g->clip_data : "";
        XChangeProperty(g->dpy, req->requestor, prop, req->target, 8,
                        PropModeReplace, (const unsigned char *)data,
                        (int)strlen(data));
        ev.property = prop;
    }
    XSendEvent(g->dpy, req->requestor, False, 0, (XEvent *)&ev);
    XFlush(g->dpy);
}

static void handle_selection_notify(gui_t *g, XSelectionEvent *ev)
{
    if (ev->property == None)
        return;
    Atom type;
    int format;
    unsigned long nitems, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(g->dpy, g->win, ev->property, 0, 65536, True,
                           AnyPropertyType, &type, &format, &nitems, &after,
                           &data) == Success && data) {
        tab_t *t = app_active_tab(g->app);
        if (t) {
            char *s = xmalloc(nitems + 1);
            size_t k = 0;
            for (unsigned long i = 0; i < nitems; i++)
                if (data[i] != '\n' && data[i] != '\r')
                    s[k++] = (char)data[i];
            s[k] = '\0';
            tab_input_insert(t, s);
            free(s);
        }
        XFree(data);
    }
    g->redraw = 1;
}

/* ------------------------------------------------------------------ */
/* Input handling                                                     */
/* ------------------------------------------------------------------ */

static void close_tab(gui_t *g, int index)
{
    if (app_tab_count(g->app) <= 1)
        app_quit(g->app);
    else
        app_close_tab(g->app, index);
    g->hover_tab = -1;
    g->scroll = 0;
    g->redraw = 1;
}

static void close_active_tab(gui_t *g)
{
    close_tab(g, app_active_index(g->app));
}

/* All UI paths use one function to create tabs. The app layer enforces the
 * limit; this helper turns a rejected request into visible feedback. */
static void create_tab(gui_t *g)
{
    g->scroll = 0;
    if (!app_new_tab(g->app)) {
        tab_t *t = app_active_tab(g->app);
        const char *message = "Maximum of 16 tabs reached.\n";
        if (t)
            tab_output_append(t, message, strlen(message));
    } else {
        g->scroll = 0;
    }
    g->redraw = 1;
}

static void handle_search_key(gui_t *g, const char *name)
{
    if (strcmp(name, "Return") == 0) {
        g->search_mode = 0;
        size_t count = 0;
        char **matches = history_search_matches(app_history(g->app),
                                                g->search, &count);
        tab_t *t = app_active_tab(g->app);
        if (count > 0) {
            buf_t output;
            buf_init(&output);
            for (size_t i = 0; i < count; i++) {
                buf_append_str(&output, matches[i]);
                buf_append_char(&output, '\n');
                free(matches[i]);
            }
            if (t)
                tab_output_append(t, output.data, output.len);
            buf_free(&output);
            free(matches);
        } else if (t) {
            const char *msg = "No match for search term in history\n";
            tab_output_append(t, msg, strlen(msg));
        }
        g->search_len = 0;
        g->search[0] = '\0';
    } else if (strcmp(name, "Escape") == 0) {
        g->search_mode = 0;
        g->search_len = 0;
        g->search[0] = '\0';
    } else if (strcmp(name, "BackSpace") == 0) {
        if (g->search_len > 0)
            g->search[--g->search_len] = '\0';
    } else if (name[0] && !name[1] && name[0] >= 32 && name[0] < 127) {
        if (g->search_len + 1 < sizeof(g->search)) {
            g->search[g->search_len++] = name[0];
            g->search[g->search_len] = '\0';
        }
    }
    g->redraw = 1;
}

static void handle_key(gui_t *g, XKeyEvent *ev)
{
    g->cursor_visible = 1;
    g->cursor_blink_at_ms = monotonic_millis() + 500;
    if (ev->state & ControlMask) {
        KeySym unshifted = XLookupKeysym(ev, 0);
        KeySym shifted = XLookupKeysym(ev, 1);
        if (unshifted == XK_plus || unshifted == XK_equal ||
            shifted == XK_plus || unshifted == XK_KP_Add) {
            change_font_size(g, 1);
            return;
        }
        if (unshifted == XK_minus || shifted == XK_underscore ||
            unshifted == XK_KP_Subtract) {
            change_font_size(g, -1);
            return;
        }
    }

    char name[64];
    key_name(g, ev, name, sizeof(name));

    if (getenv("TERMINATOR_KEY_DEBUG")) {
        fprintf(stderr, "KEYDBG keycode=%u state=0x%x name='%s'\n",
                ev->keycode, ev->state, name);
    }

    if (g->search_mode) {
        handle_search_key(g, name);
        return;
    }

    tab_t *t = app_active_tab(g->app);
    if (!t)
        return;

    /* Job-control signals are not configurable bindings. */
    if (strcmp(name, "Ctrl+C") == 0) {
        tab_send_signal(t, SIGINT);
        return;
    }
    if (strcmp(name, "Ctrl+Z") == 0) {
        tab_send_signal(t, SIGTSTP);
        return;
    }

    /* Scrolling. */
    if (strcmp(name, "Shift+PageUp") == 0) {
        g->scroll += g->rows > 0 ? g->rows : 10;
        g->redraw = 1;
        return;
    }
    if (strcmp(name, "Shift+PageDown") == 0) {
        g->scroll -= g->rows > 0 ? g->rows : 10;
        if (g->scroll < 0)
            g->scroll = 0;
        g->redraw = 1;
        return;
    }

    /* Home/End are common aliases for the configured Ctrl+A/Ctrl+E actions. */
    if (strcmp(name, "Home") == 0) {
        tab_cursor_home(t);
        g->redraw = 1;
        return;
    }
    if (strcmp(name, "End") == 0) {
        tab_cursor_end(t);
        g->redraw = 1;
        return;
    }
    if (strcmp(name, "Insert") == 0) {
        tab_toggle_insert_mode(t);
        g->redraw = 1;
        return;
    }

    action_t act = config_action_for_key(app_config(g->app), name);
    switch (act) {
    case ACT_CURSOR_LEFT:     tab_cursor_left(t); break;
    case ACT_CURSOR_RIGHT:    tab_cursor_right(t); break;
    case ACT_LINE_START:      tab_cursor_home(t); break;
    case ACT_LINE_END:        tab_cursor_end(t); break;
    case ACT_HISTORY_PREV:    tab_history_prev(t); break;
    case ACT_HISTORY_NEXT:    tab_history_next(t); break;
    case ACT_DELETE_TO_START: tab_delete_to_start(t); break;
    case ACT_DELETE_TO_END:   tab_delete_to_end(t); break;
    case ACT_DELETE_WORD:     tab_delete_word(t); break;
    case ACT_CLEAR_SCREEN:    tab_output_clear(t); break;
    case ACT_COPY:            do_copy(g); break;
    case ACT_PASTE:           do_paste(g); break;
    case ACT_COMPLETE:        tab_complete(t); break;
    case ACT_SEARCH:
        g->search_mode = 1;
        g->search_len = 0;
        g->search[0] = '\0';
        break;
    case ACT_NEW_TAB:
        create_tab(g);
        break;
    case ACT_CLOSE_TAB:
        close_active_tab(g);
        return;
    case ACT_NEXT_TAB:
        if (app_tab_count(g->app) > 0) {
            app_set_active(g->app,
                (app_active_index(g->app) + 1) % app_tab_count(g->app));
            g->scroll = 0;
        }
        break;
    case ACT_PREV_TAB:
        if (app_tab_count(g->app) > 0) {
            int n = app_tab_count(g->app);
            app_set_active(g->app,
                (app_active_index(g->app) - 1 + n) % n);
            g->scroll = 0;
        }
        break;
    case ACT_EOF:
        if (tab_input(t)[0] == '\0') {
            close_active_tab(g);
            return;
        }
        break;
    case ACT_NONE:
    default:
        if (strcmp(name, "Return") == 0) {
            tab_submit(t);
            g->scroll = 0;
        } else if (strcmp(name, "BackSpace") == 0 ||
                   strcmp(name, "Shift+BackSpace") == 0) {
            tab_input_backspace(t);
        } else if (strcmp(name, "Ctrl+BackSpace") == 0 ||
                   strcmp(name, "Ctrl+Shift+BackSpace") == 0) {
            tab_delete_word(t);
        } else if (strcmp(name, "Delete") == 0) {
            tab_input_delete(t);
        } else if (strcmp(name, "Escape") == 0) {
            tab_clear_input(t);
        } else if (name[0] && !name[1] && name[0] >= 32 && name[0] < 127) {
            tab_input_insert(t, name);
        }
        break;
    }
    g->redraw = 1;
}

static void handle_button(gui_t *g, XButtonEvent *ev)
{
    /* Ctrl+wheel changes the font; the unmodified wheel remains scrollback. */
    if ((ev->state & ControlMask) &&
        (ev->button == Button4 || ev->button == Button5)) {
        change_font_size(g, ev->button == Button4 ? 1 : -1);
        return;
    }
    if (ev->button == Button4) {
        g->scroll += 3;
        g->redraw = 1;
    } else if (ev->button == Button5) {
        g->scroll -= 3;
        if (g->scroll < 0)
            g->scroll = 0;
        g->redraw = 1;
    } else if (ev->button == Button1 && ev->y < tabbar_height(g)) {
        if (ev->x >= tabbar_plus_x(g) && ev->x < tabbar_plus_x(g) + 26) {
            create_tab(g);
            return;
        }
        int x = 8;
        int nt = app_tab_count(g->app);
        for (int i = 0; i < nt; i++) {
            int w = tab_tab_width(g, i);
            if (x + w > tabbar_plus_x(g) - 6)
                break;
            if (ev->x >= x && ev->x < x + w) {
                if (ev->x >= x + w - 22) {
                    app_set_active(g->app, i);
                    close_tab(g, i);
                    return;
                }
                app_set_active(g->app, i);
                g->scroll = 0;
                g->redraw = 1;
                break;
            }
            x += w + 6;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Rendering                                                          */
/* ------------------------------------------------------------------ */

static void draw_tabbar(gui_t *g, int tabbar_h)
{
    int x = 8;
    int nt = app_tab_count(g->app);
    int active = app_active_index(g->app);
    int ty = (tabbar_h - g->chh) / 2 + g->xft->ascent;
    for (int i = 0; i < nt; i++) {
        char label[32];
        tab_label(g, i, label, sizeof(label));
        size_t n = strlen(label);
        int w = tab_tab_width(g, i);
        if (x + w > tabbar_plus_x(g) - 6)
            break;
        if (i == active) {
            XSetForeground(g->dpy, g->gc, g->c_accent.pixel);
            XFillRectangle(g->dpy, g->win, g->gc, x, 3, w, tabbar_h - 6);
            draw_text(g, &g->c_bg, x + 8, ty, label, n);
        } else {
            XSetForeground(g->dpy, g->gc, g->c_surface.pixel);
            XFillRectangle(g->dpy, g->win, g->gc, x, 3, w, tabbar_h - 6);
            draw_text(g, &g->c_dim, x + 8, ty, label, n);
        }
        if (i == g->hover_tab) {
            int cx = x + w - 21;
            int cy = (tabbar_h - 16) / 2;
            XSetForeground(g->dpy, g->gc, g->c_surface.pixel);
            XFillArc(g->dpy, g->win, g->gc, cx, cy, 16, 16,
                     0, 360 * 64);
            const char close_mark[] = "×";
            int mark_w = text_width(g, close_mark, strlen(close_mark));
            draw_text(g, &g->c_fg, cx + (16 - mark_w) / 2, ty,
                      close_mark, strlen(close_mark));
        }
        x += w + 6;
    }

    /* + follows the visible tabs and stays in view as the strip fills. */
    int px = tabbar_plus_x(g);
    int py = (tabbar_h - 26) / 2;
    XSetForeground(g->dpy, g->gc, g->c_surface.pixel);
    XFillArc(g->dpy, g->win, g->gc, px, py, 26, 26, 0, 360 * 64);
    const char plus[] = "+";
    int plus_width = text_width(g, plus, 1);
    draw_text(g, &g->c_fg, px + (26 - plus_width) / 2,
              (tabbar_h - g->chh) / 2 + g->xft->ascent,
              plus, 1);
}

static void redraw(gui_t *g)
{
    /* Rendering is a pure view of app/tab state: calculate cell geometry,
     * paint tabs and scrollback, then draw the live prompt/input/cursor. The
     * worker never calls Xlib; this event-loop thread owns all drawing. */
    int W = g->W, H = g->H;
    int cw = g->cw, chh = g->chh;
    int tabbar_h = tabbar_height(g);
    int rows = (H - tabbar_h) / chh;
    if (rows < 1)
        rows = 1;
    int cols = W / cw;
    if (cols < 1)
        cols = 1;
    g->rows = rows;

    XSetForeground(g->dpy, g->gc, g->c_bg.pixel);
    XFillRectangle(g->dpy, g->win, g->gc, 0, 0, W, H);

    XSetForeground(g->dpy, g->gc, g->c_bar.pixel);
    XFillRectangle(g->dpy, g->win, g->gc, 0, 0, W, tabbar_h);

    draw_tabbar(g, tabbar_h);

    tab_t *t = app_active_tab(g->app);
    if (!t) {
        XFlush(g->dpy);
        return;
    }

    /* Compose output + prompt + input into one text blob. */
    buf_t content;
    buf_init(&content);
    unsigned char *ansi = NULL;
    size_t output_len = 0;
    char *output_snapshot = tab_output_snapshot(t, &output_len);
    visible_output(output_snapshot, output_len, &content, &ansi);
    mark_echoed_prompts(content.data ? content.data : "", content.len, ansi);
    free(output_snapshot);
    const char *completion_display = tab_completion_display(t);
    size_t completion_len = strlen(completion_display);
    if (completion_len) {
        size_t old_len = content.len;
        buf_append(&content, completion_display, completion_len);
        ansi = xrealloc(ansi, content.len ? content.len : 1);
        memset(ansi + old_len, 16, completion_len);
    }
    size_t base = content.len;
    const char *prompt = tab_command_running(t) ? "" : tab_prompt(t);
    buf_append_str(&content, prompt);
    size_t prompt_len = strlen(prompt);
    buf_append_str(&content, tab_input(t));
    size_t cursor_off = base + prompt_len + tab_cursor(t);

    /* Prompt coloring: find the ':' and '$' inside the prompt segment. */
    size_t colon_off = prompt_len, dollar_off = prompt_len;
    for (size_t k = 0; k < prompt_len; k++) {
        if (prompt[k] == ':' && colon_off == prompt_len)
            colon_off = k;
        if (prompt[k] == '$' && dollar_off == prompt_len)
            dollar_off = k;
    }

    lines_t lines;
    memset(&lines, 0, sizeof(lines));
    wrap_text(content.data ? content.data : "", content.len, cols, &lines);

    int max_scroll = (int)lines.n - rows;
    if (max_scroll < 0)
        max_scroll = 0;
    if (g->scroll > max_scroll)
        g->scroll = max_scroll;
    if (g->scroll < 0)
        g->scroll = 0;

    int first = (int)lines.n - rows - g->scroll;
    if (first < 0)
        first = 0;

    for (int r = 0; r < rows; r++) {
        int li = first + r;
        if (li < 0 || (size_t)li >= lines.n)
            continue;
        const char *s = lines.lines[li];
        size_t slen = strlen(s);
        int y = tabbar_h + r * chh + g->xft->ascent;
        int x = 8;
        size_t pos = 0;
        while (pos < slen) {
            size_t run = 1;
            const XftColor *c = color_at(g, lines.offs[li] + pos, base,
                                         prompt_len, colon_off, dollar_off,
                                         ansi);
            while (pos + run < slen &&
                   color_at(g, lines.offs[li] + pos + run, base, prompt_len,
                            colon_off, dollar_off, ansi) == c)
                run++;
            draw_text(g, c, x, y, s + pos, run);
            x += text_width(g, s + pos, run);
            pos += run;
        }
    }

    /* Blink the input cursor while the window is focused. */
    {
        size_t cl = 0;
        for (size_t i = 0; i < lines.n; i++)
            if (lines.offs[i] <= cursor_off)
                cl = i;
        int ccol = (int)(cursor_off - lines.offs[cl]);
        int crow = (int)cl - first;
        if (getenv("TERMINATOR_KEY_DEBUG"))
            fprintf(stderr,
                    "CURDBG scroll=%d focused=%d vis=%d off=%zu line=%zu "
                    "col=%d row=%d rows=%d nlines=%zu first=%d\n",
                    g->scroll, g->focused, g->cursor_visible, cursor_off,
                    cl, ccol, crow, rows, lines.n, first);
        if (g->scroll == 0 && g->focused && g->cursor_visible &&
            crow >= 0 && crow < rows) {
            int cx = 8 + ccol * cw;
            int cy = tabbar_h + crow * chh;
            XSetForeground(g->dpy, g->gc, g->c_accent.pixel);
            int cursor_w = tab_insert_mode(t) ? (cw > 2 ? 2 : 1) : cw;
            XFillRectangle(g->dpy, g->win, g->gc, cx, cy,
                           (unsigned)cursor_w, chh);
            if (!tab_insert_mode(t) && ccol < (int)strlen(lines.lines[cl])) {
                const XftColor *c = color_at(g, cursor_off, base, prompt_len,
                                             colon_off, dollar_off, ansi);
                draw_text(g, c, cx, cy + g->xft->ascent,
                          lines.lines[cl] + ccol, 1);
            }
        }
    }

    if (g->search_mode) {
        char sbuf[300];
        snprintf(sbuf, sizeof(sbuf), "Enter search term: %s", g->search);
        XSetForeground(g->dpy, g->gc, g->c_surface.pixel);
        XFillRectangle(g->dpy, g->win, g->gc, 0, H - chh - 8, W, chh + 8);
        draw_text(g, &g->c_yellow, 8, H - 8 - g->xft->descent, sbuf,
                  strlen(sbuf));
    }

    lines_free(&lines);
    free(ansi);
    buf_free(&content);
    XFlush(g->dpy);
}

/* ------------------------------------------------------------------ */
/* Event loop                                                         */
/* ------------------------------------------------------------------ */

/* Xlib's default error handler terminates the process. Selection transfers
 * can race with a requestor that has already gone away (e.g. during shutdown),
 * so log and continue instead of aborting. */
static int x_error_handler(Display *dpy, XErrorEvent *ev)
{
    (void)dpy;
    char buf[128];
    XGetErrorText(dpy, ev->error_code, buf, sizeof(buf));
    fprintf(stderr, "terminator: X error: %s (request %d)\n",
            buf, (int)ev->request_code);
    return 0;
}

int gui_run(app_t *a)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr,
                "terminator: cannot open X display (is DISPLAY set?)\n");
        return 1;
    }
    XSetErrorHandler(x_error_handler);
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    unsigned long black = BlackPixel(dpy, screen);

    gui_t g;
    memset(&g, 0, sizeof(g));
    g.app = a;
    g.dpy = dpy;
    g.hover_tab = -1;
    g.W = 900;
    g.H = 600;
    g.font_size = 10;

    /* Theme (Catppuccin-inspired dark palette). */
    struct { const char *name; XftColor *dst; } theme[] = {
        { "#1e1e2e", &g.c_bg },      /* background */
        { "#cdd6f4", &g.c_fg },      /* text */
        { "#f5c2e7", &g.c_accent },  /* active tab / cursor */
        { "#a6e3a1", &g.c_green },   /* user@host */
        { "#89dceb", &g.c_sky },     /* cwd */
        { "#79a9bf", &g.c_sky_dim }, /* cwd in earlier prompts */
        { "#f9e2af", &g.c_yellow },  /* continuation prompt */
        { "#6c7086", &g.c_dim },     /* inactive tab text */
        { "#313244", &g.c_surface }, /* inactive tab / search bar */
        { "#181825", &g.c_bar },     /* tab bar */
    };
    Colormap cmap = DefaultColormap(dpy, screen);
    Visual *vis = DefaultVisual(dpy, screen);
    for (size_t i = 0; i < sizeof(theme) / sizeof(theme[0]); i++) {
        XRenderColor rc;
        if (!XRenderParseColor(dpy, (char *)theme[i].name, &rc)) {
            fprintf(stderr, "terminator: bad theme color %s\n",
                    theme[i].name);
            return 1;
        }
        if (!XftColorAllocValue(dpy, vis, cmap, &rc, theme[i].dst)) {
            fprintf(stderr, "terminator: cannot allocate color %s\n",
                    theme[i].name);
            return 1;
        }
    }

    /* Standard and bright ANSI foreground colors, styled to match the UI. */
    static const char *ansi_hex[16] = {
        "#45475a", "#f38ba8", "#a6e3a1", "#f9e2af",
        "#89b4fa", "#f5c2e7", "#94e2d5", "#bac2de",
        "#585b70", "#f38ba8", "#a6e3a1", "#f9e2af",
        "#89b4fa", "#f5c2e7", "#94e2d5", "#a6adc8"
    };
    for (size_t i = 0; i < 16; i++) {
        XRenderColor rc;
        if (!XRenderParseColor(dpy, (char *)ansi_hex[i], &rc) ||
            !XftColorAllocValue(dpy, vis, cmap, &rc, &g.ansi[i])) {
            fprintf(stderr, "terminator: cannot allocate ANSI color %s\n",
                    ansi_hex[i]);
            return 1;
        }
    }

    g.win = XCreateSimpleWindow(dpy, root, 0, 0, (unsigned)g.W,
                                (unsigned)g.H, 0, black,
                                g.c_bg.pixel);
    XStoreName(dpy, g.win, "Terminator");
    XSelectInput(dpy, g.win,
                 ExposureMask | KeyPressMask | StructureNotifyMask |
                 ButtonPressMask | PointerMotionMask | LeaveWindowMask |
                 FocusChangeMask);
    g.wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, g.win, &g.wm_delete, 1);
    g.clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    g.utf8 = XInternAtom(dpy, "UTF8_STRING", False);

    g.xft = open_gui_font(dpy, screen, g.font_size);
    if (!g.xft)
        g.xft = XftFontOpenXlfd(dpy, screen, "-*-fixed-medium-r-normal--13-*");
    if (!g.xft) {
        fprintf(stderr, "terminator: no usable fixed font found\n");
        XDestroyWindow(dpy, g.win);
        XCloseDisplay(dpy);
        return 1;
    }
    g.gc = XCreateGC(dpy, g.win, 0, NULL);
    g.draw = XftDrawCreate(dpy, g.win, vis, cmap);
    update_cell_metrics(&g);

    XMapWindow(dpy, g.win);
    XFlush(dpy);
    g.cursor_visible = 1;
    g.focused = 1;
    g.cursor_blink_at_ms = monotonic_millis() + 500;

    if (app_tab_count(a) == 0 && !app_new_tab(a)) {
        fprintf(stderr, "terminator: could not create the initial tab\n");
        XftDrawDestroy(g.draw);
        XftFontClose(dpy, g.xft);
        for (size_t i = 0; i < 16; i++)
            XftColorFree(dpy, vis, cmap, &g.ansi[i]);
        for (size_t i = 0; i < sizeof(theme) / sizeof(theme[0]); i++)
            XftColorFree(dpy, vis, cmap, theme[i].dst);
        XFreeGC(dpy, g.gc);
        XDestroyWindow(dpy, g.win);
        XCloseDisplay(dpy);
        return 1;
    }
    g.redraw = 1;

    int xfd = ConnectionNumber(dpy);

    while (!app_should_quit(a)) {
        /* Drain every tab (hidden tabs keep collecting output). */
        int nt = app_tab_count(a);
        for (int i = 0; i < nt; i++) {
            tab_t *t = app_tab(a, i);
            if (t && tab_drain(t))
                g.redraw = 1;
        }

        /* Handle tabs whose shell requested exit. */
        for (int i = app_tab_count(a) - 1; i >= 0; i--) {
            tab_t *t = app_tab(a, i);
            if (t && tab_exit_requested(t)) {
                if (app_tab_count(a) == 1)
                    app_quit(a);
                else
                    app_close_tab(a, i);
                g.redraw = 1;
            }
        }
        if (app_should_quit(a))
            break;

        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0)
                    g.redraw = 1;
                break;
            case ConfigureNotify:
                g.W = ev.xconfigure.width;
                g.H = ev.xconfigure.height;
                g.redraw = 1;
                break;
            case KeyPress:
                handle_key(&g, &ev.xkey);
                break;
            case FocusIn:
                g.focused = 1;
                g.cursor_visible = 1;
                g.cursor_blink_at_ms = monotonic_millis() + 500;
                g.redraw = 1;
                break;
            case FocusOut:
                g.focused = 0;
                g.redraw = 1;
                break;
            case ButtonPress:
                handle_button(&g, &ev.xbutton);
                break;
            case MotionNotify: {
                int hover = tabbar_tab_at(&g, ev.xmotion.x, ev.xmotion.y);
                if (hover != g.hover_tab) {
                    g.hover_tab = hover;
                    g.redraw = 1;
                }
                break;
            }
            case LeaveNotify:
                if (g.hover_tab != -1) {
                    g.hover_tab = -1;
                    g.redraw = 1;
                }
                break;
            case SelectionRequest:
                handle_selection_request(&g, &ev.xselectionrequest);
                break;
            case SelectionNotify:
                handle_selection_notify(&g, &ev.xselection);
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == g.wm_delete)
                    app_quit(a);
                break;
            default:
                break;
            }
        }
        if (app_should_quit(a))
            break;

        /* Check the deadline each loop, not only when poll times out: worker
         * output can keep poll readable and must not freeze cursor blinking. */
        unsigned long long now_ms = monotonic_millis();
        if (now_ms >= g.cursor_blink_at_ms) {
            g.cursor_visible = !g.cursor_visible;
            g.cursor_blink_at_ms = now_ms + 500;
            if (g.focused)
                g.redraw = 1;
        }

        if (g.redraw) {
            redraw(&g);
            g.redraw = 0;
        }

        /* Wait for X input or tab output. */
        struct pollfd fds[APP_MAX_TABS + 1];
        int nfds = 0;
        fds[nfds].fd = xfd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
        nt = app_tab_count(a);
        for (int i = 0; i < nt && nfds < APP_MAX_TABS + 1; i++) {
            tab_t *t = app_tab(a, i);
            int fd = t ? tab_poll_fd(t) : -1;
            if (fd >= 0) {
                fds[nfds].fd = fd;
                fds[nfds].events = POLLIN;
                fds[nfds].revents = 0;
                nfds++;
            }
        }
        int pr = poll(fds, (nfds_t)nfds, 500);
        if (pr < 0 && errno != EINTR)
            break;
        for (int i = 1; i < nfds; i++)
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR))
                g.redraw = 1;
    }

    free(g.clip_data);
    XftDrawDestroy(g.draw);
    XftFontClose(dpy, g.xft);
    for (size_t i = 0; i < 16; i++)
        XftColorFree(dpy, vis, cmap, &g.ansi[i]);
    for (size_t i = 0; i < sizeof(theme) / sizeof(theme[0]); i++)
        XftColorFree(dpy, vis, cmap, theme[i].dst);
    XFreeGC(dpy, g.gc);
    XDestroyWindow(dpy, g.win);
    XCloseDisplay(dpy);
    return 0;
}
