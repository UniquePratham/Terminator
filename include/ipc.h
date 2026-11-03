/* ipc.h — framed message protocol for process mode.
 *
 * Every message is a fixed 12-byte header (type, tab id, payload length)
 * followed by `length` payload bytes. Reads and writes may be partial, so the
 * helpers below loop until the whole frame is transferred.
 */
#ifndef TERMINATOR_IPC_H
#define TERMINATOR_IPC_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    IPC_OUTPUT = 1,   /* worker -> GUI: command output bytes */
    IPC_PROMPT,       /* worker -> GUI: prompt is ready */
    IPC_EXIT,         /* worker -> GUI: worker exiting */
    IPC_INPUT,        /* GUI -> worker: a line of input */
    IPC_SIGNAL,       /* GUI -> worker: deliver a signal (payload = signo) */
    IPC_CWD,          /* worker -> GUI: current directory changed */
    IPC_FG_PGID       /* worker -> GUI: foreground process group changed */
} ipc_type_t;

typedef struct {
    uint32_t type;   /* one ipc_type_t value, stored as a fixed-width integer */
    uint32_t tab_id; /* identifies which tab owns this message */
    uint32_t length; /* payload bytes immediately following this header */
} ipc_header_t;

/* Send a framed message. Returns 0 on success, -1 on error. */
int ipc_send(int fd, uint32_t type, uint32_t tab_id,
             const void *payload, uint32_t len);

/* Receive a framed message. On success returns 0 and sets *type, *tab_id and
 * *payload (a heap buffer the caller frees; may be NULL when len is 0).
 * Returns -1 on error/EOF. */
int ipc_recv(int fd, uint32_t *type, uint32_t *tab_id,
             char **payload, uint32_t *len);

#endif /* TERMINATOR_IPC_H */
