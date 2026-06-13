"""Black-box contract tests for `intent-map`.

Each test drives the compiled binary as a black box (argv in, line-grammar out)
against an isolated per-test db. Tests map directly to the §11 acceptance items
of DESIGN-intent-map.md, plus the three highest-value cases (escaping-free
round-trip, concurrent-allocate distinctness, never-reuse-across-deletes).
"""
import concurrent.futures
import sqlite3
import threading

import pytest

import imap


def _label(out):
    recs = imap.parse_records(out)
    assert len(recs) == 1, out
    return recs[0].label


def _ordinal(run, label):
    code, out, err = run("get", label)
    assert code == 0, err
    return int(imap.parse_records(out)[0].fields["ordinal"])


# --------------------------------------------------------------------------- #
# allocate — minted                                                           #
# --------------------------------------------------------------------------- #

def test_allocate_minted_sets_L_ordinal_label(run):
    """Minted (no --label) sets label = 'L' || ordinal from the rowid."""
    assert _label(run("allocate", "--summary", "a")[1]) == "L1"
    assert _label(run("allocate", "--summary", "b")[1]) == "L2"
    assert _label(run("allocate", "--summary", "c")[1]) == "L3"
    assert _ordinal(run, "L2") == 2


def test_allocate_emits_grammar_valid_header(run):
    code, out, _ = run("allocate", "--summary", "hello", "--detail", "d")
    assert code == 0
    imap.assert_grammar(out)
    rec = imap.parse_records(out)[0]
    assert rec.label == "L1"
    assert rec.summary == "hello"


# --------------------------------------------------------------------------- #
# allocate — caller-supplied validation + uniqueness                          #
# --------------------------------------------------------------------------- #

def test_allocate_supplied_label_bound(run):
    code, out, _ = run("allocate", "--label", "_ZN3foo3barEi", "--summary", "s")
    assert code == 0
    assert _label(out) == "_ZN3foo3barEi"


def test_allocate_supplied_rejects_bad_charset(run):
    # tab in label
    code, _, _ = run("allocate", "--label", "bad\ttab", "--summary", "s")
    assert code == 2
    # newline in label
    code, _, _ = run("allocate", "--label", "bad\nnl", "--summary", "s")
    assert code == 2
    # empty label
    code, _, _ = run("allocate", "--label", "", "--summary", "s")
    assert code == 2


def test_allocate_supplied_rejects_duplicate_active(run):
    assert run("allocate", "--label", "K", "--summary", "first")[0] == 0
    code, _, _ = run("allocate", "--label", "K", "--summary", "again")
    assert code == 3


def test_allocate_supplied_rejects_duplicate_retired(run):
    assert run("allocate", "--label", "K", "--summary", "first")[0] == 0
    assert run("retire", "K")[0] == 0
    # burned-key rule: a retired key still collides and cannot be re-supplied
    code, _, _ = run("allocate", "--label", "K", "--summary", "revive?")
    assert code == 3


def test_allocate_rejection_writes_nothing(run):
    assert run("allocate", "--label", "K", "--summary", "first")[0] == 0
    before = run("recent", "--limit", "100")[1]
    assert run("allocate", "--label", "bad\ttab", "--summary", "x")[0] == 2
    assert run("allocate", "--label", "K", "--summary", "x")[0] == 3
    after = run("recent", "--limit", "100")[1]
    assert before == after


def test_ordinal_consumed_even_for_supplied_label(run):
    """Every binding consumes an ordinal, minted or supplied."""
    assert _ordinal(run, _label(run("allocate", "--summary", "m1")[1])) == 1
    run("allocate", "--label", "sym", "--summary", "s")
    assert _ordinal(run, "sym") == 2
    assert _ordinal(run, _label(run("allocate", "--summary", "m3")[1])) == 3


# --------------------------------------------------------------------------- #
# search                                                                      #
# --------------------------------------------------------------------------- #

def test_search_is_keyword_not_exact(run):
    # keyword lives in detail, not the label — full-text, not exact-key match
    run("allocate", "--summary", "thermal regulator", "--detail", "controls coolant flow")
    hits = imap.parse_search(run("search", "coolant")[1])
    assert hits == [("L1", "thermal regulator")]


