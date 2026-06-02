"""Regression tests for features.extract().

Run with: python3 -m pytest portfolio/tests/test_features.py -v
(or: python3 portfolio/tests/test_features.py for stdlib-only)
"""

import os
import sys
import tempfile
import unittest

# Make portfolio/ importable when running this file directly.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import features  # noqa: E402


def _write_cnf(text: str) -> str:
    tmp = tempfile.NamedTemporaryFile(mode="w", suffix=".cnf", delete=False)
    tmp.write(text)
    tmp.close()
    return tmp.name


class TestFeatures(unittest.TestCase):
    def test_basic_3var_2clause(self):
        path = _write_cnf("p cnf 3 2\n1 2 0\n-1 3 0\n")
        try:
            f = features.extract(path)
            self.assertEqual(f.n_vars, 3)
            self.assertEqual(f.n_clauses, 2)
            self.assertAlmostEqual(f.primal_density, 2/3)
            self.assertEqual(f.max_clause_len, 2)
            self.assertEqual(f.n_binary_clauses, 2)
            self.assertEqual(f.n_unit_clauses, 0)
            self.assertIsNone(f.indep_set_size)
            self.assertIsNone(f.metis_root_sep_size)
        finally:
            os.unlink(path)

    def test_clause_length_stats(self):
        path = _write_cnf("p cnf 5 4\n1 0\n2 3 0\n1 -2 -3 0\n4 5 1 -2 0\n")
        try:
            f = features.extract(path)
            self.assertEqual(f.n_clauses, 4)
            self.assertEqual(f.n_unit_clauses, 1)
            self.assertEqual(f.n_binary_clauses, 1)
            self.assertEqual(f.max_clause_len, 4)
            self.assertAlmostEqual(f.avg_clause_len, (1+2+3+4) / 4)
        finally:
            os.unlink(path)

    def test_indep_set_marker_parsed(self):
        path = _write_cnf("c p show 1 3 5 0\np cnf 5 1\n1 2 3 4 5 0\n")
        try:
            f = features.extract(path)
            self.assertEqual(f.indep_set_size, 3)
        finally:
            os.unlink(path)

    def test_indep_set_marker_across_multiple_lines(self):
        path = _write_cnf(
            "c p show 1 2 0\nc p show 3 4 0\np cnf 5 1\n1 2 3 4 5 0\n"
        )
        try:
            f = features.extract(path)
            # Union: {1, 2, 3, 4} → size 4
            self.assertEqual(f.indep_set_size, 4)
        finally:
            os.unlink(path)

    def test_empty_formula(self):
        path = _write_cnf("p cnf 0 0\n")
        try:
            f = features.extract(path)
            self.assertEqual(f.n_vars, 0)
            self.assertEqual(f.n_clauses, 0)
            self.assertEqual(f.primal_density, 0.0)
            self.assertEqual(f.max_clause_len, 0)
        finally:
            os.unlink(path)


if __name__ == "__main__":
    unittest.main()
