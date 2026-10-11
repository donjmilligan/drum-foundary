// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "PluginEditor.h"
#include "dsp/OfflineRenderer.h"
#include "ui/CompScope.h"
#include "ui/Controls.h"
#include "ui/EnvelopeEditor.h"
#include "ui/EqPanel.h"
#include "ui/FxChainStrip.h"
#include "ui/ReverbPanel.h"
#include "ui/HeaderBar.h"
#include "ui/LayerPanels.h"
#include "ui/SampleBrowser.h"
#include "ui/SpectrumAnalyzer.h"
#include "dsp/StyleInfo.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <cstring>
#include <type_traits>

using namespace drum;
using namespace drum::ui;

namespace
{
    // One rate for both the display and DRAG, so a single render serves both.
    constexpr double kRenderSampleRate = 48000.0;

    /** Bytewise equality. Padding bytes can make equal values compare unequal, which only
        costs a redundant render; it can never make different values compare equal. */
    template <typename T>
    bool sameBytes (const T& a, const T& b) noexcept
    {
        static_assert (std::is_trivially_copyable_v<T>);
        return std::memcmp (&a, &b, sizeof (T)) == 0;
    }
}

//==============================================================================
/** Renders the hit off the message thread, so automation during playback doesn't
    make the host's UI thread render a drum hit 30 times a second.

    Requests are coalesced through a generation counter: request() only bumps it, and
    the worker always renders the newest generation, so a burst of changes costs at
    most one render in flight plus one more. The generation is read *before* the
    parameters and envelopes, so a result is never older than the generation it's
    tagged with. Results are published in generation order, and an older one never
    replaces a newer one.

    The newest result doubles as the DRAG export, and is only used for it while no
    request has arrived since it was started.

    The render includes the imported samples. It polls the processor's sample
    generation, so loading, clearing or restoring a sample re-renders by itself.

    It renders the note set with setNote(): with KEY on, the last note played, so
    the display shows what the host is actually playing. */
class DrumFoundryEditor::WaveformRenderer : private juce::Thread,
                                            private juce::Timer
{
public:
    struct Hit
    {
        juce::AudioBuffer<float> audio;
        CompCaptures comps;                 // what each compressor did, aligned with `audio`
        float lengthMs = 0.0f;
    };
    using HitPtr = std::shared_ptr<const Hit>;

    explicit WaveformRenderer (DrumFoundryProcessor& p)
        : juce::Thread ("Drum Foundry waveform"), processor (p)
    {
    }

    ~WaveformRenderer() override
    {
        stopTimer();

        // A render doesn't poll threadShouldExit(), so allow for the longest one.
        signalThreadShouldExit();
        notify();
        stopThread (4000);
    }

    void start()
    {
        lastSampleGeneration = processor.getSampleGeneration();
        startThread (juce::Thread::Priority::low);
        startTimerHz (10);
    }

    /** The MIDI note to render (60 = C3). Takes effect with the next request. */
    void setNote (int midiNote) noexcept { note.store (midiNote); }
    int getNote() const noexcept { return note.load(); }

    /** Asks for a render of the current sound. Message thread only (notify() can lock,
        so the audio thread must never call this). */
    void request()
    {
        ++requested;
        notify();
    }

    /** Renders the current sound on the calling thread and publishes it. This is itself
        a request, so it always supersedes whatever the worker is doing. */
    HitPtr renderNow()
    {
        const auto generation = ++requested;
        const auto params = processor.readParams();
        processor.bakeTables (*callerTables);
        auto hit = render (params, *callerTables, getSamples(), note.load());
        publish (hit, generation);
        return hit;
    }

    /** The newest result if it hasn't been taken yet, otherwise nullptr. */
    HitPtr takeNew()
    {
        const juce::ScopedLock sl (lock);
        return std::exchange (unseen, nullptr);
    }

    /** The newest result if nothing has been requested since it was started, otherwise nullptr. */
    HitPtr getCurrent() const
    {
        const juce::ScopedLock sl (lock);
        return latestGeneration == requested.load() ? latest : nullptr;
    }

private:
    using Samples = std::array<SamplePtr, numSampleSlots>;

    Samples getSamples() const
    {
        Samples s;
        for (int i = 0; i < numSampleSlots; ++i)
            s[(size_t) i] = processor.getSample ((SampleSlot) i);
        return s;
    }

    void timerCallback() override
    {
        if (const int g = processor.getSampleGeneration(); g != lastSampleGeneration)
        {
            lastSampleGeneration = g;
            request();
        }
    }

    void run() override
    {
        auto tables = std::make_unique<EnvTables>();
        auto lastTables = std::make_unique<EnvTables>();
        DrumParams lastParams;
        Samples lastSamples;
        int lastNote = -1;
        HitPtr last; // this thread's newest render, made from lastParams + lastTables + lastSamples

        while (! threadShouldExit())
        {
            const auto generation = requested.load();
            if (generation <= getLatestGeneration())
            {
                wait (-1);
                continue;
            }

            const auto params = processor.readParams();
            processor.bakeTables (*tables);
            auto samples = getSamples();
            const int renderNote = note.load();

            // If nothing audible changed (a re-sent value, a click on a point), re-tag
            // the last render instead of repeating it.
            if (last == nullptr || ! sameBytes (params, lastParams) || ! sameBytes (*tables, *lastTables)
                || samples != lastSamples || renderNote != lastNote)
            {
                last = render (params, *tables, samples, renderNote);
                lastParams = params;
                lastNote = renderNote;
                lastSamples = std::move (samples); // holds them until the next render
                std::swap (tables, lastTables);
            }

            publish (last, generation);
        }
    }

    /** `samples` keeps every sample alive for the length of the render. */
    static HitPtr render (const DrumParams& params, const EnvTables& tables, const Samples& samples, int midiNote)
    {
        SampleSet set;
        for (size_t i = 0; i < samples.size(); ++i)
            set.slots[i] = samples[i].get();

        auto hit = std::make_shared<Hit>();
        hit->audio = renderHit (params, tables, kRenderSampleRate, midiNote, 1.0f, 0x5EEDu, set, nullptr, &hit->comps);
        hit->lengthMs = params.lengthMs;
        return hit;
    }

    void publish (HitPtr hit, uint64_t generation)
    {
        const juce::ScopedLock sl (lock);
        if (generation <= latestGeneration)
            return; // something at least as new is already out

        latestGeneration = generation;
        if (hit != latest)
        {
            latest = hit;
            unseen = std::move (hit);
        }
    }

    uint64_t getLatestGeneration() const
    {
        const juce::ScopedLock sl (lock);
        return latestGeneration;
    }

    DrumFoundryProcessor& processor;
    int lastSampleGeneration = 0; // message thread
    std::atomic<uint64_t> requested { 1 };
    std::atomic<int> note { 60 };
    std::unique_ptr<EnvTables> callerTables = std::make_unique<EnvTables>(); // renderNow() only

    juce::CriticalSection lock; // guards the three below; only ever held briefly
    uint64_t latestGeneration = 0;
    HitPtr latest, unseen;
};

