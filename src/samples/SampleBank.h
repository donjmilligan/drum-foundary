// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "../dsp/SampleData.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include <array>
#include <atomic>
#include <optional>
#include <vector>

namespace drum
{

/** Display name for a slot: "Body", "Noise", "Click", "Layer 1".. */
juce::String sampleSlotName (SampleSlot slot);
/** Name a slot is saved under in the state: "body", "noise", "click", "layer1".. */
juce::String sampleSlotKey (SampleSlot slot);

/** The imported samples, one per SampleSlot, plus the browser's audition.

    Files are decoded on a background thread (loads the user asks for) or on the
    calling thread (restore(), for host sessions and presets). Every change is
    published to the audio thread without blocking it: the audio thread only
    try-locks a table of raw pointers, the same way the processor hands over its
    envelope tables. A replaced sample is never freed on the audio thread: it's
    kept on a retired list until the audio thread has picked up a set without it,
    and released later on another thread.

    Threads:
    - load(), clear(), preview(), stopPreview(): message thread.
    - restore(), get(), getPath(), isMissing(), isLoading(), getGeneration(): any
      thread except the audio thread.
    - beginBlock(), mixPreview(): the audio thread only. */
class SampleBank : private juce::AsyncUpdater,
                   private juce::Timer
{
public:
    /** Longer files are cut to this. */
    static constexpr double maxSeconds = 10.0;
    /** Level of the audition, relative to the file. */
    static constexpr float previewGain = 0.5f;

    SampleBank();
    ~SampleBank() override;

    //==============================================================================
    /** Decodes `file` in the background and puts it in `slot`. On success,
        onUserLoad (slot) follows on the message thread. On failure (not audio,
        unreadable) the slot keeps what it had. A later load, clear or restore of
        the same slot cancels this one. */
    void load (SampleSlot slot, const juce::File& file);
    void clear (SampleSlot slot);

    /** Replaces every slot at once, decoding on the calling thread. An empty path
        empties the slot; a path that can't be loaded keeps its path and is marked
        missing. Cancels any load in progress. Not for the audio thread.
        With `keepUnchanged`, a slot that already holds audio from the same path
        keeps it instead of decoding the file again (for undo; a host recall or a
        preset load reads the file afresh). */
    void restore (const std::array<juce::String, numSampleSlots>& paths, bool keepUnchanged = false);

    /** Called on the message thread after a load() has been installed. */
    std::function<void (SampleSlot)> onUserLoad;

    SamplePtr get (SampleSlot slot) const;
    /** The slot's file, even if it couldn't be loaded. Empty if the slot is empty. */
    juce::String getPath (SampleSlot slot) const;
    /** True if the slot has a path but no audio (the file is gone or unreadable). */
    bool isMissing (SampleSlot slot) const;
    /** True while a load() into the slot is being decoded. */
    bool isLoading (SampleSlot slot) const;
    /** True from a load() being installed until its onUserLoad has been called. */
    bool hasPendingUserLoads() const;
    /** Bumps whenever any slot changes (load, clear, restore, missing). */
    int getGeneration() const noexcept { return generation.load(); }

    //==============================================================================
    /** Plays `file` as it is (resampled to the output rate, at previewGain),
        replacing any audition that's playing. Decoded in the background. */
    void preview (const juce::File& file);
    void stopPreview();
    /** True from preview() until the file has played out or stopPreview(). */
    bool isPreviewing() const noexcept;

    //==============================================================================
    /** Blocks until the background decoder has nothing left to do, then runs
        the pending onUserLoad callbacks on the calling thread. For tests and the
        dev tool, which have no message loop. Returns false on timeout. */
    bool waitForPendingLoads (int timeoutMs);

    /** Frees replaced samples the audio thread no longer uses. Runs on a timer
        and after every change; public for tests. */
    void releaseRetired();

    //==============================================================================
    /** The output rate the audition is resampled to. Call from prepareToPlay(). */
    void prepare (double outputSampleRate) noexcept { outputRate = outputSampleRate; }

    /** Audio thread, at the start of every block: picks up changes if it can do so
        without waiting, and returns the set for this block. Pointers in it stay
        valid until the next call. */
    const SampleSet& beginBlock() noexcept;
    /** Audio thread: adds the audition, if one is playing, to `out`. */
    void mixPreview (juce::AudioBuffer<float>& out, int numSamples) noexcept;

    //==============================================================================
    /** Reads up to maxSeconds of the first two channels, at the file's own rate.
        Non-finite samples become 0. Returns nullptr (and sets `error`) on failure.
        Any thread; `formats` must not be shared with another thread. */
    static SamplePtr decode (juce::AudioFormatManager& formats, const juce::File& file, juce::String& error);

    /** Extensions the bank reads, as a FileChooser / WildcardFileFilter pattern. */
    static juce::String audioFileWildcard();
    static bool isAudioFile (const juce::File& file);

private:
    class Loader;

    static constexpr int numEntries = numSampleSlots + 1; // the slots, then the audition
    static constexpr int previewEntry = numSampleSlots;

    struct Entry
    {
        SamplePtr sample;
        juce::String path;
        uint32_t request = 0; // bumped by every change, so a stale background load is dropped
    };

    /** Called by the loader thread with a finished decode. */
    void install (int entry, uint32_t request, SamplePtr sample, bool fromUser);
    /** Swaps an entry's sample, retiring the old one. modelLock must be held. */
    void replaceLocked (int entry, SamplePtr sample);
    /** Hands the current pointers to the audio thread. modelLock must be held. */
    void publishLocked();

    void handleAsyncUpdate() override;
    void timerCallback() override { releaseRetired(); }

    mutable juce::CriticalSection modelLock;
    std::array<Entry, numEntries> entries;
    uint32_t previewCount = 0; // bumped by preview() / stopPreview()
    std::vector<std::pair<SamplePtr, uint64_t>> retired; // sample, first serial without it
    uint64_t serial = 0;
    std::vector<SampleSlot> userLoaded; // waiting for onUserLoad
    std::atomic<int> generation { 0 };

    // Published table: written under audioLock, read by the audio thread under a try-lock.
    juce::SpinLock audioLock;
    std::array<const SampleData*, numEntries> published {};
    uint64_t publishedSerial = 0;
    uint32_t publishedPreviewCount = 0;
    bool audioHasAdopted = false; // false until the audio thread has taken any pointer
    std::atomic<bool> audioDirty { false };
    std::atomic<uint64_t> adoptedSerial { 0 };

    // Audio thread only.
    SampleSet audioSet;
    const SampleData* audioPreview = nullptr;
    uint32_t audioPreviewCount = 0;
    double previewPos = -1.0; // < 0: not playing
    double outputRate = 0.0;

    std::atomic<bool> previewActive { false };
    std::atomic<uint32_t> latestPreviewCount { 0 }; // previewCount, for the audio thread

    std::unique_ptr<Loader> loader;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleBank)
};

} // namespace drum
