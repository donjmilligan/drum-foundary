// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <memory>

namespace drum
{

/** Every place a sample can be loaded: the three core layers (used when their
    style / type is "Sample") and the extra layers. */
enum SampleSlot
{
    slotBody = 0,
    slotNoise,
    slotClick,
    slotLayer1,
    slotLayer2,
    slotLayer3,
    numSampleSlots
};

/** A decoded, immutable sample. Created on a background / message thread and
    shared by pointer; the audio thread never allocates or frees one. */
struct SampleData
{
    juce::AudioBuffer<float> audio; // 1 or 2 channels, at `sampleRate`
    double sampleRate = 44100.0;
    juce::String path;              // absolute path it was loaded from
    juce::String name;              // file name without extension, for display
};

using SamplePtr = std::shared_ptr<const SampleData>;

/** What the voices see during one block (or one offline render). Pointers stay
    valid for at least that long; nullptr means the slot is empty. Plain data, so
    it can be copied on the audio thread. */
struct SampleSet
{
    std::array<const SampleData*, numSampleSlots> slots {};

    const SampleData* get (int slot) const noexcept { return slots[(size_t) slot]; }
};

} // namespace drum
