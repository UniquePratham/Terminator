/* completion.c — filename completion in a tab's logical directory.
 *
 * The prefix is split into directory and basename parts. Matching directory
 * entries are sorted; one match completes fully, several matches extend only
 * the common prefix, and the caller can display the remaining candidates.
 */
#include "completion.h"
#include "util.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void completion_free(completion_t *c)
{
    free(c->replacement);
    for (size_t i = 0; i < c->nmatches; i++)
        free(c->matches[i]);
    free(c->matches);
    c->replacement = NULL;
    c->matches = NULL;
    c->nmatches = 0;
}

/* Longest common prefix of a list of strings. */
static char *common_prefix(char **items, size_t n)
{
    if (n == 0)
        return xstrdup("");
    size_t len = strlen(items[0]);
    for (size_t i = 1; i < n; i++) {
        size_t j = 0;
        while (j < len && items[i][j] && items[i][j] == items[0][j])
            j++;
        len = j;
    }
    return xstrndup(items[0], len);
}

static int compare_names(const void *a, const void *b)
{
    const char *const *left = a;
    const char *const *right = b;
    return strcmp(*left, *right);
}

completion_t completion_run(const char *cwd, const char *word)
{
    completion_t res;
    memset(&res, 0, sizeof(res));
    res.kind = COMPLETE_NONE;

    /* Split into directory part and base part. */
    const char *slash = strrchr(word, '/');
    char *dirpart;
    const char *base;
    if (slash) {
        dirpart = xstrndup(word, (size_t)(slash - word) + 1); /* keep '/' */
        base = slash + 1;
    } else {
        dirpart = xstrdup("");
        base = word;
    }

    /* Build the directory to scan. */
    char *scan;
    if (dirpart[0] == '/') {
        scan = xstrdup(dirpart);
    } else {
        buf_t b;
        buf_init(&b);
        buf_append_str(&b, cwd);
        if (b.len == 0 || b.data[b.len - 1] != '/')
            buf_append_char(&b, '/');
        buf_append_str(&b, dirpart);
        scan = buf_steal(&b);
    }

    DIR *d = opendir(scan);
    if (!d) {
        free(dirpart);
        free(scan);
        return res;
    }

    size_t blen = strlen(base);
    char **matches = NULL;
    size_t nm = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (strncmp(de->d_name, base, blen) != 0)
            continue;
        if (nm == cap) {
            cap = cap ? cap * 2 : 8;
            matches = xrealloc(matches, cap * sizeof(char *));
        }
        matches[nm++] = xstrdup(de->d_name);
    }
    closedir(d);

    /* Stable ordering makes numbered choices predictable. */
    if (nm > 1)
        qsort(matches, nm, sizeof(*matches), compare_names);

    if (nm == 0) {
        free(matches);
        free(dirpart);
        free(scan);
        return res;
    }

    if (nm == 1) {
        buf_t b;
        buf_init(&b);
        buf_append_str(&b, dirpart);
        buf_append_str(&b, matches[0]);
        /* Append '/' for directories. */
        buf_t full;
        buf_init(&full);
        buf_append_str(&full, scan);
        buf_append_char(&full, '/');
        buf_append_str(&full, matches[0]);
        struct stat st;
        if (stat(full.data, &st) == 0 && S_ISDIR(st.st_mode))
            buf_append_char(&b, '/');
        buf_free(&full);
        res.kind = COMPLETE_ONE;
        res.replacement = buf_steal(&b);
        for (size_t i = 0; i < nm; i++)
            free(matches[i]);
        free(matches);
    } else {
        char *cp = common_prefix(matches, nm);
        if (strlen(cp) > blen) {
            buf_t b;
            buf_init(&b);
            buf_append_str(&b, dirpart);
            buf_append_str(&b, cp);
            res.kind = COMPLETE_PREFIX;
            res.replacement = buf_steal(&b);
            for (size_t i = 0; i < nm; i++)
                free(matches[i]);
            free(matches);
        } else {
            res.kind = COMPLETE_AMBIGUOUS;
            res.matches = matches;
            res.nmatches = nm;
        }
        free(cp);
    }

    free(dirpart);
    free(scan);
    return res;
}
