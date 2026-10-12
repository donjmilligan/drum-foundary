// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "Parameters.h"
#include "dsp/AnalyzerTap.h"
#include "dsp/Reverb.h"
#include "dsp/DrumEngine.h"
#include "dsp/TransientTrigger.h"
#include "presets/PresetManager.h"
#include "samples/SampleBank.h"
#include "UndoHistory.h"

class DrumFoundryProcessor : public juce::AudioProcessor
{
public:
    /** The bus layout depends on the plugin format (see makeBuses()); by
        default it's the format this binary was loaded as. */
    explicit DrumFoundryProcessor (WrapperType busesFor = juce::PluginHostType::getPluginLoadedAs());
    ~DrumFoundryProcessor() override;

    /** Output bus 0 is the synth. The one input bus, if any, is the sidechain
        trigger key: it is only ever analysed, never passed to the output. */
    static BusesProperties makeBuses (WrapperType wrapper);
    /** Index of the sidechain input bus, or -1 (Standalone has none). */
    int getSidechainBusIndex() const noexcept { return sidechainBus; }

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    /** Forgets any half-detected sidechain hit. The audio thread does the
        actual reset, in case a host calls this while processing. */
    void reset() override { sidechainResetPending.store (true); }
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::VST3ClientExtensions* getVST3ClientExtensions() override { return &vst3Extensions; }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Drum Foundry"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    juce::AudioProcessorValueTreeState& getState() noexcept { return apvts; }
    /** The patch as the parameters stand, with the host's last tempo (for synced LFOs). */
    drum::DrumParams readParams() const noexcept
    {
        auto p = paramReader.read();
        p.bpm = hostBpm.load (std::memory_order_relaxed);
        p.chain = getFxChain();
        return p;
    }

    /** The master chain's order: part of the patch (presets, sessions, undo) but not a
        parameter. Any thread. */
    drum::FxChain getFxChain() const noexcept { return drum::FxChain::unpack (fxChainCode.load (std::memory_order_relaxed)); }
    /** Message thread. */
    void setFxChain (const drum::FxChain& chain);
    /** Gain reduction (dB) in the last block of chain compressor n (0, 1) or the output one (n < 0). */
    float getCompReductionDb (int n) const noexcept { return engine.getCompReductionDb (n); }
    drum::SidechainParams readSidechainParams() const noexcept { return paramReader.readSidechain(); }

    drum::CurveEnvelope getEnvelope (drum::EnvId id) const;
    /** Called by the editor while the user draws. Takes effect immediately,
        including on hits that are already ringing. */
    void setEnvelope (drum::EnvId id, const drum::CurveEnvelope& env);
    void bakeTables (drum::EnvTables& dest) const;
    /** Bumps with every setEnvelope(), so the undo history (and the display) can spot edits cheaply. */
    int getEnvelopeEditCount() const noexcept { return envelopeEdits.load (std::memory_order_relaxed); }

    /** Sets where extra layer `layer`'s (0-based) pitch curve (`pitch`) or level curve
        comes from: `source` is a LayerEnv (Body, Noise, Click, Flat or Own). Switching
        from a core layer's curve to Own starts the layer's own curve as a copy of it, so
        nothing changes until it's edited. Message thread. */
    void setLayerCurveSource (int layer, bool pitch, int source);

    //==============================================================================
    // Imported samples. Message thread, except the getters (any thread but the audio thread).

    /** Decodes `file` in the background and puts it in `slot`. Once it's in, the
        slot's layer is switched to play it: Body, Noise and Click are turned on
        with their style / type set to "Sample"; an extra layer is turned on with
        its source set to "Sample". A file that can't be read changes nothing. */
    void loadSample (drum::SampleSlot slot, const juce::File& file);
    /** Empties the slot. The layer's style is left as it is. */
    void clearSample (drum::SampleSlot slot);
    /** The slot's audio, or nullptr (empty, still loading, or missing). */
    drum::SamplePtr getSample (drum::SampleSlot slot) const { return samples.get (slot); }
    /** The slot's file, even if it's missing. Empty if the slot is empty. */
    juce::String getSamplePath (drum::SampleSlot slot) const { return samples.getPath (slot); }
    /** True if the patch names a file for the slot that can't be loaded. */
    bool isSampleMissing (drum::SampleSlot slot) const { return samples.isMissing (slot); }
    bool isSampleLoading (drum::SampleSlot slot) const { return samples.isLoading (slot); }
    /** True while a sample the user loaded is still decoding, or hasn't switched its
        layer over yet (the undo history waits for it). */
    bool isLoadingUserSample() const;
    /** Bumps whenever any slot changes, so the UI can poll it. */
    int getSampleGeneration() const noexcept { return samples.getGeneration(); }

