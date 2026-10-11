// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "MasterChain.h"
#include "DrumVoice.h"
#include "Lfo.h"
#include "SampleData.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>

namespace drum
{

/** A hit that doesn't come from MIDI (the sidechain trigger): note 60 at a
    sample offset within the current block. */
struct TriggerHit
{
    int   sampleOffset = 0;
    float velocity     = 1.0f;
};

/** Complete synth: voice allocation, sample-accurate MIDI, master chain.
    Used for realtime playback and for offline rendering (display / export). */
class DrumEngine
{
public:
    static constexpr int maxPolyphony = 8;
    static constexpr int numVoiceSlots = 12; // headroom for voices still fading out
    /** Hit positions passed on to the reverb gate per chunk. Past this, the
        last entry is overwritten, so the gate still opens at the first hit and
        closes Gate Time after the latest. */
    static constexpr int maxHitsPerChunk = 128;
    /** While any LFO modulates something, the engine renders in chunks of at most
        this many samples and applies the LFOs at the start of each (and at each hit). */
    static constexpr int modulationBlock = 32;

    /** Allocates everything the audio thread needs. A non-finite or
        non-positive sample rate, or a block size <= 0, leaves the engine
        unprepared, and process() then outputs silence. */
    void prepare (double sampleRate, int maxBlockSize);
    /** Kills all voices and clears the master chain's filter/oversampler state. */
    void reset();

    bool isPrepared() const noexcept { return prepared; }

    /** The patch as set: LFO routes are applied on top of it while rendering. */
    void setParams (const DrumParams& p) noexcept { params = p; updateModulation(); }
    const DrumParams& getParams() const noexcept { return params; }
    /** What the voices and master chain play right now: the patch, with the LFOs applied. */
    const DrumParams& getLiveParams() const noexcept { return params.numRoutes > 0 ? modulated : params; }
    /** LFO `index`'s current output, -1..1 (for the UI). */
    float getLfoValue (int index) const noexcept { return lfoValues[(size_t) juce::jlimit (0, kNumLfos - 1, index)]; }

    EnvTables& getTables() noexcept { return tables; }
    void setTables (const EnvTables& t) noexcept { tables = t; }

    /** The samples the voices play from, for the blocks that follow. The caller
        keeps every non-null pointer alive until it passes a set without it and
        the next process() call has returned. */
    void setSamples (const SampleSet& s) noexcept { samples = s; }
    const SampleSet& getSamples() const noexcept { return samples; }

    /** Makes the noise sequence repeatable (offline renders). */
    void setSeed (uint32_t seed) noexcept { seedCounter = seed; }

    /** Starts a hit. Called outside process() (the audition pad), it lands at
        the start of the next block, for the voices and the reverb gate alike. */
    void noteOn (int midiNote, float velocity) noexcept;
    void allNotesOff() noexcept;

    /** Renders into `out`, replacing its contents. Any block size is fine.
        Outputs silence if unprepared. If the output ever contains NaN or Inf,
        the whole block is cleared and the engine is reset, so the bad values
        can't stay trapped in the voices' or master chain's filter states. */
    void process (juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi) noexcept;

    /** Same, plus extra hits interleaved sample-accurately with the MIDI.
        `hits` must be sorted by sampleOffset. Nothing here allocates, which is
        why hits come in as a plain array rather than being merged into a
        MidiBuffer. */
    void process (juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi,
                  const TriggerHit* hits, int numHits) noexcept;

    int getLatencySamples() const noexcept { return master.getLatencySamples(); }
    /** See MasterChain::setPreClipCapture() (offline renders only). */
    void setPreClipCapture (juce::AudioBuffer<float>* buffer) noexcept { master.setPreClipCapture (buffer); }
    int getClipperLatencySamples() const noexcept { return master.getClipperLatencySamples(); }
    /** See MasterChain::setCompCapture() / getCompLeadSamples() (offline renders only). */
    void setCompCapture (int n, juce::AudioBuffer<float>* buffer) noexcept { master.setCompCapture (n, buffer); }
    int getCompLeadSamples (int n) const noexcept { return master.getCompLeadSamples (n); }
    /** See MasterChain::getPreClipLeadSamples(). */
    int getPreClipLeadSamples() const noexcept { return master.getPreClipLeadSamples(); }
    /** See MasterChain::getCompReductionDb(). */
    float getCompReductionDb (int n) const noexcept { return master.getCompReductionDb (n); }
    /** The gain EQ band `band` applies now (a dynamic band moves), for the UI. */
    float getEqAppliedGainDb (int band) const noexcept { return master.getEqAppliedGainDb (band); }

    /** How long one dry hit lasts: Length, plus the largest positive layer
        Offset (a late layer plays its whole Length after it starts). Doesn't
        include the reverb tail or the master chain's latency. */
    static double hitLengthSeconds (const DrumParams& p) noexcept
    {
        return ((double) p.lengthMs + (double) DrumVoice::latestOffsetMs (p)) * 0.001;
    }

    /** For tests: bypasses the reverb completely (see MasterChain). */
    void setReverbEnabled (bool shouldBeEnabled) noexcept { master.setReverbEnabled (shouldBeEnabled); }
    bool isReverbIdle() const noexcept { return master.isReverbIdle(); }

    /** Number of voices started since construction (MIDI and trigger hits). */
    int getNumVoicesStarted() const noexcept { return voicesStarted; }

    /** Normalised position (0..1) of the most recent hit, or -1 if silent. */
    float getPlayhead() const noexcept;

private:
    void handleMidi (const juce::MidiMessage& m) noexcept;
    /** Renders the voices (and moves the LFOs on) over part of the current chunk. */
    void renderVoices (int start, int numSamples) noexcept;
    /** Re-reads the LFOs and applies them to `modulated`. */
    void updateModulation() noexcept;
    uint32_t nextSeed() noexcept;

    bool prepared = false;
    double sampleRate = 44100.0;
    int maxBlock = 512;

    DrumParams params, modulated;
    LfoBank lfos;
    std::array<float, kNumLfos> lfoValues {};
    EnvTables tables;
    SampleSet samples;
    std::array<DrumVoice, numVoiceSlots> voices;
    int newestVoice = -1;
    int voicesStarted = 0;
    uint32_t seedCounter = 0xC0FFEEu;

    MasterChain master;
    juce::AudioBuffer<float> scratch;

    // Where hits start in the current chunk, for the reverb gate. chunkOffset
    // is the position noteOn() records: 0 outside process().
    std::array<int, maxHitsPerChunk> chunkHits {};
    int numChunkHits = 0;
    int chunkOffset = 0;
};

} // namespace drum
