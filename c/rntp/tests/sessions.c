#include "sessions.h"
#include "fixture.h"
#include <stdio.h>
static uint8_t request[RNTP_FRAME], reply[RNTP_FRAME];
static uint64_t id;
static RntpHeader call(RntpSessions *s, unsigned peer, uint8_t op, uint64_t lba, unsigned length) {
    RntpHeader h = { op, 0, length, ++id, lba, op == RNTP_HELLO || op == RNTP_HELLO_SHARED ? 0 : s->generation }, out;
    rntp_encode(request, &h);
    assert(rntp_sessions_handle(s, peer, request, reply)); assert(rntp_decode(reply, &out));
    assert(out.id == id && out.op == (op | RNTP_RESPONSE)); return out;
}
int main(void) {
    uint8_t *image = fixture(2), *undo = malloc(2 << 20);
    RntpSessions s; assert(rntp_sessions_init(&s, image, undo, 4096, 123));
    assert(call(&s, 1, RNTP_HELLO_SHARED, 0, 0).status == RNTP_OK);
    assert(call(&s, 2, RNTP_HELLO_SHARED, 0, 0).status == RNTP_OK);
    assert(call(&s, 3, RNTP_HELLO, 0, 0).status == RNTP_BUSY);
    assert(call(&s, 1, RNTP_READ, 100, 0).status == RNTP_BUSY);
    assert(call(&s, 1, RNTP_LOCK, 0, 0).status == RNTP_OK);
    assert(rntp_get_number(reply + RNTP_HEADER, 8) == 1);
    assert(call(&s, 2, RNTP_LOCK, 0, 0).status == RNTP_BUSY);
    uint64_t free_before = s.committed_free;
    memset(request + RNTP_HEADER, 42, 512);
    assert(call(&s, 1, RNTP_WRITE, 100, 512).status == RNTP_OK);
    memset(request + RNTP_HEADER, 99, 512);
    assert(call(&s, 1, RNTP_WRITE, 100, 512).status == RNTP_OK);
    assert(call(&s, 1, RNTP_READ, 100, 0).status == RNTP_OK && reply[RNTP_HEADER] == 99);
    assert(call(&s, 2, RNTP_READ, 100, 0).status == RNTP_BUSY);
    assert(call(&s, 2, RNTP_STAT, 0, 0).status == RNTP_OK);
    assert(rntp_get_number(reply + RNTP_HEADER, 8) == free_before);
    assert(call(&s, 1, RNTP_ABORT, 0, 0).status == RNTP_OK && s.revision == 1 && image[100 * 512] == 0);
    assert(call(&s, 2, RNTP_LOCK, 0, 0).status == RNTP_OK);
    memset(request + RNTP_HEADER, 17, 512);
    assert(call(&s, 2, RNTP_WRITE, 100, 512).status == RNTP_OK);
    assert(call(&s, 2, RNTP_UNLOCK, 0, 0).status == RNTP_OK && s.revision == 2);
    assert(call(&s, 1, RNTP_LOCK, 0, 0).status == RNTP_OK);
    assert(rntp_get_number(reply + RNTP_HEADER, 8) == 2);
    rntp_put_number(request + RNTP_HEADER, 32, 4);
    assert(call(&s, 1, RNTP_READ_BATCH, 100, 4).length == 32 * 512);
    assert(reply[RNTP_HEADER] == 17);
    rntp_put_number(request + RNTP_HEADER, 32, 4);
    assert(call(&s, 1, RNTP_READ_BATCH, 4090, 4).status == RNTP_INVALID);
    memset(request + RNTP_HEADER, 77, 512);
    assert(call(&s, 1, RNTP_WRITE, 0, 512).status == RNTP_INVALID);
    rntp_put_number(request + RNTP_HEADER, 2, 4);
    rntp_put_number(request + RNTP_HEADER + 4, 100, 8);
    memset(request + RNTP_HEADER + 12, 77, 512);
    rntp_put_number(request + RNTP_HEADER + 4 + RNTP_WRITE_RECORD, 4096, 8);
    assert(call(&s, 1, RNTP_WRITE_BATCH, 0, 4 + 2 * RNTP_WRITE_RECORD).status == RNTP_INVALID);
    assert(image[100 * 512] == 17 && !s.modified);
    rntp_put_number(request + RNTP_HEADER + 4 + RNTP_WRITE_RECORD, 101, 8);
    assert(call(&s, 1, RNTP_WRITE_BATCH, 0, 4 + 2 * RNTP_WRITE_RECORD).status == RNTP_OK);
    rntp_sessions_release(&s, 1); assert(image[100 * 512] == 17 && image[101 * 512] == 0 && s.revision == 2);
    assert(call(&s, 2, RNTP_LOCK, 0, 0).status == RNTP_OK);
    rntp_sessions_release(&s, 2);
    assert(call(&s, 3, RNTP_HELLO, 0, 0).status == RNTP_OK);
    assert(call(&s, 1, RNTP_HELLO_SHARED, 0, 0).status == RNTP_BUSY);
    assert(call(&s, 3, RNTP_READ, 100, 0).status == RNTP_OK && reply[RNTP_HEADER] == 17);
    rntp_sessions_release(&s, 3);
    free(image); free(undo); puts("shared sessions: PASS"); return 0;
}
