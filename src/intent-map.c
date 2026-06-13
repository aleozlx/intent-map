/*
 * intent-map — an agent-friendly, symbol-oriented binding store.
 *
 * A single executable wrapping a SQLite (WAL) store that maps stable opaque
 * keys ("labels") to their meaning (one-line `summary` + durable free-form
 * `detail`), with full-text search, recency, and a constrained,
 * durability-preserving operation set.
 *
 * Design: see DESIGN-intent-map.md. SQLite owns storage and concurrency; this
 * file is a thin CLI over it. Output uses the line-oriented wire grammar of
 * §5.1 (one leading sigil per line, escaping-free for value prose).
 */
#define _XOPEN_SOURCE 700
#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- exit codes (documented in --help; callers switch on these) ---------- */
enum {
    EX_OK        = 0,  /* success                                            */
    EX_USAGE     = 1,  /* bad/missing arguments, unknown verb                */
    EX_VALIDATE  = 2,  /* caller-supplied label failed charset validation    */
    EX_CONFLICT  = 3,  /* label already exists (active or retired/burned)     */
    EX_NOTFOUND  = 4,  /* get/annotate/retire on a label that does not exist  */
    EX_DB        = 5   /* SQLite/internal error                              */
};

/* ISO-8601 UTC with millisecond precision; recency resolution for `recent`. */
#define NOW_SQL "strftime('%Y-%m-%dT%H:%M:%fZ','now')"

/* ========================================================================= */
/* Wire-format emitters (§5.1)                                               */
/* ========================================================================= */

/*
 * Emit a record header line: '@' label TAB <first line of summary> NL.
 * `label` is guaranteed tab/newline-free (charset invariant). Only the first
 * physical line of `summary` is placed on the header so the line is always a
 * single, grammar-valid header (full summary round-trips via its own field).
 */
static void emit_header(const char *label, const char *summary)
{
    const char *nl = strchr(summary, '\n');
    fputc('@', stdout);
    fputs(label, stdout);
    fputc('\t', stdout);
    if (nl)
        fwrite(summary, 1, (size_t)(nl - summary), stdout);
    else
        fputs(summary, stdout);
    fputc('\n', stdout);
}

/*
 * Emit a field with continuation-wrapped value (the entire escaping burden):
 *   ':' key TAB <first value line> NL   ('.' <rest line> NL)*
 * Every physical line after the first is prefixed with '.', so value prose may
 * contain any character — '@', ':', '.', tabs, embedded newlines — with zero
 * character-level escaping. A newline is re-wrapped, never escaped.
 */
static void emit_field(const char *key, const char *value)
{
    fputc(':', stdout);
    fputs(key, stdout);
    fputc('\t', stdout);

    const char *p = value;
    const char *nl = strchr(p, '\n');
    if (nl)
        fwrite(p, 1, (size_t)(nl - p), stdout);
    else {
        fputs(p, stdout);
        fputc('\n', stdout);
        return;
    }
    fputc('\n', stdout);
    p = nl + 1;
    for (;;) {
        nl = strchr(p, '\n');
        fputc('.', stdout);
        if (!nl) {
            fputs(p, stdout);
            fputc('\n', stdout);
            return;
        }
        fwrite(p, 1, (size_t)(nl - p), stdout);
        fputc('\n', stdout);
        p = nl + 1;
    }
}

/* Glance line: '@' label TAB <summary first line> [TAB <extra>] NL. */
static void emit_glance(const char *label, const char *summary, const char *extra)
{
    const char *nl = strchr(summary, '\n');
    fputc('@', stdout);
    fputs(label, stdout);
    fputc('\t', stdout);
    if (nl)
        fwrite(summary, 1, (size_t)(nl - summary), stdout);
    else
        fputs(summary, stdout);
    if (extra) {
        fputc('\t', stdout);
        fputs(extra, stdout);
    }
    fputc('\n', stdout);
}

static void errf(const char *msg) { fprintf(stderr, "intent-map: %s\n", msg); }

/* ========================================================================= */
/* Database open + schema (idempotent on every run)                          */
/* ========================================================================= */