def test_search_multiple_keywords_ored_for_recall(run):
    run("allocate", "--summary", "alpha widget", "--detail", "x")
    run("allocate", "--summary", "beta gadget", "--detail", "y")
    labels = {l for l, _ in imap.parse_search(run("search", "alpha", "beta")[1])}
    assert labels == {"L1", "L2"}


def test_search_empty_result_is_success(run):
    run("allocate", "--summary", "something", "--detail", "x")
    code, out, _ = run("search", "nonexistentterm")
    assert code == 0
    assert out == ""


# --------------------------------------------------------------------------- #
# get                                                                         #
# --------------------------------------------------------------------------- #

def test_get_batch_returns_summary_and_detail(run):
    run("allocate", "--summary", "s1", "--detail", "d1")
    run("allocate", "--summary", "s2", "--detail", "d2")
    code, out, _ = run("get", "L1", "L2")
    assert code == 0
    recs = imap.parse_records(out)
    assert len(recs) == 2
    # summary is on the header, detail in a field; neither value appears twice
    assert (recs[0].summary, recs[0].fields["detail"]) == ("s1", "d1")
    assert (recs[1].summary, recs[1].fields["detail"]) == ("s2", "d2")
    assert "summary" not in recs[0].fields


def test_get_missing_label_is_nonzero(run):
    run("allocate", "--summary", "s")
    code, _, _ = run("get", "NOPE")
    assert code == 4
    # partial: one good, one missing -> still nonzero, good record printed
    code, out, _ = run("get", "L1", "NOPE")
    assert code == 4
    assert imap.parse_records(out)[0].label == "L1"


# --------------------------------------------------------------------------- #
# index                                                                       #
# --------------------------------------------------------------------------- #

def test_index_counts_by_default(run):
    run("allocate", "--summary", "apple banana", "--detail", "")
    run("allocate", "--summary", "banana cherry", "--detail", "")
    counts = imap.parse_index(run("index")[1])
    assert counts["banana"] == 2
    assert counts["apple"] == 1
    assert counts["cherry"] == 1


def test_index_with_labels(run):
    run("allocate", "--summary", "apple banana", "--detail", "")
    run("allocate", "--summary", "banana cherry", "--detail", "")
    groups = imap.parse_index_labels(run("index", "--with-labels")[1])
    count, members = groups["banana"]
    assert count == 2
    assert {lab for lab, _ in members} == {"L1", "L2"}


def test_index_does_not_drift_from_bindings(run):
    run("allocate", "--summary", "initial", "--detail", "")
    assert "freshword" not in imap.parse_index(run("index")[1])
    run("annotate", "L1", "--detail", "freshword added")
    assert "freshword" in imap.parse_index(run("index")[1])


# --------------------------------------------------------------------------- #
# recent                                                                      #
# --------------------------------------------------------------------------- #

def test_recent_orders_by_modified_desc(run):
    run("allocate", "--summary", "first")
    run("allocate", "--summary", "second")
    run("allocate", "--summary", "third")
    # touch L1 so it becomes the most recently modified
    run("annotate", "L1", "--summary", "first edited")
    order = [lab for lab, _, _ in imap.parse_recent(run("recent")[1])]
    assert order[0] == "L1"
    assert set(order) == {"L1", "L2", "L3"}


def test_recent_honors_limit(run):
    for i in range(5):
        run("allocate", "--summary", f"s{i}")
    rows = imap.parse_recent(run("recent", "--limit", "2")[1])
    assert len(rows) == 2


# --------------------------------------------------------------------------- #
# annotate                                                                    #
# --------------------------------------------------------------------------- #

def test_annotate_edits_prose_and_bumps_modified(run):
    run("allocate", "--summary", "old", "--detail", "olddetail")
    before = imap.parse_records(run("get", "L1")[1])[0].fields["modified_at"]
    assert run("annotate", "L1", "--summary", "new", "--detail", "newdetail")[0] == 0
    rec = imap.parse_records(run("get", "L1")[1])[0]
    assert rec.summary == "new"
    assert rec.fields["detail"] == "newdetail"
    assert rec.fields["modified_at"] >= before
    # the key binding is untouched
    assert rec.label == "L1"
    assert rec.fields["ordinal"] == "1"


def test_annotate_one_field_only(run):
    run("allocate", "--summary", "keepme", "--detail", "origdetail")
    run("annotate", "L1", "--detail", "changedonly")
    rec = imap.parse_records(run("get", "L1")[1])[0]
    assert rec.summary == "keepme"
    assert rec.fields["detail"] == "changedonly"


