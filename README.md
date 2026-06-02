# COCOA

A model-counting (#SAT) solver package combining two complementary engines
behind a selection layer:

- **`cocoa/`** — the full-counting engine (separator branching + clause
  branching + the canonical-key "anonymization" cache). Formerly
  `sharpsat-separator`.
- **`ganak-canonical/`** — a Ganak fork that is projection-capable and also
  supports the canonical-key cache (`--cachehash canonical`).
- **`portfolio/`** — the selection layer: routes an instance to the better
  engine + flags, and `ab_compare.py` for head-to-head configuration A/B.

`third_party/` vendors METIS + GKlib (the separator engine's dependencies).

See `build.sh` for the build order and the per-component READMEs for details.
