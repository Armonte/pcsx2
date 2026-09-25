# Lua 5.4.7 -- static library for the SDBZ Lua overlay scripting layer.
# Vendored verbatim from https://www.lua.org/ftp/lua-5.4.7.tar.gz (src/*.c, *.h, lua.hpp).
# Built as C; the stand-alone mains lua.c (interpreter) and luac.c (compiler) are excluded.

file(GLOB LUA_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
list(REMOVE_ITEM LUA_SOURCES
	"${CMAKE_CURRENT_SOURCE_DIR}/lua.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/luac.c"
)

add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(lua PROPERTIES C_STANDARD 99)

if(WIN32)
	# Windows facilities (LoadLibrary-based package.loadlib, etc.). We build a STATIC lib,
	# so deliberately do NOT define LUA_BUILD_AS_DLL.
	target_compile_definitions(lua PRIVATE LUA_USE_WINDOWS)
endif()