static const char *SCHEMA_SQL =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA foreign_keys=ON;"
    "CREATE TABLE IF NOT EXISTS bindings("
    "  ordinal     INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  label       TEXT NOT NULL UNIQUE,"
    "  summary     TEXT NOT NULL,"
    "  detail      TEXT NOT NULL DEFAULT '',"
    "  status      TEXT NOT NULL DEFAULT 'active'"
    "                CHECK (status IN ('active','retired')),"
    "  created_at  TEXT NOT NULL,"
    "  modified_at TEXT NOT NULL,"
    "  retired_at  TEXT"
    ");"
    "CREATE VIRTUAL TABLE IF NOT EXISTS bindings_fts USING fts5("
    "  label UNINDEXED, summary, detail,"
    "  content='bindings', content_rowid='ordinal'"
    ");"
    /* Keep the external-content FTS index in lockstep with bindings via
     * triggers, so the index is a pure projection and cannot drift. */
    "CREATE TRIGGER IF NOT EXISTS bindings_ai AFTER INSERT ON bindings BEGIN"
    "  INSERT INTO bindings_fts(rowid,label,summary,detail)"
    "    VALUES(new.ordinal,new.label,new.summary,new.detail);"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS bindings_ad AFTER DELETE ON bindings BEGIN"
    "  INSERT INTO bindings_fts(bindings_fts,rowid,label,summary,detail)"
    "    VALUES('delete',old.ordinal,old.label,old.summary,old.detail);"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS bindings_au AFTER UPDATE ON bindings BEGIN"
    "  INSERT INTO bindings_fts(bindings_fts,rowid,label,summary,detail)"
    "    VALUES('delete',old.ordinal,old.label,old.summary,old.detail);"
    "  INSERT INTO bindings_fts(rowid,label,summary,detail)"
    "    VALUES(new.ordinal,new.label,new.summary,new.detail);"
    "END;"
    /* Vocabulary projection powering `index` (term + per-doc occurrences). */
    "CREATE VIRTUAL TABLE IF NOT EXISTS bindings_vocab"
    "  USING fts5vocab('bindings_fts','instance');";

