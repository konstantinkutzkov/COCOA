# ND-hierarchy orphan-leaf bug

## Summary

The ND-hierarchy built by `NDHierarchy::build` in [nd_hierarchy.cpp](../src/nd_hierarchy.cpp)
leaves some vars and clauses with **leaf IDs outside the root subtree's
`[leaf_lo..leaf_hi]` range**. At runtime, `mapToChild` at the root
misclassifies these "orphans" as belonging to the RIGHT subtree, producing
`-2` (invariant-violated) returns whenever a sub-component contains a mix
of true-LEFT vars and orphan vars.

One visible manifestation is `SEPARATOR_INVARIANT_VIOLATED` firing in
[solver_rec.cpp:600](../src/solver_rec.cpp#L600) and calling `std::abort()`,
which we reproduce deterministically on `/tmp/step1/correct_neg.cnf`
(`latest_correct.cnf` + unit `-1324`).

## Observed

On `correct_neg.cnf` with default flags (`-rec -sep 5 -cb 3 -sepMode metis`):

```
*** SEPARATOR_INVARIANT_VIOLATED ***
  nd_node=0 sub_vars.size()=266 removed_clauses=4
  child subtrees: left=[0..5] right=[6..18]
```

- Root node 0: `lc=1` covering leaves `[0..5]` (LEFT), `rc=2` covering `[6..18]` (RIGHT).
- Reported `npes=1177` total leaves. Therefore leaves `19..1176` are
  **not covered by the root** — they live in orphan subtrees.
- The ND-hierarchy dump contains vars/clauses with `leaf` values like
  `284, 292, 304, 321, 627, 631, 641` — well beyond `leaf_hi=18`.

## Root cause in the code

### BFS build produces a tree with dead subtrees

`NDHierarchy::build` builds the separator tree breadth-first
([nd_hierarchy.cpp:169-243](../src/nd_hierarchy.cpp#L169-L243)). Each
`WorkItem` is a vertex set + its tree-node index. For each item:

1. If `nv < min_sep_vars`, make the node a leaf and assign `leaf_id = next_leaf++`.
2. Otherwise attempt `bisect_subgraph` (METIS vertex-separator call).
3. If bisect succeeds, store the separator at this node, allocate two
   children, queue both subtrees.
4. If bisect **fails** (`METIS_ComputeVertexSeparator` returns non-OK, or
   L/R empty), fall back to making this node a leaf.

Leaf IDs are assigned sequentially from `next_leaf = 0`. Because the tree
is built BFS, leaves are numbered across sibling subtrees in the order
they're discovered — not in DFS order.

### Post-pass 1 DFS remap only covers root-reachable leaves

[nd_hierarchy.cpp:245-277](../src/nd_hierarchy.cpp#L245-L277) then walks
the tree **from node 0** in DFS order, assigning new contiguous leaf IDs:

```cpp
std::vector<int> old_to_new(next_leaf, -1);
int dfs_leaf_id = 0;
std::function<void(int)> dfs = [&](int node) {
    if (node < 0) return;
    if (separator[node].empty()) {
        int old_id = leaf_lo[node];
        if (old_id >= 0 && old_id < (int)old_to_new.size()) {
            if (old_to_new[old_id] < 0)
                old_to_new[old_id] = dfs_leaf_id++;
            leaf_lo[node] = leaf_hi[node] = old_to_new[old_id];
        }
        return;
    }
    dfs(left_child[node]);
    dfs(right_child[node]);
};
dfs(0);

for (size_t i = 0; i < var_leaf.size(); i++) {
    int old_id = var_leaf[i];
    if (old_id >= 0 && old_id < (int)old_to_new.size() && old_to_new[old_id] >= 0)
        var_leaf[i] = old_to_new[old_id];
}
```

If some leaves are **not reached from node 0**, their `old_to_new[old_id]`
stays at `-1`, and the remap condition `old_to_new[old_id] >= 0` is false, so
`var_leaf[i]` is NOT updated — it retains its pre-remap old ID.

### Why leaves become unreachable from node 0

If a bisection at an internal node partially succeeds but one child's
subsequent bisection fails catastrophically, the child pointer may be valid
but the subtree never assigns useful leaves to its vars. The `leaf_lo=-1`
cascading from unreachable children prevents post-pass 2 from propagating
ranges up. Nodes 3, 7, 8, 10, etc. in the dump all have
`leaf_lo=-1 leaf_hi=-1 sep_size=nonzero`, indicating separators stored at
nodes whose children never produced valid leaf ranges.

Post-pass 2 ([nd_hierarchy.cpp:279-303](../src/nd_hierarchy.cpp#L279-L303))
then sets internal-node `leaf_lo` only when BOTH children have valid
`leaf_lo`. For a dead-child node, leaves remain `-1`, and separator elements
at that node get `assigned_leaf = 0` (the fallback, line 295). But vars
placed into a deep leaf inside the dead subtree keep their pre-remap IDs.

### `mapToChild` misclassifies orphans as RIGHT

At [nd_hierarchy.cpp:358-368](../src/nd_hierarchy.cpp#L358-L368):

```cpp
for (unsigned var_id : active_var_ids) {
    if (var_id >= var_leaf.size()) continue;
    int leaf = var_leaf[var_id];
    if (leaf < 0) continue;
    if (leaf >= left_lo_val && leaf <= left_hi_val)
      n_left++;
    else
      n_right++;
}
```

An orphan with `leaf=284` falls in the `else` branch → counted as RIGHT.
A sub-component containing any orphan and any true-LEFT var (leaf ∈ [0..5])
yields `n_left>0 && n_right>0` → `return -2`.

## Evidence: bridge clauses at the crash point are all ORIGINAL

Instrumenting the abort site to dump every clause whose active lits span
both sides of the root partition (run on `/tmp/step1/correct_neg.cnf`):

```
BRIDGE_CLAUSE ofs=32956 kind=ORIGINAL nL=1 nR=1 nX=0 lits=1371(R/17),1634(L/0),
BRIDGE_CLAUSE ofs=32963 kind=ORIGINAL ...
BRIDGE_CLAUSE ofs=36994 kind=ORIGINAL lits=1762(L/0),-1371(R/17),
BRIDGE_CLAUSE ofs=37001 kind=ORIGINAL ...
BRIDGE_CLAUSE ofs=38163 kind=ORIGINAL lits=1796(L/0),-1404(R/17),
...
```

Every bridge is `kind=ORIGINAL` — clause learning is **not** the cause.
Example clause `ofs=32956` is a 3-lit original clause
`(1687 ∨ 1423 ∨ ¬1390)` with var leaves `(0, 17, 0)`. During search,
var 1390 is assigned, leaving the clause as an effective binary bridge
between var 1687 (leaf=0, LEFT) and var 1423 (leaf=17, RIGHT).

Lookup in the ND-hierarchy:
```
clause ofs=32956  clause_leaf=19
clause ofs=32963  clause_leaf=18
clause ofs=36994  clause_leaf=17
clause ofs=37001  clause_leaf=18
clause ofs=38163  clause_leaf=17
clause ofs=38170  clause_leaf=17
```

`clause_leaf=19` is **outside the root's [0..18] range** — orphaned.
`clause_leaf=17/18` is inside RIGHT but the clause has LEFT-leaf vars too,
meaning this clause straddles the partition and was not placed in the
root separator as it should have been. (See below for why.)

### Why clauses whose vars span L and R can end up outside root's separator

METIS at the root level produces a valid vertex separator on the bipartite
graph: removing separator vertices disconnects L and R. A clause-node whose
vars span both sides **must** be in the root's separator output. Placed
there, post-pass 2 would assign it `clause_leaf = leaf_lo[root] = 0`.

The observed `clause_leaf` values (17, 18, 19) suggest the clause-node
was **not** in the root's separator — instead it was placed in some deeper
(non-root) separator, or in an orphan subtree. This contradicts what METIS
should produce. Two possible root causes:
1. **The bipartite graph fed to METIS is incomplete** — a clause-var edge
   is missing for this clause. Unlikely to affect these specific ones,
   since [solver_rec.cpp:422-431](../src/solver_rec.cpp#L422-L431) walks
   every active long clause. Worth auditing.
2. **The recursive bisection carries vertices into subtrees where they
   don't belong** — vertex sets passed to children may include vertices
   whose adjacency crosses out of the subgraph (dropped edges), so
   later recursion can pack them into orphan leaves.

## Implications

- **Crash on `correct_neg.cnf`**: deterministic abort, prevents computing
  a count.
- **Silent miscount on `latest_correct.cnf`**: same sub-component
  composition likely arises during search but `mapToChild` returns `-2`
  less often (or the `mapToChild` check isn't reached on that code path).
  The cache hits on canonical keys stored in inconsistent ND contexts can
  silently undercount. The `2^26` undercount on `latest_correct.cnf`
  may share this root cause.
- **MC2025 track1 mismatches** (039, 053, 065, 067, 153, 155, 171, ...
  from the dense-compare monitor) are plausibly the same bug surfacing
  silently.

## Fix strategy options

1. **Make the DFS remap cover all nodes**, not just root-reachable ones.
   Assign orphan leaves new IDs appended after the root-reachable range,
   mark them with a sentinel meaning "not in any subtree", and have
   `mapToChild` return `-1` for them instead of counting them as RIGHT.

2. **Fail-fast the ND-hierarchy build** when a dead subtree is created.
   Set `valid = false` if any vertex ends up unreachable from root, and
   fall through to no-hierarchy mode (slower but sound).

3. **Fix the root cause of orphans** by making `bisect_subgraph` handle
   failures by merging vertices into the parent's separator instead of
   silently making an internal node a leaf with unclaimed sub-vertices.

Option 2 is the smallest correctness-preserving change. Option 1 is minimal
but leaves the perf loss from orphan subtrees. Option 3 is the right
long-term fix.

## Reproducer

```bash
cd /Users/konstantin.kutzkov/Desktop/Code/SharpSAT/sharpsat-separator/build
./sharpSAT -rec -sep 5 -cb 3 -sepMode metis /tmp/step1/correct_neg.cnf
# → SIGABRT with SEPARATOR_INVARIANT_VIOLATED
```

`/tmp/step1/correct_neg.cnf` is `latest_correct.cnf` (2045 vars, 10017
clauses) with appended unit clause `-1324 0`, header updated to 10018
clauses.

## Related

- [bug_investigation_t1_011.md](bug_investigation_t1_011.md) — parent
  context for the t1_011 miscount investigation that exposed this.
- The `2^26` undercount on `latest_correct.cnf` vs `latest_wrong.cnf`
  (the order-dependent bug) may be a silent manifestation of this same
  orphan-leaf construction bug.