//==============================================================================
/** The sample browser as a drawer over the left of the lower half, with a close
    button and a shadow along its right edge. The shadow margin lets clicks through. */
class BrowserDrawer : public juce::Component
{
public:
    static constexpr int shadowWidth = 14;

    explicit BrowserDrawer (DrumFoundryProcessor& p) : browser (p)
    {
        setInterceptsMouseClicks (false, true);
        addAndMakeVisible (browser);

        closeButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xc3\x97")));
        closeButton.setTooltip ("Close the browser");
        closeButton.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (closeButton);
    }

    std::function<void()> onClose;

    void paint (juce::Graphics& g) override
    {
        const auto b = browser.getBounds().toFloat();
        const auto shadow = juce::Rectangle<float> (b.getRight() - 6.0f, b.getY() + 4.0f, (float) shadowWidth + 6.0f, b.getHeight() - 4.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.45f), shadow.getX(), 0.0f,
                                                 juce::Colours::transparentBlack, shadow.getRight(), 0.0f, false));
        g.fillRoundedRectangle (shadow, 6.0f);
    }

    void resized() override
    {
        browser.setBounds (getLocalBounds().withTrimmedRight (shadowWidth));
        closeButton.setBounds (browser.getRight() - 32, browser.getY() + 5, 22, 20); // in the browser's title row
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key != juce::KeyPress::escapeKey || onClose == nullptr)
            return false;
        onClose();
        return true;
    }

    SampleBrowser browser;

private:
    juce::TextButton closeButton;
};

//==============================================================================
/** Everything, laid out at the base size. The editor scales this as a whole.

    Top to bottom it follows the signal: the layers (Body, then a tabbed section
    with Noise, Click and the extra layers), the envelope display (with the layers'
    timing lanes), then the effects and, at the bottom right, the output (spectrum +
    output filter, gain and clipper). The browser opens as a drawer over the left of
    the lower half, so the layers' sample slots stay in view while browsing. The
    sidechain trigger lives in the settings (the header's gear). */
