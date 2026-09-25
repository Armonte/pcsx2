# sol2 v3.3.0 -- header-only Lua <-> C++ binding. Single-header amalgamation in include/sol/.
# Vendored from https://github.com/ThePhd/sol2 (release assets sol.hpp + forward.hpp).
add_library(sol2 INTERFACE)
target_include_directories(sol2 INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(sol2 INTERFACE
	SOL_ALL_SAFETIES_ON=1
	SOL_EXCEPTIONS_SAFE_PROPAGATION=1
)
target_link_libraries(sol2 INTERFACE lua)
