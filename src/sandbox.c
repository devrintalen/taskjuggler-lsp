/*
 * taskjuggler-lsp - Language Server Protocol implementation for TaskJuggler v3
 * Copyright (C) 2026  Devrin Talen <dct23@cornell.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/** @file */

/** O_PATH is a GNU extension not exposed under the project-wide _DEFAULT_SOURCE. */
#define _GNU_SOURCE 1

#include "sandbox.h"

/* Landlock is Linux-only and needs both the uapi header and the syscall
 * numbers.  When either is missing we compile stubs that report the sandbox
 * as unavailable, so the caller falls back to not generating reports. */
#if defined(__linux__) && defined(__has_include)
#  if __has_include(<linux/landlock.h>)
#    define TJLSP_TRY_LANDLOCK 1
#  endif
#endif

#ifdef TJLSP_TRY_LANDLOCK
#include <linux/landlock.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#endif

#if defined(TJLSP_TRY_LANDLOCK) && defined(__NR_landlock_create_ruleset) \
    && defined(__NR_landlock_add_rule) && defined(__NR_landlock_restrict_self)

/* Thin wrappers over the raw syscalls: glibc versions predating Landlock do
 * not expose these, and calling syscall() directly keeps the child path
 * async-signal-safe. */
static long ll_create_ruleset(const struct landlock_ruleset_attr *attr,
                              size_t size, uint32_t flags) {
    return syscall(__NR_landlock_create_ruleset, attr, size, flags);
}
static long ll_add_rule(int ruleset_fd, enum landlock_rule_type type,
                        const void *attr, uint32_t flags) {
    return syscall(__NR_landlock_add_rule, ruleset_fd, type, attr, flags);
}
static long ll_restrict_self(int ruleset_fd, uint32_t flags) {
    return syscall(__NR_landlock_restrict_self, ruleset_fd, flags);
}

/** Assemble the set of write-family access rights to confine, masked to what
 *  the running kernel's ABI version @p abi understands.  Read, execute, and
 *  ioctl rights are deliberately excluded so they remain unrestricted.
 *  @param abi  Landlock ABI version reported by the kernel (>= 1).
 *  @return Bitmask of LANDLOCK_ACCESS_FS_* write rights. */
static uint64_t write_access_mask(long abi) {
    uint64_t writes =
        LANDLOCK_ACCESS_FS_WRITE_FILE  |
        LANDLOCK_ACCESS_FS_REMOVE_DIR  |
        LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_MAKE_CHAR   |
        LANDLOCK_ACCESS_FS_MAKE_DIR    |
        LANDLOCK_ACCESS_FS_MAKE_REG    |
        LANDLOCK_ACCESS_FS_MAKE_SOCK   |
        LANDLOCK_ACCESS_FS_MAKE_FIFO   |
        LANDLOCK_ACCESS_FS_MAKE_BLOCK  |
        LANDLOCK_ACCESS_FS_MAKE_SYM;
    if (abi >= 2) writes |= LANDLOCK_ACCESS_FS_REFER;    /* rename/link across dirs */
    if (abi >= 3) writes |= LANDLOCK_ACCESS_FS_TRUNCATE; /* truncate(2) */
    return writes;
}

int sandbox_available(void) {
    static int cached = -1;   /* -1 unknown, 0 absent, 1 present */
    if (cached >= 0) return cached;
    long abi = ll_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    cached = (abi >= 1) ? 1 : 0;
    return cached;
}

int sandbox_confine_writes_to(const char *dir) {
    long abi = ll_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1) return -1;

    uint64_t writes = write_access_mask(abi);

    /* Only handle the write-family rights: unhandled rights (read, execute,
     * ioctl) stay fully permitted, so tj3 and its Ruby runtime load normally.
     * Zero-initialize the whole struct: the header may declare fields newer
     * than the ones set here (handled_access_net, scoped, ...) and the kernel
     * rejects a ruleset whose unhandled tail bytes are non-zero. */
    struct landlock_ruleset_attr rattr = {0};
    rattr.handled_access_fs = writes;
    int ruleset_fd = (int)ll_create_ruleset(&rattr, sizeof(rattr), 0);
    if (ruleset_fd < 0) return -1;

    /* Grant the full write set beneath dir, and nowhere else. */
    int dir_fd = open(dir, O_PATH | O_CLOEXEC);
    if (dir_fd < 0) { close(ruleset_fd); return -1; }

    struct landlock_path_beneath_attr pb;
    pb.allowed_access = writes;
    pb.parent_fd      = dir_fd;
    int rc = (int)ll_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
    close(dir_fd);
    if (rc != 0) { close(ruleset_fd); return -1; }

    /* RubyGems' binstub opens /dev/null for writing while activating the
     * taskjuggler gem, so tj3 dies on startup unless it is writable.  It is
     * a pure data sink, so allowing it does not weaken the confinement.
     * Only WRITE_FILE: a file rule must not carry directory-only rights. */
    int null_fd = open("/dev/null", O_PATH | O_CLOEXEC);
    if (null_fd < 0) { close(ruleset_fd); return -1; }
    pb.allowed_access = LANDLOCK_ACCESS_FS_WRITE_FILE;
    pb.parent_fd      = null_fd;
    rc = (int)ll_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
    close(null_fd);
    if (rc != 0) { close(ruleset_fd); return -1; }

    /* Landlock requires no_new_privs for unprivileged use. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) { close(ruleset_fd); return -1; }
    if (ll_restrict_self(ruleset_fd, 0) != 0) { close(ruleset_fd); return -1; }
    close(ruleset_fd);
    return 0;
}

#else  /* no Landlock support at build time */

int sandbox_available(void) { return 0; }
int sandbox_confine_writes_to(const char *dir) { (void)dir; return -1; }

#endif
