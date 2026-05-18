#!/usr/bin/env python3
"""
probe_flags.py — short-budget flag-variant comparison probe.

Runs the solver for a fixed wall-clock budget under each named flag
variant on the same CNF, parses the existing solver output (DIAG_STATS,
L2_HIT_HIST, FULL_CACHE_STATS, OPEN_WORK, Reactive-METIS summary), and
prints a side-by-side comparison table.

Sister to scripts/probe_anchor.py; same general structure (subprocess
per probe, parse stderr/stdout, aggregate). Used as a cheap parameter-
selection probe per docs/portfolio_plan.md §6.

Variants are passed as `name:flag-string` pairs. Each is run with -t T
appended; the same CNF path is appended last.

Output rank (default): (finished_desc, progress_bits_desc, l2_hit_rate_desc).
The intent is "did it finish > how much of the search space did it close
> how much cache amplification per decision". progress_bits is the
monotone odometer n_root - log2(remaining-unfinished-subtree-mass)
computed by Solver::captureOpenWorkSnapshot. Override via --rank.
"""

import argparse
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field
from typing import List, Optional


REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SOLVER = os.path.join(REPO, "build", "sharpSAT")
DEFAULT_CNF    = os.path.normpath(os.path.join(
    REPO, "..", "temp_cnf", "mc2025_track1_041.cnf"))

# Default variant set, aligned with portfolio_insights.md / benchmark_log.md
# terminology. "plain" matches the docs' definition (no -reactiveMetis,
# no -adaptive, default -wlIter 1). The 6 variants span:
#   - separator reorder (plain vs clausesFirst)
#   - reactive METIS regime (off vs default vs aggressive)
#   - BCP-driven picker (off vs adaptive with auto-α)
#   - WL-labeling sharpness (default 1 vs explicit 2)
DEFAULT_VARIANTS = [
    "plain:-rec -sep 5 -cb 3 -sepMode metis",
    "clausesFirst:-rec -sep 5 -cb 3 -sepMode metis -sepClausesFirst",
    "react-default:-rec -sep 5 -cb 3 -sepMode metis -reactiveMetis",
    "react-agg:-rec -sep 5 -cb 3 -sepMode metis -reactiveMetis -reactiveMetisMin 10 -reactiveMetisSkip 4",
    "adaptive:-rec -sep 5 -cb 3 -sepMode metis -adaptive",
    "plain-wl2:-rec -sep 5 -cb 3 -sepMode metis -wlIter 2",
]


@dataclass
class VariantResult:
    name: str
    flags: str
    wall_s: Optional[float] = None
    finished: bool = False         # solved within budget (no TIMEOUT marker)
    timeout: bool = False
    decisions: int = 0
    bcp_per_dec: float = 0.0
    avg_comp_at_entry: float = 0.0
    conflicts: int = 0
    l2_stores: int = 0
    l2_hits: int = 0
    l1_stores: int = 0
    l1_hits: int = 0
    hist_200_400: int = 0
    hist_400_plus: int = 0
    # OPEN_WORK fields:
    n_root: int = 0
    n_open_comps: int = 0
    open_bound_log2: float = 0.0
    progress_bits: float = 0.0
    chain_max_size: int = 0
    chain_min_size: int = 0
    chain_depth: int = 0
    # Reactive METIS:
    react_calls: int = 0
    react_accepted: int = 0
    react_overhead_pct: float = 0.0
    # Diagnostics:
    raw_stderr_tail: str = ""

    @property
    def l2_hit_rate(self) -> float:
        return self.l2_hits / max(1, self.decisions)

    @property
    def large_hit_rate(self) -> float:
        return (self.hist_200_400 + self.hist_400_plus) / max(1, self.decisions)


_DIAG = re.compile(
    r"DIAG_STATS\s+num_decisions=(\d+)\s+avg_bcp/dec=([\d.eE+-]+)\s+"
    r"avg_comp_at_entry=([\d.eE+-]+)")
_HIST = re.compile(
    r"L2_HIT_HIST\s+\[0-25\)=\d+\s+\[25-50\)=\d+\s+\[50-100\)=\d+\s+"
    r"\[100-200\)=\d+\s+\[200-400\)=(\d+)\s+\[400\+\)=(\d+)")
_FULL = re.compile(
    r"FULL_CACHE_STATS\s+l2_stores=(\d+)\s+l2_hits=(\d+)\s+"
    r"l1_stores=(\d+)\s+l1_hits=(\d+)")
_OPEN = re.compile(
    r"OPEN_WORK\s+n_root=(\d+)\s+n_open_comps=(\d+)\s+"
    r"bound_log2=([\d.eE+-]+)\s+progress_bits=([\d.eE+-]+)\s+sizes=([0-9,]*)")
