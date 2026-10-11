// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "dsp/StyleInfo.h"
#include "dsp/ParametricEq.h"

#include <set>

using namespace drum;

const juce::Identifier DrumFoundryProcessor::envelopesId ("ENVELOPES");
const juce::Identifier DrumFoundryProcessor::presetNameId ("presetName");
const juce::Identifier DrumFoundryProcessor::formatVersionId ("format");
const juce::Identifier DrumFoundryProcessor::fxChainId ("fxChain");
const juce::Identifier DrumFoundryProcessor::samplesId ("SAMPLES");

namespace
{
    const juce::Identifier uiTabId ("uiTab");     // before format 4: the EnvId on show
    const juce::Identifier uiWidthId ("uiWidth");
    const juce::Identifier uiPitchFocusId ("uiPitchFocus");
    const juce::Identifier uiPitchPanId ("uiPitchPan");
    const juce::Identifier uiChartLayerId ("uiChartLayer"); // UiState::chartLayer
    const juce::Identifier uiChartTabId ("uiChartTab");     // UiState::chartTab
    const juce::Identifier uiChartAllId ("uiChartAll");     // UiState::chartShowAll
    const juce::Identifier uiLfoTabId ("uiLfoTab");
    // No longer written (the display's line buttons and "Chart follows the selected
    // layer"), but stripped from what older sessions load.
    const juce::Identifier uiPitchEnvId ("uiPitchEnv");
    const juce::Identifier uiLevelEnvId ("uiLevelEnv");
    const juce::Identifier uiEnvVisibleId ("uiEnvVisible");
    const juce::Identifier uiChartFollowsId ("uiChartFollows");

    // One SAMPLE child of SAMPLES per loaded (or missing) slot.
    const juce::Identifier sampleTypeId ("SAMPLE");
    const juce::Identifier sampleSlotId ("slot");
    const juce::Identifier samplePathId ("path");

    // How AudioProcessorValueTreeState stores each parameter.
    const juce::Identifier paramTypeId ("PARAM");
    const juce::Identifier paramIdId ("id");
    const juce::Identifier paramValueId ("value");

    /** Makes every known parameter value in a state tree (host session or
        preset file) finite and in range: NaN/Inf fall back to the default,
        anything else is clamped. Unknown ids are left for APVTS to ignore. */
    void sanitiseParameterValues (juce::ValueTree& state, const juce::AudioProcessorValueTreeState& apvts)
    {
        for (auto child : state)
        {
            if (! child.hasType (paramTypeId) || ! child.hasProperty (paramValueId))
                continue;

            auto* param = apvts.getParameter (child.getProperty (paramIdId).toString());
            if (param == nullptr)
                continue;

            // Same var -> number conversion APVTS applies when it reads the tree.
            const double value = child.getProperty (paramValueId);
            const auto& range = param->getNormalisableRange();

            const double safe = std::isfinite (value)
                                    ? juce::jlimit ((double) range.start, (double) range.end, value)
                                    : (double) param->convertFrom0to1 (param->getDefaultValue());

            if (! juce::exactlyEqual (safe, value))
                child.setProperty (paramValueId, safe, nullptr);
        }
    }
}

namespace
{
    /** A patch saved before a parameter existed (the LFOs, say) gets that parameter at
        its default, rather than keeping whatever the current patch has. */
    void addMissingParameters (juce::ValueTree& state, juce::AudioProcessorValueTreeState& apvts)
    {
        std::set<juce::String> present;
        for (const auto& child : state)
            if (child.hasType (paramTypeId))
                present.insert (child.getProperty (paramIdId).toString());

        for (const auto& id : allParameterIds())
        {
            if (present.count (id) > 0)
                continue;
            if (auto* param = apvts.getParameter (id))
            {
                juce::ValueTree child (paramTypeId);
                child.setProperty (paramIdId, id, nullptr);
                child.setProperty (paramValueId, param->convertFrom0to1 (param->getDefaultValue()), nullptr);
                state.appendChild (child, nullptr);
            }
        }
    }
}

