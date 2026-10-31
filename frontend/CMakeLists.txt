# Deadcast's frontend: its menu and pages, drawn on the CPU, shared by Deadcast and every recomp module
# (one look, one menu: a module adds no menu of its own). Needs nothing of the emulator.
add_library(deadcast_frontend STATIC
        canvas.cpp
        canvas.h
        recomp.cpp
        recomp.h
        ui.cpp
        ui.h)
target_include_directories(deadcast_frontend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(deadcast_frontend PUBLIC cxx_std_17)
target_link_libraries(deadcast_frontend PRIVATE freetype)

# the menu's font, Noto Sans Bold (SIL OFL 1.1, fonts/NotoSans-OFL.txt), into the library itself
set(DEADCAST_FONT_ZIP ${CMAKE_CURRENT_SOURCE_DIR}/../fonts/NotoSans-Bold.ttf.zip)
set(DEADCAST_FONT ${CMAKE_CURRENT_BINARY_DIR}/font/NotoSans-Bold.ttf)
if(NOT EXISTS ${DEADCAST_FONT} OR ${DEADCAST_FONT_ZIP} IS_NEWER_THAN ${DEADCAST_FONT})
    file(ARCHIVE_EXTRACT INPUT ${DEADCAST_FONT_ZIP} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/font)
endif()
target_compile_definitions(deadcast_frontend PRIVATE DEADCAST_FONT="${DEADCAST_FONT}")
set_source_files_properties(ui.cpp PROPERTIES OBJECT_DEPENDS ${DEADCAST_FONT})
