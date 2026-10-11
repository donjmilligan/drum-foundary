// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "PresetManager.h"
#include "FactoryPresets.h"
#include "../PluginProcessor.h"
#include "../dsp/StyleInfo.h"

using namespace drum;

namespace
{
    const juce::Identifier paramType ("PARAM");
    const juce::Identifier paramId ("id");
    const juce::Identifier paramValue ("value");

    bool isSessionOnly (const juce::ValueTree& child)
    {
        return child.hasType (paramType) && sessionOnlyParameterIds().contains (child.getProperty (paramId).toString());
    }

    bool isKickBody (int style) noexcept
    {
        return style == bodyKick || style == bodyRound || style == bodyTaper || style == bodyHard
            || style == bodyFold || style == bodyTonal;
    }

    /** A user preset file's Body Style, or -1 if it can't be read. */
    int bodyStyleOf (const juce::File& file)
    {
        if (auto xml = juce::XmlDocument::parse (file))
            for (auto* child : xml->getChildIterator())
                if (child->hasTagName (paramType.toString()) && child->getStringAttribute (paramId.toString()) == pid::bodyStyle)
                    return juce::roundToInt (child->getDoubleAttribute (paramValue.toString(), -1.0));
        return -1;
    }

    void removeSessionOnlyParams (juce::ValueTree& tree)
    {
        for (int i = tree.getNumChildren(); --i >= 0;)
            if (isSessionOnly (tree.getChild (i)))
                tree.removeChild (i, nullptr);
    }
}

PresetManager::PresetManager (DrumFoundryProcessor& p) : processor (p)
{
    refresh();
}

juce::File PresetManager::getUserFolder()
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
               .getChildFile ("Drum Foundry")
               .getChildFile ("Presets");
}

void PresetManager::refresh()
{
    entries.clear();

    const auto saysKick = [] (const juce::String& name) { return name.containsIgnoreCase ("kick"); };

    const auto& factory = factoryPresets();
    for (int i = 0; i < (int) factory.size(); ++i)
    {
        const juce::String name (factory[(size_t) i].name);
        entries.push_back ({ name, true, i, {}, saysKick (name) ? Category::kick : Category::snare });
    }

    auto files = getUserFolder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + fileExtension);
    files.sort();
    for (const auto& f : files)
    {
        const auto name = f.getFileNameWithoutExtension();
        const bool kick = saysKick (name) || (! name.containsIgnoreCase ("snare") && isKickBody (bodyStyleOf (f)));
        entries.push_back ({ name, false, -1, f, kick ? Category::kick : Category::snare });
    }

    // Listed by group (factory kicks, factory snares, user kicks, user snares), each with
    // its Init first and the rest A to Z.
    displayOrder.clear();
    for (const bool factoryGroup : { true, false })
        for (const auto category : { Category::kick, Category::snare })
        {
            std::vector<int> group;
            for (int i = 0; i < (int) entries.size(); ++i)
                if (entries[(size_t) i].isFactory == factoryGroup && entries[(size_t) i].category == category)
                    group.push_back (i);
            std::stable_sort (group.begin(), group.end(), [this] (int a, int b)
            {
                const auto& x = entries[(size_t) a].name;
                const auto& y = entries[(size_t) b].name;
                const bool xi = x.startsWithIgnoreCase ("init"), yi = y.startsWithIgnoreCase ("init");
                if (xi != yi)
                    return xi;
                return x.compareNatural (y) < 0;
            });
            displayOrder.insert (displayOrder.end(), group.begin(), group.end());
        }
}

juce::String PresetManager::getCurrentName() const
{
    return processor.getState().state.getProperty (DrumFoundryProcessor::presetNameId).toString();
}

int PresetManager::getCurrentIndex() const
{
    const auto name = getCurrentName();
    for (int i = 0; i < (int) entries.size(); ++i)
        if (entries[(size_t) i].name == name)
            return i;
    return -1;
}

