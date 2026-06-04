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

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
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

static char *path_join(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *out = malloc(la + 1 + lb + 1);
    if (!out) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
    memcpy(out, a, la);
    out[la] = '/';
    memcpy(out + la + 1, b, lb + 1);
    return out;
}

/* Create @p dir and any missing parents (mkdir -p), ignoring EEXIST. */
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

static void write_text_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    if (text && *text) fwrite(text, 1, strlen(text), f);
    fclose(f);
}

/* Recursively remove @p path (file or directory). */
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

/* Length of the longest common directory prefix (including its trailing '/')
 * over @p n absolute paths. */
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

/* Fork/exec tj3 with cwd = @p tmpdir, returning its captured stderr as a
 * heap string (owned by caller), or NULL on failure. */
static char *run_tj3(const char *tmpdir, char *const argv[]) {
    int errpipe[2];
    if (pipe(errpipe) != 0) return NULL;

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
    char  *buf = NULL;
    size_t len = 0, cap = 0;
    char   chunk[4096];
    ssize_t n;
    while ((n = read(errpipe[0], chunk, sizeof(chunk))) > 0) {
        if (len + (size_t)n + 1 > cap) {
            size_t nc = (len + (size_t)n + 1) * 2;
            char *t = realloc(buf, nc);
            if (!t) { free(buf); buf = NULL; len = 0; cap = 0; break; }
            buf = t;
            cap = nc;
        }
        memcpy(buf + len, chunk, (size_t)n);
        len += (size_t)n;
    }
    /* Drain any remaining bytes if we bailed on OOM, so the child can exit. */
    if (!buf) while (read(errpipe[0], chunk, sizeof(chunk)) > 0) { }
    if (buf) buf[len] = '\0';
    close(errpipe[0]);

    int status;
    waitpid(pid, &status, 0);
    return buf;
}

/* ── stderr parsing ──────────────────────────────────────────────────────── */

/* Map a tj3-reported path to a member URI, or NULL if it is outside the
 * project.  tj3 may report the path relative to the temp dir or absolute
 * under it; normalise both forms before matching against member relpaths. */
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
    return NULL;
}

static void parse_diagnostics(char *stderr_buf,
                              const char *tmpdir, const char *real_tmp,
                              const member *members, int nmembers,
                              diag_set *out) {
    if (!stderr_buf) return;

    char *save = NULL;
    for (char *line = strtok_r(stderr_buf, "\n", &save);
         line;
         line = strtok_r(NULL, "\n", &save)) {

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
            continue;
        }

        const char *message = marker + marker_len;

        /* The segment [line, marker) is "<path>:<lineno>". */
        char *colon = NULL;
        for (char *q = line; q < marker; q++)
            if (*q == ':') colon = q;
        if (!colon) continue;

        int lineno = 0, have_digits = 0;
        for (char *q = colon + 1; q < marker; q++) {
            if (*q < '0' || *q > '9') { have_digits = 0; break; }
            lineno = lineno * 10 + (*q - '0');
            have_digits = 1;
        }
        if (!have_digits) continue;

        char *path = strndup(line, (size_t)(colon - line));
        if (!path) continue;
        const char *uri = map_reported_path(path, tmpdir, real_tmp,
                                            members, nmembers);
        free(path);
        if (!uri) continue;

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
}

/* ── entry point ─────────────────────────────────────────────────────────── */

void tj3_collect_project(const workspace_snapshot *ws, const ws_project *proj,
                         tj3_mode mode, diag_set *out) {
    if (!ws || !proj || !out) return;
    if (!tj3_available()) return;

    int pindex = -1;
    for (int i = 0; i < ws->num_projects; i++)
        if (ws->projects[i] == proj) { pindex = i; break; }
    if (pindex < 0) return;

    /* Collect the project's member documents. */
    member *members = NULL;
    int     nmembers = 0, cap = 0;
    for (int i = 0; i < ws->num_docs; i++) {
        const ws_doc *wd = &ws->docs[i];
        if (wd->project_index != pindex || !wd->snap) continue;
        char *path = uri_to_path(wd->snap->uri);
        if (!path) continue;
        if (nmembers >= cap) {
            cap = cap ? cap * 2 : 4;
            member *t = realloc(members, (size_t)cap * sizeof(member));
            if (!t) { free(path); break; }
            members = t;
        }
        members[nmembers].path    = path;
        members[nmembers].relpath = NULL;
        members[nmembers].uri     = wd->snap->uri;
        members[nmembers].text    = wd->snap->text;
        nmembers++;
    }
    if (nmembers == 0) { free(members); return; }

    /* Paths relative to the members' common ancestor, so includes resolve. */
    char **paths = malloc((size_t)nmembers * sizeof(char *));
    if (!paths) { goto cleanup; }
    for (int i = 0; i < nmembers; i++) paths[i] = members[i].path;
    size_t cl = common_dir_len(paths, nmembers);
    free(paths);
    for (int i = 0; i < nmembers; i++) {
        char *rel = members[i].path + cl;
        if (*rel == '\0') {
            char *slash = strrchr(members[i].path, '/');
            rel = slash ? slash + 1 : members[i].path;
        }
        members[i].relpath = rel;
    }

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

    char *argv[4];
    int ai = 0;
    argv[ai++] = "tj3";
    if (mode == TJ3_SYNTAX_ONLY) argv[ai++] = "--check-syntax";
    argv[ai++] = root_rel;
    argv[ai]   = NULL;

    char *errbuf = run_tj3(tmpdir, argv);
    parse_diagnostics(errbuf, tmpdir, real_tmp, members, nmembers, out);
    free(errbuf);

    remove_recursive(tmpdir);
    free(real_tmp);

cleanup:
    for (int i = 0; i < nmembers; i++) free(members[i].path);
    free(members);
}