class DrumFoundryEditor::MainPanel : public juce::Component,
                                     public juce::DragAndDropContainer // LFO handles onto knobs
{
public:
    /** Presses anywhere on the Body panel (it listens to it and everything in it): the
        display shows Body's curves, even from the LFO tab. */
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || (e.eventComponent != &body && ! body.isParentOf (e.eventComponent)))
            return;
        layerTouched (-1, true);
    }

    /** The user touched a layer: Body (-1) or a Layers tab. The display shows its
        curves (an extra layer's own, or the core layer's it's coupled to).
        `fromTab`: the layer was picked by its tab or its M / S / Ø, so its curves come
        back on show if the display is on the LFO tab. A press on a knob leaves the LFO
        tab where it is (it's where you'd be tuning that knob's modulation). */
    void layerTouched (int tab, bool fromTab = false)
    {
        // Chart layers: 0 Body, 1 Noise, 2 Click, 3 + k extra layer k (the Layers tabs
        // are Noise, Click, then the extra layers).
        static_assert (LayersSection::noiseTab == 0 && LayersSection::clickTab == 1 && LayersSection::firstExtraTab == 2);
        const int layer = tab < 0 ? 0 : tab + 1;
        if (juce::isPositiveAndBelow (layer, DrumFoundryProcessor::UiState::numChartLayers))
            display.focusLayer (layer, fromTab);
    }

    explicit MainPanel (DrumFoundryProcessor& p)
        : processor (p), header (p), display (p), analyzer (p), drawer (p),
          body (p, "Body", colours::body, slotBody, pid::bodyOn, pid::bodySolo, pid::bodyStyle, pid::bodyInvert, &bodyUsesSample, 4),
          noise (p, "Noise", colours::noise, slotNoise, pid::noiseOn, pid::noiseSolo, pid::noiseStyle, pid::noiseInvert, &noiseUsesSample, 5, true),
          click (p, "Click", colours::click, slotClick, pid::clickOn, pid::clickSolo, pid::clickType, pid::clickInvert, &clickUsesSample, 5, true),
          layers (p, noise, click), output ("Output", colours::output, 2),
          drive ("Drive", colours::drive), eq ("EQ", colours::eq), master ("Master", colours::master),
          reverb ("Reverb", colours::reverb), trigger ("Sidechain Trigger", colours::trigger),
          drive2 ("Drive 2", colours::drive), comp1 ("Comp", FxChainStrip::colourFor (fxComp1)),
          comp2 ("Comp 2", FxChainStrip::colourFor (fxComp2)), clip1 ("Clip", FxChainStrip::colourFor (fxClip1)),
          clip2 ("Clip 2", FxChainStrip::colourFor (fxClip2)), scActivity (colours::trigger),
          reverbToOutput (FlowArrow::Direction::right), eqOverlay (p), reverbOverlay (p)
    {
        auto& s = p.getState();

        auto knob = [&] (const juce::String& id, const char* caption, juce::Colour c, bool bipolar = false) -> Knob&
        {
            knobs.push_back (std::make_unique<Knob> (s, id, caption, c, bipolar));
            return *knobs.back();
        };
        auto choice = [&] (const juce::String& id, const char* caption, juce::Colour c) -> ChoiceSelector&
        {
            choices.push_back (std::make_unique<ChoiceSelector> (s, id, caption, c));
            return *choices.back();
        };
        auto toggle = [&] (const char* id, const char* text, juce::Colour c, const char* tip) -> ToggleSwitch*
        {
            toggles.push_back (std::make_unique<ToggleSwitch> (s, id, text, c, tip));
            return toggles.back().get();
        };

        // Offset: when the layer starts, from 100 ms early (its start cut off) to 100 ms
        // late. The display's timing lanes drag the same parameters.
        auto offsetKnob = [&] (const char* id, juce::Colour c) -> Knob&
        {
            auto& k = knob (id, "Offset", c, true);
            k.getSlider().setTooltip ("Offset: starts the layer later (+) or earlier (-). Earlier cuts off the part before the hit.\n"
                                      "Also draggable on the display's timing lanes.");
            return k;
        };

        // Layers. The captions of style-dependent knobs come from StyleInfo.h.
        auto& tuneKnob = knob (pid::tune, "Tune", colours::body);
        tuneKnob.getSlider().setTooltip ("Tune: the pitch the body settles on (C3 plays it). The pitch curve's end point is Tune:\n"
                                         "drag it on the Pitch tab to retune without moving the rest of the curve.");
        body.addKnob (tuneKnob);
        body.addKnob (knob (pid::bodyShape, "Shape", colours::body), &bodyShapeCaption);
        body.addKnob (knob (pid::bodyLevel, "Level", colours::body));
        body.addKnob (knob (pid::otLevel, "Overtone", colours::body), &bodyOvertoneCaption);
        body.addKnob (knob (pid::otRatio, "OT Ratio", colours::body), &bodyRatioCaption);
        body.addKnob (knob (pid::modes, "Head Modes", colours::body), &bodyModesCaption);
        body.addSampleKnob (knob (pid::bodySmpTune, "Sample Tune", colours::body, true));
        body.addKnob (offsetKnob (pid::bodyOffset, colours::body));

        noise.addKnob (knob (pid::noiseLevel, "Level", colours::noise));
        noise.addKnob (knob (pid::noiseHP, "Low Cut", colours::noise));
        noise.addKnob (knob (pid::noiseLP, "High Cut", colours::noise));
        noise.addKnob (knob (pid::noiseColor, "Color", colours::noise), &noiseColorCaption);
        noise.addKnob (knob (pid::noiseCouple, "Coupling", colours::noise), &noiseCouplingCaption);
        noise.addKnob (knob (pid::noiseWidth, "Width", colours::noise), &noiseWidthCaption);
        noise.addKnob (knob (pid::noiseGrit, "Grit", colours::noise), &noiseGritCaption);
        noise.addSampleKnob (knob (pid::noiseSmpTune, "Sample Tune", colours::noise, true));
        noise.addKnob (offsetKnob (pid::noiseOffset, colours::noise));

        click.addKnob (knob (pid::clickLevel, "Level", colours::click));
        click.addKnob (knob (pid::clickTone, "Tone", colours::click), &clickToneCaption);
        click.addKnob (knob (pid::clickLength, "Length", colours::click), &clickLengthCaption);
        click.addSampleKnob (knob (pid::clickSmpTune, "Sample Tune", colours::click, true));
        click.addKnob (offsetKnob (pid::clickOffset, colours::click));

        // Effects, each with its own power switch.
        drive.setPowerParameter (s, pid::driveOn);
        eq.setPowerParameter (s, pid::eqOn);
        // The full-screen EQ (all 8 bands, styles, dynamics); the panel has a small version of its graph.
        eqExpand.setTooltip ("Open the full EQ: 8 bands (bells, shelves, cuts, notches, tilt), styles, dynamic bands, "
                             "listening, and finding resonances in the hit.");
        eqExpand.onClick = [this] { setEqVisible (true); };
        eq.setHeaderComponent (eqExpand, 46);
        revExpand.setTooltip ("Open the full reverb: its response, the engine (Cloud or Classic) and every control");
        revExpand.onClick = [this] { setReverbVisible (true); };
        reverb.setHeaderComponent (revExpand, 46);
        reverbOverlay.onClose = [this] { setReverbVisible (false); };
        eqOverlay.onClose = [this] { setEqVisible (false); };
        reverb.setPowerParameter (s, pid::revOn);

        drive.addControl (choice (pid::driveType, "Type", colours::drive), 1.2f);
        drive.addControl (knob (pid::driveAmount, "Amount", colours::drive));
        drive.addControl (knob (pid::driveMix, "Mix", colours::drive));

        // A small version of the full EQ: drag the nodes, scroll for Q; FULL (or a double-click) for the rest.
        miniEq.setCompact (true);
        miniEq.onOpenFull = [this] { setEqVisible (true); };
        eq.addControl (miniEq, 4.0f);

        master.addControl (knob (pid::velSens, "Velocity", colours::master));
        master.addStack ({ toggle (pid::keytrack, "KEY", colours::master,
                                   "Pitch follows the MIDI note: C3 plays at Tune, and other notes transpose the whole drum.\n"
                                   "The display follows the last note played (see the KEY badge above it)."),
                           toggle (pid::choke, "CHOKE", colours::master, "Each new hit cuts off the previous one") },
                         1.1f);

        // Reverb, with the gate's switch over its own two knobs (Time and Fade).
        reverb.addControl (knob (pid::revMix, "Mix", colours::reverb));
        reverb.addControl (knob (pid::revSize, "Size", colours::reverb));
        reverb.addControl (knob (pid::revDecay, "Decay", colours::reverb));
        reverb.addControl (knob (pid::revPreDelay, "Pre-Delay", colours::reverb));
        reverb.addControl (knob (pid::revLowCut, "Low Cut", colours::reverb));
        reverb.addControl (knob (pid::revHighCut, "High Cut", colours::reverb));
        auto* gateSwitch = toggle (pid::revGate, "GATE", colours::reverb,
                                   "Cut the reverb tail Time after each hit, fading out over Fade");
        auto& gateTime = knob (pid::revGateTime, "Time", colours::reverb);
        auto& gateFade = knob (pid::revGateFade, "Fade", colours::reverb);
        reverb.addControl (gateTime);
        reverb.addControl (gateFade);
        reverb.setTopComponent (*gateSwitch, 6);
        dimWhileOff (*gateSwitch, { &gateTime, &gateFade });

        // The chain's other modules: a second Drive, two compressors and two clippers.
        drive2.setPowerParameter (s, pid::drive2On);
        drive2.addControl (choice (pid::drive2Type, "Type", colours::drive), 1.2f);
        drive2.addControl (knob (pid::drive2Amount, "Amount", colours::drive));
        drive2.addControl (knob (pid::drive2Mix, "Mix", colours::drive));

        for (int n = 0; n < 2; ++n)
        {
            auto& section = n == 0 ? comp1 : comp2;
            const auto c = FxChainStrip::colourFor (fxComp1);
            const auto id = [n] (const char* suffix) { return compParamId (n, suffix); };
            section.setPowerParameter (s, id (pid::compOn));
            section.addControl (knob (id (pid::compThresh), "Threshold", c));
            section.addControl (knob (id (pid::compRatio), "Ratio", c));
            section.addControl (knob (id (pid::compAttack), "Attack", c));
            section.addControl (knob (id (pid::compRelease), "Release", c));
            section.addControl (knob (id (pid::compKnee), "Knee", c));
            section.addControl (knob (id (pid::compMakeup), "Makeup", c));
            section.addControl (knob (id (pid::compMix), "Mix", c));
            auto& style = choice (id (pid::compStyle), "Style", c);
            style.getComboBox().setTooltip ("Character, modelled on classic compressors:\n"
                                            "Clean: transparent, feed-forward\n"
                                            "FET: 1176-style, fast and aggressive, grit as it digs in\n"
                                            "Opto: LA-2A-style, smooth, a release that slows the longer it works\n"
                                            "Bus: SSL-style glue, auto release\n"
                                            "Punch: dbx 160-style, RMS, snappy on drums");
            section.addControl (style, 1.2f);
            // What it does to the hit, over the knobs.
            auto& scope = compScopes[(size_t) n];
            section.setTopComponent (scope, 0, 86);
        }
        for (int n = 0; n < 2; ++n)
        {
            auto& section = n == 0 ? clip1 : clip2;
            const auto c = FxChainStrip::colourFor (fxClip1);
            const auto id = [n] (const char* suffix) { return chainClipParamId (n, suffix); };
            section.setPowerParameter (s, id (pid::chainClipOn));
            section.addControl (choice (id (pid::chainClipShape), "Shape", c), 1.2f);
            section.addControl (knob (id (pid::chainClipDrive), "Drive", c));
            section.addControl (knob (id (pid::chainClipCeiling), "Ceiling", c));
        }

        // The chain strip: the modules in their order. Clicking one shows its panel.
        chainStrip.getChain = [&p] { return p.getFxChain(); };
        chainStrip.setChain = [&p] (const FxChain& c)
        {
            p.getUndoHistory().noteUserActivity();
            p.setFxChain (c);
        };
        chainStrip.isOn = [&s] (int m)
        {
            const auto* v = s.getRawParameterValue (moduleOnParam (m));
            return v != nullptr && v->load() > 0.5f;
        };
        chainStrip.toggleOn = [&s] (int m)
        {
            if (auto* param = s.getParameter (moduleOnParam (m)))
            {
                param->beginChangeGesture();
                param->setValueNotifyingHost (param->getValue() > 0.5f ? 0.0f : 1.0f);
                param->endChangeGesture();
            }
        };
        chainStrip.readout = [&p] (int m) -> juce::String
        {
            if (fxKindOf (m) != fxKindComp)
                return {};
            const float gr = p.getCompReductionDb (m == fxComp1 ? 0 : 1);
            return gr < -0.1f ? juce::String (gr, 1) + " dB" : juce::String();
        };
        chainStrip.onSelect = [this] (int m) { showModule (m); };
        const int shown = p.uiState.fxModule;
        showModule (juce::isPositiveAndBelow (shown, (int) numFxModules) ? shown
                    : p.getFxChain().length > 0 ? p.getFxChain()[0] : fxDrive1);

        // Sidechain trigger (in the settings).
        auto* scSwitch = toggle (pid::scTrigger, "SIDECHAIN", colours::trigger,
                                 "Trigger hits from transients on the sidechain input");
        auto& scThreshold = knob (pid::scThreshold, "Threshold", colours::trigger);
        auto& scRetrigger = knob (pid::scRetrigger, "Retrigger", colours::trigger);
        trigger.setTopComponent (*scSwitch);
        trigger.addControl (scThreshold, 1.3f);
        trigger.addControl (scRetrigger, 1.3f);
        scActivity.setTooltip ("Flashes when a sidechain transient fires a hit");
        trigger.setHeaderComponent (scActivity);
        dimWhileOff (*scSwitch, { &scThreshold, &scRetrigger }, [this] (bool on) { scActivity.setArmed (on); });

        // Output: the filter the analyzer's handles drag, then gain and the clipper.
        auto& hpSlope = choice (pid::outHPSlope, "Slope", colours::output);
        auto& lpSlope = choice (pid::outLPSlope, "Slope", colours::output);
        hpSlope.getComboBox().setTooltip ("Low cut slope (or scroll over the HP handle)");
        lpSlope.getComboBox().setTooltip ("High cut slope (or scroll over the LP handle)");
        auto& outHP = knob (pid::outHP, "Low Cut", colours::output);
        auto& outLP = knob (pid::outLP, "High Cut", colours::output);
        outHP.getSlider().setTooltip ("Output low cut: the last filter before the output (fully left = off)");
        outLP.getSlider().setTooltip ("Output high cut: the last filter before the output (fully right = off)");
        auto& clipMode = choice (pid::clip, "Clip", colours::clipZone);
        clipMode.getComboBox().setTooltip ("Output clipper: what happens to anything over the ceiling (Off lets it through).\n"
                                           "The Body and Noise tabs of the display show it too, with its Drive and Ceiling.");
        for (auto [c, pill] : std::initializer_list<std::pair<juce::Component*, int>> {
                 { &outHP, 0 }, { &hpSlope, 0 }, { &outLP, 0 }, { &lpSlope, 0 },
                 { &knob (pid::outGain, "Output", colours::output), 0 }, { &clipMode, 0 } })
        {
            output.addAndMakeVisible (c);
            output.addCell (*c, pill);
        }

        // Every slot's BROWSE opens the browser on that slot.
        body.getSlotWidget().onBrowse = [this] (SampleSlot slot) { browseSlot (slot); };
        layers.onBrowse = [this] (SampleSlot slot) { browseSlot (slot); };

        auto& browser = drawer.browser;
        browser.onTargetSlotChanged = [this] (SampleSlot slot)
        {
            layers.showSlot (slot);
            updateBrowseTarget();
        };
        browser.onFileLoaded = [this] (SampleSlot slot, const juce::File&) { layers.showSlot (slot); };
        header.onBrowserClicked = [this] { setBrowserVisible (! drawer.isVisible()); };
        drawer.onClose = [this] { setBrowserVisible (false); };

        settings.setTriggerSection (trigger);
        settings.onClose = [this] { setSettingsVisible (false); };

        // Touching a layer points the display at its curves (see layerTouched()).
        body.addMouseListener (this, true);
        for (auto* button : std::initializer_list<juce::Button*> { body.getMuteButton(), body.getSoloButton(), &body.getInvertButton() })
            if (button != nullptr)
                alsoOnClick (*button, [this] { layerTouched (-1, true); });
        layers.onLayerTouched = [this] (int tab, bool fromTab) { layerTouched (tab, fromTab); };
        // A layer picked on the display (a dimmed curve with ALL on): its panel follows.
        display.onLayerFocused = [this] (int layer)
        {
            if (layer > 0)
                layers.selectTab (layer - 1);
        };
        header.onSettingsClicked = [this] { setSettingsVisible (! settings.isVisible()); };

        for (auto* c : std::initializer_list<juce::Component*> { &header, &body, &layers, &display,
                                                                 &master, &chainStrip, &analyzer, &output })
            addAndMakeVisible (c);
        for (auto* section : moduleSections())
            addChildComponent (section); // showModule() shows one at a time

        // The arrow goes over the gap between the effects and the output, so it's added after them.
        addAndMakeVisible (reverbToOutput);

        addChildComponent (drawer);
        addChildComponent (settings);
        addChildComponent (overlay);
        addChildComponent (eqOverlay);
        addChildComponent (reverbOverlay);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (colours::background);
    }

    void resized() override
    {
        constexpr int margin = 16, gap = 8;
        auto area = getLocalBounds();

        header.setBounds (area.removeFromTop (58));
        area = area.reduced (margin, 0);
        area.removeFromTop (gap);
        area.removeFromBottom (12);

        // Layers: Body, then the tabbed Noise / Click / extra layers, which also carry
        // the tabs and the selectors next to their sample slots.
        layoutWeighted (area.removeFromTop (232), { { &body, 1.0f }, { &layers, 1.0f } });
        area.removeFromTop (10);

        // Effects and output along the bottom. The output (spectrum, then the filter,
        // gain and clipper) sits at the bottom right, where the signal leaves.
        auto bottom = area.removeFromBottom (264);
        auto out = bottom.removeFromRight (620);
        output.setBounds (out.removeFromRight (184));
        out.removeFromRight (gap);
        analyzer.setBounds (out);
        bottom.removeFromRight (gap);

        // The effects chain: its strip (the modules in signal order) over the selected
        // module's panel, then out to the analyzer and output. Master isn't in the chain,
        // so it takes the corner on the left.
        chainStrip.setBounds (bottom.removeFromTop (32));
        bottom.removeFromTop (gap);
        master.setBounds (bottom.removeFromLeft (150));
        bottom.removeFromLeft (gap);
        for (auto* section : moduleSections())
            section->setBounds (bottom);

        reverbToOutput.setBounds (juce::Rectangle<int> (FlowArrow::size, FlowArrow::size)
                                      .withCentre ({ (bottom.getRight() + analyzer.getX()) / 2, bottom.getCentreY() }));

        area.removeFromBottom (10);
        display.setBounds (area);

        // The browser drawer covers the left of the display and the effects below it.
        drawer.setBounds (margin, display.getY(), 300 + BrowserDrawer::shadowWidth, getHeight() - 12 - display.getY());

        settings.setBounds (getLocalBounds());
        overlay.setBounds (getLocalBounds());
        eqOverlay.setBounds (getLocalBounds());
        reverbOverlay.setBounds (getLocalBounds());
    }

    /** Re-reads the layer switches and styles (from the editor's timer). */
    void update()
    {
        body.update();
        layers.update(); // and the Noise and Click pages
        chainStrip.refresh();
    }

    /** Shows module `m`'s panel under the chain strip (and marks it there). */
    void showModule (int m)
    {
        if (! juce::isPositiveAndBelow (m, (int) numFxModules))
            return;
        for (int k = 0; k < numFxModules; ++k)
            moduleSection (k).setVisible (k == m);
        chainStrip.setSelected (m);
        processor.uiState.fxModule = m;
    }

    Section& moduleSection (int m)
    {
        switch (m)
        {
            case fxDrive1: return drive;
            case fxDrive2: return drive2;
            case fxEq:     return eq;
            case fxComp1:  return comp1;
            case fxComp2:  return comp2;
            case fxClip1:  return clip1;
            case fxClip2:  return clip2;
            case fxReverb:
            default:       return reverb;
        }
    }

    std::vector<Section*> moduleSections()
    {
        std::vector<Section*> v;
        for (int m = 0; m < numFxModules; ++m)
            v.push_back (&moduleSection (m));
        return v;
    }

    /** The parameter that switches module `m` on. */
    static juce::String moduleOnParam (int m)
    {
        switch (m)
        {
            case fxDrive1: return pid::driveOn;
            case fxDrive2: return pid::drive2On;
            case fxEq:     return pid::eqOn;
            case fxComp1:  return compParamId (0, pid::compOn);
            case fxComp2:  return compParamId (1, pid::compOn);
            case fxClip1:  return chainClipParamId (0, pid::chainClipOn);
            case fxClip2:  return chainClipParamId (1, pid::chainClipOn);
            case fxReverb:
            default:       return pid::revOn;
        }
    }

    void setBrowserVisible (bool shouldBeVisible)
    {
        // The browser itself too: hiding it is what stops its audition.
        drawer.browser.setVisible (shouldBeVisible);
        drawer.setVisible (shouldBeVisible);
        if (shouldBeVisible)
            drawer.toFront (false);
        header.setBrowserShown (shouldBeVisible);
        updateBrowseTarget();
    }

    void setReverbVisible (bool shouldBeVisible)
    {
        reverbOverlay.setVisible (shouldBeVisible);
        if (shouldBeVisible)
        {
            reverbOverlay.toFront (false);
            if (reverbOverlay.isShowing())
                reverbOverlay.grabKeyboardFocus();
        }
    }

    void setEqVisible (bool shouldBeVisible)
    {
        eqOverlay.setVisible (shouldBeVisible);
        if (shouldBeVisible)
        {
            eqOverlay.toFront (false);
            if (eqOverlay.isShowing())
                eqOverlay.grabKeyboardFocus();
        }
        else
        {
            processor.setEqListen (0.0f); // a Listen held as it closed
        }
    }

    void setSettingsVisible (bool shouldBeVisible)
    {
        settings.setVisible (shouldBeVisible);
        header.setSettingsShown (shouldBeVisible);
        if (shouldBeVisible)
        {
            settings.toFront (false);
            if (settings.isShowing())
                settings.grabKeyboardFocus();
        }
    }

    /** Opens the browser pointed at `slot`. */
    void browseSlot (SampleSlot slot)
    {
        drawer.browser.setTargetSlot (slot);
        layers.showSlot (slot);
        setBrowserVisible (true);
    }

    ~MainPanel() override
    {
        body.removeMouseListener (this);
    }

    DrumFoundryProcessor& processor;
    HeaderBar header;
    DisplayPanel display;
    SpectrumAnalyzer analyzer;
    BrowserDrawer drawer;
    LayerPanel body, noise, click; // Noise and Click are pages in `layers`
    LayersSection layers;
    GridPanel output;
    Section drive, eq, master, reverb, trigger;
    Section drive2, comp1, comp2, clip1, clip2;  // the chain's other modules
    FxChainStrip chainStrip;
    ActivityLight scActivity;
    FlowArrow reverbToOutput;                    // the chain out to the analyzer and output
    std::array<CompScope, 2> compScopes { CompScope (FxChainStrip::colourFor (fxComp1)), CompScope (FxChainStrip::colourFor (fxComp1)) };
    SettingsOverlay settings;
    SaveOverlay overlay;
    juce::TextButton eqExpand { "FULL" };        // on the EQ section: opens eqOverlay
    EqOverlay eqOverlay;                         // the full-screen EQ
    juce::TextButton revExpand { "FULL" };       // on the Reverb section: opens reverbOverlay
    ReverbOverlay reverbOverlay;                 // the full-screen reverb
    EqGraph miniEq { processor };                // the EQ section's small graph

