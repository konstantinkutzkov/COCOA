cmake_minimum_required (VERSION 3.5)
project (SHARPSAT_SEPARATOR)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -std=c++11 -Wall")

# Release flags. The three additions over plain -O3:
#   -fno-stack-protector: skip stack-canary insertion. Safe for a
#       research-only CLI (no untrusted attack surface). 1-3% on hot
#       functions with stack arrays. Confirmed used by ganak.
#   -D_FORTIFY_SOURCE=0: disables glibc bounds-checking on str/mem
#       functions. No-op on macOS (libSystem doesn't use FORTIFY) but
#       harmless. Confirmed used by ganak.
#   -mcpu=native (only when CMAKE_SYSTEM_PROCESSOR is arm64): allow the
#       compiler to use the full Apple Silicon instruction set on the
#       build machine. Sacrifices binary portability for ~1-5% on
#       inner loops; acceptable since this is a research solver and
#       we don't redistribute binaries.
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -std=c++11 -O3 -DNDEBUG -Wall -fno-stack-protector -D_FORTIFY_SOURCE=0")
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -mcpu=native")
endif()

set(CMAKE_CXX_FLAGS_PROFILING "${CMAKE_CXX_FLAGS_PROFILING} -std=c++11 -O3 -g -DNDEBUG -Wall")

IF(UNIX)
  IF(APPLE)
    include_directories(/opt/local/include)
    include_directories(/opt/homebrew/opt/gmp/include)
    link_directories(/opt/homebrew/opt/gmp/lib)
  ELSE(APPLE)


  ENDIF(APPLE)
ELSE(UNIX)

ENDIF(UNIX)


find_library(GMP_LIB gmp)
find_library(GMPXX_LIB gmpxx)

# METIS graph partitioning library (for separator finding)
set(METIS_DIR "${CMAKE_SOURCE_DIR}/../METIS" CACHE PATH "Path to METIS source")
set(GKLIB_DIR "${CMAKE_SOURCE_DIR}/../GKlib" CACHE PATH "Path to GKlib source")
include_directories(${METIS_DIR}/include ${GKLIB_DIR}/include)
add_definitions(-DIDXTYPEWIDTH=32 -DREALTYPEWIDTH=32)

# CryptoMiniSat5 — used by preprocessor_light.cpp for count-preserving
# CNF simplification (Arjun-light). Points at the static lib already
# built under ganak's _deps tree to avoid a second build.
set(CMS5_DIR "${CMAKE_SOURCE_DIR}/../ganak/build/_deps/cryptominisat5-src/src"
    CACHE PATH "Path to CryptoMiniSat5 headers")
set(CMS5_BUILD_INCLUDE "${CMAKE_SOURCE_DIR}/../ganak/build/_deps/cryptominisat5-build/include"
    CACHE PATH "Path to CryptoMiniSat5 build-time headers (cryptominisat5/...) — required by Arjun")
set(CMS5_LIB "${CMAKE_SOURCE_DIR}/../ganak/build/_deps/cryptominisat5-build/lib/libcryptominisat5.a"
    CACHE FILEPATH "Path to CryptoMiniSat5 static library")
set(CADICAL_LIB "${CMAKE_SOURCE_DIR}/../ganak/build/_deps/cadical-build/libcadical.a"
    CACHE FILEPATH "Path to Cadical static library")
set(CADIBACK_LIB "${CMAKE_SOURCE_DIR}/../ganak/build/_deps/cadiback-build/libcadiback.a"
    CACHE FILEPATH "Path to Cadiback static library")
set(SBVA_LIB "${CMAKE_SOURCE_DIR}/../ganak/build/_deps/sbva-build/lib/libsbva.a"
    CACHE FILEPATH "Path to SBVA static library")
set(CMS5_DEPS ${CMS5_LIB} ${CADIBACK_LIB} ${CADICAL_LIB} ${SBVA_LIB})

