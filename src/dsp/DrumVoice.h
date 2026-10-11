// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "../Parameters.h"
#include "BodyGen.h"
#include "DspUtils.h"
#include "Envelope.h"
#include "NoiseClickGen.h"
#include "SampleData.h"
#include "SamplePlayer.h"
#include "StyleInfo.h"
#include <algorithm>

namespace drum
{

/** One drum hit. Renders body + noise + click + the extra layers and adds the
    result into a stereo buffer. Hits are one-shots: note-offs are ignored.

    Each layer runs on its own clock, shifted by its Offset: + starts it later
    (silence first), - starts it early, so at the hit it is already that far
    into its timeline (oscillators, noise, click, sample position and its
    envelope alike) and the part before the hit is lost. A cut layer fades in
    over kCutFadeMs (0.5 ms) at the hit so the cut doesn't click. Every layer
    lasts Length on its own clock; the voice ends when the latest one does
    (Length + the largest positive Offset, see latestOffsetMs()), and a layer
    that ends earlier fades out on its own over the last 2 ms.

    Solo: when any layer is soloed only the soloed ones sound, and mute (a
    layer's on switch) wins over solo. Mute and solo are read each block and
    move with the voice's 2.5 ms gain smoothing. A layer silenced by another's
    solo keeps running, so unsoloing brings it back in time, and still feeds
    the Wires coupling: a soloed Noise sounds as it does in the mix.

    The body style, noise style, click type, the Offsets and each extra
    layer's on switch, source, noise style, click type and body style are
    fixed when the hit starts, so changing them affects the next hit. Every other parameter is
    read each block. See StyleInfo.h for what the knobs mean in each style.

    Pitch: the pitch curve is in semitones over Tune. Every oscillator's
    frequency is kept between 0 Hz (it holds still) and 0.45 x the sample rate;
    partials that would pass that are muted, and the waveshaped styles
    (Classic's Shape, the 909's Drive) fade to a sine as the pitch goes past
    ~1.5 kHz (at 48 kHz), so extreme curves stay alias-free. The Noise and
    Click pitch curves transpose those layers (see StyleInfo.h), read at the
    layer's own clock like its level curve; filters and the like follow them
    every 16 samples.

    Amplitude envelopes (Body, Noise, Click) go up to 4 (+12 dB) and are
    applied unclamped; only the controls that they also steer (Noise Color's
    darkening, the FM index) stop following them above 1. The Click curve
    multiplies the click's own decay.

    Phase invert flips a layer's output (not what Wires coupling hears from
    the body). It is read each block and swings through zero over 2.5 ms. */
class DrumVoice
{
public:
    static constexpr float kCutFadeMs = 0.5f;   // fade-in where a negative Offset cuts into a layer

    void prepare (double sampleRate);
    void start (int midiNote, float vel, uint32_t seed, const DrumParams& p);

    /** How long after the hit the latest layer that is on starts, in ms: the
        largest positive Offset, or 0. A hit lasts Length plus this. */
    static float latestOffsetMs (const DrumParams& p) noexcept;

    /** Quick fade to silence (voice stealing, choke, all-notes-off). */
    void fadeOut() noexcept { fading = true; }
    void kill() noexcept { active = false; }

    bool isActive() const noexcept { return active; }
    bool isFading() const noexcept { return fading; }
    double getAgeSamples() const noexcept { return pos; }
    /** Position since the hit relative to Length (past 1 while a late layer plays on). */
    float getPosition() const noexcept { return (float) (juce::jmax (0.0, pos) / lengthSamples); }

    /** Adds the next numSamples into outL / outR. `samples` is the engine's
        current set: sample pointers are looked up afresh on every call and never
        kept, so the caller may swap them between blocks. */
    void render (float* outL, float* outR, int numSamples, const DrumParams& p, const EnvTables& env,
                 const SampleSet& samples) noexcept;

private:
    /** The voice renders in slices of at most this many samples: first the
        per-sample gain and envelope values, then each layer in its own loop. */
    static constexpr int sliceSize = 64;

    /** A layer's own clock: its timeline runs `offset` samples behind the
        voice's (+ = it starts late, - = early, with its start cut off). */
    struct Clock
    {
        double offset = 0.0;
        bool cut = false;       // starts early: fades in at the hit
        bool endsFirst = false; // ends before the voice, so it fades out on its own
    };

    /** A 0..1 gain that follows its target at the voice's gain smoothing rate
        (mute and solo). */
    struct Fader
    {
        float value = 1.0f, target = 1.0f;

        bool isSilent() const noexcept { return value <= 0.0f && target <= 0.0f; }
        void set (bool on) noexcept { value = target = on ? 1.0f : 0.0f; }
        /** Writes the next n values into dst. */
        void fill (float* dst, int n, float step) noexcept
        {
            if (juce::exactlyEqual (value, target))
            {
                std::fill (dst, dst + n, value);
                return;
            }
            for (int i = 0; i < n; ++i)
            {
                value += juce::jlimit (-step, step, target - value);
                dst[i] = value;
            }
        }
    };

