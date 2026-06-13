"""pytest fixtures: per-test isolation via a throwaway db (tmp_path)."""
import sys
from pathlib import Path

import pytest

# Make the harness importable as `import imap` regardless of pytest rootdir.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import imap  # noqa: E402


@pytest.fixture(scope="session", autouse=True)
def _require_binary():
    if not Path(imap.BIN).exists():
        pytest.fail(
            f"intent-map binary not found at {imap.BIN}; run `make build` first "
            "(or `make test`, which builds then tests)."
        )


@pytest.fixture
def db(tmp_path):
    """A unique, throwaway db path per test — no shared state, no ordering."""
    return tmp_path / "intent-map.db"


@pytest.fixture
def run(db):
    """run(*args) -> (exit_code, stdout, stderr), bound to this test's db."""
    def _run(*args):
        return imap.run(*args, db=db)
    return _run
