cmake_minimum_required(VERSION 3.14)
project(MandelbrotApp)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_BUILD_TYPE Release)

include(FetchContent)

# --- Raylib ---
FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG        5.0
    GIT_SHALLOW    ON
)
FetchContent_MakeAvailable(raylib)

FetchContent_Declare(
    boost
    URL https://archives.boost.io/release/1.84.0/source/boost_1_84_0.tar.bz2
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_GetProperties(boost)
if(NOT boost_POPULATED)
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    
    FetchContent_Populate(boost)
endif()

# --- Suppress compiler warnings from building Raylib and its bundled GLFW ---
if(TARGET raylib)
    target_compile_options(raylib PRIVATE $<$<C_COMPILER_ID:GNU,Clang>:-w>)
endif()
if(TARGET glfw)
    target_compile_options(glfw PRIVATE $<$<C_COMPILER_ID:GNU,Clang>:-w>)
endif()

file(GLOB_RECURSE PROJECT_HEADERS 
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp" 
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
)

file(GLOB_RECURSE PROJECT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(${PROJECT_NAME} 
    ${PROJECT_SOURCES}
    ${PROJECT_HEADERS}
)

target_include_directories(${PROJECT_NAME} 
    PUBLIC SYSTEM 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    SYSTEM PRIVATE 
        ${boost_SOURCE_DIR} 
)

target_compile_options(${PROJECT_NAME} PRIVATE 
    $<$<CONFIG:Release>:-O3 -march=native -mtune=native>
    
    $<$<CONFIG:Release>:-ftree-vectorize -funroll-loops>
    
    $<$<CONFIG:Release>:-ffast-math -fno-finite-math-only -fno-signed-zeros>

    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wno-deprecated-literal-operator>

    -DNDEBUG
)

include(CheckIPOSupported)
check_ipo_supported(RESULT lto_supported OUTPUT error)
if(lto_supported)
    set_property(
      TARGET ${PROJECT_NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE
    )
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE 
    raylib 
    quadmath 
)