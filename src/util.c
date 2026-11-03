/* util.c — checked allocation, dynamic buffers and small string helpers.
 *
 * These helpers keep ownership rules consistent across modules: allocation
 * failures stop immediately, buffers grow geometrically and remain
 * NUL-terminated, and write_all() handles partial writes and EINTR.
 */
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void oom(size_t n)
{
    fprintf(stderr, "terminator: out of memory allocating %zu bytes\n", n);
    abort();
}

void *xmalloc(size_t n)
{
    if (n == 0)
        n = 1;
    void *p = malloc(n);
    if (!p)
        oom(n);
    return p;
}

void *xcalloc(size_t n, size_t sz)
{
    if (n == 0 || sz == 0) {
        n = 1;
        sz = 1;
    }
    void *p = calloc(n, sz);
    if (!p)
        oom(n * sz);
    return p;
}

void *xrealloc(void *p, size_t n)
{
    if (n == 0)
        n = 1;
    void *q = realloc(p, n);
    if (!q)
        oom(n);
    return q;
}

char *xstrdup(const char *s)
{
    if (!s)
        return NULL;
    size_t n = strlen(s);
    char *p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    size_t len = 0;
    while (len < n && s[len])
        len++;
    char *p = xmalloc(len + 1);
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

void buf_init(buf_t *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(buf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_clear(buf_t *b)
{
    b->len = 0;
    if (b->data)
        b->data[0] = '\0';
}

void buf_reserve(buf_t *b, size_t extra)
{
    /* Keep one spare byte for the terminating NUL. */
    size_t need = b->len + extra + 1;
    if (need <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 32;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void buf_append(buf_t *b, const void *data, size_t n)
{
    if (n == 0)
        return;
    buf_reserve(b, n);
    memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_append_str(buf_t *b, const char *s)
{
    if (s)
        buf_append(b, s, strlen(s));
}

void buf_append_char(buf_t *b, char c)
{
    buf_reserve(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

void buf_appendf(buf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    buf_reserve(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

char *buf_steal(buf_t *b)
{
    if (!b->data) {
        char *p = xmalloc(1);
        p[0] = '\0';
        return p;
    }
    char *p = b->data;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return p;
}

int str_starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

int str_ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls)
        return 0;
    return strcmp(s + ls - lf, suffix) == 0;
}

char *str_trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    size_t n = strlen(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            s[--n] = '\0';
        else
            break;
    }
    return s;
}

int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}
