target_compile_definitions(${PROJECT_NAME} PRIVATE USE_VULKAN HAVE_VULKAN)
target_compile_definitions(${PROJECT_NAME} PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)

target_sources(${PROJECT_NAME} PRIVATE
        allocator.cpp
        allocator.h
        buffer.cpp
        buffer.h
        commandpool.cpp
        commandpool.h
        desc_set.h
        drawer.cpp
        drawer.h
        overlay.cpp
        overlay.h
        panel.cpp
        panel.h
        pipeline.cpp
        pipeline.h
        quad.cpp
        quad.h
        shaders.cpp
        shaders.h
        texture.cpp
        texture.h
        utils.h
        vulkan.h
        vulkan_context.cpp
        vulkan_context.h
        vulkan_driver.h
        vulkan_renderer.cpp
        vulkan_renderer.h
        "${IMGUI_BACKENDS_DIR}/imgui_impl_vulkan.cpp"
        "${IMGUI_BACKENDS_DIR}/imgui_impl_vulkan.h")

# Deadcast: the shaders (shaders/*) are compiled to SPIR-V here, by the host's glslangValidator (glslang),
# into headers shaders.cpp includes: one for each set of the preprocessor's switches a shader takes.
# Nothing compiles GLSL on the Brick.
find_program(DEADCAST_GLSLANG glslangValidator NO_CMAKE_FIND_ROOT_PATH REQUIRED)
set(DEADCAST_SPV_DIR "${CMAKE_CURRENT_BINARY_DIR}/spv")
file(MAKE_DIRECTORY "${DEADCAST_SPV_DIR}")
set(DEADCAST_SPV_HEADERS)
function(deadcast_spv name source)
    set(defines)
    foreach(define ${ARGN})
        list(APPEND defines "-D${define}")
    endforeach()
    set(out "${DEADCAST_SPV_DIR}/${name}.h")
    add_custom_command(OUTPUT "${out}"
        COMMAND "${DEADCAST_GLSLANG}" -V --target-env vulkan1.1 ${defines} --vn "spv_${name}" -o "${out}"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${source}"
        DEPENDS "shaders/${source}" shaders/n2_light.glsl
        COMMENT "SPIR-V ${name}"
        VERBATIM)
    set(DEADCAST_SPV_HEADERS ${DEADCAST_SPV_HEADERS} "${out}" PARENT_SCOPE)
endfunction()
foreach(g 0 1)
    foreach(d 0 1)
        deadcast_spv(poly_vert_g${g}_d${d} poly.vert pp_Gouraud=${g} DIV_POS_Z=${d})
        deadcast_spv(poly_n2_vert_g${g}_t${d} poly_n2.vert pp_Gouraud=${g} pp_Texture=${d})
    endforeach()
    deadcast_spv(poly_frag_g${g} poly.frag pp_Gouraud=${g})
    deadcast_spv(modvol_vert_d${g} modvol.vert DIV_POS_Z=${g})
    deadcast_spv(modvol_frag_d${g} modvol.frag DIV_POS_Z=${g})
    deadcast_spv(quad_vert_r${g} quad.vert ROTATE=${g})
    deadcast_spv(quad_frag_a${g} quad.frag IGNORE_TEX_ALPHA=${g})
endforeach()
deadcast_spv(modvol_n2_vert modvol_n2.vert)
add_custom_target(deadcast_spv DEPENDS ${DEADCAST_SPV_HEADERS})
add_dependencies(${PROJECT_NAME} deadcast_spv)
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
