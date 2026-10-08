#ifndef _BSH_INT_H_
#define _BSH_INT_H_

/*
 *  What bsh's own files share, and no host needs: the mount table, small
 *  string helpers, and the base commands for the table in bsh.c.
 */

#include "syscall.h"

#include "bsh.h"

#define BSH_MAX_MOUNTS 8

/*  The mount table (syscall 0x2c), as last loaded.  */
extern MountInfo_T bsh_mnt[BSH_MAX_MOUNTS];
extern int bsh_mnt_count;

void bsh_load_mounts(void);
const char *bsh_fs_name(uint8_t type);
/*  The type of the mount <path> is on (the deepest that holds it), 0 if none.
 *  Load the table first.  */
uint8_t bsh_mount_type_at(const uint8_t *path);
/*  The directory under <path> on the way to mount <m>: its name at *name and
 *  the length, 0 when <m> is not below <path>.  */
uint8_t bsh_mount_child(const MountInfo_T *m, const uint8_t *path, const uint8_t **name);
/*  A directory only the mount table knows: the root, or one on the way to a
 *  mount point ("/mnt"), on rootfs.  Load the table first.  */
int bsh_mount_dir(const uint8_t *path);
/*  A directory there is: one the mount table knows, or one the kernel lists.  */
int bsh_dir_exists(const uint8_t *path);

uint8_t bsh_copy(uint8_t *dst, const uint8_t *src, uint8_t cap);
int bsh_eq(const uint8_t *a, const char *b);

/*  The kernel's working directory follows the session being served: `play`
 *  and `bg` take names relative to it, and deleting needs it.  */
void bsh_kernel_cwd(const uint8_t *path);
void bsh_kernel_cwd_for(BshSession *s);

/*  The base commands.  */
int bsh_cmd_ls(BshSession *s, const uint8_t *arg);
int bsh_cmd_cd(BshSession *s, const uint8_t *arg);
int bsh_cmd_mkdir(BshSession *s, const uint8_t *arg);
int bsh_cmd_rmdir(BshSession *s, const uint8_t *arg);
int bsh_cmd_rm(BshSession *s, const uint8_t *arg);
int bsh_cmd_read(BshSession *s, const uint8_t *arg);
int bsh_cmd_mount(BshSession *s, const uint8_t *arg);
int bsh_cmd_sysinfo(BshSession *s, const uint8_t *arg);
int bsh_cmd_ts(BshSession *s, const uint8_t *arg);
int bsh_cmd_kill(BshSession *s, const uint8_t *arg);
int bsh_cmd_meminfo(BshSession *s, const uint8_t *arg);
int bsh_cmd_heap(BshSession *s, const uint8_t *arg);
int bsh_cmd_bg(BshSession *s, const uint8_t *arg);
int bsh_cmd_play(BshSession *s, const uint8_t *arg);
int bsh_cmd_stop(BshSession *s, const uint8_t *arg);

#endif
