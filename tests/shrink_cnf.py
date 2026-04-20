#!/usr/bin/env python3
"""
Shrink a CNF by randomly assigning K of its N variables, then
simplifying: satisfied clauses are dropped; falsified literals are
removed from surviving clauses. The remaining variables are renumbered
to stay in [1..M]. The output preserves the original DIMACS structure
minus the frozen subset.

Use this to create smaller sub-instances of hard benchmarks (e.g. a
5-var assignment of t1_049 yields a 85-var restriction that may solve
in seconds while retaining the hardness character).

Usage:
  python3 shrink_cnf.py in.cnf out.cnf K [--seed S]

Prints to stderr: original (N, M), frozen count, written (N', M').
"""
import argparse, random, sys


def parse_cnf(path: str):
    n_vars = n_cls = 0
    clauses: list[list[int]] = []
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("c"):
            continue
        if line.startswith("p"):
            toks = line.split()
            n_vars = int(toks[2])
            n_cls = int(toks[3])
            continue
        lits = [int(x) for x in line.split() if x and x != "0"]
        if lits:
            clauses.append(lits)
    return n_vars, n_cls, clauses


def shrink(n_vars: int, clauses: list[list[int]], k: int, seed: int):
    rng = random.Random(seed)
    # Pick K vars uniformly at random.
    frozen_vars = rng.sample(range(1, n_vars + 1), k)
    values: dict[int, bool] = {v: bool(rng.getrandbits(1)) for v in frozen_vars}

    new_clauses: list[list[int]] = []
    trivially_unsat = False
    for cl in clauses:
        # Resolve literals against the frozen assignment.
        kept: list[int] = []
        sat = False
        for l in cl:
            v = abs(l)
            if v in values:
                polarity = l > 0
                if values[v] == polarity:
                    sat = True
                    break
                # else: literal is falsified — drop it
            else:
                kept.append(l)
        if sat:
            continue
        if not kept:
            # Empty clause after simplification — the assignment falsifies
            # the formula. The remaining instance is trivially UNSAT, which
            # is OK for benchmarking but we signal it.
            trivially_unsat = True
            new_clauses.append([])
            continue
        new_clauses.append(kept)

    # Renumber remaining vars to 1..M.
    active_vars = sorted({abs(l) for cl in new_clauses for l in cl})
    remap = {old: new for new, old in enumerate(active_vars, start=1)}
    renumbered: list[list[int]] = []
    for cl in new_clauses:
        renumbered.append([(remap[abs(l)] if l > 0 else -remap[abs(l)])
                            for l in cl])
    return len(active_vars), renumbered, frozen_vars, values, trivially_unsat


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("input_cnf")
    ap.add_argument("output_cnf")
    ap.add_argument("k", type=int, help="number of variables to freeze")
    ap.add_argument("--seed", type=int, default=17)
    args = ap.parse_args()

    n, m, cls = parse_cnf(args.input_cnf)
    new_n, new_cls, frozen, values, unsat = shrink(n, cls, args.k, args.seed)

    print(f"original: n={n} m={m}", file=sys.stderr)
    print(f"frozen: k={args.k} seed={args.seed}", file=sys.stderr)
    assignments = ", ".join(f"x{v}={1 if values[v] else 0}" for v in frozen)
    print(f"assignments: {assignments}", file=sys.stderr)
    print(f"reduced: n={new_n} m={len(new_cls)} "
          f"{'(contains empty clause — trivially UNSAT)' if unsat else ''}",
          file=sys.stderr)

    with open(args.output_cnf, "w") as fh:
        fh.write(f"c reduced from {args.input_cnf} by freezing {args.k} vars (seed={args.seed})\n")
        fh.write(f"c frozen: {assignments}\n")
        fh.write(f"p cnf {new_n} {len(new_cls)}\n")
        for cl in new_cls:
            fh.write(" ".join(str(l) for l in cl) + " 0\n")


if __name__ == "__main__":
    main()
