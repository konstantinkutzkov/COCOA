"""End-to-end route.py smoke + classifier sanity tests.

Run with: python3 -m pytest portfolio/tests/test_route.py -v
"""

import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import features  # noqa: E402
from classifier import decide, CLASSIFIER_VERSION  # noqa: E402
from classifier.thresholds import SEP_RATIO_MAX, BALANCE_MIN  # noqa: E402


_PORTFOLIO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_REPO_ROOT = os.path.dirname(_PORTFOLIO_DIR)
_DEFAULT_SHARPSAT = os.path.join(
    _REPO_ROOT, "sharpsat-separator", "build", "sharpSAT"
)


class TestThresholdClassifier(unittest.TestCase):
    """v2 thresholds — Phase-2 gate routes."""

    def test_no_metis_data_routes_ganak(self):
        f = features.Features(n_vars=10, n_clauses=20, primal_density=2.0)
        d = decide(f)
        self.assertEqual(d.solver, "ganak-canonical")
        self.assertEqual(d.reason, "metis_features_missing")

    def test_small_balanced_separator_routes_sharpsat(self):
        f = features.Features(
            n_vars=100, n_clauses=200, primal_density=2.0,
            metis_root_sep_size=10,
            metis_root_balance=0.45,
            metis_sep_ratio=SEP_RATIO_MAX - 0.01,
        )
        d = decide(f)
        self.assertEqual(d.solver, "sharpsat-separator")
        self.assertTrue(d.reason.startswith("phase2_gate_pass"))

    def test_too_wide_separator_routes_ganak(self):
        f = features.Features(
            n_vars=100, n_clauses=200,
            metis_root_sep_size=50,
            metis_root_balance=0.45,
            metis_sep_ratio=SEP_RATIO_MAX + 0.01,
        )
        d = decide(f)
        self.assertEqual(d.solver, "ganak-canonical")
        self.assertTrue(d.reason.startswith("phase2_gate_fail"))

    def test_too_imbalanced_routes_ganak(self):
        f = features.Features(
            n_vars=100, n_clauses=200,
            metis_root_sep_size=10,
            metis_root_balance=BALANCE_MIN - 0.01,
            metis_sep_ratio=0.10,
        )
        d = decide(f)
        self.assertEqual(d.solver, "ganak-canonical")
        self.assertTrue(d.reason.startswith("phase2_gate_fail"))

    def test_classifier_version_is_recognizable(self):
        self.assertTrue(CLASSIFIER_VERSION.startswith("thresholds-"))


class TestRouteSmoke(unittest.TestCase):
    """Invokes portfolio/run.sh end-to-end. Requires sharpSAT binary."""

    @unittest.skipUnless(
        os.path.exists(_DEFAULT_SHARPSAT) and os.access(_DEFAULT_SHARPSAT, os.X_OK),
        f"sharpSAT not found at {_DEFAULT_SHARPSAT}",
    )
    def test_route_returns_correct_count(self):
        cnf_path = None
        try:
            tmp = tempfile.NamedTemporaryFile(
                mode="w", suffix=".cnf", delete=False
            )
            tmp.write("p cnf 3 2\n1 2 0\n-1 3 0\n")
            tmp.close()
            cnf_path = tmp.name

            run_sh = os.path.join(_PORTFOLIO_DIR, "run.sh")
            proc = subprocess.run(
                [run_sh, cnf_path, "-t", "10"],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0,
                             f"run.sh failed: stderr={proc.stderr!r}")
            # First non-empty line of stdout should be the count.
            count_line = next(
                (line.strip() for line in proc.stdout.splitlines()
                 if line.strip()),
                None,
            )
            self.assertEqual(count_line, "4")
        finally:
            if cnf_path and os.path.exists(cnf_path):
                os.unlink(cnf_path)


if __name__ == "__main__":
    unittest.main()
