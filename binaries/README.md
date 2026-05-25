# Checkpointed binaries

Reference binaries for A/B regression testing against historical baselines.
Each binary is named `sharpSAT.<git-sha7>` and is accompanied by a
sidecar `sharpSAT.<git-sha7>.md` documenting:
- the commit (full SHA + one-line description)
- build flags (Release, library paths)
- characterization timings (which instance, which CLI flags, observed
  wall time)
- any build-time fixups needed (e.g. files copied from a later commit
  to make the historical tree build with the current toolchain)

When comparing a current binary against a historical baseline, ALWAYS
use the binary from this directory rather than checking out the commit
and rebuilding (which is slow and error-prone). When a new
performance-relevant commit lands, add a checkpoint binary here.

## Standard regression suite

The reference benchmark for end-to-end timing comparisons:

```
./<binary> -rec -sep 5 -cb 3 -sepMode metis temp_cnf/mc2025_track1_049.cnf
```

t1_049 is the workhorse — finishes in ~5 min, exercises the full
search + cache + UIP machinery, and has a well-known historical
baseline (834f33a at ~282 s).

For per-bucket BCP / canonical-key cost analysis, run with `-t 60`
and parse OP_STATS lines from stderr.