    /** Auditions a file (from the browser) mixed into the output, without loading
        it. A new preview replaces the one playing. */
    void previewFile (const juce::File& file) { samples.preview (file); }
    void stopPreview() { samples.stopPreview(); }
    bool isPreviewing() const noexcept { return samples.isPreviewing(); }

    drum::SampleBank& getSampleBank() noexcept { return samples; }
    /** For tests: the sample set the engine was given in the last block. */
    const drum::SampleSet& getEngineSamples() const noexcept { return engine.getSamples(); }

    /** Complete patch (parameters + envelopes + preset name + sample paths). */
    juce::ValueTree createStateTree() const;
    void loadStateTree (const juce::ValueTree& tree);
    /** Puts back a patch from the undo history (message thread): like
        loadStateTree(), except that the session-only parameters (sidechain, solo)
        keep their current values, and samples whose file hasn't changed aren't
        decoded again. */
    void restorePatch (const juce::ValueTree& patch);

    /** Undo and redo for the patch. It lives here, so it survives the editor
        closing; the editor feeds it user activity and polls it. Message thread. */
    drum::UndoHistory& getUndoHistory() noexcept { return undoHistory; }

    /** Increments whenever the patch is replaced wholesale (preset load, host
        recall), so the editor knows to re-read envelopes. */
    int getStateVersion() const noexcept { return stateVersion.load(); }

    /** Voices started since construction (MIDI, sidechain and audition hits).
        For tests and metering; updated once per block. */
    int getHitCount() const noexcept { return hitCount.load(); }
    /** Hits fired by the sidechain detector since construction (drives the activity light). */
    int getSidechainHitCount() const noexcept { return sidechainHitCount.load (std::memory_order_relaxed); }

    /** The most recent MIDI note played (60 = C3 until one arrives). With KEY on
        the whole drum transposes by (note - 60), so the display, the pitch ruler
        and the trigger pad all use this note to show and play what the host plays. */
    int getDisplayNote() const noexcept { return displayNote.load (std::memory_order_relaxed); }

    /** Sets the note the display and the trigger pad use (the editor's "reset to C3").
        The next MIDI note replaces it. */
    void setDisplayNote (int midiNote) noexcept { displayNote.store (juce::jlimit (0, 127, midiNote), std::memory_order_relaxed); }

    /** The EQ overlay's Listen: while `hz` > 0 the output is a band pass of the EQ's input
        at `hz` (and `q`). 0 stops it. Not part of the patch. Any thread. */
    void setEqListen (float hz, float q = 4.0f) noexcept
    {
        eqListenQ.store (q, std::memory_order_relaxed);
        eqListenHz.store (hz, std::memory_order_relaxed);
    }
    float getEqListenHz() const noexcept { return eqListenHz.load (std::memory_order_relaxed); }
    /** The gain EQ band `band` applied in the last block (a dynamic band moves). */
    float getEqAppliedGainDb (int band) const noexcept { return engine.getEqAppliedGainDb (band); }

    /** Plays a hit from the UI (the trigger pad), on getDisplayNote(). */
    void triggerAudition (float velocity) noexcept;

    float getPlayhead() const noexcept { return playhead.load(); }
    /** LFO `index`'s output, -1..1, as of the last block (for the UI). */
    float getLfoValue (int index) const noexcept { return lfoOutputs[(size_t) juce::jlimit (0, drum::kNumLfos - 1, index)].load (std::memory_order_relaxed); }
    /** Peak since the last call, per channel. */
    float takePeak (int channel) noexcept { return peaks[(size_t) channel].exchange (0.0f); }

    PresetManager& getPresets() noexcept { return presets; }

    /** The final output, for the spectrum analyzer (UI thread pulls). */
    drum::AnalyzerTap& getAnalyzerTap() noexcept { return analyzerTap; }