# Arjun — used by preprocessor_arjun.cpp (Phase D) for independent-support
# extraction + count-multiplier-preserving simplification. Points at the
# static lib already built under ganak's _deps tree (same pattern as CMS5).
set(ARJUN_DIR "${CMAKE_SOURCE_DIR}/../ganak/build/_deps/arjun-build/include"
    CACHE PATH "Path to Arjun headers (arjun/arjun.h)")
set(ARJUN_LIB "${CMAKE_SOURCE_DIR}/../ganak/build/_deps/arjun-build/lib/libarjun.a"
    CACHE FILEPATH "Path to Arjun static library")
set(APPROXMC_LIB "${CMAKE_SOURCE_DIR}/../ganak/build/_deps/approxmc-build/lib/libapproxmc.a"
    CACHE FILEPATH "Path to ApproxMC static library (Arjun dependency)")
set(TREEDECOMP_LIB "${CMAKE_SOURCE_DIR}/../ganak/build/_deps/treedecomp-build/lib/libtreedecomp.a"
    CACHE FILEPATH "Path to treedecomp static library (Arjun dependency)")
# Arjun also pulls MPFR (for its Field types). Provided by Homebrew on macOS.
find_library(MPFR_LIB mpfr)
set(ARJUN_DEPS ${ARJUN_LIB} ${APPROXMC_LIB} ${TREEDECOMP_LIB} ${MPFR_LIB})

# CMS5_DIR is scoped to libsharpSAT only (where preprocessor_light.cpp
# consumes <cryptominisat.h>). Don't put it in the project-wide include
# path — CMS5 ships its own solver.h that would otherwise shadow ours
# whenever a test executable does #include "solver.h" with quoted form.

add_subdirectory(src)

add_executable (sharpSAT src/main.cpp)
target_link_libraries (sharpSAT libsharpSAT ${GMP_LIB} ${GMPXX_LIB}
    ${METIS_DIR}/build/libmetis/libmetis.a
    ${GKLIB_DIR}/build/libGKlib.a
    ${CMS5_DEPS}
    ${ARJUN_DEPS})

add_executable (test_probe tests/test_probe.cpp)
target_include_directories (test_probe PRIVATE src)
target_link_libraries (test_probe libsharpSAT ${GMP_LIB} ${GMPXX_LIB}
    ${METIS_DIR}/build/libmetis/libmetis.a
    ${GKLIB_DIR}/build/libGKlib.a
    ${CMS5_DEPS})

add_executable (test_canonical_key_invariance tests/test_canonical_key_invariance.cpp)
target_include_directories (test_canonical_key_invariance PRIVATE src)
target_link_libraries (test_canonical_key_invariance libsharpSAT ${GMP_LIB} ${GMPXX_LIB}
    ${METIS_DIR}/build/libmetis/libmetis.a
    ${GKLIB_DIR}/build/libGKlib.a
    ${CMS5_DEPS})

add_executable (test_canonical_key_free_var tests/test_canonical_key_free_var.cpp)
target_include_directories (test_canonical_key_free_var PRIVATE src)
target_link_libraries (test_canonical_key_free_var libsharpSAT ${GMP_LIB} ${GMPXX_LIB}
    ${METIS_DIR}/build/libmetis/libmetis.a
    ${GKLIB_DIR}/build/libGKlib.a
    ${CMS5_DEPS})

add_executable (test_canonical_key_free_var_midsearch tests/test_canonical_key_free_var_midsearch.cpp)
target_include_directories (test_canonical_key_free_var_midsearch PRIVATE src)
target_link_libraries (test_canonical_key_free_var_midsearch libsharpSAT ${GMP_LIB} ${GMPXX_LIB}
    ${METIS_DIR}/build/libmetis/libmetis.a
    ${GKLIB_DIR}/build/libGKlib.a
    ${CMS5_DEPS})

add_executable (test_canonical_key_learned tests/test_canonical_key_learned.cpp)
target_include_directories (test_canonical_key_learned PRIVATE src)
target_link_libraries (test_canonical_key_learned libsharpSAT ${GMP_LIB} ${GMPXX_LIB}
    ${METIS_DIR}/build/libmetis/libmetis.a
    ${GKLIB_DIR}/build/libGKlib.a
    ${CMS5_DEPS})

