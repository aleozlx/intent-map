# intent-map — Design Document

**Status:** ready for implementation
**Audience:** coding-session handoff (build from this doc alone)
**Deliverable:** a single executable `intent-map` wrapping a SQLite store

---

## 1. What it is

`intent-map` is an **agent-friendly, symbol-oriented binding store**: it maps
stable opaque **keys** to their **meaning** (a one-line summary plus durable
free-form detail), with full-text search, recency, and a constrained,
durability-preserving operation set. It is built to be driven by an agent shelling
out to a single binary, with a parse-light line grammar for output.

Typical use: a producer emits artifacts whose symbols are short, stable tokens
(assembly labels, compiler-mangled C++ names, minified identifiers, feature-flag
codes, experiment IDs), and keeps the meaning and design reasoning for each token
in `intent-map`, retrievable later by key, keyword, or recency.

The store is **symbol-agnostic**: a key is just a stable opaque token. The store
neither parses nor assigns meaning to the key string. (One downstream consequence,
not a goal of the tool: because keys carry no meaning, the published symbols reveal
no intent on their own — see §12.)

---

## 2. What this is and is not

**Is:** a metadata binding store + a thin CLI over SQLite. Low-frequency lookups
keyed by symbol or keyword. Single executable, line-grammar I/O.

**Is not:** a hot-path data structure or a public-facing API. There is no
performance-critical inner loop — the bottleneck is occasional I/O, never CPU. Do
not hand-roll storage or concurrency; SQLite owns both.

---

## 3. Core concepts

### 3.1 The binding
A **binding** links one opaque key to its meaning:

| field        | meaning                                                        |
|--------------|----------------------------------------------------------------|
| `label`      | the opaque symbol/token. Stable key.                          |
| `summary`    | one-line meaning. Glance tier.                                |
| `detail`     | full durable reasoning: approach, rejected alternatives, rationale. Commit tier. |
| `status`     | `active` or `retired`. Never hard-deleted.                     |
| `created_at` | allocation timestamp.                                          |
| `modified_at`| last-edit timestamp. Drives recency.                          |

Keys come from one of two `allocate` modes (see §5): the store **mints** a compact
monotonic `L1, L2, …` token when the caller provides none — for callers that
cannot or should not name keys themselves, like a compiler that just needs a fresh
slot — or the caller **supplies** its own key, which the store validates and binds.
The token can be anything stable and opaque: an assembly label, a C++ mangled
symbol name (e.g. `_ZN3foo3barEi`), a minified identifier, a flag code. They all
fit the same slot; the store does not care about the form.

### 3.2 Sticky keys
Keys are **allocated once and never renumbered or reused.** A retired key stays
burned. This is what makes the store durable: any stored reference to a key — in
notes, in other bindings, in the producer's own memory across sessions — resolves
to the same concept forever. Renumbering or reuse would scramble that. It is the
one property the tool must make impossible to violate.

### 3.3 Two-tier read model
`summary` is cheap context (load many at once to orient); `detail` is the full
reasoning (pull only for the binding about to be touched). The verb you call
determines the tier you get — see §5. This is lazy-loading encoded in the API.

### 3.4 Key location is not tracked
A key *is* its own locator wherever it is used — found by name (a plain text
search). Positions/line numbers are derived, redundant, and deliberately **not**
stored. A binding whose key is absent from the current artifact is harmless — look
it up, then revive or retire. Store and artifact reconcile lazily.

---

## 4. Storage substrate — SQLite (WAL mode)

SQLite is chosen because it already solves the two hard requirements that a
hand-rolled structure (or a bare mutex) does not:

- **Concurrent multi-actor RW.** `PRAGMA journal_mode=WAL` gives concurrent
  readers alongside a single writer, cross-process, with no torn reads. Multiple
  caller processes each open the db, run a short transaction, close. WAL serializes
  writers; the OS handles the rest. **Zero additional coordination code.**
- **Crash-safe atomic writes.** ACID transactions mean a process dying mid-write
  leaves the store consistent. A mutex protects an in-memory critical section; it
  does *not* make a disk write durable or coordinate across processes. That gap is
  exactly where a bare mutex fails and a database does not.

Keyword search uses **FTS5** (native full-text index) over `summary` + `detail` —
no hand-rolled tokenizer.

> **Concurrency scope assumption:** co-located processes sharing one db file on a
> local filesystem. WAL covers this natively. If access ever goes cross-machine,
> SQLite-on-a-shared-file breaks down and the substrate becomes a client-server DB
> (Postgres, or SQLite behind a small service) — but the CLI contract in §5 stays
> identical, so that swap never reaches callers. Out of scope for v1.

