# Portfolio run-book — run ONE MC competition instance end-to-end

This file exists so we **don't re-derive the procedure every session**.
Detailed results live in [`cocoa/docs/benchmark_log.md`](../cocoa/docs/benchmark_log.md) — that is the source of truth for counts.

## Where the instances live
- **CURRENT set — MC2026 track1** (100 instances, odd `001`–`199`, xz-COMPRESSED):
  `/Users/konstantin.kutzkov/Desktop/Code/SharpSAT/MC2026_Public/mc2026_track1_public/mc2026_track1_NNN.cnf.xz`
  Working cache (decompress here on demand; NEVER `/tmp` — macOS wipes it): `…/SharpSAT/temp_cnf/mc2026_public/`
- **PRIOR set — MC2025 track1** (already worked through; see benchmark_log): `…/MC2025_Public/mc2025_track1_public/`, cache `temp_cnf/mc2025_public/`.
- **Logging names:** mc2026 → `mc2026_track1_NNN`, mc2025 → `t1_NNN`. Same numbers are DIFFERENT instances (verified zero content overlap), so keep them distinct in benchmark_log.

## Run one instance (copy-paste)
```bash
R=/Users/konstantin.kutzkov/Desktop/Code/COCOA
SRC=/Users/konstantin.kutzkov/Desktop/Code/SharpSAT/MC2026_Public/mc2026_track1_public
WORK=/Users/konstantin.kutzkov/Desktop/Code/SharpSAT/temp_cnf/mc2026_public
N=001                                                  # <-- instance number  (for mc2025: swap mc2026->mc2025, MC2026->MC2025)

# 1) decompress on demand (keeps the .xz; no-op if already cached)
mkdir -p "$WORK"
[ -f "$WORK/mc2026_track1_$N.cnf" ] || xz -dkc "$SRC/mc2026_track1_$N.cnf.xz" > "$WORK/mc2026_track1_$N.cnf"

# 2) pre-flight — CPU MUST be idle, or timings are garbage
ps aux | grep -iE "[s]harpSAT|[g]anak" | grep -v grep   # expect NO output

# 3) run the 8-config pipeline, full 1 h budget, in the background
cd "$R/portfolio"
PORTFOLIO_METIS_FEATURES_BIN=$R/cocoa/build/metis_features \
PORTFOLIO_NDCOST_BIN=$R/cocoa/build/nd_cost \
PORTFOLIO_SHARPSAT_BIN=$R/cocoa/build/sharpSAT \
PORTFOLIO_GANAK_BIN=$R/ganak-canonical/build/ganak \
nohup python3 -u pipeline.py "$WORK/mc2026_track1_$N.cnf" --budget 3600 \
  > runlogs/mc2026_t1_$N.log 2>&1 &

tail -f runlogs/mc2026_t1_$N.log                        # watch progress
```

## Always verify the count independently
```bash
$R/ganak-canonical/build/ganak --prob 0 "$WORK/mc2026_track1_$N.cnf"
# "c s exact arb int <N>" MUST equal the pipeline count. A mismatch = critical bug, STOP.
```

## Always record, right after the run
Append to `cocoa/docs/benchmark_log.md`: instance · count · VERIFIED/unverified · wall time ·
nd band · winning config · exact flags / notes.

## Pipeline flow (current)
METIS + `nd_cost` → band orders an **8-config** covering set (order only affects short-circuit
latency) → funnel **8→4→2→1**, ~1 min/round: the **round-1 cut (8→4) ranks by `predict_cb`** (only 60s
of data), the **round-2/3 cuts (4→2, 2→1) by `predict_cb_arima`** (≥120s, past ARIMA's 90s floor).
Round 1 has a **memory-admission gate** (don't start a new config once resident RSS > 20 GB). The single
leader runs to the budget, handing to **Ganak** only after **10 consecutive 10 s ARIMA forecasts of
ETA > 1.25× remaining** AND **≥3 min active** (the fair-chance floor). Any config that FINISHES early
short-circuits and wins immediately.

## Status — instances RUN
**MC2026 (current set):** `001` (count 48). 1 of 100 done — pick any other odd `NNN` in `001`–`199`.
**MC2025 (prior set; see benchmark_log):**
```
001 003 005 007 009 011 013 015 017 019 021 023 025 027 031 041 045 047 049 053
059 065 067 069 071 073 089 101 105 123 149 159 163 167 175
```