    /** Editor state that survives closing the window and reloading the session
        (but isn't part of presets). Message thread only. */
    struct UiState
    {
        /** The layer whose curves the display shows: 0 Body, 1 Noise, 2 Click, then extra
            layer k at 3 + k. Selecting a layer (the Body panel, a Layers tab) sets it. */
        int chartLayer = 0;
        static constexpr int numChartLayers = 3 + drum::kNumExtraLayers;
        /** The display's curve tab: 0 Pitch, 1 Level. */
        int chartTab = 0;
        int width = 0; // 0 = default size
        /** The display shows the LFO tab instead of the curves. */
        bool lfoTab = false;
        int layerTab = 0; // the Layers section's selected tab
        bool chartShowAll = false; // the display's ALL: every layer's curves on the tab together
        int fxModule = -1; // the effects module whose panel shows under the chain strip (an FxModule; -1 = the chain's first)
        /** Extra layers that keep their tab while muted, valid for layerTabsVersion
            (getStateVersion()): loading a patch forgets them. */
        std::array<bool, drum::kNumExtraLayers> layerTabs {};
        int layerTabsVersion = -1;
        float pitchFocus = 0.0f; // the pitch axis' FOCUS dial: 0 = 0 Hz - 16 kHz (the default), 1 = the band around the curves
        float pitchPan = 0.0f;   // axis units (about an octave each) the focused pitch axis is panned by
    };
    UiState uiState;

    static const juce::Identifier envelopesId;
    static const juce::Identifier presetNameId;
    static const juce::Identifier samplesId;
    /** Stored on every saved patch. Format 2: extra layers' Env choice has Click
        at index 2 and Flat at 3 (format 1, or none, had Flat at 2). Format 3: Body
        Style has the kick bodies (Round .. Tonal) before Sample (Sample was at 6).
        Format 4: extra layers have their own curves, and separate Pitch (PitchEnv) and
        Level (Env) choices; before, Env alone picked both, depending on the source. Click
        Type has more types before Sample (Sample was at 7). Format 5: the EQ has 8 bands
        and a Style; older patches load on the Digital style (the old 3-band EQ's curves).
        Format 6: the master chain has an order (the fxChainId property, FxChain::toString())
        and Clip Mode has Comp at the end; a patch without the property gets the chain it
        always had (FxChain::defaults()). Format 7: Body Style has the basic waveforms
        (Sine .. Triangle) before Sample (Sample was at 11). Format 8: the reverb has an
        engine, Cloud by default; older patches load on Classic, the reverb they had. */
    static const juce::Identifier formatVersionId;
    static constexpr int stateFormatVersion = 9;
    /** The master chain's order, on the state tree (see setFxChain()). */
    static const juce::Identifier fxChainId;
    static juce::Identifier envelopeTreeId (int envIndex);

private:
    /** Makes the one input bus a VST3 kAux bus instead of kMain, so hosts
        (Ableton Live in particular) offer it as a sidechain. */
    struct Vst3Extensions : juce::VST3ClientExtensions
    {
        bool getPluginHasMainInput() const override { return false; }
    };

    void publishTables();
    void loadStateTree (const juce::ValueTree& tree, bool reuseSamples);
    /** Switches the slot's layer on and over to its "Sample" source. */
    void selectSampleSource (drum::SampleSlot slot);

    juce::AudioProcessorValueTreeState apvts;
    drum::ParamReader paramReader;
    drum::DrumEngine engine;
    drum::TransientTrigger sidechainTrigger;
    const int sidechainBus;
    Vst3Extensions vst3Extensions;

    mutable juce::CriticalSection modelLock;
    std::array<drum::CurveEnvelope, drum::numEnvs> envelopes;
    std::unique_ptr<drum::EnvTables> bakeScratch;

    juce::SpinLock tableLock;
    std::unique_ptr<drum::EnvTables> publishedTables;
    std::atomic<bool> tablesDirty { true };

    std::atomic<int> stateVersion { 0 };
    std::atomic<int> envelopeEdits { 0 };
    std::atomic<float> auditionVelocity { 0.0f };
    std::atomic<float> eqListenHz { 0.0f }, eqListenQ { 4.0f };
    std::atomic<uint64_t> fxChainCode { drum::FxChain::defaults().pack() };
    std::atomic<int> displayNote { 60 };
    std::atomic<float> playhead { -1.0f };
    std::atomic<float> hostBpm { 120.0f };
    std::array<std::atomic<float>, drum::kNumLfos> lfoOutputs {};
    std::atomic<int> hitCount { 0 };
    std::atomic<int> sidechainHitCount { 0 };
    std::atomic<bool> sidechainResetPending { false };
    std::array<std::atomic<float>, 2> peaks {};

    drum::AnalyzerTap analyzerTap;
    drum::SampleBank samples;
    PresetManager presets;
    drum::UndoHistory undoHistory { *this }; // last: it reads everything above

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumFoundryProcessor)
};