---

## 5. CLI contract

One executable, subcommands, **line-oriented grammar on stdout** (see §5.1), exit
code carries success/failure. The output format is a deliberately minimal regular
grammar: leaner than JSON for an LLM consumer (no syntax tokens repeated per
record), and escaping-free for arbitrary value prose by construction.

The operation set is a **constrained binding lifecycle**, not generic CRUD — the
constraints (no key remap, no hard delete) are features that preserve durability,
not gaps to be filled. See §6.

```
intent-map allocate [--label KEY] --summary STR --detail STR   -> @<label>   (minted if --label omitted)
intent-map search KEYWORD [KEYWORD ...]                -> @<label> TAB <summary>   (one per line)
intent-map get LABEL [LABEL ...]                       -> record per label (header + fields, see §5.1)
intent-map index [--with-labels]                       -> :<keyword> TAB <count>  (or @<label> lines under each)
intent-map recent [--limit N]                          -> @<label> TAB <summary> TAB <modified_at>
intent-map annotate LABEL [--summary STR] [--detail STR]
intent-map retire LABEL
```

### 5.1 Output wire format (regular grammar)

The output is a **line-oriented format with a single leading sigil per line.**
Structure lives only in column zero (the sigil) and the first tab; everything
after is verbatim. This makes the format a regular grammar — one regex classifies
each line — and **escaping-free for value content** (see invariant below).

```
record       := header field*
header       := '@' label TAB summary NL
field        := ':' key TAB value-first NL continuation*
continuation := '.' value-rest NL
```

Per-line classification — a reader is a one-pass loop with `switch (line[0])`:

```
^@([^\t]+)\t(.*)$     new record   -> (label, summary)
^:([^\t]+)\t(.*)$     field        -> (key, value first line)
^\.(.*)$              continuation -> append "\n" + rest to current field
```

**Charset constraints (make the first-tab split exact, not heuristic):**
- `label` is tab-free and newline-free (opaque keys are single tokens by nature —
  asm labels, mangled symbols, flag codes all qualify).
- `key` is a fixed closed vocabulary (`summary`, `detail`, `modified_at`, …) —
  no tab, no newline.
- Because keys/labels are tab-free, "first tab splits key from value" is a
  *corollary* of the charset, and tabs inside a value are inert (swallowed by `.*`).

**The single writer invariant (the entire escaping burden, in one sentence):**
> Every physical line of a multi-line value is prefixed with `.`; the producer
> never emits a bare value line.

Given this, value prose may contain **any** character — `@`, `:`, `.`, tabs,
embedded newlines — with zero character-level escaping. A newline in a value is
not escaped; it is *re-wrapped* into `.`-prefixed continuation lines. A value line
that happens to start with `@`/`:`/`.` is safe because the structural sigil is the
`.` in column zero; the value's own leading char sits in column one. Escaping is
**positional and uniform** (one sigil per line), never content-dependent scanning.

The only hard rule, trivially satisfied: keys and labels never contain tab or
newline — and they are a controlled vocabulary the producer mints.

### Verb semantics

- **`allocate`** — create a binding, in one of two modes, atomically (write the
  row, FTS index updates fall out, all in one transaction):
  - **Minted (`--label` omitted):** the allocator hands out the next `L?` key from
    the monotonic ordinal sequence. Guaranteed unique and charset-valid, zero
    caller burden. This is the path for callers that cannot or should not name keys
    themselves — e.g. a compiler that just needs a fresh stable slot.
  - **Caller-supplied (`--label KEY`):** the caller provides the key (e.g. a
    compiler-mangled symbol). `allocate` **validates before binding** — charset
    (tab-free, newline-free per §5.1) and uniqueness against **both active and
    retired** keys (the burned-key rule forbids re-supplying a retired key). On any
    violation it rejects with a nonzero exit and writes nothing; it never silently
    coerces the key, since that would hand back a different key than the caller
    bound.

  Returns the bound key either way.

- **`search KEYWORD…`** — the load-bearing verb. Keyword (not exact-match) lookup
  over the FTS index; recall over precision. **Batch:** pass multiple keywords.
  Returns the **summary** tier (cheap glance), keyed by label.

- **`get LABEL…`** — batch exact fetch by key. Returns the **detail** tier
  (full reasoning). The commit-tier read, used when entering a region.

- **`index`** — overview / table-of-contents over the store's vocabulary.
  Default returns keyword→count (cheap orientation). `--with-labels` returns
  keyword→labels for drill-down. The index is a **projection of the FTS data**,
  never a separately maintained structure, so it cannot drift from the bindings.

