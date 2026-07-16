cmake_minimum_required(VERSION 3.14)
project(MandelbrotApp)

# Use C++26 for std::experimental::simd
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CRITICAL FOR LSP: Generates compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Force Release Mode for maximum compiler optimization passes
set(CMAKE_BUILD_TYPE Release)

# =====================================================================
# 1. TRACK ALL SOURCE AND HEADER FILES FOR THE TARGET
# =====================================================================
# We explicitly gather all .hpp and .h files. This maps your entire include/
# folder structure into the build system so clangd stops treating headers 
# inside subdirectories as isolated orphans.
file(GLOB_RECURSE PROJECT_HEADERS 
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp" 
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
)

# All translation units under src/ (main + the split-out engine components).
# NOTE: GLOB is resolved at configure time — re-run cmake after adding a new .cpp.
file(GLOB_RECURSE PROJECT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

# =====================================================================
# 2. ADD DEPENDENCIES
# =====================================================================
# Add Raylib subdirectory (compiles it contextually)
add_subdirectory(${CMAKE_SOURCE_DIR}/deps/raylib)

# =====================================================================
# 3. DEFINE EXECUTABLE TARGET
# =====================================================================
# Passing both sources AND headers forces CMake to group them under the same target.
add_executable(${PROJECT_NAME} 
    ${PROJECT_SOURCES}
    ${PROJECT_HEADERS}
)

# =====================================================================
# 4. TARGET INCLUDE DIRECTORIES (The Angle Bracket Fix)
# =====================================================================
# - We use PUBLIC so these directories flow directly into every header file context.
# - We use SYSTEM to force the build toolchain and Clangd to index "include/" 
#   as a permanent system anchor root, enabling perfect <...> resolution.
target_include_directories(${PROJECT_NAME} 
    PUBLIC SYSTEM 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    SYSTEM PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/deps/boost
)
target_compile_options(${PROJECT_NAME} PRIVATE 
    $<$<CONFIG:Release>:-O3 -march=native -ffast-math>
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wno-deprecated-literal-operator>
)

# =====================================================================
# 5. COMPILER OPTIMIZATIONS & LINKING
# =====================================================================
target_compile_options(${PROJECT_NAME} PRIVATE -O3 -march=native -ffast-math)
target_link_libraries(${PROJECT_NAME} PRIVATE raylib quadmath)