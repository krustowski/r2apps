#ifndef _TNT_AUTH_H_
#define _TNT_AUTH_H_

#include "types.h"

/*
 *  The login a new connection is asked for.
 *
 *  Memento's first login since boot that gives credentials leaves a salted
 *  hash of the login and the password in /mnt/tmp/SESSION.CFG
 *  (cpp/memento-hello/windows/login_window.cpp writes it).  While that file
 *  is there, a new telnet connection has to give the same pair before it gets
 *  a shell; connections made before it are left as they are.  Until then the
 *  system is root with no password, and tnt asks nothing, as it always did.
 */

/*  Whether new connections have to log in: SESSION.CFG is there and holds
 *  credentials.  One that holds the default ones --- empty, or "root" with no
 *  password, which an older Memento kept --- is no session.  */
int auth_required(void);

/*  Whether `login` and `pass` are the session's.  Also false when the file
 *  cannot be read or makes no sense: a session that is there but cannot be
 *  checked lets nobody in.  */
int auth_check(const uint8_t *login, uint32_t login_len, const uint8_t *pass, uint32_t pass_len);

#endif
