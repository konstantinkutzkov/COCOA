# Running tests, experiments and benchmarks

This file is the **single source of truth** for: where the test instances
live, how to build the solver, how to run the calibration regression
check, and how to write ad-hoc experiments without falling into the shell
and path traps that have wasted hours.

If you are about to do anything timing-related, **read at least sections
1-3 first**. If you are writing a shell harness, **also read section 6**.


## 1. Repo layout

```
~/Desktop/Code/                            # parent dir
├── SharpSAT/
│   ├── sharpsat-separator/               # THIS REPO
│   │   ├── build/sharpSAT                # the solver binary (Release build)
│   │   ├── src/                          # solver source
│   │   ├── scripts/calibration_run.py    # regression test harness
│   │   ├── tests/                        # unit/regression test executables
│   │   └── docs/                         # this folder
│   └── temp_cnf/                         # ⭐ MC2025 CNF instances live HERE
└── ganak/                                 # external comparison solver
    └── build/ganak                       # comparison binary
```

**Common gotcha:** the CNFs are at `SharpSAT/temp_cnf/`, NOT at
`Code/temp_cnf/`. Both paths exist sometimes (the latter from earlier
experiments) — always use `~/Desktop/Code/SharpSAT/temp_cnf/`.


## 2. Building

The solver is a CMake project. Release is the only configuration whose
timings are meaningful.

```bash
cd ~/Desktop/Code/SharpSAT/sharpsat-separator
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build                       # builds sharpSAT + tests + metis_replay
cmake --build build --target sharpSAT     # just the solver
```

