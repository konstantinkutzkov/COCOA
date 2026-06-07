# Portfolio run-book — run ONE MC2025 instance end-to-end

This file exists so we **don't re-derive the procedure every session**.
Detailed results live in [`cocoa/docs/benchmark_log.md`](../cocoa/docs/benchmark_log.md) — that is the source of truth for counts.

## Where the instances live
- **Canonical set — 100 instances, odd `001`–`199`, xz-COMPRESSED:**
  `/Users/konstantin.kutzkov/Desktop/Code/SharpSAT/MC2025_Public/mc2025_track1_public/mc2025_track1_NNN.cnf.xz`
- **Decompressed working cache** (decompress here on demand; NEVER `/tmp` for CNFs — macOS wipes it):
  `/Users/konstantin.kutzkov/Desktop/Code/SharpSAT/temp_cnf/mc2025_public/mc2025_track1_NNN.cnf`

## Run one instance (copy-paste)
```bash
R=/Users/konstantin.kutzkov/Desktop/Code/COCOA
SRC=/Users/konstantin.kutzkov/Desktop/Code/SharpSAT/MC2025_Public/mc2025_track1_public
WORK=/Users/konstantin.kutzkov/Desktop/Code/SharpSAT/temp_cnf/mc2025_public
N=031                                                  # <-- instance number

# 1) decompress on demand (keeps the .xz; no-op if already cached)
[ -f "$WORK/mc2025_track1_$N.cnf" ] || xz -dkc "$SRC/mc2025_track1_$N.cnf.xz" > "$WORK/mc2025_track1_$N.cnf"

# 2) pre-flight — CPU MUST be idle, or timings are garbage
ps aux | grep -iE "[s]harpSAT|[g]anak" | grep -v grep   # expect NO output

# 3) run the 8-config pipeline, full 1 h budget, in the background
cd "$R/portfolio"
PORTFOLIO_METIS_FEATURES_BIN=$R/cocoa/build/metis_features \
PORTFOLIO_NDCOST_BIN=$R/cocoa/build/nd_cost \
PORTFOLIO_SHARPSAT_BIN=$R/cocoa/build/sharpSAT \
PORTFOLIO_GANAK_BIN=$R/ganak-canonical/build/ganak \
nohup python3 -u pipeline.py "$WORK/mc2025_track1_$N.cnf" --budget 3600 \
  > runlogs/t1_$N.log 2>&1 &

tail -f runlogs/t1_$N.log                               # watch progress
```

## Always verify the count independently
```bash
$R/ganak-canonical/build/ganak --prob 0 "$WORK/mc2025_track1_$N.cnf"
# "c s exact arb int <N>" MUST equal the pipeline count. A mismatch = critical bug, STOP.
```

## Always record, right after the run
Append to `cocoa/docs/benchmark_log.md`: instance · count · VERIFIED/unverified · wall time ·
nd band · winning config · exact flags / notes.

## Pipeline flow (current)
METIS + `nd_cost` → band (low/mid/high) **orders** an **8-config** covering set (order only
affects short-circuit latency, not selection) → funnel **8→4→2→1** by `predict_cb` ETA, ~1 min/round,
with a round-1 **memory-admission gate** (stop starting new candidates once resident RSS > 20 GB; never
kills a running one) → the single leader runs to the budget → hands to **Ganak** only on sustained
overshoot (10 consecutive 10 s forecasts of ETA > 1.25× remaining). Any config that FINISHES early
short-circuits and wins immediately.

## Status — what we've RUN (32 of 100)
See `benchmark_log.md` for counts/detail. Logged so far:
```
001 003 005 007 009 011 013 015 021 023 025 027 041 045 047 049 053 059
065 067 069 071 073 089 101 105 123 149 159 163 167 175
```
**To pick a NEW instance:** any odd `NNN` in `001`–`199` NOT in the list above (68 remain — e.g.
017 019 029 031 033 035 037 039 043 051 055 057 061 063 075 077 079 081 …).