juce::Identifier DrumFoundryProcessor::envelopeTreeId (int envIndex)
{
    static const juce::Identifier ids[] { "PITCH", "BODY", "NOISE", "NOISE_PITCH", "CLICK_PITCH", "CLICK",
                                          "LAYER1_PITCH", "LAYER1_LEVEL", "LAYER2_PITCH", "LAYER2_LEVEL",
                                          "LAYER3_PITCH", "LAYER3_LEVEL" };
    static_assert (std::size (ids) == numEnvs);
    return ids[envIndex];
}

juce::AudioProcessor::BusesProperties DrumFoundryProcessor::makeBuses (WrapperType wrapper)
{
    auto buses = BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true);

    // The sidechain is the only input bus. For VST3, Vst3Extensions marks it
    // kAux, which is what Live and other hosts look for; a dummy disabled main
    // input isn't needed, and the AU wrapper would force it on anyway (AU has
    // no disabled buses: it calls enableAllBuses()). The standalone app gets no
    // input at all: it has nothing to route into one, and any input bus makes
    // it open the audio input, which triggers the macOS microphone prompt.
    if (wrapper != wrapperType_Standalone)
        buses = buses.withInput ("Sidechain", juce::AudioChannelSet::stereo(), true);

    return buses;
}

DrumFoundryProcessor::DrumFoundryProcessor (WrapperType busesFor)
    : AudioProcessor (makeBuses (busesFor)),
      apvts (*this, nullptr, "DRUMFOUNDRY", createParameterLayout()),
      paramReader (apvts),
      sidechainBus (getBusCount (true) > 0 ? 0 : -1),
      bakeScratch (std::make_unique<EnvTables>()),
      publishedTables (std::make_unique<EnvTables>()),
      presets (*this)
{
    for (int e = 0; e < numEnvs; ++e)
        envelopes[(size_t) e] = defaultEnvelope ((EnvId) e);

    apvts.state.setProperty (presetNameId, "Init", nullptr);
    publishTables();
    engine.setTables (*publishedTables);

    samples.onUserLoad = [this] (SampleSlot slot)
    {
        selectSampleSource (slot);
        undoHistory.noteUserActivity(); // the load and the style switch are one undo step
    };
}

DrumFoundryProcessor::~DrumFoundryProcessor() = default;

//==============================================================================
void DrumFoundryProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
    engine.setParams (readParams());
    sidechainTrigger.prepare (sampleRate, samplesPerBlock);
    samples.prepare (sampleRate);
    analyzerTap.sampleRate.store (sampleRate);
    tablesDirty = true;
    setLatencySamples (engine.getLatencySamples());
}

bool DrumFoundryProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    for (int i = 0; i < layouts.inputBuses.size(); ++i)
    {
        const auto& in = layouts.getChannelSet (true, i);

        // The sidechain may be off, mono or stereo. Any other input would be
        // ignored, so it has to stay disabled.
        const bool ok = in.isDisabled()
                     || (i == sidechainBus && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo()));
        if (! ok)
            return false;
    }
    return true;
}

void DrumFoundryProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // Some hosts call processBlock before prepareToPlay, or prepare with a
    // nonsense sample rate that the engine refuses.
    if (! engine.isPrepared())
    {
        buffer.clear();
        return;
    }

    const int numSamples = buffer.getNumSamples();

    // JUCE hands us max(inputs, outputs) channels, and input channels share
    // memory with output channels: the sidechain lives in the very samples the
    // engine is about to overwrite. So the detector scans the whole block first.
    // A disabled bus gives 0 channels (Live disables it when no source is
    // picked, and the channels then still hold our own last output), and the
    // detector does nothing.
    if (sidechainResetPending.exchange (false))
        sidechainTrigger.reset();

    if (sidechainBus >= 0)
        sidechainTrigger.process (getBusBuffer (buffer, true, sidechainBus), numSamples, paramReader.readSidechain());

    // Pick up envelope edits without ever blocking: if the editor is mid-write
    // we simply try again next block.
    if (tablesDirty.load (std::memory_order_acquire))
    {
        const juce::SpinLock::ScopedTryLockType lock (tableLock);
        if (lock.isLocked())
        {
            engine.setTables (*publishedTables);
            tablesDirty.store (false, std::memory_order_release);
        }
    }

    // Synced LFOs follow the host's tempo (the last one it reported, when it stops reporting).
    if (auto* ph = getPlayHead())
        if (const auto position = ph->getPosition())
            if (const auto bpm = position->getBpm(); bpm.hasValue() && std::isfinite (*bpm) && *bpm > 0.0)
                hostBpm.store ((float) *bpm, std::memory_order_relaxed);

    {
        auto p = readParams();
        p.eqListenHz = eqListenHz.load (std::memory_order_relaxed);
        p.eqListenQ = eqListenQ.load (std::memory_order_relaxed);
        engine.setParams (p);
    }
    engine.setSamples (samples.beginBlock());

    for (const auto meta : midi)
        if (const auto m = meta.getMessage(); m.isNoteOn())
            displayNote.store (m.getNoteNumber(), std::memory_order_relaxed);

    if (const float vel = auditionVelocity.exchange (0.0f); vel > 0.0f)
        engine.noteOn (displayNote.load (std::memory_order_relaxed), vel);

    // Only the main output bus gets synth audio; every other channel (e.g. the
    // second sidechain channel next to a mono output) is cleared, so the key
    // signal never leaks through.
    auto out = getBusBuffer (buffer, false, 0);
    for (int ch = out.getNumChannels(); ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    // Also guards the output: a NaN/Inf block comes back silent, with the
    // engine reset (see DrumEngine::process, shared with the offline renderer).
    const int numHits = sidechainBus >= 0 ? sidechainTrigger.getNumHits() : 0;
    if (numHits > 0)
        sidechainHitCount.fetch_add (numHits, std::memory_order_relaxed);
    engine.process (out, midi, sidechainTrigger.getHits(), numHits);
    hitCount.store (engine.getNumVoicesStarted());
    samples.mixPreview (out, numSamples);

    analyzerTap.push (out, numSamples);

    playhead.store (engine.getPlayhead());
    for (int i = 0; i < kNumLfos; ++i)
        lfoOutputs[(size_t) i].store (engine.getLfoValue (i), std::memory_order_relaxed);
    for (int ch = 0; ch < juce::jmin (2, out.getNumChannels()); ++ch)
    {
        const float mag = out.getMagnitude (ch, 0, numSamples);
        if (mag > peaks[(size_t) ch].load())
            peaks[(size_t) ch].store (mag);
    }
    if (out.getNumChannels() == 1)
        peaks[1].store (peaks[0].load());
}

double DrumFoundryProcessor::getTailLengthSeconds() const
{
    // The dry hit (Length plus the latest layer offset) or the reverb tail,
    // whichever is longer, plus the master chain's latency. The gated tail ends
    // Gate Time + Gate Fade after the hit (Gate Time counts from the hit, so
    // neither pre-delay nor a late layer lengthens it).
    const auto p = paramReader.read();
    const double reverbTail = Reverb::tailSeconds (p) + (p.revGate ? 0.0 : DrumVoice::latestOffsetMs (p) * 0.001);
    const double tail = juce::jmax (DrumEngine::hitLengthSeconds (p), reverbTail);
    const double sr = getSampleRate();
    return tail + (sr > 0.0 ? engine.getLatencySamples() / sr : 0.0);
}

void DrumFoundryProcessor::triggerAudition (float velocity) noexcept
{
    auditionVelocity.store (juce::jlimit (0.01f, 1.0f, velocity));
}

//==============================================================================
CurveEnvelope DrumFoundryProcessor::getEnvelope (EnvId id) const
{
    const juce::ScopedLock sl (modelLock);
    return envelopes[(size_t) id];
}

void DrumFoundryProcessor::setEnvelope (EnvId id, const CurveEnvelope& env)
{
    {
        const juce::ScopedLock sl (modelLock);
        envelopes[(size_t) id] = env;
    }
    ++envelopeEdits;
    publishTables();
}

void DrumFoundryProcessor::setLayerCurveSource (int layer, bool pitch, int source)
{
    if (! juce::isPositiveAndBelow (layer, kNumExtraLayers) || ! juce::isPositiveAndBelow (source, layerEnvNames.size()))
        return;

    auto* param = apvts.getParameter (layerParamId (layer, pitch ? pid::layPitchEnv : pid::layEnv));
    if (param == nullptr)
        return;
    const int was = juce::roundToInt (param->convertFrom0to1 (param->getValue()));
    if (was == source)
        return;

    // To Own from a core layer's curve: the own curve starts as a copy of it, so the
    // layer sounds the same until it's edited.
    if (source == layerEnvOwn)
    {
        const int from = pitch ? layerPitchEnvelope (layer, was) : layerLevelEnvelope (layer, was);
        const int to = pitch ? layerOwnPitchEnvelope (layer) : layerOwnLevelEnvelope (layer);
        if (from >= 0 && from != to)
            setEnvelope ((EnvId) to, CurveEnvelope (getEnvelope ((EnvId) from).getPoints(), envelopeRange (to)));
    }

    param->beginChangeGesture();
    param->setValueNotifyingHost (param->convertTo0to1 ((float) source));
    param->endChangeGesture();
}

