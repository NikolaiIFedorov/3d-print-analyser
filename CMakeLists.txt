cmake_minimum_required(VERSION 3.20)
project(CAD_OpenGL)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON) 
set(CMAKE_CXX_EXTENSIONS OFF) 

message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

# Verbose output
message(STATUS "========================================")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "C++ Flags: ${CMAKE_CXX_FLAGS}")
message(STATUS "Generator: ${CMAKE_GENERATOR}")
message(STATUS "========================================")

# Add GLAD
add_library(glad include/glad/src/glad.c)
target_include_directories(glad PUBLIC include/glad/include)

# Find GLFW
find_package(glfw3 REQUIRED)

# GLM (header-only from local directory)
include_directories(include/glm-1.0.2)

# TinyNURBS (header-only)
include_directories(include/tinynurbs/include)

include_directories(include/earcut.hpp-2.2.4/include/)



# Source files
set(SOURCES
    src/main.cpp
    src/utils/log.cpp
    src/utils/clock.cpp                        
    src/display/rendering/OpenGL/shaders/OpenGLShader.cpp
    src/display/rendering/OpenGL/OpenGLRenderer.cpp
    src/display/rendering/SceneRenderer/SceneRenderer.cpp
    src/display/rendering/SceneRenderer/RenderBuffer/RenderBuffer.cpp
    src/display/rendering/SceneRenderer/Patch/patch.cpp
    src/display/rendering/SceneRenderer/Wireframe/Wireframe.cpp
    src/display/rendering/SceneRenderer/color.cpp
    src/display/rendering/Camera/camera.cpp
    src/display/rendering/Camera/camera.cpp
    src/display/display.cpp
    src/scene/scene.cpp
    src/scene/Geometry/Curve.cpp
    src/logic/Analysis.cpp

)

add_executable(CAD_OpenGL
    src/main.cpp
    src/utils/log.cpp  
    src/utils/clock.cpp                        
    src/display/rendering/OpenGL/shaders/OpenGLShader.cpp
    src/display/rendering/OpenGL/OpenGLRenderer.cpp
    src/display/rendering/SceneRenderer/SceneRenderer.cpp
    src/display/rendering/SceneRenderer/RenderBuffer/RenderBuffer.cpp
    src/display/rendering/SceneRenderer/Patch/patch.cpp
    src/display/rendering/SceneRenderer/Wireframe/Wireframe.cpp
    src/display/rendering/SceneRenderer/color.cpp
    src/display/rendering/Camera/camera.cpp
    src/display/rendering/Camera/camera.cpp
    src/display/display.cpp
    src/scene/scene.cpp
    src/scene/Geometry/Curve.cpp
    src/logic/Analysis.cpp
)

add_custom_command(TARGET CAD_OpenGL POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/src/display/rendering/OpenGL/shaders
        ${CMAKE_BINARY_DIR}/shaders
)

target_include_directories(CAD_OpenGL PRIVATE
    ${CMAKE_SOURCE_DIR}/include/glad/include
    ${DMAKE_SOURCE_DIR}/include/earcut.hpp-2.2.4/include/mapbox
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/logic
    ${CMAKE_SOURCE_DIR}/src/display
    ${CMAKE_SOURCE_DIR}/src/display/window
    ${CMAKE_SOURCE_DIR}/src/display/rendering/Camera
    ${CMAKE_SOURCE_DIR}/src/display/rendering/OpenGL
    ${CMAKE_SOURCE_DIR}/src/display/rendering/SceneRenderer
    ${CMAKE_SOURCE_DIR}/src/scene
    ${CMAKE_SOURCE_DIR}/src/scene/Geometry

)


target_link_libraries(CAD_OpenGL
    glad
    glfw
)

# Mac-specific
if(APPLE)
    target_link_libraries(CAD_OpenGL
        "-framework OpenGL"
        "-framework Cocoa"
        "-framework IOKit"
        "-framework CoreVideo"
    )
endif()