static sqlite3 *open_db(const char *path)
{
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        errf(sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }
    /* Wait rather than fail when another process holds the write lock. */
    sqlite3_busy_timeout(db, 10000);

    char *err = NULL;
    rc = sqlite3_exec(db, SCHEMA_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "intent-map: schema init failed: %s\n",
                err ? err : sqlite3_errmsg(db));
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

/* ========================================================================= */
/* Verb: allocate                                                            */
/* ========================================================================= */

/* Caller-supplied labels must be a single opaque token: non-empty and free of
 * tab/newline (CR or LF), so the first-tab split of §5.1 stays exact. */
static int label_charset_ok(const char *label)
{
    if (label[0] == '\0')
        return 0;
    for (const char *p = label; *p; ++p)
        if (*p == '\t' || *p == '\n' || *p == '\r')
            return 0;
    return 1;
}

/* `summary` is the one-line glance tier, carried verbatim on the record header
 * (its single home), so it must be newline-free; tabs are inert after the
 * first tab and remain allowed. */
static int summary_ok(const char *summary)
{
    for (const char *p = summary; *p; ++p)
        if (*p == '\n' || *p == '\r')
            return 0;
    return 1;
}

static int do_allocate(sqlite3 *db, const char *label,
                       const char *summary, const char *detail)
{
    int rc;
    char *err = NULL;

    if (summary == NULL) {
        errf("allocate requires --summary");
        return EX_USAGE;
    }
    if (!summary_ok(summary)) {
        errf("invalid --summary: must be a single line (no newline)");
        return EX_VALIDATE;
    }
    if (detail == NULL)
        detail = "";

    if (label != NULL && !label_charset_ok(label)) {
        errf("invalid --label: must be non-empty and contain no tab/newline");
        return EX_VALIDATE;
    }

    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, &err) != SQLITE_OK) {
        errf(err ? err : "begin failed");
        sqlite3_free(err);
        return EX_DB;
    }

    sqlite3_stmt *st = NULL;
    /* Either mode inserts a row first (consuming an ordinal via AUTOINCREMENT);
     * minted mode inserts a transient empty label and overwrites it below. */
    rc = sqlite3_prepare_v2(db,
        "INSERT INTO bindings(label,summary,detail,created_at,modified_at)"
        " VALUES(?1,?2,?3," NOW_SQL "," NOW_SQL ");", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        errf(sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return EX_DB;
    }
    sqlite3_bind_text(st, 1, label ? label : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, summary, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, detail, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE) {
        int ec = EX_DB;
        if (sqlite3_extended_errcode(db) == SQLITE_CONSTRAINT_UNIQUE ||
            sqlite3_errcode(db) == SQLITE_CONSTRAINT) {
            /* The UNIQUE index spans active *and* retired rows: a burned key
             * collides correctly. Never silently coerce; reject. */
            errf("label already in use (active or retired); keys are never reused");
            ec = EX_CONFLICT;
        } else {
            errf(sqlite3_errmsg(db));
        }
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return ec;
    }

    sqlite3_int64 ordinal = sqlite3_last_insert_rowid(db);
    char minted[32];

    if (label == NULL) {
        /* Minted: the label *is* the ordinal, prefixed. Set it from the just
         * assigned rowid, in the same transaction. */
        rc = sqlite3_prepare_v2(db,
            "UPDATE bindings SET label='L'||ordinal WHERE ordinal=?1;",
            -1, &st, NULL);
        if (rc != SQLITE_OK) {
            errf(sqlite3_errmsg(db));
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            return EX_DB;
        }
        sqlite3_bind_int64(st, 1, ordinal);
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            errf(sqlite3_errmsg(db));
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            return EX_DB;
        }
        snprintf(minted, sizeof minted, "L%lld", (long long)ordinal);
    }

    if (sqlite3_exec(db, "COMMIT;", NULL, NULL, &err) != SQLITE_OK) {
        errf(err ? err : "commit failed");
        sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return EX_DB;
    }

    emit_header(label ? label : minted, summary);
    return EX_OK;
}

/* ========================================================================= */
/* Verb: search                                                              */
/* ========================================================================= */

/* Build an FTS5 MATCH expression from raw keywords: each keyword becomes a
 * quoted string literal (embedded quotes doubled) so tokenizer/syntax chars are
 * inert, and keywords are OR'd for recall over precision. Caller frees. */
static char *build_match(int n, char **kw)
{
    size_t cap = 1;
    for (int i = 0; i < n; ++i)
        cap += strlen(kw[i]) * 2 + 8; /* quotes, doubling, " OR " */
    char *out = malloc(cap);
    if (!out)
        return NULL;
    char *o = out;
    for (int i = 0; i < n; ++i) {
        if (i)
            o += sprintf(o, " OR ");
        *o++ = '"';
        for (const char *p = kw[i]; *p; ++p) {
            if (*p == '"')
                *o++ = '"';
            *o++ = *p;
        }
        *o++ = '"';
    }
    *o = '\0';
    return out;
}

static int do_search(sqlite3 *db, int n, char **kw)
{
    if (n < 1) {
        errf("search requires at least one KEYWORD");
        return EX_USAGE;
    }
    char *match = build_match(n, kw);
    if (!match) {
        errf("out of memory");
        return EX_DB;
    }

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT b.label,b.summary"
        " FROM bindings_fts JOIN bindings b ON b.ordinal=bindings_fts.rowid"
        " WHERE bindings_fts MATCH ?1 AND b.status='active'"
        " ORDER BY bindings_fts.rank;", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        errf(sqlite3_errmsg(db));
        free(match);
        return EX_DB;
    }
    sqlite3_bind_text(st, 1, match, -1, SQLITE_TRANSIENT);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        emit_glance((const char *)sqlite3_column_text(st, 0),
                    (const char *)sqlite3_column_text(st, 1), NULL);
    }
    sqlite3_finalize(st);
    free(match);
    return rc == SQLITE_DONE ? EX_OK : EX_DB;
}

/* ========================================================================= */
/* Verb: get                                                                 */
/* ========================================================================= */

