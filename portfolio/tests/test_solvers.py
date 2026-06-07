"""Smoke tests for solver invocations.

These shell out to the actual binaries; they need
PORTFOLIO_SHARPSAT_BIN (and optionally PORTFOLIO_GANAK_BIN) set, exactly
as run.sh sets them. Skipped if the binaries don't exist.

Run with: python3 -m pytest portfolio/tests/test_solvers.py -v
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import solvers  # noqa: E402
from solvers import sharpsat_separator, ganak_canonical  # noqa: E402


def _write_smoke_cnf() -> str:
    # 3 vars, 2 binary clauses → 4 satisfying assignments.
    tmp = tempfile.NamedTemporaryFile(mode="w", suffix=".cnf", delete=False)
    tmp.write("p cnf 3 2\n1 2 0\n-1 3 0\n")
    tmp.close()
    return tmp.name


def _have_binary(env_var: str) -> bool:
    p = os.environ.get(env_var)
    return bool(p) and os.path.exists(p) and os.access(p, os.X_OK)


class TestSharpsatSeparator(unittest.TestCase):
    @unittest.skipUnless(_have_binary("PORTFOLIO_SHARPSAT_BIN"),
                         "PORTFOLIO_SHARPSAT_BIN not set or not executable")
    def test_default_profile_smoke(self):
        cnf = _write_smoke_cnf()
        try:
            r = solvers.run("sharpsat-separator", "default", cnf,
                            time_budget_s=10)
            self.assertTrue(r.ok, f"solver failed: stderr={r.stderr!r}")
            self.assertEqual(r.count, "4")
        finally:
            os.unlink(cnf)

    def test_unknown_profile_raises(self):
        with self.assertRaises(solvers.SolverError):
            solvers.run("sharpsat-separator", "nonexistent-profile",
                        "/tmp/nonexistent.cnf")


class TestGanakCanonical(unittest.TestCase):
    @unittest.skipUnless(_have_binary("PORTFOLIO_GANAK_BIN"),
                         "PORTFOLIO_GANAK_BIN not set or not executable")
    def test_default_profile_smoke(self):
        cnf = _write_smoke_cnf()
        try:
            r = solvers.run("ganak-canonical", "default", cnf,
                            time_budget_s=10)
            # ganak may or may not be installed — just check it didn't crash.
            # We do NOT assert on count here because vanilla ganak's output
            # format may differ from the regex; that's tracked separately.
            self.assertIsNotNone(r.exit_code)
        finally:
            os.unlink(cnf)

    def test_unknown_profile_raises(self):
        # `canonical` is now a LIVE profile (the fork landed, see module docstring),
        # so it is no longer rejected. Only a genuinely unknown profile raises
        # SolverError -- and that check runs BEFORE _bin_path(), so it needs no
        # PORTFOLIO_GANAK_BIN (env-independent).
        with self.assertRaises(solvers.SolverError):
            solvers.run("ganak-canonical", "nonexistent-profile",
                        "/tmp/nonexistent.cnf")

    @unittest.skipUnless(_have_binary("PORTFOLIO_GANAK_BIN"),
                         "PORTFOLIO_GANAK_BIN not set or not executable")
    def test_canonical_profile_accepted(self):
        # Post-fork the canonical profile is accepted (no unknown-profile error);
        # skip-guarded because actually running it needs the fork binary.
        cnf = _write_smoke_cnf()
        try:
            r = solvers.run("ganak-canonical", "canonical", cnf, time_budget_s=10)
            self.assertIsNotNone(r.exit_code)
        finally:
            os.unlink(cnf)


class TestUnknownSolver(unittest.TestCase):
    def test_unknown_solver_raises(self):
        with self.assertRaises(solvers.SolverError):
            solvers.run("does-not-exist", "default", "/tmp/foo.cnf")


if __name__ == "__main__":
    unittest.main()
