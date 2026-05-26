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

# Find SDL3
find_package(SDL3 REQUIRED)

# Find FreeType
find_package(Freetype REQUIRED)

# GLM (header-only from local directory)
include_directories(include/glm-1.0.2)

# TinyNURBS (header-only)
include_directories(include/tinynurbs/include)

# ImGui
include_directories(include/imgui include/imgui/backends)

include_directories(include/earcut.hpp-2.2.4/include/)

add_executable(CAD_OpenGL
    src/main.cpp
    src/utils/log.cpp
    src/utils/SessionLogger.cpp
    src/logic/GeometryValidity.cpp
    src/utils/ShutdownStackTrace.cpp
    src/utils/Settings.cpp
    src/display/rendering/OpenGL/shaders/OpenGLShader.cpp
    src/display/rendering/OpenGL/OpenGLRenderer.cpp
    src/display/rendering/SceneRenderer/SceneRenderer.cpp
    src/display/rendering/SceneRenderer/Patch/patch.cpp
    src/display/rendering/SceneRenderer/Wireframe/Wireframe.cpp
    src/display/rendering/color.cpp
    src/display/rendering/UIRenderer/UIRenderer.cpp
    src/display/rendering/UIRenderer/TextRenderer.cpp
    src/display/rendering/UIRenderer/ToolPanel.cpp
    src/display/rendering/UIRenderer/ToolUserErrorFeedback.cpp
    src/display/rendering/ViewportRenderer/ViewportRenderer.cpp
    src/display/rendering/Camera/camera.cpp
    src/display/rendering/ScenePick.cpp
    src/display/rendering/CalibPickSegments.cpp
    src/display/display.cpp
    src/input/Input.cpp
    src/input/FileImport.cpp
    src/scene/scene.cpp
    src/scene/Geometry/Curve.cpp
    src/scene/Geometry/Face.cpp
    src/scene/Geometry/Surface.cpp
    src/logic/Analysis/Analysis.cpp
    src/logic/Analysis/Overhang/Overhang.cpp
    src/logic/Analysis/ThinSection/ThinSection.cpp
    src/logic/Analysis/SharpCorner/SharpCorner.cpp
    src/logic/Analysis/SmallFeature/SmallFeature.cpp
    src/logic/Analysis/utils/Slice.cpp
    src/logic/Calibrate/CalibDistanceType.cpp
    src/logic/Calibrate/CalibNominal.cpp
    src/logic/Calibrate/CalibCompensation.cpp
    src/logic/Import/STLImport.cpp
    src/logic/Import/OBJImport.cpp
    src/logic/Import/ThreeMFImport.cpp
    src/logic/Import/STEPImport.cpp
    include/miniz.c
    include/miniz_tdef.c
    include/miniz_tinfl.c
    include/miniz_zip.c
    include/tinyxml2.cpp
    include/imgui/imgui.cpp
    include/imgui/imgui_draw.cpp
    include/imgui/imgui_tables.cpp
    include/imgui/imgui_widgets.cpp
    include/imgui/imgui_demo.cpp
    include/imgui/backends/imgui_impl_sdl3.cpp
    include/imgui/backends/imgui_impl_opengl3.cpp

)

if(APPLE)
    target_sources(CAD_OpenGL PRIVATE
        src/utils/SystemAccent.mm
        src/utils/SystemAppearance.mm
    )
else()
    target_sources(CAD_OpenGL PRIVATE
        src/utils/SystemAccent_stub.cpp
        src/utils/SystemAppearance_stub.cpp
    )
endif()

add_custom_command(TARGET CAD_OpenGL POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/src/display/rendering/OpenGL/shaders
        ${CMAKE_BINARY_DIR}/shaders
)

add_custom_command(TARGET CAD_OpenGL POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/src/display/rendering/UIRenderer/shaders
        ${CMAKE_BINARY_DIR}/shaders
)