static int do_get(sqlite3 *db, int n, char **labels)
{
    if (n < 1) {
        errf("get requires at least one LABEL");
        return EX_USAGE;
    }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT label,summary,detail,status,created_at,modified_at,"
        "       ordinal,retired_at"
        "  FROM bindings WHERE label=?1;", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        errf(sqlite3_errmsg(db));
        return EX_DB;
    }

    int missing = 0;
    for (int i = 0; i < n; ++i) {
        sqlite3_reset(st);
        sqlite3_bind_text(st, 1, labels[i], -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            const char *label   = (const char *)sqlite3_column_text(st, 0);
            const char *summary = (const char *)sqlite3_column_text(st, 1);
            const char *detail  = (const char *)sqlite3_column_text(st, 2);
            const char *status  = (const char *)sqlite3_column_text(st, 3);
            const char *created = (const char *)sqlite3_column_text(st, 4);
            const char *modif   = (const char *)sqlite3_column_text(st, 5);
            char ordbuf[32];
            snprintf(ordbuf, sizeof ordbuf, "%lld",
                     (long long)sqlite3_column_int64(st, 6));
            const char *retired = (sqlite3_column_type(st, 7) == SQLITE_NULL)
                                      ? NULL
                                      : (const char *)sqlite3_column_text(st, 7);
            /* summary lives on the header (its single home); the fields
             * carry detail + metadata. No value is emitted twice. */
            emit_header(label, summary);
            emit_field("detail", detail);
            emit_field("status", status);
            emit_field("created_at", created);
            emit_field("modified_at", modif);
            emit_field("ordinal", ordbuf);
            if (retired)
                emit_field("retired_at", retired);
        } else if (rc == SQLITE_DONE) {
            fprintf(stderr, "intent-map: no such label: %s\n", labels[i]);
            missing = 1;
        } else {
            errf(sqlite3_errmsg(db));
            sqlite3_finalize(st);
            return EX_DB;
        }
    }
    sqlite3_finalize(st);
    return missing ? EX_NOTFOUND : EX_OK;
}

/* ========================================================================= */
/* Verb: index                                                               */
/* ========================================================================= */

static int do_index(sqlite3 *db, int with_labels)
{
    sqlite3_stmt *st = NULL;
    int rc;

    if (!with_labels) {
        rc = sqlite3_prepare_v2(db,
            "SELECT v.term,COUNT(DISTINCT v.doc)"
            "  FROM bindings_vocab v JOIN bindings b ON b.ordinal=v.doc"
            " WHERE b.status='active'"
            " GROUP BY v.term ORDER BY 2 DESC,v.term ASC;", -1, &st, NULL);
        if (rc != SQLITE_OK) {
            errf(sqlite3_errmsg(db));
            return EX_DB;
        }
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            char cnt[32];
            snprintf(cnt, sizeof cnt, "%d", sqlite3_column_int(st, 1));
            emit_field((const char *)sqlite3_column_text(st, 0), cnt);
        }
        sqlite3_finalize(st);
        return rc == SQLITE_DONE ? EX_OK : EX_DB;
    }

    /* --with-labels: emit each keyword's count, then its labels+summaries.
     * Rows arrive grouped by term; buffer one term's labels to count them
     * before printing the term header. */
    rc = sqlite3_prepare_v2(db,
        "SELECT DISTINCT v.term,b.label,b.summary"
        "  FROM bindings_vocab v JOIN bindings b ON b.ordinal=v.doc"
        " WHERE b.status='active'"
        " ORDER BY v.term ASC,b.label ASC;", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        errf(sqlite3_errmsg(db));
        return EX_DB;
    }

    char *cur = NULL;                 /* current term */
    char **labs = NULL, **sums = NULL;
    int count = 0, cap = 0;

    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *term = (const char *)sqlite3_column_text(st, 0);
        const char *lab  = (const char *)sqlite3_column_text(st, 1);
        const char *sum  = (const char *)sqlite3_column_text(st, 2);
        if (cur == NULL || strcmp(cur, term) != 0) {
            if (cur != NULL) {
                char cnt[32];
                snprintf(cnt, sizeof cnt, "%d", count);
                emit_field(cur, cnt);
                for (int i = 0; i < count; ++i) {
                    emit_glance(labs[i], sums[i], NULL);
                    free(labs[i]);
                    free(sums[i]);
                }
                free(cur);
            }
            cur = strdup(term);
            count = 0;
        }
        if (count == cap) {
            cap = cap ? cap * 2 : 8;
            labs = realloc(labs, (size_t)cap * sizeof *labs);
            sums = realloc(sums, (size_t)cap * sizeof *sums);
        }
        labs[count] = strdup(lab);
        sums[count] = strdup(sum);
        count++;
    }
    if (cur != NULL) {
        char cnt[32];
        snprintf(cnt, sizeof cnt, "%d", count);
        emit_field(cur, cnt);
        for (int i = 0; i < count; ++i) {
            emit_glance(labs[i], sums[i], NULL);
            free(labs[i]);
            free(sums[i]);
        }
        free(cur);
    }
    free(labs);
    free(sums);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? EX_OK : EX_DB;
}

