#ifndef _TNT_XFER_H_
#define _TNT_XFER_H_

#include "net.h"
#include "types.h"

/*
 *  File download over a data connection of its own, FTP style.
 *
 *  The telnet stream cannot carry a file: libcr2's TCP never retransmits, so
 *  one frame the NIC drops leaves the peer waiting on a gap forever, whatever
 *  protocol (Kermit, XMODEM, ...) rides on top.  The sender here watches the
 *  peer's ACKs itself and goes back to the first unacknowledged byte when one
 *  goes missing; the file is re-read from disk for that, so nothing needs to
 *  be kept around for a resend.
 *
 *  A client that opens with an HTTP request (curl, wget, a browser) gets an
 *  HTTP response; one that says nothing (nc) gets the raw bytes.
 */

#define XFER_PORT 8023

typedef enum {
    XFER_OK,
    XFER_NO_SOCKET, /* the socket pool is full */
    XFER_NO_CLIENT, /* nobody connected in time */
    XFER_CANCELLED, /* the telnet user pressed a key */
    XFER_STALLED,   /* the peer stopped acknowledging */
    XFER_RESET,     /* the peer went away */
    XFER_READ_ERROR,
} XferStatus_T;

typedef struct {
    uint64_t bytes; /* file bytes the peer acknowledged */
    uint32_t crc32; /* of those bytes, as zlib computes it */
    uint32_t resent; /* segments sent more than once */
    uint64_t ms;
    uint8_t http;
    uint8_t peer_ip[4];
} XferResult_T;

/* Call once with the driver name ("eth"/"slip") the process runs on. */
void xfer_init(const uint8_t *net_name);

/*
 *  Waits for one client on <port> and sends it <path>.  <size> is what
 *  bsh_file_size() said (BSH_SIZE_UNKNOWN: send until the file ends).  Blocks until the transfer is over; a key pressed
 *  in <session> cancels it.
 */
XferStatus_T xfer_send(TcpSocket_T sockets[MAX_SOCKETS], TcpSocket_T *session, const uint8_t *path, const uint8_t *name, int64_t size, uint16_t port, XferResult_T *res);

/*
 *  Sends <len> bytes from memory down <conn>, an established connection that
 *  stays open afterwards --- the telnet session itself, for `read` --- with
 *  the same care as a download: the peer's ACKs are followed and whatever
 *  went missing is sent again.  A key typed into <conn> meanwhile stops it,
 *  once what is already out has arrived, so the session goes on cleanly.
 *  XFER_OK when all of it arrived, XFER_CANCELLED when it was stopped
 *  (res->bytes says how far it got); XFER_STALLED and XFER_RESET leave the
 *  connection gone.
 */
XferStatus_T xfer_send_buffer(TcpSocket_T sockets[MAX_SOCKETS], TcpSocket_T *conn, const uint8_t *data, uint32_t len, XferResult_T *res);

#endif
