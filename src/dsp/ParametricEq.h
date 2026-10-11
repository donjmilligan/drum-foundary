// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "../Parameters.h"
#include <array>
#include <atomic>

namespace drum
{

/** Indices into eqTypeNames (Parameters.h). */
enum EqType
{
    eqBell = 0,
    eqLowShelf,
    eqHighShelf,
    eqLowCut,
    eqHighCut,
    eqNotch,
    eqBandPass,
    eqTilt,
    numEqTypes
};

/** Indices into eqStyleNames. */
enum EqStyle
{
    eqStyleClean = 0,   // matched to the analog curve: no cramping near Nyquist
    eqStyleDigital,     // bilinear (the classic digital EQ; what the old 3-band EQ was)
    eqStyleBritish,     // console-style: a bell narrows as it boosts or cuts harder
    eqStylePultec,      // Pultec-style: resonant shelves (a dip next to the boost), broad bells
    numEqStyles
};

/** True for the types whose Gain does something (and that can be dynamic). */
inline constexpr bool eqTypeHasGain (int type) noexcept
{
    return type == eqBell || type == eqLowShelf || type == eqHighShelf || type == eqTilt;
}

/** True for the types whose Slope does something (the cuts). */
inline constexpr bool eqTypeHasSlope (int type) noexcept { return type == eqLowCut || type == eqHighCut; }

/** True for the types whose Q does something (all but Tilt). On a cut it's the
    resonance at the corner (0.71 = none). */
inline constexpr bool eqTypeHasQ (int type) noexcept { return type != eqTilt; }

/** Filter order for each eqSlopeNames entry (6 dB/oct per order). */
inline constexpr int eqSlopeOrder (int slope) noexcept
{
    constexpr int orders[] { 1, 2, 3, 4, 6, 8, 12, 16 };
    return orders[slope < 0 ? 0 : slope > 7 ? 7 : slope];
}

/** One second-order section, normalised (a0 = 1). A first-order one has b2 = a2 = 0. */
struct EqSection
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

/** A band's filter: up to kMaxSections sections in series (96 dB/oct is 8). None = flat. */
struct EqDesign
{
    static constexpr int kMaxSections = 9;
    std::array<EqSection, kMaxSections> sections {};
    int count = 0;

    /** |H| at `hz`, linear. */
    double magnitude (double hz, double sampleRate) const noexcept;
};

/** The EQ: kNumEqBands bands in series, each a bell, shelf, cut, notch, band pass
    or tilt, drawn from an analog prototype and turned digital per the EQ's Style:

    - Clean, British, Pultec: each section's poles are the analog poles (impulse
      invariance), and its zeros are picked so its magnitude matches the analog
      curve at DC, at Nyquist and at the band's frequency (the method in Martin
      Vicanek's "Matched Second Order Digital Filters"); a cut is the matching boost
      inverted. So a bell or shelf up near 20 kHz keeps its analog shape instead of
      squashing into Nyquist. Where the bilinear transform happens to fit the analog
      curve better (a notch, a resonant shelf near the top), the band uses that.
    - Digital: the bilinear transform, prewarped at the band's frequency (the RBJ
      cookbook filters). Bands 1-3 at their defaults are exactly the old 3-band EQ.

    British narrows a bell as its gain grows (Q x (1 + |gain| / 12 dB)) and gives the
    shelves a slight overshoot; Pultec gives the shelves a resonant bump and dip
    (Q x 2) and widens the bells (Q x 0.6).

    Cuts are Butterworth (6 to 96 dB/oct); their Q, above 0.71, adds a resonant
    peak at the corner. Tilt is a first-order tilt around its frequency: Gain is the
    difference between the top and the bottom (half of it each way).

    A dynamic band (bell, shelf or tilt) only reaches its Gain while the band is
    loud: a detector on the band's own frequencies (a band pass at a bell's or
    tilt's, a low pass under a low shelf's, a high pass over a high shelf's) follows
    the level with the band's Attack and Release, and the band's gain goes from 0 at
    Threshold to its full Gain 10 dB above it.

    Switching a band (or the EQ) on or off fades over 20 ms: a band with a gain
    fades its gain, the others cross-fade with the dry signal. The first block after
    prepare() or reset() starts from the settings as they are (no fade).

    Listen (DrumParams::eqListenHz > 0) replaces the EQ's output with a band pass
    of its input at that frequency, so you can hunt for a resonance by ear.

    Coefficients are recomputed every kChunk samples, and only when they change.
    Doesn't allocate in process(). */
class ParametricEq
{
public:
    static constexpr int kChunk = 32;
    /** A dynamic band's gain goes from 0 to its full Gain over this much level above Threshold. */
    static constexpr float kDynamicRangeDb = 10.0f;

    void prepare (double sampleRate);
    void reset();

    /** In place. `on` is the EQ's switch. */
    void process (float* left, float* right, int numSamples, const DrumParams& p, bool on) noexcept;

    /** The gain band `band` is applying now (a dynamic one moves), for the UI. */
    float getAppliedGainDb (int band) const noexcept;

    /** The band's filter at `gainDb` (instead of its own Gain) in a style. A gain type
        at 0 dB, or a band that's off, gives no sections. */
    static EqDesign design (const DrumParams::EqBand& band, int style, double sampleRate, float gainDb);
    static EqDesign design (const DrumParams::EqBand& band, int style, double sampleRate) { return design (band, style, sampleRate, band.gainDb); }

    /** The analog curve the band is drawn from, |H| in dB at `hz` (for tests). */
    static double analogMagnitudeDb (const DrumParams::EqBand& band, int style, double hz);

private:
    struct Band
    {
        EqDesign design;
        double z[2][EqDesign::kMaxSections][2] {}; // channel, section, state
        float mix = 0.0f;                        // 0..1: the on / off fade
        // What `design` was made from, to skip redesigning when nothing changed.
        DrumParams::EqBand made {};
        int madeStyle = -1;
        float madeGain = 0.0f;
        bool hasDesign = false;
        // Dynamics
        EqSection detector;
        double dz[2] {};
        DrumParams::EqBand detectorFor {};
        bool hasDetector = false;
        float env = 0.0f, dynAmount = 1.0f;
        std::atomic<float> appliedGainDb { 0.0f };
    };

    void runBand (Band& b, const DrumParams::EqBand& bp, int style, float* L, float* R, int n, bool on) noexcept;

    double sampleRate = 44100.0;
    bool fresh = true; // the next block starts at the settings instead of fading to them
    std::array<Band, kNumEqBands> bands;
    EqSection listen;
    double lz[2][2] {};
    float listenMix = 0.0f, listenHz = 0.0f, listenQ = 0.0f;
    float dryL[kChunk] {}, dryR[kChunk] {};
};

} // namespace drum