def test_annotate_offers_no_key_remap(run):
    run("allocate", "--summary", "s")
    # there is no flag to change the key binding; --label is not accepted
    code, _, _ = run("annotate", "L1", "--label", "L2")
    assert code != 0


def test_annotate_missing_label_nonzero(run):
    code, _, _ = run("annotate", "NOPE", "--summary", "x")
    assert code == 4


# --------------------------------------------------------------------------- #
# retire                                                                      #
# --------------------------------------------------------------------------- #

def test_retire_tombstones_keeps_row_and_status(run):
    run("allocate", "--summary", "to retire", "--detail", "d")
    assert run("retire", "L1")[0] == 0
    rec = imap.parse_records(run("get", "L1")[1])[0]  # row still fetchable
    assert rec.fields["status"] == "retired"
    assert "retired_at" in rec.fields


def test_retire_burns_the_key(run):
    run("allocate", "--label", "sym", "--summary", "s")
    run("retire", "sym")
    # key stays burned: cannot be re-allocated
    assert run("allocate", "--label", "sym", "--summary", "new")[0] == 3


def test_retire_excludes_from_discovery_but_not_get(run):
    run("allocate", "--summary", "findable keyword", "--detail", "d")
    run("retire", "L1")
    assert imap.parse_search(run("search", "findable")[1]) == []
    assert imap.parse_recent(run("recent")[1]) == []
    assert imap.parse_index(run("index")[1]) == {}
    assert run("get", "L1")[0] == 0  # exact fetch still reaches the tombstone


def test_no_hard_delete_path_exists(run):
    run("allocate", "--summary", "s")
    for verb in ("delete", "remove", "rm", "drop", "destroy", "purge"):
        assert run(verb, "L1")[0] != 0


def test_retire_missing_label_nonzero(run):
    assert run("retire", "NOPE")[0] == 4


# --------------------------------------------------------------------------- #
# wire grammar + exit codes                                                   #
# --------------------------------------------------------------------------- #

def test_all_output_conforms_to_grammar(run):
    run("allocate", "--summary", "alpha beta", "--detail", "multi\nline\n@x\n:y\n.z")
    run("allocate", "--label", "sym", "--summary", "gamma")
    for args in (
        ("get", "L1", "sym"),
        ("search", "alpha", "gamma"),
        ("index",),
        ("index", "--with-labels"),
        ("recent",),
    ):
        code, out, _ = run(*args)
        assert code == 0
        imap.assert_grammar(out)


def test_exit_codes_distinguish_success_and_failure(run):
    assert run("allocate", "--summary", "s")[0] == 0          # ok
    assert run("get", "L1")[0] == 0                            # ok
    assert run("get", "MISSING")[0] == 4                       # failure
    assert run("bogusverb")[0] != 0                            # failure
    assert run("allocate")[0] != 0                             # usage failure


# --------------------------------------------------------------------------- #
# multi-line value round-trip (property-style, the §5.1 writer invariant)     #
# --------------------------------------------------------------------------- #

TRICKY_VALUES = [
    "simple",
    "",
    "line1\nline2\nline3",
    "tab\there\tand\tthere",
    "@leading at-sign",
    ":leading colon",
    ".leading dot",
    "\n",
    "\n\n",
    "trailing newline\n",
    "ends with two\n\n",
    "mixed\n@at\n:colon\n.dot\twith\ttabs\nend",
    ".\n.\n.",
    "@\n:\n.",
    "\nstarts with a newline",
    "café — ☃ — 日本語 — emoji 🎯",
]


@pytest.mark.parametrize("detail", TRICKY_VALUES)
def test_detail_roundtrips_byte_identical(run, detail):
    code, out, _ = run("allocate", "--summary", "s", "--detail", detail)
    assert code == 0
    label = _label(out)
    code, out, _ = run("get", label)
    assert code == 0
    imap.assert_grammar(out)
    assert imap.parse_records(out)[0].fields["detail"] == detail


@pytest.mark.parametrize("summary", ["one line", "tab\tin\tit", "@:.weird", "ends with dot."])
def test_single_line_summary_roundtrips_via_header(run, summary):
    code, out, _ = run("allocate", "--summary", summary, "--detail", "d")
    assert code == 0
    label = _label(out)
    rec = imap.parse_records(run("get", label)[1])[0]
    assert rec.summary == summary          # summary's single home is the header
    assert "summary" not in rec.fields     # never emitted twice


