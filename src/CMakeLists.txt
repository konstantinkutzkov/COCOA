add_subdirectory (component_types)

SET(SOURCES alt_component_analyzer.cpp
	canonical_key.cpp
	component_management.cpp
	instance.cpp
	nd_hierarchy.cpp
	preprocessor.cpp
	preprocessor_light.cpp
	probe_preprocessor.cpp
	solver.cpp
	solver_rec.cpp
	statistics.cpp)

# preprocessor_light.cpp includes cryptominisat.h, which transitively
# pulls in solvertypesmini.h — both rely on C++17 features
# (std::make_unique, if constexpr). The rest of the project is C++11.
# Strip the project-level -std=c++11 from this file and substitute -std=c++17.
set_source_files_properties(preprocessor_light.cpp PROPERTIES
	COMPILE_FLAGS "-std=c++17 -U_LIBCPP_DISABLE_DEPRECATION_WARNINGS")

ADD_LIBRARY(libsharpSAT ${SOURCES} component_types)
target_link_libraries (libsharpSAT component_types) 