void PresetManager::step (int delta)
{
    const int n = (int) entries.size();
    if (n == 0)
        return;

    // Through the list as it's shown (getDisplayOrder()).
    const int current = getCurrentIndex();
    const auto at = std::find (displayOrder.begin(), displayOrder.end(), current);
    const int pos = at == displayOrder.end() ? -1 : (int) (at - displayOrder.begin());
    load (displayOrder[(size_t) (pos < 0 ? 0 : (pos + delta + n) % n)]);
}

void PresetManager::load (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) entries.size()))
        return;

    const auto& entry = entries[(size_t) index];
    juce::ValueTree tree;

    if (entry.isFactory)
    {
        tree = buildFactoryState (entry.factoryIndex);
    }
    else if (auto xml = juce::XmlDocument::parse (entry.file))
    {
        tree = juce::ValueTree::fromXml (*xml);
    }

    if (! tree.isValid())
        return;

    tree.setProperty (DrumFoundryProcessor::presetNameId, entry.name, nullptr);

    // Sidechain trigger settings are routing, not sound: whatever the preset
    // (or an old preset file) says, keep the current values.
    removeSessionOnlyParams (tree);
    for (const auto& id : sessionOnlyParameterIds())
    {
        juce::ValueTree child (paramType);
        child.setProperty (paramId, id, nullptr);
        child.setProperty (paramValue, processor.getState().getRawParameterValue (id)->load(), nullptr);
        tree.appendChild (child, nullptr);
    }

    processor.getUndoHistory().noteUserActivity(); // a preset load is one undo step
    processor.loadStateTree (tree);
}

bool PresetManager::saveUserPreset (const juce::String& rawName)
{
    const auto name = rawName.trim();
    const auto fileName = juce::File::createLegalFileName (name);
    if (fileName.isEmpty())
        return false;

    auto folder = getUserFolder();
    if (! folder.createDirectory())
        return false;

    processor.getState().state.setProperty (DrumFoundryProcessor::presetNameId, name, nullptr);

    auto tree = processor.createStateTree();
    removeSessionOnlyParams (tree);

    const auto xml = tree.createXml();
    if (xml == nullptr || ! xml->writeTo (folder.getChildFile (fileName + fileExtension)))
        return false;

    refresh();
    return true;
}

juce::ValueTree PresetManager::buildFactoryState (int factoryIndex) const
{
    const auto& preset = factoryPresets()[(size_t) factoryIndex];

    juce::ValueTree state (processor.getState().state.getType());
    state.setProperty (DrumFoundryProcessor::formatVersionId, DrumFoundryProcessor::stateFormatVersion, nullptr);
    state.setProperty (DrumFoundryProcessor::fxChainId,
                       preset.chain != nullptr ? FxChain::fromString (preset.chain).toString() : FxChain::defaults().toString(), nullptr);

    for (auto* param : processor.getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param);
        if (ranged == nullptr || sessionOnlyParameterIds().contains (ranged->paramID))
            continue;

        float value = ranged->convertFrom0to1 (ranged->getDefaultValue());
        for (const auto& [id, v] : preset.values)
            if (ranged->paramID == id)
                value = v;

        juce::ValueTree child (paramType);
        child.setProperty (paramId, ranged->paramID, nullptr);
        child.setProperty (paramValue, value, nullptr);
        state.appendChild (child, nullptr);
    }

    juce::ValueTree envs (DrumFoundryProcessor::envelopesId);
    for (int e = 0; e < numEnvs; ++e)
    {
        const auto& pts = preset.points (e);
        const auto env = pts.empty() ? defaultEnvelope ((EnvId) e) : CurveEnvelope (pts, envelopeRange (e));
        envs.appendChild (env.toValueTree (DrumFoundryProcessor::envelopeTreeId (e)), nullptr);
    }
    state.appendChild (envs, nullptr);
    return state;
}