@pytest.mark.parametrize("summary", ["multi\nline", "trailing\n", "\nleading"])
def test_multiline_summary_rejected(run, summary):
    # summary is the one-line glance tier; a newline is rejected, not truncated
    assert run("allocate", "--summary", summary)[0] == 2
    assert run("allocate", "--summary", "ok")[0] == 0  # the rejection wrote nothing
    assert run("annotate", "L1", "--summary", summary)[0] == 2


# --------------------------------------------------------------------------- #
# concurrency + never-reuse (WAL + insert transaction)                        #
# --------------------------------------------------------------------------- #

def test_wal_mode_enabled(run, db):
    run("allocate", "--summary", "x")
    conn = sqlite3.connect(str(db))
    try:
        mode = conn.execute("PRAGMA journal_mode").fetchone()[0]
    finally:
        conn.close()
    assert mode.lower() == "wal"


def test_concurrent_read_while_writing(db):
    errors = []

    def writer():
        for i in range(40):
            code, _, err = imap.run("allocate", "--summary", f"w{i}", db=db)
            if code != 0:
                errors.append(("write", err))

    def reader():
        for _ in range(40):
            code, _, err = imap.run("recent", "--limit", "5", db=db)
            if code != 0:
                errors.append(("read", err))

    tw, tr = threading.Thread(target=writer), threading.Thread(target=reader)
    tw.start(); tr.start(); tw.join(); tr.join()
    assert not errors, errors


def test_concurrent_allocate_distinct_and_contiguous(db):
    N = 25

    def alloc(i):
        return imap.run("allocate", "--summary", f"c{i}", db=db)

    with concurrent.futures.ThreadPoolExecutor(max_workers=N) as ex:
        results = list(ex.map(alloc, range(N)))

    labels = []
    for code, out, err in results:
        assert code == 0, err
        labels.append(_label(out))

    assert len(set(labels)) == N  # all distinct: WAL + insert tx serialize minting
    ordinals = sorted(int(l[1:]) for l in labels)
    assert ordinals == list(range(1, N + 1))  # contiguous, no gaps/reuse


def test_never_reuse_across_deletes(run, db):
    for i in range(3):
        assert run("allocate", "--summary", f"x{i}")[0] == 0
    conn = sqlite3.connect(str(db))
    try:
        max_ord = conn.execute("SELECT MAX(ordinal) FROM bindings").fetchone()[0]
        assert max_ord == 3
        conn.execute("DELETE FROM bindings WHERE ordinal = ?", (max_ord,))
        conn.commit()
    finally:
        conn.close()
    # AUTOINCREMENT high-water mark: the next ordinal exceeds the deleted one
    code, out, _ = run("allocate", "--summary", "after-delete")
    assert code == 0
    new_ord = int(_label(out)[1:])
    assert new_ord == max_ord + 1
    assert new_ord > max_ord


# --------------------------------------------------------------------------- #
# discoverability + deployment                                                #
# --------------------------------------------------------------------------- #

def test_help_documents_verbs_codes_and_grammar(run):
    code, out, _ = run("--help")
    assert code == 0
    for verb in ("allocate", "search", "get", "index", "recent", "annotate", "retire", "view"):
        assert verb in out
    for ec in ("0", "1", "2", "3", "4", "5"):
        assert ec in out
    # the three §5.1 regexes appear verbatim
    assert r"^@([^\t]+)\t(.*)$" in out
    assert r"^:([^\t]+)\t(.*)$" in out
    assert r"^\.(.*)$" in out
    # charset + continuation invariant
    assert "tab-free" in out
    assert "Continuation invariant" in out


def test_per_verb_help(run):
    for verb in ("allocate", "search", "get", "index", "recent", "annotate", "retire", "view"):
        code, out, _ = run(verb, "--help")
        assert code == 0
        assert verb in out


def test_db_path_via_flag(tmp_path):
    db = tmp_path / "viaflag.db"
    code, out, _ = imap.run("--db", str(db), "allocate", "--summary", "flag", db=None)
    assert code == 0
    assert db.exists()
    # round-trips through the same explicit path
    code, out, _ = imap.run("--db", str(db), "get", "L1", db=None)
    assert code == 0
    assert imap.parse_records(out)[0].summary == "flag"


