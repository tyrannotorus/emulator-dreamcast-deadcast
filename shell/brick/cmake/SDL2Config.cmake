# SDL2 from the tg5040 sysroot, which ships only sdl2.pc and no CMake package. Linked by soname, so
# on the device NextUI's own SDL2 (/usr/trimui/lib) is the one loaded.
if(NOT TARGET SDL2::SDL2)
	add_library(SDL2::SDL2 SHARED IMPORTED)
	set_target_properties(SDL2::SDL2 PROPERTIES
		IMPORTED_LOCATION "${CMAKE_SYSROOT}/usr/lib/libSDL2.so"
		INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SYSROOT}/usr/include/SDL2"
		INTERFACE_COMPILE_DEFINITIONS "_REENTRANT")
endif()
set(SDL2_FOUND TRUE)
