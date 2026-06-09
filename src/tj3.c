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

#include "tj3.h"
#include "pathutil.h"
#include "debug.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/**
 * One materialised member document of the project, ready to be
 * written into the temporary directory tj3 runs against.
 */
typedef struct member {
    char       *path;     /**< owned; absolute filesystem path */
    char       *relpath;  /**< points into `path`: path relative to the common dir */
    const char *uri;      /**< borrowed from the doc_snapshot */
    const char *text;     /**< borrowed from the doc_snapshot */
} member;

/* ── small filesystem helpers ────────────────────────────────────────────── */

/**
 * Concatenate two path components with a '/' separator.
 *
 * @param a Left-hand path component (must be NUL-terminated).
 * @param b Right-hand path component (must be NUL-terminated).
 * @return Newly heap-allocated string of the form "@p a/@p b"; the
 *         caller is responsible for freeing it.  Calls exit(1) on
 *         allocation failure.
 */
static char *path_join(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *out = malloc(la + 1 + lb + 1);
    if (!out) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    memcpy(out, a, la);
    out[la] = '/';
    memcpy(out + la + 1, b, lb + 1);
    return out;
}

/**
 * Create @p dir and any missing parents (equivalent to mkdir -p),
 * ignoring EEXIST at each level.
 *
 * @param dir Absolute or relative path of the directory to create.
 */
static void make_dirs(const char *dir) {
    char *tmp = strdup(dir);
    if (!tmp) return;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    mkdir(tmp, 0700);
    free(tmp);
}

/**
 * Write @p text to the file at @p path, creating or truncating it.
 * Returns silently if the file cannot be opened.
 *
 * @param path Filesystem path of the file to write.
 * @param text NUL-terminated content to write; NULL or empty string
 *             produces an empty file.
 */
static void write_text_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    if (text && *text) fwrite(text, 1, strlen(text), f);
    fclose(f);
}

/**
 * Recursively remove @p path (file or directory), equivalent to
 * rm -rf.  Returns silently if @p path does not exist.
 *
 * @param path Filesystem path of the file or directory to remove.
 */
