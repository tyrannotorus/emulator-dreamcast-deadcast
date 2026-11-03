if (ENABLE_DC_PROFILER)
    target_sources(${PROJECT_NAME} PRIVATE
            dc_profiler.cpp
            dc_profiler.h)

    target_compile_definitions(${PROJECT_NAME} PRIVATE DC_PROFILER)
endif()
