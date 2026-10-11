// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "Parameters.h"

#include <array>
#include <atomic>
#include <deque>

class DrumFoundryProcessor;

namespace drum
{

/** Undo and redo for the whole patch: every parameter, the drawn envelopes, the
    imported samples and the preset name (DrumFoundryProcessor::createStateTree()).
    The session-only parameters (sidechain trigger, solo) are left out: undo never
    changes them.

    It keeps snapshots rather than recording edits. The patch as it was after the
    last step is the baseline, and poll() compares the patch with it. A change the
    user made becomes one step once their gesture is over; any other change (host
    automation, a host recall, a change while the editor was closed) silently
    becomes the new baseline. So it has to be told about user activity:
    noteUserActivity() for every mouse press, drag, wheel or key in the editor and
    for actions that land later (a menu choice, a preset load, a sample that
    finishes decoding), and poll()'s `userBusy` while a gesture is still going on
    (a mouse button down, a menu open), so a knob drag or a point drag is one step
    however many values it passes through.

    Polling is cheap while nothing changes: it compares the parameter values and a
    few counters with the ones it saw last, and only builds and compares a
    snapshot when one of those moved.

    Message thread only, except noteHostRecall(). Never touches the audio thread. */
class UndoHistory
{
public:
    /** Steps kept in each direction; the oldest undo step is dropped past this. */
    static constexpr int maxSteps = 100;
    /** How long user activity with no change to show for it still counts, for a
        change that lands a little after the gesture (ms). */
    static constexpr double activityGraceMs = 1500.0;

    explicit UndoHistory (DrumFoundryProcessor& processor);

    /** The user did something that may change the patch. The next change poll()
        finds (once the user isn't busy) becomes an undo step. */
    void noteUserActivity();

    /** Looks for changes: commits a step, or absorbs a change that wasn't the
        user's. `userBusy`: a gesture is still going on, so wait (a sample the user
        is loading counts as busy by itself). */
    void poll (bool userBusy);

    /** Commits or absorbs any change now, busy or not (before an undo or redo, and
        when the editor closes). */
    void flush();

    bool canUndo() const noexcept { return ! undoSteps.empty(); }
    bool canRedo() const noexcept { return ! redoSteps.empty(); }
    int getNumUndoSteps() const noexcept { return (int) undoSteps.size(); }
    int getNumRedoSteps() const noexcept { return (int) redoSteps.size(); }
    /** What the next undo / redo reverts, e.g. "Tune", "pitch curve edit",
        "preset load" (empty when there's nothing to undo / redo). */
    juce::String getUndoDescription() const;
    juce::String getRedoDescription() const;

    /** Restores the patch before the last step. Returns false (and does nothing)
        when there's nothing to undo. */
    bool undo();
    bool redo();

    /** Forgets every step and takes the current patch as the baseline. */
    void clear();

    /** The host replaced the state (setStateInformation). Any thread. The next
        poll() takes the new patch as the baseline, and forgets the history unless
        the patch is the same as before (some hosts re-send the state they got). */
    void noteHostRecall() noexcept { hostRecalled.store (true); }

    /** Bumps whenever canUndo(), canRedo() or the descriptions may have changed. */
    int getRevision() const noexcept { return revision; }

    /** The processor's getStateVersion() right after the last undo or redo (-1 if
        none), so the editor can tell a restore from a preset or session load: it
        keeps its layer tabs for a restore (see UiState::layerTabs). */
    int getRestoredStateVersion() const noexcept { return restoredVersion; }

private:
    struct Snapshot
    {
        juce::ValueTree state;
        /** The editor's extra-layer tabs that went with it (a muted layer keeps its tab). */
        std::array<bool, kNumExtraLayers> layerTabs {};
    };

    struct Step
    {
        Snapshot snapshot;
        juce::String description;
    };

    /** Everything poll() looks at before building a snapshot. */
    struct Fingerprint
    {
        int stateVersion = -1, envelopeEdits = -1, sampleGeneration = -1;
        juce::String presetName;
        juce::String fxChain;   // the master chain's order (DrumFoundryProcessor::fxChainId)
        std::vector<float> values;

        bool operator== (const Fingerprint& other) const noexcept;
        bool operator!= (const Fingerprint& other) const noexcept { return ! operator== (other); }
    };

    Fingerprint takeFingerprint() const;
    Snapshot takeSnapshot() const;
    void settle (bool force);
    void setBaseline (Snapshot current, Fingerprint fingerprint);
    void restore (const Snapshot& snapshot);
    void expireActivity();
    bool isUserLoadingSample() const;

    static juce::String describe (const juce::ValueTree& before, const juce::ValueTree& after,
                                  const juce::AudioProcessorValueTreeState& apvts);

    DrumFoundryProcessor& processor;
    std::vector<std::atomic<float>*> watched; // raw values of every patch parameter

    Snapshot baseline; // invalid until the first poll
    Fingerprint baselineFingerprint;
    std::deque<Step> undoSteps, redoSteps;

    bool userActive = false;
    double lastActivityMs = 0.0;
    std::atomic<bool> hostRecalled { false };
    int revision = 0;
    int restoredVersion = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UndoHistory)
};

} // namespace drum
