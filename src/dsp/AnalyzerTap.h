// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <vector>

namespace drum
{

/** Lock-free single-producer / single-consumer tap of the plug-in's final
    output (stereo; a mono output is sent as two equal channels), for the
    spectrum analyzer and its output meter. The audio thread push()es, the UI
    pull()s. If the UI falls behind (or no editor is open), new audio is dropped
    rather than blocking; the analyzer only ever needs the most recent window,
    so a consumer that has been away should discard() first. */
class AnalyzerTap
{
public:
    static constexpr int capacity = 1 << 15;

    AnalyzerTap() : fifo (capacity), left ((size_t) capacity, 0.0f), right ((size_t) capacity, 0.0f) {}

    /** Audio thread. Takes the first two channels (or the one channel twice). */
    void push (const juce::AudioBuffer<float>& buffer, int numSamples) noexcept
    {
        const int chans = juce::jmin (2, buffer.getNumChannels());
        if (chans == 0 || numSamples <= 0)
            return;

        const float* l = buffer.getReadPointer (0);
        const float* r = buffer.getReadPointer (chans - 1);
        const auto scope = fifo.write (juce::jmin (numSamples, fifo.getFreeSpace()));
        auto copy = [&] (int start, int size, int srcOffset)
        {
            std::copy (l + srcOffset, l + srcOffset + size, left.begin() + start);
            std::copy (r + srcOffset, r + srcOffset + size, right.begin() + start);
        };
        copy (scope.startIndex1, scope.blockSize1, 0);
        copy (scope.startIndex2, scope.blockSize2, scope.blockSize1);
    }

    /** UI thread. Reads up to maxSamples per channel, oldest first; returns how many. */
    int pull (float* destLeft, float* destRight, int maxSamples) noexcept
    {
        const auto scope = fifo.read (juce::jmin (maxSamples, fifo.getNumReady()));
        auto copy = [&] (int start, int size, int destOffset)
        {
            std::copy (left.begin() + start, left.begin() + start + size, destLeft + destOffset);
            std::copy (right.begin() + start, right.begin() + start + size, destRight + destOffset);
        };
        copy (scope.startIndex1, scope.blockSize1, 0);
        copy (scope.startIndex2, scope.blockSize2, scope.blockSize1);
        return scope.blockSize1 + scope.blockSize2;
    }

    /** UI thread. Reads up to maxSamples of the mono sum (L + R) / 2, oldest first. */
    int pull (float* dest, int maxSamples) noexcept
    {
        const auto scope = fifo.read (juce::jmin (maxSamples, fifo.getNumReady()));
        auto copy = [&] (int start, int size, int destOffset)
        {
            for (int i = 0; i < size; ++i)
                dest[destOffset + i] = 0.5f * (left[(size_t) (start + i)] + right[(size_t) (start + i)]);
        };
        copy (scope.startIndex1, scope.blockSize1, 0);
        copy (scope.startIndex2, scope.blockSize2, scope.blockSize1);
        return scope.blockSize1 + scope.blockSize2;
    }

    /** UI thread. Drops everything waiting (stale audio from while nobody was reading). */
    void discard() noexcept
    {
        const auto scope = fifo.read (fifo.getNumReady());
        juce::ignoreUnused (scope);
    }

    /** Sample rate of the pushed audio (set in prepareToPlay). */
    std::atomic<double> sampleRate { 44100.0 };

private:
    juce::AbstractFifo fifo;
    std::vector<float> left, right;
};

} // namespace drum