void DrumFoundryProcessor::bakeTables (EnvTables& dest) const
{
    const juce::ScopedLock sl (modelLock);
    for (int e = 0; e < numEnvs; ++e)
        dest.bake (e, envelopes[(size_t) e]);
}

void DrumFoundryProcessor::publishTables()
{
    const juce::ScopedLock sl (modelLock);
    for (int e = 0; e < numEnvs; ++e)
        bakeScratch->bake (e, envelopes[(size_t) e]);

    const juce::SpinLock::ScopedLockType lock (tableLock);
    *publishedTables = *bakeScratch;
    tablesDirty.store (true, std::memory_order_release);
}

//==============================================================================
void DrumFoundryProcessor::loadSample (SampleSlot slot, const juce::File& file)
{
    undoHistory.noteUserActivity();
    samples.load (slot, file);
}

void DrumFoundryProcessor::clearSample (SampleSlot slot)
{
    undoHistory.noteUserActivity();
    samples.clear (slot);
}

bool DrumFoundryProcessor::isLoadingUserSample() const
{
    for (int s = 0; s < numSampleSlots; ++s)
        if (samples.isLoading ((SampleSlot) s))
            return true;
    return samples.hasPendingUserLoads();
}

void DrumFoundryProcessor::selectSampleSource (SampleSlot slot)
{
    const auto set = [this] (const juce::String& id, float plainValue)
    {
        auto* param = apvts.getParameter (id);
        if (param == nullptr)
            return;

        const float value = param->convertTo0to1 (plainValue);
        if (juce::exactlyEqual (param->getValue(), value))
            return;

        param->beginChangeGesture();
        param->setValueNotifyingHost (value);
        param->endChangeGesture();
    };

    switch (slot)
    {
        case slotBody:
            set (pid::bodyOn, 1.0f);
            set (pid::bodyStyle, (float) sampleStyleIndex (bodyStyleNames));
            break;
        case slotNoise:
            set (pid::noiseOn, 1.0f);
            set (pid::noiseStyle, (float) sampleStyleIndex (noiseStyleNames));
            break;
        case slotClick:
            set (pid::clickOn, 1.0f);
            set (pid::clickType, (float) sampleStyleIndex (clickTypeNames));
            break;
        case slotLayer1:
        case slotLayer2:
        case slotLayer3:
        {
            const int layer = (int) slot - (int) slotLayer1;
            set (layerParamId (layer, pid::layOn), 1.0f);
            set (layerParamId (layer, pid::laySource), (float) layerSourceNames.indexOf ("Sample"));
            break;
        }
        case numSampleSlots:
        default:
            break;
    }
}

//==============================================================================
juce::ValueTree DrumFoundryProcessor::createStateTree() const
{
    auto state = const_cast<juce::AudioProcessorValueTreeState&> (apvts).copyState();
    state.removeChild (state.getChildWithName (envelopesId), nullptr);
    state.setProperty (formatVersionId, stateFormatVersion, nullptr);

    juce::ValueTree envs (envelopesId);
    {
        const juce::ScopedLock sl (modelLock);
        for (int e = 0; e < numEnvs; ++e)
            envs.appendChild (envelopes[(size_t) e].toValueTree (envelopeTreeId (e)), nullptr);
    }
    state.appendChild (envs, nullptr);

    // Absolute paths, missing files included, so a session or preset opened
    // before the files are back still remembers them.
    state.removeChild (state.getChildWithName (samplesId), nullptr);
    juce::ValueTree smp (samplesId);
    for (int s = 0; s < numSampleSlots; ++s)
    {
        const auto path = samples.getPath ((SampleSlot) s);
        if (path.isEmpty())
            continue;

        juce::ValueTree child (sampleTypeId);
        child.setProperty (sampleSlotId, sampleSlotKey ((SampleSlot) s), nullptr);
        child.setProperty (samplePathId, path, nullptr);
        smp.appendChild (child, nullptr);
    }
    state.appendChild (smp, nullptr);
    return state;
}