/* ========================================================================= */
/* Verb: recent                                                              */
/* ========================================================================= */

static int do_recent(sqlite3 *db, int limit)
{
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT label,summary,modified_at FROM bindings"
        " WHERE status='active'"
        " ORDER BY modified_at DESC,ordinal DESC LIMIT ?1;", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        errf(sqlite3_errmsg(db));
        return EX_DB;
    }
    sqlite3_bind_int(st, 1, limit);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        emit_glance((const char *)sqlite3_column_text(st, 0),
                    (const char *)sqlite3_column_text(st, 1),
                    (const char *)sqlite3_column_text(st, 2));
    }
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? EX_OK : EX_DB;
}

/* ========================================================================= */
/* Verb: annotate                                                            */
/* ========================================================================= */

static int do_annotate(sqlite3 *db, const char *label,
                       const char *summary, const char *detail)
{
    if (label == NULL) {
        errf("annotate requires a LABEL");
        return EX_USAGE;
    }
    if (summary == NULL && detail == NULL) {
        errf("annotate requires --summary and/or --detail");
        return EX_USAGE;
    }
    if (summary != NULL && !summary_ok(summary)) {
        errf("invalid --summary: must be a single line (no newline)");
        return EX_VALIDATE;
    }

    /* Edit prose only and bump modified_at; never touch the key binding. */
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "UPDATE bindings SET"
        "   summary=COALESCE(?2,summary),"
        "   detail=COALESCE(?3,detail),"
        "   modified_at=" NOW_SQL
        " WHERE label=?1;", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        errf(sqlite3_errmsg(db));
        return EX_DB;
    }
    sqlite3_bind_text(st, 1, label, -1, SQLITE_TRANSIENT);
    if (summary) sqlite3_bind_text(st, 2, summary, -1, SQLITE_TRANSIENT);
    else         sqlite3_bind_null(st, 2);
    if (detail)  sqlite3_bind_text(st, 3, detail, -1, SQLITE_TRANSIENT);
    else         sqlite3_bind_null(st, 3);

    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        errf(sqlite3_errmsg(db));
        return EX_DB;
    }
    if (sqlite3_changes(db) == 0) {
        fprintf(stderr, "intent-map: no such label: %s\n", label);
        return EX_NOTFOUND;
    }
    return EX_OK;
}

/* ========================================================================= */
/* Verb: retire                                                              */
/* ========================================================================= */

static int do_retire(sqlite3 *db, const char *label)
{
    if (label == NULL) {
        errf("retire requires a LABEL");
        return EX_USAGE;
    }
    /* First confirm the key exists at all (existence vs already-retired). */
    sqlite3_stmt *chk = NULL;
    sqlite3_prepare_v2(db, "SELECT 1 FROM bindings WHERE label=?1;",
                       -1, &chk, NULL);
    sqlite3_bind_text(chk, 1, label, -1, SQLITE_TRANSIENT);
    int exists = (sqlite3_step(chk) == SQLITE_ROW);
    sqlite3_finalize(chk);
    if (!exists) {
        fprintf(stderr, "intent-map: no such label: %s\n", label);
        return EX_NOTFOUND;
    }

    /* Soft-delete: tombstone the row, keep it, keep the key burned forever.
     * Idempotent — retiring an already-retired key is a harmless no-op. */
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "UPDATE bindings SET status='retired',retired_at=" NOW_SQL
        " WHERE label=?1 AND status='active';", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        errf(sqlite3_errmsg(db));
        return EX_DB;
    }
    sqlite3_bind_text(st, 1, label, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        errf(sqlite3_errmsg(db));
        return EX_DB;
    }
    return EX_OK;
}