add_executable (test_probe_preprocessor_harness tests/test_probe_preprocessor_harness.cpp)
target_include_directories (test_probe_preprocessor_harness PRIVATE src)
target_link_libraries (test_probe_preprocessor_harness libsharpSAT ${GMP_LIB} ${GMPXX_LIB}
    ${METIS_DIR}/build/libmetis/libmetis.a
    ${GKLIB_DIR}/build/libGKlib.a
    ${CMS5_DEPS})

add_executable (test_probe_preprocessor_lift tests/test_probe_preprocessor_lift.cpp)
target_include_directories (test_probe_preprocessor_lift PRIVATE src)
target_link_libraries (test_probe_preprocessor_lift libsharpSAT ${GMP_LIB} ${GMPXX_LIB}
    ${METIS_DIR}/build/libmetis/libmetis.a
    ${GKLIB_DIR}/build/libGKlib.a
    ${CMS5_DEPS})

add_executable (test_probe_preprocessor_r4 tests/test_probe_preprocessor_r4.cpp)
target_include_directories (test_probe_preprocessor_r4 PRIVATE src)
target_link_libraries (test_probe_preprocessor_r4 libsharpSAT ${GMP_LIB} ${GMPXX_LIB}
    ${METIS_DIR}/build/libmetis/libmetis.a
    ${GKLIB_DIR}/build/libGKlib.a
    ${CMS5_DEPS})

add_executable (test_probe_preprocessor_filter tests/test_probe_preprocessor_filter.cpp)
target_include_directories (test_probe_preprocessor_filter PRIVATE src)
target_link_libraries (test_probe_preprocessor_filter libsharpSAT ${GMP_LIB} ${GMPXX_LIB}
    ${METIS_DIR}/build/libmetis/libmetis.a
    ${GKLIB_DIR}/build/libGKlib.a
    ${CMS5_DEPS})

# SCC-based 2-SAT UNSAT detector — standalone module with no
# Solver/METIS/CMS5 dependencies. Compile scc_unsat.cpp directly into
# the test exe rather than linking libsharpSAT so the unit test
# exercises the module in true isolation.
add_executable (test_scc_unsat tests/test_scc_unsat.cpp src/scc_unsat.cpp)
target_include_directories (test_scc_unsat PRIVATE src)

# Integration test for the solver-side bridge (Solver::sccCheckComponentUnsat).
# Links libsharpSAT — same pattern as test_canonical_key_*.
add_executable (test_scc_unsat_solver tests/test_scc_unsat_solver.cpp)
target_include_directories (test_scc_unsat_solver PRIVATE src)
target_link_libraries (test_scc_unsat_solver libsharpSAT ${GMP_LIB} ${GMPXX_LIB}
    ${METIS_DIR}/build/libmetis/libmetis.a
    ${GKLIB_DIR}/build/libGKlib.a
    ${CMS5_DEPS})

# Regression test for the 2026-05-27 fix: SCC bridge must filter
# removed_clauses_ to keep its verdict invariant under canonical_key.
# Without the fix this test would return UNSAT on a residual that is
# actually SAT (see file header for full bug history).
add_executable (test_scc_removed_clauses tests/test_scc_removed_clauses.cpp)
target_include_directories (test_scc_removed_clauses PRIVATE src)
target_link_libraries (test_scc_removed_clauses libsharpSAT ${GMP_LIB} ${GMPXX_LIB}
    ${METIS_DIR}/build/libmetis/libmetis.a
    ${GKLIB_DIR}/build/libGKlib.a
    ${CMS5_DEPS})

# Standalone METIS replay tool — reads a dump produced by sharpSAT's
# -dumpReactiveMetisInputs flag and replays each input through METIS to
# characterise failure modes. See tools/metis_replay.cpp.
add_executable (metis_replay tools/metis_replay.cpp)
target_link_libraries (metis_replay
    ${METIS_DIR}/build/libmetis/libmetis.a
    ${GKLIB_DIR}/build/libGKlib.a
    ${CMS5_DEPS})
