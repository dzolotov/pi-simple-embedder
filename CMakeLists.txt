cmake_minimum_required(VERSION 3.10)
project(flutter_x11_embedder)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(PkgConfig REQUIRED)
find_package(X11 REQUIRED)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLES2 REQUIRED glesv2)

# Add executable
add_executable(flutter_x11_embedder
    x11_embedder.cc
)

# Include directories
target_include_directories(flutter_x11_embedder PRIVATE
    ${X11_INCLUDE_DIR}
    ${EGL_INCLUDE_DIRS}
    ${GLES2_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Link libraries
target_link_libraries(flutter_x11_embedder
    ${X11_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES2_LIBRARIES}
    ${CMAKE_DL_LIBS}
    pthread
    ${CMAKE_CURRENT_SOURCE_DIR}/../libflutter_engine.so
)

# Compile options
target_compile_options(flutter_x11_embedder PRIVATE
    ${EGL_CFLAGS_OTHER}
    ${GLES2_CFLAGS_OTHER}
    -Wall
    -Wextra
)

# Set RPATH
set_target_properties(flutter_x11_embedder PROPERTIES
    INSTALL_RPATH "${CMAKE_CURRENT_SOURCE_DIR}/.."
    BUILD_WITH_INSTALL_RPATH TRUE
)