    /** A layer's mute (its on switch) and solo gains, and its polarity (+1, or
        -1 when inverted). */
    struct Faders
    {
        Fader mute, solo, polarity;
    };

    /** Per-voice state of one extra layer. */
    struct Layer
    {
        bool on = false;
        int source = 0;
        float phase = 0;
        double smpPos = 0.0;
        dsp::Svf hpL, hpR, lpL, lpR;    // Low / High Cut (a Noise layer uses its generator's own)
        NoiseGen noise;
        ClickGen click;
        BodyGen body;
        BodyGen::Settings bodySettings;     // Body source: its style's fixed Overtone / Ratio / Head Modes
        Clock clock;
        Faders faders;
    };

    /** Values worked out once per render() call and shared by the slices. */
    struct Block
    {
        float invLength = 0, endFadeSamples = 1, lenF = 1, cutFadeSamples = 1;
        float baseInc = 0;
    };

    /** Everything one slice of up to n samples needs: the voice gain, then each
        layer into its slice buffer. Returns how many samples the voice still
        plays (fewer than n when it ends in this slice). */
    int renderSlice (int n, const DrumParams& p, const EnvTables& env, const SampleSet& samples) noexcept;

    /** Fills ctlGain for up to n samples. Returns how many samples the voice
        still plays. */
    int computeControl (int n) noexcept;

    /** A layer's clock over the slice: x[i] is its envelope position and g[i]
        its gain from the clock (the cut fade-in and its own end fade), for i
        from the returned index (where it starts) to n. Returns n when the
        layer doesn't play in this slice. */
    int runClock (const Clock& c, int n, float* x, float* g) const noexcept;

    /** Mute, solo and polarity gains for the slice into fadeMute / fadeSolo /
        fadePol. Returns false when the layer is muted and has faded out (it isn't
        rendered then). */
    bool runFaders (Faders& f, int n) noexcept;

    Clock makeClock (float offsetMs) const noexcept;

    // Each writes samples [from, n) of its layer into the slice buffers.
    void renderBody (int from, int n, const DrumParams& p, const SampleSet& samples) noexcept;
    void renderBodySample (int from, int n, const DrumParams& p, const SampleSet& samples) noexcept;
    void renderNoise (int from, int n, const DrumParams& p, const SampleSet& samples) noexcept;
    void renderClickSlice (int from, int n, const DrumParams& p, const SampleSet& samples) noexcept;
    void renderLayers (int n, const DrumParams& p, const EnvTables& env, const SampleSet& samples) noexcept;

    float sampleRate = 44100.0f;
    bool active = false, fading = false;
    double pos = 0.0, lengthSamples = 1.0;
    double lateSamples = 0.0;                   // the latest layer's Offset: the voice runs Length + this
    float gain = 1.0f, gainSlew = 0.001f;
    bool anySolo = false;

    float noteRatio = 1.0f, velGain = 1.0f, velocity = 1.0f;
    int bodyStyle = 0;

    // Body ------------------------------------------------------------------------
    BodyGen body;
    double bodySmpPos = 0.0;

    // Noise, click and extra layers ---------------------------------------------------
    NoiseGen noise;
    ClickGen click;
    std::array<Layer, kNumExtraLayers> layers;

    Clock bodyClock, noiseClock, clickClock;
    Faders bodyFaders, noiseFaders, clickFaders;

    Block blk;

    // Slice buffers ---------------------------------------------------------------
    float ctlGain[sliceSize] {}, ctlPitch[sliceSize] {}, ctlBody[sliceSize] {}, ctlNoise[sliceSize] {};
    float ctlNoisePitch[sliceSize] {}, ctlClickPitch[sliceSize] {}, ctlClick[sliceSize] {};
    float clockX[sliceSize] {}, clockGain[sliceSize] {};     // the layer being rendered: envelope position, clock gain
    float fadeMute[sliceSize] {}, fadeSolo[sliceSize] {}, fadePol[sliceSize] {}; // the layer being rendered: mute, solo, polarity
    float bodyL[sliceSize] {}, bodyR[sliceSize] {}, coupleSig[sliceSize] {}; // coupleSig: the body oscillator for Coupling, as it sounds
    float noiseL[sliceSize] {}, noiseR[sliceSize] {};
    float clickL[sliceSize] {}, clickR[sliceSize] {};
    float layerL[sliceSize] {}, layerR[sliceSize] {};
    float layerEnv[sliceSize] {}, layerTmpL[sliceSize] {}, layerTmpR[sliceSize] {}; // one extra layer's envelope / noise
    float layerPitch[sliceSize] {}, layerCouple[sliceSize] {};  // one extra layer's pitch curve; a Body layer's (unused) coupling
    bool bodyStereo = false, clickStereo = false;
    bool layersWritten = false;                 // renderLayers() wrote layerL / layerR this slice
};

} // namespace drum