void DrumFoundryProcessor::loadStateTree (const juce::ValueTree& tree)
{
    loadStateTree (tree, false);
}

void DrumFoundryProcessor::setFxChain (const FxChain& chain)
{
    fxChainCode.store (chain.pack(), std::memory_order_relaxed);
    apvts.state.setProperty (fxChainId, chain.toString(), nullptr);
}

void DrumFoundryProcessor::restorePatch (const juce::ValueTree& patch)
{
    // As a preset load does: the session-only parameters keep their values.
    auto tree = patch.createCopy();
    const auto& sessionOnly = sessionOnlyParameterIds();
    for (int i = tree.getNumChildren(); --i >= 0;)
    {
        const auto child = tree.getChild (i);
        if (child.hasType (paramTypeId) && sessionOnly.contains (child.getProperty (paramIdId).toString()))
            tree.removeChild (i, nullptr);
    }
    for (const auto& id : sessionOnly)
    {
        juce::ValueTree child (paramTypeId);
        child.setProperty (paramIdId, id, nullptr);
        child.setProperty (paramValueId, apvts.getRawParameterValue (id)->load(), nullptr);
        tree.appendChild (child, nullptr);
    }

    loadStateTree (tree, true);
}

void DrumFoundryProcessor::loadStateTree (const juce::ValueTree& tree, bool reuseSamples)
{
    if (! tree.hasType (apvts.state.getType()))
        return;

    auto copy = tree.createCopy();
    copy.removeProperty (uiTabId, nullptr);
    copy.removeProperty (uiWidthId, nullptr);
    copy.removeProperty (uiPitchFocusId, nullptr);
    copy.removeProperty (uiPitchPanId, nullptr);
    for (const auto& id : { uiPitchEnvId, uiLevelEnvId, uiEnvVisibleId, uiChartFollowsId, uiLfoTabId, uiChartLayerId, uiChartTabId, uiChartAllId })
        copy.removeProperty (id, nullptr);
    auto envs = copy.getChildWithName (envelopesId).createCopy();

    // Patches saved before the pitch curve was in semitones stored it as 0..1
    // of a "pitchDepth" parameter; scale it up so they sound the same.
    for (auto child : copy)
    {
        if (child.hasType (paramTypeId) && child.getProperty (paramIdId).toString() == "pitchDepth")
        {
            const double depth = child.getProperty (paramValueId);
            for (auto point : envs.getChildWithName (envelopeTreeId (envPitch)))
                if (std::isfinite (depth))
                    point.setProperty ("y", (double) point.getProperty ("y") * depth, nullptr);
            break;
        }
    }

    {
        const juce::ScopedLock sl (modelLock);
        for (int e = 0; e < numEnvs; ++e)
            envelopes[(size_t) e] = CurveEnvelope::fromValueTree (envs.getChildWithName (envelopeTreeId (e)),
                                                                  defaultEnvelope ((EnvId) e));
    }
    copy.removeChild (copy.getChildWithName (envelopesId), nullptr);

    // No SAMPLES (factory presets, older sessions and preset files) means no samples.
    std::array<juce::String, numSampleSlots> samplePaths;
    const auto smp = copy.getChildWithName (samplesId);
    for (const auto& child : smp)
    {
        if (! child.hasType (sampleTypeId))
            continue;
        const auto key = child.getProperty (sampleSlotId).toString();
        for (int s = 0; s < numSampleSlots; ++s)
            if (key == sampleSlotKey ((SampleSlot) s))
                samplePaths[(size_t) s] = child.getProperty (samplePathId).toString();
    }
    copy.removeChild (smp, nullptr);
    samples.restore (samplePaths, reuseSamples);

    // Format 1 patches had Flat at index 2 of an extra layer's Env choice, where
    // Click is now.
    if ((int) copy.getProperty (formatVersionId, 1) < 2)
    {
        for (auto child : copy)
            if (child.hasType (paramTypeId) && child.getProperty (paramIdId).toString().endsWith (pid::layEnv)
                && juce::approximatelyEqual ((double) child.getProperty (paramValueId), 2.0))
                child.setProperty (paramValueId, 3.0, nullptr);
    }

    // Format 1-2 patches had Sample at index 6 of Body Style, where Round now is
    // (the kick bodies went in before it).
    if ((int) copy.getProperty (formatVersionId, 1) < 3)
    {
        for (auto child : copy)
            if (child.hasType (paramTypeId) && child.getProperty (paramIdId).toString() == pid::bodyStyle
                && juce::approximatelyEqual ((double) child.getProperty (paramValueId), 6.0))
                child.setProperty (paramValueId, (double) sampleStyleIndex (bodyStyleNames), nullptr);
    }

    // Format 3-6 patches had Sample at index 11 of Body Style, where Sine now is (the
    // basic waveforms went in before it). An extra layer's list has no Sample, so its
    // indices are unchanged.
    if (const int format = (int) copy.getProperty (formatVersionId, 1); format >= 3 && format < 7)
    {
        for (auto child : copy)
            if (child.hasType (paramTypeId) && child.getProperty (paramIdId).toString() == pid::bodyStyle
                && juce::approximatelyEqual ((double) child.getProperty (paramValueId), 11.0))
                child.setProperty (paramValueId, (double) sampleStyleIndex (bodyStyleNames), nullptr);
    }

    // Format 1-3 patches had Sample at index 7 of Click Type, before the newer click
    // types went in ahead of it.
    if ((int) copy.getProperty (formatVersionId, 1) < 4)
    {
        for (auto child : copy)
            if (child.hasType (paramTypeId) && child.getProperty (paramIdId).toString() == pid::clickType
                && juce::approximatelyEqual ((double) child.getProperty (paramValueId), 7.0))
                child.setProperty (paramValueId, (double) sampleStyleIndex (clickTypeNames), nullptr);
    }

    // Format 1-3 patches had one Envelope choice per extra layer, which picked a core
    // layer's level curve and, depending on the source, a pitch curve. It's now two
    // choices, Level and Pitch, that take any source; set them so the layer sounds the same.
    if ((int) copy.getProperty (formatVersionId, 1) < 4)
    {
        for (int k = 0; k < kNumExtraLayers; ++k)
        {
            const auto find = [&copy] (const juce::String& id)
            {
                for (auto child : copy)
                    if (child.hasType (paramTypeId) && child.getProperty (paramIdId).toString() == id)
                        return child;
                return juce::ValueTree();
            };
            const auto valueOf = [] (const juce::ValueTree& child, int fallback)
            {
                const double v = child.isValid() ? (double) child.getProperty (paramValueId, fallback) : (double) fallback;
                return std::isfinite (v) ? juce::roundToInt (v) : fallback;
            };

            auto envChild = find (layerParamId (k, pid::layEnv));
            const int source = valueOf (find (layerParamId (k, pid::laySource)), layerSample);
            const int env = juce::jlimit ((int) layerEnvBody, (int) layerEnvFlat, valueOf (envChild, layerEnvBody));

            if (! envChild.isValid())
            {
                envChild = juce::ValueTree (paramTypeId);
                envChild.setProperty (paramIdId, layerParamId (k, pid::layEnv), nullptr);
                copy.appendChild (envChild, nullptr);
            }
            envChild.setProperty (paramValueId, (double) legacyLayerLevelEnvelope (source, env), nullptr);

            auto pitchChild = find (layerParamId (k, pid::layPitchEnv));
            if (! pitchChild.isValid())
            {
                pitchChild = juce::ValueTree (paramTypeId);
                pitchChild.setProperty (paramIdId, layerParamId (k, pid::layPitchEnv), nullptr);
                copy.appendChild (pitchChild, nullptr);
            }
            pitchChild.setProperty (paramValueId, (double) legacyLayerPitchEnvelope (source, env), nullptr);
        }
    }

    // Format 1-4 patches had the 3-band EQ: bilinear filters, which the Digital style is.
    // (Its bands 1-3 are the new EQ's defaults, so the curve is exactly what it was.)
    if ((int) copy.getProperty (formatVersionId, 1) < 5)
    {
        for (int i = copy.getNumChildren(); --i >= 0;)
            if (copy.getChild (i).hasType (paramTypeId) && copy.getChild (i).getProperty (paramIdId).toString() == pid::eqStyle)
                copy.removeChild (i, nullptr);
        juce::ValueTree style (paramTypeId);
        style.setProperty (paramIdId, pid::eqStyle, nullptr);
        style.setProperty (paramValueId, (double) eqStyleDigital, nullptr);
        copy.appendChild (style, nullptr);
    }

    // Format 1-7 patches had the Classic reverb (the only one there was).
    if ((int) copy.getProperty (formatVersionId, 1) < 8)
    {
        for (int i = copy.getNumChildren(); --i >= 0;)
            if (copy.getChild (i).hasType (paramTypeId) && copy.getChild (i).getProperty (paramIdId).toString() == pid::revEngine)
                copy.removeChild (i, nullptr);
        juce::ValueTree engineChild (paramTypeId);
        engineChild.setProperty (paramIdId, pid::revEngine, nullptr);
        engineChild.setProperty (paramValueId, (double) revEngineClassic, nullptr);
        copy.appendChild (engineChild, nullptr);
    }
    copy.setProperty (formatVersionId, stateFormatVersion, nullptr);

    sanitiseParameterValues (copy, apvts);
    addMissingParameters (copy, apvts);

    // The chain's order (format 6 on). Without it, the chain patches always had.
    const auto chain = copy.hasProperty (fxChainId) ? FxChain::fromString (copy.getProperty (fxChainId).toString())
                                                    : FxChain::defaults();
    copy.setProperty (fxChainId, chain.toString(), nullptr);
    fxChainCode.store (chain.pack(), std::memory_order_relaxed);

    apvts.replaceState (copy);
    publishTables();
    ++stateVersion;
}

void DrumFoundryProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto tree = createStateTree();
    tree.setProperty (uiWidthId, uiState.width, nullptr);
    tree.setProperty (uiPitchFocusId, uiState.pitchFocus, nullptr);
    tree.setProperty (uiPitchPanId, uiState.pitchPan, nullptr);
    tree.setProperty (uiChartLayerId, uiState.chartLayer, nullptr);
    tree.setProperty (uiChartTabId, uiState.chartTab, nullptr);
    tree.setProperty (uiChartAllId, uiState.chartShowAll, nullptr);
    tree.setProperty (uiLfoTabId, uiState.lfoTab, nullptr);

    if (auto xml = tree.createXml())
        copyXmlToBinary (*xml, destData);
}

void DrumFoundryProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        const auto tree = juce::ValueTree::fromXml (*xml);
        uiState.width = (int) tree.getProperty (uiWidthId, 0);
        const auto finiteOr0 = [] (float v) { return std::isfinite (v) ? v : 0.0f; };
        uiState.pitchFocus = juce::jlimit (0.0f, 1.0f, finiteOr0 ((float) tree.getProperty (uiPitchFocusId, 0.0f)));
        uiState.pitchPan = juce::jlimit (-10.0f, 10.0f, finiteOr0 ((float) tree.getProperty (uiPitchPanId, 0.0f)));

        // The layer and tab the display was on. Sessions from before format 4 stored the
        // curve on show (an EnvId of a core layer): its layer, on its tab.
        if (tree.hasProperty (uiChartLayerId))
        {
            uiState.chartLayer = juce::jlimit (0, UiState::numChartLayers - 1, (int) tree.getProperty (uiChartLayerId, 0));
            uiState.chartTab = juce::jlimit (0, 1, (int) tree.getProperty (uiChartTabId, 0));
            uiState.chartShowAll = (bool) tree.getProperty (uiChartAllId, false);
        }
        else
        {
            const int env = juce::jlimit (0, numCoreEnvs - 1, (int) tree.getProperty (uiTabId, 0));
            uiState.chartLayer = env == envNoise || env == envNoisePitch ? 1 : env == envClick || env == envClickPitch ? 2 : 0;
            uiState.chartTab = isPitchEnvelope (env) ? 0 : 1;
        }
        uiState.lfoTab = (bool) tree.getProperty (uiLfoTabId, false);
        loadStateTree (tree);
        undoHistory.noteHostRecall(); // may be on any thread: the history catches up on its next poll
    }
}

//==============================================================================
juce::AudioProcessorEditor* DrumFoundryProcessor::createEditor()
{
    return new DrumFoundryEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DrumFoundryProcessor();
}
