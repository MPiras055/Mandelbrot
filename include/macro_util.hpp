#pragma once

// =====================================================================
// 1. CACHE LINE CONFIGURATION
// =====================================================================
#include <immintrin.h>
#ifndef CACHE_LINE_SIZE
    #define CACHE_LINE_SIZE 128
#endif

#define CACHE_ALIGN alignas(CACHE_LINE_SIZE)

// Helper macros for unique identifier generation
#define PT_CONCAT_INNER(a, b) a ## b
#define PT_CONCAT(a, b) PT_CONCAT_INNER(a, b)
#define UNIQUE_PAD_NAME PT_CONCAT(_cache_pad_, __LINE__)

// =====================================================================
// 2. SMART CACHE PADDING MACRO
// =====================================================================
/**
 * @brief Computes the remaining space in a cache line for a given type 
 * and injects the exact number of padding bytes required to close it out.
 * * @details If sizeof(Type) is a perfectly clean multiple of CACHE_LINE_SIZE, 
 * it sets padding to 0 bytes so no memory is wasted.
 */
#define CACHE_PAD(Type) \
    unsigned char UNIQUE_PAD_NAME[(sizeof(Type) % CACHE_LINE_SIZE == 0) ? 0 : (CACHE_LINE_SIZE - (sizeof(Type) % CACHE_LINE_SIZE))];