private:
    /** Outlines the layer the browser loads into, while it's open. */
    void updateBrowseTarget()
    {
        const bool open = drawer.isVisible();
        const auto target = drawer.browser.getTargetSlot();
        body.setHighlighted (open && target == slotBody);
        layers.setHighlighted (open && target != slotBody);
    }

    static void layoutWeighted (juce::Rectangle<int> row, std::initializer_list<std::pair<juce::Component*, float>> items)
    {
        constexpr int gap = 8;
        float total = 0.0f;
        for (const auto& item : items)
            total += item.second;

        const float unit = (float) (row.getWidth() - gap * ((int) items.size() - 1)) / total;
        for (const auto& [c, weight] : items)
        {
            // The last one takes whatever rounding left over.
            const bool last = c == std::prev (items.end())->first;
            c->setBounds (row.removeFromLeft (last ? row.getWidth() : juce::roundToInt (unit * weight)));
            row.removeFromLeft (gap);
        }
    }

    static void layoutRow (juce::Rectangle<int> row, std::initializer_list<Section*> sections)
    {
        std::vector<std::pair<juce::Component*, float>> items;
        for (auto* s : sections)
            items.emplace_back (s, s->getSlotCount());

        constexpr int gap = 8;
        float totalSlots = 0.0f;
        for (const auto& item : items)
            totalSlots += item.second;

        const float unit = (float) (row.getWidth() - gap * ((int) items.size() - 1)) / totalSlots;
        for (size_t i = 0; i < items.size(); ++i)
        {
            const int w = i + 1 == items.size() ? row.getWidth() : juce::roundToInt (unit * items[i].second);
            items[i].first->setBounds (row.removeFromLeft (w));
            row.removeFromLeft (gap);
        }
    }

    /** Dims `dimmed` while `toggle` is off (they stay editable), and passes the state on
        to `onChange`, if given. */
    static void dimWhileOff (ToggleSwitch& toggle, std::initializer_list<juce::Component*> dimmed,
                             std::function<void (bool)> onChange = {})
    {
        auto& button = toggle.getButton();
        auto update = [b = &button, components = std::vector<juce::Component*> (dimmed), onChange]
        {
            const bool on = b->getToggleState();
            for (auto* c : components)
                c->setAlpha (on ? 1.0f : 0.55f);
            if (onChange)
                onChange (on);
        };
        update();
        button.onStateChange = std::move (update); // also fires when the attachment syncs it
    }

    std::vector<std::unique_ptr<Knob>> knobs;
    std::vector<std::unique_ptr<ChoiceSelector>> choices;
    std::vector<std::unique_ptr<ToggleSwitch>> toggles;
};

