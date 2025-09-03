cmake_minimum_required(VERSION 3.20)

# Find OpenGL and X11 libraries
find_package(OpenGL REQUIRED)

if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
endif()

# No external math library needed - using simple Vec3 struct

# Create the opengl_renderer library
add_library(remoteview_opengl_renderer STATIC
    huespace_renderer.cpp
)

target_include_directories(remoteview_opengl_renderer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(remoteview_opengl_renderer PRIVATE
    remoteview_config
    spdlog::spdlog
    ${HUESPACE_LIBRARY}
    OpenGL::GL
)

if(UNIX AND NOT APPLE)
    target_link_libraries(remoteview_opengl_renderer PRIVATE
        X11::X11
        X11::Xutil
    )
endif()

# Link with GLX on Linux
if(UNIX AND NOT APPLE)
    find_library(GLX_LIBRARY GLX)
    if(GLX_LIBRARY)
        target_link_libraries(remoteview_opengl_renderer PRIVATE ${GLX_LIBRARY})
    endif()
endif()

target_compile_features(remoteview_opengl_renderer PRIVATE cxx_std_20)

# Include HueSpace headers
target_include_directories(remoteview_opengl_renderer PRIVATE ${OPENVDS_INCLUDE_DIR})

# Add OpenGL extension loader (GLX functions)
if(UNIX AND NOT APPLE)
    target_compile_definitions(remoteview_opengl_renderer PRIVATE GL_GLEXT_PROTOTYPES)
endif()