- **`recent`** — bindings ordered by `modified_at DESC`, limited to `N` (default
  e.g. 20). Answers "what did I touch last session" — the everyday context-reload
  read. Returns label + summary + timestamp (glance tier). Cheap: one indexed
  `ORDER BY`. A first-class shipped verb.

- **`annotate LABEL`** — edit `summary` and/or `detail` freely; bumps
  `modified_at`; re-indexes that row. **Cannot change the key binding** — the
  key↔concept link is immutable once allocated.

- **`retire LABEL`** — soft-delete. Sets `status=retired`, stamps `retired_at`,
  keeps the row, keeps the key burned. Never frees the key for reuse. The *only*
  form of deletion; hard delete does not exist, because it would orphan stored
  references.

### Discoverability
`intent-map --help` and `intent-map VERB --help` are the complete discoverability
layer — sufficient for a shell-native caller, no separate manifest. Help text must
document every verb, its args, exit codes, and **the output wire format as an
explicit regular grammar** (the three line-regexes of §5.1, the charset
constraints, and the continuation-prefix invariant) so a consumer can parse output
correctly from `--help` alone.

---

## 6. Interface design rationale (why the surface is this small)

The operation set is a constrained lifecycle, not generic CRUD. Two operations are
**deliberately excluded** because they break durability:

- **edit-the-key / remap** — would let a key point at a different concept;
  stored references would silently rot. Bindings are immutable in their key.
- **hard delete** — would orphan any reference to that key. Replaced by `retire`
  (tombstone).

The tool's constraints *are* the design. By not offering the dangerous verbs, the
durability guarantees are enforced by mechanism, not discipline. An implementer
should resist "completing the CRUD" — the missing verbs are intentional.

---

## 7. Key semantics (agnostic by design)

The store assigns **no meaning to the key string**. Default keys are compact
monotonic tokens (`L1, L2, …`); callers may supply their own. This agnosticism is
what lets the same store back assembly labels, compiler-mangled C++ symbols,
minified identifiers, or flag codes interchangeably — the meaning lives in
`summary`/`detail`, never in the key.

A note on what this *enables* downstream (a property of how a caller uses it, not a
function of the tool): since keys carry no intent, a producer can publish artifacts
that use only the keys while keeping all meaning in the store. Whether to exploit
that is the caller's deployment choice; the tool neither requires nor enforces it.

Out of scope (v1): the tool does nothing about structural inference (control-flow
shape, data-access patterns). It binds keys to meaning; it is not an obfuscator.

---

## 8. Deployment

- **Implementation language: C.** SQLite *is* a C library, so a C wrapper is the
  most native host — `#include <sqlite3.h>`, call `sqlite3_open_v2` /
  `sqlite3_prepare_v2` / `sqlite3_bind_*` / `sqlite3_step` directly. No FFI, no
  interpreter, wrapper and engine in one language. Produces a true single static
  binary with no runtime to bundle.
  - **Tradeoff vs. Python:** in C you link SQLite yourself. Output is the §5.1
    line grammar, not JSON — emission is plain `printf("@%s\t%s\n", ...)` plus a
    continuation-wrapping helper for multi-line values. No JSON library, no JSON
    escaping. Simpler in C than JSON would have been. Python would have given
    stdlib `sqlite3` but bundling overhead for the single-binary goal. C is the
    deliberate primary choice.
  - **FTS5 must be compiled in.** Build the SQLite amalgamation with
    `-DSQLITE_ENABLE_FTS5`, or ensure the linked system `libsqlite3` includes FTS5
    (most do). `search` and `index` depend on it.
- **Stable contract = swappable language.** The argv-in / line-grammar-out
  boundary is the real interface; callers are indifferent to the implementation
  language. A Python or Go reimplementation could replace the binary unnoticed.
- **Distribution:** single self-contained executable. Shipping one binary is
  **orthogonal to** SQLite managing data on disk — the engine is linked in; the
  data lives beside it as `intent-map.db` (+ WAL sidecar).
- **Data location:** db path configurable (env var or flag), defaulting to a
  conventional location. The db is the caller's private working store.

---

## 9. Proposed schema (implementer's starting point)

