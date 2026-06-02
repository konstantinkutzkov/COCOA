#!/usr/bin/env bash
# Two-solver portfolio entry point.
# Usage: ./run.sh <input.cnf> [-- <extra-flags-for-chosen-solver>]
#
# Resolves the two solver binaries from env vars (with sensible defaults
# computed from this script's location) and execs the Python driver.
#
# Override binaries:
#   PORTFOLIO_SHARPSAT_BIN=/path/to/sharpSAT
#   PORTFOLIO_GANAK_BIN=/path/to/ganak       (must be the ganak-canonical fork:
#                                             vanilla ganak lacks --cachehash)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

export PORTFOLIO_SHARPSAT_BIN="${PORTFOLIO_SHARPSAT_BIN:-$REPO_ROOT/cocoa/build/sharpSAT}"
# The ganak-canonical fork serves both the identity (native) and canonical
# hash profiles; vanilla ganak/build/ganak does NOT understand --cachehash.
export PORTFOLIO_GANAK_BIN="${PORTFOLIO_GANAK_BIN:-$REPO_ROOT/ganak-canonical/build/ganak}"

if [ ! -x "$PORTFOLIO_SHARPSAT_BIN" ]; then
  echo "ERROR: sharpSAT binary not found or not executable: $PORTFOLIO_SHARPSAT_BIN" >&2
  echo "  Build it (./build.sh, or cd cocoa/build && cmake --build . --target sharpSAT) or set PORTFOLIO_SHARPSAT_BIN." >&2
  exit 2
fi
if [ ! -x "$PORTFOLIO_GANAK_BIN" ]; then
  echo "WARNING: ganak binary not found or not executable: $PORTFOLIO_GANAK_BIN" >&2
  echo "  Only sharpSAT-routed runs will work until ganak is built or PORTFOLIO_GANAK_BIN is set." >&2
fi

exec python3 "$SCRIPT_DIR/route.py" "$@"
