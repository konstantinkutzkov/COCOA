cmake_minimum_required (VERSION 3.5)
project (SHARPSAT_SEPARATOR)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -std=c++11 -Wall")

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -std=c++11 -O3 -DNDEBUG -Wall")

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

add_subdirectory(src)

add_executable (sharpSAT src/main.cpp)
target_link_libraries (sharpSAT libsharpSAT ${GMP_LIB} ${GMPXX_LIB}
    ${METIS_DIR}/build/libmetis/libmetis.a
    ${GKLIB_DIR}/build/libGKlib.a)

add_executable (test_probe tests/test_probe.cpp)
target_include_directories (test_probe PRIVATE src)
target_link_libraries (test_probe libsharpSAT ${GMP_LIB} ${GMPXX_LIB}
    ${METIS_DIR}/build/libmetis/libmetis.a
    ${GKLIB_DIR}/build/libGKlib.a)

add_executable (test_canonical_key_invariance tests/test_canonical_key_invariance.cpp)
target_include_directories (test_canonical_key_invariance PRIVATE src)
target_link_libraries (test_canonical_key_invariance libsharpSAT ${GMP_LIB} ${GMPXX_LIB}
    ${METIS_DIR}/build/libmetis/libmetis.a
    ${GKLIB_DIR}/build/libGKlib.a)

add_executable (test_canonical_key_learned tests/test_canonical_key_learned.cpp)
target_include_directories (test_canonical_key_learned PRIVATE src)
target_link_libraries (test_canonical_key_learned libsharpSAT ${GMP_LIB} ${GMPXX_LIB}
    ${METIS_DIR}/build/libmetis/libmetis.a
    ${GKLIB_DIR}/build/libGKlib.a)
