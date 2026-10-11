// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "UndoHistory.h"
#include "PluginProcessor.h"

#include <map>

namespace drum
{

namespace
{
    // How AudioProcessorValueTreeState stores each parameter.
    const juce::Identifier paramTypeId ("PARAM");
    const juce::Identifier paramIdId ("id");
    const juce::Identifier paramValueId ("value");
    const juce::Identifier sampleSlotId ("slot");
    const juce::Identifier samplePathId ("path");

    double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

    /** Parameter id -> value, for every PARAM child. */
    std::map<juce::String, double> paramValues (const juce::ValueTree& state)
    {
        std::map<juce::String, double> values;
        for (const auto& child : state)
            if (child.hasType (paramTypeId))
                values[child.getProperty (paramIdId).toString()] = child.getProperty (paramValueId);
        return values;
    }

    /** Slot key -> path, for every sample in the patch. */
    std::map<juce::String, juce::String> samplePaths (const juce::ValueTree& state)
    {
        std::map<juce::String, juce::String> paths;
        for (const auto& child : state.getChildWithName (DrumFoundryProcessor::samplesId))
            paths[child.getProperty (sampleSlotId).toString()] = child.getProperty (samplePathId).toString();
        return paths;
    }
}

//==============================================================================
bool UndoHistory::Fingerprint::operator== (const Fingerprint& other) const noexcept
{
    return stateVersion == other.stateVersion && envelopeEdits == other.envelopeEdits
        && sampleGeneration == other.sampleGeneration && presetName == other.presetName && fxChain == other.fxChain
        && values.size() == other.values.size()
        && std::equal (values.begin(), values.end(), other.values.begin(),
                       [] (float a, float b) { return juce::exactlyEqual (a, b); });
}

//==============================================================================
UndoHistory::UndoHistory (DrumFoundryProcessor& p) : processor (p)
{
    const auto& sessionOnly = sessionOnlyParameterIds();
    for (const auto& id : allParameterIds())
        if (! sessionOnly.contains (id))
            if (auto* value = processor.getState().getRawParameterValue (id))
                watched.push_back (value);
}

void UndoHistory::noteUserActivity()
{
    userActive = true;
    lastActivityMs = nowMs();
}

void UndoHistory::poll (bool userBusy)
{
    // A host recall is dealt with straight away, whatever the user is doing.
    if (! hostRecalled.load() && (userBusy || isUserLoadingSample()))
        return;
    settle (false);
}

void UndoHistory::flush()
{
    settle (true);
}

juce::String UndoHistory::getUndoDescription() const
{
    return undoSteps.empty() ? juce::String() : undoSteps.back().description;
}

juce::String UndoHistory::getRedoDescription() const
{
    return redoSteps.empty() ? juce::String() : redoSteps.back().description;
}

bool UndoHistory::undo()
{
    flush();
    if (undoSteps.empty())
        return false;

    auto step = std::move (undoSteps.back());
    undoSteps.pop_back();
    redoSteps.push_back ({ baseline, step.description });
    restore (step.snapshot);
    return true;
}

bool UndoHistory::redo()
{
    flush();
    if (redoSteps.empty())
        return false;

    auto step = std::move (redoSteps.back());
    redoSteps.pop_back();
    undoSteps.push_back ({ baseline, step.description });
    while ((int) undoSteps.size() > maxSteps)
        undoSteps.pop_front();
    restore (step.snapshot);
    return true;
}

void UndoHistory::clear()
{
    hostRecalled.store (false);
    undoSteps.clear();
    redoSteps.clear();
    userActive = false;
    setBaseline (takeSnapshot(), takeFingerprint());
    ++revision;
}

//==============================================================================
UndoHistory::Fingerprint UndoHistory::takeFingerprint() const
{
    Fingerprint f;
    f.stateVersion = processor.getStateVersion();
    f.envelopeEdits = processor.getEnvelopeEditCount();
    f.sampleGeneration = processor.getSampleGeneration();
    f.presetName = processor.getState().state.getProperty (DrumFoundryProcessor::presetNameId).toString();
    f.fxChain = processor.getState().state.getProperty (DrumFoundryProcessor::fxChainId).toString();
    f.values.reserve (watched.size());
    for (const auto* v : watched)
        f.values.push_back (v->load (std::memory_order_relaxed));
    return f;
}

UndoHistory::Snapshot UndoHistory::takeSnapshot() const
{
    Snapshot s;
    s.state = processor.createStateTree();

    const auto& sessionOnly = sessionOnlyParameterIds();
    for (int i = s.state.getNumChildren(); --i >= 0;)
    {
        const auto child = s.state.getChild (i);
        if (child.hasType (paramTypeId) && sessionOnly.contains (child.getProperty (paramIdId).toString()))
            s.state.removeChild (i, nullptr);
    }

    // The tabs are only current while they belong to this patch (see UiState).
    const auto& ui = processor.uiState;
    if (ui.layerTabsVersion == processor.getStateVersion())
        s.layerTabs = ui.layerTabs;
    return s;
}

bool UndoHistory::isUserLoadingSample() const
{
    return processor.isLoadingUserSample();
}

void UndoHistory::settle (bool force)
{
    if (hostRecalled.exchange (false))
    {
        // A new session (or the host's own undo): the old steps belong to another
        // patch, unless the host just re-sent the patch we have.
        auto current = takeSnapshot();
        if (baseline.state.isValid() && ! current.state.isEquivalentTo (baseline.state)
            && (! undoSteps.empty() || ! redoSteps.empty()))
        {
            undoSteps.clear();
            redoSteps.clear();
            ++revision;
        }
        userActive = false;
        setBaseline (std::move (current), takeFingerprint());
        return;
    }

    const auto fingerprint = takeFingerprint();
    if (! baseline.state.isValid())
    {
        userActive = false;
        setBaseline (takeSnapshot(), fingerprint);
        return;
    }

    if (fingerprint == baselineFingerprint)
    {
        if (! force)
            expireActivity();
        return;
    }

    auto current = takeSnapshot();
    if (current.state.isEquivalentTo (baseline.state))
    {
        // Changed and changed back (or only a counter moved).
        baselineFingerprint = fingerprint;
        if (! force)
            expireActivity();
        return;
    }

    // Saving a preset only renames the patch: not worth a step.
    auto renamed = baseline.state.createCopy();
    renamed.setProperty (DrumFoundryProcessor::presetNameId, current.state.getProperty (DrumFoundryProcessor::presetNameId), nullptr);
    const bool onlyRenamed = renamed.isEquivalentTo (current.state);

    if (userActive && ! onlyRenamed)
    {
        undoSteps.push_back ({ baseline, describe (baseline.state, current.state, processor.getState()) });
        while ((int) undoSteps.size() > maxSteps)
            undoSteps.pop_front();
        redoSteps.clear();
        userActive = false;
        ++revision;
    }

    setBaseline (std::move (current), fingerprint);
}

void UndoHistory::setBaseline (Snapshot current, Fingerprint fingerprint)
{
    baseline = std::move (current);
    baselineFingerprint = std::move (fingerprint);
}

void UndoHistory::restore (const Snapshot& snapshot)
{
    processor.restorePatch (snapshot.state);
    restoredVersion = processor.getStateVersion();

    // Put back the tabs that went with it, so undoing "add layer" closes the tab
    // and undoing an edit on a muted layer doesn't.
    auto& ui = processor.uiState;
    ui.layerTabs = snapshot.layerTabs;
    ui.layerTabsVersion = restoredVersion;

    // The restored patch as it now reads back is the new baseline, so the restore
    // itself never shows up as a change.
    hostRecalled.store (false);
    userActive = false;
    setBaseline (takeSnapshot(), takeFingerprint());
    ++revision;
}

void UndoHistory::expireActivity()
{
    // Activity that led to no change (a click on an empty spot, a menu dismissed)
    // mustn't turn a later automation change into a step.
    if (userActive && nowMs() - lastActivityMs > activityGraceMs)
        userActive = false;
}

//==============================================================================
juce::String UndoHistory::describe (const juce::ValueTree& before, const juce::ValueTree& after,
                                    const juce::AudioProcessorValueTreeState& apvts)
{
    if (before.getProperty (DrumFoundryProcessor::presetNameId) != after.getProperty (DrumFoundryProcessor::presetNameId))
        return "preset load";

    if (before.getProperty (DrumFoundryProcessor::fxChainId) != after.getProperty (DrumFoundryProcessor::fxChainId))
        return "effects chain";

    // Samples (a load also switches the layer's style, so this goes first).
    {
        const auto was = samplePaths (before), now = samplePaths (after);
        for (int s = 0; s < numSampleSlots; ++s)
        {
            const auto key = sampleSlotKey ((SampleSlot) s);
            const auto a = was.count (key) != 0 ? was.at (key) : juce::String();
            const auto b = now.count (key) != 0 ? now.at (key) : juce::String();
            if (a != b)
                return (b.isEmpty() ? "sample clear (" : "sample load (") + sampleSlotName ((SampleSlot) s) + ")";
        }
    }

    const auto was = paramValues (before), now = paramValues (after);
    juce::StringArray changed;
    for (const auto& [id, value] : now)
        if (auto it = was.find (id); it == was.end() || ! juce::exactlyEqual (it->second, value))
            changed.add (id);

    // Adding a layer switches it on with fresh settings (and starts its own curves).
    for (int k = 0; k < kNumExtraLayers && changed.size() > 1; ++k)
    {
        const auto on = layerParamId (k, pid::layOn);
        if (changed.contains (on) && now.at (on) >= 0.5)
        {
            const int source = juce::roundToInt (now.count (layerParamId (k, pid::laySource)) != 0 ? now.at (layerParamId (k, pid::laySource)) : 0.0);
            return "add " + layerSourceNames[juce::jlimit (0, layerSourceNames.size() - 1, source)] + " layer";
        }
    }

    // Where an extra layer's curve comes from (switching to Own also copies the curve).
    if (changed.size() == 1 && (changed[0].endsWith (pid::layEnv) || changed[0].endsWith (pid::layPitchEnv)))
        if (auto* param = apvts.getParameter (changed[0]))
            return param->getName (64);

    // Envelopes (dragging the pitch curve's end point moves Tune too).
    {
        const auto envs0 = before.getChildWithName (DrumFoundryProcessor::envelopesId);
        const auto envs1 = after.getChildWithName (DrumFoundryProcessor::envelopesId);
        // By EnvId (one name per envelope: indexing past the end would read garbage).
        static const char* names[] { "pitch", "body", "noise", "noise pitch", "click pitch", "click",
                                     "layer 1 pitch", "layer 1 level", "layer 2 pitch", "layer 2 level",
                                     "layer 3 pitch", "layer 3 level" };
        static_assert (std::size (names) == numEnvs);
        juce::StringArray curves;
        for (int e = 0; e < numEnvs; ++e)
        {
            const auto id = DrumFoundryProcessor::envelopeTreeId (e);
            if (! envs0.getChildWithName (id).isEquivalentTo (envs1.getChildWithName (id)))
                curves.add (names[e]);
        }
        if (curves.size() == 1)
            return curves[0] + " curve edit";
        if (curves.size() > 1)
            return "envelope edit";
    }

    if (changed.isEmpty())
        return "edit";

    if (changed.size() > 1)
        return juce::String (changed.size()) + " parameter changes";

    auto* param = apvts.getParameter (changed[0]);
    if (param == nullptr)
        return "edit";

    // A switch reads as what it did: "Layer 2 off", "Reverb Gate on".
    auto name = param->getName (64);
    if (dynamic_cast<juce::AudioParameterBool*> (param) == nullptr)
        return name;
    if (name.endsWith (" On"))
        name = name.dropLastCharacters (3);
    return name + (now.at (changed[0]) >= 0.5 ? " on" : " off");
}

} // namespace drum