# --------------------------------------------------------------------------- #
# view — source overlay (presentation, filename:symbol convention)            #
# --------------------------------------------------------------------------- #

def _block_above(lines, label_line):
    """The contiguous run of ';' comment lines immediately above label_line."""
    idx = lines.index(label_line)
    j = idx - 1
    blk = []
    while j >= 0 and lines[j].lstrip().startswith(";"):
        blk.append(lines[j])
        j -= 1
    return list(reversed(blk))


def test_view_injects_intent_before_label(run, tmp_path):
    src = tmp_path / "demo.s"
    src.write_text("section .text\n_start:\n    mov rax, 1\narg_table:\n    dq 0\n")
    f = str(src)
    run("allocate", "--label", f"{f}:_start", "--summary", "program entry point", "--detail", "sets up the stack")
    run("allocate", "--label", f"{f}:arg_table", "--summary", "flag handler table", "--detail", "maps flag to offset")
    code, out, _ = run("view", f, "--stdout")
    assert code == 0
    lines = out.split("\n")
    # comment block sits immediately above each label, summary+detail present
    blk1 = _block_above(lines, "_start:")
    assert blk1 and "program entry point" in "\n".join(blk1)
    assert "sets up the stack" in "\n".join(blk1)
    blk2 = _block_above(lines, "arg_table:")
    assert "flag handler table" in "\n".join(blk2)
    # original source content is preserved in order
    assert "    mov rax, 1" in lines and "    dq 0" in lines


def test_view_matches_label_indentation(run, tmp_path):
    src = tmp_path / "f.asm"
    src.write_text("    indented_label:\n        nop\n")
    f = str(src)
    run("allocate", "--label", f"{f}:indented_label", "--summary", "s", "--detail", "d")
    out = run("view", f, "--stdout")[1]
    blk = _block_above(out.split("\n"), "    indented_label:")
    assert blk
    assert all(line.startswith("    ;") for line in blk)  # indent preserved


def test_view_wraps_at_80_columns(run, tmp_path):
    src = tmp_path / "f.asm"
    src.write_text("sym:\n")
    f = str(src)
    long_detail = " ".join(f"word{i}" for i in range(80))
    run("allocate", "--label", f"{f}:sym", "--summary", "short", "--detail", long_detail)
    out = run("view", f, "--stdout")[1]
    comment_lines = [l for l in out.split("\n") if l.lstrip().startswith(";")]
    assert comment_lines
    assert all(len(l) <= 80 for l in comment_lines)  # nothing exceeds 80 cols
    assert len(comment_lines) > 3                     # actually wrapped


def test_view_only_exact_filename_match(run, tmp_path):
    src = tmp_path / "a.asm"
    src.write_text("foo:\n")
    f = str(src)
    run("allocate", "--label", "other.asm:foo", "--summary", "wrong file", "--detail", "x")
    code, out, err = run("view", f, "--stdout")
    assert code == 0
    assert "wrong file" not in out
    assert "no active entries" in err


def test_view_skips_and_reports_unplaced_symbols(run, tmp_path):
    src = tmp_path / "a.asm"
    src.write_text("present:\n")
    f = str(src)
    run("allocate", "--label", f"{f}:present", "--summary", "here", "--detail", "d")
    run("allocate", "--label", f"{f}:absent", "--summary", "nowhere", "--detail", "d")
    code, out, err = run("view", f, "--stdout")
    assert code == 0
    assert "here" in out and "nowhere" not in out
    assert "1/2" in err


def test_view_file_level_note_at_top(run, tmp_path):
    src = tmp_path / "notes.txt"
    src.write_text("hello world\n")
    f = str(src)
    run("allocate", "--label", f, "--summary", "file overview", "--detail", "top-level note")
    out = run("view", f, "--stdout")[1]
    lines = out.split("\n")
    assert lines[0].lstrip().startswith(";")
    assert "file overview" in out and "hello world" in out


def test_view_requires_exactly_one_file(run, tmp_path):
    src = tmp_path / "a.asm"
    src.write_text("x:\n")
    assert run("view")[0] == 1                       # zero files
    assert run("view", str(src), "extra")[0] == 1    # two files


def test_view_missing_file_is_nonzero(run):
    assert run("view", "/no/such/file.asm", "--stdout")[0] == 4