static void remove_recursive(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                    continue;
                char *child = path_join(path, e->d_name);
                remove_recursive(child);
                free(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

/**
 * Compute the length of the longest common directory prefix shared by
 * all @p n absolute paths, including its trailing '/'.
 *
 * @param paths Array of @p n NUL-terminated absolute path strings.
 * @param n     Number of entries in @p paths.
 * @return Length in bytes of the common directory prefix, including
 *         the trailing '/'.  Returns 0 if @p n is zero.
 */
static size_t common_dir_len(char *const *paths, int n) {
    if (n <= 0) return 0;
    size_t len = strlen(paths[0]);
    for (int i = 1; i < n; i++) {
        size_t j = 0;
        while (j < len && paths[i][j] && paths[i][j] == paths[0][j]) j++;
        len = j;
    }
    while (len > 0 && paths[0][len - 1] != '/') len--;
    return len;
}

/* ── tj3 discovery ───────────────────────────────────────────────────────── */

/**
 * Test whether the tj3 binary is available and should be invoked.
 *
 * The result is computed once and cached.  If the environment variable
 * TASKJUGGLER_LSP_DISABLE_TJ3 is set, the function always returns 0
 * regardless of PATH, which provides a deterministic escape hatch for
 * test environments.
 *
 * @return 1 if tj3 is found and executable on PATH, 0 otherwise.
 */
static int tj3_available(void) {
    static int cached = -1;        /* -1 unknown, 0 absent, 1 present */
    if (cached >= 0) return cached;

    /* Escape hatch for the golden test harness and any environment that wants
     * deterministic, tj3-independent behavior: treat tj3 as absent. */
    if (getenv("TASKJUGGLER_LSP_DISABLE_TJ3")) { cached = 0; return cached; }

    int found = 0;
    const char *path = getenv("PATH");
    if (path) {
        char *copy = strdup(path);
        if (copy) {
            char *save = NULL;
            for (char *dir = strtok_r(copy, ":", &save);
                 dir && !found;
                 dir = strtok_r(NULL, ":", &save)) {
                if (!*dir) continue;
                char *cand = path_join(dir, "tj3");
                if (access(cand, X_OK) == 0) found = 1;
                free(cand);
            }
            free(copy);
        }
    }
    cached = found;
    return cached;
}

/* ── run tj3, capturing stderr ───────────────────────────────────────────── */

/**
 * Fork and exec tj3 with cwd set to @p tmpdir, capturing its stderr
 * output into a heap-allocated string.
 *
 * @param tmpdir Working directory for the tj3 child process.
 * @param argv   NULL-terminated argument vector passed to execvp;
 *               argv[0] must be "tj3".
 * @return Heap-allocated NUL-terminated string containing the full
 *         stderr output of tj3, owned by the caller; NULL on fork,
 *         pipe, or allocation failure.
 */
/**
 * Read @p fd to EOF into a freshly allocated NUL-terminated buffer.
 *
 * On allocation failure the partial buffer is freed and @p fd is still
 * drained to EOF (so the writer at the other end can finish and exit),
 * then NULL is returned.
 *
 * @param fd       Readable file descriptor; not closed by this function.
 * @param out_len  Receives the number of bytes read, excluding the NUL.
 * @return Heap-allocated buffer the caller must free, or NULL on OOM.
 */
static char *read_fd_to_string(int fd, size_t *out_len) {
    char  *buf = NULL;
    size_t len = 0, cap = 0;
    char   chunk[4096];
    ssize_t n;
    while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
        if (len + (size_t)n + 1 > cap) {
            size_t new_cap = (len + (size_t)n + 1) * 2;
            char *grown = realloc(buf, new_cap);
            if (!grown) { free(buf); buf = NULL; len = 0; cap = 0; break; }
            buf = grown;
            cap = new_cap;
        }
        memcpy(buf + len, chunk, (size_t)n);
        len += (size_t)n;
    }
    /* Drain any remaining bytes if we bailed on OOM, so the child can exit. */
    if (!buf) while (read(fd, chunk, sizeof(chunk)) > 0) { }
    if (buf) buf[len] = '\0';
    *out_len = len;
    return buf;
}

static char *run_tj3(const char *tmpdir, char *const argv[]) {
    int errpipe[2];
    if (pipe(errpipe) != 0) return NULL;

#if DEBUG_TJ3
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    DLOG(DEBUG_TJ3, LOG_VERBOSE, "exec tj3 %s%s (cwd=%s)",
         argv[1] ? argv[1] : "", argv[1] && argv[2] ? " ..." : "", tmpdir);
#endif

    pid_t pid = fork();
    if (pid < 0) {
        close(errpipe[0]);
        close(errpipe[1]);
        return NULL;
    }
    if (pid == 0) {
        /* Child: only async-signal-safe calls between fork and exec. */
        if (chdir(tmpdir) != 0) _exit(127);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        dup2(errpipe[1], STDERR_FILENO);
        close(errpipe[0]);
        close(errpipe[1]);
        execvp("tj3", argv);
        _exit(127);
    }

    /* Parent: drain stderr to EOF, then reap. */
    close(errpipe[1]);
    size_t len = 0;
    char *buf = read_fd_to_string(errpipe[0], &len);
    close(errpipe[0]);

    int status;
    waitpid(pid, &status, 0);

#if DEBUG_TJ3
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0
              + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    DLOG(DEBUG_TJ3, LOG_INFO, "tj3 exited %d in %.1f ms, %zu bytes stderr",
         WIFEXITED(status) ? WEXITSTATUS(status) : -1, ms, len);
#endif

    return buf;
}

/* ── stderr parsing ──────────────────────────────────────────────────────── */

/**
 * Map a tj3-reported file path to the corresponding member URI.
 *
 * tj3 may emit paths relative to the temp directory (e.g. "./foo.tjp")
 * or absolute under it; both forms are normalised to a bare relative
 * path before being matched against member relpaths.
 *
 * @param path     Raw path string emitted by tj3 in a diagnostic line.
 * @param tmpdir   The temporary directory passed to run_tj3().
 * @param real_tmp The result of realpath() on @p tmpdir, or NULL if
 *                 unavailable; used to resolve symlinks in the path.
 * @param members  Array of project member descriptors.
 * @param nmembers Number of entries in @p members.
 * @return Borrowed pointer to the member's URI string, or NULL if the
 *         path does not correspond to any project member.
 */
static const char *map_reported_path(const char *path,
                                     const char *tmpdir, const char *real_tmp,
                                     const member *members, int nmembers) {
    const char *p = path;
    while (p[0] == '.' && p[1] == '/') p += 2;
    if (p[0] == '/') {
        size_t tl = strlen(tmpdir);
        if (strncmp(p, tmpdir, tl) == 0 && p[tl] == '/') {
            p += tl + 1;
        } else if (real_tmp) {
            size_t rl = strlen(real_tmp);
            if (strncmp(p, real_tmp, rl) == 0 && p[rl] == '/') p += rl + 1;
        }
    }
    while (p[0] == '.' && p[1] == '/') p += 2;

    for (int i = 0; i < nmembers; i++)
        if (strcmp(members[i].relpath, p) == 0)
            return members[i].uri;
    DLOG(DEBUG_TJ3, LOG_VERBOSE,
         "tj3 reported path '%s' did not map to any project member", path);
    return NULL;
}

/**
 * Decode one tj3 stderr line and, if it reports a diagnostic, append it to
 * @p out.
 *
 * A line matches when it contains an ": Error: " or ": Warning: " marker
 * preceded by a "<path>:<lineno>" prefix that maps to a project member.
 * Matching lines become a Diagnostic (file path, 1-based line number,
 * severity, message); non-matching lines are ignored.
 *
 * @param line      One stderr line (NUL-terminated); read but not modified.
 * @param tmpdir    Temporary directory used for the tj3 run; passed to
 *                  map_reported_path() for path normalisation.
 * @param real_tmp  realpath() result for @p tmpdir, or NULL.
 * @param members   Array of project member descriptors.
 * @param nmembers  Number of entries in @p members.
 * @param out       Destination diag_set to receive the diagnostic.
 */
static void parse_diagnostic_line(char *line,
                                  const char *tmpdir, const char *real_tmp,
                                  const member *members, int nmembers,
                                  diag_set *out) {
    int   severity;
    char *marker;
    size_t marker_len;
    if ((marker = strstr(line, ": Error: ")) != NULL) {
        severity   = DIAG_ERROR;
        marker_len = strlen(": Error: ");
    } else if ((marker = strstr(line, ": Warning: ")) != NULL) {
        severity   = DIAG_WARNING;
        marker_len = strlen(": Warning: ");
    } else {
        return;
    }

    const char *message = marker + marker_len;

    /* The segment [line, marker) is "<path>:<lineno>". */
    char *colon = NULL;
    for (char *q = line; q < marker; q++)
        if (*q == ':') colon = q;
    if (!colon) return;

    int lineno = 0, have_digits = 0;
    for (char *q = colon + 1; q < marker; q++) {
        if (*q < '0' || *q > '9') { have_digits = 0; break; }
        lineno = lineno * 10 + (*q - '0');
        have_digits = 1;
    }
    if (!have_digits) return;

    char *path = strndup(line, (size_t)(colon - line));
    if (!path) return;
    const char *uri = map_reported_path(path, tmpdir, real_tmp,
                                        members, nmembers);
    free(path);
    if (!uri) return;

    uint32_t l = lineno > 0 ? (uint32_t)(lineno - 1) : 0;
    Diagnostic d;
    d.range.start.line      = l;
    d.range.start.character = 0;
    d.range.end.line        = l;
    d.range.end.character   = (uint32_t)INT_MAX;
    d.severity              = severity;
    d.source                = "tj3";
    d.message               = strdup(message);
    diag_set_add(out, uri, d);
}

/**
 * Parse tj3 stderr output and populate @p out with the resulting
 * diagnostics, one per matching line (see parse_diagnostic_line()).
 *
 * @param stderr_buf  NUL-terminated buffer of tj3 stderr; modified in
 *                    place by strtok_r.  May be NULL (no-op).
 * @param tmpdir      Temporary directory used for the tj3 run.
 * @param real_tmp    realpath() result for @p tmpdir, or NULL.
 * @param members     Array of project member descriptors.
 * @param nmembers    Number of entries in @p members.
 * @param out         Destination diag_set to receive the parsed
 *                    diagnostics.
 */
static void parse_diagnostics(char *stderr_buf,
                              const char *tmpdir, const char *real_tmp,
                              const member *members, int nmembers,
                              diag_set *out) {
    if (!stderr_buf) return;

    char *save = NULL;
    for (char *line = strtok_r(stderr_buf, "\n", &save);
         line;
         line = strtok_r(NULL, "\n", &save))
        parse_diagnostic_line(line, tmpdir, real_tmp, members, nmembers, out);
}

/* ── entry point ─────────────────────────────────────────────────────────── */

/**
 * Gather the member documents of project @p pindex from @p ws into a freshly
 * allocated array.
 *
 * Each member's `path` is heap-allocated (the caller frees it); `relpath` is
 * left NULL for assign_member_relpaths() to fill in later, while `uri` and
 * `text` borrow the document snapshot. A member whose URI cannot be converted
 * to a path is skipped.
 *
 * @param ws            Workspace snapshot to scan.
 * @param pindex        Index of the project whose members are wanted.
 * @param out_nmembers  Receives the number of members collected.
 * @return Heap-allocated member array (NULL when the project has no usable
 *         members), owned by the caller.
 */
static member *collect_project_members(const workspace_snapshot *ws, int pindex,
                                       int *out_nmembers) {
    member *members = NULL;
    int     nmembers = 0, cap = 0;
    for (int i = 0; i < ws->num_docs; i++) {
        const ws_doc *wd = &ws->docs[i];
        if (wd->project_index != pindex || !wd->snap) continue;
        char *path = uri_to_path(wd->snap->uri);
        if (!path) continue;
        if (nmembers >= cap) {
            cap = cap ? cap * 2 : 4;
            member *grown = realloc(members, (size_t)cap * sizeof(member));
            if (!grown) { free(path); break; }
            members = grown;
        }
        members[nmembers].path    = path;
        members[nmembers].relpath = NULL;
        members[nmembers].uri     = wd->snap->uri;
        members[nmembers].text    = wd->snap->text;
        nmembers++;
    }
    *out_nmembers = nmembers;
    return members;
}

/** Fill each member's relpath: the portion of its absolute path below the
 *  members' common ancestor directory, so includes resolve identically once
 *  the members are staged under a shared tmpdir. A member that *is* the common
 *  ancestor (empty remainder) falls back to its basename.
 *  @param members   Member array whose `path` fields are set; `relpath` filled.
 *  @param nmembers  Number of entries in @p members.
 *  @return 1 on success, 0 on allocation failure (relpaths left unset). */
static int assign_member_relpaths(member *members, int nmembers) {
    char **paths = malloc((size_t)nmembers * sizeof(char *));
    if (!paths) return 0;
    for (int i = 0; i < nmembers; i++) paths[i] = members[i].path;
    size_t common_len = common_dir_len(paths, nmembers);
    free(paths);

    for (int i = 0; i < nmembers; i++) {
        char *rel = members[i].path + common_len;
        if (*rel == '\0') {
            char *slash = strrchr(members[i].path, '/');
            rel = slash ? slash + 1 : members[i].path;
        }
        members[i].relpath = rel;
    }
    return 1;
}

/** Write each member's text into @p tmpdir at its relpath, creating parent
 *  directories as needed, so tj3 can run over a faithful on-disk copy of the
 *  project's source.
 *  @param tmpdir    Staging directory (already created).
 *  @param members   Member array with relpath and text set.
 *  @param nmembers  Number of entries in @p members. */
static void stage_members_to_tmpdir(const char *tmpdir, const member *members,
                                    int nmembers) {
    for (int i = 0; i < nmembers; i++) {
        char *dest = path_join(tmpdir, members[i].relpath);
        char *slash = strrchr(dest, '/');
        if (slash) {
            *slash = '\0';
            make_dirs(dest);
            *slash = '/';
        }
        write_text_file(dest, members[i].text);
        free(dest);
    }
}

void tj3_collect_project(const workspace_snapshot *ws, const ws_project *proj,
                         tj3_mode mode, diag_set *out) {
    if (!ws || !proj || !out) return;
    if (!tj3_available()) return;

    int pindex = -1;
    for (int i = 0; i < ws->num_projects; i++)
        if (ws->projects[i] == proj) { pindex = i; break; }
    if (pindex < 0) return;

    /* Collect the project's member documents. */
    int nmembers = 0;
    member *members = collect_project_members(ws, pindex, &nmembers);
    if (nmembers == 0) { free(members); return; }

    /* Paths relative to the members' common ancestor, so includes resolve. */
    if (!assign_member_relpaths(members, nmembers)) goto cleanup;

    /* The project's root document = the member whose URI is the project id. */
    char *root_rel = NULL;
    for (int i = 0; i < nmembers; i++)
        if (members[i].uri && strcmp(members[i].uri, proj->id) == 0) {
            root_rel = members[i].relpath;
            break;
        }
    if (!root_rel) goto cleanup;

    char tmpl[] = "/tmp/tjlsp-XXXXXX";
    char *tmpdir = mkdtemp(tmpl);
    if (!tmpdir) goto cleanup;
    char *real_tmp = realpath(tmpdir, NULL);

    stage_members_to_tmpdir(tmpdir, members, nmembers);

    char *argv[4];
    int ai = 0;
    argv[ai++] = "tj3";
    if (mode == TJ3_SYNTAX_ONLY) argv[ai++] = "--check-syntax";
    argv[ai++] = root_rel;
    argv[ai]   = NULL;

    DLOG(DEBUG_TJ3, LOG_INFO, "collect project '%s': %d members, mode=%s, root=%s",
         proj->id ? proj->id : "(no-id)", nmembers,
         mode == TJ3_SYNTAX_ONLY ? "syntax-only" : "full", root_rel);

    char *errbuf = run_tj3(tmpdir, argv);
    parse_diagnostics(errbuf, tmpdir, real_tmp, members, nmembers, out);
    free(errbuf);

    remove_recursive(tmpdir);
    free(real_tmp);

cleanup:
    for (int i = 0; i < nmembers; i++) free(members[i].path);
    free(members);
}