_TIME = re.compile(r"^time:\s*([\d.]+)\s*s", re.MULTILINE)
_TIMEOUT = re.compile(r"TIMEOUT\s*!")
_CONF = re.compile(r"^decisions\s+\d+\s*$.*?conflicts.*?(\d+)", re.MULTILINE | re.DOTALL)
_LEARN = re.compile(r"learned_clauses=(\d+)")
_REACT_CALLS = re.compile(r"calls\s*:\s*(\d+)")
_REACT_ACC = re.compile(r"accepted\s*&\s*used\s*:\s*(\d+)")
_REACT_OVH = re.compile(r"overhead vs solve time\s*:\s*([\d.eE+-]+)\s*%")


def parse_output(out: str, vr: VariantResult) -> None:
    m = _DIAG.search(out)
    if m:
        vr.decisions = int(m.group(1))
        try: vr.bcp_per_dec = float(m.group(2))
        except ValueError: pass
        try: vr.avg_comp_at_entry = float(m.group(3))
        except ValueError: pass
    m = _HIST.search(out)
    if m:
        vr.hist_200_400 = int(m.group(1))
        vr.hist_400_plus = int(m.group(2))
    m = _FULL.search(out)
    if m:
        vr.l2_stores = int(m.group(1))
        vr.l2_hits = int(m.group(2))
        vr.l1_stores = int(m.group(3))
        vr.l1_hits = int(m.group(4))
    m = _OPEN.search(out)
    if m:
        vr.n_root = int(m.group(1))
        vr.n_open_comps = int(m.group(2))
        vr.open_bound_log2 = float(m.group(3))
        vr.progress_bits = float(m.group(4))
        sizes_str = m.group(5)
        if sizes_str:
            sizes = [int(s) for s in sizes_str.split(",") if s]
            vr.chain_depth = len(sizes)
            vr.chain_max_size = max(sizes)
            vr.chain_min_size = min(sizes)
    m = _TIMEOUT.search(out)
    vr.timeout = bool(m)
    m = _TIME.search(out)
    if m:
        vr.wall_s = float(m.group(1))
    vr.finished = (vr.wall_s is not None) and (not vr.timeout)
    # learned_clauses ≈ conflicts on this codebase:
    m = _LEARN.search(out)
    if m:
        vr.conflicts = int(m.group(1))
    # Reactive METIS summary (only present if -reactiveMetis):
    m = _REACT_CALLS.search(out)
    if m: vr.react_calls = int(m.group(1))
    m = _REACT_ACC.search(out)
    if m: vr.react_accepted = int(m.group(1))
    m = _REACT_OVH.search(out)
    if m: vr.react_overhead_pct = float(m.group(1))


def run_variant(solver: str, cnf: str, budget_s: float, name: str,
                flags: str) -> VariantResult:
    cmd = [solver] + flags.split() + ["-t", str(budget_s), cnf]
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          timeout=max(10.0, budget_s + 5.0))
    out = (proc.stdout or "") + "\n" + (proc.stderr or "")
    vr = VariantResult(name=name, flags=flags)
    parse_output(out, vr)
    err_tail = (proc.stderr or "").splitlines()[-3:]
    vr.raw_stderr_tail = " | ".join(err_tail)
    return vr


def make_rank_key(rank_spec: str):
    """Build a sort key function from a comma-separated spec.
    Each token is `field[:asc|desc]`. Default is desc."""
    fields = []
    for tok in rank_spec.split(","):
        tok = tok.strip()
        if not tok: continue
        parts = tok.split(":")
        name = parts[0]
        order = parts[1] if len(parts) > 1 else "desc"
        fields.append((name, order))

    def key(vr: VariantResult):
        out = []
        for name, order in fields:
            v = getattr(vr, name, None)
            if v is None:
                # Try property
                v = getattr(type(vr), name, None)
                v = v.fget(vr) if v is not None else 0
            if order == "desc":
                v = -v if isinstance(v, (int, float)) else v
            out.append(v)
        return tuple(out)
    return key