```sql
PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

CREATE TABLE bindings (
    -- ordinal is the rowid: signed 64-bit, AUTOINCREMENT guarantees monotonic,
    -- never-reused values even across deletes (high-water mark in sqlite_sequence).
    -- This makes "never reuse" a property of the schema, not of insert logic.
    ordinal      INTEGER PRIMARY KEY AUTOINCREMENT,
    label        TEXT NOT NULL UNIQUE,    -- opaque key; the real lookup key (unique index)
    summary      TEXT NOT NULL,
    detail       TEXT NOT NULL DEFAULT '',
    status       TEXT NOT NULL DEFAULT 'active'   -- 'active' | 'retired'
                  CHECK (status IN ('active','retired')),
    created_at   TEXT NOT NULL,             -- ISO-8601
    modified_at  TEXT NOT NULL,
    retired_at   TEXT                        -- NULL unless retired
);

-- Full-text index powering `search` and `index`. Kept in sync via triggers
-- (or rebuilt in the same transaction as writes) so it cannot drift.
-- content_rowid ties to ordinal (the rowid) — the natural external-content join.
CREATE VIRTUAL TABLE bindings_fts USING fts5(
    label UNINDEXED,
    summary,
    detail,
    content='bindings',
    content_rowid='ordinal'
);
```

