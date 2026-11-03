/* util.h — checked allocation, dynamic buffers and small string helpers. */
#ifndef TERMINATOR_UTIL_H
#define TERMINATOR_UTIL_H

#include <stddef.h>
#include <stdarg.h>

/* Checked allocation. On failure a diagnostic is printed and the process
 * aborts: allocation failure is unrecoverable for this application. */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* Growable byte buffer. Always NUL-terminated after append operations so it
 * can be used as a C string, but may also contain embedded NULs. */
typedef struct {
    char  *data; /* owned allocation; NULL before first reserve */
    size_t len;  /* bytes in use, not counting the trailing NUL */
    size_t cap;  /* allocated bytes, including room for the trailing NUL */
} buf_t;

void  buf_init(buf_t *b);
void  buf_free(buf_t *b);
void  buf_clear(buf_t *b);
void  buf_reserve(buf_t *b, size_t extra);
void  buf_append(buf_t *b, const void *data, size_t n);
void  buf_append_str(buf_t *b, const char *s);
void  buf_append_char(buf_t *b, char c);
void  buf_appendf(buf_t *b, const char *fmt, ...)
          __attribute__((format(printf, 2, 3)));
/* Detach the buffer contents as a heap string and reset the buffer. */
char *buf_steal(buf_t *b);

/* String helpers. */
int   str_starts_with(const char *s, const char *prefix);
int   str_ends_with(const char *s, const char *suffix);
char *str_trim(char *s);

/* Write the whole buffer, retrying on partial writes and EINTR.
 * Returns 0 on success, -1 on error. */
int   write_all(int fd, const void *buf, size_t n);

#endif /* TERMINATOR_UTIL_H */
