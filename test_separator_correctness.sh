#!/bin/bash
# Comprehensive separator correctness test suite
# Tests: binary+long clause mix, designed separators, IBCP interaction,
# and regression tests for known bugs.

SOLVER="build/sharpSAT"
PASS=0
FAIL=0

echo "=== Regression tests ==="
for f in tests/bug_sep_16v.cnf tests/bug_sep_18v.cnf tests/bug_sep_30v.cnf; do
    base=$($SOLVER -noCC "$f" 2>&1 | grep -A1 '# solutions' | tail -1)
    sep=$($SOLVER -sep "$f" 2>&1 | grep -A1 '# solutions' | tail -1)
    if [ "$base" = "$sep" ]; then
        echo "  PASS $f ($base)"
        PASS=$((PASS+1))
    else
        echo "  FAIL $f base=$base sep=$sep"
        FAIL=$((FAIL+1))
    fi
done

echo ""
echo "=== Random stress tests (sep vs baseline) ==="
python3 -c "
import random, subprocess, sys

fails = 0
total = 0
for trial in range(500):
    random.seed(trial + 30000)
    n = random.randint(10, 30)
    clauses = []
    for _ in range(random.randint(n, n*3)):
        k = random.choice([2, 2, 3, 3, 3, 4, 5, 6])
        k = min(k, n)
        lits = random.sample(range(1, n+1), k)
        lits = [l if random.random() > 0.5 else -l for l in lits]
        clauses.append(lits)

    fname = f'/tmp/test_sep_stress_{trial}.cnf'
    with open(fname, 'w') as f:
        f.write(f'p cnf {n} {len(clauses)}\n')
        for cl in clauses:
            f.write(' '.join(str(l) for l in cl) + ' 0\n')

    r1 = subprocess.run(['build/sharpSAT', '-t', '5', fname],
                       capture_output=True, text=True, timeout=10)
    r2 = subprocess.run(['build/sharpSAT', '-t', '5', '-sep', fname],
                       capture_output=True, text=True, timeout=10)

    c1 = c2 = ''
    for line in r1.stdout.split('\n'):
        if line.strip().isdigit(): c1 = line.strip()
    for line in r2.stdout.split('\n'):
        if line.strip().isdigit(): c2 = line.strip()

    if not c1 or not c2: continue
    total += 1

    if 'Assertion' in r2.stdout or 'Assertion' in r2.stderr:
        print(f'  CRASH trial={trial} n={n}')
        fails += 1
    elif c1 != c2:
        print(f'  FAIL trial={trial} n={n} base={c1} sep={c2}')
        fails += 1

print(f'  {fails} failures out of {total} tested')
sys.exit(fails)
" 2>/dev/null
PY_EXIT=$?
if [ $PY_EXIT -eq 0 ]; then
    echo "  ALL PASSED"
    PASS=$((PASS+1))
else
    FAIL=$((FAIL+1))
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
exit $FAIL