**Key allocation:** `ordinal` is the table's rowid — `INTEGER PRIMARY KEY
AUTOINCREMENT`, signed 64-bit (max 9223372036854775807). Every binding gets the
next ordinal automatically on insert, minted or caller-supplied; the engine, not
the wrapper, assigns it. `AUTOINCREMENT` guarantees the value is **monotonic and
never reused even across deletes**, so the sticky-key invariant is enforced by the
schema rather than by allocation code.

- **Minted mode (`--label` omitted):** insert the row, then set
  `label = 'L' || ordinal` from the just-assigned rowid (`last_insert_rowid()`),
  in the same transaction. The label *is* the ordinal, prefixed.
- **Caller-supplied mode (`--label KEY`):** validate (charset + uniqueness vs.
  active *and* retired — the `UNIQUE` index covers existing rows; retired rows are
  still present as tombstones so they collide correctly) and bind the caller's key.
  The row still consumes an ordinal; it just isn't reflected in the label.

The WAL-serialized writer plus the single insert transaction make concurrent
`allocate` calls collision-free. Range is effectively unbounded for this use (a
binding per concept tops out in the thousands-to-millions; 64-bit is ~292,000
years at a million allocations/second). On the unreachable event of exhaustion,
`AUTOINCREMENT` returns `SQLITE_FULL` rather than wrapping or reusing — the correct
refusal for a never-reuse store.

---

## 10. Deferred (designed, not built)

- **Retention policy (pruning).** `modified_at` is recorded and recency ships as a
  first-class verb (§5 `recent`), but an automated *pruning* pass is premature for
  a store of tens-to-hundreds of bindings. If built later, it must **flag/report
  stale bindings, not reap them** — cold ≠ dead; a stable foundational binding is
  untouched precisely because it is correct. Any automated action may only
  *tombstone* (status transition), never hard-delete or free a key. Defer until the
  store actually feels cluttered (it may never).

---

## 10a. Testing (pytest, black-box over the binary)

Unit/contract tests are written in **pytest**, driving the compiled `intent-map`
binary as a **black box**: each test shells out (`subprocess.run`), passes argv,
and asserts on captured stdout + exit code. The tested surface is the CLI contract
(§5) and the wire grammar (§5.1) — not C internals. This mirrors real usage
(argv in, line-grammar out) and makes the §11 acceptance items executable.

- **Isolation:** one throwaway db per test via a `tmp_path` fixture, passed through
  the configurable db path (§8 env var / flag). No shared state, no ordering
  dependence between tests.
- **Harness:** a thin wrapper `run(*args) -> (exit_code, stdout)` plus a reference
  parser implementing the three §5.1 regexes, returning structured records. Writing
  this parser doubles as validation that the grammar is parseable exactly as
  specified — it is the reference reader for the format.
- **Build-then-test:** `make test` compiles the binary, then runs pytest against
  it; this is the CI gate. (Fits the existing `make run` / `make test` convention.)
- **Highest-value cases** (beyond mapping each §11 item to a test):
  - *Escaping-free round-trip (property-style):* generate `detail` strings
    containing newlines, tabs, and leading `@`/`:`/`.` chars; `allocate` then `get`;
    assert the returned detail is byte-identical. This is the real proof of the
    §5.1 writer invariant.
  - *Concurrency:* spawn N concurrent `allocate` subprocesses; assert all returned
    labels are distinct and ordinals form a contiguous set — proving WAL + the
    insert transaction actually serialize minting.
  - *Never-reuse across deletes:* allocate, force-delete the highest row directly
    in SQLite, allocate again; assert the new ordinal exceeds the deleted one
    (AUTOINCREMENT high-water mark), never reused.



- [ ] `allocate` (minted, `--label` omitted) sets `label = 'L' || ordinal` from the autoincrement rowid within one transaction; concurrent calls never collide or reuse, including across prior deletes (AUTOINCREMENT high-water mark).
- [ ] `allocate --label KEY` validates charset + uniqueness (vs. active *and* retired) and rejects (nonzero, no write) on violation; never silently coerces.
- [ ] `ordinal` is `INTEGER PRIMARY KEY AUTOINCREMENT` (the rowid); every binding consumes one; FTS `content_rowid` joins on it.
- [ ] `search` does keyword (not exact) matching, accepts multiple keywords, returns summaries.
- [ ] `get` accepts multiple keys, returns summary+detail.
- [ ] `index` returns counts by default, label lists with `--with-labels`, never drifts from bindings.
- [ ] `recent` returns bindings ordered by `modified_at DESC`, honoring `--limit`.
- [ ] `annotate` edits prose + bumps `modified_at`; offers no way to change a key binding.
- [ ] `retire` tombstones, keeps the row, keeps the key burned; no hard-delete path exists anywhere.
- [ ] WAL enabled; two concurrent processes can read while one writes without corruption or torn reads.
- [ ] All output conforms to the §5.1 line grammar; the three regexes parse every line; exit codes distinguish success/failure.
- [ ] Multi-line value prose round-trips: a `detail` containing newlines and leading `@`/`:`/`.` chars emits as `.`-prefixed continuations and reads back byte-identical, with no character-level escaping.
- [ ] `--help` documents every verb, args, exit codes, and the output grammar (regexes + charset + continuation invariant).
- [ ] Builds to a single executable; db created/opened at a configurable path.
- [ ] pytest suite drives the compiled binary as a black box; each test isolated via a per-test temp db (configurable path).
- [ ] `make test` builds the binary then runs pytest; passes as the CI gate.
- [ ] Suite includes the property-style escaping round-trip, the concurrent-allocate distinctness test, and the never-reuse-across-deletes test.

---

## 12. Incidental capabilities (no scope change)

**Implementer note:** do **not** specialize the implementation to any one artifact
kind. The store is symbol-agnostic — `label` is an opaque sticky key,
`summary`/`detail` are arbitrary prose. Keep the core generic: don't hard-code key
*forms* (the `L[0-9]+` scheme is only the *default* allocator; accept
caller-supplied keys too), and don't name internal functions after any single
domain. The following all work **today, with zero new verbs, zero schema changes,
zero new constraints** — the same tool pointed at a different domain:

- **Symbol → meaning dictionary across languages.** assembly labels,
  compiler-mangled C++ symbols (`_ZN3foo3barEi`), minified JS identifiers,
  feature-flag codes, experiment IDs. One store, any symbol form.
- **Append-only decision log (ADR-lite).** key = decision ID, summary = the
  decision, detail = rationale + rejected alternatives. `retire` = superseded; the
  burned-key rule means a superseded decision never loses its ID — a clean audit
  trail. (Caveat: `annotate` overwrites, so edit *history* of one decision is lost
  — see not-free list.)
- **Codebase / team glossary.** term → definition, keyword-searchable; `index` is
  the concept table-of-contents. Free onboarding artifact.
- **Shared durable memory for multiple cooperating processes/agents.** Stripped of
  any domain framing this is a keyed, full-text-searchable, recency-fed, two-tier
  (glance/commit) notes store that is *already concurrent-safe across processes*
  via WAL. The highest-value incidental capability — shared durable memory is hard
  to build and falls out here for free. Worth not foreclosing.
- **Bookmark / snippet / reference stash + tiny personal FTS index.** Because FTS5
  is compiled in, any prose put in summary/detail is ranked-full-text searchable.

**NOT free (do not let these creep into v1 — they need new structure):**
- Relationships/links between bindings (call graphs, supersedes-edges) → needs a
  join table. The moment you want a *graph*, you are past scope.
- Version history of a single binding's `detail` → `annotate` overwrites; a trail
  needs an append-only revisions table.
- Multi-project namespacing → one db = one flat namespace; use separate db files,
  not a `namespace` column, to stay free.
- Access control between callers → WAL gives concurrency, not authorization.
  Anyone who can open the file has full RW. Fine for cooperating callers; not a
  security boundary.

One-line characterization: *an agent-friendly, symbol-agnostic, durable,
full-text-searchable, concurrent, two-tier key→meaning store with recency and a
constrained lifecycle.* Everything above is a consequence of that shape.
