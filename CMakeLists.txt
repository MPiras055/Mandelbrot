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
    URL https://github.com/raysan5/raylib/archive/refs/tags/5.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
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

# --- 1. Exclude entry points from the shared source list ---
list(REMOVE_ITEM PROJECT_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark.cpp"
)

# --- 2. Define both executables ---
add_executable(${PROJECT_NAME} 
    src/main.cpp 
    ${PROJECT_SOURCES} 
    ${PROJECT_HEADERS}
)

add_executable(benchmarkUtil 
    src/benchmark.cpp 
    ${PROJECT_SOURCES} 
    ${PROJECT_HEADERS}
)

# --- 3. Create a shared settings container ---
add_library(SharedProjectSettings INTERFACE)

target_include_directories(SharedProjectSettings INTERFACE 
    SYSTEM "${CMAKE_CURRENT_SOURCE_DIR}/include"
    SYSTEM "${boost_SOURCE_DIR}"
)

target_compile_options(SharedProjectSettings INTERFACE 
    $<$<CONFIG:Release>:-O3 -march=native -mtune=native>
    $<$<CONFIG:Release>:-ftree-vectorize -funroll-loops>
    $<$<CONFIG:Release>:-ffast-math -fno-finite-math-only -fno-signed-zeros>
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wno-deprecated-literal-operator>
    -DNDEBUG
)

target_link_libraries(SharedProjectSettings INTERFACE 
    raylib 
    quadmath
)

# --- 4. Apply settings to both executables ---
target_link_libraries(${PROJECT_NAME} PRIVATE SharedProjectSettings)
target_link_libraries(benchmarkUtil PRIVATE SharedProjectSettings)

# --- 5. LTO / IPO Support ---
include(CheckIPOSupported)
check_ipo_supported(RESULT lto_supported OUTPUT error)
if(lto_supported)
    set_property(
        TARGET ${PROJECT_NAME} benchmarkUtil 
        PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE
    )
endif()

# =====================================================================
# 6. LINK LIBRARIES (Handled by SharedProjectSettings above)
# =====================================================================

# CRITICAL WINDOWS FIX: The Ultimate Static Hammer
# Forces MinGW to pack libstdc++, 
# libgcc, winpthread, and quadmath directly into the .exe
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_EXE_LINKER_FLAGS 
      "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++"
    )
endif()