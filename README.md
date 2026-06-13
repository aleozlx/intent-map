# intent-map

[![CI](https://github.com/aleozlx/intent-map/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/aleozlx/intent-map/actions/workflows/ci.yml)

An agent-friendly, symbol-oriented binding store: a single executable wrapping
a SQLite (WAL) store that maps stable opaque **keys** (`label`s) to their
**meaning** — a one-line `summary` (glance tier) plus durable free-form
`detail` (commit tier) — with full-text search, recency, and a constrained,
durability-preserving operation set.

It is built to be driven by an agent shelling out to one binary, with a
parse-light line grammar on stdout. The store is **symbol-agnostic**: a key is
just a stable opaque token (an assembly label, a C++ mangled symbol like
`_ZN3foo3barEi`, a minified identifier, a feature-flag code, an experiment ID).
The meaning lives in `summary`/`detail`, never in the key.

See [`DESIGN-intent-map.md`](DESIGN-intent-map.md) for the full design and
rationale.

## Build

Requires a C compiler and a SQLite library with **FTS5** compiled in (most
distro `libsqlite3` builds qualify; verified at build time via `pkg-config`).

```sh
make build        # compiles ./intent-map, linking system libsqlite3
make test         # builds, then runs the pytest black-box suite (CI gate)
make run ARGS="…" # build + run, e.g. make run ARGS="search coolant"
make clean
```

## Database location

The db path resolves to, in order: the `--db PATH` flag, then `$INTENT_MAP_DB`,
then `./intent-map.db`. SQLite WAL mode gives concurrent readers alongside a
single writer, crash-safe, across co-located processes sharing the file.

## Verbs

```
intent-map allocate [--label KEY] --summary STR [--detail STR]
intent-map search KEYWORD [KEYWORD ...]
intent-map get LABEL [LABEL ...]
intent-map index [--with-labels]
intent-map recent [--limit N]
intent-map annotate LABEL [--summary STR] [--detail STR]
intent-map retire LABEL
```

- **`allocate`** — create a binding atomically. *Minted* (no `--label`) hands
  out the next monotonic `L<ordinal>` key — for callers that just need a fresh
  stable slot. *Caller-supplied* (`--label KEY`) validates the key (non-empty,
  no tab/newline) and its uniqueness against **both active and retired** keys,
  rejecting (nonzero, no write) on any violation. Prints the bound header line.
- **`search`** — full-text (not exact) lookup over `summary`+`detail`; multiple
  keywords are OR'd for recall. Returns the glance tier (active bindings).
- **`get`** — exact fetch by key (any status). Returns the full detail tier.
- **`index`** — vocabulary table-of-contents: `keyword → count` by default, or
  each keyword followed by its matching labels with `--with-labels`. A live
  projection of the FTS data, so it cannot drift from the bindings.
- **`recent`** — bindings by `modified_at DESC` (default `N=20`); the everyday
  context-reload read.
- **`annotate`** — edit `summary`/`detail` and bump `modified_at`. **Cannot**
  change the key binding — the key↔concept link is immutable once allocated.
- **`retire`** — soft-delete (tombstone): keeps the row, keeps the key burned
  forever, removes it from discovery. The *only* form of deletion; there is no
  hard delete and no key remap, by design (see §6 of the design doc).

`intent-map --help` and `intent-map VERB --help` are the complete
discoverability layer — including the exit codes and the wire grammar below.

### Exit codes

| code | meaning |
|------|---------|
| 0 | success |
| 1 | usage error (bad/missing args, unknown verb) |
| 2 | invalid caller-supplied label (charset) |
| 3 | label conflict — key already in use (active or retired/burned) |
| 4 | not found — `get`/`annotate`/`retire` on a nonexistent label |
| 5 | database error |

## Output wire format (§5.1)

A line-oriented format with **a single leading sigil per line**. Structure
lives only in column zero (the sigil) and the first tab; everything after is
verbatim. A reader is a one-pass loop, `switch(line[0])`, over three regexes:

```
^@([^\t]+)\t(.*)$     record header  -> (label, summary)
^:([^\t]+)\t(.*)$     field          -> (key, value first line)
^\.(.*)$              continuation   -> append "\n" + rest to current field
```

Labels and field keys are tab-free and newline-free, so "the first tab splits
key from value" is exact and tabs inside a value are inert.

**The single writer invariant (the entire escaping burden, in one sentence):**
every physical line of a multi-line value is prefixed with `.`, and the
producer never emits a bare value line. Given this, value prose may contain
**any** character — `@`, `:`, `.`, tabs, embedded newlines — with zero
character-level escaping: a newline is *re-wrapped* into `.`-prefixed
continuation lines, never escaped.

Example — `get` on a binding whose `detail` spans multiple lines:

```
@L2	world peace plan
:detail	multi
.line
.detail
:status	active
:created_at	2026-06-13T20:54:16.782Z
:modified_at	2026-06-13T20:54:16.782Z
:ordinal	2
```

`summary` is the one-line glance tier and lives only on the header line (its
single home); the fields carry `detail` and metadata. No value is emitted twice.

`tests/imap.py` is the reference reader implementing exactly these three
regexes.

## Example

```sh
$ export INTENT_MAP_DB=/tmp/notes.db
$ intent-map allocate --summary "thermal regulator" --detail "controls coolant flow"
@L1	thermal regulator
$ intent-map allocate --label _ZN3foo3barEi --summary "foo::bar(int) overload"
@_ZN3foo3barEi	foo::bar(int) overload
$ intent-map search coolant
@L1	thermal regulator
$ intent-map retire L1          # tombstone; key L1 stays burned forever
```
