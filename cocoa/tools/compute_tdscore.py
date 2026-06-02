#!/usr/bin/env python3
"""compute_tdscore.py — port of ganak's compute_td_score_using_raw.

Pipeline:
  CNF  -->  primal graph (clique encoding for long clauses)
       -->  PACE .gr file
       -->  flow_cutter_pace17 (anytime, killed by `timeout`)
       -->  PACE .td file (bags + tree edges)
       -->  centroid bag of bag tree
       -->  BFS distance in bag tree from centroid to every bag
       -->  per-var distance = min over bags containing v
       -->  tdscore[v] = 100 * (max_dist - dist[v]) / max_dist
       -->  one float per line, 1-indexed to DIMACS var id

Vars absent from every bag (free vars) and vars not in the formula get 0.

Usage:
  compute_tdscore.py <input.cnf> <output.scores> [--timeout 30]
                     [--fc-binary PATH] [--keep-tmp]
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from collections import deque


def parse_cnf(path):
    n_vars = 0
    clauses = []
    with open(path) as f:
        for line in f:
            s = line.strip()
            if not s or s[0] == "c":
                continue
            if s[0] == "p":
                parts = s.split()
                if len(parts) >= 4 and parts[1] == "cnf":
                    n_vars = int(parts[2])
                continue
            lits = []
            for tok in s.split():
                v = int(tok)
                if v == 0:
                    break
                lits.append(v)
            if lits:
                clauses.append(lits)
    return n_vars, clauses


def primal_edges(n_vars, clauses):
    edges = set()
    for cl in clauses:
        vs = sorted({abs(l) for l in cl})
        if len(vs) <= 1:
            continue
        for i in range(len(vs)):
            for j in range(i + 1, len(vs)):
                edges.add((vs[i], vs[j]))
    return edges


def write_gr(path, n_vars, edges):
    with open(path, "w") as f:
        f.write(f"p tw {n_vars} {len(edges)}\n")
        for u, v in edges:
            f.write(f"{u} {v}\n")


def run_flow_cutter(fc_binary, gr_path, td_path, timeout_s):
    cmd = f"timeout {timeout_s}s {fc_binary} < {gr_path} > {td_path} 2>/dev/null"
    rc = subprocess.call(cmd, shell=True)
    if not os.path.exists(td_path) or os.path.getsize(td_path) == 0:
        raise RuntimeError(
            f"flow_cutter_pace17 produced no output (rc={rc}). "
            f"Check binary path: {fc_binary}"
        )
    return rc


def parse_td(path):
    """Return (n_bags, bags_dict[bag_id]=list_of_vars, tree_edges_list)."""
    n_bags = 0
    bags = {}
    edges = []
    with open(path) as f:
        for line in f:
            s = line.strip()
            if not s or s[0] == "c":
                continue
            tok = s.split()
            if tok[0] == "s":
                assert tok[1] == "td"
                n_bags = int(tok[2])
            elif tok[0] == "b":
                bid = int(tok[1])
                bags[bid] = [int(x) for x in tok[2:]]
            else:
                a, b = int(tok[0]), int(tok[1])
                edges.append((a, b))
    return n_bags, bags, edges


def find_centroid(n_bags, edges):
    """Standard tree-centroid algorithm. Iterative DFS, no recursion limit risk."""
    if n_bags == 0:
        return None
    adj = {i: [] for i in range(1, n_bags + 1)}
    for a, b in edges:
        adj[a].append(b)
        adj[b].append(a)

    root = 1
    parent = {root: -1}
    order = []
    stack = [root]
    visited = {root}
    while stack:
        u = stack.pop()
        order.append(u)
        for v in adj[u]:
            if v not in visited:
                visited.add(v)
                parent[v] = u
                stack.append(v)

    size = {u: 1 for u in adj}
    for u in reversed(order):
        p = parent[u]
        if p != -1:
            size[p] += size[u]

    total = n_bags
    cur = root
    while True:
        moved = False
        for v in adj[cur]:
            if v != parent.get(cur, -1) and size[v] > total // 2:
                parent[cur] = v
                size[cur] = total - size[v]
                size[v] = total
                cur = v
                moved = True
                break
        if not moved:
            return cur


def bfs_dist_from(n_bags, edges, source):
    adj = {i: [] for i in range(1, n_bags + 1)}
    for a, b in edges:
        adj[a].append(b)
        adj[b].append(a)
    dist = {source: 0}
    q = deque([source])
    while q:
        u = q.popleft()
        for v in adj[u]:
            if v not in dist:
                dist[v] = dist[u] + 1
                q.append(v)
    return dist


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input_cnf")
    ap.add_argument("output_scores")
    ap.add_argument("--timeout", type=int, default=30,
                    help="seconds to let flow_cutter_pace17 run (default 30)")
    ap.add_argument(
        "--fc-binary",
        default="/Users/konstantin.kutzkov/Desktop/Code/SharpSAT/sharpsat-td/bin/flow_cutter_pace17",
    )
    ap.add_argument("--keep-tmp", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.fc_binary):
        print(f"ERROR: flow_cutter binary not found at {args.fc_binary}", file=sys.stderr)
        sys.exit(2)

    n_vars, clauses = parse_cnf(args.input_cnf)
    print(f"[tdscore] CNF: n_vars={n_vars} n_clauses={len(clauses)}", file=sys.stderr)

    edges = primal_edges(n_vars, clauses)
    print(f"[tdscore] primal graph: {len(edges)} edges", file=sys.stderr)

    tmpdir = tempfile.mkdtemp(prefix="tdscore_")
    try:
        gr_path = os.path.join(tmpdir, "primal.gr")
        td_path = os.path.join(tmpdir, "primal.td")
        write_gr(gr_path, n_vars, edges)
        print(f"[tdscore] running flow_cutter_pace17 for {args.timeout}s...", file=sys.stderr)
        run_flow_cutter(args.fc_binary, gr_path, td_path, args.timeout)

        n_bags, bags, tree_edges = parse_td(td_path)
        max_bag = max((len(b) for b in bags.values()), default=0)
        print(f"[tdscore] TD: {n_bags} bags, max_bag_size={max_bag}, tw={max_bag - 1}",
              file=sys.stderr)

        if n_bags == 0:
            print("[tdscore] empty TD; writing all-zero scores", file=sys.stderr)
            dist_v = {v: 0 for v in range(1, n_vars + 1)}
            max_dist = 0
        else:
            centroid = find_centroid(n_bags, tree_edges)
            print(f"[tdscore] centroid bag id={centroid} size={len(bags[centroid])}",
                  file=sys.stderr)

            bag_dist = bfs_dist_from(n_bags, tree_edges, centroid)
            print(f"[tdscore] BFS reached {len(bag_dist)}/{n_bags} bags", file=sys.stderr)

            dist_v = {}
            for bid, bs in bags.items():
                d = bag_dist.get(bid, float("inf"))
                for v in bs:
                    if 1 <= v <= n_vars:
                        if v not in dist_v or d < dist_v[v]:
                            dist_v[v] = d
            for v in range(1, n_vars + 1):
                dist_v.setdefault(v, 0)
            max_dist = max((d for d in dist_v.values() if d != float("inf")), default=0)
            print(f"[tdscore] max_dist={max_dist}", file=sys.stderr)

        if max_dist == 0:
            scores = [0.0] * (n_vars + 1)
        else:
            scores = [0.0] * (n_vars + 1)
            for v in range(1, n_vars + 1):
                d = dist_v.get(v, max_dist)
                if d == float("inf"):
                    d = max_dist
                scores[v] = 100.0 * (max_dist - d) / max_dist

        with open(args.output_scores, "w") as f:
            for v in range(1, n_vars + 1):
                f.write(f"{scores[v]:.6f}\n")
        nz = sum(1 for s in scores[1:] if s > 0)
        n_at_100 = sum(1 for s in scores[1:] if s >= 99.999)
        print(f"[tdscore] wrote {n_vars} scores: {nz} positive, {n_at_100} at 100",
              file=sys.stderr)
    finally:
        if not args.keep_tmp:
            shutil.rmtree(tmpdir, ignore_errors=True)
        else:
            print(f"[tdscore] kept tmpdir {tmpdir}", file=sys.stderr)


if __name__ == "__main__":
    main()
