"""Tests for preprocess.metis_wrapper and the METIS-integrated features.extract.

Skipped if the metis_features helper binary isn't built.

Run with: python3 -m unittest discover portfolio/tests
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import features  # noqa: E402
from preprocess import metis_wrapper  # noqa: E402


_HERE = os.path.dirname(os.path.abspath(__file__))
_DEFAULT_HELPER = os.path.normpath(os.path.join(
    _HERE, "..", "..", "sharpsat-separator", "build", "metis_features"
))
_HELPER_AVAILABLE = (
    os.path.exists(_DEFAULT_HELPER) and os.access(_DEFAULT_HELPER, os.X_OK)
)


def _write_cnf(text: str) -> str:
    tmp = tempfile.NamedTemporaryFile(mode="w", suffix=".cnf", delete=False)
    tmp.write(text)
    tmp.close()
    return tmp.name


class TestMetisWrapperHelperMissing(unittest.TestCase):
    def test_missing_helper_raises(self):
        os.environ["PORTFOLIO_METIS_FEATURES_BIN"] = "/does/not/exist"
        try:
            with self.assertRaises(metis_wrapper.MetisHelperMissing):
                metis_wrapper.run("/tmp/whatever.cnf")
        finally:
            del os.environ["PORTFOLIO_METIS_FEATURES_BIN"]


@unittest.skipUnless(_HELPER_AVAILABLE,
                     f"metis_features helper not built at {_DEFAULT_HELPER}")
class TestMetisWrapperReal(unittest.TestCase):
    def test_too_small_returns_clean_status(self):
        cnf = _write_cnf("p cnf 3 2\n1 2 0\n-1 3 0\n")
        try:
            d = metis_wrapper.run(cnf)
            self.assertEqual(d["metis_status"], "too_small")
            self.assertEqual(d["metis_n_vars"], 3)
            self.assertEqual(d["metis_n_clauses"], 2)
        finally:
            os.unlink(cnf)

    def test_medium_cnf_produces_features(self):
        # Build a 10-var, 8-binary-clause graph: chain + a couple cross-edges.
        cnf = _write_cnf(
            "p cnf 10 10\n"
            "1 2 0\n2 3 0\n3 4 0\n4 5 0\n5 6 0\n"
            "6 7 0\n7 8 0\n8 9 0\n9 10 0\n1 10 0\n"
        )
        try:
            d = metis_wrapper.run(cnf)
            self.assertEqual(d["metis_status"], "ok")
            self.assertGreater(d["metis_sep_vars"], 0)
            self.assertGreaterEqual(d["metis_balance"], 0.0)
            self.assertLessEqual(d["metis_balance"], 0.5)
            self.assertGreaterEqual(d["metis_sep_ratio"], 0.0)
        finally:
            os.unlink(cnf)


@unittest.skipUnless(_HELPER_AVAILABLE,
                     f"metis_features helper not built at {_DEFAULT_HELPER}")
class TestFeaturesWithMetis(unittest.TestCase):
    def test_features_extract_populates_metis_when_eligible(self):
        cnf = _write_cnf(
            "p cnf 10 10\n"
            "1 2 0\n2 3 0\n3 4 0\n4 5 0\n5 6 0\n"
            "6 7 0\n7 8 0\n8 9 0\n9 10 0\n1 10 0\n"
        )
        try:
            f = features.extract(cnf, with_metis=True)
            self.assertIsNotNone(f.metis_root_sep_size)
            self.assertIsNotNone(f.metis_root_balance)
            self.assertIsNotNone(f.metis_sep_ratio)
        finally:
            os.unlink(cnf)

    def test_features_extract_skips_metis_when_too_small(self):
        cnf = _write_cnf("p cnf 3 2\n1 2 0\n-1 3 0\n")
        try:
            f = features.extract(cnf, with_metis=True)
            # too_small status → METIS fields stay None.
            self.assertIsNone(f.metis_root_sep_size)
            self.assertIsNone(f.metis_root_balance)
            self.assertIsNone(f.metis_sep_ratio)
        finally:
            os.unlink(cnf)

    def test_features_extract_with_metis_false(self):
        # Even on an eligible instance, with_metis=False yields no Tier-2 data.
        cnf = _write_cnf(
            "p cnf 10 10\n"
            "1 2 0\n2 3 0\n3 4 0\n4 5 0\n5 6 0\n"
            "6 7 0\n7 8 0\n8 9 0\n9 10 0\n1 10 0\n"
        )
        try:
            f = features.extract(cnf, with_metis=False)
            self.assertIsNone(f.metis_root_sep_size)
        finally:
            os.unlink(cnf)


if __name__ == "__main__":
    unittest.main()