def print_table(results: List[VariantResult]) -> None:
    if results:
        print(f"\n  n_root (active vars at root, post-preprocess): {results[0].n_root}")
    print(f"\n{'name':<14} {'wall':>7} {'fin':>3} {'progress':>9} "
          f"{'dec':>9} {'l2_hit/dec':>10} {'big_hit/dec':>11} "
          f"{'bcp/dec':>8} {'conf':>5} "
          f"{'chain_d':>7} {'chain_max':>9} {'chain_min':>9} "
          f"{'react':>5}")
    print("-" * 132)
    for vr in results:
        wall = f"{vr.wall_s:6.2f}" if vr.wall_s is not None else "   ?  "
        fin  = "Y" if vr.finished else ("T" if vr.timeout else "?")
        react = f"{vr.react_calls}" if vr.react_calls else "-"
        # progress_bits comes directly from the solver's OPEN_WORK line
        # (= n_root - bound_log2). For finishers, equals n_root.
        prog = f"{vr.progress_bits:7.1f}"
        print(f"{vr.name:<14} {wall:>7} {fin:>3} {prog:>9} {vr.decisions:>9} "
              f"{vr.l2_hit_rate:>10.4f} {vr.large_hit_rate:>11.4f} "
              f"{vr.bcp_per_dec:>8.3f} {vr.conflicts:>5} "
              f"{vr.chain_depth:>7} {vr.chain_max_size:>9} {vr.chain_min_size:>9} "
              f"{react:>5}")
    print()
    print("Legend:")
    print("  fin: Y=solved within budget, T=TIMEOUT marker, ?=unknown")
    print("  progress    = n_root - log2(Σ 2^|unfinished subtree|); monotone odometer")
    print("                (= n_root if finished; higher = more search space closed)")
    print("  l2_hit/dec  = L2 cache hit rate per decision")
    print("  big_hit/dec = hits on components ≥200 vars / decisions")
    print("  chain_*     = # unfinished subtrees + max/min size of any in the list")
    print("  react       = reactive-METIS calls (only when -reactiveMetis on)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cnf", default=DEFAULT_CNF,
                    help=f"CNF path (default: {DEFAULT_CNF})")
    ap.add_argument("--solver", default=DEFAULT_SOLVER,
                    help=f"sharpSAT binary (default: {DEFAULT_SOLVER})")
    ap.add_argument("--budget", type=float, default=25.0,
                    help="Per-variant wall budget in seconds (default 25)")
    ap.add_argument("--variants", nargs="+", default=None,
                    help="Variant specs as 'name:flags'. "
                         "Default: 6 mechanism toggles.")
    ap.add_argument("--rank", default="finished:desc,progress_bits:desc,l2_hit_rate:desc",
                    help="Sort spec: comma-separated field[:asc|desc]. "
                         "Default ranks by the monotone progress odometer "
                         "(n_root - log2 of remaining unfinished subtree mass).")
    args = ap.parse_args()

    if not os.path.exists(args.solver):
        sys.exit(f"solver not found: {args.solver}")
    if not os.path.exists(args.cnf):
        sys.exit(f"CNF not found: {args.cnf}")

    variants = args.variants if args.variants else DEFAULT_VARIANTS
    parsed = []
    for spec in variants:
        if ":" not in spec:
            sys.exit(f"variant must be 'name:flags', got: {spec}")
        name, flags = spec.split(":", 1)
        parsed.append((name.strip(), flags.strip()))

    print(f"probe_flags: cnf={args.cnf}")
    print(f"             solver={args.solver}")
    print(f"             budget={args.budget}s/variant, n_variants={len(parsed)}")
    print(f"             expected wall: ~{len(parsed) * args.budget:.0f}s")

    results = []
    t0 = time.time()
    for i, (name, flags) in enumerate(parsed, 1):
        vr = run_variant(args.solver, args.cnf, args.budget, name, flags)
        results.append(vr)
        fin_marker = "SOLVE" if vr.finished else ("TIMEOUT" if vr.timeout else "?")
        wall = f"{vr.wall_s:.2f}s" if vr.wall_s is not None else "?"
        elapsed = time.time() - t0
        # Per-variant detail block emitted as soon as the variant completes,
        # so the user sees real-time progress on long runs.
        print(f"\n[{i}/{len(parsed)}] {name} — {fin_marker} {wall} "
              f"(total elapsed {elapsed:.0f}s)")
        print(f"   decisions={vr.decisions:,}  bcp/dec={vr.bcp_per_dec:.3f}  "
              f"conflicts={vr.conflicts}")
        print(f"   l2_hit_rate={vr.l2_hit_rate:.4f}  "
              f"big_hit_rate={vr.large_hit_rate:.4f}  "
              f"progress_bits={vr.progress_bits:.1f}/{vr.n_root}")
        print(f"   chain_depth={vr.chain_depth}  chain_max={vr.chain_max_size}  "
              f"chain_min={vr.chain_min_size}  "
              f"react_calls={vr.react_calls if vr.react_calls else '-'}")
        sys.stdout.flush()

    rank_key = make_rank_key(args.rank)
    results.sort(key=rank_key)

    print_table(results)
    print(f"Total wall: {time.time()-t0:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
