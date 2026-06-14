#!/usr/bin/env bash
# =====================================================================
# Model Counting Competition 2026 — Track 1 (exact, unweighted MC)
# Submission entry point for the COCOA #SAT portfolio.
#
#   Usage:  ./run.sh <instance.cnf>      (EXACTLY one parameter, per the rules)
#
# Per the MC2026 format document (mccomp_format_25.pdf), the answer is written
# to STDOUT as the solution lines below; the full solver trace goes to STDERR
# so it never pollutes the graded stdout:
#
#     s SATISFIABLE                  (s UNSATISFIABLE when the count is 0)
#     c s type mc
#     c s log10-estimate <log10(N)>  (-inf when the count is 0)        [MANDATORY]
#     c s exact arb int <N>          (the exact arbitrary-precision count)
#
# The solver reads the `c t mc` type line (handled below): a non-`mc` type is
# rejected with a format error, satisfying the "support the type line" rule.
# =====================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$#" -ne 1 ]; then
    echo "c o ERROR: run.sh expects exactly one argument: the benchmark instance" >&2
    echo "c o usage: run.sh <instance.cnf>" >&2
    echo "s UNKNOWN"
    exit 2
fi
INSTANCE="$1"

if [ ! -r "$INSTANCE" ]; then
    echo "c o ERROR: cannot read instance: $INSTANCE" >&2
    echo "s UNKNOWN"
    exit 2
fi

# ---- Type-line guard: this solver implements Track 1 (mc) only. -------------
# The format allows `c t mc|wmc|pmc|pwmc|amc-complex`. We must either handle the
# stated type or output an error (handling via CLI flags alone is insufficient).
TYPE_LINE="$(grep -m1 -E '^c[[:space:]]+t[[:space:]]' "$INSTANCE" 2>/dev/null || true)"
if [ -n "$TYPE_LINE" ]; then
    TY="$(printf '%s\n' "$TYPE_LINE" | awk '{print $3}')"
    if [ "$TY" != "mc" ]; then
        echo "c o ERROR: unsupported problem type '$TY' (this solver supports Track 1 'mc' only)" >&2
        echo "c o format error: only exact unweighted model counting (c t mc) is supported" >&2
        echo "s UNKNOWN"
        exit 0
    fi
fi
# (No `c t` line ⇒ a plain DIMACS CNF, which is exact unweighted #SAT ⇒ OK.)

# ---- Solver binaries (overridable; default to the in-repo build). -----------
export PORTFOLIO_SHARPSAT_BIN="${PORTFOLIO_SHARPSAT_BIN:-$SCRIPT_DIR/cocoa/build/sharpSAT}"
export PORTFOLIO_GANAK_BIN="${PORTFOLIO_GANAK_BIN:-$SCRIPT_DIR/ganak-canonical/build/ganak}"
export PORTFOLIO_METIS_FEATURES_BIN="${PORTFOLIO_METIS_FEATURES_BIN:-$SCRIPT_DIR/cocoa/build/metis_features}"
export PORTFOLIO_NDCOST_BIN="${PORTFOLIO_NDCOST_BIN:-$SCRIPT_DIR/cocoa/build/nd_cost}"

# ---- Run the validated portfolio pipeline. ----------------------------------
# Sound flags (-cachePurge 1 -provLocalTaint) are baked into every COCOA
# archetype (portfolio/race/archetypes.py), so the result is sound by
# construction; the Ganak handoff is the battle-tested fallback. All of the
# pipeline's own stdout (progress, the chosen-config trace, ganak/cocoa output)
# is redirected to STDERR; only our solution lines reach the graded stdout.
LOG="$(mktemp "${TMPDIR:-/tmp}/cocoa_run.XXXXXX")"
cleanup() { rm -f "$LOG"; }
trap cleanup EXIT

python3 -u "$SCRIPT_DIR/portfolio/pipeline.py" "$INSTANCE" --budget 3600 >"$LOG" 2>&1
# Mirror the full trace to stderr (allowed; graded output is stdout only).
cat "$LOG" >&2

# The pipeline prints `COUNT: <N>` on success (status=solved); absent on
# timeout / both-engines-fail.
COUNT="$(grep -E '^COUNT:' "$LOG" | tail -1 | awk '{print $2}')"

if [ -z "${COUNT:-}" ]; then
    # No exact count within the time/memory budget.
    echo "c o no exact result within the budget (timeout / both engines failed)"
    echo "s UNKNOWN"
    echo "c s type mc"
    exit 0
fi

# ---- Emit the MC2026 solution lines (arbitrary-precision log10 via Python). --
python3 - "$COUNT" <<'PY'
import sys
from decimal import Decimal, getcontext
getcontext().prec = 60
raw = sys.argv[1].strip()
try:
    N = int(raw)
except ValueError:
    print("c o ERROR: could not parse count: %r" % raw)
    print("s UNKNOWN"); print("c s type mc"); sys.exit(0)

print("c o solution from the COCOA #SAT portfolio (cocoa canonical-cache / ganak fallback)")
if N == 0:
    print("s UNSATISFIABLE")
    print("c s type mc")
    print("c s log10-estimate -inf")
    print("c s exact arb int 0")
else:
    log10 = Decimal(N).ln() / Decimal(10).ln()   # exact for arbitrarily large N
    print("s SATISFIABLE")
    print("c s type mc")
    print("c s log10-estimate %.15f" % log10)
    print("c s exact arb int %d" % N)
PY
exit 0
