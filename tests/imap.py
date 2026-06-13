"""Black-box harness for the compiled `intent-map` binary.

Drives the binary via argv and captures stdout/exit code, then parses the
output with a *reference reader* implementing the three §5.1 regexes verbatim.
Writing this parser doubles as proof that the wire grammar is parseable exactly
as specified — it is the reference reader for the format.
"""
import os
import re
import subprocess
from pathlib import Path

# Binary under test: $INTENT_MAP_BIN, else <repo-root>/intent-map.
BIN = os.environ.get("INTENT_MAP_BIN") or str(
    Path(__file__).resolve().parent.parent / "intent-map"
)

# ---- the three §5.1 regexes (a reader is a one-pass switch on line[0]) -----
RE_HEADER = re.compile(r"^@([^\t]+)\t(.*)$")  # record header -> (label, summary)
RE_FIELD = re.compile(r"^:([^\t]+)\t(.*)$")   # field         -> (key, value-1st)
RE_CONT = re.compile(r"^\.(.*)$")             # continuation  -> append "\n"+rest


def run(*args, db=None):
    """Run `intent-map <args>`; return (exit_code, stdout, stderr).

    stdout/stderr are decoded as UTF-8 with no newline translation, so byte
    fidelity of value prose is preserved. `db` (when given) is passed via the
    INTENT_MAP_DB env var; pass db=None to exercise the --db flag / default.
    """
    env = dict(os.environ)
    if db is not None:
        env["INTENT_MAP_DB"] = str(db)
    else:
        env.pop("INTENT_MAP_DB", None)
    p = subprocess.run([BIN, *map(str, args)], capture_output=True, env=env)
    return p.returncode, p.stdout.decode("utf-8"), p.stderr.decode("utf-8")


class Record:
    def __init__(self, label, summary):
        self.label = label      # opaque key
        self.summary = summary  # header summary (first physical line, glance)
        self.fields = {}        # field key -> reconstructed value

    def __repr__(self):
        return f"Record(label={self.label!r}, summary={self.summary!r}, fields={self.fields!r})"


def _lines(stdout):
    parts = stdout.split("\n")
    if parts and parts[-1] == "":  # drop the trailing-newline artifact
        parts.pop()
    return parts


def classify(line):
    if RE_HEADER.match(line):
        return "header"
    if RE_FIELD.match(line):
        return "field"
    if RE_CONT.match(line):
        return "cont"
    return None


def assert_grammar(stdout):
    """Every physical line is classified by exactly one of the three regexes."""
    for line in _lines(stdout):
        assert classify(line) is not None, f"unparseable line: {line!r}"


def parse_records(stdout):
    """Reference reader: parse a header+fields record stream (`get`, `allocate`).

    Continuations append "\\n" + rest to the current field — the exact inverse
    of the producer's continuation wrapping.
    """
    records = []
    cur = None  # (record, field_key)
    for line in _lines(stdout):
        m = RE_HEADER.match(line)
        if m:
            rec = Record(m.group(1), m.group(2))
            records.append(rec)
            cur = None
            continue
        m = RE_FIELD.match(line)
        if m:
            rec = records[-1]
            rec.fields[m.group(1)] = m.group(2)
            cur = (rec, m.group(1))
            continue
        m = RE_CONT.match(line)
        if m:
            rec, key = cur
            rec.fields[key] += "\n" + m.group(1)
            continue
        raise AssertionError(f"unparseable line: {line!r}")
    return records


def parse_search(stdout):
    """search output: header-only records -> list of (label, summary)."""
    return [(r.label, r.summary) for r in parse_records(stdout)]


def parse_recent(stdout):
    """recent output: @label TAB summary TAB modified_at per line."""
    out = []
    for line in _lines(stdout):
        m = RE_HEADER.match(line)
        assert m, f"bad recent line: {line!r}"
        summary, _, modified = m.group(2).rpartition("\t")
        out.append((m.group(1), summary, modified))
    return out


def parse_index(stdout):
    """index (default): :keyword TAB count -> dict[keyword] = count."""
    counts = {}
    for line in _lines(stdout):
        m = RE_FIELD.match(line)
        assert m, f"bad index line: {line!r}"
        counts[m.group(1)] = int(m.group(2))
    return counts


def parse_index_labels(stdout):
    """index --with-labels: dict[keyword] = (count, [(label, summary), ...])."""
    groups = {}
    cur = None
    for line in _lines(stdout):
        mf = RE_FIELD.match(line)
        if mf:
            cur = mf.group(1)
            groups[cur] = (int(mf.group(2)), [])
            continue
        mh = RE_HEADER.match(line)
        if mh:
            groups[cur][1].append((mh.group(1), mh.group(2)))
            continue
        raise AssertionError(f"bad index --with-labels line: {line!r}")
    return groups
