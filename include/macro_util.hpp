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

// =====================================================================
// 3. ENHANCED FILE & LINE "TODO" WARNING MACROS
// =====================================================================
#define PT_STR_HELPER(x) #x
#define PT_STR(x) PT_STR_HELPER(x)

#if defined(_MSC_VER)
    // MSVC prints clickable IDE lines via "FILE(LINE) : message"
    #define TODO(msg) __pragma(message(__FILE__ "(" PT_STR(__LINE__) ") : TODO: " msg))

#elif defined(__clang__)
    // Clang supports diagnostic pragmas that expand macros flawlessly
    #define TODO(msg) _Pragma(PT_STR(GCC warning ("TODO: " msg)))

#elif defined(__GNUC__)
    // GCC requires explicit string aggregation to show file/line inline in the text
    #define TODO(msg) _Pragma(PT_STR(message ("TODO [" __FILE__ ":" PT_STR(__LINE__) "]: " msg)))

#else
    // Fallback
    #define TODO(msg)
#endif



static inline void cpu_relax() {
    #if defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
    #elif defined(__arm__) || defined(__aarch64__)
        asm volatile("yield" ::: "memory");
    #else
        std::this_thread::yield();
    #endif
}