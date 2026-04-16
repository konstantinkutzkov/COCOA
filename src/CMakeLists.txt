add_subdirectory (component_types)

SET(SOURCES alt_component_analyzer.cpp
	canonical_key.cpp
	component_analyzer.cpp
	component_management.cpp
	instance.cpp
	nd_hierarchy.cpp
	new_component_analyzer.cpp
	separator.cpp
	solver.cpp
	solver_rec.cpp
	statistics.cpp)


ADD_LIBRARY(libsharpSAT ${SOURCES} component_types)
target_link_libraries (libsharpSAT component_types) 