/* ========================================================================= */
/* Help / discoverability (§5 + §5.1)                                        */
/* ========================================================================= */

static void print_main_help(void)
{
    fputs(
"intent-map — an agent-friendly, symbol-oriented binding store.\n"
"\n"
"Maps stable opaque keys (labels) to their meaning: a one-line `summary`\n"
"(glance tier) plus durable free-form `detail` (commit tier), with full-text\n"
"search, recency, and a constrained, durability-preserving lifecycle. Keys are\n"
"allocated once and never renumbered, reused, or hard-deleted.\n"
"\n"
"USAGE\n"
"  intent-map [--db PATH] <verb> [args]\n"
"  intent-map <verb> --help\n"
"\n"
"VERBS\n"
"  allocate [--label KEY] --summary STR [--detail STR]\n"
"        Create a binding. Minted mode (--label omitted) hands out the next\n"
"        monotonic L<ordinal> key. Caller-supplied mode (--label KEY) validates\n"
"        and binds the given key. Prints the bound binding's header line.\n"
"  search KEYWORD [KEYWORD ...]\n"
"        Full-text (not exact) lookup over summary+detail; keywords OR'd for\n"
"        recall. Prints the summary (glance) tier, one record header per match.\n"
"  get LABEL [LABEL ...]\n"
"        Exact fetch by key. Prints the full detail tier as a field record.\n"
"  index [--with-labels]\n"
"        Vocabulary table-of-contents: keyword->count by default, or each\n"
"        keyword followed by its matching label headers with --with-labels.\n"
"  recent [--limit N]\n"
"        Bindings by modified_at DESC (default N=20). Glance tier + timestamp.\n"
"  annotate LABEL [--summary STR] [--detail STR]\n"
"        Edit prose and bump modified_at. Cannot change the key binding.\n"
"  retire LABEL\n"
"        Soft-delete (tombstone). Keeps the row and burns the key forever.\n"
"        There is no hard delete and no key remap, by design.\n"
"\n"
"DATABASE\n"
"  Path resolves to: --db PATH, else $INTENT_MAP_DB, else ./intent-map.db.\n"
"  SQLite in WAL mode: concurrent readers + one writer, crash-safe, across\n"
"  co-located processes sharing the file.\n"
"\n"
"EXIT CODES\n"
"  0 ok   1 usage   2 invalid label   3 label conflict (burned key)\n"
"  4 not found   5 database error\n"
"\n"
"OUTPUT WIRE FORMAT (regular grammar; one leading sigil per line)\n"
"  A reader is a one-pass loop with switch(line[0]) over three regexes:\n"
"    ^@([^\\t]+)\\t(.*)$    record header -> (label, summary)\n"
"    ^:([^\\t]+)\\t(.*)$    field         -> (key, value first line)\n"
"    ^\\.(.*)$             continuation  -> append \"\\n\"+rest to current field\n"
"  Charset: labels and field keys are tab-free and newline-free, so the first\n"
"  tab exactly splits key from value; tabs inside a value are inert.\n"
"  Continuation invariant: every physical line of a multi-line value is\n"
"  prefixed with '.', and the producer never emits a bare value line. Given\n"
"  this, value prose may contain ANY character (@, :, ., tab, newline) with\n"
"  zero character-level escaping — newlines are re-wrapped, not escaped.\n"
"  `get` carries summary on the record header (its single home); its field\n"
"  keys are detail, status, created_at, modified_at, ordinal, retired_at.\n"
"  `index` emits ':'<keyword>TAB<count>. `recent` appends TAB<modified_at> to\n"
"  each header line.\n",
        stdout);
}

