target_sources(${PROJECT_NAME} PRIVATE
        CustomTexture.cpp
        CustomTexture.h
        osd.cpp
        osd.h
        sorter.cpp
        sorter.h
        TexCache.cpp
        TexCache.h
        texconv.cpp
        texconv.h
        transform_matrix.cpp
        transform_matrix.h
        norend/norend.cpp)

add_subdirectory(vulkan)
