# t1_011 order-dependent miscount — observations

## Reproducer files

Shrunken pair derived from `t1_011.cnf` by the overnight shrinker
(`/tmp/shrink_sat2.py`), located at:

- `/tmp/sat_shrink2/latest_wrong.cnf`
- `/tmp/sat_shrink2/latest_correct.cnf`

Both files have header `p cnf 2045 10029` (10001 original clauses + 28
appended unit clauses). The 28 locked units are listed in
`/tmp/sat_shrink2/latest_units.txt`:

```
-207 -598 1369 -400 -113 -841 -1922 -183 -725 1354 352 1357 18 -1989
-1214 -97 248 75 -528 -608 -264 354 1371 -632 -1967 -789 -355 -1210
```

The two files contain the same clauses and the same appended units; they
differ only in clause ordering (shuffled at the start of the shrinker
run and carried through).

## Solver version

Binary: `/Users/konstantin.kutzkov/Desktop/Code/SharpSAT/sharpsat-separator/build/sharpSAT`
(timestamp 2026-04-23 13:17, post orphan-leaf fix, with size gate
`min(0.3*n, 20)`).

Command used for all runs:

```
sharpSAT -rec -sep 5 -cb 3 -sepMode metis <file.cnf>
```

## Counts

Ganak (oracle, `/tmp/ganak/ganak`):

| File | Ganak count |
|---|---|
| `latest_wrong.cnf` | 1,310,720 |
| `latest_correct.cnf` | 1,310,720 |

Our solver:

| File | Count | Time |
|---|---|---|
| `latest_wrong.cnf` | 1,310,720 | 0.54 s |
| `latest_correct.cnf` | 1,179,648 | 0.58 s |

Difference:

```
1,310,720 − 1,179,648 = 131,072 = 2^17
```

Factored:

```
1,310,720 =     10 × 2^17
1,179,648 =      9 × 2^17
```

On `latest_correct.cnf` our solver produces an answer `2^17` smaller than
ganak. On `latest_wrong.cnf` our solver matches ganak. Both orderings
are mathematically identical formulas.

## Preprocessing footprint (both orderings)

Both orderings report identical preprocessing statistics:

```
variables (all/used/free):   2045/2045/0
variables (all/used/free):   1972/1972/0
variables (total / active / free)  1972/1972/0
```

Both start at 2045 variables, preprocess to 1972 active variables, with
0 free variables at the top level.

## Shrinker history

The file pair is the latest accepted reproducer from the overnight
shrinker's round-8 baseline:

```
[round 8] baseline c=1,179,648 w=1,310,720
```

Prior rounds (all showing an order-dependent miscount between our solver
on two clause orderings of the same formula):

| Round | Baseline c | Baseline w | Diff | Diff as power of 2 |
|---|---|---|---|---|
| 1 | 536,870,912,306 | 530,990,498,117 | 5,880,414,189 | not a power of 2 |
| 2 | 62,277,026,398 | 62,270,734,942 | 6,291,456 | not a power of 2 |
| 3 | 25,220,350,282 | 25,232,933,194 | 12,582,912 | not a power of 2 |
| 4 | 7,918,845,952 | 7,889,485,824 | 29,360,128 | not a power of 2 |
| 5 | 2,818,572,288 | 2,751,463,424 | 67,108,864 | 2^26 |
| 6 | 65,011,712 | 67,108,864 | 2,097,152 | 2^21 |
| 7 | 16,252,928 | 16,777,216 | 524,288 | 2^19 |
| 8 | 1,179,648 | 1,310,720 | 131,072 | **2^17** |

From round 5 onward the diff is a clean power of 2.

## Other tested instances from the same pair

Appending a single additional unit to both `latest_wrong.cnf` and
`latest_correct.cnf` produces:

| Base | Added unit | File | Our count | Time |
|---|---|---|---|---|
| wrong  | `1324 0`  | `/tmp/step1/wrong_pos.cnf`  | 2,818,572,288 | ~4 s |
| correct | `1324 0`  | `/tmp/step1/correct_pos.cnf` | 2,818,572,288 | ~3 s |
| wrong  | `-1324 0` | `/tmp/step1/wrong_neg.cnf`  | 0 | 0.60 s |
| correct | `-1324 0` | `/tmp/step1/correct_neg.cnf` | 0 | 0.39 s |

Note: these step1 files use the pre-round-8 parent formulas
(`latest_wrong.cnf` / `latest_correct.cnf` at that earlier point, 2045
vars / 10017 clauses).

## Behavior across fixes

| Event | `latest_wrong` → our count | `latest_correct` → our count |
|---|---|---|
| Before orphan-leaf fix | 1,310,720 | 1,179,648 |
| After orphan-leaf fix | 1,310,720 | 1,179,648 |
| After size-gate change to `min(0.3*n, 20)` | 1,310,720 | 1,179,648 |

The 2^17 order-dependent miscount reproduces unchanged across all three
solver versions tested today.

## Flags not yet tested against this reproducer

- `-noLearn`
- `-cb 0` (no clause branching)
- `-sep 0` (no ND-hierarchy / no separator branching)
- `-noIBCP`

## Files

- `/tmp/sat_shrink2/latest_wrong.cnf` (CNF with clause order that yields
  the correct count)
- `/tmp/sat_shrink2/latest_correct.cnf` (CNF with clause order that
  yields the `2^17`-undercount)
- `/tmp/sat_shrink2/latest_units.txt` (28 locked units)
- `/tmp/ganak/ganak` (oracle)
- `/Users/konstantin.kutzkov/Desktop/Code/SharpSAT/sharpsat-separator/build/sharpSAT`
  (our solver, post-orphan-leaf fix, timestamp 2026-04-23 13:17)
