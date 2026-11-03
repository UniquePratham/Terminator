/* config.h — persistent, user-editable settings and key bindings. */
#ifndef TERMINATOR_CONFIG_H
#define TERMINATOR_CONFIG_H

#include <stddef.h>

/* Named actions that can be bound to keys. */
typedef enum {
    ACT_NONE = 0,
    ACT_CURSOR_LEFT,
    ACT_CURSOR_RIGHT,
    ACT_LINE_START,
    ACT_LINE_END,
    ACT_HISTORY_PREV,
    ACT_HISTORY_NEXT,
    ACT_DELETE_TO_START,
    ACT_DELETE_TO_END,
    ACT_DELETE_WORD,
    ACT_CLEAR_SCREEN,
    ACT_EOF,
    ACT_COPY,
    ACT_PASTE,
    ACT_COMPLETE,
    ACT_SEARCH,
    ACT_NEW_TAB,
    ACT_CLOSE_TAB,
    ACT_NEXT_TAB,
    ACT_PREV_TAB,
    ACT_COUNT
} action_t;

typedef struct {
    action_t action;
    char     key[32];   /* canonical key name, e.g. "Ctrl+L", "Up", "Tab" */
} binding_t;

typedef struct config {
    binding_t bindings[ACT_COUNT]; /* one configured key per named action */
    int       scrollback_lines;    /* retained output capacity in the GUI */
    int       multiwatch_interval; /* refresh period, in seconds */
    char     *path;                /* heap copy of the config filename */
} config_t;

/* Load configuration from `path` (created with defaults if missing).
 * Invalid or conflicting bindings are reported to stderr and fall back to
 * defaults. */
config_t *config_load(const char *path);
void      config_free(config_t *c);

/* Look up the action bound to a canonical key name, or ACT_NONE. */
action_t  config_action_for_key(const config_t *c, const char *key);

/* Canonical name for an action (for diagnostics). */
const char *config_action_name(action_t a);

#endif /* TERMINATOR_CONFIG_H */