static void print_verb_help(const char *verb)
{
    if (strcmp(verb, "allocate") == 0) {
        fputs(
"intent-map allocate [--label KEY] --summary STR [--detail STR]\n"
"  Create a binding atomically (row + FTS index in one transaction).\n"
"  Minted (no --label): assigns the next monotonic key 'L'<ordinal>.\n"
"  Caller-supplied (--label KEY): validates charset (non-empty, no tab/newline)\n"
"    and uniqueness vs. active AND retired keys; rejects (nonzero, no write) on\n"
"    violation and never coerces. Re-using a retired key is forbidden.\n"
"  --summary is the one-line glance (no newline); --detail is free-form,\n"
"    multi-line prose.\n"
"  Prints: @<label>TAB<summary>. Exit: 0 ok, 1 usage, 2 invalid arg,\n"
"    3 conflict, 5 db.\n", stdout);
    } else if (strcmp(verb, "search") == 0) {
        fputs(
"intent-map search KEYWORD [KEYWORD ...]\n"
"  Full-text (not exact) lookup over summary+detail. Multiple keywords are\n"
"  OR'd (recall over precision). Returns active bindings only, ranked.\n"
"  Prints: @<label>TAB<summary> per match. Empty result is success (exit 0).\n",
            stdout);
    } else if (strcmp(verb, "get") == 0) {
        fputs(
"intent-map get LABEL [LABEL ...]\n"
"  Exact fetch by key (any status, including retired). Prints one record per\n"
"  found label: a header line (label + summary) plus fields detail, status,\n"
"  created_at, modified_at, ordinal, and retired_at (only if retired). Exit 4\n"
"  if any requested label does not exist.\n", stdout);
    } else if (strcmp(verb, "index") == 0) {
        fputs(
"intent-map index [--with-labels]\n"
"  Vocabulary overview, a live projection of the FTS data (active bindings).\n"
"  Default: ':'<keyword>TAB<count> per term, by descending count.\n"
"  --with-labels: each ':'<keyword>TAB<count> followed by its @<label>TAB<summary>\n"
"  header lines.\n", stdout);
    } else if (strcmp(verb, "recent") == 0) {
        fputs(
"intent-map recent [--limit N]\n"
"  Active bindings ordered by modified_at DESC (default N=20).\n"
"  Prints: @<label>TAB<summary>TAB<modified_at> per binding.\n", stdout);
    } else if (strcmp(verb, "annotate") == 0) {
        fputs(
"intent-map annotate LABEL [--summary STR] [--detail STR]\n"
"  Edit summary and/or detail; bumps modified_at and re-indexes the row.\n"
"  Cannot change the key binding (immutable by design). Exit 4 if no such\n"
"  label; exit 1 if neither --summary nor --detail is given.\n", stdout);
    } else if (strcmp(verb, "retire") == 0) {
        fputs(
"intent-map retire LABEL\n"
"  Soft-delete: sets status=retired, stamps retired_at, keeps the row, keeps\n"
"  the key burned forever. The only form of deletion; idempotent. Exit 4 if no\n"
"  such label.\n", stdout);
    } else {
        print_main_help();
    }
}

/* ========================================================================= */
/* Argument plumbing + dispatch                                              */
/* ========================================================================= */

/* Match "--name" (returns value from next arg, advances *i) or "--name=VALUE". */
static const char *opt_value(const char *name, int argc, char **argv, int *i)
{
    size_t nlen = strlen(name);
    const char *a = argv[*i];
    if (strncmp(a, name, nlen) == 0) {
        if (a[nlen] == '=')
            return a + nlen + 1;
        if (a[nlen] == '\0') {
            if (*i + 1 >= argc)
                return NULL; /* missing value */
            (*i)++;
            return argv[*i];
        }
    }
    return NULL;
}

static int has_help(int argc, char **argv)
{
    for (int i = 0; i < argc; ++i)
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            return 1;
    return 0;
}

