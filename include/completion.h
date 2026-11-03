/* completion.h — filename completion in a tab's logical directory. */
#ifndef TERMINATOR_COMPLETION_H
#define TERMINATOR_COMPLETION_H

#include <stddef.h>

/* Result of a completion attempt. */
typedef enum {
    COMPLETE_NONE,      /* no matches: leave input unchanged */
    COMPLETE_ONE,       /* single match: `replacement` holds the full word */
    COMPLETE_PREFIX,    /* extended to common prefix: `replacement` holds it */
    COMPLETE_AMBIGUOUS  /* multiple matches: `matches` lists candidates */
} complete_result_t;

typedef struct {
    complete_result_t kind; /* tells the editor whether/how input changed */
    char  *replacement;   /* heap string, caller frees (may be NULL) */
    char **matches;       /* heap array of heap strings for AMBIGUOUS */
    size_t nmatches;
} completion_t;

void completion_free(completion_t *c);

/* Complete `word` (the token before the cursor) against entries in `cwd`.
 * `word` may contain a path prefix such as "src/". */
/* The result owns its returned strings; release it with completion_free(). */
completion_t completion_run(const char *cwd, const char *word);

#endif /* TERMINATOR_COMPLETION_H */
