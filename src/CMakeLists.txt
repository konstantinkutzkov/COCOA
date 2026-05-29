add_subdirectory (component_types)

SET(SOURCES alt_component_analyzer.cpp
	canonical_key.cpp
	component_management.cpp
	instance.cpp
	nd_hierarchy.cpp
	preprocessor.cpp
	preprocessor_arjun.cpp
	preprocessor_light.cpp
	probe_preprocessor.cpp
	sat_check.cpp
	scc_unsat.cpp
	scc_unsat_solver.cpp
	solver.cpp
	solver_diagnostics.cpp
	solver_rec.cpp
	statistics.cpp)

# preprocessor_light.cpp, sat_check.cpp, and preprocessor_arjun.cpp all
# include cryptominisat.h or arjun.h, which transitively pull in C++17
# features (std::make_unique, if constexpr, polymorphic Field types).
# The rest of the project is C++11. Strip the project-level -std=c++11
# from these files.
set_source_files_properties(preprocessor_light.cpp sat_check.cpp
	preprocessor_arjun.cpp PROPERTIES
	COMPILE_FLAGS "-std=c++17 -U_LIBCPP_DISABLE_DEPRECATION_WARNINGS")

ADD_LIBRARY(libsharpSAT ${SOURCES} component_types)
target_link_libraries (libsharpSAT component_types)
# CMS5 headers are only consumed by preprocessor_light.cpp.
# Arjun headers are only consumed by preprocessor_arjun.cpp.
# PRIVATE keeps the include path off the test-executable compiles.
target_include_directories(libsharpSAT PRIVATE ${CMS5_DIR} ${CMS5_BUILD_INCLUDE} ${ARJUN_DIR} /opt/homebrew/include)
