/* lexer.h — quote-aware tokenizer for the Terminator shell.
 *
 * The lexer never uses strtok(). It produces explicit tokens; a WORD token
 * carries a list of segments so that quoting, variable references and command
 * substitutions survive into the expansion phase with their exact meaning.
 */
#ifndef TERMINATOR_LEXER_H
#define TERMINATOR_LEXER_H

#include <stddef.h>

/* A piece of a word. */
typedef enum {
    SEG_LITERAL,   /* literal text (may have come from quotes) */
    SEG_VAR,       /* $NAME / ${NAME}; text is the variable name */
    SEG_CMDSUB     /* $(...) or `...`; text is the inner command */
} seg_type_t;

typedef struct {
    seg_type_t type;
    char      *text;
    int        quoted;   /* literal came from quotes: suppress globbing */
} segment_t;

typedef struct {
    segment_t *segs;
    size_t     nsegs;
    size_t     cap;
    int        had_quote; /* any quoting present in the word */
} word_t;

void word_init(word_t *w);
void word_free(word_t *w);
/* Deep-copy `src` into `dst` (dst must be initialised). */
void word_copy(word_t *dst, const word_t *src);
void word_add_seg(word_t *w, seg_type_t type, const char *text, size_t n, int quoted);
/* Append literal text to the last literal segment, or create one. */
void word_add_literal(word_t *w, const char *text, size_t n, int quoted);
/* Render the word with quotes/escapes removed (no expansion). Caller frees. */
char *word_plain(const word_t *w);

typedef enum {
    TOK_WORD,
    TOK_PIPE,        /* |  */
    TOK_REDIR_IN,    /* <  */
    TOK_REDIR_OUT,   /* >  */
    TOK_REDIR_APPEND,/* >> */
    TOK_BG,          /* &  */
    TOK_SEMI,        /* ;  */
    TOK_NEWLINE,
    TOK_EOF
} token_type_t;

typedef struct {
    token_type_t type;
    word_t       word;   /* valid only for TOK_WORD */
    int          line;
} token_t;

typedef struct {
    token_t *toks;
    size_t   ntoks;
    size_t   cap;
} token_list_t;

typedef enum {
    LEX_OK,          /* complete, well-formed line */
    LEX_INCOMPLETE,  /* needs continuation (open quote, trailing \, open $( ) */
    LEX_ERROR        /* malformed syntax */
} lex_status_t;

void token_list_init(token_list_t *tl);
void token_list_free(token_list_t *tl);

/* Tokenize one logical line. On LEX_ERROR, errbuf receives a message. */
lex_status_t lex_line(const char *input, token_list_t *out,
                      char *errbuf, size_t errlen);

#endif /* TERMINATOR_LEXER_H */
