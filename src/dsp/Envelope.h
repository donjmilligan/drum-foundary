// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <array>
#include <vector>

namespace drum
{

/** The drawable envelopes: a pitch and a level curve for each of the three core
    layers, then each extra layer's own pair. All share the global Length time base
    (each layer's own Offset shifts where it reads them). The first three keep their
    original indices, so old sessions and presets map straight across. */
enum EnvId
{
    envPitch = 0,     // Body pitch
    envBody,          // Body level
    envNoise,         // Noise level
    envNoisePitch,    // Noise pitch: transposes the noise (filters, Metallic, sample)
    envClickPitch,    // Click pitch: transposes the click (tone, sample)
    envClick,         // Click level: multiplies the click's own decay
    envLayer1Pitch,   // Extra layer 1's own pitch: transposes the layer (semitones from its base)
    envLayer1Level,   // Extra layer 1's own level
    envLayer2Pitch,
    envLayer2Level,
    envLayer3Pitch,
    envLayer3Level,
    numEnvs
};

/** Extra layers with their own curves (kNumExtraLayers in Parameters.h). */
inline constexpr int kNumLayerEnvelopePairs = 3;
inline constexpr int numCoreEnvs = envLayer1Pitch;

/** Extra layer `layer`'s (0-based) own pitch / level curve. */
inline constexpr int layerOwnPitchEnvelope (int layer) noexcept { return envLayer1Pitch + 2 * layer; }
inline constexpr int layerOwnLevelEnvelope (int layer) noexcept { return envLayer1Level + 2 * layer; }
/** The extra layer (0-based) that owns curve `env`, or -1 for a core layer's curve. */
inline constexpr int layerOfEnvelope (int env) noexcept { return env >= envLayer1Pitch && env < numEnvs ? (env - envLayer1Pitch) / 2 : -1; }

inline constexpr bool isPitchEnvelope (int env) noexcept
{
    return env == envPitch || env == envNoisePitch || env == envClickPitch
        || (env >= envLayer1Pitch && env < numEnvs && (env - envLayer1Pitch) % 2 == 0);
}

/** A breakpoint. x is normalised 0..1; y is in the envelope's units (see
    EnvRange); `curve` (-1..1) bends the segment that starts at this point
    (0 = straight line). */
struct EnvPoint
{
    float x = 0.0f;
    float y = 0.0f;
    float curve = 0.0f;
};

/** What an envelope's y means and where it may go.
    - Pitch (Body; Noise, Click and the extra layers are the same but relative to
      their own base, e.g. Click Tone): semitones relative to Tune. The last point is pinned to 0, so a
      curve always lands on Tune; the rest may go anywhere in the range (the
      display shows it as absolute Hz, 20 Hz to 16 kHz).
    - Body / Noise: linear gain. 1 = 0 dB; above 1 (up to +12 dB) drives the
      output into the clipper. */
struct EnvRange
{
    float lo = 0.0f, hi = 1.0f;
    bool pinEndToZero = false;
};

inline constexpr float kPitchRangeSt = 120.0f;   // +-10 octaves around Tune
inline constexpr float kMaxEnvGain   = 4.0f;     // +12 dB

EnvRange envelopeRange (int envId) noexcept;

/** Editable breakpoint curve. Lives on the message thread; the audio thread
    only ever sees the baked EnvTables. First point is pinned to x = 0 and the
    last to x = 1. */
class CurveEnvelope
{
public:
    static constexpr int maxPoints = 48;

    CurveEnvelope() = default;
    explicit CurveEnvelope (std::vector<EnvPoint> pts, EnvRange range = {});

    const EnvRange& getRange() const noexcept { return range; }

    const std::vector<EnvPoint>& getPoints() const noexcept { return points; }
    int size() const noexcept { return (int) points.size(); }

    float evaluate (float x) const noexcept;
    void renderTable (float* dest, int size) const noexcept;

    /** Inserts a point, keeping x order. Returns its index, or -1 if full. */
    int addPoint (float x, float y);
    bool removePoint (int index);
    void movePoint (int index, float x, float y);
    void setCurve (int index, float curve);

    /** Moves every point up or down by deltaY (clamped to the range), except a
        pinned end. The editor uses it on the pitch curve when the end point is
        dragged: Tune moves by the drag, and the other points move back by the
        same amount so they stay where they were in absolute pitch. */
    void offsetAll (float deltaY);

    /** y value at the middle of segment `index` (for drawing the bend handle). */
    float segmentMidY (int index) const noexcept;

    juce::ValueTree toValueTree (const juce::Identifier& type) const;
    /** Reads points saved by toValueTree(); the result takes `fallback`'s range. */
    static CurveEnvelope fromValueTree (const juce::ValueTree& tree, const CurveEnvelope& fallback);

    /** Shapes a 0..1 ramp. curve < 0 bows up early (fast then slow), > 0 late. */
    static float shape (float t, float curve) noexcept;

    bool operator== (const CurveEnvelope& other) const noexcept;

private:
    void sanitise();

    std::vector<EnvPoint> points { { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } };
    EnvRange range;
};

inline constexpr int kEnvTableSize = 4096;

/** Baked, fixed-size envelope data the voices read from. Plain memory so it can
    be copied between threads without allocation. */
struct EnvTables
{
    std::array<std::array<float, kEnvTableSize>, numEnvs> data {};

    inline float lookup (int env, float x) const noexcept
    {
        // Clamp written so that NaN lands on 0 (jlimit would pass it through and
        // the int cast below would index out of bounds). +/-inf clamp as usual.
        const float pos = (x > 0.0f ? juce::jmin (x, 1.0f) : 0.0f) * (float) (kEnvTableSize - 1);
        const int i = juce::jmin ((int) pos, kEnvTableSize - 2);
        const float frac = pos - (float) i;
        const auto& t = data[(size_t) env];
        return t[(size_t) i] + frac * (t[(size_t) i + 1] - t[(size_t) i]);
    }

    void bake (int env, const CurveEnvelope& e) noexcept { e.renderTable (data[(size_t) env].data(), kEnvTableSize); }
};

/** The envelopes a freshly initialised patch starts with. */
CurveEnvelope defaultEnvelope (EnvId id);

const char* envelopeName (EnvId id);

} // namespace drum
