#!/usr/bin/env zsh
# Test script for clause branching: correctness + performance regression
# Usage: ./test_clause_branching.sh [--reset] [build_dir]
#
# Small tests: correctness only (count must match).
# Larger tests: correctness + performance regression detection.
# Use --reset to re-record baselines.

BUILD_DIR="build"
BASELINE_FILE=".test_baselines"
PASS=0
FAIL=0
WARN=0
TIME_LIMIT=10
REGRESSION_THRESHOLD=10  # percent increase that triggers a warning

for arg in "$@"; do
    if [ "$arg" = "--reset" ]; then
        rm -f "$BASELINE_FILE"
        echo "Baselines cleared."
    else
        BUILD_DIR="$arg"
    fi
done

SOLVER="$BUILD_DIR/sharpSAT"

get_baseline() {
    local name="$1"
    if [ -f "$BASELINE_FILE" ]; then
        grep "^${name}=" "$BASELINE_FILE" 2>/dev/null | cut -d= -f2
    fi
}

set_baseline() {
    local name="$1" time="$2"
    if [ -f "$BASELINE_FILE" ]; then
        if grep -q "^${name}=" "$BASELINE_FILE" 2>/dev/null; then
            sed -i '' "s/^${name}=.*/${name}=${time}/" "$BASELINE_FILE"
        else
            echo "${name}=${time}" >> "$BASELINE_FILE"
        fi
    else
        echo "${name}=${time}" > "$BASELINE_FILE"
    fi
}

# Correctness-only test (no performance tracking)
run_correctness_test() {
    local name="$1"
    local file="$2"
    local cb_flag="$3"
    local expected_count="$4"

    local output
    output=$(timeout $TIME_LIMIT "$SOLVER" $cb_flag "$file" 2>&1)
    if [ $? -ne 0 ]; then
        echo "FAIL  $name — solver crashed or timed out"
        FAIL=$((FAIL + 1))
        return
    fi

    local count=$(echo "$output" | grep -A1 '# solutions' | tail -1 | tr -d ' ')
    if [ "$count" != "$expected_count" ]; then
        echo "FAIL  $name — count=$count expected=$expected_count"
        FAIL=$((FAIL + 1))
        return
    fi

    echo "PASS  $name  (count=$count)"
    PASS=$((PASS + 1))
}

# Correctness + performance regression test
run_perf_test() {
    local name="$1"
    local file="$2"
    local cb_flag="$3"
    local expected_count="$4"

    local output
    output=$(timeout $TIME_LIMIT "$SOLVER" $cb_flag "$file" 2>&1)
    if [ $? -ne 0 ]; then
        echo "FAIL  $name — solver crashed or timed out"
        FAIL=$((FAIL + 1))
        return
    fi

    local count=$(echo "$output" | grep -A1 '# solutions' | tail -1 | tr -d ' ')
    local time=$(echo "$output" | grep '^time:' | sed 's/time: //;s/s//')

    if [ "$count" != "$expected_count" ]; then
        echo "FAIL  $name — count=$count expected=$expected_count"
        FAIL=$((FAIL + 1))
        return
    fi

    local baseline=$(get_baseline "$name")
    local result_tag="PASS"
    local detail="time=${time}s"

    if [ -n "$baseline" ]; then
        local pct_increase=$(echo "scale=1; ($time - $baseline) * 100 / $baseline" | bc -l 2>/dev/null)
        local is_regression=$(echo "$pct_increase > $REGRESSION_THRESHOLD" | bc -l 2>/dev/null)
        detail="time=${time}s (baseline=${baseline}s"
        if [ "$is_regression" = "1" ]; then
            result_tag="WARN"
            detail="${detail}, +${pct_increase}% REGRESSION"
            WARN=$((WARN + 1))
        else
            detail="${detail}, ${pct_increase}%"
        fi
        detail="${detail})"
    else
        detail="time=${time}s (new baseline)"
        set_baseline "$name" "$time"
    fi

    echo "$result_tag  $name  ($detail)"
    PASS=$((PASS + 1))
}

echo "=== Clause Branching Tests ==="
echo ""

echo "--- Correctness ---"
run_correctness_test "single_clause"        tests/test_single_clause.cnf ""       "28"
run_correctness_test "single_clause_cb3"    tests/test_single_clause.cnf "-cb 3"  "28"
run_correctness_test "test_cb"              tests/test_cb.cnf            ""       "23"
run_correctness_test "test_cb_cb3"          tests/test_cb.cnf            "-cb 3"  "23"
run_correctness_test "test_cb2"             tests/test_cb2.cnf           ""       "194"
run_correctness_test "test_cb2_cb3"         tests/test_cb2.cnf           "-cb 3"  "194"

echo ""
echo "--- Correctness + Performance ---"
run_perf_test "v50"                  tests/test_cb_perf.cnf       ""       "4932877747456"
run_perf_test "v50_cb8"              tests/test_cb_perf.cnf       "-cb 8"  "4932877747456"
run_perf_test "test183"              tests/test183.cnf            ""       "1813388729421943762059264"
run_perf_test "test183_cb8"          tests/test183.cnf            "-cb 8"  "1813388729421943762059264"

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $WARN warnings ==="
if [ $WARN -gt 0 ]; then
    echo "WARNING: Performance regressions detected! Run with --reset after investigating."
fi
exit $FAIL
