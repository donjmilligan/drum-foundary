// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "DrumEngine.h"
#include <cmath>
#include <limits>

namespace drum
{

namespace
{
    /** True if any of the first numSamples of any channel is NaN or Inf.
        Relies on IEEE semantics: breaks silently under -ffast-math. */
    bool containsNonFinite (const juce::AudioBuffer<float>& buffer, int numSamples) noexcept
    {
        for (int c = 0; c < buffer.getNumChannels(); ++c)
        {
            const float* x = buffer.getReadPointer (c);
            for (int i = 0; i < numSamples; ++i)
                if (! std::isfinite (x[i]))
                    return true;
        }
        return false;
    }
}

void DrumEngine::prepare (double sr, int maxBlockSize)
{
    // A bad rate would design filters at NaN Hz and a bad block size would
    // leave buffers empty, so refuse both and stay unprepared (silent).
    prepared = false;
    if (! std::isfinite (sr) || sr <= 0.0 || maxBlockSize <= 0)
        return;

    sampleRate = sr;
    maxBlock = juce::jmax (16, maxBlockSize);
    scratch.setSize (2, maxBlock, false, true, false);

    for (auto& v : voices)
        v.prepare (sr);

    lfos.prepare (sr);
    master.prepare (sr, maxBlock);
    reset();
    prepared = true;
}

void DrumEngine::reset()
{
    for (auto& v : voices)
        v.kill();
    newestVoice = -1;
    master.reset();
    numChunkHits = 0;
    chunkOffset = 0;
}

uint32_t DrumEngine::nextSeed() noexcept
{
    seedCounter = seedCounter * 1664525u + 1013904223u;
    return seedCounter;
}

void DrumEngine::updateModulation() noexcept
{
    if (params.numRoutes <= 0)
        return;

    lfos.getValues (params, lfoValues.data());
    modulated = params;
    applyModulation (modulated, params, lfoValues.data());
}

void DrumEngine::noteOn (int midiNote, float velocity) noexcept
{
    const uint32_t seed = nextSeed();

    // The LFOs restart with the hit, so it starts with their values at its start.
    lfos.retrigger (params, seed);
    updateModulation();
    const auto& live = getLiveParams();

    if (live.choke)
        for (auto& v : voices)
            if (v.isActive())
                v.fadeOut();

    int sounding = 0;
    int oldest = -1;
    for (int i = 0; i < numVoiceSlots; ++i)
    {
        const auto& v = voices[(size_t) i];
        if (v.isActive() && ! v.isFading())
        {
            ++sounding;
            if (oldest < 0 || v.getAgeSamples() > voices[(size_t) oldest].getAgeSamples())
                oldest = i;
        }
    }
    if (sounding >= maxPolyphony && oldest >= 0)
        voices[(size_t) oldest].fadeOut();

    int slot = -1;
    for (int i = 0; i < numVoiceSlots; ++i)
        if (! voices[(size_t) i].isActive()) { slot = i; break; }

    if (slot < 0)
    {
        // Every slot is busy (rapid-fire rolls): reuse the oldest outright.
        slot = 0;
        for (int i = 1; i < numVoiceSlots; ++i)
            if (voices[(size_t) i].getAgeSamples() > voices[(size_t) slot].getAgeSamples())
                slot = i;
    }

    voices[(size_t) slot].start (midiNote, velocity, seed, live);
    newestVoice = slot;
    ++voicesStarted;

    if (numChunkHits < maxHitsPerChunk)
        chunkHits[(size_t) numChunkHits++] = chunkOffset;
    else
        chunkHits[(size_t) maxHitsPerChunk - 1] = chunkOffset;
}

void DrumEngine::allNotesOff() noexcept
{
    for (auto& v : voices)
        if (v.isActive())
            v.fadeOut();
}

void DrumEngine::handleMidi (const juce::MidiMessage& m) noexcept
{
    if (m.isNoteOn())
        noteOn (m.getNoteNumber(), m.getFloatVelocity());
    else if (m.isAllNotesOff() || m.isAllSoundOff())
        allNotesOff();
}

void DrumEngine::renderVoices (int start, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    float* L = scratch.getWritePointer (0, start);
    float* R = scratch.getWritePointer (1, start);
    const auto& live = getLiveParams();
    for (auto& v : voices)
        if (v.isActive())
            v.render (L, R, numSamples, live, tables, samples);

    lfos.advance (params, numSamples);
}

void DrumEngine::process (juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi) noexcept
{
    process (out, midi, nullptr, 0);
}

void DrumEngine::process (juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi,
                           const TriggerHit* hits, int numHits) noexcept
{
    // e.g. a host calling processBlock before prepareToPlay: scratch and the
    // oversampler aren't allocated yet.
    if (! prepared)
    {
        out.clear();
        numChunkHits = 0;
        return;
    }

    const int total = out.getNumSamples();
    const int outChannels = out.getNumChannels();
    auto it = midi.cbegin();
    const auto end = midi.cend();
    int hit = 0;
    if (hits == nullptr)
        numHits = 0;

    // With LFOs routed, short chunks, so the modulation moves smoothly.
    const int chunkSize = params.numRoutes > 0 ? juce::jmin (maxBlock, modulationBlock) : maxBlock;

    for (int chunkStart = 0; chunkStart < total; chunkStart += chunkSize)
    {
        const int n = juce::jmin (chunkSize, total - chunkStart);
        const int chunkEnd = chunkStart + n;
        const bool lastChunk = chunkEnd >= total;

        scratch.clear (0, n);
        updateModulation();
        int cursor = chunkStart;

        // Merge the two event streams in time order. On a tie MIDI goes first.
        for (;;)
        {
            const bool haveMidi = it != end;
            const bool haveHit = hit < numHits;
            if (! haveMidi && ! haveHit)
                break;

            const int midiPos = haveMidi ? (*it).samplePosition : std::numeric_limits<int>::max();
            const int hitPos = haveHit ? hits[hit].sampleOffset : std::numeric_limits<int>::max();
            const bool takeMidi = midiPos <= hitPos;
            const int pos = takeMidi ? midiPos : hitPos;

            if (pos >= chunkEnd && ! lastChunk)
                break;

            const int at = juce::jlimit (cursor, chunkEnd, pos);
            renderVoices (cursor - chunkStart, at - cursor);
            cursor = at;
            chunkOffset = at - chunkStart;

            if (takeMidi)
            {
                handleMidi ((*it).getMessage());
                ++it;
            }
            else
            {
                noteOn (60, hits[hit].velocity);
                ++hit;
            }
        }
        renderVoices (cursor - chunkStart, chunkEnd - cursor);

        master.process (scratch, n, getLiveParams(), chunkHits.data(), numChunkHits);
        numChunkHits = 0;
        chunkOffset = 0;

        // Once a NaN/Inf reaches an IIR state it recirculates forever, so drop
        // the whole block (and any MIDI left in it) and start again from silence.
        if (containsNonFinite (scratch, n))
        {
            reset();
            out.clear();
            return;
        }

        if (outChannels >= 2)
        {
            out.copyFrom (0, chunkStart, scratch, 0, 0, n);
            out.copyFrom (1, chunkStart, scratch, 1, 0, n);
            for (int c = 2; c < outChannels; ++c)
                out.clear (c, chunkStart, n);
        }
        else if (outChannels == 1)
        {
            out.clear (0, chunkStart, n);
            out.addFrom (0, chunkStart, scratch, 0, 0, n, 0.5f);
            out.addFrom (0, chunkStart, scratch, 1, 0, n, 0.5f);
        }
    }
}

float DrumEngine::getPlayhead() const noexcept
{
    if (newestVoice >= 0 && voices[(size_t) newestVoice].isActive())
        return voices[(size_t) newestVoice].getPosition();
    return -1.0f;
}

} // namespace drum
