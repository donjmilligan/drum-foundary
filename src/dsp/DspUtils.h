// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include <cstdint>

namespace drum::dsp
{

inline constexpr float kTwoPi = 6.283185307179586f;

/** Synthesised partials are kept under this fraction of the sample rate. */
inline constexpr float kMaxInc = 0.45f;

inline float wrap01 (float phase) noexcept { return phase - std::floor (phase); }

/** 2^(st / 12): the frequency ratio for a pitch curve value in semitones, with
    the value limited to +-120 st (the curves' range). A NaN reads as 0 st. */
inline float transposeRatio (float st) noexcept
{
    constexpr float range = 120.0f;
    const float limited = st > -range ? juce::jmin (st, range) : (st <= -range ? -range : 0.0f);
    return std::exp2 (limited * (1.0f / 12.0f));
}

/** Hashes a voice seed with a salt, so each noise source of a hit gets its own stream. */
inline uint32_t mixSeed (uint32_t s, uint32_t salt) noexcept
{
    uint32_t x = s ^ salt;
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/** PolyBLEP residual for a unit step at phase 0, for an oscillator moving dt per sample. */
inline float polyBlep (float t, float dt) noexcept
{
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt)
    {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

/** Band-limited square, +1 for the first half cycle. */
inline float blepSquare (float phase, float dt) noexcept
{
    const float half = phase < 0.5f ? phase + 0.5f : phase - 0.5f;
    return (phase < 0.5f ? 1.0f : -1.0f) + polyBlep (phase, dt) - polyBlep (half, dt);
}

/** sin(2*pi*phase) for any phase, accurate to ~4e-6. Much cheaper than std::sin
    and plenty for oscillators. */
inline float fastSin01 (float phase) noexcept
{
    float p = phase - std::floor (phase);
    float s = 1.0f;
    if (p >= 0.5f) { p -= 0.5f; s = -1.0f; }
    if (p > 0.25f) p = 0.5f - p;

    const float x = p * kTwoPi;
    const float x2 = x * x;
    return s * x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
}

/** Naive triangle in phase with fastSin01 (starts at 0 heading up). */
inline float triangle01 (float phase) noexcept
{
    const float p = phase - std::floor (phase);
    if (p < 0.25f) return 4.0f * p;
    if (p < 0.75f) return 2.0f - 4.0f * p;
    return 4.0f * p - 4.0f;
}

/** Pade-style tanh, clamped. Smooth and monotonic; within ~2% of std::tanh. */
inline float fastTanh (float x) noexcept
{
    if (x > 3.0f)  return 1.0f;
    if (x < -3.0f) return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/** xorshift32 noise source. Deterministic for a given seed, which lets the
    waveform display render the same hit every time. */
struct Rng
{
    uint32_t state = 0x9E3779B9u;

    void seed (uint32_t s) noexcept { state = s != 0 ? s : 0x9E3779B9u; }

    inline uint32_t nextInt() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    /** Uniform in [-1, 1). */
    inline float bipolar() noexcept  { return (float) (int32_t) nextInt() * (1.0f / 2147483648.0f); }
    /** Uniform in [0, 1). */
    inline float unipolar() noexcept { return (float) (nextInt() >> 8) * (1.0f / 16777216.0f); }
};

/** Topology-preserving-transform state variable filter (Cytomic / Simper). */
struct Svf
{
    float a1 = 0, a2 = 0, a3 = 0, k = 1.41421356f;
    float ic1 = 0, ic2 = 0;

    void reset() noexcept { ic1 = ic2 = 0.0f; }

    void setCoefficients (float cutoffHz, float q, float sampleRate) noexcept
    {
        const float fc = juce::jlimit (10.0f, sampleRate * 0.49f, cutoffHz);
        const float g = std::tan (juce::MathConstants<float>::pi * fc / sampleRate);
        k = 1.0f / q;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    struct Out { float lp, bp, hp; };

    inline Out process (float v0) noexcept
    {
        const float v3 = v0 - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return { v2, v1, v0 - k * v1 - v2 };
    }

    inline float lowpass (float x) noexcept  { return process (x).lp; }
    inline float highpass (float x) noexcept { return process (x).hp; }
    /** Band-pass normalised to unity gain at the centre frequency. */
    inline float bandpass (float x) noexcept { return process (x).bp * k; }
};

/** RBJ biquad, transposed direct form II, up to two channels. Coefficient
    calculation never allocates, so it is safe to call per block. */
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1[2] {}, z2[2] {};

    void reset() noexcept { z1[0] = z1[1] = z2[0] = z2[1] = 0.0f; }

    void setPeak (float sr, float freq, float q, float gainDb) noexcept
    {
        const float A = std::pow (10.0f, gainDb / 40.0f);
        const float w0 = kTwoPi * juce::jlimit (10.0f, sr * 0.49f, freq) / sr;
        const float cw = std::cos (w0), alpha = std::sin (w0) / (2.0f * q);
        set (1.0f + alpha * A, -2.0f * cw, 1.0f - alpha * A,
             1.0f + alpha / A, -2.0f * cw, 1.0f - alpha / A);
    }

    void setLowShelf (float sr, float freq, float gainDb) noexcept
    {
        const float A = std::pow (10.0f, gainDb / 40.0f);
        const float w0 = kTwoPi * juce::jlimit (10.0f, sr * 0.49f, freq) / sr;
        const float cw = std::cos (w0), alpha = std::sin (w0) / (2.0f * 0.7071f);
        const float sa = 2.0f * std::sqrt (A) * alpha;
        set (A * ((A + 1) - (A - 1) * cw + sa), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - sa),
             (A + 1) + (A - 1) * cw + sa, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - sa);
    }

    void setHighShelf (float sr, float freq, float gainDb) noexcept
    {
        const float A = std::pow (10.0f, gainDb / 40.0f);
        const float w0 = kTwoPi * juce::jlimit (10.0f, sr * 0.49f, freq) / sr;
        const float cw = std::cos (w0), alpha = std::sin (w0) / (2.0f * 0.7071f);
        const float sa = 2.0f * std::sqrt (A) * alpha;
        set (A * ((A + 1) + (A - 1) * cw + sa), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - sa),
             (A + 1) - (A - 1) * cw + sa, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - sa);
    }

    inline float process (float x, int ch) noexcept
    {
        const float y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return y;
    }

private:
    void set (float nb0, float nb1, float nb2, float na0, float na1, float na2) noexcept
    {
        const float inv = 1.0f / na0;
        b0 = nb0 * inv; b1 = nb1 * inv; b2 = nb2 * inv;
        a1 = na1 * inv; a2 = na2 * inv;
    }
};

/** First-order DC blocker, ~10 Hz corner. */
struct DcBlocker
{
    float r = 0.9987f, x1 = 0, y1 = 0;

    void prepare (float sr) noexcept { r = 1.0f - kTwoPi * 10.0f / sr; reset(); }
    void reset() noexcept { x1 = y1 = 0.0f; }

    inline float process (float x) noexcept
    {
        const float y = x - x1 + r * y1;
        x1 = x;
        y1 = y;
        return y;
    }
};

} // namespace drum::dsp