target_include_directories(CAD_OpenGL PRIVATE
    ${CMAKE_SOURCE_DIR}/include/glad/include
    ${CMAKE_SOURCE_DIR}/include/earcut.hpp-2.2.4/include/mapbox
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/logic
    ${CMAKE_SOURCE_DIR}/src/logic/Calibrate
    ${CMAKE_SOURCE_DIR}/src/display
    ${CMAKE_SOURCE_DIR}/src/display/window
    ${CMAKE_SOURCE_DIR}/src/display/rendering/Camera
    ${CMAKE_SOURCE_DIR}/src/display/rendering/OpenGL
    ${CMAKE_SOURCE_DIR}/src/display/rendering/SceneRenderer
    ${CMAKE_SOURCE_DIR}/src/display/rendering/ViewportRenderer
    ${CMAKE_SOURCE_DIR}/src/display/rendering/UIRenderer
    ${CMAKE_SOURCE_DIR}/src/scene
    ${CMAKE_SOURCE_DIR}/src/scene/Geometry
    ${FREETYPE_INCLUDE_DIRS}
)


target_link_libraries(CAD_OpenGL PRIVATE
    glad
    SDL3::SDL3
    Freetype::Freetype
)

if(WIN32)
    target_link_libraries(CAD_OpenGL PRIVATE opengl32)
endif()

# CGAL is used by:
#   1. The Structure tool's face triangulation and carve (`StructureTriangulation.cpp`, `StructureCarve.cpp`).
#   2. The legacy STL planar remesh experiment (`STLCgalPlanarExperiment.cpp`).
# Default ON because the Structure tool depends on it; can be disabled for distributions that need
# to avoid the (L)GPL transitive licence. When OFF, the Structure tool falls back to a "CGAL not
# available" hint and no triangulation runs.
option(CAD_USE_CGAL
    "Link against CGAL (required for the Structure tool's face triangulation; LGPL/GPL — see docs)"
    ON)
if(CAD_USE_CGAL)
    # CGAL prints a deferred notice when CMAKE_BUILD_TYPE is not Release; Debug is normal here.
    if(NOT DEFINED CGAL_DO_NOT_WARN_ABOUT_CMAKE_BUILD_TYPE)
        set(CGAL_DO_NOT_WARN_ABOUT_CMAKE_BUILD_TYPE TRUE CACHE BOOL
            "Suppress CGAL non-Release build reminder (unset with cmake -U... to see it again)")
    endif()
    find_package(CGAL REQUIRED CONFIG)
    target_compile_definitions(CAD_OpenGL PRIVATE CAD_USE_CGAL=1)
    target_sources(CAD_OpenGL PRIVATE
        src/logic/Import/STLCgalPlanarExperiment.cpp
        src/logic/Structure/StructureTriangulation.cpp
        src/logic/Structure/StructureCarve.cpp)
    target_link_libraries(CAD_OpenGL PRIVATE CGAL::CGAL)
    message(STATUS "CGAL: ENABLED (Structure triangulation + STL planar remesh experiment)")
else()
    message(STATUS "CGAL: disabled — Structure triangulation unavailable")
endif()

# Mac-specific
if(APPLE)
    target_link_libraries(CAD_OpenGL PRIVATE
        "-framework OpenGL"
        "-framework AppKit"
    )
endif()

# OpenCASCADE Support
find_package(OpenCASCADE)
if(OpenCASCADE_FOUND)
    message(STATUS "OpenCASCADE found: ${OpenCASCADE_VERSION}")
    target_include_directories(CAD_OpenGL PRIVATE ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(CAD_OpenGL PRIVATE ${OpenCASCADE_LIBRARIES})
    
    # Spike executable
    add_executable(occt_spike src/spike/occt_spike.cpp)
    target_include_directories(occt_spike PRIVATE ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(occt_spike PRIVATE ${OpenCASCADE_LIBRARIES})
else()
    message(STATUS "OpenCASCADE not found. STEP import and Spike executable will not be available.")
endif()