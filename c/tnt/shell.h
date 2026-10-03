#ifndef _TNT_SHELL_H_
#define _TNT_SHELL_H_

#include "net.h"
#include "types.h"

#define LINE_CAP 128

void shell_banner(TcpSocket_T *sock);
void shell_prompt(TcpSocket_T *sock);

/* Sends what the shell has written since the last flush.  Call it once a
 * command and its prompt are done, and before closing the connection. */
void shell_flush(void);

/* Returns 1 if the connection should be closed (exit/quit), 0 otherwise. */
int shell_dispatch(TcpSocket_T *sock, TcpSocket_T sockets[MAX_SOCKETS], uint8_t *line, uint8_t len);

#endif