//==============================================================================
/** Tells the undo history what the user does anywhere in the editor: every press,
    drag, double-click and scroll counts as activity, a button held down (or a
    scroll in the last moment) as a gesture still going on, and a mouse-up is the
    moment to commit it. */
class DrumFoundryEditor::ActivityWatcher : public juce::MouseListener
{
public:
    explicit ActivityWatcher (DrumFoundryEditor& e) : editor (e) {}

    bool isBusy()
    {
        // Check the real button state too: an external drag (DRAG) can swallow the mouse-up.
        if (buttonDown && ! juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown())
            buttonDown = false;
        return buttonDown || juce::Time::getMillisecondCounterHiRes() - lastWheelMs < wheelGestureMs;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        buttonDown = true;
        touched (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override        { touched (e); }
    void mouseDoubleClick (const juce::MouseEvent& e) override { touched (e); }

    void mouseUp (const juce::MouseEvent&) override
    {
        buttonDown = false;
        editor.pollUndo();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails&) override
    {
        lastWheelMs = juce::Time::getMillisecondCounterHiRes();
        touched (e);
    }

private:
    /** A scroll over a knob is one step once it has paused this long (ms). */
    static constexpr double wheelGestureMs = 400.0;

    void touched (const juce::MouseEvent& e)
    {
        // Clicking undo or redo isn't an edit (and mustn't make a change that lands
        // meanwhile, such as automation, into a step that clears the redo list).
        if (dynamic_cast<HistoryButton*> (e.originalComponent) == nullptr)
            editor.drumProcessor.getUndoHistory().noteUserActivity();
    }

    DrumFoundryEditor& editor;
    bool buttonDown = false;
    double lastWheelMs = -1.0e9;
};

//==============================================================================
DrumFoundryEditor::DrumFoundryEditor (DrumFoundryProcessor& p)
    : AudioProcessorEditor (&p), drumProcessor (p), renderer (std::make_unique<WaveformRenderer> (p))
{
    // Read before anything resizes us (resized() records the current width).
    const int savedWidth = drumProcessor.uiState.width;

    setLookAndFeel (&lookAndFeel);

    main = std::make_unique<MainPanel> (p);
    addAndMakeVisible (*main);

    main->header.onSaveClicked = [this] { main->overlay.show (drumProcessor.getPresets().getCurrentName()); };
    main->header.dragExport.exportFile = [this] { return exportWav(); };
    main->overlay.onSave = [this] (const juce::String& name)
    {
        drumProcessor.getPresets().saveUserPreset (name);
        main->header.refreshPresets();
    };
    main->display.getCanvas().onEdited = [this] { waveformDirty = true; };
    main->settings.onScaleChosen = [this] (float scale)
    {
        int w = juce::roundToInt ((float) baseWidth * scale);
        if (const auto* c = getConstrainer())
            w = juce::jlimit (c->getMinimumWidth(), c->getMaximumWidth(), w);
        setSize (w, juce::roundToInt ((float) w * (float) baseHeight / (float) baseWidth));
    };

    // Undo / redo. The history lives in the processor, so it outlives the editor.
    main->header.onUndo = [this] { undo(); };
    main->header.onRedo = [this] { redo(); };
    activityWatcher = std::make_unique<ActivityWatcher> (*this);
    addMouseListener (activityWatcher.get(), true);
    updateHistoryButtons();

    for (const auto& id : allParameterIds())
        drumProcessor.getState().addParameterListener (id, this);

    lastStateVersion = drumProcessor.getStateVersion();
    updateDisplayNote();
    lastSidechainHits = drumProcessor.getSidechainHitCount();

    constexpr int minWidth = baseWidth * 7 / 10, maxWidth = baseWidth * 8 / 5;
    setResizable (true, true);
    setResizeLimits (minWidth, baseHeight * 7 / 10, maxWidth, baseHeight * 8 / 5);
    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio ((double) baseWidth / (double) baseHeight);

    // The saved width is the scale the user chose. Open at it, but never bigger than the
    // screen: the default (scale 1) doesn't fit a 1440 x 900 laptop screen, so there it
    // opens a little smaller (the layout's aspect ratio has also changed since some
    // sizes were saved).
    int width = savedWidth >= minWidth && savedWidth <= maxWidth ? savedWidth : baseWidth;
    if (const auto* screen = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        const int maxHeight = (int) screen->userBounds.getHeight() - 60; // room for the host's window frame
        const int maxScreenWidth = (int) screen->userBounds.getWidth() - 40;
        width = juce::jmin (width, maxScreenWidth, maxHeight * baseWidth / baseHeight);
        width = juce::jmax (minWidth, width);
    }
    setSize (width, juce::roundToInt ((float) width * (float) baseHeight / (float) baseWidth));
    main->update();

    // Render the first frame synchronously: it's one render when the editor opens (as
    // before), and the display is never blank. Everything after this is in the background.
    refreshWaveform();
    renderer->start();
    startTimerHz (30);
}

DrumFoundryEditor::~DrumFoundryEditor()
{
    stopTimer();
    for (const auto& id : allParameterIds())
        drumProcessor.getState().removeParameterListener (id, this);

    // Whatever the user just did counts, even if the window closes before the next poll.
    removeMouseListener (activityWatcher.get());
    drumProcessor.getUndoHistory().flush();

    // Join the worker (at most one render) before anything else goes away. It only uses
    // the processor, which outlives us, and its own members.
    renderer = nullptr;
    setLookAndFeel (nullptr);
}

void DrumFoundryEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
}

void DrumFoundryEditor::resized()
{
    const float scale = (float) getWidth() / (float) baseWidth;
    main->setTransform (juce::AffineTransform::scale (scale));
    main->setBounds (0, 0, baseWidth, baseHeight);
    main->settings.setCurrentScale (scale);
    drumProcessor.uiState.width = getWidth();
}

void DrumFoundryEditor::parameterChanged (const juce::String&, float)
{
    // May arrive on the audio thread (automation): just flag it.
    waveformDirty = true;
}

void DrumFoundryEditor::timerCallback()
{
    pollChanges();
    showNewWaveform();
    main->update();

    main->display.getCanvas().setPlayhead (drumProcessor.getPlayhead());
    main->header.setLevels (drumProcessor.takePeak (0), drumProcessor.takePeak (1));

    // Flash on each sidechain hit, then fade out over a few frames.
    if (const int hits = drumProcessor.getSidechainHitCount(); hits != lastSidechainHits)
    {
        lastSidechainHits = hits;
        sidechainGlow = 1.0f;
    }
    else
    {
        sidechainGlow = sidechainGlow > 0.02f ? sidechainGlow * 0.8f : 0.0f;
    }
    main->scActivity.setActivity (sidechainGlow);
    {
        const auto* sc = drumProcessor.getState().getRawParameterValue (pid::scTrigger);
        main->header.setSidechainState (sc != nullptr && sc->load() >= 0.5f, sidechainGlow);
    }
    main->display.getCanvas().updateLanes();

    // The display shows the selected layer's curves. An extra layer whose tab went away
    // (removed, or a patch without it) hands over to the tab selected now.
    if (const int k = main->display.getLayer() - EnvelopeCanvas::firstExtraLayer; k >= 0 && ! main->layers.hasTab (k))
        main->display.focusLayer (main->layers.getSelectedTab() + 1);
    main->display.update();

    updateGateMarker();

    // A few times a second is plenty: every mouse-up polls as well.
    if (++undoPollTicks >= 3)
    {
        undoPollTicks = 0;
        pollUndo();
    }
}

void DrumFoundryEditor::pollUndo()
{
    // A menu (a ComboBox's, the preset list, "+") is modal: its choice lands after
    // the mouse-up, and belongs to the gesture that opened it.
    const bool menuOpen = juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0;
    drumProcessor.getUndoHistory().poll (activityWatcher->isBusy() || menuOpen);
    updateHistoryButtons();
}

void DrumFoundryEditor::updateHistoryButtons()
{
    const auto& history = drumProcessor.getUndoHistory();
    if (history.getRevision() == lastHistoryRevision)
        return;

    lastHistoryRevision = history.getRevision();
    const auto name = [] (bool can, const juce::String& what) { return can ? (what.isNotEmpty() ? what : juce::String ("edit")) : juce::String(); };
    main->header.setHistoryState (name (history.canUndo(), history.getUndoDescription()),
                                  name (history.canRedo(), history.getRedoDescription()));
}

void DrumFoundryEditor::undo()
{
    if (drumProcessor.getUndoHistory().undo())
        pollChanges(); // the envelopes, tabs and preset name, now rather than on the next tick
    updateHistoryButtons();
}

void DrumFoundryEditor::redo()
{
    if (drumProcessor.getUndoHistory().redo())
        pollChanges();
    updateHistoryButtons();
}

bool DrumFoundryEditor::keyPressed (const juce::KeyPress& key)
{
    using drum::ui::KeyAction;
    const auto action = keyMap->find (key);
    if (! action)
        return false;

    switch (*action)
    {
        case KeyAction::play:
        {
            // Holding the key repeats it: one hit per press. The time check covers a
            // release we didn't hear about (the focus moved while it was down).
            const auto now = juce::Time::getMillisecondCounterHiRes();
            const bool repeat = heldPlayKey == key.getKeyCode() && now - heldPlayKeyTime < 250.0;
            heldPlayKey = key.getKeyCode();
            heldPlayKeyTime = now;
            if (! repeat)
                main->header.trigger.press();
            break;
        }
        case KeyAction::undo:           undo(); break;
        case KeyAction::redo:           redo(); break;
        case KeyAction::previousPreset: main->header.showPreviousPreset(); break;
        case KeyAction::nextPreset:     main->header.showNextPreset(); break;
        case KeyAction::toggleBrowser:  setBrowserVisible (! isBrowserVisible()); break;
        case KeyAction::numActions:     return false;
    }
    return true;
}

bool DrumFoundryEditor::keyStateChanged (bool)
{
    if (heldPlayKey != 0 && ! juce::KeyPress::isKeyCurrentlyDown (heldPlayKey))
        heldPlayKey = 0;
    return false;
}

void DrumFoundryEditor::updateGateMarker()
{
    // Mark where the reverb gate cuts, while there's a gated reverb to cut. Read straight
    // from the parameters, so the marker follows the knobs even before a render lands.
    const auto value = [this] (const char* id)
    {
        const auto* v = drumProcessor.getState().getRawParameterValue (id);
        return v != nullptr ? v->load() : 0.0f;
    };
    const bool gated = value (pid::revOn) >= 0.5f && value (pid::revGate) >= 0.5f && value (pid::revMix) > 0.0f;
    main->display.getCanvas().setGateMarker (gated ? value (pid::revGateTime) : -1.0f, value (pid::revGateFade));
}

void DrumFoundryEditor::pollChanges()
{
    if (const int v = drumProcessor.getStateVersion(); v != lastStateVersion)
    {
        lastStateVersion = v;
        main->display.getCanvas().reloadFromProcessor();
        main->header.updatePresetDisplay();

        // An undo or redo keeps the tabs that went with the patch; anything else
        // (a preset, a session) starts afresh.
        if (v == drumProcessor.getUndoHistory().getRestoredStateVersion())
            main->layers.restoreTabs();
        else
            main->layers.resetSelection();
        waveformDirty = true;
    }

    updateDisplayNote();

    // parameterChanged() can run on the audio thread, so it only sets the flag and the
    // request (which can lock) is made from here.
    if (waveformDirty.exchange (false))
        renderer->request();
}

void DrumFoundryEditor::updateDisplayNote()
{
    // With KEY on the whole drum moves with the note, so draw the note the host last
    // played (the trigger pad plays it too). With KEY off every note sounds the same.
    const auto* key = drumProcessor.getState().getRawParameterValue (pid::keytrack);
    const bool keyOn = key != nullptr && key->load() >= 0.5f;
    const int played = drumProcessor.getDisplayNote();
    const int note = keyOn ? played : 60;
    if (note != renderer->getNote())
    {
        renderer->setNote (note);
        waveformDirty = true;
    }
    main->display.setKeyState (keyOn, played);

    // The pitch ruler reads Tune as it paints: repaint it as soon as it moves, rather
    // than when the new render lands.
    const auto params = drumProcessor.readParams();
    if (! juce::exactlyEqual (params.tuneHz, rulerTune) )
    {
        rulerTune = params.tuneHz;
        main->display.getCanvas().repaint();
    }
}

void DrumFoundryEditor::showNewWaveform()
{
    if (const auto hit = renderer->takeNew())
    {
        main->display.getCanvas().setWaveform (hit->audio, kRenderSampleRate, hit->lengthMs);
        main->analyzer.setHitAudio (hit->audio, kRenderSampleRate);
        main->eqOverlay.setHitAudio (hit->audio, kRenderSampleRate);
        main->miniEq.setHitAudio (hit->audio, kRenderSampleRate);
        for (int n = 0; n < 2; ++n)
            main->compScopes[(size_t) n].setCapture (hit->comps.comp[(size_t) n], kRenderSampleRate);
    }
}

void DrumFoundryEditor::refreshWaveform()
{
    updateDisplayNote();
    renderer->renderNow();
    showNewWaveform();
    updateGateMarker();
}

void DrumFoundryEditor::setBrowserVisible (bool shouldBeVisible)
{
    main->setBrowserVisible (shouldBeVisible);
}

bool DrumFoundryEditor::isBrowserVisible() const
{
    return main->drawer.isVisible();
}

void DrumFoundryEditor::setSettingsVisible (bool shouldBeVisible)
{
    main->setSettingsVisible (shouldBeVisible);
}

void DrumFoundryEditor::setEqVisible (bool shouldBeVisible)
{
    main->setEqVisible (shouldBeVisible);
}

bool DrumFoundryEditor::isEqVisible() const
{
    return main->eqOverlay.isVisible();
}

void DrumFoundryEditor::setReverbVisible (bool shouldBeVisible)
{
    main->setReverbVisible (shouldBeVisible);
}

bool DrumFoundryEditor::isReverbVisible() const
{
    return main->reverbOverlay.isVisible();
}

bool DrumFoundryEditor::isSettingsVisible() const
{
    return main->settings.isVisible();
}

void DrumFoundryEditor::browseSlot (drum::SampleSlot slot)
{
    main->browseSlot (slot);
}

void DrumFoundryEditor::selectExtraLayer (int index)
{
    main->layers.selectExtraLayer (index);
}

void DrumFoundryEditor::selectLayerTab (int tab)
{
    main->layers.selectTab (tab);
}

int DrumFoundryEditor::addExtraLayer (int source)
{
    return main->layers.addLayer (source);
}

void DrumFoundryEditor::touchLayer (int tab)
{
    if (tab < 0)
        main->layerTouched (-1, true);
    else
        main->layers.touchTab (tab);
}

juce::File DrumFoundryEditor::exportWav()
{
    // Turn any change the timer hasn't picked up yet into a request first, so a
    // render that's out of date can't pass as current.
    pollChanges();

    // Use the background render if it's current. Otherwise it's still rendering (or
    // hasn't started), and the drag needs the file now, so render here.
    auto rendered = renderer->getCurrent();
    if (rendered == nullptr)
    {
        rendered = renderer->renderNow();
        showNewWaveform();
    }
    const auto& hit = rendered->audio;

    // Trim the silent tail.
    int length = hit.getNumSamples();
    while (length > 1 && std::abs (hit.getSample (0, length - 1)) < 1.0e-5f
                      && std::abs (hit.getSample (1, length - 1)) < 1.0e-5f)
        --length;

    auto folder = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                      .getChildFile ("Drum Foundry").getChildFile ("Exports");
    if (! folder.createDirectory())
        return {};

    auto name = drumProcessor.getPresets().getCurrentName();
    if (name.isEmpty())
        name = "Drum";
    const auto file = folder.getChildFile (juce::File::createLegalFileName (
        "Drum Foundry - " + name + " " + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".wav"));
    file.deleteFile();

    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    if (! static_cast<juce::FileOutputStream*> (stream.get())->openedOk())
        return {};

    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                   .withSampleRate (kRenderSampleRate)
                                                   .withNumChannels (2)
                                                   .withBitsPerSample (24));
    if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (hit, 0, length))
        return {};

    return file;
}
