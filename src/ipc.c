/* ipc.c — framed message protocol for process mode.
 *
 * Pipes are byte streams, not message queues: one read may return only part
 * of a header/payload. read_all()/write_all() restore message boundaries, and
 * ipc_recv() validates lengths before allocating the payload buffer.
 */
#include "ipc.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IPC_MAX_PAYLOAD (16u * 1024u * 1024u)

static int read_all(int fd, void *buf, size_t n)
{
    char *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            return -1; /* EOF */
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

int ipc_send(int fd, uint32_t type, uint32_t tab_id,
             const void *payload, uint32_t len)
{
    ipc_header_t h = { type, tab_id, len };
    if (write_all(fd, &h, sizeof(h)) != 0)
        return -1;
    if (len > 0 && write_all(fd, payload, len) != 0)
        return -1;
    return 0;
}

int ipc_recv(int fd, uint32_t *type, uint32_t *tab_id,
             char **payload, uint32_t *len)
{
    ipc_header_t h;
    if (read_all(fd, &h, sizeof(h)) != 0)
        return -1;
    if (h.length > IPC_MAX_PAYLOAD)
        return -1; /* reject absurd lengths */
    char *buf = NULL;
    if (h.length > 0) {
        /* IPC string payloads are length-delimited on the wire, but callers
         * pass them to string helpers after receipt. Keep the wire length
         * unchanged while adding a local terminator. */
        buf = xmalloc((size_t)h.length + 1);
        if (read_all(fd, buf, h.length) != 0) {
            free(buf);
            return -1;
        }
        buf[h.length] = '\0';
    }
    *type = h.type;
    *tab_id = h.tab_id;
    *payload = buf;
    *len = h.length;
    return 0;
}
