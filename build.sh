#!/usr/bin/env bash
# COCOA package superbuild.
#
# Builds the components in dependency order:
#   1. third_party/GKlib   (static libGKlib.a)        — METIS dependency
#   2. third_party/METIS   (static libmetis.a)        — separator engine
#   3. ganak-canonical     (ganak binary + STATIC SAT deps under build/_deps)
#   4. cocoa               (the sharpSAT binary; links METIS/GKlib + ganak's
#                           static cryptominisat5/sbva/cadical/cadiback)
#
# NOTE: step 3 uses CMake FetchContent to download its SAT deps
# (cryptominisat, cadical, sbva, approxmc, arjun, treedecomp) on the FIRST
# build — so the first build needs network access. Building it with
# BUILD_SHARED_LIBS=OFF makes those deps STATIC (.a), which cocoa links.
#
# Usage:  ./build.sh [-j N]      (default: all cores)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
if [ "${1:-}" = "-j" ]; then JOBS="$2"; fi

say() { printf '\n=== %s ===\n' "$*"; }

# ---- 0) Build dependencies (best-effort) -----------------------------------
# COCOA builds entirely from source, but needs a C/C++ toolchain plus a few
# -dev libraries.  Required packages (Ubuntu names; from ganak's flake.nix
# buildInputs = cmake, pkg-config, gmp, mpfr, zlib, python3+numpy):
#     build-essential cmake make git pkg-config python3 python3-numpy
#     libgmp-dev libmpfr-dev zlib1g-dev
# On the MC competition image these are normally preinstalled.  If apt-get is
# available AND we are root, we install them; guarded so a missing apt / no
# network / non-root never aborts the build (we then assume they are present).
# NOTE: step 3 (ganak) uses CMake FetchContent to download its SAT deps on the
# FIRST build, so build.sh needs network access during that step.
ensure_deps() {
  command -v apt-get >/dev/null 2>&1 || return 0
  [ "$(id -u 2>/dev/null)" = "0" ] || return 0
  say "0/4 ensuring build dependencies (best-effort apt-get)"
  apt-get update -y || return 0
  DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake make git pkg-config python3 python3-numpy \
      libgmp-dev libmpfr-dev zlib1g-dev || true
}
ensure_deps

# 1) GKlib — build only the library target (its example apps/ use x86 asm).
say "1/4 GKlib"
cmake -S "$ROOT/third_party/GKlib" -B "$ROOT/third_party/GKlib/build" \
      -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/third_party/GKlib/build" --target GKlib -j"$JOBS"

# 2) METIS — its Makefile 'config' proxy generates build/xinclude/metis.h,
#    then we (re)configure the build dir as a STATIC lib and build it.
say "2/4 METIS"
( cd "$ROOT/third_party/METIS"
  make config gklib_path="$ROOT/third_party/GKlib" cc=cc shared=0 >/dev/null
  cmake "$ROOT/third_party/METIS/build" -DSHARED=OFF -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$ROOT/third_party/METIS/build" --target metis -j"$JOBS" )

# ---- Fully static binaries on Linux ----------------------------------------
# HPC/competition machines often lack the shared GMP/MPFR/etc. at runtime
# (e.g. "libgmpxx.so.4: cannot open shared object file"), so on Linux we link
# the math + C++ runtime STATICALLY.
#   ganak: -DSTATIC_BINARY=ON  (its CMakeLists adds -static on Linux only) and
#          -DGANAK_WITH_FLINT=OFF (FLINT is used only by the unused polynomial
#          mode 3, and Ubuntu ships no libflint.a — dropping it makes the binary
#          fully static; gmp/gmpxx/mpfr all have .a).
#   cocoa: link static libgmp/libgmpxx + -static.
# On macOS, -static is unsupported, so the cocoa flags stay empty and ganak's
# -static is already gated to Linux inside its own CMakeLists.
GANAK_STATIC_FLAGS="-DSTATIC_BINARY=ON -DGANAK_WITH_FLINT=OFF"
COCOA_STATIC_FLAGS=""
if [ "$(uname -s)" = "Linux" ]; then
  GMP_A="$(cc -print-file-name=libgmp.a 2>/dev/null)";    case "$GMP_A"   in /*) ;; *) GMP_A="$(find /usr/lib /usr/lib64 -name libgmp.a   2>/dev/null | head -1)";;   esac
  GMPXX_A="$(cc -print-file-name=libgmpxx.a 2>/dev/null)"; case "$GMPXX_A" in /*) ;; *) GMPXX_A="$(find /usr/lib /usr/lib64 -name libgmpxx.a 2>/dev/null | head -1)";; esac
  COCOA_STATIC_FLAGS="-DCMAKE_EXE_LINKER_FLAGS=-static"
  [ -n "$GMP_A" ]   && COCOA_STATIC_FLAGS="$COCOA_STATIC_FLAGS -DGMP_LIB=$GMP_A"
  [ -n "$GMPXX_A" ] && COCOA_STATIC_FLAGS="$COCOA_STATIC_FLAGS -DGMPXX_LIB=$GMPXX_A"
fi

# 3) ganak-canonical — STATIC libraries (so cocoa can link cryptominisat5/sbva)
#    AND a fully-static ganak binary (no runtime .so deps).  First build
#    downloads SAT deps via FetchContent (needs network).
say "3/4 ganak-canonical (first build fetches SAT deps — needs network)"
cmake -S "$ROOT/ganak-canonical" -B "$ROOT/ganak-canonical/build" \
      -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release $GANAK_STATIC_FLAGS
cmake --build "$ROOT/ganak-canonical/build" -j"$JOBS"

# 4) cocoa — links METIS/GKlib (third_party) + ganak-canonical's static CMS deps.
#    Also builds metis_features (the separator-size probe the portfolio router uses).
say "4/4 cocoa (sharpSAT + metis_features)"
# Portability shim: newer libstdc++ (gcc 13+) does not transitively pull in the
# standard headers that Apple libc++ does, so some TUs reference size_t / uint*_t
# / std::string without an explicit include.  Force-include the standard headers
# for every cocoa TU so the build is portable across toolchains.
COCOA_PORTABILITY_FLAGS="-include cstddef -include cstdint -include cstring -include string"
cmake -S "$ROOT/cocoa" -B "$ROOT/cocoa/build" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="$COCOA_PORTABILITY_FLAGS" $COCOA_STATIC_FLAGS
cmake --build "$ROOT/cocoa/build" --target sharpSAT metis_features -j"$JOBS"

say "Build complete"
echo "  cocoa solver : $ROOT/cocoa/build/sharpSAT"
echo "  ganak        : $ROOT/ganak-canonical/build/ganak"
echo "  portfolio    : python3 $ROOT/portfolio/ab_compare.py -t 30 <cnfs...>"
echo "                 (or $ROOT/portfolio/run.sh <cnf> to auto-route)"