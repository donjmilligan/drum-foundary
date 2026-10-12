// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <memory>

namespace drum
{

/** The Natural reverb engine: a room built as an impulse response and convolved, wet
    only. There are no recirculating delays, so nothing rings at the delays' pitches
    (what makes small algorithmic rooms sound metallic): the room is shaped noise.

    The impulse (makeImpulse()) is the pre-delay, then early reflections (Early: a few
    discrete, softened taps over the first 5-45 ms, later and sparser with Size), then
    the late tail: noise whose echo density grows with the square of time, as in a real
    room (sparse and grainy at first, smooth once the room is "mixed": sooner with more
    Diffusion, later with more Size), rising over 3-33 ms (Size), in four bands
    (crossovers at 350 Hz, 1.8 kHz and 6 kHz) that each decay at their own RT60: Decay,
    with the lows a little longer and the highs shorter with Damping. Width blends each
    side's noise from shared (0) to independent (1). It's seeded, so the same settings
    always give the same room.

    Convolution is JUCE's, non-uniformly partitioned with no latency. A new room is
    built when the settings change: on a background thread in the plug-in (setAsync(),
    the convolver crossfades to it when it's ready), or on the spot for offline renders,
    so a render always has the room its settings describe.

    prepare() allocates. process() and setSettings() don't, in async mode (in sync mode
    setSettings() builds the room, which allocates). */
class NaturalReverb
{
public:
    struct Settings
    {
        float size = 0.6f;          // 0..1
        float decaySeconds = 2.5f;  // RT60 of the mid bands
        float preDelayMs = 10.0f;
        float diffusion = 0.7f;     // 0..1: how soon the tail is dense
        float width = 1.0f;         // 0 = mono .. 1 = fully decorrelated
        float early = 0.2f;         // 0..1: level of the early reflections
        float damping = 0.4f;       // 0..1: how much sooner the highs die

        bool operator== (const Settings& o) const noexcept;
        bool operator!= (const Settings& o) const noexcept { return ! (*this == o); }
    };

    static constexpr int maxBlock = 256;
    static constexpr double maxTailSeconds = 8.0; // the late part of the impulse is at most this long

    NaturalReverb();
    ~NaturalReverb();

    void prepare (double sampleRate);
    /** Clears the convolver's state (keeps the room). */
    void reset() noexcept;

    /** True: build rooms on a background thread (the plug-in's audio thread). False (the
        default): build them in setSettings(), for offline renders. Call off the audio thread. */
    void setAsync (bool shouldBeAsync);

    /** The room to use; a new one is built if these differ from the last. */
    void setSettings (const Settings& s);

    /** Wet only, numSamples <= maxBlock. Silent until the first room is in place. */
    void process (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;

    /** Call once a block while process() isn't being called (nothing to reverberate):
        picks up a finished room and moves it into place, so the next hit has it. The
        convolver only does that while it runs, so this runs it on one sample of silence
        until the room is in. Only while idle: the convolver's history is silence then. */
    void serviceWhileIdle() noexcept;

    /** How long the room's impulse is, in samples (0 before the first). */
    int getImpulseLength() const noexcept { return impulseLength; }

    /** The room's impulse response for these settings: stereo, at `sampleRate`, the
        pre-delay first, normalised to unit energy per side. */
    static juce::AudioBuffer<float> makeImpulse (const Settings& s, double sampleRate);

private:
    class Builder;
    void pickUpRoom() noexcept;

    juce::SharedResourcePointer<juce::dsp::ConvolutionMessageQueue> queue;
    std::unique_ptr<juce::dsp::Convolution> convolution;
    std::unique_ptr<Builder> builder;   // async mode, once prepared
    std::unique_ptr<juce::AudioBuffer<float>> heldBuffer; // a room's emptied buffer, waiting to go back to the Builder
    double sampleRate = 0.0;
    bool async = false;
    Settings current;
    bool hasSettings = false;
    int impulseLength = 0;
    std::array<float, 1> silence {}, discard {};
};

} // namespace drum
