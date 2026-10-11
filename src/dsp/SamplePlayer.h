// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "SampleData.h"
#include <cmath>

namespace drum::dsp
{

/** 4-point, 3rd-order Hermite interpolation between x0 and x1 at fraction t. */
inline float hermite4 (float xm1, float x0, float x1, float x2, float t) noexcept
{
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

/** Raw channel pointers into a SampleData, taken at the start of each block.
    Voices build one per block from the engine's current SampleSet and never keep
    it: the processor may swap or drop a sample between blocks. */
struct SampleView
{
    const float* left = nullptr;
    const float* right = nullptr;   // == left for a mono sample
    int length = 0;
    double sampleRate = 44100.0;
    bool stereo = false;

    static SampleView of (const SampleData* s) noexcept
    {
        SampleView v;
        if (s == nullptr || s->audio.getNumChannels() < 1 || s->audio.getNumSamples() < 1
             || ! std::isfinite (s->sampleRate) || s->sampleRate <= 0.0)
            return v;

        v.left = s->audio.getReadPointer (0);
        v.stereo = s->audio.getNumChannels() > 1;
        v.right = v.stereo ? s->audio.getReadPointer (1) : v.left;
        v.length = s->audio.getNumSamples();
        v.sampleRate = s->sampleRate;
        return v;
    }

    bool isValid() const noexcept { return left != nullptr; }

    /** True once `pos` has run past the last sample (every read is silent). */
    bool isFinished (double pos) const noexcept { return ! isValid() || pos >= (double) length + 1.0; }

    /** Playback increment, in source samples per output sample, for a transpose ratio. */
    double rateFor (double outputRate, double transpose) const noexcept { return sampleRate / outputRate * transpose; }

    /** Hermite-interpolated frame at `pos`. Anything outside the sample reads as zero. */
    inline void read (double pos, float& l, float& r) const noexcept
    {
        const double fl = std::floor (pos);
        const int i = (int) fl;
        const float t = (float) (pos - fl);

        if (i >= 1 && i + 2 < length)
        {
            l = hermite4 (left[i - 1], left[i], left[i + 1], left[i + 2], t);
            r = stereo ? hermite4 (right[i - 1], right[i], right[i + 1], right[i + 2], t) : l;
            return;
        }

        const auto at = [this] (const float* ch, int k) { return (unsigned) k < (unsigned) length ? ch[k] : 0.0f; };
        l = hermite4 (at (left, i - 1), at (left, i), at (left, i + 1), at (left, i + 2), t);
        r = stereo ? hermite4 (at (right, i - 1), at (right, i), at (right, i + 1), at (right, i + 2), t) : l;
    }
};

} // namespace drum::dsp
