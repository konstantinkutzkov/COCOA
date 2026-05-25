# Plan: extend `precomputed_key` plumbing through the remaining recursive call sites

**Status:** draft plan; do not implement without the explicit per-site
audit + smoke pass below.

**Background:** Commit `cf60d14` added the optional
`const CanonicalKey *precomputed_key = nullptr` parameter to
`Solver::solveComponent`. One call site (current `solver_rec.cpp:1320`,
the post-consumption decompose loop) passes a real key. The other six
recursive `solveComponent` calls pass `nullptr`, paying a redundant
`buildCanonicalKey` + L2 `peek` at entry. 60 s OP_STATS measurement on
t1_049 shows the current binary builds canonical_key **2.1× more often
per decision** than the historical 834f33a baseline (1.17 vs 0.55 per
decision). Reducing this multiplier is the largest remaining concrete
lever for closing the 39 s gap to historical.

The existing `docs/precomputed_key_safety_analysis.md` audited five
sites and concluded the other four "cannot have a key available" — but
that audit may be conservative for the rest-continuation pattern, and
the audit predates today's diagnostic work (the sub-agent flagged this
as a live opportunity). This plan re-audits each remaining site under
current line numbers, designs the extension safely, and lays out the
test protocol.

## Inventory of remaining call sites (current line numbers)

| Line | Path | Existing audit verdict | Plan |
|---|---|---|---|
| 304  | root post-root-decompose loop | No key | re-audit (1) |
| 887  | mid-separator decompose recursion | No key | re-audit (2) |
| 1320 | post-consumption decompose loop | YES (already plumbed) | unchanged |
| 1827 | rest-continuation (var consumed by BCP, separator) | "No (BCP changed state)" | **re-audit (3) — likely INCORRECT** |
| 1851 | rest-continuation (clause removed/satisfied, separator) | "No (BCP changed state)" | **re-audit (4) — likely INCORRECT** |
| 2015 | branchOnLiteral: lit already T_TRI, no decision made | No (BCP changed state) | re-audit (5) |
| 2157 | branchOnClause: arm0-equiv path | No | re-audit (6) |
| 2394 | branchOnClause: arm1 path | No | re-audit (7) |

The two "rest-continuation" sites (1827, 1851) are the most likely
candidates the original audit missed. They recurse with the SAME `comp`
AFTER a separator element has been consumed (var assigned by an earlier
BCP, or clause removed/satisfied). The mutation that consumed the element
happened BEFORE `solveComponent` was entered for this comp — so the key
built at the entry CANONICAL site already reflects it. By the time we
reach the rest-continuation, all branching side-effects (sub-component
solves, recursive solveComponent calls) have been unwound by their own
unSet stacks. The literal_values_ visible at the rest-continuation
should equal the literal_values_ at the entry CANONICAL build.

If that's true, **the entry-built key is still valid** at the
rest-continuation. We just need to plumb it.

## Per-site re-audit checklist

For each remaining site, confirm the following before plumbing a key:

**A. State equivalence.** Are `literal_values_`, `removed_clauses_`,
   `learned_clause_scope_` — every input `buildCanonicalKey` reads —
   identical at the recursion site to what they were when the candidate
   key was built? If any input was mutated and not fully restored, the
   key is stale.

**B. Same component.** Is `comp` the same Component object as when the
   candidate key was built? `buildCanonicalKey` is a function of `comp`'s
   vars/clauses; passing a key built for a different component is unsafe.

**C. Key availability.** Does the caller actually have a key in scope?
   For sites in branchOnLiteral / branchOnClause, the parent's key was
   built in solveComponent's body — to forward it, we'd need to add a
   parameter to branchOnLiteral / branchOnClause.

**D. Cache miss precondition.** The `precomputed_key` path skips
   `peek` and asserts "caller established a cache miss." If the caller's
   peek wasn't done OR the peek result was a HIT (so the caller used
   the cached value and never recursed), the recursion is unreachable
   in the hit case. For the rest-continuation paths, the parent did
   peek + miss + entered solveComponent's body — that property still
   holds at the rest-continuation. Verify per site.

## Proposed per-site dispositions

These are HYPOTHESES — each requires running the per-site re-audit
checklist before implementation.

1. **Site 304** (root post-root-decompose loop). No precomputed key
   available — the root has no caller that built one for these sub-comps.
   The body must build it at recursion entry. **Leave nullptr.**

2. **Site 887** (mid-separator decompose recursion). Same as 304 — sub-
   components produced by a decompose; no parent has built a key for
   them. **Leave nullptr.**

3. **Site 1827** (rest-continuation, var consumed by BCP, separator).
   Same comp, recursion. Hypothesis: entry-built key still valid.
   **Candidate for plumbing if A/B/C/D check out.** Requires the key
   built at solveComponent line 396 to be kept in a local variable
   alive until line 1827 (it is — `cached_key` survives the entire
   solveComponent body).

4. **Site 1851** (rest-continuation, clause removed/satisfied, separator).
   Same as 1827. **Candidate for plumbing.**

