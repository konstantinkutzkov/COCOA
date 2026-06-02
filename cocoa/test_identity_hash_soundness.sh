#!/bin/bash
# test_identity_hash_soundness.sh
#
# Regression guard for the 2026-06-02 buildIdentityKey fix.
#
# buildIdentityKey (the -hashMode identity cache key) used to omit binary
# clauses from both the packed key AND n_in_clauses. That produced wrong
# model counts on binary-containing formulas via two mechanisms:
#   (1) free-var miscount: a binary-only var was treated as free, inflating
#       the 2^free_vars factor (dominant; usually under-counts);
#   (2) key collision: components differing only in binaries hashed equal
#       (silent wrong cache hits; non-power-of-2 wrong values).
# Canonical mode always included binaries and was sound.
#
# This test asserts that on binary-containing formulas, -hashMode identity
# agrees with -hashMode canonical (the trusted reference): both the curated
# historical reproducers AND a randomized binary-heavy stress.

SOLVER="build/sharpSAT"
FLAGS="-rec -sep 5 -cb 3"
PASS=0
FAIL=0

count() {  # count <hashmode> <cnf>
    $SOLVER $FLAGS -hashMode "$1" "$2" 2>/dev/null \
        | grep -A1 '# solutions' | tail -1
}

echo "=== Curated reproducers (identity must equal canonical) ==="
# name:expected_true_count  (expected from canonical/ganak)
for entry in \
    "tests/identity_bin_min_over.cnf:29" \
    "tests/identity_bin_collision.cnf:126" \
    "tests/identity_bin_freevar.cnf:272" \
    "tests/bug_sep_16v.cnf:15012" \
    "tests/bug_sep_18v.cnf:9598" \
    "tests/test_cb2.cnf:194"; do
    f="${entry%%:*}"; exp="${entry##*:}"
    id=$(count identity "$f")
    cn=$(count canonical "$f")
    if [ "$id" = "$cn" ] && [ "$id" = "$exp" ]; then
        echo "  PASS $(basename "$f")  identity=canonical=$id"
        PASS=$((PASS+1))
    else
        echo "  FAIL $(basename "$f")  identity=$id canonical=$cn expected=$exp"
        FAIL=$((FAIL+1))
    fi
done

echo ""
echo "=== Random binary-heavy stress (identity vs canonical) ==="
python3 -c "
import random, subprocess, sys
def count(mode, cnf):
    out = subprocess.run(['build/sharpSAT','-rec','-sep','5','-cb','3',
                          '-hashMode',mode,cnf],
                         capture_output=True, text=True, timeout=15).stdout
    L = out.split('\n')
    for i,l in enumerate(L):
        if '# solutions' in l:
            for j in range(i+1,len(L)):
                if L[j].strip(): return L[j].strip()
    return None
fails = 0; tested = 0
for trial in range(300):
    random.seed(trial + 424242)
    n = random.randint(8, 26)
    cls = []
    for _ in range(random.randint(n, n*3)):
        k = min(random.choice([2,2,2,3,3,4]), n)   # binary-heavy
        lits = random.sample(range(1, n+1), k)
        cls.append([l if random.random() > 0.5 else -l for l in lits])
    fn = f'/tmp/idsound_{trial}.cnf'
    with open(fn,'w') as fh:
        fh.write(f'p cnf {n} {len(cls)}\n')
        for c in cls: fh.write(' '.join(map(str,c)) + ' 0\n')
    ci = count('identity', fn); cc = count('canonical', fn)
    import os; os.unlink(fn)
    if ci is None or cc is None: continue
    tested += 1
    if ci != cc:
        print(f'  FAIL trial={trial} n={n} identity={ci} canonical={cc}')
        fails += 1
print(f'  {fails} failures out of {tested} tested')
sys.exit(fails)
"
if [ $? -eq 0 ]; then
    echo "  ALL PASSED"
    PASS=$((PASS+1))
else
    FAIL=$((FAIL+1))
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
exit $FAIL
