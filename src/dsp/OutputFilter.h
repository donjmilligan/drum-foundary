// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "../Parameters.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>

namespace drum
{

/** The final output's low cut (high-pass) and high cut (low-pass): Butterworth
    cascades of 12, 24 or 48 dB/oct (one, two or four 2nd-order sections with the
    Butterworth Qs), built from TPT state variable filters so fast cutoff
    automation can't make them blow up.

    The cutoffs glide (exponentially, ~40 ms) and the coefficients follow every
    16 samples. A side that is off (low cut <= highPassOffHz, high cut >=
    lowPassOffHz) leaves the samples untouched, so with both off the output is
    bit-identical to not having the filter at all. Switching a side on or off, or
    changing its slope, crossfades over ~5 ms between the old and new cascade. */
class OutputFilter
{
public:
    static constexpr float highPassOffHz = 20.5f;
    static constexpr float lowPassOffHz = 19900.0f;

    static bool isHighPassOff (float hz) noexcept { return hz <= highPassOffHz; }
    static bool isLowPassOff (float hz) noexcept  { return hz >= lowPassOffHz; }

    /** Filter order for an index into filterSlopeNames: 2, 4 or 8. */
    static int slopeOrder (int slopeIndex) noexcept;

    /** Combined magnitude response of both sides, in dB, at freqHz, for the
        given settings (slopes are indices into filterSlopeNames). This is the
        exact response of the digital filter once the cutoffs have settled, so
        the UI can draw it. Cheap enough to call per pixel; any thread. */
    static float responseDb (float freqHz, float highPassHz, int highPassSlope,
                             float lowPassHz, int lowPassSlope, double sampleRate) noexcept;

    void prepare (double sampleRate);
    /** Clears the filter states; the next block starts from its own settings
        rather than gliding to them. */
    void reset() noexcept;

    /** Filters a stereo pair of channels in place. */
    void process (float* left, float* right, int numSamples, const DrumParams& p) noexcept;

private:
    static constexpr int maxSections = 4;
    static constexpr int updateInterval = 16;

    /** One 2nd-order section: Cytomic / Simper TPT SVF, two channels. */
    struct Section
    {
        float k = 1.41421356f, a1 = 0, a2 = 0, a3 = 0;
        float ic1[2] {}, ic2[2] {};
    };

    /** Up to four sections for one order (0 = straight through). */
    struct Cascade
    {
        int order = 0;
        std::array<Section, maxSections> sections;

        void setOrder (int newOrder) noexcept;
        void setCutoff (float g) noexcept;
        void reset() noexcept;
        int numSections() const noexcept { return order / 2; }
    };

    /** The low cut or the high cut. */
    class Side
    {
    public:
        explicit Side (bool isHighPass) : highPass (isHighPass) {}

        void prepare (double sampleRate);
        void reset() noexcept { first = true; }
        void setTarget (float cutoffHz, int slopeIndex) noexcept;
        void process (float* left, float* right, int numSamples) noexcept;

    private:
        void update() noexcept;
        void run (float* left, float* right, int numSamples) noexcept;
        void runCascade (Cascade& c, float* x, int ch, int numSamples) const noexcept;

        const bool highPass;
        double sampleRate = 44100.0;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> cutoff;
        int targetOrder = 4;
        bool first = true;

        Cascade cascades[2];
        int current = 0;        // the cascade being heard; the other one fades in
        int fadePos = -1;       // -1 = not fading
        int fadeLength = 256;   // a multiple of updateInterval, so fades end on an update
        int untilUpdate = 0;
        float lastCutoff = -1.0f;
        float fadeScratch[updateInterval] {};
    };

    Side highCut { false }, lowCut { true };
};

} // namespace drum