5. **Site 2015** (branchOnLiteral: lit already T_TRI). Same comp,
   recursion. branchOnLiteral was called WITH a state where lit was
   already T_TRI. If branchOnLiteral takes a `parent_key` parameter,
   the caller's solveComponent can forward its `cached_key`. **Candidate
   if branchOnLiteral signature change is acceptable.**

6. **Sites 2157, 2394** (branchOnClause arms after BCP). The branchOnClause
   negate arm has SET several lits (via setLiteralAsBranchConstraint),
   actually mutating `literal_values_` for vars in comp. Therefore the
   parent's pre-call key is STALE — does not match the current state.
   **Leave nullptr.** (The original audit was right on these.)

Net: 3 plausible new sites (1827, 1851, 2015), 4 must stay nullptr.

## Implementation order

One site at a time. For each:

1. **Re-audit:** walk the A/B/C/D checklist on the site, reading the
   code path between key-build and recursion to confirm no input is
   mutated-and-not-restored. Document in a short comment block near
   the call site.

2. **Code change:**
   - For sites 1827 and 1851: change call signature from
     `solveComponent(comp, rest, …)` to
     `solveComponent(comp, rest, …, &cached_key)`. `cached_key` is
     already in scope (built at line 396, kept as a local).
   - For site 2015: add a `const CanonicalKey *parent_key = nullptr`
     parameter to `Solver::branchOnLiteral`. At the recursion site,
     pass `parent_key`. At the solveComponent caller of branchOnLiteral,
     pass `&cached_key`. (The "lit already T_TRI" case at 2015 is the
     only branch of branchOnLiteral that recurses with the same comp;
     other branches do BCP and need a fresh key.)

3. **Smoke test:**
   - `./sharpSAT -rec -sep 5 -cb 3 -sepMode metis temp_cnf/mc2025_track1_065.cnf`
     → count must be `37778931862957161709568`.
   - `./sharpSAT -rec -sep 5 -cb 3 -sepMode metis temp_cnf/mc2025_track1_011.cnf`
     → count must be `536870912306`.
   - Both must finish under 1.5× their current wall time (regression
     guard).

4. **Perf measurement:**
   - 60 s run on t1_049 (`-t 60`). Compare decisions/60 s and OP_STATS
     vs the pre-change baseline (currently 27.3 M dec / 60 s).
   - If improvement < 1 % (within noise) and no regression: keep the
     change. If 1-5 % improvement: keep and proceed to next site. If
     larger improvement: full t1_049 run to confirm.

5. **Full t1_049 run** after all candidate sites are plumbed. Wall
   time target: <320 s (i.e., 2 s better than today's 321.6 s). Count
   must match `8695763196077742`.

6. **Cross-instance sanity:** run t1_011, t1_065, t1_071 to confirm
   counts on the standard suite.

## Test plan (mandatory before commit)

For each commit (one per plumbed site, or one bundled commit):

| Test | Pass criterion |
|---|---|
| t1_065 count | exactly `37778931862957161709568` |
| t1_011 default count | exactly `536870912306` |
| t1_011 + `-derivCacheBias 1` count | exactly `536870912306` (regression test for the historic UIP bug) |
| t1_049 60 s decision throughput | ≥ baseline; no >2 % drop |
| t1_049 full run count | exactly `8695763196077742` |
| t1_049 full run wall time | ≤ today's 321.6 s baseline |

Commit only if all pass.

## Rollback strategy

Each site's plumbing is a small isolated change (one parameter added at
the call site, optionally one parameter added to branchOnLiteral). If
any count regression appears, `git revert <site-commit>` is clean —
keep one commit per site so this stays granular. Do NOT bundle multiple
sites into a single commit unless every site has passed the per-site
smoke independently first.

## Risks and unknowns

- **Hidden state mutations between line 396 (entry key build) and the
  rest-continuation sites at 1827/1851.** The hypothesis above says
  nothing mutates literal_values_ without being unwound, but the code
  path between those lines is ~1430 lines long. The re-audit (step 1)
  must walk it carefully — at minimum spot-check the SubVarsetGuard
  scope, the conflict-clause learning path, and any places
  `markClauseRemoved` / `unmarkClauseRemoved` could fire without
  matching restoration.

- **The branchOnLiteral signature change** (for site 2015) propagates
  to every branchOnLiteral caller. Audit all callers to confirm the
  new `parent_key` parameter is correct (nullptr is always safe;
  passing a wrong key is corruption).

- **Cross-instance count regressions.** The historical UIP bug
  (`t1_011 + -derivCacheBias 1` giving wrong count) is the canary —
  if any plumbed key change breaks that test it means the precomputed
  path is being taken where it shouldn't. Run that test specifically.

## What we are NOT doing in this plan

- Not removing the redundant CANONICAL build at solveComponent's entry
  — that's still the safe path when no precomputed key is available.
- Not changing canonical_key semantics or invariants.
- Not touching the sound_provenance check (orthogonal — already verified
  in 60 s probe to be cost-free).

## Estimated effort

- Re-audit per site: ~20-30 min careful reading
- Implementation per site: ~10-15 min plus smoke
- Test plan execution: ~10 min smoke + 5 min 60 s perf
- Full t1_049 perf run after all sites: ~6 min
- Total for all three candidate sites: ~3 hours of focused work,
  half of which is the audit reading.