If you suspect a stale build (e.g. CMakeLists changed but a target
didn't pick it up):

```bash
rm build/src/liblibsharpSAT.a            # force re-archive
cmake --build build --target sharpSAT
```

Hard reset (last resort):

```bash
rm -rf build && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```


## 3. The regression check (use this first, always)

`scripts/calibration_run.py` is the canonical way to verify the solver
isn't broken. Run it after every code change before any experiment.

```bash
cd ~/Desktop/Code/SharpSAT/sharpsat-separator
python3 scripts/calibration_run.py
```

It runs 5 representative instances (~30s total wall on an idle Mac),
verifies counts match the recorded expected values, and reports
PASS / FAIL per case. Exits 0 if all pass, 1 if any fail.

The manifest lives at the top of the script. To update expected wall
times after an intentional perf improvement:

```bash
python3 scripts/calibration_run.py --bootstrap
# copy the printed expected_wall_s values back into the manifest
```

**Why use this instead of running by hand:** it uses
`subprocess.run([list, of, args])` which bypasses the shell entirely —
no word-splitting traps, no quoting issues, no stream redirection
surprises. If you are running a single-config sanity check, just call
this script.


## 4. Manual single-instance invocations

Standard production flag set (default solver behaviour, used by 3 of 5
calibration cases):

```bash
build/sharpSAT -rec -sep 5 -cb 3 -sepMode metis \
    ~/Desktop/Code/SharpSAT/temp_cnf/mc2025_track1_071.cnf
```

Common variations:

| Flag set | When to use |
|---|---|
| `-rec -sep 5 -cb 3 -sepMode metis` | default production |
| `… -sepClausesFirst` | small density-1 instances (e.g. t1_021_k10_s1) |
| `… -wlIter 2 -reactiveMetis -reactiveMetisMin 10 -reactiveMetisSkip 4` | large density-1 (t1_041 et al.) |
| `… -adaptive` | Phase-3 τ-based branching on the no-separator path |
| `… -unifiedPicker` | unified-picker fallback (research) |
| `… -arjunLight` | CMS5-based preprocessing pass |
| `-noPP` | disable preprocessing entirely (debug) |
| `-q` | suppress per-step output |
| `-t <sec>` | internal time bound (solver self-terminates) |

The full flag list is in `src/main.cpp`'s `argc <= 1` help block.


## 5. The MC2025 instances

The MC2025 Track-1 instances are at
`~/Desktop/Code/SharpSAT/temp_cnf/mc2025_track1_<NNN>.cnf`. Available
out of the box:

```bash
ls ~/Desktop/Code/SharpSAT/temp_cnf/mc2025_track1_*.cnf
```

Shrunken variants (frozen variables, made smaller for faster
debugging) follow the pattern
`mc2025_track1_<NNN>_k<N>_s<S>.cnf` — e.g.
`mc2025_track1_021_k10_s1.cnf` (instance 21, freezer k=10, seed=1).

The 5 instances in the calibration manifest cover the structural
regimes we care about:

| Case | Instance | Wall | Notes |
|---|---|---|---|
| t1_065_plain | 065 | ~0.02s | sparse uniform-5 CNF |
| t1_071_plain | 071 | ~0.4s  | sparse 3-SAT |
| t1_011_plain | 011 | ~14s   | sparse, larger |
| t1_021_k10_s1_clausesFirst | 021_k10_s1 | ~4s | density-1 with `-sepClausesFirst` |
| t1_041_react_agg_no_anchor | 041 | ~10s | density-1 + reactive METIS |


## 6. Writing ad-hoc shell experiments — pitfalls

If you write a shell loop that runs sharpSAT with different flag sets,
**zsh and bash behave differently** in ways that have wasted hours.

### 6.1 zsh does NOT word-split unquoted variable expansion

This is the #1 trap. macOS default shell is **zsh**. In zsh:

```sh
flags="-rec -sep 5 -cb 3 -sepMode metis"
build/sharpSAT $flags some.cnf
# zsh:   passes ONE arg "-rec -sep 5 -cb 3 -sepMode metis" → solver gets no flags
# bash:  passes 7 args                                     → solver gets all flags
```

When the solver gets no flags, it runs in default mode (no separator
branching, no clause branching), which on most MC2025 instances
explodes into many decisions and looks like a TIMEOUT in your harness
— a very misleading symptom because the calibration script (which uses
Python and avoids this trap) works fine.

**Three fixes that work in zsh:**

```sh
# Option A: ${=var} forces splitting (zsh-only, terse)
build/sharpSAT ${=flags} some.cnf

# Option B: build an array (portable across zsh/bash)
flag_array=( -rec -sep 5 -cb 3 -sepMode metis )
build/sharpSAT "${flag_array[@]}" some.cnf

# Option C: enable POSIX word-splitting once (zsh-only, set globally)
setopt sh_word_split
build/sharpSAT $flags some.cnf
```

**Best fix:** just write the experiment in Python (use `subprocess.run`
with a list of args). No shell, no traps.

### 6.2 `time:` is stdout; diagnostic stats are stderr

When parsing wall time, remember:

- `time: <seconds>s` → printed to **stdout** at end of solve
- `OPEN_WORK …`, `FULL_CACHE_STATS …`, `MIDSEP_STATS …`, `DIAG_STATS …`,
  `preprocess: …`, `NDHierarchy: …` → printed to **stderr** during solve
- `# solutions\n<COUNT>\n# END` → **stdout**

If your harness merges streams with `2>&1` and then greps for
`^time:`, it works. If you only capture stdout, you also get `time:`
and `# solutions`, but you miss the stats. If you only capture stderr,
you get the stats but no time/count.

### 6.3 Capturing output with `$(timeout … 2>&1)` can hang under load

Under heavy system load (Spotlight reindex, Defender scan, etc.) on
macOS, `out=$(timeout N solver flags cnf 2>&1)` has been observed to
hang for the full timeout window even on instances that finish in
milliseconds. The exact root cause is unclear (suspected: a pipe-buffer
interaction with `timeout`'s SIGTERM-on-deadline). Symptom: exit code
124 (timeout-killed) with partial output captured.

**Diagnosis sequence:**

1. Run the bare invocation manually:
   `time build/sharpSAT <flags> <cnf>`. If this is fast,
   the solver is fine.
2. Check load: `uptime` and `ps -A -o pcpu,comm | sort -rn | head -5`.
3. If load is high (>5 on a 14-core M-series), the culprit is usually
   Spotlight indexing your venvs / node_modules — see section 7.


## 7. macOS Spotlight thrash on developer machines

If `uptime` shows load avg consistently >5 with `mds` and
`mdworker_shared` near the top of CPU usage, Spotlight is grinding
through directories with thousands of small files (Python venvs, npm
`node_modules`, `__pycache__`). These have zero search value and should
be excluded.

**One-time fix (current dirs only):**

```bash
find ~/Desktop/Code -type d \
    \( -name venv -o -name .venv -o -name node_modules -o -name __pycache__ \) \
    -exec touch {}/.metadata_never_index \;
```

The `.metadata_never_index` marker file tells Spotlight to skip that
directory tree. It survives reboots and OS updates. Re-run the find
periodically when new projects appear.

**Immediate kill switch (until you re-enable):**

```bash
sudo mdutil -a -i off    # pause indexing on all volumes
# ... do your benchmark ...
sudo mdutil -a -i on     # resume
```


## 8. The picker-comparison experiment protocol (worked example)

Demonstrates how to write an experiment correctly. Compares the
default solver against the `-sepClausesFirst` reorder and the
unified-picker fallback:

```bash
cd ~/Desktop/Code/SharpSAT/sharpsat-separator
SOLVER=build/sharpSAT
TEMP=~/Desktop/Code/SharpSAT/temp_cnf

declare -a cases=(
  "t1_065_plain|mc2025_track1_065.cnf|-rec -sep 5 -cb 3 -sepMode metis|37778931862957161709568|30"
  "t1_071_plain|mc2025_track1_071.cnf|-rec -sep 5 -cb 3 -sepMode metis|456…640|60"
)

run_one() {
  local cnf="$1" flags="$2" timeout_s="$3" expected="$4"
  local out rc
  # ⭐ ${=flags} is required for zsh — see section 6.1
  out=$(timeout "$timeout_s" "$SOLVER" ${=flags} "$cnf" 2>&1)
  rc=$?
  local count=$(echo "$out" | awk '/^# solutions/{getline; print}')
  local wall=$(echo "$out" | grep -E '^time:' | head -1 \
                          | awk '{print $2}' | tr -d 's')
  if [ "$rc" -eq 124 ]; then wall="TIMEOUT"; fi
  if [ -z "$wall" ]; then wall="NO_TIME"; fi
  local sound="ok"
  if [ "$wall" != "TIMEOUT" ] && [ "$wall" != "NO_TIME" ]; then
    [ "$count" != "$expected" ] && sound="MISMATCH"
  fi
  printf "%9s/%-8s" "$wall" "$sound"
}

for line in "${cases[@]}"; do
  IFS='|' read -r name cnf base_flags expected timeout_s <<< "$line"
  cnf_path="$TEMP/$cnf"
  r_base=$(run_one "$cnf_path" "$base_flags" "$timeout_s" "$expected")
  r_clf=$( run_one "$cnf_path" \
        "$base_flags -sepClausesFirst" \
        "$timeout_s" "$expected")
  r_uni=$( run_one "$cnf_path" \
        "$base_flags -unifiedPicker" \
        "$timeout_s" "$expected")
  printf "%-26s  %s  %s  %s\n" "$name" "$r_base" "$r_clf" "$r_uni"
done
```

Preflight checklist before running ANY experiment:

1. `pmset -g batt | head -2` — confirm **AC power** (Apple Silicon
   throttles ~60% on battery).
2. `uptime` — confirm load avg <3 on a 14-core Mac.
3. `python3 scripts/calibration_run.py` — confirm the baseline still
   passes after any recent code changes.

If load is high, see section 7. If on battery, plug in and wait 30
seconds for the SoC to ramp.


## 9. Build artefacts for the test suite

`tests/` contains regression-test executables, all built by
`cmake --build build`:

| Executable | Tests |
|---|---|
| `test_probe` | Phase-1 probe primitives |
| `test_canonical_key_invariance` | canonical key is invariant under stored-literal permutation |
| `test_canonical_key_learned` | canonical key handles injected learned clauses |
| `test_probe_preprocessor_*` | probe-preprocessor passes |

Each takes a CNF and an expected file (or just a CNF for the
canonical-key ones). They are not yet wired into a `ctest` driver
— invoke manually. The canonical-key tests use the underscored
`Solver::_permuteClauseLiteralsForTest` and related test helpers in
`src/solver_diagnostics.cpp`.
