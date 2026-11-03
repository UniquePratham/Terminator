/* config.c — persistent, user-editable settings and key bindings.
 *
 * Defaults are the safe baseline. The config loader applies user bindings one
 * at a time and rejects invalid/conflicting entries rather than leaving an
 * action unbound or making key dispatch ambiguous.
 */
#include "config.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    action_t action;
    const char *name;
    const char *default_key;
} DEFAULTS[] = {
    { ACT_CURSOR_LEFT,    "cursor-left",    "Left" },
    { ACT_CURSOR_RIGHT,   "cursor-right",   "Right" },
    { ACT_LINE_START,     "line-start",     "Ctrl+A" },
    { ACT_LINE_END,       "line-end",       "Ctrl+E" },
    { ACT_HISTORY_PREV,   "history-prev",   "Up" },
    { ACT_HISTORY_NEXT,   "history-next",   "Down" },
    { ACT_DELETE_TO_START,"delete-to-start","Ctrl+U" },
    { ACT_DELETE_TO_END,  "delete-to-end",  "Ctrl+K" },
    { ACT_DELETE_WORD,    "delete-word",    "Ctrl+W" },
    { ACT_CLEAR_SCREEN,   "clear-screen",   "Ctrl+L" },
    { ACT_EOF,            "eof",            "Ctrl+D" },
    { ACT_COPY,           "copy",           "Ctrl+Shift+C" },
    { ACT_PASTE,          "paste",          "Ctrl+Shift+V" },
    { ACT_COMPLETE,       "complete",       "Tab" },
    { ACT_SEARCH,         "search",         "Ctrl+R" },
    { ACT_NEW_TAB,        "new-tab",        "Ctrl+Shift+T" },
    { ACT_CLOSE_TAB,      "close-tab",      "Ctrl+Shift+W" },
    { ACT_NEXT_TAB,       "next-tab",       "Ctrl+PageDown" },
    { ACT_PREV_TAB,       "prev-tab",       "Ctrl+PageUp" },
};

static const char *action_name(action_t a)
{
    for (size_t i = 0; i < sizeof(DEFAULTS) / sizeof(DEFAULTS[0]); i++)
        if (DEFAULTS[i].action == a)
            return DEFAULTS[i].name;
    return "unknown";
}

const char *config_action_name(action_t a) { return action_name(a); }

static action_t action_from_name(const char *name)
{
    for (size_t i = 0; i < sizeof(DEFAULTS) / sizeof(DEFAULTS[0]); i++)
        if (strcmp(DEFAULTS[i].name, name) == 0)
            return DEFAULTS[i].action;
    return ACT_NONE;
}

static int valid_key_name(const char *key)
{
    const char *base = key;
    if (strncmp(base, "Ctrl+Shift+", 11) == 0)
        base += 11;
    else if (strncmp(base, "Ctrl+", 5) == 0 ||
             strncmp(base, "Shift+", 6) == 0)
        base += base[0] == 'C' ? 5 : 6;
    if (!*base)
        return 0;

    static const char *const special[] = {
        "Left", "Right", "Up", "Down", "Tab", "PageUp", "PageDown",
        "Home", "End", "BackSpace", "Return", "Escape", "Delete"
    };
    for (size_t i = 0; i < sizeof(special) / sizeof(special[0]); i++)
        if (strcmp(base, special[i]) == 0)
            return 1;
    return base[1] == '\0' && (unsigned char)base[0] >= 32 &&
           (unsigned char)base[0] < 127;
}

static void set_defaults(config_t *c)
{
    for (size_t i = 0; i < sizeof(DEFAULTS) / sizeof(DEFAULTS[0]); i++) {
        c->bindings[DEFAULTS[i].action].action = DEFAULTS[i].action;
        snprintf(c->bindings[DEFAULTS[i].action].key,
                 sizeof(c->bindings[0].key), "%s", DEFAULTS[i].default_key);
    }
    c->scrollback_lines = 5000;
    c->multiwatch_interval = 2;
}

static void write_default_file(const char *path, const config_t *c)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "# Terminator configuration\n");
    fprintf(f, "# bind <action> = <key>\n");
    fprintf(f, "scrollback = %d\n", c->scrollback_lines);
    fprintf(f, "multiwatch_interval = %d\n", c->multiwatch_interval);
    for (size_t i = 0; i < sizeof(DEFAULTS) / sizeof(DEFAULTS[0]); i++) {
        action_t a = DEFAULTS[i].action;
        fprintf(f, "bind %s = %s\n", DEFAULTS[i].name, c->bindings[a].key);
    }
    fclose(f);
}

config_t *config_load(const char *path)
{
    config_t *c = xcalloc(1, sizeof(*c));
    c->path = xstrdup(path);
    set_defaults(c);

    FILE *f = fopen(path, "r");
    if (!f) {
        write_default_file(path, c);
        return c;
    }

    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *s = str_trim(line);
        if (*s == '\0' || *s == '#')
            continue;
        if (str_starts_with(s, "scrollback")) {
            char *eq = strchr(s, '=');
            if (eq) {
                int v = atoi(eq + 1);
                if (v > 0)
                    c->scrollback_lines = v;
            }
            continue;
        }
        if (str_starts_with(s, "multiwatch_interval")) {
            char *eq = strchr(s, '=');
            if (eq) {
                int v = atoi(eq + 1);
                if (v > 0)
                    c->multiwatch_interval = v;
            }
            continue;
        }
        if (str_starts_with(s, "bind")) {
            char *rest = str_trim(s + 4);
            char *eq = strchr(rest, '=');
            if (!eq) {
                fprintf(stderr, "terminator: %s:%d: malformed bind line\n",
                        path, lineno);
                continue;
            }
            *eq = '\0';
            char *aname = str_trim(rest);
            char *key = str_trim(eq + 1);
            action_t a = action_from_name(aname);
            if (a == ACT_NONE) {
                fprintf(stderr, "terminator: %s:%d: unknown action '%s'\n",
                        path, lineno, aname);
                continue;
            }
            if (*key == '\0') {
                fprintf(stderr, "terminator: %s:%d: empty key for '%s'\n",
                        path, lineno, aname);
                continue;
            }
            if (!valid_key_name(key)) {
                fprintf(stderr, "terminator: %s:%d: invalid key '%s'; "
                        "keeping default for '%s'\n",
                        path, lineno, key, aname);
                continue;
            }
            /* Detect conflicts: another action already using this key. */
            int conflict = 0;
            for (int i = 0; i < ACT_COUNT; i++) {
                if ((action_t)i != a && c->bindings[i].action &&
                    strcmp(c->bindings[i].key, key) == 0) {
                    fprintf(stderr,
                            "terminator: %s:%d: key '%s' already bound to '%s'; "
                            "keeping default for '%s'\n",
                            path, lineno, key, action_name((action_t)i), aname);
                    conflict = 1;
                    break;
                }
            }
            if (conflict)
                continue;
            snprintf(c->bindings[a].key, sizeof(c->bindings[a].key), "%s", key);
            continue;
        }
        fprintf(stderr, "terminator: %s:%d: unrecognised directive\n",
                path, lineno);
    }
    fclose(f);
    return c;
}

void config_free(config_t *c)
{
    if (!c)
        return;
    free(c->path);
    free(c);
}

action_t config_action_for_key(const config_t *c, const char *key)
{
    for (int i = 0; i < ACT_COUNT; i++) {
        if (c->bindings[i].action && strcmp(c->bindings[i].key, key) == 0)
            return c->bindings[i].action;
    }
    return ACT_NONE;
}