/* Resolve the db path and strip "--db PATH"/"--db=PATH" from argv in place. */
static const char *extract_db_path(int *argc, char **argv)
{
    const char *path = getenv("INTENT_MAP_DB");
    int w = 0;
    for (int r = 0; r < *argc; ++r) {
        if (strcmp(argv[r], "--db") == 0 && r + 1 < *argc) {
            path = argv[r + 1];
            r++; /* skip value */
            continue;
        }
        if (strncmp(argv[r], "--db=", 5) == 0) {
            path = argv[r] + 5;
            continue;
        }
        argv[w++] = argv[r];
    }
    *argc = w;
    if (!path)
        path = "intent-map.db";
    return path;
}

int main(int argc, char **argv)
{
    /* argv[0] is the program; work over the remaining args. */
    int n = argc - 1;
    char **a = argv + 1;

    const char *db_path = extract_db_path(&n, a);

    if (n < 1) {
        print_main_help();
        return EX_USAGE;
    }

    const char *verb = a[0];

    if (strcmp(verb, "--help") == 0 || strcmp(verb, "-h") == 0 ||
        strcmp(verb, "help") == 0) {
        print_main_help();
        return EX_OK;
    }
    /* `<verb> --help` → verb-specific help, before touching the db. */
    if (has_help(n - 1, a + 1)) {
        print_verb_help(verb);
        return EX_OK;
    }

    sqlite3 *db = open_db(db_path);
    if (!db)
        return EX_DB;

    int rc;
    if (strcmp(verb, "allocate") == 0) {
        const char *label = NULL, *summary = NULL, *detail = NULL;
        for (int i = 1; i < n; ++i) {
            const char *v;
            if ((v = opt_value("--label", n, a, &i)))        label = v;
            else if ((v = opt_value("--summary", n, a, &i))) summary = v;
            else if ((v = opt_value("--detail", n, a, &i)))  detail = v;
            else { fprintf(stderr, "intent-map: unknown allocate arg: %s\n", a[i]);
                   sqlite3_close(db); return EX_USAGE; }
        }
        rc = do_allocate(db, label, summary, detail);
    } else if (strcmp(verb, "search") == 0) {
        rc = do_search(db, n - 1, a + 1);
    } else if (strcmp(verb, "get") == 0) {
        rc = do_get(db, n - 1, a + 1);
    } else if (strcmp(verb, "index") == 0) {
        int with_labels = 0;
        for (int i = 1; i < n; ++i) {
            if (strcmp(a[i], "--with-labels") == 0) with_labels = 1;
            else { fprintf(stderr, "intent-map: unknown index arg: %s\n", a[i]);
                   sqlite3_close(db); return EX_USAGE; }
        }
        rc = do_index(db, with_labels);
    } else if (strcmp(verb, "recent") == 0) {
        int limit = 20;
        for (int i = 1; i < n; ++i) {
            const char *v;
            if ((v = opt_value("--limit", n, a, &i))) limit = atoi(v);
            else { fprintf(stderr, "intent-map: unknown recent arg: %s\n", a[i]);
                   sqlite3_close(db); return EX_USAGE; }
        }
        rc = do_recent(db, limit);
    } else if (strcmp(verb, "annotate") == 0) {
        const char *label = NULL, *summary = NULL, *detail = NULL;
        for (int i = 1; i < n; ++i) {
            const char *v;
            if ((v = opt_value("--summary", n, a, &i)))      summary = v;
            else if ((v = opt_value("--detail", n, a, &i)))  detail = v;
            else if (a[i][0] != '-' && label == NULL)        label = a[i];
            else { fprintf(stderr, "intent-map: unknown annotate arg: %s\n", a[i]);
                   sqlite3_close(db); return EX_USAGE; }
        }
        rc = do_annotate(db, label, summary, detail);
    } else if (strcmp(verb, "retire") == 0) {
        const char *label = (n >= 2) ? a[1] : NULL;
        rc = do_retire(db, label);
    } else {
        fprintf(stderr, "intent-map: unknown verb: %s\n", verb);
        print_main_help();
        rc = EX_USAGE;
    }

    sqlite3_close(db);
    return rc;
}
