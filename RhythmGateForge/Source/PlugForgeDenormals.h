#pragma once
/**
 * Portable flush-to-zero / denormal control for real-time DSP.
 * - Intel/AMD (JUCE_INTEL): MXCSR FTZ+DAZ via _mm_setcsr (NO xmmintrin/mmintrin includes)
 * - Apple Silicon / aarch64: FPCR.FZ bit
 * - Otherwise: no-op (callers may also use juce::ScopedNoDenormals)
 *
 * Never include <xmmintrin.h> or <mmintrin.h> — those break Apple Silicon arm64 builds
 * when pulled into unity / module amalgamation incorrectly.
 */
#include <cstdint>

inline void pfEnableFlushToZero() noexcept
{
#if JUCE_INTEL
    _mm_setcsr (_mm_getcsr() | 0x8040);
#elif (defined(__aarch64__) || defined(_M_ARM64)) && (defined(__GNUC__) || defined(__clang__))
    std::uint64_t fpcr = 0;
    __asm__ __volatile__ ("mrs %0, fpcr" : "=r" (fpcr));
    fpcr |= (1ull << 24); // FZ — flush denormals to zero
    __asm__ __volatile__ ("msr fpcr, %0" :: "r" (fpcr));
#else
    /* no-op */
#endif
}
