#!/usr/bin/env python3
"""
Generate grid-structured CNFs to stress the precomputed separator
hierarchy. Variables are arranged in an n x n grid; clauses tie
adjacent cells. An optional noise parameter adds random 3-clauses
tying random pairs, which typically makes intermediate-level
separators imbalanced (exercising TIER1_REJECT / case-D).

Each cell (i, j) is variable id  i*n + j + 1  (1-indexed).

Clauses generated:
  "Adjacency" triples for each inner-cell (i, j):
      (x[i,j]  v  x[i+1,j]  v  x[i,j+1])
      (-x[i,j] v -x[i+1,j] v -x[i,j+1])
  Last row/col get only the 2-neighbor version.

  If noise > 0, add `noise * n` random 3-clauses over random var
  triples (with random polarities), seeded for reproducibility.

Usage:
  python3 gen_grid_cnf.py N [noise] [--seed S] > out.cnf
"""
import argparse, random, sys


def grid_cnf(n: int, noise: int = 0, seed: int = 42) -> str:
    rng = random.Random(seed)
    n_vars = n * n
    clauses: list[list[int]] = []

    def vid(i, j):
        return i * n + j + 1

    # Adjacency triples (and length-2 fallbacks at the edges).
    for i in range(n):
        for j in range(n):
            v = vid(i, j)
            lits_pos = [v]
            lits_neg = [-v]
            if i + 1 < n:
                lits_pos.append(vid(i + 1, j))
                lits_neg.append(-vid(i + 1, j))
            if j + 1 < n:
                lits_pos.append(vid(i, j + 1))
                lits_neg.append(-vid(i, j + 1))
            if len(lits_pos) >= 2:
                clauses.append(lits_pos)
                clauses.append(lits_neg)

    # Random noise clauses (3-clauses over random triples).
    for _ in range(noise * n):
        picks = rng.sample(range(1, n_vars + 1), 3)
        clause = [v if rng.random() > 0.5 else -v for v in picks]
        clauses.append(clause)

    lines = [f"c grid {n}x{n} (noise={noise}, seed={seed})",
             f"p cnf {n_vars} {len(clauses)}"]
    for cl in clauses:
        lines.append(" ".join(str(l) for l in cl) + " 0")
    return "\n".join(lines) + "\n"


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("n", type=int, help="grid side length (n x n variables)")
    p.add_argument("noise", type=int, nargs="?", default=0,
                   help="number of random 3-clauses = noise * n (default 0)")
    p.add_argument("--seed", type=int, default=42)
    args = p.parse_args()
    sys.stdout.write(grid_cnf(args.n, args.noise, args.seed))


if __name__ == "__main__":
    main()
