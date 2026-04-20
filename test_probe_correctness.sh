#!/bin/bash
# test_probe_correctness.sh — Phase-1 regression harness.
#
# 1. Runs the C++ unit harness (tests/test_probe.cpp) which exercises
#    Solver::probeLiteral / commitFailedLiteral against the Phase-1 CNFs
#    and checks vars_forced, delta_2clauses, and UIP-clause contents
#    against the .expected sidecars.
#
# 2. Runs the full solver on each CNF under three configurations
#    (default, -noIBCP, -noCC) and checks the model count matches the
#    expected value — verifying the implicitBCP refactor has not
#    changed behaviour.

set -u

SOLVER="build/sharpSAT"
PROBE_BIN="build/test_probe"

if [ ! -x "$SOLVER" ]; then
    echo "ERROR: $SOLVER not found. Build it first (cmake + make)."
    exit 2
fi
if [ ! -x "$PROBE_BIN" ]; then
    echo "ERROR: $PROBE_BIN not found. Build it first (cmake + make test_probe)."
    exit 2
fi

TESTS=(
    cascade_binary_chain
    cascade_fanout
    cascade_3to2_shortening
    cascade_ternary_primed
    failed_lit_direct
    failed_lit_chain
    both_polarities_fail
    no_failed_lit
)

echo "=== Unit probe tests (test_probe) ==="
args=()
for name in "${TESTS[@]}"; do
    args+=("tests/${name}.cnf" "tests/${name}.expected")
done
"$PROBE_BIN" "${args[@]}"
UNIT_RC=$?

echo ""
echo "=== End-to-end model-count regression ==="
PASS=0
FAIL=0
for name in "${TESTS[@]}"; do
    cnf="tests/${name}.cnf"
    expected="tests/${name}.expected"
    expected_count=$(awk '/^model_count/ {print $2; exit}' "$expected")

    for args in "" "-noIBCP" "-noCC" "-noIBCP -noCC"; do
        actual=$($SOLVER $args "$cnf" 2>/dev/null | awk '/# solutions/ {getline; print; exit}')
        if [ "$actual" = "$expected_count" ]; then
            PASS=$((PASS+1))
        else
            echo "FAIL ${name} ($args) expected=${expected_count} actual=${actual}"
            FAIL=$((FAIL+1))
        fi
    done
done
echo "Model-count regression: PASS=${PASS} FAIL=${FAIL}"

if [ $UNIT_RC -ne 0 ] || [ $FAIL -ne 0 ]; then
    exit 1
fi
echo ""
echo "ALL PHASE-1 TESTS PASS"
exit 0
