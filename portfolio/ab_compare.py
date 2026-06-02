#!/usr/bin/env python3
"""A/B comparison harness for portfolio solver versions.

Runs two or more *versions* (see versions.py) over a set of CNFs under a
fixed per-run time budget and reports, per instance and in aggregate:

  - the model count           (CORRECTNESS GATE: all completing versions on
                               an instance MUST agree; a disagreement is the
                               loudest possible failure)
  - wall time                 (the speed axis)
  - peak resident set size    (the memory axis)
  - per-instance winner       (fastest version that returned the agreed count)

This is the *infrastructure* only. It is deliberately parameterized by the
time budget and the instance set so a later evaluation methodology — which
must keep runs short — can drive it (small budgets, curated instance sets,
proxy metrics) without touching this file.

Usage:
  python3 portfolio/ab_compare.py -t 30 path/to/cnfs/ a.cnf b.cnf
  python3 portfolio/ab_compare.py -t 10 --versions cocoa-identity cocoa-canonical *.cnf

Binaries resolve from PORTFOLIO_SHARPSAT_BIN / PORTFOLIO_GANAK_BIN; if unset
they default to the in-repo Release builds. Versions whose binary is missing
are skipped with a warning.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import solvers          # noqa: E402
import versions as versions_mod  # noqa: E402

_REPO_ROOT = Path(__file__).resolve().parent.parent
_DEFAULT_BINS = {
    "PORTFOLIO_SHARPSAT_BIN": _REPO_ROOT / "sharpsat-separator" / "build" / "sharpSAT",
    "PORTFOLIO_GANAK_BIN":    _REPO_ROOT / "ganak-canonical" / "build" / "ganak",
}
# Which env var each solver's binary comes from (mirror of the wrappers).
_SOLVER_BIN_ENV = {
    "sharpsat-separator": "PORTFOLIO_SHARPSAT_BIN",
    "ganak-canonical":    "PORTFOLIO_GANAK_BIN",
}


def _utc_now_iso() -> str:
    return (_dt.datetime.now(_dt.timezone.utc)
            .replace(microsecond=0).isoformat().replace("+00:00", "Z"))


def _ensure_default_bins() -> None:
    """Populate PORTFOLIO_*_BIN from the in-repo builds if unset."""
    for env, path in _DEFAULT_BINS.items():
        if not os.environ.get(env):
            os.environ[env] = str(path)


def _binary_ok(solver_name: str) -> bool:
    env = _SOLVER_BIN_ENV[solver_name]
    p = os.environ.get(env)
    return bool(p) and os.path.exists(p) and os.access(p, os.X_OK)


def _human_bytes(n: int | None) -> str:
    if n is None:
        return "n/a"
    mb = n / (1024 * 1024)
    return f"{mb:.0f}MB" if mb >= 1 else f"{n/1024:.0f}KB"


def _collect_cnfs(paths: list[str]) -> list[str]:
    out: list[str] = []
    for p in paths:
        pp = Path(p)
        if pp.is_dir():
            out.extend(sorted(str(f) for f in pp.rglob("*.cnf")))
        elif pp.exists():
            out.append(str(pp))
        else:
            print(f"WARNING: input not found, skipping: {p}", file=sys.stderr)
    return out


def run_one(version: str, cnf: str, time_budget_s: int | None) -> dict:
    """Run a single (version, cnf) and return a plain-dict record."""
    spec = versions_mod.resolve(version)
    rec = {
        "version": version,
        "solver": spec["solver"],
        "profile": spec["profile"],
        "hash": spec["hash"],
        "cnf": cnf,
    }
    try:
        res = solvers.run(spec["solver"], spec["profile"], cnf,
                          time_budget_s=time_budget_s)
    except solvers.SolverError as e:
        rec.update(status="error", error=str(e), count=None,
                   wall_s=None, peak_rss_bytes=None)
        return rec

    if res.timed_out:
        status = "timeout"
    elif res.ok:
        status = "ok"
    else:
        status = "fail"
    rec.update(
        status=status,
        count=res.count,
        wall_s=round(res.wall_time_s, 4),
        peak_rss_bytes=res.peak_rss_bytes,
        exit_code=res.exit_code,
    )
    if status in ("fail", "timeout"):
        # Keep a short stderr tail for triage; full output is not logged.
        rec["stderr_tail"] = (res.stderr or "")[-400:]
    return rec


def compare(version_names: list[str], cnfs: list[str],
            time_budget_s: int | None) -> dict:
    """Run every (version, cnf) and assemble per-instance + aggregate results."""
    # Filter to versions whose binary is present.
    usable = []
    for v in version_names:
        spec = versions_mod.resolve(v)
        if _binary_ok(spec["solver"]):
            usable.append(v)
        else:
            env = _SOLVER_BIN_ENV[spec["solver"]]
            print(f"WARNING: skipping version {v!r} — binary missing "
                  f"(${env}={os.environ.get(env)!r})", file=sys.stderr)
    if not usable:
        raise SystemExit("ERROR: no runnable versions (check binaries).")

    per_instance: list[dict] = []
    runs: list[dict] = []
    for cnf in cnfs:
        results = {v: run_one(v, cnf, time_budget_s) for v in usable}
        runs.extend(results.values())

        # --- correctness gate: completing versions must agree on the count ---
        counts = {v: r["count"] for v, r in results.items() if r["status"] == "ok"}
        distinct = set(counts.values())
        agreed = next(iter(distinct)) if len(distinct) == 1 else None
        mismatch = len(distinct) > 1

        # --- winner: fastest version returning the agreed count ---
        winner = None
        if agreed is not None:
            ok_correct = {v: results[v]["wall_s"] for v in counts
                          if counts[v] == agreed and results[v]["wall_s"] is not None}
            if ok_correct:
                winner = min(ok_correct, key=ok_correct.get)

        per_instance.append({
            "cnf": cnf,
            "agreed_count": agreed,
            "count_mismatch": mismatch,
            "counts": counts,
            "winner": winner,
            "per_version": {v: {k: results[v][k] for k in
                                ("status", "count", "wall_s", "peak_rss_bytes")}
                            for v in usable},
        })
        if mismatch:
            print(f"*** COUNT MISMATCH on {cnf}: {counts} ***", file=sys.stderr)

    return {
        "ts": _utc_now_iso(),
        "time_budget_s": time_budget_s,
        "versions": usable,
        "runs": runs,
        "per_instance": per_instance,
        "summary": _summarize(usable, per_instance),
    }


def _summarize(versions_used: list[str], per_instance: list[dict]) -> dict:
    n = len(per_instance)
    solved = {v: 0 for v in versions_used}
    wins = {v: 0 for v in versions_used}
    peak = {v: None for v in versions_used}
    # Instances where every version completed — the fair set for time stats.
    commonly_solved_walls = {v: [] for v in versions_used}
    mismatches = 0
    for inst in per_instance:
        if inst["count_mismatch"]:
            mismatches += 1
        if inst["winner"]:
            wins[inst["winner"]] += 1
        all_ok = all(inst["per_version"][v]["status"] == "ok" for v in versions_used)
        for v in versions_used:
            pv = inst["per_version"][v]
            if pv["status"] == "ok":
                solved[v] += 1
            if pv["peak_rss_bytes"] is not None:
                peak[v] = max(peak[v] or 0, pv["peak_rss_bytes"])
            if all_ok and pv["wall_s"] is not None:
                commonly_solved_walls[v].append(pv["wall_s"])

    def _median(xs):
        if not xs:
            return None
        s = sorted(xs)
        m = len(s) // 2
        return s[m] if len(s) % 2 else round((s[m - 1] + s[m]) / 2, 4)

    common_n = len(next(iter(commonly_solved_walls.values()))) if versions_used else 0
    return {
        "n_instances": n,
        "n_count_mismatches": mismatches,
        "n_commonly_solved": common_n,
        "per_version": {
            v: {
                "solved": solved[v],
                "wins": wins[v],
                "peak_rss_bytes": peak[v],
                "median_wall_s_on_common": _median(commonly_solved_walls[v]),
                "total_wall_s_on_common": round(sum(commonly_solved_walls[v]), 4),
            } for v in versions_used
        },
    }


def _print_summary(result: dict) -> None:
    s = result["summary"]
    vs = result["versions"]
    print()
    print(f"=== A/B summary  (budget={result['time_budget_s']}s, "
          f"{s['n_instances']} instances, {s['n_commonly_solved']} solved by all) ===")
    hdr = f"{'version':22s} {'solved':>8s} {'wins':>6s} {'med_wall':>10s} {'peak_rss':>10s}"
    print(hdr)
    print("-" * len(hdr))
    for v in vs:
        pv = s["per_version"][v]
        med = pv["median_wall_s_on_common"]
        med_s = f"{med:.3f}s" if med is not None else "n/a"
        print(f"{v:22s} {pv['solved']:>4d}/{s['n_instances']:<3d} "
              f"{pv['wins']:>6d} {med_s:>10s} "
              f"{_human_bytes(pv['peak_rss_bytes']):>10s}")
    if s["n_count_mismatches"]:
        print(f"\n!!! {s['n_count_mismatches']} COUNT MISMATCH(es) — "
              f"a soundness failure, NOT a perf result. See stderr / JSONL.")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description="A/B comparison harness for portfolio solver versions.")
    ap.add_argument("cnfs", nargs="+",
                    help="CNF files and/or directories (recursed for *.cnf).")
    ap.add_argument("-t", "--time-budget", type=int, default=30,
                    help="Per-run wall-clock budget in seconds (default 30).")
    ap.add_argument("--versions", nargs="+", default=list(versions_mod.VERSIONS),
                    metavar="V",
                    help=f"Versions to compare (default: all). "
                         f"Known: {sorted(versions_mod.VERSIONS)}")
    ap.add_argument("--out", default=None,
                    help="JSONL output path (default: portfolio/ab_results_<ts>.jsonl).")
    args = ap.parse_args(argv)

    for v in args.versions:
        versions_mod.resolve(v)  # validate names early

    _ensure_default_bins()
    cnfs = _collect_cnfs(args.cnfs)
    if not cnfs:
        print("ERROR: no CNFs to run.", file=sys.stderr)
        return 1

    result = compare(args.versions, cnfs, args.time_budget)

    out = args.out or str(Path(__file__).resolve().parent /
                          f"ab_results_{result['ts'].replace(':', '').replace('-', '')}.jsonl")
    with open(out, "w") as fh:
        # One line per run record, then a final summary line.
        for r in result["runs"]:
            fh.write(json.dumps(r, default=str) + "\n")
        fh.write(json.dumps({"_summary": result["summary"],
                             "_meta": {"ts": result["ts"],
                                       "time_budget_s": result["time_budget_s"],
                                       "versions": result["versions"]}},
                            default=str) + "\n")

    _print_summary(result)
    print(f"\nrecords -> {out}")
    # Exit non-zero on any count mismatch (soundness failure) for CI use.
    return 2 if result["summary"]["n_count_mismatches"] else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
