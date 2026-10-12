// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

// Dev tool for Drum Foundry. Not shipped.
//
//   drumfoundry-tools render   <outDir> [float]         factory presets -> WAV (24-bit or 32-bit float) + PNG + stats
//   drumfoundry-tools snapshot <out.png> [preset] [scale] [tab] [options]  editor screenshot
//                              (options: <paramId>=<value>, browser, browse=<slot>, samples, layer=<n>, hit,
//                               focus=<0..1>, pan=<oct>, settings, move=<env>,<i>,<y>, point=<env>,<x>,<y>, hoverlane=<n>)
//   drumfoundry-tools bench                              realtime CPU cost
//   drumfoundry-tools analyzer <outDir> [preset] [w] [h]    spectrum analyzer screenshots (LIVE, hover, HIT)
//   drumfoundry-tools check                              sanity checks (NaN, levels, state round-trip, sidechain trigger, reverb gate, reverb Size / noise Width, output filter, analyzer, samples, engine styles,
//                                                        layer offsets, solo, clap, pitch curve and envelope levels)
//   drumfoundry-tools measure / measure-presets          pitch, sweep, decay and spectrum analysis (see Measure.cpp)
//   drumfoundry-tools measure-clicks <dirs> [--ours]     click / transient features per folder (see ClickAnalysis.cpp)
//   drumfoundry-tools styles   <outDir>                  example hit per body style (and kicks) -> WAV + PNG, measured fundamentals
//   drumfoundry-tools check-samples                      only the imported-sample checks
//   drumfoundry-tools check-clipper                      only the output clipper checks
//   drumfoundry-tools check-undo                         only the undo / redo checks
//   drumfoundry-tools samples-ui [outDir]                sample browser + slot widget screenshots (default ./shots)

#include "Measure.h"

namespace drum::clicks { int run (const juce::StringArray& args); }
namespace drum::eqchecks { int run(); } // EqChecks.cpp
namespace drum::compchecks { int run(); } // CompChecks.cpp
namespace drum::orderchecks { int run(); } // OrderChecks.cpp
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "dsp/Clipper.h"
#include "dsp/DriveStage.h"
#include "dsp/NoiseClickGen.h"
#include "dsp/OfflineRenderer.h"
#include "dsp/OutputFilter.h"
#include "dsp/Reverb.h"
#include "dsp/TransientTrigger.h"
#include "presets/FactoryPresets.h"
#include "ui/SampleBrowser.h"
#include "ui/SampleSlotWidget.h"
#include "ui/FoundryLookAndFeel.h"
#include "ui/EnvelopeEditor.h"
#include "ui/LayerPanels.h"
#include "ui/HeaderBar.h"
#include "ui/SpectrumAnalyzer.h"
#include "ui/ReverbPanel.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <set>
#include <thread>

using namespace drum;

namespace
{

struct Stats
{
    float peakDb = -120, rmsDb = -120, decayMs = 0, centroidHz = 0;
    float bands[5] {}; // % energy: <150, 150-500, 500-2k, 2k-8k, >8k
};

Stats analyse (const juce::AudioBuffer<float>& b, double sr)
{
    Stats s;
    const int n = b.getNumSamples();
    std::vector<float> mono ((size_t) n);
    float peak = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float l = b.getSample (0, i), r = b.getSample (1, i);
        peak = juce::jmax (peak, std::abs (l), std::abs (r));
        mono[(size_t) i] = 0.5f * (l + r);
    }
    s.peakDb = juce::Decibels::gainToDecibels (peak, -120.0f);

    // RMS over the first 150 ms (the part you actually hear as loudness)
    const int rmsLen = juce::jmin (n, (int) (0.15 * sr));
    double early = 0.0;
    for (int i = 0; i < rmsLen; ++i)
        early += mono[(size_t) i] * mono[(size_t) i];
    s.rmsDb = juce::Decibels::gainToDecibels ((float) std::sqrt (early / juce::jmax (1, rmsLen)), -120.0f);

    // Decay: last sample above peak - 60 dB
    const float thresh = peak * 0.001f;
    int last = 0;
    for (int i = 0; i < n; ++i)
        if (std::abs (mono[(size_t) i]) > thresh)
            last = i;
    s.decayMs = (float) (last * 1000.0 / sr);

    // Spectrum of the whole hit
    constexpr int order = 14, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> data ((size_t) size * 2, 0.0f);
    for (int i = 0; i < juce::jmin (n, size); ++i)
        data[(size_t) i] = mono[(size_t) i];
    fft.performFrequencyOnlyForwardTransform (data.data());

    double num = 0, den = 0, bandE[5] {};
    for (int k = 1; k < size / 2; ++k)
    {
        const double f = k * sr / size;
        const double e = (double) data[(size_t) k] * data[(size_t) k];
        num += f * e;
        den += e;
        const int band = f < 150 ? 0 : f < 500 ? 1 : f < 2000 ? 2 : f < 8000 ? 3 : 4;
        bandE[band] += e;
    }
    s.centroidHz = den > 0 ? (float) (num / den) : 0.0f;
    for (int i = 0; i < 5; ++i)
        s.bands[i] = den > 0 ? (float) (100.0 * bandE[i] / den) : 0.0f;
    return s;
}

juce::Colour heat (float v)
{
    v = juce::jlimit (0.0f, 1.0f, v);
    if (v < 0.33f) return juce::Colour (0xff05060a).interpolatedWith (juce::Colour (0xff3b1c6e), v / 0.33f);
    if (v < 0.66f) return juce::Colour (0xff3b1c6e).interpolatedWith (juce::Colour (0xffe0457b), (v - 0.33f) / 0.33f);
    return juce::Colour (0xffe0457b).interpolatedWith (juce::Colour (0xfffff3b0), (v - 0.66f) / 0.34f);
}

void writePng (const juce::AudioBuffer<float>& b, double sr, const juce::String& title, const juce::File& file)
{
    constexpr int W = 1200, H = 560, waveH = 200;
    juce::Image img (juce::Image::RGB, W, H, true);
    juce::Graphics g (img);
    g.fillAll (juce::Colour (0xff0b0d10));

    const int n = b.getNumSamples();
    // Waveform (mono)
    g.setColour (juce::Colour (0xff3bd2ea));
    for (int x = 0; x < W; ++x)
    {
        const int i0 = (int) ((int64_t) x * n / W), i1 = juce::jmax (i0 + 1, (int) ((int64_t) (x + 1) * n / W));
        float mn = 0, mx = 0;
        for (int i = i0; i < i1 && i < n; ++i)
        {
            const float v = 0.5f * (b.getSample (0, i) + b.getSample (1, i));
            mn = juce::jmin (mn, v);
            mx = juce::jmax (mx, v);
        }
        g.drawVerticalLine (x, waveH * 0.5f - mx * waveH * 0.48f, waveH * 0.5f - mn * waveH * 0.48f + 1.0f);
    }

    // Spectrogram, log frequency 40 Hz - 20 kHz, 80 dB range
    constexpr int order = 10, size = 1 << order;
    juce::dsp::FFT fft (order);
    juce::dsp::WindowingFunction<float> window (size, juce::dsp::WindowingFunction<float>::hann);
    std::vector<float> frame ((size_t) size * 2);
    const int specTop = waveH + 10, specH = H - specTop - 20;
    for (int x = 0; x < W; ++x)
    {
        const int centre = (int) ((int64_t) x * n / W);
        std::fill (frame.begin(), frame.end(), 0.0f);
        for (int i = 0; i < size; ++i)
        {
            const int idx = centre - size / 2 + i;
            frame[(size_t) i] = (idx >= 0 && idx < n) ? 0.5f * (b.getSample (0, idx) + b.getSample (1, idx)) : 0.0f;
        }
        window.multiplyWithWindowingTable (frame.data(), (size_t) size);
        fft.performFrequencyOnlyForwardTransform (frame.data());

        for (int y = 0; y < specH; ++y)
        {
            const float frac = 1.0f - (float) y / (float) specH;
            const float f = 40.0f * std::pow (500.0f, frac);
            const float bin = f * size / (float) sr;
            const int k = juce::jlimit (1, size / 2 - 1, (int) bin);
            const float mag = frame[(size_t) k] / (size * 0.25f);
            const float db = juce::Decibels::gainToDecibels (mag, -100.0f);
            img.setPixelAt (x, specTop + y, heat ((db + 80.0f) / 80.0f));
        }
    }

    g.setColour (juce::Colours::white.withAlpha (0.8f));
    g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    g.drawText (title, 10, 6, 800, 20, juce::Justification::left);
    g.setFont (juce::FontOptions (11.0f));
    g.setColour (juce::Colours::white.withAlpha (0.6f));
    for (float f : { 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float frac = std::log (f / 40.0f) / std::log (500.0f);
        const int y = specTop + (int) ((1.0f - frac) * specH);
        g.drawText (f >= 1000 ? juce::String (f / 1000.0f, 0) + "k" : juce::String ((int) f), 4, y - 7, 40, 14,
                    juce::Justification::left);
    }
    const double totalMs = n * 1000.0 / sr;
    g.drawText (juce::String (totalMs, 0) + " ms", W - 80, H - 18, 76, 14, juce::Justification::right);

    file.deleteFile();
    juce::FileOutputStream out (file);
    juce::PNGImageFormat().writeImageToStream (img, out);
}

bool writeWav (const juce::AudioBuffer<float>& b, double sr, const juce::File& file, int bits = 24)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                                     .withSampleRate (sr)
                                                                     .withNumChannels (2)
                                                                     .withBitsPerSample (bits));
    return writer != nullptr && writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

juce::AudioBuffer<float> renderCurrent (DrumFoundryProcessor& proc, double sr, int note = 60, float vel = 1.0f)
{
    EnvTables tables;
    proc.bakeTables (tables);
    return renderHit (proc.readParams(), tables, sr, note, vel);
}

bool allFinite (const juce::AudioBuffer<float>& b)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int s = 0; s < b.getNumSamples(); ++s)
            if (! std::isfinite (b.getSample (c, s)))
                return false;
    return true;
}

bool allFinite (const EnvTables& t)
{
    for (const auto& table : t.data)
        for (float v : table)
            if (! std::isfinite (v))
                return false;
    return true;
}

bool allFinite (const CurveEnvelope& e)
{
    for (const auto& p : e.getPoints())
        if (! std::isfinite (p.x) || ! std::isfinite (p.y) || ! std::isfinite (p.curve))
            return false;
    return true;
}

bool allFinite (const DrumParams& p)
{
    for (float v : { p.lengthMs, p.tuneHz, p.velSens, p.bodyGain, p.bodyShape, p.otGain, p.otRatio,
                     p.modes, p.noiseGain, p.noiseHP, p.noiseLP, p.noiseColor, p.noiseCouple, p.noiseWidth,
                     p.noiseGrit, p.clickGain, p.clickTone, p.clickLengthMs, p.driveAmount, p.driveMix,
                     p.eqBands[0].gainDb, p.eqBands[1].freq, p.eqBands[1].gainDb, p.eqBands[2].gainDb, p.outGain,
                     p.revMix, p.revSize, p.revDecayMs, p.revPreDelayMs, p.revLowCut, p.revHighCut,
                     p.revGateMs, p.revFadeMs })
        if (! std::isfinite (v))
            return false;
    return true;
}

/** Current patch with non-finite / out-of-range values planted in both the
    PARAM values and the envelope points. `bad` maps a marker string ("nan",
    "inf", "-inf") to the var that gets stored, so the same tree can be built
    from strings (as a corrupt XML file would give) or from real float values. */
juce::ValueTree makeCorruptState (DrumFoundryProcessor& proc, const std::function<juce::var (const char*)>& bad)
{
    auto tree = proc.createStateTree();

    const auto setParam = [&tree] (const char* id, const juce::var& value)
    {
        for (auto child : tree)
            if (child.hasType ("PARAM") && child.getProperty ("id").toString() == id)
                child.setProperty ("value", value, nullptr);
    };
    setParam (pid::tune,       bad ("nan"));
    setParam (pid::length,     bad ("inf"));
    setParam (pid::outGain,    bad ("inf"));
    setParam (pid::bodyLevel,  bad ("-inf"));
    setParam (pid::driveType,  bad ("nan"));
    setParam (pid::clipDrive,  1.0e9);   // out of range
    setParam (pid::clickType,  99);      // out of range choice index
    setParam (pid::noiseLP,    -5000.0); // out of range

    const auto point = [] (const juce::var& x, const juce::var& y, const juce::var& c)
    {
        juce::ValueTree p ("P");
        p.setProperty ("x", x, nullptr);
        p.setProperty ("y", y, nullptr);
        p.setProperty ("c", c, nullptr);
        return p;
    };

    auto envs = tree.getChildWithName (DrumFoundryProcessor::envelopesId);

    // Pitch: a few bad points mixed in with the good ones.
    auto pitch = envs.getChildWithName (DrumFoundryProcessor::envelopeTreeId (envPitch));
    pitch.appendChild (point (bad ("nan"), 0.5, 0.0), nullptr);
    pitch.appendChild (point (0.4, bad ("inf"), 0.0), nullptr);
    pitch.appendChild (point (bad ("-inf"), bad ("nan"), bad ("inf")), nullptr);
    pitch.appendChild (point (0.5, 0.3, bad ("nan")), nullptr);
    pitch.getChild (0).setProperty ("c", bad ("-inf"), nullptr);

    // Body: nothing usable at all.
    auto body = envs.getChildWithName (DrumFoundryProcessor::envelopeTreeId (envBody));
    body.removeAllChildren (nullptr);
    body.appendChild (point (bad ("nan"), bad ("nan"), bad ("nan")), nullptr);
    body.appendChild (point (bad ("inf"), bad ("-inf"), 0.0), nullptr);
    body.appendChild (point (0.5, bad ("inf"), 0.0), nullptr);

    // Noise: only one good point left once the bad ones are dropped.
    auto noise = envs.getChildWithName (DrumFoundryProcessor::envelopeTreeId (envNoise));
    noise.removeAllChildren (nullptr);
    noise.appendChild (point (0.0, 1.0, bad ("nan")), nullptr);
    noise.appendChild (point (bad ("inf"), 0.0, 0.0), nullptr);

    return tree;
}

int cmdRender (const juce::File& outDir, int bits)
{
    outDir.createDirectory();
    DrumFoundryProcessor proc;
    auto& presets = proc.getPresets();
    const auto& entries = presets.getEntries();
    constexpr double sr = 48000.0;

    std::printf ("%-16s %7s %8s %7s %8s %9s %6s %6s %6s %6s %6s\n", "preset", "peak", "preclip", "rms150", "decay", "centroid",
                 "<150", "150-500", "500-2k", "2k-8k", ">8k");
    for (int i = 0; i < (int) entries.size(); ++i)
    {
        if (! entries[(size_t) i].isFactory)
            continue;
        presets.load (i);
        const auto hit = renderCurrent (proc, sr);
        const auto s = analyse (hit, sr);

        auto unclipped = proc.readParams();
        unclipped.clipMode = 0;
        EnvTables tables;
        proc.bakeTables (tables);
        const auto raw = analyse (renderHit (unclipped, tables, sr), sr);

        const auto name = entries[(size_t) i].name;
        const auto base = juce::String (i).paddedLeft ('0', 2) + "_" + juce::File::createLegalFileName (name).replaceCharacter (' ', '_');

        writeWav (hit, sr, outDir.getChildFile (base + ".wav"), bits);
        writePng (hit, sr, name, outDir.getChildFile (base + ".png"));

        std::printf ("%-16s %6.1fdB %+6.1fdB %6.1fdB %6.0fms %7.0fHz %5.1f%% %5.1f%% %5.1f%% %5.1f%% %5.1f%%\n",
                     name.toRawUTF8(), s.peakDb, raw.peakDb, s.rmsDb, s.decayMs, s.centroidHz,
                     s.bands[0], s.bands[1], s.bands[2], s.bands[3], s.bands[4]);
    }
    return 0;
}

void setParam (DrumFoundryProcessor& p, const char* id, float plainValue);

bool writeTestAudio (juce::AudioFormat& format, const juce::File& file, double sr, int channels, double seconds, float hz);

/** The first child of `root` (at any depth) of type T, or nullptr. */
template <typename T>
T* findChild (juce::Component& root)
{
    for (auto* c : root.getChildren())
    {
        if (auto* t = dynamic_cast<T*> (c))
            return t;
        if (auto* t = findChild<T> (*c))
            return t;
    }
    return nullptr;
}

/** Shows a core curve (an EnvId) on the display: its layer, on its tab. */
void showCurve (ui::DisplayPanel& display, int env)
{
    display.focusLayer (env == envNoise || env == envNoisePitch ? 1 : env == envClick || env == envClickPitch ? 2 : 0);
    display.showTab (isPitchEnvelope (env) ? ui::EnvelopeCanvas::pitchTab : ui::EnvelopeCanvas::levelTab);
}

/** A mouse event on `c` at `pos` (in c's coordinates), for driving a component's
    mouse handlers directly. */
juce::MouseEvent mouseEventAt (juce::Component& c, juce::Point<float> pos, juce::ModifierKeys mods = {},
                               int clicks = 1, juce::Point<float> downPos = { -1.0f, -1.0f }, bool dragged = false)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, mods,
                             juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                             juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                             juce::MouseInputSource::defaultTiltY, &c, &c, now,
                             downPos.x < 0.0f ? pos : downPos, now, clicks, dragged);
}

/** Editor screenshot at the base size, times `scale`. After the positional arguments,
    each option is one of:
      <paramId>=<value>   set a parameter (plain value; a choice also takes its name, e.g. bodyStyle=Sample)
      browser             open the sample browser
      browse=<slot>       open it on a slot: body, noise, click, layer1..layer3
      samples             put test samples in the Body, Noise, Click and Layer 1 slots
      layer=<n>           show extra layer n (1-based)
      layers=<tab>        show a Layers tab: noise, click, or an extra layer's number (1-based)
      add=<source>        add an extra layer, as the "+" menu does: noise, click, sample or sine
      mute=<tab>          click a Layers tab's mute button: noise, click, or an extra layer's number
      note=<midi>         play this MIDI note first (the display follows it with KEY on)
      hit                 show the analyzer's HIT spectrum (default: LIVE, after one hit)
      focus=<0..1>        the pitch axis' FOCUS dial (0 = 20 Hz - 16 kHz, 1 = the band around the curve)
      pan=<octaves>       pan of the focused pitch axis
      settings            open the settings (the header's gear)
      eq                  open the full-screen EQ
      move=<env>,<i>,<y>  move point i of an envelope to y (semitones / linear gain). Envelopes: pitch (Body
                          pitch), body (Body level), noise, click (levels), noisepitch, clickpitch
      point=<env>,<x>,<y> add a point to an envelope
      active=<env>        the layer and tab the display shows, by one of its curves (instead of the tab argument, an EnvId)
      chart=<layer>,<tab> the layer the display shows (0 Body, 1 Noise, 2 Click, 3.. extra layers) and its tab (0 Pitch, 1 Level)
      touch=<layer>       press a layer as the user does: body, noise, click, or an extra layer's number
      curvesource=<n>     where the shown extra layer's curve on this tab comes from (a LayerEnv: 0 Body .. 3 Flat, 4 Own), as its menu
      hoverlane=<n>       hover the display's timing lane n (0 = Body), to show its readout
      hoverpoint=<i>      hover point i of the curve on show, to show its readout
      hovergate           hover the reverb gate's line (gatedrag=<ms> drags it by that much, and holds the button)
      undo                make one undoable edit (Tune), so the header's undo button is lit
      chartall            the display's ALL on: every layer's curves on the tab together
      chain=<keys>        the effects chain, e.g. chain=drive1,comp1,clip1,eq,reverb (FxChain keys)
      module=<key>        the module whose panel shows under the chain strip: a key (module=comp1) or an FxModule number (module=2) */
int cmdSnapshot (const juce::File& out, int presetIndex, float scale, int tab, const juce::StringArray& options)
{
    DrumFoundryProcessor proc;
    // The tab argument is a core curve (an EnvId): show its layer, on its tab.
    const auto showCurve = [&proc] (int env)
    {
        env = juce::jlimit (0, numCoreEnvs - 1, env);
        proc.uiState.chartLayer = env == envNoise || env == envNoisePitch ? 1 : env == envClick || env == envClickPitch ? 2 : 0;
        proc.uiState.chartTab = isPitchEnvelope (env) ? 0 : 1;
    };
    showCurve (tab);
    proc.prepareToPlay (48000.0, 512);
    if (presetIndex >= 0)
        proc.getPresets().load (presetIndex);

    const auto sampleDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("DrumFoundrySnapshot");
    if (options.contains ("samples"))
    {
        sampleDir.createDirectory();
        juce::WavAudioFormat wav;
        std::array<juce::String, numSampleSlots> paths;
        const std::tuple<SampleSlot, const char*, float, double> files[] { { slotBody, "Deep Kick 01.wav", 55.0f, 0.6 },
                                                                          { slotNoise, "Snare Wires Top.wav", 2400.0f, 0.4 },
                                                                          { slotClick, "Rim Knock.wav", 900.0f, 0.12 },
                                                                          { slotLayer1, "Clap Layer 04.wav", 1300.0f, 0.3 } };
        for (const auto& [slot, name, hz, seconds] : files)
        {
            const auto f = sampleDir.getChildFile (name);
            writeTestAudio (wav, f, 44100.0, 2, seconds, hz);
            paths[(size_t) slot] = f.getFullPathName();
        }
        proc.getSampleBank().restore (paths);
    }

    const auto envFor = [] (const juce::String& name)
    {
        return name == "pitch" ? envPitch : name == "body" ? envBody : name == "noisepitch" ? envNoisePitch
             : name == "clickpitch" ? envClickPitch : name == "click" ? envClick : envNoise;
    };
    for (const auto& option : options)
    {
        const auto value = option.fromFirstOccurrenceOf ("=", false, false);
        const auto fields = juce::StringArray::fromTokens (value, ",", "");
        if (option.startsWith ("active="))
            showCurve (envFor (value));
        else if (option.startsWith ("chain="))
            proc.setFxChain (FxChain::fromString (value));
        else if (option.startsWith ("module="))
        {
            // A key (comp1) or an FxModule number (2 = the EQ).
            proc.uiState.fxModule = value.containsOnly ("0123456789") ? value.getIntValue() : -1;
            for (int m = 0; m < numFxModules; ++m)
                if (value == fxModuleKey (m))
                    proc.uiState.fxModule = m;
        }
        else if (option.startsWith ("chart=") && fields.size() >= 2)
        {
            proc.uiState.chartLayer = juce::jlimit (0, DrumFoundryProcessor::UiState::numChartLayers - 1, fields[0].getIntValue());
            proc.uiState.chartTab = juce::jlimit (0, 1, fields[1].getIntValue());
        }
        else if (option == "lfotab")
            proc.uiState.lfoTab = true;
        else if (option == "chartall")
            proc.uiState.chartShowAll = true;
        else if (option.startsWith ("focus="))
            proc.uiState.pitchFocus = value.getFloatValue();
        else if (option.startsWith ("pan="))
            proc.uiState.pitchPan = value.getFloatValue();
        else if ((option.startsWith ("move=") || option.startsWith ("point=")) && fields.size() == 3)
        {
            const auto id = envFor (fields[0]);
            auto env = proc.getEnvelope (id);
            if (option.startsWith ("move="))
            {
                const int i = fields[1].getIntValue();
                if (juce::isPositiveAndBelow (i, env.size()))
                    env.movePoint (i, env.getPoints()[(size_t) i].x, fields[2].getFloatValue());
            }
            else
            {
                env.addPoint (fields[1].getFloatValue(), fields[2].getFloatValue());
            }
            proc.setEnvelope (id, env);
        }
    }

    for (const auto& option : options)
    {
        if (! option.containsChar ('=') || option.startsWith ("browse=") || option.startsWith ("layer=")
            || option.startsWith ("layers=") || option.startsWith ("add=") || option.startsWith ("note=")
            || option.startsWith ("mute=") || option.startsWith ("focus=") || option.startsWith ("pan=")
            || option.startsWith ("move=") || option.startsWith ("point=") || option.startsWith ("hoverlane=")
            || option.startsWith ("active=") || option.startsWith ("chart=") || option.startsWith ("module=")
            || option.startsWith ("touch=") || option.startsWith ("hoverpoint=") || option.startsWith ("curvesource=")
            || option.startsWith ("gatedrag=") || option.startsWith ("chain=") || option.startsWith ("module="))
            continue;
        const auto id = option.upToFirstOccurrenceOf ("=", false, false);
        const auto value = option.fromFirstOccurrenceOf ("=", false, false);
        auto* param = proc.getState().getParameter (id);
        if (param == nullptr)
        {
            std::printf ("unknown parameter: %s\n", id.toRawUTF8());
            return 1;
        }
        float plain = value.getFloatValue();
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (param); choice != nullptr && choice->choices.contains (value))
            plain = (float) choice->choices.indexOf (value);
        setParam (proc, id.toRawUTF8(), plain);
    }

    // One undoable edit, so the header's undo button is lit.
    if (options.contains ("undo"))
    {
        auto& history = proc.getUndoHistory();
        history.clear();
        history.noteUserActivity();
        setParam (proc, pid::tune, proc.readParams().tuneHz + 5.0f);
        history.poll (false);
    }

    // Play a hit and advance a little so the playhead + meters show something.
    // (a MIDI note, if one is given: with KEY on the display follows it)
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    int note = -1;
    for (const auto& option : options)
        if (option.startsWith ("note="))
            note = juce::jlimit (0, 127, option.fromFirstOccurrenceOf ("=", false, false).getIntValue());
    if (note >= 0)
        midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.9f), 0);
    else
        proc.triggerAudition (0.9f);
    for (int i = 0; i < 6; ++i)
    {
        proc.processBlock (buf, midi);
        midi.clear();
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setVisible (true);
    editor->setSize (DrumFoundryEditor::baseWidth, DrumFoundryEditor::baseHeight);
    if (auto* ed = dynamic_cast<DrumFoundryEditor*> (editor.get()))
    {
        for (const auto& option : options)
        {
            if (! option.startsWith ("add="))
                continue;
            const auto source = layerSourceNames.indexOf (option.fromFirstOccurrenceOf ("=", false, false), true);
            if (source < 0 || ed->addExtraLayer (source) < 0)
            {
                std::printf ("can't add layer: %s\n", option.toRawUTF8());
                return 1;
            }
            ed->setBrowserVisible (false); // a Sample layer opens it
        }
        const auto tabFor = [] (const juce::String& which)
        {
            return which == "noise" ? ui::LayersSection::noiseTab
                 : which == "click" ? ui::LayersSection::clickTab
                                    : ui::LayersSection::firstExtraTab + which.getIntValue() - 1;
        };
        if (auto* layers = findChild<ui::LayersSection> (*editor))
            for (const auto& option : options)
                if (option.startsWith ("mute="))
                    if (auto& mute = layers->getMuteButton (tabFor (option.fromFirstOccurrenceOf ("=", false, false))); mute.onClick)
                        mute.onClick();
        ed->refreshWaveform();
        for (const auto& option : options)
        {
            if (option.startsWith ("layers="))
                ed->selectLayerTab (tabFor (option.fromFirstOccurrenceOf ("=", false, false)));
            if (option == "browser")
                ed->setBrowserVisible (true);
            if (option.startsWith ("browse="))
            {
                const auto name = option.fromFirstOccurrenceOf ("=", false, false);
                for (int slot = 0; slot < numSampleSlots; ++slot)
                    if (sampleSlotKey ((SampleSlot) slot).equalsIgnoreCase (name))
                        ed->browseSlot ((SampleSlot) slot);
            }
            if (option.startsWith ("layer="))
                ed->selectExtraLayer (option.fromFirstOccurrenceOf ("=", false, false).getIntValue() - 1);
            if (option.startsWith ("touch="))
            {
                const auto which = option.fromFirstOccurrenceOf ("=", false, false);
                ed->touchLayer (which == "body" ? -1 : tabFor (which));
            }
            if (option.startsWith ("curvesource="))
                if (auto* d = findChild<ui::DisplayPanel> (*editor))
                    d->getSourceBox().setSelectedId (option.fromFirstOccurrenceOf ("=", false, false).getIntValue() + 1, juce::sendNotificationSync);
            if (option == "settings")
                ed->setSettingsVisible (true);
            if (option == "eq")
                ed->setEqVisible (true);
            if (option == "reverbfull")
            {
                ed->setReverbVisible (true);
                // Its response renders on a thread and arrives on the message thread.
                if (auto* overlay = findChild<ui::ReverbOverlay> (*editor))
                    overlay->getResponse().renderNow();
            }
        }

        if (auto* canvas = findChild<ui::EnvelopeCanvas> (*editor))
        {
            canvas->updateLanes(); // as the editor's timer does
            for (const auto& option : options)
                if (option.startsWith ("hoverlane="))
                {
                    const auto lane = canvas->getLaneBounds (option.fromFirstOccurrenceOf ("=", false, false).getIntValue());
                    canvas->mouseMove (mouseEventAt (*canvas, { (float) canvas->getWidth() * 0.5f, lane.getCentreY() }));
                }
                else if (option.startsWith ("hoverpoint=") && canvas->getActiveEnvelope() >= 0)
                {
                    const int env = canvas->getActiveEnvelope();
                    const int index = juce::jlimit (0, canvas->getEnvelope ((EnvId) env).size() - 1,
                                                    option.fromFirstOccurrenceOf ("=", false, false).getIntValue());
                    canvas->mouseMove (mouseEventAt (*canvas, canvas->getPointPosition (env, index)));
                }
                else if (option == "hovergate" || option.startsWith ("gatedrag="))
                {
                    const auto plot = canvas->getPlotArea();
                    const juce::Point<float> from (canvas->getGateMarkerX(), plot.getCentreY() + 20.0f);
                    if (from.x < 0.0f)
                    {
                        std::printf ("no gate marker (switch the gate on and raise revMix)\n");
                        return 1;
                    }
                    canvas->mouseMove (mouseEventAt (*canvas, from));
                    if (option.startsWith ("gatedrag="))
                    {
                        // Press and drag, and leave the button down, so the drag state shows.
                        const float ms = option.fromFirstOccurrenceOf ("=", false, false).getFloatValue();
                        const auto to = from.translated (canvas->msToX (ms) - canvas->msToX (0.0f), 0.0f);
                        canvas->mouseDown (mouseEventAt (*canvas, from));
                        canvas->mouseDrag (mouseEventAt (*canvas, to, {}, 1, from, true));
                    }
                }
        }
    }

    if (auto* analyzer = findChild<ui::SpectrumAnalyzer> (*editor))
    {
        if (options.contains ("hit"))
            analyzer->setMode (ui::SpectrumAnalyzer::Mode::hit);
        analyzer->pollNow();
    }
    if (auto* browser = findChild<ui::SampleBrowser> (*editor); browser != nullptr && browser->getParentComponent()->isVisible())
        browser->waitForScan (20000);

    const auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, scale);
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat().writeImageToStream (img, stream);
    std::printf ("wrote %s (%dx%d)\n", out.getFullPathName().toRawUTF8(), img.getWidth(), img.getHeight());

    editor = nullptr;
    proc.stopPreview();
    sampleDir.deleteRecursively();
    return 0;
}

void setParam (DrumFoundryProcessor& p, const char* id, float plainValue);

void setParam (DrumFoundryProcessor& p, const char* id, float plainValue);
void benchEngineStyles();

int cmdBench()
{
    constexpr double sr = 48000.0;
    constexpr int block = 128;

    struct Setup { const char* name; float mix; bool gate; float decayMs; bool lfos = false; };
    const Setup setups[] { { "reverb off (Mix 0)", 0.0f, true, 2500.0f },
                           { "reverb on, gated, 2.5 s decay", 0.5f, true, 2500.0f },
                           { "reverb on, gate off, 6 s decay", 0.5f, false, 6000.0f },
                           { "reverb on, gated, 3 LFOs routed", 0.5f, true, 2500.0f, true } };

    for (const auto& setup : setups)
    {
        DrumFoundryProcessor proc;
        proc.prepareToPlay (sr, block);
        proc.getPresets().load (5); // something with drive + modes
        setParam (proc, pid::revMix, setup.mix);
        setParam (proc, pid::revGate, setup.gate ? 1.0f : 0.0f);
        setParam (proc, pid::revDecay, setup.decayMs);
        if (setup.lfos)
        {
            // Modulation renders in short chunks: Tune, Noise Low Cut and Drive, all moving.
            const char* targets[] { pid::tune, pid::noiseHP, pid::driveAmount };
            for (int l = 0; l < kNumLfos; ++l)
            {
                setParam (proc, lfoParamId (l, pid::lfoDest, 0).toRawUTF8(), (float) (modTargetIndex (targets[l]) + 1));
                setParam (proc, lfoParamId (l, pid::lfoAmt, 0).toRawUTF8(), 0.2f);
            }
        }

        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer midi;
        const int totalBlocks = (int) (sr * 30.0 / block);
        const int hitEvery = (int) (sr * 0.06); // 1/32 notes at ~125 bpm: plenty of overlapping voices
        int64_t samplePos = 0;

        const auto start = juce::Time::getHighResolutionTicks();
        for (int b = 0; b < totalBlocks; ++b)
        {
            midi.clear();
            const int64_t next = ((samplePos / hitEvery) + 1) * hitEvery;
            if (next < samplePos + block)
                midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), (int) (next - samplePos));
            proc.processBlock (buf, midi);
            samplePos += block;
        }
        const double secs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
        std::printf ("%-32s 30 s of dense hits rendered in %.3f s  ->  %.2f%% of one core (%.0fx realtime)\n",
                     setup.name, secs, 100.0 * secs / 30.0, 30.0 / secs);
    }

    // The reverb on its own, ringing (gate off, 6 s decay).
    {
        DrumParams p;
        p.revMix = 0.5f;
        p.revGate = false;
        p.revDecayMs = 6000.0f;
        Reverb r;
        r.prepare (sr);
        juce::AudioBuffer<float> buf (2, 512);
        juce::Random rng (1);
        const int blocks = (int) (sr * 30.0 / 512);
        double secs = 0.0;
        for (int b = 0; b < blocks; ++b)
        {
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 512; ++i)
                    buf.setSample (c, i, b % 200 == 0 ? rng.nextFloat() - 0.5f : 0.0f);
            const auto start = juce::Time::getHighResolutionTicks();
            r.process (buf.getWritePointer (0), buf.getWritePointer (1), 512, p, nullptr, 0, 0);
            secs += juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
        }
        std::printf ("%-32s 30 s in %.3f s  ->  %.2f%% of one core (%.1f ns per sample)\n", "Reverb alone, ringing",
                     secs, 100.0 * secs / 30.0, 1.0e9 * secs / (sr * 30.0));
    }

    // The output clipper on its own, per mode, on a hot signal (always clipping).
    {
        const int n = (int) sr * 10;
        juce::AudioBuffer<float> hot (2, n);
        juce::Random rng (5);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                hot.setSample (c, i, 2.4f * (float) std::sin (0.0072 * i) + 0.6f * (rng.nextFloat() - 0.5f));
        juce::String line;
        for (int m = 0; m < Clipper::numModes; ++m)
        {
            Clipper clipper;
            clipper.prepare (sr);
            auto buf = hot;
            const auto start = juce::Time::getHighResolutionTicks();
            for (int pos = 0; pos < n; pos += block)
                clipper.process (buf.getWritePointer (0, pos), buf.getWritePointer (1, pos), juce::jmin (block, n - pos), m, 6.0f, 0.0f);
            const double secs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
            line << clipModeNames[m] << " " << juce::String (100.0 * secs / 10.0, 2) << "%  ";
        }
        std::printf ("%-32s %s(of one core, %dx, hot signal)\n", "Clipper alone, per mode", line.toRawUTF8(),
                     Clipper::factorForSampleRate (sr));
    }

    // The editor's waveform render (background thread, up to 30 Hz).
    {
        DrumFoundryProcessor proc;
        EnvTables tables;
        proc.bakeTables (tables);
        auto p = proc.readParams();
        p.revMix = 0.5f;
        for (const auto& [name, gate, decay] : { std::tuple<const char*, bool, float> { "display render, Mix 0", false, 0.0f },
                                                 { "display render, gated 2.5 s", true, 2500.0f },
                                                 { "display render, ungated 6 s", false, 6000.0f } })
        {
            auto q = p;
            q.revMix = decay > 0.0f ? 0.5f : 0.0f;
            q.revGate = gate;
            q.revDecayMs = decay > 0.0f ? decay : 2500.0f;
            constexpr int runs = 20;
            int length = 0;
            const auto start = juce::Time::getHighResolutionTicks();
            for (int i = 0; i < runs; ++i)
                length = renderHit (q, tables, sr).getNumSamples();
            const double ms = 1000.0 * juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start) / runs;
            std::printf ("%-32s %.2f ms per render (%.0f ms of audio @48k)\n", name, ms, 1000.0 * length / sr);
        }
    }

    benchEngineStyles();
    return 0;
}

//==============================================================================
// Sidechain trigger checks

/** Adds a burst to both channels of `b` at `start`: a sample of `peakDb` right
    at the onset (so the crossing is exactly there), then noise decaying with
    time constant tauMs. */
void addBurst (juce::AudioBuffer<float>& b, double sr, int start, float peakDb, float tauMs, juce::Random& rng)
{
    const float a = juce::Decibels::decibelsToGain (peakDb);
    const int len = juce::jmin (b.getNumSamples() - start, (int) (sr * tauMs * 0.001 * 12.0));
    for (int i = 0; i < len; ++i)
    {
        const float env = a * std::exp (-(float) i / (float) (sr * tauMs * 0.001));
        const float v = i == 0 ? a : env * (rng.nextFloat() * 2.0f - 1.0f);
        for (int c = 0; c < b.getNumChannels(); ++c)
            b.getWritePointer (c)[start + i] += v;
    }
}

struct DetectedHit
{
    int64_t position;
    float velocity;
};

/** Runs the detector over `signal` in blocks (sizes cycle through `blockSizes`)
    and returns every hit as an absolute sample position. */
std::vector<DetectedHit> runDetector (TransientTrigger& det, const juce::AudioBuffer<float>& signal,
                                      std::vector<int> blockSizes, const SidechainParams& params)
{
    std::vector<DetectedHit> result;
    const int total = signal.getNumSamples();
    size_t which = 0;
    for (int pos = 0; pos < total;)
    {
        const int len = juce::jmin (blockSizes[which++ % blockSizes.size()], total - pos);
        juce::AudioBuffer<float> block (const_cast<float* const*> (signal.getArrayOfReadPointers()),
                                        signal.getNumChannels(), pos, len);
        det.process (block, len, params);
        for (int h = 0; h < det.getNumHits(); ++h)
        {
            const auto& hit = det.getHits()[h];
            if (! juce::isPositiveAndBelow (hit.sampleOffset, len) || ! std::isfinite (hit.velocity))
                result.push_back ({ -1, hit.velocity }); // flagged as bad by the callers
            else
                result.push_back ({ pos + hit.sampleOffset, hit.velocity });
        }
        pos += len;
    }
    return result;
}

void setParam (DrumFoundryProcessor& p, const char* id, float plainValue)
{
    auto* param = p.getState().getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

float rawParam (DrumFoundryProcessor& p, const char* id)
{
    return p.getState().getRawParameterValue (id)->load();
}

int checkSidechain()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: %s\n", msg.toRawUTF8()); ++failures; };

    constexpr double sr = 48000.0;
    juce::Random rng (1234);

    SidechainParams on;
    on.enabled = true;
    on.thresholdDb = -24.0f;
    on.retriggerMs = 50.0f;

    TransientTrigger det;
    det.prepare (sr, 512);
    const int delay = det.getReportDelaySamples();

    const auto describe = [] (const std::vector<DetectedHit>& hits)
    {
        juce::String s;
        for (const auto& h : hits)
            s << (s.isEmpty() ? "" : ", ") << juce::String (h.position) << " (v " << juce::String (h.velocity, 2) << ")";
        return "[" + s + "]";
    };

    // 1. Bursts at known positions, one straddling a 512-sample block boundary
    //    (crossing 20 samples before 20480, so the velocity window ends in the
    //    next block). Same result with a ragged block-size sequence.
    {
        juce::AudioBuffer<float> sig (2, (int) sr);
        sig.clear();
        const int onsets[] { 1000, 10000, 20460, 30000, 40000 };
        const float levels[] { -6.0f, -18.0f, 0.0f, -12.0f, 3.0f };
        for (int i = 0; i < 5; ++i)
            addBurst (sig, sr, onsets[i], levels[i], 15.0f, rng);

        for (const auto& sizes : { std::vector<int> { 512 }, std::vector<int> { 1, 37, 700, 64, 511, 2048, 13 } })
        {
            det.reset();
            const auto hits = runDetector (det, sig, sizes, on);
            const auto tag = "detector (blocks " + juce::String (sizes.size() == 1 ? "512" : "ragged") + ")";
            if (hits.size() != 5)
            {
                fail (tag + ": expected 5 hits, got " + juce::String ((int) hits.size()) + " " + describe (hits));
                continue;
            }

            int maxError = 0;
            for (int i = 0; i < 5; ++i)
            {
                const int error = (int) (hits[(size_t) i].position - (onsets[i] + delay));
                maxError = juce::jmax (maxError, std::abs (error));
                if (std::abs (error) > delay)
                    fail (tag + ": hit " + juce::String (i) + " at " + juce::String (hits[(size_t) i].position)
                          + ", expected " + juce::String (onsets[i] + delay));

                // Peak -> velocity: threshold..0 dBFS maps to 0.3..1, linear in dB.
                const float t = juce::jlimit (0.0f, 1.0f, (levels[i] + 24.0f) / 24.0f);
                const float expected = 0.3f + 0.7f * t;
                if (std::abs (hits[(size_t) i].velocity - expected) > 0.02f)
                    fail (tag + ": hit " + juce::String (i) + " velocity " + juce::String (hits[(size_t) i].velocity)
                          + ", expected " + juce::String (expected));
            }
            std::printf ("%s: 5/5 hits, max timing error %d samples vs onset + %d (%.2f ms report delay)\n",
                         tag.toRawUTF8(), maxError, delay, 1000.0 * delay / sr);
        }

        // A more realistic onset: a decaying 200 Hz sine starting at a zero
        // crossing, so the threshold is reached part-way up the first cycle.
        {
            juce::AudioBuffer<float> tone (2, (int) sr / 2);
            tone.clear();
            for (float level : { -18.0f, -6.0f })
            {
                tone.clear();
                const float a = juce::Decibels::decibelsToGain (level);
                for (int i = 0; i < 4800; ++i)
                    for (int c = 0; c < 2; ++c)
                        tone.getWritePointer (c)[2000 + i] = a * std::exp (-(float) i / 480.0f)
                                                             * std::sin (juce::MathConstants<float>::twoPi * 200.0f * (float) i / (float) sr);
                det.reset();
                const auto hits = runDetector (det, tone, { 512 }, on);
                if (hits.size() != 1)
                    fail ("detector: 200 Hz burst at " + juce::String (level) + " dB gave " + juce::String ((int) hits.size()) + " hits");
                else
                    std::printf ("detector: 200 Hz sine burst at %+.0f dBFS -> hit %.2f ms after onset (v %.2f)\n",
                                 level, 1000.0 * (double) (hits[0].position - 2000) / sr, hits[0].velocity);
            }
        }
    }

    // 2. Retrigger: two short bursts 45 ms apart. The detector has re-armed by
    //    then (~36 ms), so Retrigger 50 ms swallows the second and 25 ms lets it through.
    {
        juce::AudioBuffer<float> sig (2, (int) sr / 2);
        sig.clear();
        addBurst (sig, sr, 4000, -12.0f, 1.0f, rng);
        addBurst (sig, sr, 4000 + (int) (0.045 * sr), -12.0f, 1.0f, rng);

        det.reset();
        auto hits = runDetector (det, sig, { 512 }, on);
        if (hits.size() != 1)
            fail ("retrigger 50 ms: expected 1 hit from bursts 45 ms apart, got " + juce::String ((int) hits.size()) + " " + describe (hits));

        auto fast = on;
        fast.retriggerMs = 25.0f;
        det.reset();
        hits = runDetector (det, sig, { 512 }, fast);
        if (hits.size() != 2)
            fail ("retrigger 25 ms: expected 2 hits from bursts 45 ms apart, got " + juce::String ((int) hits.size()) + " " + describe (hits));
        else
            std::printf ("retrigger: bursts 45 ms apart -> 1 hit at Retrigger 50 ms, 2 hits at 25 ms\n");
    }

    // 3. Sustained tones above the threshold fire once, even with the shortest
    //    Retrigger: low tones only just over the threshold (where the envelope
    //    sags most between peaks), and a level wobbling +-3 dB around the
    //    threshold at 4 Hz (which only the 6 dB hysteresis keeps quiet).
    {
        auto quick = on;
        quick.retriggerMs = 10.0f;
        struct Tone { float hz, db, wobbleDb; };
        for (const auto& tone : { Tone { 220.0f, -6.0f, 0.0f }, Tone { 1000.0f, 0.0f, 0.0f },
                                  Tone { 50.0f, -23.5f, 0.0f }, Tone { 20.0f, -23.5f, 0.0f },
                                  Tone { 440.0f, -24.0f, 3.0f } })
        {
            juce::AudioBuffer<float> sig (2, (int) sr * 2);
            for (int i = 0; i < sig.getNumSamples(); ++i)
            {
                const float t = (float) i / (float) sr;
                const float db = tone.db + tone.wobbleDb * std::sin (juce::MathConstants<float>::twoPi * 4.0f * t);
                const float v = juce::Decibels::decibelsToGain (db) * std::sin (juce::MathConstants<float>::twoPi * tone.hz * t);
                for (int c = 0; c < 2; ++c)
                    sig.getWritePointer (c)[i] = v;
            }
            det.reset();
            const auto hits = runDetector (det, sig, { 480 }, quick);
            if (hits.size() != 1)
                fail ("sustained " + juce::String (tone.hz) + " Hz at " + juce::String (tone.db) + " dB (wobble "
                      + juce::String (tone.wobbleDb) + " dB): expected 1 hit, got " + juce::String ((int) hits.size()));
        }
    }

    // 4. Silence and below-threshold input: nothing.
    {
        juce::AudioBuffer<float> sig (2, (int) sr);
        sig.clear();
        det.reset();
        if (const auto hits = runDetector (det, sig, { 512 }, on); ! hits.empty())
            fail ("silence gave " + juce::String ((int) hits.size()) + " hits");

        for (int i = 0; i < 10; ++i)
            addBurst (sig, sr, 1000 + i * 4000, -30.0f, 5.0f, rng); // 6 dB under the threshold
        det.reset();
        if (const auto hits = runDetector (det, sig, { 512 }, on); ! hits.empty())
            fail ("bursts below the threshold gave " + juce::String ((int) hits.size()) + " hits");
    }

    // 5. NaN / Inf in the sidechain: no crash, no endless firing, and real
    //    bursts around the garbage still register.
    {
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        juce::AudioBuffer<float> sig (2, (int) sr);
        for (float bad : { nan, inf, -inf })
        {
            for (int c = 0; c < 2; ++c)
                juce::FloatVectorOperations::fill (sig.getWritePointer (c), bad, sig.getNumSamples());
            det.reset();
            const auto hits = runDetector (det, sig, { 512 }, on);
            if (! hits.empty())
                fail ("sidechain full of " + juce::String (bad) + " gave " + juce::String ((int) hits.size()) + " hits");
        }

        // Garbage on one channel, alternating NaN/Inf, with two real bursts in between.
        sig.clear();
        for (int i = 0; i < sig.getNumSamples(); ++i)
            sig.getWritePointer (1)[i] = (i % 3 == 0) ? nan : (i % 3 == 1 ? inf : -inf);
        juce::AudioBuffer<float> left (sig.getArrayOfWritePointers(), 1, sig.getNumSamples());
        addBurst (left, sr, 5000, -6.0f, 10.0f, rng);
        addBurst (left, sr, 30000, -6.0f, 10.0f, rng);
        det.reset();
        const auto hits = runDetector (det, sig, { 512 }, on);
        bool sane = true;
        for (const auto& h : hits)
            sane &= h.position >= 0 && std::isfinite (h.velocity) && h.velocity > 0.0f && h.velocity <= 1.0f;
        if (hits.size() != 2 || ! sane)
            fail ("bursts next to a NaN/Inf channel: expected 2 sane hits, got " + describe (hits));
    }

    // 6. Trigger off: nothing, and switching it off mid-hit drops the pending hit.
    {
        juce::AudioBuffer<float> sig (2, (int) sr / 2);
        sig.clear();
        addBurst (sig, sr, 1000, 0.0f, 15.0f, rng);
        auto off = on;
        off.enabled = false;
        det.reset();
        if (const auto hits = runDetector (det, sig, { 512 }, off); ! hits.empty())
            fail ("trigger off gave " + juce::String ((int) hits.size()) + " hits");

        // Crossing at 1000 with the window running past the first block's end.
        juce::AudioBuffer<float> first (sig.getArrayOfWritePointers(), 2, 0, 1010);
        juce::AudioBuffer<float> rest (sig.getArrayOfWritePointers(), 2, 1010, sig.getNumSamples() - 1010);
        det.reset();
        det.process (first, first.getNumSamples(), on);
        det.process (rest, rest.getNumSamples(), off);
        if (det.getNumHits() != 0)
            fail ("a hit pending when the trigger was switched off still fired");

        // A sidechain bus with no channels (disabled) does nothing either.
        juce::AudioBuffer<float> none;
        det.process (none, 512, on);
        if (det.getNumHits() != 0)
            fail ("a sidechain with no channels gave hits");
    }

    // 7. End to end through processBlock with the sidechain enabled, the way
    //    JUCE lays the buffer out: max(in, out) channels, with the sidechain
    //    in the same channels the output is written to.
    {
        const int onsets[] { 3000, 20460, 40000 };
        juce::AudioBuffer<float> key (2, (int) sr);
        key.clear();
        for (int onset : onsets)
            addBurst (key, sr, onset, -6.0f, 15.0f, rng);

        struct Case
        {
            const char* name;
            juce::AudioChannelSet in, out;
            bool trigger;
            int expectedHits;
        };
        const Case cases[] {
            { "stereo sidechain -> stereo out", juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo(), true, 3 },
            { "stereo sidechain -> mono out",   juce::AudioChannelSet::stereo(), juce::AudioChannelSet::mono(),   true, 3 },
            { "mono sidechain -> stereo out",   juce::AudioChannelSet::mono(),   juce::AudioChannelSet::stereo(), true, 3 },
            { "sidechain bus disabled",         juce::AudioChannelSet::disabled(), juce::AudioChannelSet::stereo(), true, 0 },
            { "trigger off",                    juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo(), false, 0 },
        };

        for (const auto& tc : cases)
        {
            DrumFoundryProcessor p;
            juce::AudioProcessor::BusesLayout layout;
            layout.inputBuses.add (tc.in);
            layout.outputBuses.add (tc.out);
            if (p.getSidechainBusIndex() != 0 || ! p.setBusesLayout (layout))
            {
                fail (juce::String (tc.name) + ": layout refused");
                continue;
            }
            setParam (p, pid::scTrigger, tc.trigger ? 1.0f : 0.0f);
            p.prepareToPlay (sr, 512);

            const int numChannels = juce::jmax (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
            juce::AudioBuffer<float> out (2, key.getNumSamples());
            out.clear();
            juce::AudioBuffer<float> block (numChannels, 512);
            juce::MidiBuffer midi;
            bool finite = true;

            for (int pos = 0; pos < key.getNumSamples(); pos += 512)
            {
                const int len = juce::jmin (512, key.getNumSamples() - pos);
                block.setSize (numChannels, len, false, false, true);
                block.clear();
                // What the host passes in: sidechain channels first. When the bus
                // is disabled, fill with the key anyway, like Live leaving stale
                // audio in the buffer: it must be ignored.
                const int inChannels = tc.in.isDisabled() ? numChannels : tc.in.size();
                for (int c = 0; c < inChannels; ++c)
                    block.copyFrom (c, 0, key, juce::jmin (c, 1), pos, len);

                p.processBlock (block, midi);
                finite &= allFinite (block);
                for (int c = 0; c < 2; ++c)
                    out.copyFrom (c, pos, block, juce::jmin (c, numChannels - 1), 0, len);
            }

            const auto tag = juce::String (tc.name);
            if (! finite)
                fail (tag + ": non-finite output");
            if (p.getHitCount() != tc.expectedHits)
                fail (tag + ": " + juce::String (p.getHitCount()) + " voices started, expected " + juce::String (tc.expectedHits));

            // The key itself must never reach the output: nothing before the
            // first hit (which lands `delay` samples after the burst starts, and
            // comes out after the reported latency, less the drive's oversampler:
            // an IIR answers at once. What's left is the output clipper and the
            // delay standing in for the chain's modules that aren't in it).
            DriveStage driveForLatency;
            driveForLatency.prepare (sr, 512);
            const int clipDelay = p.getLatencySamples() - driveForLatency.getLatencySamples();
            const float leak = out.getMagnitude (0, onsets[0] + delay + clipDelay);
            if (leak != 0.0f)
                fail (tag + ": sidechain leaked to the output (peak " + juce::String (leak) + " before the first hit)");

            if (tc.expectedHits > 0)
            {
                for (int onset : onsets)
                {
                    const float atHit = out.getMagnitude (0, onset + delay, 2000);
                    if (atHit < 0.01f)
                        fail (tag + ": near-silent output at the hit at " + juce::String (onset + delay));
                }
                // Mono out with a stereo sidechain: the second buffer channel isn't
                // an output, but it held the key and must come back cleared.
                if (tc.out == juce::AudioChannelSet::mono() && numChannels > 1)
                    if (const float ch1 = block.getMagnitude (1, 0, block.getNumSamples()); ch1 != 0.0f)
                        fail (tag + ": extra channel not cleared (" + juce::String (ch1) + ")");
                // Onset of the first synth hit is exactly where the detector reported it.
                const int first = onsets[0] + delay + clipDelay;
                if (out.getMagnitude (0, first, 1) == 0.0f && out.getMagnitude (0, first + 1, 1) == 0.0f)
                    fail (tag + ": first hit didn't start at sample " + juce::String (first));
                std::printf ("end to end, %s: %d voices, peak %.2f, finite\n", tc.name, p.getHitCount(),
                             out.getMagnitude (0, out.getNumSamples()));
            }
        }

        // MIDI and sidechain hits in the same block both play, in order.
        {
            DrumFoundryProcessor p;
            setParam (p, pid::scTrigger, 1.0f);
            p.prepareToPlay (sr, 512);
            juce::AudioBuffer<float> block (2, 512);
            block.clear();
            addBurst (block, sr, 100, 0.0f, 15.0f, rng);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 50);
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 300);
            p.processBlock (block, midi);
            if (p.getHitCount() != 3 || ! allFinite (block))
                fail ("MIDI + sidechain in one block: " + juce::String (p.getHitCount()) + " voices, expected 3");
            if (block.getMagnitude (0, 0, 50) != 0.0f)
                fail ("MIDI + sidechain in one block: output before the first note");
        }

        // Bus layouts: the sidechain may be off/mono/stereo, nothing else; the
        // standalone build has no input bus at all.
        {
            DrumFoundryProcessor p;
            const auto supported = [&p] (juce::AudioChannelSet in, juce::AudioChannelSet out)
            {
                juce::AudioProcessor::BusesLayout l;
                l.inputBuses.add (in);
                l.outputBuses.add (out);
                return p.checkBusesLayoutSupported (l);
            };
            using Set = juce::AudioChannelSet;
            if (! supported (Set::disabled(), Set::stereo()) || ! supported (Set::mono(), Set::mono())
                || supported (Set::create5point1(), Set::stereo()) || supported (Set::stereo(), Set::create5point1())
                || supported (Set::stereo(), Set::disabled()))
                fail ("isBusesLayoutSupported accepts/refuses the wrong layouts");

            DrumFoundryProcessor standalone (juce::AudioProcessor::wrapperType_Standalone);
            if (standalone.getBusCount (true) != 0 || standalone.getSidechainBusIndex() != -1)
                fail ("standalone build has an input bus");
            if (p.getBusCount (true) != 1 || p.getBus (true, 0)->getName() != "Sidechain" || ! p.getBus (true, 0)->isEnabled())
                fail ("plugin build lacks an enabled Sidechain input bus");
        }
    }

    // 8. Presets leave the sidechain settings alone; host recall restores them.
    {
        DrumFoundryProcessor p;
        const auto setSc = [&p] (float enabled, float thr, float retrig)
        {
            setParam (p, pid::scTrigger, enabled);
            setParam (p, pid::scThreshold, thr);
            setParam (p, pid::scRetrigger, retrig);
        };
        const auto scIs = [] (DrumFoundryProcessor& q, float enabled, float thr, float retrig)
        {
            return juce::approximatelyEqual (rawParam (q, pid::scTrigger), enabled)
                && std::abs (rawParam (q, pid::scThreshold) - thr) < 0.01f
                && std::abs (rawParam (q, pid::scRetrigger) - retrig) < 0.01f;
        };
        const auto scString = [] (DrumFoundryProcessor& q)
        {
            return juce::String (rawParam (q, pid::scTrigger)) + "/" + juce::String (rawParam (q, pid::scThreshold))
                 + "/" + juce::String (rawParam (q, pid::scRetrigger));
        };

        setSc (1.0f, -12.0f, 120.0f);
        auto& presets = p.getPresets();
        for (int i : { 3, 0, 7 })
        {
            presets.load (i);
            if (! scIs (p, 1.0f, -12.0f, 120.0f))
                fail ("factory preset " + presets.getEntries()[(size_t) i].name + " changed the sidechain settings to " + scString (p));
        }

        // User preset: saving strips the sc values, and loading one (even an
        // old file that has them) leaves the current ones in place. Uses a
        // throwaway file in the real user folder, removed afterwards.
        const auto name = "__snareone_check_" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt());
        if (! presets.saveUserPreset (name))
        {
            fail ("could not save a user preset for the preset checks");
        }
        else
        {
            const auto file = PresetManager::getUserFolder().getChildFile (name + PresetManager::fileExtension);
            const auto text = file.loadFileAsString();
            for (const auto& id : sessionOnlyParameterIds())
                if (text.contains ("\"" + id + "\""))
                    fail ("user preset file stores " + id);

            // Simulate an older preset that did store them (all different).
            if (auto xml = juce::XmlDocument::parse (file))
            {
                for (const auto& [id, v] : { std::pair<const char*, float> { pid::scTrigger, 0.0f },
                                             { pid::scThreshold, -50.0f }, { pid::scRetrigger, 200.0f } })
                {
                    auto* e = xml->createNewChildElement ("PARAM");
                    e->setAttribute ("id", id);
                    e->setAttribute ("value", v);
                }
                xml->writeTo (file);
            }
            presets.refresh();

            setSc (0.0f, -36.0f, 30.0f);
            int index = -1;
            for (int i = 0; i < (int) presets.getEntries().size(); ++i)
                if (presets.getEntries()[(size_t) i].name == name)
                    index = i;
            presets.load (index);
            if (index < 0 || presets.getCurrentName() != name)
                fail ("user preset didn't load");
            if (! scIs (p, 0.0f, -36.0f, 30.0f))
                fail ("user preset changed the sidechain settings to " + scString (p));

            file.deleteFile();
            presets.refresh();
        }

        // Host recall restores them.
        setSc (1.0f, -9.0f, 180.0f);
        juce::MemoryBlock session;
        p.getStateInformation (session);
        DrumFoundryProcessor recalled;
        recalled.setStateInformation (session.getData(), (int) session.getSize());
        if (! scIs (recalled, 1.0f, -9.0f, 180.0f))
            fail ("setStateInformation did not restore the sidechain settings (" + scString (recalled) + ")");
        const auto sc = recalled.readSidechainParams();
        if (! sc.enabled || std::abs (sc.thresholdDb + 9.0f) > 0.01f || std::abs (sc.retriggerMs - 180.0f) > 0.01f)
            fail ("ParamReader::readSidechain disagrees with the restored values");
        else
            std::printf ("presets keep sidechain settings; session recall restores them (%s)\n", scString (recalled).toRawUTF8());
    }

    // --- Body Style migration: format 2 had Sample at 6, where Round is now ---------------
    {
        const auto bodyStyleAfterLoading = [] (int format, double value)
        {
            DrumFoundryProcessor source, loaded;
            auto tree = source.createStateTree();
            tree.setProperty (DrumFoundryProcessor::formatVersionId, format, nullptr);
            for (auto child : tree)
                if (child.getProperty ("id").toString() == pid::bodyStyle)
                    child.setProperty ("value", value, nullptr);
            loaded.loadStateTree (tree);
            return juce::roundToInt (loaded.getState().getRawParameterValue (pid::bodyStyle)->load());
        };
        const int oldSample = bodyStyleAfterLoading (2, 6.0), newRound = bodyStyleAfterLoading (3, 6.0),
                  oldFM = bodyStyleAfterLoading (2, 5.0);
        if (oldSample != bodySample || newRound != bodyRound || oldFM != bodyFM)
            fail ("Body Style migration: format 2 Sample -> " + juce::String (oldSample) + ", format 3 Round -> "
                  + juce::String (newRound) + ", format 2 FM -> " + juce::String (oldFM));
        else
            std::printf ("Body Style migration: format 2 Sample (6) loads as Sample (%d); format 3 keeps Round\n", oldSample);
    }

    return failures;
}


//==============================================================================
// Reverb and gate checks

/** Renders `total` samples with a fresh engine in blocks of blockSize. Hits
    are MIDI notes (or sidechain TriggerHits) at absolute sample positions. */
juce::AudioBuffer<float> renderEngine (const DrumParams& p, const EnvTables& tables, double sr,
                                       const std::vector<int>& hits, int total, int blockSize = 512,
                                       bool reverbEnabled = true, bool asTriggerHits = false)
{
    DrumEngine engine;
    engine.prepare (sr, blockSize);
    engine.setParams (p);
    engine.setTables (tables);
    engine.setSeed (0x5EEDu);
    engine.setReverbEnabled (reverbEnabled);

    juce::AudioBuffer<float> out (2, total);
    juce::AudioBuffer<float> block (2, blockSize);
    std::vector<TriggerHit> triggerHits;
    for (int pos = 0; pos < total; pos += blockSize)
    {
        const int n = juce::jmin (blockSize, total - pos);
        block.setSize (2, n, false, false, true);
        juce::MidiBuffer midi;
        triggerHits.clear();
        for (int h : hits)
            if (h >= pos && h < pos + n)
            {
                if (asTriggerHits)
                    triggerHits.push_back ({ h - pos, 1.0f });
                else
                    midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), h - pos);
            }
        engine.process (block, midi, triggerHits.data(), (int) triggerHits.size());
        out.copyFrom (0, pos, block, 0, 0, n);
        out.copyFrom (1, pos, block, 1, 0, n);
    }
    return out;
}

/** The wet signal alone: the render minus the same render with Mix at 0.
    Exact as long as the clipper is off (everything after the reverb is linear). */
juce::AudioBuffer<float> renderWet (DrumParams p, const EnvTables& tables, double sr,
                                    const std::vector<int>& hits, int total, int blockSize = 512,
                                    bool asTriggerHits = false)
{
    p.clipMode = 0;
    auto wet = renderEngine (p, tables, sr, hits, total, blockSize, true, asTriggerHits);
    p.revMix = 0.0f;
    const auto dry = renderEngine (p, tables, sr, hits, total, blockSize, true, asTriggerHits);
    for (int c = 0; c < 2; ++c)
        juce::FloatVectorOperations::subtract (wet.getWritePointer (c), dry.getReadPointer (c), total);
    return wet;
}

float peakIn (const juce::AudioBuffer<float>& b, int start, int end)
{
    start = juce::jlimit (0, b.getNumSamples(), start);
    end = juce::jlimit (start, b.getNumSamples(), end);
    return end > start ? juce::jmax (b.getMagnitude (0, start, end - start), b.getMagnitude (1, start, end - start)) : 0.0f;
}

float rmsIn (const juce::AudioBuffer<float>& b, int start, int end)
{
    start = juce::jlimit (0, b.getNumSamples(), start);
    end = juce::jlimit (start, b.getNumSamples(), end);
    return end > start ? 0.5f * (b.getRMSLevel (0, start, end - start) + b.getRMSLevel (1, start, end - start)) : 0.0f;
}

/** T30 (from -5 to -35 dB on the Schroeder decay curve, doubled) of a stereo
    impulse response, in seconds, or -1 if the curve never gets that low. */
double measureRt60 (const juce::AudioBuffer<float>& ir, double sr)
{
    const int n = ir.getNumSamples();
    std::vector<double> edc ((size_t) n + 1, 0.0);
    for (int i = n; --i >= 0;)
    {
        const double l = ir.getSample (0, i), r = ir.getSample (1, i);
        edc[(size_t) i] = edc[(size_t) i + 1] + l * l + r * r;
    }
    if (edc[0] <= 0.0)
        return -1.0;

    int t5 = -1, t35 = -1;
    for (int i = 0; i < n; ++i)
    {
        const double db = 10.0 * std::log10 (edc[(size_t) i] / edc[0] + 1.0e-30);
        if (t5 < 0 && db <= -5.0)  t5 = i;
        if (t35 < 0 && db <= -35.0) { t35 = i; break; }
    }
    return (t5 < 0 || t35 < 0) ? -1.0 : 2.0 * (t35 - t5) / sr;
}

DrumParams reverbTestParams (DrumFoundryProcessor& proc)
{
    auto p = proc.readParams();
    p.revEngine = revEngineClassic; // these checks are of the Classic engine's design (checkReverbEngines has Cloud's)
    p.clipMode = 0;
    p.revMix = 0.6f;
    p.revSize = 0.6f;
    p.revDecayMs = 2500.0f;
    p.revPreDelayMs = 10.0f;
    p.revLowCut = 200.0f;
    p.revHighCut = 8000.0f;
    p.revGate = true;
    p.revGateMs = 180.0f;
    p.revFadeMs = 15.0f;
    return p;
}

int checkReverb()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: %s\n", msg.toRawUTF8()); ++failures; };

    constexpr double sr = 48000.0;
    const auto ms = [] (double v) { return (int) std::lround (v * sr * 0.001); };

    DrumFoundryProcessor proc;
    EnvTables tables;
    proc.bakeTables (tables);
    const auto base = reverbTestParams (proc);

    DrumEngine probe;
    probe.prepare (sr, 512);
    const int latency = probe.getLatencySamples();

    // 1. One hit: the wet is there before the cut and exactly silent from
    //    Gate Time + Gate Fade after the hit (plus the oversampler's latency).
    {
        const int hit = 1000;
        const auto wet = renderWet (base, tables, sr, { hit }, (int) sr);
        const int cut = hit + latency + ms (base.revGateMs) + ms (base.revFadeMs);
        const float after = peakIn (wet, cut, wet.getNumSamples());
        const float before = rmsIn (wet, hit + ms (base.revPreDelayMs + 10.0), hit + ms (base.revGateMs));
        const float lastOpen = peakIn (wet, cut - ms (base.revFadeMs) - ms (20.0), cut - ms (base.revFadeMs));
        if (after > 1.0e-6f)
            fail ("gate: wet after the cut peaks at " + juce::String (after));
        if (before < 1.0e-3f || lastOpen < 1.0e-3f)
            fail ("gate: wet too quiet while open (rms " + juce::String (before) + ", peak just before the fade "
                  + juce::String (lastOpen) + ")");
        std::printf ("gate: one hit, open rms %.1f dB, peak in the 20 ms before the fade %.1f dB, "
                     "peak from %d samples on (hit + latency %d + gate + fade) %s\n",
                     juce::Decibels::gainToDecibels (before), juce::Decibels::gainToDecibels (lastOpen),
                     cut - hit, latency, after == 0.0f ? "exactly 0" : juce::String (after).toRawUTF8());
    }

    // 2. The gate's envelope, straight from the Reverb: steady noise in, gate
    //    on vs off; their ratio is the gate gain. Checks the attack, the hold
    //    measured from the hit (+ hitDelay), the raised-cosine fade length, and
    //    a retrigger in the middle of the fade.
    {
        const int total = (int) sr;
        juce::Random rng (99);
        juce::AudioBuffer<float> input (2, total);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < total; ++i)
                input.setSample (c, i, 0.1f * (rng.nextFloat() * 2.0f - 1.0f));

        struct Run { std::vector<int> hits; float fadeMs; int hitDelay; };
        const auto gains = [&] (const Run& run)
        {
            juce::AudioBuffer<float> gated (input), open (input);
            Reverb a, b;
            a.prepare (sr);
            b.prepare (sr);
            auto pa = base;
            pa.revFadeMs = run.fadeMs;
            auto pb = pa;
            pb.revGate = false;
            constexpr int block = 256;
            for (int pos = 0; pos < total; pos += block)
            {
                const int n = juce::jmin (block, total - pos);
                std::vector<int> offsets;
                for (int h : run.hits)
                    if (h >= pos && h < pos + n)
                        offsets.push_back (h - pos);
                a.process (gated.getWritePointer (0, pos), gated.getWritePointer (1, pos), n, pa,
                           offsets.data(), (int) offsets.size(), run.hitDelay);
                b.process (open.getWritePointer (0, pos), open.getWritePointer (1, pos), n, pb,
                           offsets.data(), (int) offsets.size(), run.hitDelay);
            }
            std::vector<float> g ((size_t) total, -1.0f); // -1: too quiet to tell
            for (int i = 0; i < total; ++i)
            {
                const float w = open.getSample (0, i) - input.getSample (0, i);
                if (std::abs (w) > 2.0e-3f)
                    g[(size_t) i] = (gated.getSample (0, i) - input.getSample (0, i)) / w;
            }
            return g;
        };

        for (float fadeMs : { 1.0f, 15.0f, 80.0f })
        {
            const int hit = 20000, delay = 7;
            const auto g = gains ({ { hit }, fadeMs, delay });
            const int arrive = hit + delay, fadeStart = arrive + ms (base.revGateMs), fadeEnd = fadeStart + ms (fadeMs);

            float holdMin = 1.0f, holdMax = 0.0f, closedMax = 0.0f, preMax = 0.0f;
            for (int i = hit - 2000; i < arrive; ++i)
                if (g[(size_t) i] >= 0) preMax = juce::jmax (preMax, std::abs (g[(size_t) i]));
            for (int i = arrive + ms (Reverb::gateAttackMs) + 1; i <= fadeStart; ++i)
                if (g[(size_t) i] >= 0) { holdMin = juce::jmin (holdMin, g[(size_t) i]); holdMax = juce::jmax (holdMax, g[(size_t) i]); }
            for (int i = fadeEnd; i < fadeEnd + 4000; ++i)
                if (g[(size_t) i] >= 0) closedMax = juce::jmax (closedMax, std::abs (g[(size_t) i]));

            // Measured fade: where the gain crosses 90%, 50% and 10% on the way
            // down, and the first sample that is fully shut. A raised cosine
            // falls from 90% to 10% in 0.590 of its length, and is at 50%
            // half-way through.
            const auto crossing = [&] (float level)
            {
                for (int i = fadeStart - 10; i < total; ++i)
                    if (g[(size_t) i] >= 0 && g[(size_t) i] < level)
                        return i;
                return -1;
            };
            int firstShut = -1;
            for (int i = fadeStart; i < total && firstShut < 0; ++i)
                if (juce::exactlyEqual (g[(size_t) i], 0.0f))
                    firstShut = i;
            const int t90 = crossing (0.9f), t50 = crossing (0.5f), t10 = crossing (0.1f);
            const double fall = t10 - t90, expectedFall = 0.5903 * ms (fadeMs);
            const int midI = fadeStart + ms (fadeMs) / 2;
            const float mid = g[(size_t) midI];
            const auto tag = "gate envelope (fade " + juce::String (fadeMs) + " ms)";
            if (preMax > 1.0e-6f)
                fail (tag + ": gate not shut before the hit arrived (" + juce::String (preMax) + ")");
            if (holdMin < 0.9999f || holdMax > 1.0001f)
                fail (tag + ": gain during the hold " + juce::String (holdMin) + ".." + juce::String (holdMax));
            if (closedMax > 1.0e-6f)
                fail (tag + ": not shut after the fade (" + juce::String (closedMax) + ")");
            if (std::abs (firstShut - fadeEnd) > 1 || std::abs (t50 - midI) > 2 || std::abs (fall - expectedFall) > 2.0)
                fail (tag + ": 50% at " + juce::String (t50 - arrive) + ", shut at " + juce::String (firstShut - arrive)
                      + " samples after the hit arrived, 90->10% in " + juce::String (fall) + " samples; expected "
                      + juce::String (midI - arrive) + ", " + juce::String (fadeEnd - arrive) + ", " + juce::String (expectedFall, 1));
            if (mid >= 0 && fadeMs > 5.0f && std::abs (mid - 0.5f) > 0.03f)
                fail (tag + ": gain half-way through the fade is " + juce::String (mid) + ", expected 0.5");
            const int lastOpen = fadeStart, firstShutPrinted = firstShut;
            std::printf ("%s: hit arrives at +%d (hitDelay), 50%% at %d, shut at %d samples after it (gate %d + fade %d); "
                         "90->10%% in %.0f samples (raised cosine: %.1f)\n", tag.toRawUTF8(), delay, t50 - arrive,
                         firstShutPrinted - arrive, lastOpen - arrive, ms (fadeMs), fall, expectedFall);
        }

        // Retrigger half-way through a 40 ms fade: the gain turns round and is
        // back at 1 within the 1 ms attack, with no step.
        {
            const int hit = 20000, fadeStart = hit + ms (base.revGateMs), again = fadeStart + ms (20.0);
            const auto g = gains ({ { hit, again }, 40.0f, 0 });
            float maxStep = 0.0f, prev = -1.0f;
            int reopened = -1;
            for (int i = fadeStart; i < again + ms (5.0); ++i)
            {
                if (g[(size_t) i] < 0) { prev = -1.0f; continue; }
                if (prev >= 0) maxStep = juce::jmax (maxStep, std::abs (g[(size_t) i] - prev));
                prev = g[(size_t) i];
                if (i > again && reopened < 0 && g[(size_t) i] > 0.9999f) reopened = i;
            }
            const float atRetrigger = g[(size_t) again];
            if (reopened < 0 || reopened - again > ms (1.0) + 2)
                fail ("gate retrigger during the fade: back open after " + juce::String (reopened - again) + " samples");
            if (maxStep > 0.03f)
                fail ("gate retrigger during the fade: gain jumps by " + juce::String (maxStep) + " in one sample");
            std::printf ("gate retrigger mid-fade: gain %.3f at the retrigger, fully open %.2f ms later, largest step %.4f\n",
                         atRetrigger, 1000.0 * (reopened - again) / sr, maxStep);
        }
    }

    // 3. Retriggering while the gate is open extends it (through the engine).
    {
        const int first = 1000, second = first + ms (100.0);
        const auto wet = renderWet (base, tables, sr, { first, second }, (int) sr);
        const int firstCut = first + latency + ms (base.revGateMs) + ms (base.revFadeMs);
        const int secondCut = second + latency + ms (base.revGateMs) + ms (base.revFadeMs);
        const float between = rmsIn (wet, firstCut, secondCut - ms (base.revFadeMs));
        const float after = peakIn (wet, secondCut, wet.getNumSamples());
        if (between < 1.0e-3f)
            fail ("gate retrigger: silent after the first hit's cut (rms " + juce::String (between) + ")");
        if (after > 1.0e-6f)
            fail ("gate retrigger: wet after the second hit's cut (" + juce::String (after) + ")");
        std::printf ("gate retrigger at +100 ms: open past the first cut (rms %.1f dB), silent after the second (%s)\n",
                     juce::Decibels::gainToDecibels (between), after == 0.0f ? "exactly 0" : juce::String (after).toRawUTF8());
    }

    // 4. Gate off: the tail's RT60 follows Decay.
    {
        for (float size : { 0.2f, 0.6f, 1.0f })
            for (float decay : { 500.0f, 2000.0f })
            {
                auto p = base;
                p.revGate = false;
                p.revSize = size;
                p.revDecayMs = decay;
                p.revPreDelayMs = 0.0f;
                const int total = (int) (sr * decay * 0.001 * 1.6) + 4096;
                juce::AudioBuffer<float> ir (2, total);
                ir.clear();
                juce::Random rng (7);
                for (int i = 0; i < ms (2.0); ++i)
                    for (int c = 0; c < 2; ++c)
                        ir.setSample (c, i, 0.5f * (rng.nextFloat() * 2.0f - 1.0f));
                juce::AudioBuffer<float> dry (ir);
                Reverb r;
                r.prepare (sr);
                for (int pos = 0; pos < total; pos += 512)
                    r.process (ir.getWritePointer (0, pos), ir.getWritePointer (1, pos), juce::jmin (512, total - pos), p, nullptr, 0, 0);
                for (int c = 0; c < 2; ++c)
                    juce::FloatVectorOperations::subtract (ir.getWritePointer (c), dry.getReadPointer (c), total);

                const double rt = measureRt60 (ir, sr);
                const double err = rt / (decay * 0.001) - 1.0;
                if (rt < 0 || std::abs (err) > 0.25)
                    fail ("RT60 at size " + juce::String (size) + ", decay " + juce::String (decay) + " ms measured "
                          + juce::String (rt * 1000.0, 0) + " ms");
                std::printf ("gate off, size %.1f, decay %4.0f ms: measured T30 %4.0f ms (%+.0f%%)\n",
                             size, decay, rt * 1000.0, err * 100.0);
            }
    }

    // 5. Mix 0: bit-identical to the chain with the reverb bypassed, for every
    //    factory preset, whatever the other reverb settings, and it stays idle.
    {
        juce::Random rng (5);
        int compared = 0;
        const auto& entries = proc.getPresets().getEntries();
        for (int i = 0; i < (int) entries.size(); ++i)
        {
            if (! entries[(size_t) i].isFactory)
                continue;
            proc.getPresets().load (i);
            EnvTables t;
            proc.bakeTables (t);
            auto p = proc.readParams();
            p.revMix = 0.0f;
            p.revSize = rng.nextFloat();
            p.revDecayMs = 100.0f + 5900.0f * rng.nextFloat();
            p.revPreDelayMs = 80.0f * rng.nextFloat();
            p.revGate = rng.nextBool();
            const std::vector<int> hits { 0, 511, 7000, 7001, 30000 };
            const auto on = renderEngine (p, t, sr, hits, (int) sr, 480, true);
            const auto off = renderEngine (p, t, sr, hits, (int) sr, 480, false);
            bool same = true;
            for (int c = 0; c < 2; ++c)
                same &= std::memcmp (on.getReadPointer (c), off.getReadPointer (c), sizeof (float) * (size_t) on.getNumSamples()) == 0;
            if (! same)
                fail ("Mix 0 differs from the bypassed reverb on " + entries[(size_t) i].name);
            ++compared;
        }

        DrumEngine e;
        e.prepare (sr, 512);
        auto p = base;
        p.revMix = 0.0f;
        e.setParams (p);
        e.setTables (tables);
        juce::AudioBuffer<float> buf (2, 512);
        bool alwaysIdle = true;
        for (int b = 0; b < 100; ++b)
        {
            juce::MidiBuffer m;
            if (b % 20 == 0)
                m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 17);
            e.process (buf, m);
            alwaysIdle &= e.isReverbIdle();
        }
        if (! alwaysIdle)
            fail ("the reverb woke up with Mix at 0");
        std::printf ("Mix 0: bit-identical to the bypassed reverb on %d factory presets; reverb stays idle\n", compared);

        // Turning Mix down lets the tail ring out instead of cutting it.
        auto q = base;
        q.revGate = false;
        DrumEngine d;
        d.prepare (sr, 512);
        d.setParams (q);
        d.setTables (tables);
        d.setSeed (1);
        juce::AudioBuffer<float> out (2, (int) sr);
        for (int pos = 0; pos < out.getNumSamples(); pos += 512)
        {
            juce::MidiBuffer m;
            if (pos == 0)
                m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
            if (pos == 512 * 40) // ~0.43 s, after the dry hit
                q.revMix = 0.0f;
            d.setParams (q);
            juce::AudioBuffer<float> blockBuf (out.getArrayOfWritePointers(), 2, pos, juce::jmin (512, out.getNumSamples() - pos));
            d.process (blockBuf, m);
        }
        const float tailAfter = rmsIn (out, 512 * 40 + ms (100.0), 512 * 40 + ms (200.0));
        if (tailAfter < 1.0e-4f)
            fail ("turning Mix to 0 cut the ringing tail (rms " + juce::String (tailAfter) + ")");
        else
            std::printf ("Mix to 0 mid-tail: tail keeps ringing (rms %.1f dB 100-200 ms later)\n", juce::Decibels::gainToDecibels (tailAfter));
    }

    // 6. Extremes and random automation: finite and bounded.
    {
        float worst = 0.0f;
        bool finite = true;
        int combos = 0;
        auto p = base;
        p.revMix = 1.0f;
        for (int mask = 0; mask < 128; ++mask)
        {
            p.revSize       = (mask & 1)  ? 1.0f : 0.0f;
            p.revDecayMs    = (mask & 2)  ? 6000.0f : 100.0f;
            p.revPreDelayMs = (mask & 4)  ? 80.0f : 0.0f;
            p.revLowCut     = (mask & 8)  ? 1000.0f : 20.0f;
            p.revHighCut    = (mask & 16) ? 20000.0f : 1000.0f;
            p.revGate       = (mask & 32) != 0;
            p.revGateMs     = (mask & 64) ? 2000.0f : 10.0f;
            p.revFadeMs     = (mask & 64) ? 1.0f : 200.0f;
            const auto out = renderEngine (p, tables, sr, { 0, ms (250.0), ms (500.0), ms (750.0) }, (int) sr, 256);
            finite &= allFinite (out);
            worst = juce::jmax (worst, peakIn (out, 0, out.getNumSamples()));
            ++combos;
        }

        // Every reverb parameter jumping at random every block, with random hits.
        DrumEngine e;
        e.prepare (sr, 256);
        e.setTables (tables);
        juce::Random rng (2024);
        juce::AudioBuffer<float> buf (2, 256);
        float randomPeak = 0.0f;
        for (int b = 0; b < (int) (sr * 6.0 / 256); ++b)
        {
            auto q = base;
            const auto pick = [&rng] (float lo, float hi) { const float r = rng.nextFloat(); return r < 0.15f ? lo : r > 0.85f ? hi : lo + (hi - lo) * rng.nextFloat(); };
            q.revMix = pick (0.0f, 1.0f);
            q.revSize = pick (0.0f, 1.0f);
            q.revDecayMs = pick (100.0f, 6000.0f);
            q.revPreDelayMs = pick (0.0f, 80.0f);
            q.revLowCut = pick (20.0f, 1000.0f);
            q.revHighCut = pick (1000.0f, 20000.0f);
            q.revGate = rng.nextBool();
            q.revGateMs = pick (10.0f, 2000.0f);
            q.revFadeMs = pick (1.0f, 200.0f);
            e.setParams (q);
            juce::MidiBuffer m;
            if (rng.nextInt (12) == 0)
                m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), rng.nextInt (256));
            e.process (buf, m);
            finite &= allFinite (buf);
            randomPeak = juce::jmax (randomPeak, buf.getMagnitude (0, 0, 256), buf.getMagnitude (1, 0, 256));
        }

        // Longest decay, smallest room, hits every 100 ms for 12 s: the level
        // must settle rather than keep growing.
        auto dense = base;
        dense.revMix = 1.0f;
        dense.revSize = 0.0f;
        dense.revDecayMs = 6000.0f;
        dense.revGate = false;
        dense.revLowCut = 20.0f;
        dense.revHighCut = 20000.0f;
        std::vector<int> hits;
        for (int i = 0; i < 120; ++i)
            hits.push_back (ms (100.0 * i));
        const auto out = renderEngine (dense, tables, sr, hits, (int) (sr * 12.0), 512);
        finite &= allFinite (out);
        const float early = rmsIn (out, (int) (sr * 5.0), (int) (sr * 6.0));
        const float late = rmsIn (out, (int) (sr * 11.0), (int) (sr * 12.0));

        if (! finite)
            fail ("reverb extremes / automation: non-finite output");
        if (worst > 8.0f || randomPeak > 8.0f || late > early * 1.25f)
            fail ("reverb extremes / automation: peaks " + juce::String (worst) + " / " + juce::String (randomPeak)
                  + ", dense 6 s decay " + juce::String (early) + " -> " + juce::String (late));
        std::printf ("reverb extremes: %d min/max combos peak %.2f (clipper off); 6 s random automation peak %.2f; "
                     "hits every 100 ms at 6 s decay: rms %.3f at 5-6 s, %.3f at 11-12 s\n",
                     combos, worst, randomPeak, early, late);
    }

    // 7. A sidechain TriggerHit opens the gate just as a MIDI note does, and an
    //    audition hit (triggerAudition) does too.
    {
        const int hit = 5300;
        const auto midiWet = renderWet (base, tables, sr, { hit }, (int) sr, 512, false);
        const auto scWet = renderWet (base, tables, sr, { hit }, (int) sr, 512, true);
        const int cut = hit + latency + ms (base.revGateMs) + ms (base.revFadeMs);
        const float open = rmsIn (scWet, hit + ms (20.0), hit + ms (base.revGateMs));
        const float after = peakIn (scWet, cut, scWet.getNumSamples());
        float diff = 0.0f;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < scWet.getNumSamples(); ++i)
                diff = juce::jmax (diff, std::abs (scWet.getSample (c, i) - midiWet.getSample (c, i)));
        if (open < 1.0e-3f || after > 1.0e-6f || diff > 1.0e-6f)
            fail ("sidechain hit and the gate: open rms " + juce::String (open) + ", after cut " + juce::String (after)
                  + ", diff from MIDI " + juce::String (diff));

        // Audition through processBlock: wet (vs. Mix 0) present, then cut.
        const auto auditionRun = [&] (float mix)
        {
            DrumFoundryProcessor p;
            setParam (p, pid::revMix, mix);
            setParam (p, pid::clip, 0.0f);
            p.prepareToPlay (sr, 512);
            juce::AudioBuffer<float> out (2, (int) sr / 2), block (2, 512);
            for (int pos = 0; pos < out.getNumSamples(); pos += 512)
            {
                if (pos == 512 * 4)
                    p.triggerAudition (1.0f);
                juce::MidiBuffer m;
                block.clear();
                p.processBlock (block, m);
                for (int c = 0; c < 2; ++c)
                    out.copyFrom (c, pos, block, c, 0, juce::jmin (512, out.getNumSamples() - pos));
            }
            return out;
        };
        auto aw = auditionRun (0.5f);
        const auto ad = auditionRun (0.0f);
        for (int c = 0; c < 2; ++c)
            juce::FloatVectorOperations::subtract (aw.getWritePointer (c), ad.getReadPointer (c), aw.getNumSamples());
        const int aHit = 512 * 4;
        const float aOpen = rmsIn (aw, aHit + ms (20.0), aHit + ms (180.0));
        const float aAfter = peakIn (aw, aHit + latency + ms (180.0) + ms (15.0), aw.getNumSamples());
        if (aOpen < 1.0e-3f || aAfter > 1.0e-6f)
            fail ("audition hit and the gate: open rms " + juce::String (aOpen) + ", after cut " + juce::String (aAfter));
        std::printf ("sidechain TriggerHit opens the gate like MIDI (max diff %.1e, open rms %.1f dB, silent after the cut); "
                     "audition too (open rms %.1f dB, after cut %s)\n", diff, juce::Decibels::gainToDecibels (open),
                     juce::Decibels::gainToDecibels (aOpen), aAfter == 0.0f ? "exactly 0" : juce::String (aAfter).toRawUTF8());
    }

    // 8. The tail estimate and the offline render (display / DRAG export).
    {
        auto p = base;
        const auto time = [&] (const DrumParams& q, int& samples)
        {
            const auto start = juce::Time::getHighResolutionTicks();
            const auto hit = renderHit (q, tables, sr);
            samples = hit.getNumSamples();
            if (! allFinite (hit))
                fail ("renderHit with the reverb on gave non-finite samples");
            return 1000.0 * juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
        };

        int dryLen = 0, gatedLen = 0, openLen = 0, longLen = 0;
        auto dry = p;
        dry.revMix = 0.0f;
        time (dry, dryLen);
        const double gatedMs = time (p, gatedLen);
        auto open = p;
        open.revGate = false;
        open.revDecayMs = 2500.0f;
        const double openMs = time (open, openLen);
        auto longest = open;
        longest.revDecayMs = 6000.0f;
        longest.revSize = 1.0f;
        const double longMs = time (longest, longLen);

        const int expectedDry = (int) std::ceil ((p.lengthMs * 0.001 + 0.03) * sr);
        if (dryLen != expectedDry)
            fail ("renderHit with Mix 0 is " + juce::String (dryLen) + " samples, expected " + juce::String (expectedDry));
        if (gatedLen > juce::jmax (expectedDry, ms (p.revGateMs + p.revFadeMs) + ms (5.0)))
            fail ("gated renderHit too long: " + juce::String (gatedLen));
        // (renderHit caps a reverb tail at 8 s, plus its 5 ms margin.)
        if (openLen <= ms (2500.0) || openLen > ms (1.5 * 2500.0 + 200.0) || longLen > ms (8005.0))
            fail ("ungated renderHit lengths " + juce::String (openLen) + " / " + juce::String (longLen));
        if (longMs > 150.0)
            fail ("renderHit of a 6 s tail took " + juce::String (longMs, 1) + " ms");
        std::printf ("renderHit @48k: Mix 0 %.0f ms (unchanged); gated %.0f ms in %.1f ms; ungated 2.5 s decay %.0f ms "
                     "in %.1f ms; ungated 6 s decay %.0f ms in %.1f ms\n",
                     1000.0 * dryLen / sr, 1000.0 * gatedLen / sr, gatedMs, 1000.0 * openLen / sr, openMs,
                     1000.0 * longLen / sr, longMs);

        DrumFoundryProcessor tp;
        tp.prepareToPlay (sr, 512);
        const double t0 = tp.getTailLengthSeconds();
        setParam (tp, pid::revMix, 0.5f);
        const double t1 = tp.getTailLengthSeconds();
        setParam (tp, pid::revGate, 0.0f);
        const double t2 = tp.getTailLengthSeconds();
        if (t1 < 0.195 || t2 < 1.5 * 2.5)
            fail ("getTailLengthSeconds: " + juce::String (t0) + " / " + juce::String (t1) + " / " + juce::String (t2));
        std::printf ("getTailLengthSeconds: Mix 0 %.3f s, gated %.3f s, ungated %.3f s\n", t0, t1, t2);
    }

    return failures;
}

//==============================================================================
// Reverb Size and Noise Width: turning each up does what its name says.

/** The Reverb's wet impulse response on its own: full band, no gate, no pre-delay. */
juce::AudioBuffer<float> reverbImpulse (float size, float decayMs, double sr, double seconds)
{
    DrumParams p;
    p.revEngine = revEngineClassic; // the Classic engine's Size design (checkReverbEngines has Cloud's)
    p.revMix = 1.0f;
    p.revSize = size;
    p.revDecayMs = decayMs;
    p.revPreDelayMs = 0.0f;
    p.revLowCut = 20.0f;
    p.revHighCut = 20000.0f;
    p.revGate = false;

    const int total = (int) (sr * seconds);
    juce::AudioBuffer<float> ir (2, total);
    ir.clear();
    ir.setSample (0, 0, 1.0f);
    ir.setSample (1, 0, 1.0f);
    Reverb r;
    r.prepare (sr);
    for (int pos = 0; pos < total; pos += 512)
        r.process (ir.getWritePointer (0, pos), ir.getWritePointer (1, pos), juce::jmin (512, total - pos), p, nullptr, 0, 0);
    ir.setSample (0, 0, ir.getSample (0, 0) - 1.0f); // take the dry impulse back out
    ir.setSample (1, 0, ir.getSample (1, 0) - 1.0f);
    return ir;
}

/** What makes a room sound big, measured on an impulse response.
    energyDb: the whole response (L + R). onsetMs: the first sample within 40 dB
    of the peak (the gap before the first reflection). buildMs: when the 10 ms
    energy envelope first gets within 3 dB of its maximum (how long the room
    takes to fill up). colourDb: the spread (standard deviation, dB) of the
    tail's magnitude spectrum (200-700 ms, 300 Hz - 6 kHz) around its 1/3-octave
    smoothed shape; about 5.6 dB for a dense, even tail (Rayleigh magnitudes),
    more when a few loop modes ring out (a small, boxy or metallic room). */
struct RoomCues { double energyDb = 0, onsetMs = 0, buildMs = 0, colourDb = 0; };

RoomCues roomCues (const juce::AudioBuffer<float>& ir, double sr)
{
    RoomCues c;
    const int n = ir.getNumSamples();
    const float* l = ir.getReadPointer (0);
    const float* r = ir.getReadPointer (1);
    const auto ms = [sr] (double v) { return (int) (v * sr * 0.001); };

    std::vector<double> e ((size_t) n);
    double total = 0.0, peak = 0.0;
    for (int i = 0; i < n; ++i)
    {
        e[(size_t) i] = (double) l[i] * l[i] + (double) r[i] * r[i];
        total += e[(size_t) i];
        peak = std::max (peak, e[(size_t) i]);
    }
    c.energyDb = 10.0 * std::log10 (0.5 * total + 1.0e-30);
    for (int i = 0; i < n; ++i)
        if (e[(size_t) i] > 1.0e-4 * peak) { c.onsetMs = 1000.0 * i / sr; break; }

    std::vector<double> env ((size_t) n);
    double acc = 0.0, envMax = 0.0;
    for (int i = 0; i < n; ++i)
    {
        acc += e[(size_t) i] - (i >= ms (10.0) ? e[(size_t) (i - ms (10.0))] : 0.0);
        env[(size_t) i] = acc;
        envMax = std::max (envMax, acc);
    }
    for (int i = 0; i < n; ++i)
        if (env[(size_t) i] >= 0.5 * envMax) { c.buildMs = 1000.0 * i / sr; break; }

    // Tail colouration, on the left channel.
    constexpr int order = 15;
    const int fftSize = 1 << order, start = ms (200.0), len = juce::jmin (ms (500.0), n - start);
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) fftSize * 2, 0.0f);
    for (int i = 0; i < len; ++i)
        buf[(size_t) i] = l[start + i] * (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (len - 1)));
    fft.performFrequencyOnlyForwardTransform (buf.data());
    std::vector<double> cum ((size_t) fftSize / 2 + 1, 0.0);
    for (int k = 0; k < fftSize / 2; ++k)
        cum[(size_t) k + 1] = cum[(size_t) k] + (double) buf[(size_t) k] * buf[(size_t) k];
    double s = 0.0, s2 = 0.0;
    int count = 0;
    const double binHz = sr / fftSize;
    for (int k = (int) (300.0 / binHz); k < (int) (6000.0 / binHz); ++k)
    {
        const int lo = (int) (k * 0.891), hi = (int) (k * 1.122) + 1; // 1/3 octave around bin k
        const double smooth = 10.0 * std::log10 ((cum[(size_t) hi] - cum[(size_t) lo]) / (hi - lo) + 1.0e-30);
        const double d = 20.0 * std::log10 (buf[(size_t) k] + 1.0e-15) - smooth;
        s += d;
        s2 += d * d;
        ++count;
    }
    c.colourDb = std::sqrt (juce::jmax (0.0, s2 / count - (s / count) * (s / count)));
    return c;
}

int checkSizeAndWidth()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: %s\n", msg.toRawUTF8()); ++failures; };
    constexpr double sr = 48000.0;

    // 1. Reverb Size: at a fixed Decay, a bigger Size starts later, takes longer
    //    to fill up and rings less (more modes, less colouration), without the
    //    reverb getting quieter (with the same RT60, short lines used to put
    //    ~10-15 dB more energy into the tail, so turning Size up sounded smaller).
    {
        const float sizes[] { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
        for (float decay : { 400.0f, 1200.0f, 2500.0f })
        {
            const auto ref = roomCues (reverbImpulse (0.6f, decay, sr, 1.5), sr);
            RoomCues prev;
            juce::String row;
            double lowest = 1.0e9, highest = -1.0e9;
            for (size_t k = 0; k < std::size (sizes); ++k)
            {
                const auto c = roomCues (reverbImpulse (sizes[k], decay, sr, 1.5), sr);
                const auto tag = "Size " + juce::String (sizes[k], 2) + ", Decay " + juce::String ((int) decay) + " ms";
                lowest = juce::jmin (lowest, c.energyDb - ref.energyDb);
                highest = juce::jmax (highest, c.energyDb - ref.energyDb);
                if (std::abs (c.energyDb - ref.energyDb) > 2.0)
                    fail (tag + ": energy " + juce::String (c.energyDb - ref.energyDb, 1) + " dB from Size 0.6's (limit 2)");
                if (k > 0 && (c.onsetMs <= prev.onsetMs || c.buildMs < prev.buildMs + 3.0))
                    fail (tag + ": first reflection at " + juce::String (c.onsetMs, 1) + " ms, fills up at " + juce::String (c.buildMs, 1)
                          + " ms; a smaller Size had " + juce::String (prev.onsetMs, 1) + " / " + juce::String (prev.buildMs, 1) + " ms");
                if (decay >= 1000.0f && k > 0 && c.colourDb > prev.colourDb + 0.5)
                    fail (tag + ": tail colouration " + juce::String (c.colourDb, 1) + " dB, more than the smaller Size's "
                          + juce::String (prev.colourDb, 1));
                row << juce::String (sizes[k], 2) << ": " << juce::String (c.onsetMs, 1) << " / " << juce::roundToInt (c.buildMs)
                    << " ms, " << juce::String (c.colourDb, 1) << " dB, " << (c.energyDb >= ref.energyDb ? "+" : "")
                    << juce::String (c.energyDb - ref.energyDb, 1) << " dB   ";
                prev = c;
            }
            if (highest - lowest > 3.0)
                fail ("Decay " + juce::String ((int) decay) + " ms: energy spread over Size " + juce::String (highest - lowest, 1) + " dB");
            std::printf ("reverb Size, Decay %4d ms (first reflection / fills up, tail colouration, energy vs Size 0.6): %s\n",
                         (int) decay, row.toRawUTF8());
        }
    }

    // 2. Noise Width: turning it up only adds side (L - R); the mid (L + R) stays
    //    within 0.5 dB of Width 0 for every noise style. Width 0 is mono (exactly,
    //    except for Metallic's rounding, which mixes its two banks half and half).
    {
        struct Case { const char* name; int style; float grit; };
        const Case cases[] { { "Wires", noiseWires, 0.0f }, { "Wires Grit 0.5", noiseWires, 0.5f }, { "White", noiseWhite, 0.0f },
                             { "White Grit 0.5", noiseWhite, 0.5f }, { "Pink", noisePink, 0.0f }, { "Metallic", noiseMetallic, 0.0f },
                             { "Metallic Grit 0.5", noiseMetallic, 0.5f }, { "Crackle", noiseCrackle, 0.0f } };
        const int n = (int) sr;
        std::vector<float> env ((size_t) n, 1.0f), body ((size_t) n, 0.0f), l ((size_t) n), r ((size_t) n);

        for (const auto& c : cases)
        {
            double mid0 = 0.0, prevSideToMid = -1.0e9, worstMid = 0.0, sideAtZero = 0.0;
            juce::String row;
            for (int step = 0; step <= 10; ++step)
            {
                const float width = 0.1f * (float) step;
                double mid = 0.0, side = 0.0;
                for (uint32_t seed = 1; seed <= 6; ++seed)
                {
                    NoiseGen g;
                    g.start (c.style, seed * 7919u, (float) sr);
                    NoiseGen::Settings s;
                    s.width = width;
                    s.grit = c.grit;
                    for (int pos = 0; pos < n; pos += 512)
                        g.render (juce::jmin (512, n - pos), s, env.data() + pos, nullptr, body.data() + pos, l.data() + pos, r.data() + pos);
                    for (int i = 0; i < n; ++i)
                    {
                        const double a = l[(size_t) i], b = r[(size_t) i];
                        mid += 0.25 * (a + b) * (a + b);
                        side += 0.25 * (a - b) * (a - b);
                    }
                }
                if (step == 0)
                {
                    mid0 = mid;
                    sideAtZero = 10.0 * std::log10 (side / mid + 1.0e-30);
                }
                const double midDb = 10.0 * std::log10 (mid / mid0);
                const double sideToMid = 10.0 * std::log10 (side / mid + 1.0e-30);
                worstMid = std::abs (midDb) > std::abs (worstMid) ? midDb : worstMid;
                if (std::abs (midDb) > 0.5)
                    fail (juce::String (c.name) + " at Width " + juce::String (width, 1) + ": mid level " + juce::String (midDb, 2)
                          + " dB from Width 0's");
                if (step > 0 && sideToMid <= prevSideToMid + 0.1)
                    fail (juce::String (c.name) + " at Width " + juce::String (width, 1) + ": side / mid " + juce::String (sideToMid, 1)
                          + " dB, not above Width " + juce::String (width - 0.1f, 1) + "'s " + juce::String (prevSideToMid, 1));
                if (step == 10 && std::abs (sideToMid) > 1.0)
                    fail (juce::String (c.name) + " at Width 1: side / mid " + juce::String (sideToMid, 1) + " dB (expected about 0)");
                if (step % 5 == 0 && step > 0)
                    row << "S/M " << juce::String (sideToMid, 1) << " dB at " << juce::String (width, 1) << "  ";
                prevSideToMid = sideToMid;
            }
            if (sideAtZero > -120.0)
                fail (juce::String (c.name) + ": Width 0 is not mono (side / mid " + juce::String (sideAtZero, 1) + " dB)");
            std::printf ("noise Width, %-17s: %sworst mid change %+.2f dB, side at Width 0 %s\n", c.name, row.toRawUTF8(),
                         worstMid, sideAtZero < -290.0 ? "none" : (juce::String (sideAtZero, 0) + " dB").toRawUTF8());
        }

        // Through the whole engine (Noise alone, reverb, drive and clipper off), over a few hits: same thing.
        DrumFoundryProcessor proc;
        EnvTables tables;
        proc.bakeTables (tables);
        auto p = proc.readParams();
        p.bodyOn = false;
        p.clickOn = false;
        p.noiseOn = true;
        p.revMix = 0.0f;
        p.clipMode = 0;
        p.driveMix = 0.0f; // the drive's tanh is on even at Amount 0
        double mid0 = 0.0, prevSide = -1.0;
        juce::String row;
        for (float width : { 0.0f, 0.5f, 1.0f })
        {
            p.noiseWidth = width;
            double mid = 0.0, side = 0.0;
            for (uint32_t seed = 1; seed <= 8; ++seed)
            {
                const auto hit = renderHit (p, tables, sr, 60, 1.0f, seed);
                for (int i = 0; i < hit.getNumSamples(); ++i)
                {
                    const double a = hit.getSample (0, i), b = hit.getSample (1, i);
                    mid += 0.25 * (a + b) * (a + b);
                    side += 0.25 * (a - b) * (a - b);
                }
            }
            if (juce::exactlyEqual (width, 0.0f))
                mid0 = mid;
            const double midDb = 10.0 * std::log10 (mid / mid0);
            if (std::abs (midDb) > 0.5 || side <= prevSide)
                fail ("engine noise at Width " + juce::String (width, 1) + ": mid " + juce::String (midDb, 2) + " dB from Width 0's, side "
                      + juce::String (side) + " (Width below: " + juce::String (prevSide) + ")");
            row << juce::String (width, 1) << ": mid " << juce::String (midDb, 2) << " dB, S/M "
                << juce::String (10.0 * std::log10 (side / mid + 1.0e-30), 1) << " dB   ";
            prevSide = side;
        }
        std::printf ("noise Width through the engine: %s\n", row.toRawUTF8());
    }

    return failures;
}

//==============================================================================
// Output filter and analyzer checks

/** Level (dB, relative to the input) of a sine at freqHz after the output filter,
    once it has settled. Measured by projecting onto the sine, so noise and
    distortion elsewhere in the spectrum don't count. */
double measureFilterDb (const DrumParams& p, double freqHz, double sr)
{
    OutputFilter filter;
    filter.prepare (sr);

    const int settle = (int) (sr * 0.4);
    const double cycles = std::max (20.0, std::round (freqHz * 0.5));
    const int len = (int) std::lround (cycles * sr / freqHz);
    const int total = settle + len;

    std::vector<float> l ((size_t) total), r ((size_t) total);
    const double w = juce::MathConstants<double>::twoPi * freqHz / sr;
    for (int i = 0; i < total; ++i)
        l[(size_t) i] = r[(size_t) i] = (float) (0.5 * std::sin (w * i));

    for (int pos = 0; pos < total; pos += 512)
        filter.process (l.data() + pos, r.data() + pos, juce::jmin (512, total - pos), p);

    double s = 0.0, c = 0.0;
    for (int i = settle; i < total; ++i)
    {
        s += l[(size_t) i] * std::sin (w * i);
        c += l[(size_t) i] * std::cos (w * i);
    }
    const double amp = 2.0 / len * std::sqrt (s * s + c * c);
    return 20.0 * std::log10 (juce::jmax (1.0e-15, amp / 0.5));
}

DrumParams filterParams (float hp, int hpSlope, float lp, int lpSlope)
{
    DrumParams p;
    p.outHP = hp;
    p.outHPSlope = hpSlope;
    p.outLP = lp;
    p.outLPSlope = lpSlope;
    return p;
}

/** A test signal for the master chain: noise bursts plus a low sine, like hits. */
juce::AudioBuffer<float> chainTestSignal (double sr, int total)
{
    juce::AudioBuffer<float> b (2, total);
    juce::Random rng (99);
    for (int i = 0; i < total; ++i)
    {
        const double t = (double) (i % (int) (sr * 0.25)) / sr;
        const float env = (float) std::exp (-t * 18.0);
        const float body = (float) std::sin (juce::MathConstants<double>::twoPi * 55.0 * i / sr);
        b.setSample (0, i, env * (0.6f * body + 0.3f * rng.nextFloat() - 0.15f));
        b.setSample (1, i, env * (0.6f * body + 0.3f * rng.nextFloat() - 0.15f));
    }
    return b;
}

/** Runs a signal through a fresh MasterChain in uneven blocks; params may change per block. */
juce::AudioBuffer<float> runChain (const juce::AudioBuffer<float>& in, double sr, bool filterEnabled,
                                   const std::function<DrumParams (int)>& paramsAt)
{
    MasterChain chain;
    chain.prepare (sr, 512);
    chain.setOutputFilterEnabled (filterEnabled);

    auto out = in;
    juce::AudioBuffer<float> block (2, 512);
    const int sizes[] { 512, 37, 256, 1, 480, 129 };
    int k = 0;
    for (int pos = 0; pos < out.getNumSamples();)
    {
        const int n = juce::jmin (sizes[k++ % 6], out.getNumSamples() - pos);
        block.setSize (2, n, false, false, true);
        for (int c = 0; c < 2; ++c)
            block.copyFrom (c, 0, out, c, pos, n);
        const int hit[] { 0 };
        chain.process (block, n, paramsAt (pos), hit, pos % (int) (sr * 0.25) < n ? 1 : 0);
        for (int c = 0; c < 2; ++c)
            out.copyFrom (c, pos, block, c, 0, n);
        pos += n;
    }
    return out;
}

int firstDifference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int from = 0)
{
    for (int i = from; i < a.getNumSamples(); ++i)
        for (int c = 0; c < 2; ++c)
            if (! juce::exactlyEqual (a.getSample (c, i), b.getSample (c, i)))
                return i;
    return -1;
}

/** Drive, EQ and reverb's on switches: off is the same as the effect at its neutral
    setting, and switching off settles there within a few blocks. */
int checkEffectSwitches()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: %s\n", msg.toRawUTF8()); ++failures; };

    constexpr double sr = 48000.0;
    const int total = (int) (sr * 1.0);
    const auto in = chainTestSignal (sr, total);

    // Each effect on its own (a reverb tail would carry what came before the switch).
    DrumParams base;
    base.driveAmount = 0.0f;
    base.revMix = 0.0f;

    struct Effect
    {
        const char* name;
        std::function<void (DrumParams&)> setUp, switchOff, neutral;
        float settledTolerance; // the DC blocker (and the EQ's filters) still settling from before
    };
    const Effect effects[] {
        { "drive",  [] (DrumParams& p) { p.driveAmount = 0.6f; },
                    [] (DrumParams& p) { p.driveOn = false; }, [] (DrumParams& p) { p.driveMix = 0.0f; }, 1.0e-4f },
        { "EQ",     [] (DrumParams& p) { p.eqBands[0].gainDb = 9.0f; p.eqBands[1].gainDb = -6.0f; p.eqBands[2].gainDb = 6.0f; },
                    [] (DrumParams& p) { p.eqOn = false; },
                    [] (DrumParams& p) { p.eqBands[0].gainDb = p.eqBands[1].gainDb = p.eqBands[2].gainDb = 0.0f; }, 1.0e-4f },
        { "reverb", [] (DrumParams& p) { p.revMix = 0.4f; p.revGate = false; },
                    [] (DrumParams& p) { p.revOn = false; }, [] (DrumParams& p) { p.revMix = 0.0f; }, 0.0f } };

    for (const auto& e : effects)
    {
        auto on = base;
        e.setUp (on);
        auto off = on, neutral = on;
        e.switchOff (off);
        e.neutral (neutral);

        const auto reference = runChain (in, sr, true, [&] (int) { return neutral; });
        const auto offOut = runChain (in, sr, true, [&] (int) { return off; });
        if (const int diff = firstDifference (reference, offOut); diff >= 0)
            fail (juce::String (e.name) + " off differs from " + e.name + " at neutral from sample " + juce::String (diff));

        // On, then switched off half way: it fades, then matches.
        const int switchAt = total / 2;
        const auto toggled = runChain (in, sr, true, [&] (int pos) { return pos < switchAt ? on : off; });
        if (firstDifference (reference, toggled) < 0)
            fail (juce::String (e.name) + " on: expected the output to change");

        const int settled = switchAt + (int) (sr * 0.2);
        float worst = 0.0f;
        for (int c = 0; c < 2; ++c)
            for (int i = settled; i < total; ++i)
                worst = juce::jmax (worst, std::abs (reference.getSample (c, i) - toggled.getSample (c, i)));
        if (worst > e.settledTolerance)
            fail (juce::String (e.name) + " switched off: still differs by " + juce::String (worst) + " after 200 ms");
        else
            std::printf ("%s switch: off = neutral, and switching off settles (max diff %.2e)\n", e.name, worst);
    }

    return failures;
}

int checkOutputFilter()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: %s\n", msg.toRawUTF8()); ++failures; };

    // --- -3 dB at the cutoff, for every slope ----------------------------------------------
    for (int slope = 0; slope < 3; ++slope)
    {
        const auto name = filterSlopeNames[slope];
        const double hpAt = measureFilterDb (filterParams (1000.0f, slope, 20000.0f, 1), 1000.0, 48000.0);
        const double lpAt = measureFilterDb (filterParams (20.0f, 1, 5000.0f, slope), 5000.0, 48000.0);
        if (std::abs (hpAt + 3.01) > 0.25 || std::abs (lpAt + 3.01) > 0.25)
            fail ("output filter " + name + ": " + juce::String (hpAt, 2) + " dB (HP) / " + juce::String (lpAt, 2)
                  + " dB (LP) at the cutoff, expected -3.01");
        else
            std::printf ("output filter %s: at cutoff HP %.2f dB, LP %.2f dB\n", name.toRawUTF8(), hpAt, lpAt);

        // Asymptotic slope, an octave pair well past the cutoff (96 kHz keeps the LP clear of Nyquist warping).
        const double sr = 96000.0;
        const double nominal = 6.0206 * OutputFilter::slopeOrder (slope);
        const auto hp = filterParams (1000.0f, slope, 20000.0f, 1);
        const auto lp = filterParams (20.0f, 1, 2000.0f, slope);
        const double hpSlope = measureFilterDb (hp, 500.0, sr) - measureFilterDb (hp, 250.0, sr);
        const double lpSlope = measureFilterDb (lp, 4000.0, sr) - measureFilterDb (lp, 8000.0, sr);
        if (std::abs (hpSlope - nominal) > 0.06 * nominal || std::abs (lpSlope - nominal) > 0.06 * nominal)
            fail ("output filter " + name + ": slope " + juce::String (hpSlope, 1) + " (HP) / " + juce::String (lpSlope, 1)
                  + " (LP) dB/oct, expected ~" + juce::String (nominal, 1));
        else
            std::printf ("output filter %s: slope HP %.1f, LP %.1f dB/oct (nominal %.1f)\n",
                         name.toRawUTF8(), hpSlope, lpSlope, nominal);
    }

    // --- The UI's response function against the real filter --------------------------------
    {
        struct Case { float hp; int hpSlope; float lp; int lpSlope; double sr; };
        const Case cases[] { { 80.0f, 1, 8000.0f, 2, 48000.0 }, { 300.0f, 0, 20000.0f, 1, 44100.0 },
                             { 20.0f, 1, 1500.0f, 1, 48000.0 }, { 1200.0f, 2, 1000.0f, 0, 96000.0 },
                             { 45.0f, 2, 12000.0f, 0, 44100.0 } };
        double worst = 0.0;
        int points = 0;
        for (const auto& c : cases)
            for (double f : { 25.0, 40.0, 60.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 16000.0 })
            {
                const double predicted = OutputFilter::responseDb ((float) f, c.hp, c.hpSlope, c.lp, c.lpSlope, c.sr);
                if (predicted < -70.0)
                    continue;
                const double measured = measureFilterDb (filterParams (c.hp, c.hpSlope, c.lp, c.lpSlope), f, c.sr);
                worst = juce::jmax (worst, std::abs (measured - predicted));
                ++points;
                if (std::abs (measured - predicted) > 0.5)
                    fail ("output filter response at " + juce::String (f) + " Hz (HP " + juce::String (c.hp) + ", LP "
                          + juce::String (c.lp) + "): measured " + juce::String (measured, 2) + " dB, responseDb says "
                          + juce::String (predicted, 2));
            }
        std::printf ("output filter: responseDb() vs measured at %d points, worst %.4f dB\n", points, worst);
    }

    // --- Off at the extremes: bit-identical to a chain without the filter --------------------
    {
        constexpr double sr = 48000.0;
        const int total = (int) (sr * 1.5);
        const auto in = chainTestSignal (sr, total);
        DrumParams base;
        base.clipMode = 1;
        base.revMix = 0.3f;
        base.driveAmount = 0.4f;

        const auto reference = runChain (in, sr, false, [&] (int) { return base; });
        for (int slope = 0; slope < 3; ++slope)
        {
            auto p = base;
            p.outHP = 20.0f;
            p.outLP = 20000.0f;
            p.outHPSlope = p.outLPSlope = slope;
            const auto out = runChain (in, sr, true, [&] (int) { return p; });
            if (const int diff = firstDifference (reference, out); diff >= 0)
                fail ("output filter off (" + filterSlopeNames[slope] + ") changes the output from sample " + juce::String (diff));
        }

        // On for a while, then off again: once the glide and crossfade are over the
        // output is bit-identical again (nothing after the filter has state that depends on it).
        auto on = base;
        on.outHP = 300.0f;
        on.outLP = 3000.0f;
        const int switchAt = (int) (sr * 0.5);
        const auto toggled = runChain (in, sr, true, [&] (int pos) { return pos < switchAt ? on : base; });
        const int settled = switchAt + (int) (sr * 0.1);
        const int early = firstDifference (reference, toggled);
        const int late = firstDifference (reference, toggled, settled);
        if (early < 0 || early > (int) (sr * 0.01))
            fail ("output filter on: expected the output to change right away");
        if (late >= 0)
            fail ("output filter switched off: still differs at sample " + juce::String (late));
        std::printf ("output filter off: bit-identical to no filter (all slopes, and 100 ms after switching off)\n");
    }

    // --- Jumps glide: no steps or clicks in a passband sine ------------------------------------
    {
        struct Jump { const char* name; DrumParams from, to; double hz; };
        const Jump jumps[] {
            { "LP off -> 1 kHz", filterParams (20.0f, 1, 20000.0f, 2), filterParams (20.0f, 1, 1000.0f, 2), 100.0 },
            { "LP 1 kHz -> off", filterParams (20.0f, 1, 1000.0f, 2), filterParams (20.0f, 1, 20000.0f, 2), 100.0 },
            { "HP off -> 2 kHz", filterParams (20.0f, 2, 20000.0f, 1), filterParams (2000.0f, 2, 20000.0f, 1), 6000.0 },
            { "HP 2 kHz -> off", filterParams (2000.0f, 2, 20000.0f, 1), filterParams (20.0f, 2, 20000.0f, 1), 6000.0 },
            { "HP slope 12 -> 48", filterParams (400.0f, 0, 20000.0f, 1), filterParams (400.0f, 2, 20000.0f, 1), 3000.0 },
            { "LP slope 48 -> 12", filterParams (20.0f, 1, 8000.0f, 2), filterParams (20.0f, 1, 8000.0f, 0), 1000.0 } };

        constexpr double sr = 48000.0;
        double worst = 0.0;
        for (const auto& j : jumps)
        {
            OutputFilter filter;
            filter.prepare (sr);
            const int total = (int) (sr * 0.6), jumpAt = (int) (sr * 0.3);
            std::vector<float> l ((size_t) total), r ((size_t) total);
            const double w = juce::MathConstants<double>::twoPi * j.hz / sr;
            for (int i = 0; i < total; ++i)
                l[(size_t) i] = r[(size_t) i] = (float) (0.5 * std::sin (w * i));
            for (int pos = 0; pos < total; pos += 64)
                filter.process (l.data() + pos, r.data() + pos, 64, pos < jumpAt ? j.from : j.to);

            // Largest sample-to-sample step around the jump, against the sine's own.
            float step = 0.0f;
            for (int i = jumpAt - 64; i < jumpAt + (int) (sr * 0.1); ++i)
                step = juce::jmax (step, std::abs (l[(size_t) i] - l[(size_t) i - 1]));
            const double ratio = step / (0.5 * 2.0 * std::sin (w * 0.5));
            worst = juce::jmax (worst, ratio);
            if (ratio > 1.2)
                fail (juce::String ("output filter jump ") + j.name + ": steps up to " + juce::String (ratio, 2)
                      + "x the sine's own");
        }
        std::printf ("output filter jumps (cutoff, on/off, slope): largest step %.3fx the sine's own\n", worst);
    }

    // --- Random fast automation at 48 dB (and everything else) -------------------------------
    for (double sr : { 44100.0, 96000.0 })
    {
        OutputFilter filter;
        filter.prepare (sr);
        juce::Random rng (7);
        const int total = (int) (sr * 8.0);
        std::vector<float> l (64), r (64);
        DrumParams p = filterParams (200.0f, 2, 5000.0f, 2);
        float peak = 0.0f;
        bool finite = true;
        double phase = 0.0;
        for (int pos = 0; pos < total;)
        {
            const int n = 1 + rng.nextInt (64);
            if (rng.nextInt (4) == 0)
            {
                // Jumps anywhere in the range, including both "off" ends, and slope switches.
                p.outHP = std::exp (std::log (20.0f) + rng.nextFloat() * std::log (100.0f));
                p.outLP = std::exp (std::log (1000.0f) + rng.nextFloat() * std::log (20.0f));
                if (rng.nextInt (8) == 0) p.outHP = 20.0f;
                if (rng.nextInt (8) == 0) p.outLP = 20000.0f;
                p.outHPSlope = rng.nextInt (8) == 0 ? rng.nextInt (3) : 2;
                p.outLPSlope = rng.nextInt (8) == 0 ? rng.nextInt (3) : 2;
            }
            for (int i = 0; i < n; ++i)
            {
                phase += 60.0 / sr;
                const float s = 0.4f * (float) std::sin (juce::MathConstants<double>::twoPi * phase);
                l[(size_t) i] = s + 0.5f * (rng.nextFloat() - 0.5f);
                r[(size_t) i] = s - 0.5f * (rng.nextFloat() - 0.5f);
            }
            filter.process (l.data(), r.data(), n, p);
            for (int i = 0; i < n; ++i)
            {
                finite = finite && std::isfinite (l[(size_t) i]) && std::isfinite (r[(size_t) i]);
                peak = juce::jmax (peak, std::abs (l[(size_t) i]), std::abs (r[(size_t) i]));
            }
            pos += n;
        }
        if (! finite || peak > 4.0f)
            fail ("output filter under random automation at " + juce::String (sr) + " Hz: "
                  + (finite ? "peak " + juce::String (peak) : juce::String ("NaN/inf")));
        else
            std::printf ("output filter random automation (%.0f Hz): finite, peak %.2f (input peak 0.65)\n", sr, peak);
    }

    return failures;
}

int checkAnalyzer()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: %s\n", msg.toRawUTF8()); ++failures; };

    // --- The tap delivers exactly what processBlock output ------------------------------------
    {
        DrumFoundryProcessor proc;
        proc.prepareToPlay (48000.0, 512);
        auto& tap = proc.getAnalyzerTap();
        tap.discard();

        juce::AudioBuffer<float> expected (2, 512 * 8);
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        proc.triggerAudition (1.0f);
        for (int b = 0; b < 8; ++b)
        {
            buf.clear();
            proc.processBlock (buf, midi);
            for (int c = 0; c < 2; ++c)
                expected.copyFrom (c, b * 512, buf, c, 0, 512);
        }

        std::vector<float> l (512 * 8), r (512 * 8);
        const int got = tap.pull (l.data(), r.data(), 512 * 5);
        const int got2 = tap.pull (l.data() + got, r.data() + got, 512 * 8);
        bool same = got + got2 == 512 * 8;
        for (int i = 0; same && i < 512 * 8; ++i)
            same = juce::exactlyEqual (l[(size_t) i], expected.getSample (0, i))
                && juce::exactlyEqual (r[(size_t) i], expected.getSample (1, i));
        if (! same || expected.getMagnitude (0, 512 * 8) < 0.01f)
            fail ("analyzer tap: pulled audio doesn't match what processBlock produced");

        // Mono pull is the channel average; a full FIFO drops new audio instead of blocking.
        tap.push (expected, 512 * 8);
        std::vector<float> mono (512 * 8);
        const int gotMono = tap.pull (mono.data(), 512 * 8);
        bool monoSame = gotMono == 512 * 8;
        for (int i = 0; monoSame && i < gotMono; ++i)
            monoSame = std::abs (mono[(size_t) i] - 0.5f * (expected.getSample (0, i) + expected.getSample (1, i))) < 1.0e-7f;
        juce::AudioBuffer<float> big (2, AnalyzerTap::capacity + 1000);
        big.clear();
        tap.push (big, big.getNumSamples());
        tap.push (big, 100);
        std::vector<float> sink ((size_t) AnalyzerTap::capacity + 2000);
        const int overflow = tap.pull (sink.data(), (int) sink.size());
        if (! monoSame || overflow >= AnalyzerTap::capacity)
            fail ("analyzer tap: mono pull or overflow handling wrong (" + juce::String (overflow) + ")");
        else
            std::printf ("analyzer tap: delivers processBlock output exactly (stereo and mono), drops on overflow\n");
    }

    // --- Calibration and peak readout -----------------------------------------------------------
    {
        DrumFoundryProcessor proc;
        proc.prepareToPlay (48000.0, 512);
        ui::SpectrumAnalyzer analyzer (proc);
        analyzer.setSize (520, 240);

        for (double hz : { 1000.0, 198.0, 49.0 })
        {
            // Full-scale sine in both channels, eight frames' worth.
            juce::AudioBuffer<float> sine (2, 1600);
            double phase = 0.0;
            for (int frame = 0; frame < 8; ++frame)
            {
                for (int i = 0; i < 1600; ++i)
                {
                    const float s = (float) std::sin (juce::MathConstants<double>::twoPi * phase);
                    phase += hz / 48000.0;
                    sine.setSample (0, i, s);
                    sine.setSample (1, i, s);
                }
                proc.getAnalyzerTap().push (sine, 1600);
                analyzer.pollNow();
            }
            const auto peak = analyzer.getPeak();
            const float cents = 1200.0f * std::log2 (peak.hz / (float) hz);
            if (! peak.valid || std::abs (cents) > 5.0f || std::abs (peak.db) > 0.5f)
                fail ("analyzer: full-scale " + juce::String (hz) + " Hz sine read as " + juce::String (peak.hz, 2)
                      + " Hz, " + juce::String (peak.db, 2) + " dB");
            else
                std::printf ("analyzer: full-scale %.0f Hz sine reads %.2f Hz (%s), %.2f dB\n",
                             hz, peak.hz, noteName (frequencyToMidi (peak.hz)).toRawUTF8(), peak.db);
        }

        // HIT: a decaying 55 Hz "kick" rendered offline.
        juce::AudioBuffer<float> kick (2, 48000);
        for (int i = 0; i < 48000; ++i)
        {
            const double t = i / 48000.0;
            const float s = (float) (0.9 * std::exp (-t * 6.0) * std::sin (juce::MathConstants<double>::twoPi * 55.0 * t));
            kick.setSample (0, i, s);
            kick.setSample (1, i, s);
        }
        analyzer.setHitAudio (kick, 48000.0);
        analyzer.setMode (ui::SpectrumAnalyzer::Mode::hit);
        const auto hitPeak = analyzer.getPeak();
        if (! analyzer.isShowingHit() || ! hitPeak.valid || std::abs (1200.0f * std::log2 (hitPeak.hz / 55.0f)) > 10.0f)
            fail ("analyzer: HIT peak of a 55 Hz kick read as " + juce::String (hitPeak.hz, 2) + " Hz");
        else
            std::printf ("analyzer: HIT peak of a decaying 55 Hz sine reads %.2f Hz (%s)\n",
                         hitPeak.hz, noteName (frequencyToMidi (hitPeak.hz)).toRawUTF8());

        // LIVE falls back to the hit after a second of silence, and comes back with signal.
        analyzer.setMode (ui::SpectrumAnalyzer::Mode::live);
        for (int i = 0; i < 40; ++i)
            analyzer.pollNow();
        const bool autoHit = analyzer.isShowingHit();
        juce::AudioBuffer<float> sine (2, 1600);
        for (int i = 0; i < 1600; ++i)
        {
            const float s = 0.5f * (float) std::sin (0.1 * i);
            sine.setSample (0, i, s);
            sine.setSample (1, i, s);
        }
        proc.getAnalyzerTap().push (sine, 1600);
        analyzer.pollNow();
        if (! autoHit || analyzer.isShowingHit())
            fail ("analyzer: LIVE should show the hit after 1 s of silence, and go back on signal");
        else
            std::printf ("analyzer: LIVE shows the last hit after 1 s of silence and returns on signal\n");
    }

    return failures;
}

/** Renders the spectrum analyzer on its own: LIVE while two hits play, LIVE with
    the mouse over a filter handle, and HIT. */
int cmdAnalyzer (const juce::File& outDir, int presetIndex, int width, int height)
{
    outDir.createDirectory();
    constexpr double sr = 48000.0;
    DrumFoundryProcessor proc;
    proc.prepareToPlay (sr, 400);
    if (presetIndex >= 0)
        proc.getPresets().load (presetIndex);
    setParam (proc, pid::outHP, 45.0f);
    setParam (proc, pid::outHPSlope, 2.0f);
    setParam (proc, pid::outLP, 11000.0f);
    setParam (proc, pid::outLPSlope, 1.0f);

    ui::FoundryLookAndFeel lnf;
    ui::SpectrumAnalyzer analyzer (proc);
    analyzer.setLookAndFeel (&lnf);
    analyzer.setSize (width, height);
    analyzer.setVisible (true);

    // The editor hands over each offline render; do the same.
    EnvTables tables;
    proc.bakeTables (tables);
    analyzer.setHitAudio (renderHit (proc.readParams(), tables, sr), sr);

    const auto save = [&] (const juce::String& name)
    {
        const auto img = analyzer.createComponentSnapshot (analyzer.getLocalBounds(), true, 2.0f);
        const auto file = outDir.getChildFile (name);
        file.deleteFile();
        juce::FileOutputStream stream (file);
        juce::PNGImageFormat().writeImageToStream (img, stream);
        const auto peak = analyzer.getPeak();
        std::printf ("wrote %s (%dx%d)  peak %.1f Hz %s %.1f dB\n", file.getFullPathName().toRawUTF8(), img.getWidth(),
                     img.getHeight(), peak.hz, noteName (frequencyToMidi (peak.hz)).toRawUTF8(), peak.db);
    };

    // LIVE: two hits through processBlock, analysed at 30 frames a second.
    juce::AudioBuffer<float> buf (2, 400);
    juce::MidiBuffer midi;
    for (int frame = 0; frame < 16; ++frame)
    {
        if (frame == 0 || frame == 12)
            proc.triggerAudition (frame == 0 ? 1.0f : 0.8f);
        for (int b = 0; b < 4; ++b)
        {
            buf.clear();
            proc.processBlock (buf, midi);
        }
        analyzer.pollNow();
    }
    save ("analyzer-live.png");

    // Hovering the LP handle shows its value.
    const float plotX = 32.0f, plotR = (float) width - 47.0f, plotY = 30.0f, plotB = (float) height - 16.0f;
    const juce::Point<float> lpHandle (plotX + (plotR - plotX) * std::log (11000.0f / 20.0f) / std::log (1000.0f),
                                       plotY + (plotB - plotY) * 9.0f / 96.0f);
    const auto now = juce::Time::getCurrentTime();
    analyzer.mouseMove (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), lpHandle, {}, 0.0f, 0.0f, 0.0f,
                                          0.0f, 0.0f, &analyzer, &analyzer, now, lpHandle, now, 0, false));
    save ("analyzer-hover.png");
    analyzer.mouseExit (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), {}, {}, 0.0f, 0.0f, 0.0f,
                                          0.0f, 0.0f, &analyzer, &analyzer, now, {}, now, 0, false));

    // HIT, settled.
    analyzer.setMode (ui::SpectrumAnalyzer::Mode::hit);
    for (int frame = 0; frame < 45; ++frame)
        analyzer.pollNow();
    save ("analyzer-hit.png");

    analyzer.setLookAndFeel (nullptr);
    return 0;
}

//==============================================================================
// Imported samples

/** A sine burst with a quick decay, so it has a clear start and end. */
bool writeTestAudio (juce::AudioFormat& format, const juce::File& file, double sr, int channels, double seconds, float hz)
{
    file.deleteFile();
    const int n = (int) (seconds * sr);
    juce::AudioBuffer<float> b (channels, n);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            b.setSample (c, i, (float) (0.8 * std::sin (juce::MathConstants<double>::twoPi * hz * t) * std::exp (-t * 3.0)));
        }

    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                      .withSampleRate (sr)
                                                      .withNumChannels (channels)
                                                      .withBitsPerSample (24));
    return writer != nullptr && writer->writeFromAudioSampleBuffer (b, 0, n);
}

SampleSet currentSampleSet (DrumFoundryProcessor& proc, std::array<SamplePtr, numSampleSlots>& holder)
{
    SampleSet set;
    for (int s = 0; s < numSampleSlots; ++s)
    {
        holder[(size_t) s] = proc.getSample ((SampleSlot) s);
        set.slots[(size_t) s] = holder[(size_t) s].get();
    }
    return set;
}

int checkSamples()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: samples: %s\n", msg.toRawUTF8()); ++failures; };

    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("DrumFoundrySampleCheck");
    dir.deleteRecursively();
    dir.createDirectory();

    juce::WavAudioFormat wavFormat;
    juce::AiffAudioFormat aiffFormat;
    juce::FlacAudioFormat flacFormat;
    const auto wav = dir.getChildFile ("Test Kick.wav");
    const auto aiff = dir.getChildFile ("Mono Snare.aiff");
    const auto flac = dir.getChildFile ("Hat.flac");
    const auto longFile = dir.getChildFile ("Long.wav");
    const auto quad = dir.getChildFile ("Quad.wav");
    const auto fake = dir.getChildFile ("Not Audio.wav");
    if (! writeTestAudio (wavFormat, wav, 44100.0, 2, 0.3, 60.0f)
        || ! writeTestAudio (aiffFormat, aiff, 96000.0, 1, 0.2, 200.0f)
        || ! writeTestAudio (flacFormat, flac, 48000.0, 2, 0.1, 5000.0f)
        || ! writeTestAudio (wavFormat, longFile, 22050.0, 1, 12.0, 100.0f)
        || ! writeTestAudio (wavFormat, quad, 48000.0, 4, 0.1, 440.0f))
    {
        fail ("couldn't write the test files");
        return failures;
    }
    fake.replaceWithText ("this is not a wav file");

    DrumFoundryProcessor proc;
    proc.prepareToPlay (48000.0, 512);
    auto& bank = proc.getSampleBank();
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    const auto process = [&]
    {
        buf.clear();
        proc.processBlock (buf, midi);
        return buf.getMagnitude (0, buf.getNumSamples());
    };
    const auto slotTag = [] (int s) { return sampleSlotName ((SampleSlot) s); };

    // Load a file into every slot: the engine gets it, and the layer switches to it.
    const juce::File files[] { wav, aiff, flac, wav, aiff, flac };
    const int gen0 = proc.getSampleGeneration();
    for (int s = 0; s < numSampleSlots; ++s)
        proc.loadSample ((SampleSlot) s, files[s]);
    if (! bank.waitForPendingLoads (10000))
        fail ("loads didn't finish");
    process();

    for (int s = 0; s < numSampleSlots; ++s)
    {
        if (proc.getEngineSamples().get (s) == nullptr)
            fail (slotTag (s) + ": the engine got no sample");
        if (proc.getEngineSamples().get (s) != proc.getSample ((SampleSlot) s).get())
            fail (slotTag (s) + ": the engine got a different sample");
        if (proc.getSamplePath ((SampleSlot) s) != files[s].getFullPathName())
            fail (slotTag (s) + ": wrong path " + proc.getSamplePath ((SampleSlot) s));
        if (proc.isSampleMissing ((SampleSlot) s) || proc.isSampleLoading ((SampleSlot) s))
            fail (slotTag (s) + ": marked missing / loading after a good load");
    }
    if (proc.getSampleGeneration() == gen0)
        fail ("generation didn't change");

    {
        const auto p = proc.readParams();
        if (! p.bodyOn || p.bodyStyle != sampleStyleIndex (bodyStyleNames))
            fail ("Body wasn't switched to Sample");
        if (! p.noiseOn || p.noiseStyle != sampleStyleIndex (noiseStyleNames))
            fail ("Noise wasn't switched to Sample");
        if (! p.clickOn || p.clickType != sampleStyleIndex (clickTypeNames))
            fail ("Click wasn't switched to Sample");
        for (int n = 0; n < kNumExtraLayers; ++n)
            if (! p.layers[(size_t) n].on || p.layers[(size_t) n].source != layerSourceNames.indexOf ("Sample"))
                fail ("Layer " + juce::String (n + 1) + " wasn't switched on with source Sample");
    }

    // Decoding: rate and channel count kept (up to 2), length capped.
    if (const auto s = proc.getSample (slotNoise); s == nullptr || s->audio.getNumChannels() != 1 || s->sampleRate != 96000.0)
        fail ("mono 96 kHz AIFF wasn't kept as it is");
    if (const auto s = proc.getSample (slotBody); s == nullptr || s->audio.getNumChannels() != 2
                                                  || s->audio.getNumSamples() != (int) (0.3 * 44100.0) || s->name != "Test Kick")
        fail ("stereo WAV decoded wrongly");
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        juce::String error;
        const auto longSample = SampleBank::decode (formats, longFile, error);
        if (longSample == nullptr || longSample->audio.getNumSamples() != (int) (SampleBank::maxSeconds * 22050.0))
            fail ("12 s file wasn't cut to " + juce::String (SampleBank::maxSeconds) + " s");
        const auto quadSample = SampleBank::decode (formats, quad, error);
        if (quadSample == nullptr || quadSample->audio.getNumChannels() != 2)
            fail ("4-channel file wasn't cut to 2 channels");
        if (SampleBank::decode (formats, fake, error) != nullptr || error.isEmpty())
            fail ("a text file decoded as audio");
    }

    // The display render includes the samples and stays finite.
    {
        std::array<SamplePtr, numSampleSlots> holder;
        EnvTables tables;
        proc.bakeTables (tables);
        const auto hit = renderHit (proc.readParams(), tables, 48000.0, 60, 1.0f, 0x5EEDu, currentSampleSet (proc, holder));
        if (hit.getNumSamples() == 0 || ! allFinite (hit))
            fail ("offline render with samples is empty or non-finite");
    }

    // A file that won't load leaves the slot alone.
    {
        const auto before = proc.getSample (slotBody).get();
        proc.loadSample (slotBody, fake);
        proc.loadSample (slotNoise, dir.getChildFile ("Nowhere.wav"));
        bank.waitForPendingLoads (5000);
        if (proc.getSample (slotBody).get() != before || proc.getSamplePath (slotBody) != wav.getFullPathName())
            fail ("a bad file replaced the Body sample");
        if (proc.getSample (slotNoise) == nullptr || proc.isSampleMissing (slotNoise))
            fail ("a missing file replaced the Noise sample");
    }

    // State round trip keeps every path and reloads the audio.
    juce::MemoryBlock state;
    proc.getStateInformation (state);
    {
        DrumFoundryProcessor other;
        other.setStateInformation (state.getData(), (int) state.getSize());
        for (int s = 0; s < numSampleSlots; ++s)
        {
            if (other.getSamplePath ((SampleSlot) s) != proc.getSamplePath ((SampleSlot) s))
                fail (slotTag (s) + ": path lost in state round trip");
            if (other.getSample ((SampleSlot) s) == nullptr)
                fail (slotTag (s) + ": sample not reloaded from state");
        }
    }

    // Missing files: path kept, marked missing, no audio, nothing breaks.
    {
        auto tree = proc.createStateTree();
        const auto gone = dir.getChildFile ("Gone Snare.wav").getFullPathName();
        auto samplesTree = tree.getChildWithName (DrumFoundryProcessor::samplesId);
        for (auto child : samplesTree)
            if (child.getProperty ("slot").toString() == sampleSlotKey (slotLayer2))
                child.setProperty ("path", gone, nullptr);
        // Plus junk that must be ignored rather than crash.
        juce::ValueTree junk ("SAMPLE");
        junk.setProperty ("slot", "nonsense", nullptr);
        junk.setProperty ("path", "relative/path.wav", nullptr);
        samplesTree.appendChild (junk, nullptr);

        DrumFoundryProcessor other;
        other.prepareToPlay (48000.0, 512);
        other.loadStateTree (tree);
        juce::AudioBuffer<float> b (2, 512);
        other.triggerAudition (1.0f);
        other.processBlock (b, midi);

        if (! other.isSampleMissing (slotLayer2) || other.getSample (slotLayer2) != nullptr)
            fail ("missing file not marked missing");
        if (other.getSamplePath (slotLayer2) != gone)
            fail ("missing file's path not kept");
        if (other.getEngineSamples().get (slotLayer2) != nullptr || other.getEngineSamples().get (slotBody) == nullptr)
            fail ("engine set wrong with a missing file");
        if (! allFinite (b))
            fail ("non-finite output with a missing file");

        // ... and it's still in the state, so saving doesn't forget it.
        juce::MemoryBlock again;
        other.getStateInformation (again);
        DrumFoundryProcessor third;
        third.setStateInformation (again.getData(), (int) again.getSize());
        if (third.getSamplePath (slotLayer2) != gone || ! third.isSampleMissing (slotLayer2))
            fail ("missing path lost in a second round trip");

        // A file that disappears between save and load is missing too.
        const auto moved = dir.getChildFile ("Moved.flac");
        flac.copyFileTo (moved);
        other.loadSample (slotClick, moved);
        other.getSampleBank().waitForPendingLoads (5000);
        const auto withMoved = other.createStateTree();
        moved.deleteFile();
        other.loadStateTree (withMoved);
        if (! other.isSampleMissing (slotClick) || other.getSamplePath (slotClick) != moved.getFullPathName())
            fail ("deleted file not reported missing after reload");
    }

    // Clearing: the slot empties, the engine lets go, and the audio is freed off the audio thread.
    {
        std::weak_ptr<const SampleData> weak = proc.getSample (slotClick);
        const int gen = proc.getSampleGeneration();
        proc.clearSample (slotClick);
        if (proc.getSample (slotClick) != nullptr || proc.getSamplePath (slotClick).isNotEmpty() || proc.isSampleMissing (slotClick))
            fail ("clear left something behind");
        if (proc.getSampleGeneration() == gen)
            fail ("clear didn't bump the generation");
        if (weak.expired())
            fail ("cleared sample freed while the audio thread could still hold it");
        process();
        if (proc.getEngineSamples().get (slotClick) != nullptr)
            fail ("engine still has the cleared sample");
        bank.releaseRetired();
        if (! weak.expired())
            fail ("cleared sample never freed");
    }

    // A factory preset clears every slot.
    proc.getPresets().load (0);
    process();
    for (int s = 0; s < numSampleSlots; ++s)
        if (proc.getSample ((SampleSlot) s) != nullptr || proc.getSamplePath ((SampleSlot) s).isNotEmpty()
            || proc.getEngineSamples().get (s) != nullptr)
            fail (slotTag (s) + ": factory preset didn't clear it");

    // Preview: plays the raw file into the output, resampled (0.3 s at 44.1 kHz ->
    // 0.3 s at 48 kHz), then stops by itself; stopPreview() cuts it.
    {
        for (int i = 0; i < 40; ++i)
            process(); // let the preset's state settle
        proc.previewFile (wav);
        bank.waitForPendingLoads (5000);
        if (! proc.isPreviewing())
            fail ("isPreviewing() false after previewFile()");

        int lastLoud = -1;
        float peak = 0.0f;
        for (int block = 0; block < 60; ++block)
        {
            process();
            for (int i = 0; i < buf.getNumSamples(); ++i)
                if (std::abs (buf.getSample (0, i)) > 1.0e-6f)
                    lastLoud = block * buf.getNumSamples() + i;
            peak = juce::jmax (peak, buf.getMagnitude (0, buf.getNumSamples()));
        }
        const int expected = (int) (0.3 * 48000.0);
        if (peak < 0.1f || peak > 0.6f)
            fail ("preview peak " + juce::String (peak) + " (expected about " + juce::String (0.8f * SampleBank::previewGain) + ")");
        if (std::abs (lastLoud - expected) > 64)
            fail ("preview length " + juce::String (lastLoud) + " samples, expected about " + juce::String (expected));
        if (proc.isPreviewing())
            fail ("isPreviewing() still true after the file ended");

        proc.previewFile (longFile);
        bank.waitForPendingLoads (5000);
        const float playing = process();
        proc.stopPreview();
        const float stopped = process();
        if (playing < 0.05f || stopped > 1.0e-5f || proc.isPreviewing())
            fail ("stopPreview() didn't stop it (" + juce::String (playing) + " -> " + juce::String (stopped) + ")");
    }

    // Handover under load: an audio thread renders non-stop while samples are
    // loaded, cleared, restored and previewed. Every retired sample must be freed
    // in the end, and never while the audio thread could still read it.
    {
        DrumFoundryProcessor p2;
        p2.prepareToPlay (48000.0, 256);
        std::atomic<bool> stop { false };
        std::atomic<int> blocks { 0 };
        std::atomic<bool> finite { true };
        std::thread audio ([&]
        {
            juce::AudioBuffer<float> b (2, 256);
            juce::MidiBuffer m;
            for (int i = 0; ! stop.load(); ++i)
            {
                if (i % 20 == 0)
                    p2.triggerAudition (1.0f);
                b.clear();
                p2.processBlock (b, m);
                if (! allFinite (b))
                    finite = false;
                ++blocks;
            }
        });

        std::vector<std::weak_ptr<const SampleData>> seen;
        for (int i = 0; i < 60; ++i)
        {
            p2.loadSample ((SampleSlot) (i % numSampleSlots), files[i % 3]);
            if (i % 7 == 0)
                p2.clearSample (slotBody);
            if (i % 11 == 0)
                p2.loadStateTree (p2.createStateTree());
            if (i % 5 == 0)
                p2.previewFile (wav);
            p2.getSampleBank().waitForPendingLoads (5000);
            for (int s = 0; s < numSampleSlots; ++s)
                seen.push_back (p2.getSample ((SampleSlot) s));
        }
        juce::Thread::sleep (20);
        stop = true;
        audio.join();

        // The audio thread has stopped, so one more block (here) lets it adopt the
        // final table; after that only the current samples may be alive.
        juce::AudioBuffer<float> b (2, 256);
        juce::MidiBuffer m;
        p2.stopPreview();
        p2.processBlock (b, m);
        p2.getSampleBank().releaseRetired();

        std::set<const SampleData*> inUse, alive;
        for (int s = 0; s < numSampleSlots; ++s)
            if (auto sp = p2.getSample ((SampleSlot) s))
                inUse.insert (sp.get());
        for (const auto& w : seen)
            if (auto sp = w.lock())
                alive.insert (sp.get());

        if (blocks.load() < 100 || ! finite.load())
            fail ("handover stress: " + juce::String (blocks.load()) + " blocks, finite " + juce::String ((int) finite.load()));
        if (alive != inUse)
            fail ("handover stress: " + juce::String ((int) alive.size()) + " samples alive, "
                  + juce::String ((int) inUse.size()) + " in use (retired ones not freed)");
        std::printf ("samples: handover stress, %d blocks rendered alongside 60 loads / clears / restores / previews\n",
                     blocks.load());
    }

    std::printf ("samples: load into all %d slots, state round trip, missing files, clear, factory reset, preview: %s\n",
                 (int) numSampleSlots, failures == 0 ? "ok" : "FAILED");
    dir.deleteRecursively();
    return failures;
}

/** Screenshots of the sample browser and slot widgets, hosted on their own. */
int cmdSamplesUi (const juce::File& outDir)
{
    outDir.createDirectory();

    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("DrumFoundrySampleShots");
    dir.createDirectory();
    const auto wav = dir.getChildFile ("Deep Kick 01.wav");
    juce::WavAudioFormat wavFormat;
    writeTestAudio (wavFormat, wav, 44100.0, 2, 0.6, 55.0f);

    DrumFoundryProcessor proc;
    std::array<juce::String, numSampleSlots> paths;
    paths[slotBody] = wav.getFullPathName();
    paths[slotLayer1] = "/Users/nobody/Samples/Snare Crack 07.wav";
    proc.getSampleBank().restore (paths);

    struct Host : juce::Component
    {
        void paint (juce::Graphics& g) override { g.fillAll (ui::colours::background); }
    };

    ui::FoundryLookAndFeel lnf;
    Host host;
    host.setLookAndFeel (&lnf);
    host.setSize (300 + 16 * 3 + 440, 700 + 32);

    ui::SampleBrowser browser (proc);
    browser.setBounds (16, 16, 300, 700);
    host.addAndMakeVisible (browser);

    std::vector<std::unique_ptr<ui::SampleSlotWidget>> widgets;
    const std::pair<SampleSlot, juce::Colour> slots[] { { slotBody, ui::colours::body }, { slotNoise, ui::colours::noise },
                                                        { slotLayer1, ui::colours::click } };
    int y = 16;
    for (const auto& [slot, colour] : slots)
    {
        auto w = std::make_unique<ui::SampleSlotWidget> (proc, slot, colour);
        w->setBounds (300 + 32, y, 440, 34);
        host.addAndMakeVisible (*w);
        widgets.push_back (std::move (w));
        y += 34 + 10;
    }
    host.setVisible (true);

    auto* list = [&]() -> juce::ListBox*
    {
        for (auto* c : browser.getChildren())
            if (auto* lb = dynamic_cast<juce::ListBox*> (c))
                return lb;
        return nullptr;
    }();

    const auto shoot = [&] (const juce::String& name)
    {
        browser.waitForScan (20000);
        host.repaint();
        const auto img = host.createComponentSnapshot (host.getLocalBounds(), true, 2.0f);
        const auto file = outDir.getChildFile (name);
        file.deleteFile();
        juce::FileOutputStream stream (file);
        juce::PNGImageFormat().writeImageToStream (img, stream);
        std::printf ("wrote %s\n", file.getFullPathName().toRawUTF8());
    };

    // 1: the top of the User Library.
    const auto music = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    auto library = music.getChildFile ("Ableton/User Library/Samples");
    if (! library.isDirectory())
        library = music;
    browser.showFolder (library);
    shoot ("browser_place.png");

    // 2: the first folder in it with samples, with a row selected.
    for (const auto& entry : juce::RangedDirectoryIterator (library, true, "*", juce::File::findFiles | juce::File::ignoreHiddenFiles))
        if (SampleBank::isAudioFile (entry.getFile()) && entry.getFile().getParentDirectory() != library)
        {
            browser.showFolder (entry.getFile().getParentDirectory());
            break;
        }
    browser.waitForScan (20000);
    if (list != nullptr)
        list->selectRow (juce::jmin (2, list->getListBoxModel()->getNumRows() - 1));
    browser.setTargetSlot (slotLayer1);
    proc.stopPreview();
    shoot ("browser_folder.png");

    // 3: a search from the quick filters.
    browser.showFolder (library);
    browser.setSearchText ("Kick");
    shoot ("browser_search.png");

    host.removeAllChildren();
    host.setLookAndFeel (nullptr);
    dir.deleteRecursively();
    return 0;
}

//==============================================================================
// Sound engine: body / noise / click styles, samples, extra layers

/** A synthetic sine sample. With `invertRight` the right channel is the left
    one upside down, so a test can tell the channels apart. */
std::shared_ptr<SampleData> makeSineSample (double hz, double rate, double seconds, bool stereo, bool invertRight = false)
{
    auto s = std::make_shared<SampleData>();
    const int n = (int) (seconds * rate);
    s->audio.setSize (stereo ? 2 : 1, n);
    s->sampleRate = rate;
    s->name = "sine " + juce::String (hz);
    for (int i = 0; i < n; ++i)
    {
        const float v = 0.8f * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / rate);
        s->audio.setSample (0, i, v);
        if (stereo)
            s->audio.setSample (1, i, invertRight ? -v : v);
    }
    return s;
}

/** Default envelopes, or flat ones (no pitch sweep, full level throughout). */
EnvTables engineTables (bool flat)
{
    EnvTables t;
    if (flat)
    {
        // Pitch curves at 0 st, level curves at full (the extra layers' own ones too).
        for (int e = 0; e < numEnvs; ++e)
            t.data[(size_t) e].fill (isPitchEnvelope (e) ? 0.0f : 1.0f);
    }
    else
    {
        for (int e = 0; e < numEnvs; ++e)
            t.bake (e, defaultEnvelope ((EnvId) e));
    }
    return t;
}

/** The default pitch curve's shape before the pitch curve went to semitones:
    0..1, scaled by what used to be Pitch Depth (14 st by default). */
const std::vector<EnvPoint> kUnitPitchCurve { { 0.0f, 1.0f, -0.7f }, { 0.035f, 0.12f, -0.5f }, { 1.0f, 0.0f, 0.0f } };

/** `base` with its pitch curve replaced by `unitCurve` (0..1) x depthSt semitones. */
EnvTables withPitch (EnvTables base, const std::vector<EnvPoint>& unitCurve, float depthSt)
{
    auto pts = unitCurve;
    for (auto& pt : pts)
        pt.y *= depthSt;
    base.bake (envPitch, CurveEnvelope (pts, envelopeRange (envPitch)));
    return base;
}

/** Plain parameters for measuring: no drive, EQ or clipper, only the chosen layers. */
DrumParams engineParams (bool body, bool noise, bool click)
{
    DrumParams p;
    p.bodyOn = body;
    p.noiseOn = noise;
    p.clickOn = click;
    p.driveAmount = 0.0f;
    p.clipMode = 0;
    return p;
}

/** Average frequency from rising zero crossings in [start, end), optionally after
    a 24 dB/oct low-pass at lpHz (to ignore overtones). 0 if fewer than 2 crossings. */
double zeroCrossingHz (const juce::AudioBuffer<float>& b, int ch, double sr, int start, int end, float lpHz = 0.0f,
                       int maxPeriods = 1000)
{
    dsp::Svf lp1, lp2;
    if (lpHz > 0.0f)
    {
        lp1.setCoefficients (lpHz, 0.54f, (float) sr);
        lp2.setCoefficients (lpHz, 1.31f, (float) sr);
    }
    end = juce::jmin (end, b.getNumSamples());
    double first = -1.0, last = -1.0;
    int count = 0;
    float prev = 0.0f;
    for (int i = 0; i < end; ++i)
    {
        float v = b.getSample (ch, i);
        if (lpHz > 0.0f)
            v = lp2.lowpass (lp1.lowpass (v));
        if (i > start && prev < 0.0f && v >= 0.0f)
        {
            const double t = (double) (i - 1) + (double) (prev / (prev - v));
            if (first < 0.0)
                first = t;
            last = t;
            if (++count > maxPeriods)
                break;
        }
        prev = v;
    }
    return count >= 2 ? (count - 1) * sr / (last - first) : 0.0;
}

float peakOf (const juce::AudioBuffer<float>& b)
{
    return b.getNumSamples() > 0 ? juce::jmax (b.getMagnitude (0, 0, b.getNumSamples()), b.getMagnitude (1, 0, b.getNumSamples())) : 0.0f;
}

/** Renders with a fresh engine in the given block sizes (cycled), one hit at `hitAt`. */
juce::AudioBuffer<float> renderBlocks (const DrumParams& p, const EnvTables& t, const SampleSet& samples, double sr,
                                       int total, int hitAt, std::initializer_list<int> sizes)
{
    DrumEngine engine;
    engine.prepare (sr, 4096);
    engine.setParams (p);
    engine.setTables (t);
    engine.setSamples (samples);
    engine.setSeed (0x5EEDu);

    juce::AudioBuffer<float> out (2, total);
    auto it = sizes.begin();
    for (int pos = 0; pos < total;)
    {
        const int n = juce::jmin (*it, total - pos);
        if (++it == sizes.end())
            it = sizes.begin();
        juce::AudioBuffer<float> block (2, n);
        juce::MidiBuffer midi;
        if (hitAt >= pos && hitAt < pos + n)
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), hitAt - pos);
        engine.process (block, midi);
        out.copyFrom (0, pos, block, 0, 0, n);
        out.copyFrom (1, pos, block, 1, 0, n);
        pos += n;
    }
    return out;
}

int checkEngineStyles()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: %s\n", msg.toRawUTF8()); ++failures; };

    constexpr double sr = 48000.0;
    const auto defTables = engineTables (false);
    const auto flatTables = engineTables (true);

    // A sample in every slot: 220 Hz at 44.1 kHz, stereo with the right side inverted.
    const auto sine220 = makeSineSample (220.0, 44100.0, 1.0, true, true);
    const auto mono220 = makeSineSample (220.0, 44100.0, 1.0, false);
    SampleSet all;
    for (auto& s : all.slots)
        s = sine220.get();

    // --- every body style x noise style x click type -----------------------------
    {
        int combos = 0;
        for (int b = 0; b < numBodyStyles; ++b)
            for (int n = 0; n < numNoiseStyles; ++n)
                for (int c = 0; c < numClickTypes; ++c)
                {
                    DrumParams p;
                    p.bodyStyle = b;
                    p.noiseStyle = n;
                    p.clickType = c;
                    for (int k = 0; k < kNumExtraLayers; ++k)
                    {
                        p.layers[(size_t) k].on = (b + n + c + k) % 2 == 0;
                        p.layers[(size_t) k].source = k;
                    }
                    const auto hit = renderHit (p, defTables, sr, 60, 1.0f, 0x5EEDu, all);
                    const float peak = peakOf (hit);
                    const auto tag = bodyStyleNames[b] + " / " + noiseStyleNames[n] + " / " + clickTypeNames[c];
                    if (! allFinite (hit))  fail (tag + ": non-finite samples");
                    if (peak < 0.01f)       fail (tag + ": nearly silent (" + juce::String (peak) + ")");
                    if (peak > 1.001f)      fail (tag + ": peak " + juce::String (peak) + " over the clipper ceiling");
                    ++combos;
                }
        std::printf ("style combos: %d body x noise x click renders finite and audible\n", combos);
    }

    // --- each layer on its own, over tunings, notes, depths and rates -------------
    {
        juce::String levels;
        for (int b = 0; b < numBodyStyles; ++b)
        {
            float lo = 1.0e9f, hi = 0.0f;
            for (double rate : { 44100.0, 96000.0 })
                for (float tune : { 30.0f, 50.0f, 200.0f, 600.0f })
                    for (float depth : { 0.0f, 48.0f })
                        for (int note : { 24, 60, 96 })
                            for (float shape : { 0.0f, 1.0f })
                            {
                                auto p = engineParams (true, false, false);
                                p.bodyStyle = b;
                                p.tuneHz = tune;
                                p.bodyShape = shape;
                                p.modes = shape;
                                p.otGain = 1.0f;
                                p.otRatio = shape > 0.5f ? 4.0f : 1.0f;
                                p.lengthMs = 2000.0f;
                                const auto tables = withPitch (defTables, kUnitPitchCurve, depth);
                                const auto hit = renderHit (p, tables, rate, note, 1.0f, 0x5EEDu, all);
                                const float peak = peakOf (hit);
                                const auto tag = "body " + bodyStyleNames[b] + " " + juce::String (tune) + " Hz depth "
                                                 + juce::String (depth) + " note " + juce::String (note) + " @" + juce::String (rate);
                                if (! allFinite (hit)) fail (tag + ": non-finite samples");
                                if (peak < 0.05f)      fail (tag + ": nearly silent (" + juce::String (peak) + ")");
                                if (peak > 8.0f)       fail (tag + ": peak " + juce::String (peak) + " (clipper off)");
                                lo = juce::jmin (lo, peak);
                                hi = juce::jmax (hi, peak);
                            }
            levels << bodyStyleNames[b] << " " << juce::String (juce::Decibels::gainToDecibels (lo), 1) << ".."
                   << juce::String (juce::Decibels::gainToDecibels (hi), 1) << " dB  ";
        }
        std::printf ("body styles alone, peak range over tune/depth/note/shape/rate: %s\n", levels.toRawUTF8());

        juce::String noiseLevels;
        for (int n = 0; n < numNoiseStyles; ++n)
        {
            float lo = 1.0e9f, hi = 0.0f;
            for (float grit : { 0.0f, 1.0f })
                for (float width : { 0.0f, 1.0f })
                    for (float color : { 0.0f, 1.0f })
                    {
                        auto p = engineParams (false, true, false);
                        p.noiseStyle = n;
                        p.noiseGrit = grit;
                        p.noiseWidth = width;
                        p.noiseColor = color;
                        const auto hit = renderHit (p, defTables, sr, 60, 1.0f, 0x5EEDu, all);
                        const float peak = peakOf (hit);
                        const auto tag = "noise " + noiseStyleNames[n] + " grit " + juce::String (grit) + " width " + juce::String (width);
                        if (! allFinite (hit)) fail (tag + ": non-finite samples");
                        if (peak < 0.01f)      fail (tag + ": nearly silent (" + juce::String (peak) + ")");
                        lo = juce::jmin (lo, rmsIn (hit, 0, (int) (0.1 * sr)));
                        hi = juce::jmax (hi, rmsIn (hit, 0, (int) (0.1 * sr)));
                    }
            auto p = engineParams (false, true, false);
            p.noiseStyle = n;
            p.noiseGrit = n == noiseWires ? 0.0f : 0.5f;
            const auto hit = renderHit (p, defTables, sr, 60, 1.0f, 0x5EEDu, all);
            const float atDefaults = rmsIn (hit, 0, (int) (0.1 * sr));
            noiseLevels << noiseStyleNames[n] << " " << juce::String (juce::Decibels::gainToDecibels (atDefaults), 1) << " ("
                        << juce::String (juce::Decibels::gainToDecibels (lo), 1) << ".."
                        << juce::String (juce::Decibels::gainToDecibels (hi), 1) << ")  ";
        }
        std::printf ("noise styles alone, rms dB over the first 100 ms at default settings, Grit 50%% (and at grit/width/color extremes): %s\n", noiseLevels.toRawUTF8());

        for (int c = 0; c < numClickTypes; ++c)
            for (float tone : { 300.0f, 12000.0f })
                for (float len : { 0.5f, 40.0f })
                {
                    auto p = engineParams (false, false, true);
                    p.clickType = c;
                    p.clickGain = 1.0f;
                    p.clickTone = tone;
                    p.clickLengthMs = len;
                    const auto hit = renderHit (p, defTables, sr, 60, 1.0f, 0x5EEDu, all);
                    const float peak = peakOf (hit);
                    const auto tag = "click " + clickTypeNames[c] + " tone " + juce::String (tone) + " length " + juce::String (len);
                    if (! allFinite (hit)) fail (tag + ": non-finite samples");
                    // (A 12 kHz low cut on the 220 Hz test sine rightly leaves almost nothing.)
                    if (peak < 0.005f && ! (c == clickSample && tone > 1000.0f))
                        fail (tag + ": nearly silent (" + juce::String (peak) + ")");
                    if (peak > 4.0f)       fail (tag + ": peak " + juce::String (peak));
                }
    }

    // --- on switches ---------------------------------------------------------------
    {
        auto p = engineParams (false, false, false);
        const float peak = peakOf (renderHit (p, defTables, sr, 60, 1.0f, 0x5EEDu, all));
        if (peak != 0.0f)
            fail ("body, noise and click all off: not silent (" + juce::String (peak) + ")");
    }

    // --- samples: pitch, transpose, stereo, null ----------------------------------
    {
        struct PitchCase { const char* name; int slot; float tuneSt; int note; };
        const PitchCase cases[] { { "body", slotBody, 0.0f, 60 }, { "body", slotBody, 12.0f, 60 },
                                  { "body", slotBody, -5.0f, 60 }, { "body", slotBody, 0.0f, 72 },
                                  { "noise", slotNoise, 0.0f, 60 }, { "noise", slotNoise, 7.0f, 60 },
                                  { "click", slotClick, 0.0f, 60 }, { "click", slotClick, 12.0f, 60 },
                                  { "layer 2", slotLayer2, 3.0f, 60 }, { "layer 2", slotLayer2, -12.0f, 60 } };
        juce::String report;
        for (const auto& c : cases)
        {
            auto p = engineParams (c.slot == slotBody, c.slot == slotNoise, c.slot == slotClick);
            p.bodyStyle = bodySample;
            p.bodySmpTune = c.tuneSt;
            p.noiseStyle = noiseSample;
            p.noiseSmpTune = c.tuneSt;
            p.noiseHP = 20.0f;
            p.noiseLP = 20000.0f;
            p.noiseColor = 0.0f;
            p.noiseCouple = 0.0f;
            p.noiseGain = 1.0f;
            p.clickType = clickSample;
            p.clickSmpTune = c.tuneSt;
            p.clickTone = 300.0f;
            p.clickLengthMs = 40.0f;
            p.clickGain = 1.0f;
            if (c.slot == slotLayer2)
            {
                auto& l = p.layers[1];
                l.on = true;
                l.source = 0;
                l.tuneSt = c.tuneSt;
                l.env = layerEnvFlat;
            }

            SampleSet set;
            set.slots[(size_t) c.slot] = mono220.get();
            const auto hit = renderHit (p, flatTables, sr, c.note, 1.0f, 0x5EEDu, set);
            const int start = (int) (0.005 * sr), end = (int) ((c.slot == slotClick ? 0.035 : 0.3) * sr);
            const double hz = zeroCrossingHz (hit, 0, sr, start, end);
            const double expected = 220.0 * std::exp2 ((c.tuneSt + (float) (c.note - 60)) / 12.0);
            report << c.name << " " << juce::String (c.tuneSt, 0) << "st n" << c.note << " " << juce::String (hz, 1) << " Hz  ";
            if (std::abs (hz / expected - 1.0) > 0.003)
                fail (juce::String (c.name) + " sample at " + juce::String (c.tuneSt) + " st, note " + juce::String (c.note)
                      + ": " + juce::String (hz, 2) + " Hz, expected " + juce::String (expected, 2));

            // The same with no sample loaded: silence.
            const auto none = renderHit (p, flatTables, sr, c.note, 1.0f, 0x5EEDu, {});
            if (peakOf (none) != 0.0f)
                fail (juce::String (c.name) + " with no sample loaded is not silent (" + juce::String (peakOf (none)) + ")");
        }
        std::printf ("sample pitch (220 Hz @44.1k played @48k): %s\n", report.toRawUTF8());

        // Stereo samples stay stereo.
        for (int slot : { (int) slotBody, (int) slotNoise, (int) slotClick, (int) slotLayer1 })
        {
            auto p = engineParams (slot == slotBody, slot == slotNoise, slot == slotClick);
            p.bodyStyle = bodySample;
            p.noiseStyle = noiseSample;
            p.clickType = clickSample;
            p.clickTone = 300.0f;
            p.clickLengthMs = 40.0f;
            p.layers[0].on = slot == slotLayer1;
            SampleSet set;
            set.slots[(size_t) slot] = sine220.get();
            const auto hit = renderHit (p, flatTables, sr, 60, 1.0f, 0x5EEDu, set);
            double lr = 0.0, ll = 0.0, rr = 0.0;
            for (int i = 0; i < (int) (0.03 * sr); ++i)
            {
                const double l = hit.getSample (0, i), r = hit.getSample (1, i);
                lr += l * r; ll += l * l; rr += r * r;
            }
            const double corr = lr / std::sqrt (ll * rr + 1.0e-30);
            if (corr > -0.99)
                fail ("stereo sample in slot " + juce::String (slot) + " lost its stereo (L/R correlation " + juce::String (corr, 3) + ")");
        }

        // Swapping samples between blocks: the voices must pick up the new pointer.
        {
            auto p = engineParams (true, false, false);
            p.bodyStyle = bodySample;
            p.lengthMs = 900.0f;
            DrumEngine engine;
            engine.prepare (sr, 512);
            engine.setParams (p);
            engine.setTables (flatTables);
            SampleSet set;
            set.slots[slotBody] = mono220.get();
            engine.setSamples (set);
            const auto shortSample = makeSineSample (440.0, 22050.0, 0.05, true);

            juce::AudioBuffer<float> block (2, 512);
            float lateTail = 0.0f;
            bool finite = true;
            for (int b = 0; b < 80; ++b)
            {
                juce::MidiBuffer midi;
                if (b == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
                if (b == 10)
                {
                    set.slots[slotBody] = shortSample.get(); // shorter than the current read position
                    engine.setSamples (set);
                }
                if (b == 20)
                {
                    set.slots[slotBody] = nullptr;
                    engine.setSamples (set);
                }
                engine.process (block, midi);
                finite &= allFinite (block);
                if (b >= 30)
                    lateTail = juce::jmax (lateTail, peakOf (block));
            }
            if (! finite)
                fail ("swapping samples mid-hit produced non-finite samples");
            if (lateTail > 1.0e-4f)
                fail ("body still sounds after its sample was removed (" + juce::String (lateTail) + ")");
        }
    }

    // --- extra layers: on, pan, delay, filters -------------------------------------
    {
        auto layerHit = [&] (auto&& setup)
        {
            auto p = engineParams (false, false, false);
            auto& l = p.layers[0];
            l.on = true;
            l.source = 1; // sine
            l.env = layerEnvFlat;
            l.gain = 0.05f; // keeps the master's tanh stage linear
            setup (p);
            return renderHit (p, flatTables, sr, 60, 1.0f, 0x5EEDu, all);
        };

        if (peakOf (layerHit ([] (DrumParams& p) { p.layers[0].on = false; })) != 0.0f)
            fail ("layer switched off is not silent");

        for (int src = 0; src < 3; ++src)
            for (int env = 0; env < layerEnvNames.size(); ++env)
            {
                const auto hit = layerHit ([&] (DrumParams& p) { p.layers[0].source = src; p.layers[0].env = env; p.layers[0].hp = 200.0f; p.layers[0].lp = 5000.0f; });
                if (! allFinite (hit) || peakOf (hit) < 0.01f)
                    fail ("layer " + layerSourceNames[src] + " / " + layerEnvNames[env] + ": silent or non-finite");
            }

        const auto left = layerHit ([] (DrumParams& p) { p.layers[0].pan = -1.0f; });
        const auto right = layerHit ([] (DrumParams& p) { p.layers[0].pan = 1.0f; });
        const auto centre = layerHit ([] (DrumParams&) {});
        const int n = centre.getNumSamples();
        if (right.getMagnitude (0, 0, n) > 1.0e-5f || left.getMagnitude (1, 0, n) > 1.0e-5f)
            fail ("hard-panned layer leaks into the other channel");
        float centreDiff = 0.0f;
        for (int i = 0; i < n; ++i)
            centreDiff = juce::jmax (centreDiff, std::abs (centre.getSample (0, i) - centre.getSample (1, i)));
        const float hardDb = juce::Decibels::gainToDecibels (left.getRMSLevel (0, 0, n) / centre.getRMSLevel (0, 0, n));
        if (centreDiff > 1.0e-6f || std::abs (hardDb - 3.01f) > 0.1f)
            fail ("layer pan law: centre L/R diff " + juce::String (centreDiff) + ", hard side " + juce::String (hardDb, 2) + " dB (expected +3.01)");

        const auto delayed = layerHit ([] (DrumParams& p) { p.layers[0].delayMs = 50.0f; p.layers[0].tuneSt = 12.0f; });
        int onset = -1;
        for (int i = 0; i < delayed.getNumSamples() && onset < 0; ++i)
            if (std::abs (delayed.getSample (0, i)) > 1.0e-3f)
                onset = i;
        const double onsetMs = onset * 1000.0 / sr;
        if (std::abs (onsetMs - 50.0) > 0.5)
            fail ("layer delay 50 ms starts at " + juce::String (onsetMs, 2) + " ms");

        const double sineHz = zeroCrossingHz (centre, 0, sr, (int) (0.01 * sr), (int) (0.3 * sr));
        const double expectedSine = DrumParams().tuneHz;
        if (std::abs (sineHz / expectedSine - 1.0) > 0.003)
            fail ("sine layer at Tune: " + juce::String (sineHz, 2) + " Hz, expected " + juce::String (expectedSine, 2));

        const auto open = layerHit ([] (DrumParams& p) { p.layers[0].source = 2; });
        const auto dark = layerHit ([] (DrumParams& p) { p.layers[0].source = 2; p.layers[0].lp = 1000.0f; });
        const auto thin = layerHit ([] (DrumParams& p) { p.layers[0].source = 2; p.layers[0].hp = 4000.0f; });
        auto hfEnergy = [] (const juce::AudioBuffer<float>& b)
        {
            double e = 0.0;
            for (int i = 1; i < b.getNumSamples(); ++i)
                e += juce::square (b.getSample (0, i) - b.getSample (0, i - 1));
            return e;
        };
        auto lfEnergy = [] (const juce::AudioBuffer<float>& b)
        {
            double e = 0.0, y[4] {};
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                double v = b.getSample (0, i);
                for (auto& s : y)
                    v = s += 0.02 * (v - s); // four one-poles at ~150 Hz
                e += v * v;
            }
            return e;
        };
        const double hfRatio = hfEnergy (dark) / hfEnergy (open), lfRatio = lfEnergy (thin) / lfEnergy (open);
        if (hfRatio > 0.05 || lfRatio > 0.01)
            fail ("layer filters: high cut 1 kHz leaves " + juce::String (hfRatio, 3) + " of the HF, low cut 4 kHz "
                  + juce::String (lfRatio, 4) + " of the LF");

        std::printf ("layers: pan hard side %+.2f dB / centre L=R, delay 50 ms -> onset %.2f ms, sine %.2f Hz, "
                     "LP 1k keeps %.3f of HF, HP 4k keeps %.4f of LF\n",
                     hardDb, onsetMs, sineHz, hfRatio, lfRatio);
    }

    // --- block-size independence with the new styles and every layer on ---------------
    {
        struct Setup { int body, noise, click; };
        const Setup setups[] { { bodyMembrane, noiseMetallic, clickBeater }, { body808, noiseCrackle, clickSample },
                               { body909, noisePink, clickTick }, { bodyKick, noiseWhite, clickPop },
                               { bodyFM, noiseSample, clickRim }, { bodySample, noiseWires, clickSnap },
                               { bodyRound, noiseWhite, clickBeater }, { bodyTaper, noisePink, clickTick },
                               { bodyHard, noiseCrackle, clickPop }, { bodyFold, noiseWires, clickBeater },
                               { bodyTonal, noiseMetallic, clickSnap } };
        float worst = 0.0f;
        for (const auto& s : setups)
        {
            DrumParams p;
            p.bodyStyle = s.body;
            p.noiseStyle = s.noise;
            p.clickType = s.click;
            p.noiseGrit = 0.5f;
            for (int k = 0; k < kNumExtraLayers; ++k)
            {
                auto& l = p.layers[(size_t) k];
                l.on = true;
                l.source = k;
                l.env = k;
                l.delayMs = 7.0f * (float) k;
                l.hp = 100.0f;
                l.lp = 8000.0f;
                l.pan = -0.5f + 0.5f * (float) k;
            }
            constexpr int total = 30000;
            const auto big = renderBlocks (p, defTables, all, sr, total, 300, { 4096 });
            const auto small = renderBlocks (p, defTables, all, sr, total, 300, { 1, 37, 64, 65, 500, 13, 128 });
            float maxDiff = 0.0f;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < total; ++i)
                    maxDiff = juce::jmax (maxDiff, std::abs (big.getSample (c, i) - small.getSample (c, i)));
            worst = juce::jmax (worst, maxDiff);
            if (maxDiff > 1.0e-5f)
                fail ("block-size dependence (" + bodyStyleNames[s.body] + " / " + noiseStyleNames[s.noise] + " / "
                      + clickTypeNames[s.click] + ", all layers): max diff " + juce::String (maxDiff));
        }
        std::printf ("block-size independence (new styles, all layers on): max diff %.2e\n", worst);
    }

    // --- kick: zero-crossing start, deep sweeps, and the sub through the master chain ---
    {
        // The new synthesised styles start from a zero crossing even with a deep,
        // driven sweep (checked on the voice itself: the master's oversampling
        // filters blur the first output sample).
        for (int b : { (int) bodyMembrane, (int) body808, (int) body909, (int) bodyKick, (int) bodyFM, (int) bodyRound,
                       (int) bodyTaper, (int) bodyHard, (int) bodyFold, (int) bodyTonal })
        {
            auto p = engineParams (true, false, false);
            p.bodyStyle = b;
            p.tuneHz = 50.0f;
            p.bodyShape = 1.0f;
            p.otGain = 1.0f;
            p.modes = 1.0f;
            DrumVoice voice;
            voice.prepare (sr);
            voice.start (60, 1.0f, 0x5EEDu, p);
            float l[64] {}, r[64] {};
            voice.render (l, r, 64, p, defTables, all);
            if (std::abs (l[0]) > 1.0e-3f || ! std::isfinite (l[1]))
                fail (bodyStyleNames[b] + " doesn't start at a zero crossing (first sample " + juce::String (l[0]) + ")");
        }

        // Sine through the whole chain (drive off): the sub must not be thinned out.
        juce::String response;
        float worstDb = 0.0f;
        for (float hz : { 30.0f, 40.0f, 60.0f, 1000.0f })
        {
            auto q = engineParams (true, false, false);
            q.bodyStyle = bodyKick;
            q.tuneHz = hz;
            q.keytrack = false;
            q.bodyShape = 0.0f;
            q.otGain = 0.0f;
            q.bodyGain = 0.1f;
            q.lengthMs = 2000.0f;
            const auto tone = renderHit (q, flatTables, sr, 60, 1.0f, 0x5EEDu, {});
            const float rms = rmsIn (tone, (int) (1.0 * sr), (int) (1.8 * sr));
            const float db = juce::Decibels::gainToDecibels (rms / (0.1f / juce::MathConstants<float>::sqrt2));
            response << juce::String (hz, 0) << " Hz " << juce::String (db, 2) << " dB  ";
            worstDb = juce::jmin (worstDb, db);
        }
        if (worstDb < -1.0f)
            fail ("master chain loses more than 1 dB of sub: " + response);
        std::printf ("sine through voice + master chain (drive off, clip off): %s\n", response.toRawUTF8());
    }

    return failures;
}

/** Extra layers whose source is Noise or Click: every style, delay and pan, the
    click's pitch against Tone x Tune, which knobs each source ignores, a Noise
    layer against the Noise layer itself, and block-size independence. */
int checkLayerSources()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: %s\n", msg.toRawUTF8()); ++failures; };

    constexpr double sr = 48000.0;
    const auto defTables = engineTables (false);
    const auto flatTables = engineTables (true);

    /** Body, Noise and Click off, and layer 1 on with the given source (level -26 dB,
        which keeps the master's tanh stage linear). */
    auto layerHit = [&] (int source, auto&& setup, const EnvTables& tables, double rate = sr)
    {
        auto p = engineParams (false, false, false);
        auto& l = p.layers[0];
        l.on = true;
        l.source = source;
        l.env = source == layerNoise ? layerEnvNoise : layerEnvFlat;
        l.gain = 0.05f;
        setup (p, l);
        return renderHit (p, tables, rate, 60, 1.0f, 0x5EEDu, {});
    };
    auto identical = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        return a.getNumSamples() == b.getNumSamples() && firstDifference (a, b) < 0;
    };

    // --- every noise style and click type as an extra layer: finite and audible ----
    {
        juce::String levels;
        for (int s = 0; s < layerNoiseStyleNames.size(); ++s)
        {
            float lo = 1.0e9f;
            for (double rate : { 44100.0, 96000.0 })
                for (int env = 0; env < layerEnvNames.size(); ++env)
                    for (float color : { 0.0f, 1.0f })
                        for (float tune : { -24.0f, 24.0f })
                        {
                            const auto hit = layerHit (layerNoise, [&] (DrumParams&, DrumParams::Layer& l)
                            {
                                l.noiseStyle = s;
                                l.env = env;
                                l.color = color;
                                l.tuneSt = tune;
                                l.gain = 1.0f;
                                l.hp = 300.0f;
                                l.lp = 12000.0f;
                            }, defTables, rate);
                            const auto tag = "noise layer " + layerNoiseStyleNames[s] + " / " + layerEnvNames[env] + " color "
                                             + juce::String (color) + " tune " + juce::String (tune) + " @" + juce::String (rate);
                            const float peak = peakOf (hit);
                            if (! allFinite (hit)) fail (tag + ": non-finite samples");
                            if (peak < 0.01f)      fail (tag + ": nearly silent (" + juce::String (peak) + ")");
                            if (peak > 8.0f)       fail (tag + ": peak " + juce::String (peak) + " (clipper off)");
                            lo = juce::jmin (lo, peak);
                        }
            levels << layerNoiseStyleNames[s] << " " << juce::String (juce::Decibels::gainToDecibels (lo), 1) << "  ";
        }

        for (int c = 0; c < layerClickTypeNames.size(); ++c)
        {
            float lo = 1.0e9f;
            for (double rate : { 44100.0, 96000.0 })
                for (float tone : { 300.0f, 12000.0f })
                    for (float len : { 0.5f, 40.0f })
                        for (float tune : { -24.0f, 0.0f, 24.0f })
                        {
                            const auto hit = layerHit (layerClick, [&] (DrumParams&, DrumParams::Layer& l)
                            {
                                l.clickType = c;
                                l.clickTone = tone;
                                l.clickLengthMs = len;
                                l.tuneSt = tune;
                                l.gain = 1.0f;
                            }, defTables, rate);
                            const auto tag = "click layer " + layerClickTypeNames[c] + " tone " + juce::String (tone) + " length "
                                             + juce::String (len) + " tune " + juce::String (tune) + " @" + juce::String (rate);
                            const float peak = peakOf (hit);
                            if (! allFinite (hit)) fail (tag + ": non-finite samples");
                            if (peak < 0.005f)     fail (tag + ": nearly silent (" + juce::String (peak) + ")");
                            if (peak > 4.0f)       fail (tag + ": peak " + juce::String (peak));
                            lo = juce::jmin (lo, peak);
                        }
            levels << layerClickTypeNames[c] << " " << juce::String (juce::Decibels::gainToDecibels (lo), 1) << "  ";
        }
        std::printf ("noise / click layers: every style finite and audible, quietest peak dB: %s\n", levels.toRawUTF8());
    }

    // --- delay and pan -------------------------------------------------------------------
    {
        juce::String report;
        for (int source : { (int) layerNoise, (int) layerClick })
        {
            const auto name = layerSourceNames[source];
            auto setup = [source] (float pan, float delayMs)
            {
                return [=] (DrumParams&, DrumParams::Layer& l)
                {
                    l.noiseStyle = noiseWhite;
                    l.clickType = clickStick;
                    l.clickTone = 1000.0f;
                    l.clickLengthMs = 20.0f;
                    l.pan = pan;
                    l.delayMs = delayMs;
                    juce::ignoreUnused (source);
                };
            };
            const auto centre = layerHit (source, setup (0.0f, 0.0f), flatTables);
            const auto left = layerHit (source, setup (-1.0f, 0.0f), flatTables);
            const auto right = layerHit (source, setup (1.0f, 0.0f), flatTables);
            const int n = centre.getNumSamples();
            if (right.getMagnitude (0, 0, n) > 1.0e-5f || left.getMagnitude (1, 0, n) > 1.0e-5f)
                fail (name + " layer hard-panned leaks into the other channel");
            const float hardDb = juce::Decibels::gainToDecibels (left.getRMSLevel (0, 0, n) / centre.getRMSLevel (0, 0, n));
            if (std::abs (hardDb - 3.01f) > 0.1f)
                fail (name + " layer pan law: hard side " + juce::String (hardDb, 2) + " dB against the centre (expected +3.01)");

            // Noise stays stereo in the centre; a click is mono.
            double lr = 0.0, ll = 0.0, rr = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double l = centre.getSample (0, i), r = centre.getSample (1, i);
                lr += l * r; ll += l * l; rr += r * r;
            }
            const double corr = lr / std::sqrt (ll * rr + 1.0e-30);
            if (source == layerNoise ? (corr > 0.95 || corr < 0.3) : corr < 0.9999)
                fail (name + " layer in the centre: L/R correlation " + juce::String (corr, 3));

            const auto delayed = layerHit (source, setup (0.0f, 30.0f), flatTables);
            int onset = -1;
            for (int i = 0; i < delayed.getNumSamples() && onset < 0; ++i)
                if (std::abs (delayed.getSample (0, i)) > 1.0e-3f)
                    onset = i;
            const double onsetMs = onset * 1000.0 / sr;
            if (onsetMs < 29.5 || onsetMs > 30.5)
                fail (name + " layer delay 30 ms starts at " + juce::String (onsetMs, 2) + " ms");

            report << name << ": hard side " << juce::String (hardDb, 2) << " dB, centre L/R corr " << juce::String (corr, 2)
                   << ", 30 ms delay -> onset " << juce::String (onsetMs, 2) << " ms  ";
        }
        std::printf ("noise / click layer pan and delay: %s\n", report.toRawUTF8());
    }

    // --- click pitch follows Tone x 2^(Tune/12) ---------------------------------------------
    {
        juce::String report;
        for (float tune : { 0.0f, 7.0f, -12.0f, 19.0f })
        {
            const auto hit = layerHit (layerClick, [&] (DrumParams&, DrumParams::Layer& l)
            {
                l.clickType = clickStick;
                l.clickTone = 1000.0f;
                l.clickLengthMs = 40.0f;
                l.tuneSt = tune;
            }, flatTables);
            const double hz = zeroCrossingHz (hit, 0, sr, (int) (0.001 * sr), (int) (0.02 * sr));
            const double expected = 1000.0 * std::exp2 (tune / 12.0);
            report << "Stick " << juce::String (tune, 0) << " st " << juce::String (hz, 1) << " Hz  ";
            if (std::abs (hz / expected - 1.0) > 0.005)
                fail ("Stick layer at 1 kHz, Tune " + juce::String (tune) + ": " + juce::String (hz, 1) + " Hz, expected "
                      + juce::String (expected, 1));
        }

        // Pop sweeps down as it decays, and the sweep doesn't depend on the pitch.
        // So an octave up, its 2k-th rising zero crossing lands on the k-th one at Tune 0.
        auto popCrossings = [&] (float tune)
        {
            const auto hit = layerHit (layerClick, [&] (DrumParams&, DrumParams::Layer& l)
            {
                l.clickType = clickPop;
                l.clickTone = 800.0f;
                l.clickLengthMs = 40.0f;
                l.tuneSt = tune;
            }, flatTables);
            std::vector<double> t;
            for (int i = (int) (0.0005 * sr); i < (int) (0.03 * sr); ++i)
            {
                const float a = hit.getSample (0, i - 1), b = hit.getSample (0, i);
                if (a < 0.0f && b >= 0.0f)
                    t.push_back ((double) (i - 1) + (double) (a / (a - b)));
            }
            return t;
        };
        const auto pop0 = popCrossings (0.0f), pop12 = popCrossings (12.0f);
        double worstMs = 1.0e9;
        if (pop0.size() >= 6 && pop12.size() >= 12)
        {
            worstMs = 0.0;
            for (size_t k = 1; k <= 6; ++k)
                worstMs = juce::jmax (worstMs, std::abs (pop12[2 * k - 1] - pop0[k - 1]) * 1000.0 / sr);
        }
        report << "Pop +12 st: 2k-th crossings within " << juce::String (worstMs, 3) << " ms of Tune 0's k-th";
        if (worstMs > 0.03)
            fail ("Pop layer +12 st isn't an octave above Tune 0 (crossings off by " + juce::String (worstMs, 3) + " ms)");
        std::printf ("click layer pitch: %s\n", report.toRawUTF8());
    }

    // --- knobs a source ignores, and the ones it doesn't ---------------------------------------
    {
        auto noiseWith = [&] (int style, int env, float color, float tune)
        {
            return layerHit (layerNoise, [=] (DrumParams&, DrumParams::Layer& l)
            {
                l.noiseStyle = style;
                l.env = env;
                l.color = color;
                l.tuneSt = tune;
            }, defTables);
        };
        if (! identical (noiseWith (noiseWhite, layerEnvNoise, 0.4f, 0.0f), noiseWith (noiseWhite, layerEnvNoise, 0.4f, 12.0f)))
            fail ("Tune changes a White noise layer (it should only transpose Metallic)");
        if (identical (noiseWith (noiseMetallic, layerEnvNoise, 0.4f, 0.0f), noiseWith (noiseMetallic, layerEnvNoise, 0.4f, 12.0f)))
            fail ("Tune doesn't change a Metallic noise layer");
        if (! identical (noiseWith (noiseWhite, layerEnvFlat, 0.0f, 0.0f), noiseWith (noiseWhite, layerEnvFlat, 1.0f, 0.0f)))
            fail ("Color changes a White noise layer on the Flat envelope (it has nothing to follow)");
        if (identical (noiseWith (noiseWhite, layerEnvNoise, 0.0f, 0.0f), noiseWith (noiseWhite, layerEnvNoise, 1.0f, 0.0f)))
            fail ("Color doesn't darken a White noise layer on the Noise envelope");
        if (identical (noiseWith (noiseMetallic, layerEnvFlat, 0.0f, 0.0f), noiseWith (noiseMetallic, layerEnvFlat, 1.0f, 0.0f)))
            fail ("Color (Pitch) doesn't change a Metallic noise layer");

        auto clickWith = [&] (int env)
        {
            return layerHit (layerClick, [=] (DrumParams&, DrumParams::Layer& l) { l.clickType = clickRim; l.env = env; }, defTables);
        };
        if (identical (clickWith (layerEnvBody), clickWith (layerEnvFlat)))
            fail ("a Click layer's Level curve (Body) doesn't scale its own decay");

        // Each style differs from the others. (Wires only differs from White by
        // its coupling to the body, so the body is on here.)
        std::vector<juce::AudioBuffer<float>> styles;
        for (int s = 0; s < layerNoiseStyleNames.size(); ++s)
            styles.push_back (layerHit (layerNoise, [=] (DrumParams& p, DrumParams::Layer& l)
            {
                p.bodyOn = true;
                p.bodyGain = 0.05f;
                l.noiseStyle = s;
            }, defTables));
        for (size_t a = 0; a < styles.size(); ++a)
            for (size_t b = a + 1; b < styles.size(); ++b)
                if (identical (styles[a], styles[b]))
                    fail ("noise layer styles " + layerNoiseStyleNames[(int) a] + " and " + layerNoiseStyleNames[(int) b] + " render the same");

        // Captions agree with the above.
        const bool captionsOk = layerTuneCaption (layerNoise, noiseWhite) == nullptr && layerTuneCaption (layerNoise, noiseMetallic) != nullptr
                             && layerTuneCaption (layerClick, clickStick) != nullptr
                             && layerColorCaption (layerNoise, noiseWhite, layerEnvFlat) == nullptr
                             && layerColorCaption (layerNoise, noiseMetallic, layerEnvFlat) != nullptr
                             && layerColorCaption (layerClick, noiseWhite, layerEnvNoise) == nullptr
                             && layerClickToneCaption (layerNoise, clickStick) == nullptr && layerClickToneCaption (layerClick, clickTick) != nullptr
                             && layerClickLengthCaption (layerClick, clickPop) != nullptr && layerUsesClickControls (layerClick)
                             && layerUsesColor (layerNoise) && ! layerUsesColor (layerSine) && layerUsesSample (layerSample)
                             && layerSourceNames.size() == numLayerSources && layerNoiseStyleNames.size() == noiseSample
                             && layerClickTypeNames.size() == clickSample;
        for (int s = 0; s < layerNoiseStyleNames.size(); ++s)
            if (layerNoiseStyleNames[s] != noiseStyleNames[s])
                fail ("layerNoiseStyleNames[" + juce::String (s) + "] doesn't match noiseStyleNames");
        for (int c = 0; c < layerClickTypeNames.size(); ++c)
            if (layerClickTypeNames[c] != clickTypeNames[c])
                fail ("layerClickTypeNames[" + juce::String (c) + "] doesn't match clickTypeNames");
        if (! captionsOk)
            fail ("layer caption helpers in StyleInfo.h disagree with the engine");
    }

    // --- a Noise layer sounds like the Noise layer at the same settings -------------------------
    {
        // Octave-band energy (both channels) over the first 250 ms.
        auto bands = [] (const juce::AudioBuffer<float>& b)
        {
            std::array<double, 5> e {};
            const float centres[] { 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f };
            const int end = juce::jmin (b.getNumSamples(), (int) (0.25 * sr));
            for (size_t k = 0; k < e.size(); ++k)
                for (int c = 0; c < 2; ++c)
                {
                    dsp::Svf bp;
                    bp.setCoefficients (centres[k], 1.41f, (float) sr);
                    for (int i = 0; i < end; ++i)
                        e[k] += juce::square ((double) bp.bandpass (b.getSample (c, i)));
                }
            return e;
        };

        juce::String report;
        for (int s = 0; s < layerNoiseStyleNames.size(); ++s)
        {
            auto core = engineParams (false, true, false);
            core.noiseStyle = s;
            core.noiseGain = 0.05f;
            core.noiseColor = 0.4f;
            core.noiseHP = 450.0f;
            core.noiseLP = 9000.0f;
            core.noiseWidth = 0.35f;
            core.noiseGrit = s == noiseCrackle ? 0.5f : 0.0f;
            const auto coreHit = renderHit (core, defTables, sr, 60, 1.0f, 0x5EEDu, {});

            const auto layer = layerHit (layerNoise, [&] (DrumParams&, DrumParams::Layer& l)
            {
                l.noiseStyle = s;
                l.color = 0.4f;
                l.hp = 450.0f;
                l.lp = 9000.0f;
            }, defTables);

            const auto a = bands (coreHit), b = bands (layer);
            double worst = 0.0;
            for (size_t k = 0; k < a.size(); ++k)
                worst = juce::jmax (worst, std::abs (10.0 * std::log10 ((b[k] + 1.0e-20) / (a[k] + 1.0e-20))));
            const int end = (int) (0.25 * sr);
            const float rmsDb = juce::Decibels::gainToDecibels (rmsIn (layer, 0, end) / rmsIn (coreHit, 0, end));
            report << layerNoiseStyleNames[s] << " " << juce::String (rmsDb, 1) << " dB (bands within " << juce::String (worst, 1) << ")  ";
            if (worst > 3.0 || std::abs (rmsDb) > 1.5f)
                fail ("noise layer " + layerNoiseStyleNames[s] + " against the Noise layer: rms " + juce::String (rmsDb, 2)
                      + " dB, octave bands differ by up to " + juce::String (worst, 2) + " dB");
        }
        std::printf ("noise layer against the Noise layer at the same settings, rms: %s\n", report.toRawUTF8());
    }

    // --- block-size independence with three noise / click layers over the full voice ------------
    {
        struct Setup { int src[3]; int style[3]; };
        const Setup setups[] { { { layerNoise, layerNoise, layerClick }, { noiseWires, noiseMetallic, clickRim } },
                               { { layerClick, layerNoise, layerNoise }, { clickTick, noiseCrackle, noisePink } },
                               { { layerNoise, layerClick, layerClick }, { noiseWhite, clickBeater, clickPop } },
                               { { layerClick, layerClick, layerNoise }, { clickSnap, clickStick, noiseWires } } };
        float worst = 0.0f;
        for (const auto& s : setups)
        {
            DrumParams p;
            p.noiseGrit = 0.3f;
            for (int k = 0; k < kNumExtraLayers; ++k)
            {
                auto& l = p.layers[(size_t) k];
                l.on = true;
                l.source = s.src[k];
                l.noiseStyle = s.style[k];
                l.clickType = s.style[k];
                l.env = k;
                l.gain = 0.5f;
                l.tuneSt = 3.0f * (float) k;
                l.delayMs = 7.3f * (float) k;
                l.hp = 100.0f + 400.0f * (float) k;
                l.lp = 16000.0f - 4000.0f * (float) k;
                l.pan = -0.6f + 0.6f * (float) k;
                l.clickLengthMs = 10.0f;
            }
            constexpr int total = 30000;
            const auto big = renderBlocks (p, defTables, {}, sr, total, 300, { 4096 });
            const auto small = renderBlocks (p, defTables, {}, sr, total, 300, { 1, 37, 64, 65, 500, 13, 128 });
            float maxDiff = 0.0f;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < total; ++i)
                    maxDiff = juce::jmax (maxDiff, std::abs (big.getSample (c, i) - small.getSample (c, i)));
            worst = juce::jmax (worst, maxDiff);
            if (! allFinite (big) || peakOf (big) < 0.01f)
                fail ("three noise / click layers: silent or non-finite");
            if (maxDiff > 1.0e-5f)
                fail ("block-size dependence with three noise / click layers: max diff " + juce::String (maxDiff));
        }
        std::printf ("block-size independence (three noise / click layers): max diff %.2e\n", worst);
    }

    return failures;
}

//==============================================================================
// Layer offsets, solo, the Clap, the pitch curve in semitones, and envelope levels over 0 dB

/** Renders one voice on its own (no master chain), in the given block sizes
    (cycled), until it ends or `total` samples have gone by. */
juce::AudioBuffer<float> renderVoice (const DrumParams& p, const EnvTables& t, double sr, int total,
                                      std::initializer_list<int> sizes = { 512 }, uint32_t seed = 0x5EEDu,
                                      int note = 60, const SampleSet& samples = {})
{
    DrumVoice voice;
    voice.prepare (sr);
    voice.start (note, 1.0f, seed, p);
    juce::AudioBuffer<float> out (2, total);
    out.clear();
    auto it = sizes.begin();
    for (int pos = 0; pos < total && voice.isActive();)
    {
        const int n = juce::jmin (*it, total - pos);
        if (++it == sizes.end())
            it = sizes.begin();
        voice.render (out.getWritePointer (0, pos), out.getWritePointer (1, pos), n, p, t, samples);
        pos += n;
    }
    return out;
}

/** Largest difference between a[fromA ..] and b[fromB ..] over `count` samples (both channels). */
float maxDiffAt (const juce::AudioBuffer<float>& a, int fromA, const juce::AudioBuffer<float>& b, int fromB, int count)
{
    count = juce::jmin (count, a.getNumSamples() - fromA, b.getNumSamples() - fromB);
    float d = 0.0f;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < count; ++i)
            d = juce::jmax (d, std::abs (a.getSample (c, fromA + i) - b.getSample (c, fromB + i)));
    return d;
}

/** Index of the last sample louder than `threshold`, or -1. */
int lastAbove (const juce::AudioBuffer<float>& b, float threshold)
{
    for (int i = b.getNumSamples(); --i >= 0;)
        if (std::abs (b.getSample (0, i)) > threshold || std::abs (b.getSample (1, i)) > threshold)
            return i;
    return -1;
}

/** Offsets: a late layer starts late and is otherwise the same; an early one is
    already that far in at the hit (after the fade-in) and loses its start; the
    voice lives until the latest layer ends; block-size independence. */
int checkLayerOffsets()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: offsets: %s\n", msg.toRawUTF8()); ++failures; };

    constexpr double sr = 48000.0;
    constexpr int msSamples = 48;
    const auto defTables = engineTables (false);
    const auto flatTables = engineTables (true);
    const int total = (int) (0.7 * sr);

    // Each kind of layer on its own, with its offset set by `offset`.
    struct Case { const char* name; std::function<void (DrumParams&, float)> setup; };
    const Case cases[] {
        { "body", [] (DrumParams& p, float o) { p.bodyOn = true; p.bodyOffsetMs = o; } },
        { "body 808", [] (DrumParams& p, float o) { p.bodyOn = true; p.bodyStyle = body808; p.bodyOffsetMs = o; } },
        { "noise", [] (DrumParams& p, float o) { p.noiseOn = true; p.noiseOffsetMs = o; } },
        { "click", [] (DrumParams& p, float o)
          {
              p.clickOn = true;
              p.clickType = clickSnap;
              p.clickGain = 1.0f;
              p.clickLengthMs = 40.0f;
              p.clickOffsetMs = o;
          } },
        { "sine layer", [] (DrumParams& p, float o)
          {
              auto& l = p.layers[0];
              l.on = true;
              l.source = layerSine;
              l.env = layerEnvBody;
              l.delayMs = o;
          } },
        { "noise layer", [] (DrumParams& p, float o)
          {
              auto& l = p.layers[1];
              l.on = true;
              l.source = layerNoise;
              l.noiseStyle = noisePink;
              l.env = layerEnvNoise;
              l.delayMs = o;
          } },
    };

    // --- +30 ms: silence first, then exactly the un-offset layer ----------------------
    // --- -20 ms: at the hit it is 20 ms in (after the 0.5 ms fade-in) ------------------
    {
        juce::String report;
        const int fade = (int) std::ceil (DrumVoice::kCutFadeMs * 0.001 * sr);
        for (const auto& c : cases)
        {
            auto make = [&] (float offset)
            {
                auto p = engineParams (false, false, false);
                c.setup (p, offset);
                return renderVoice (p, defTables, sr, total);
            };
            const auto base = make (0.0f);
            const auto late = make (30.0f);
            const auto early = make (-20.0f);
            const int lenSamples = (int) (DrumParams().lengthMs * msSamples);

            const float before = peakIn (late, 0, 30 * msSamples);
            const float lateDiff = maxDiffAt (late, 30 * msSamples, base, 0, lenSamples - 5 * msSamples);
            if (before != 0.0f)
                fail (juce::String (c.name) + " +30 ms: sound before 30 ms (" + juce::String (before) + ")");
            if (lateDiff > 1.0e-6f)
                fail (juce::String (c.name) + " +30 ms: differs from the un-offset layer 30 ms earlier by " + juce::String (lateDiff));

            const float earlyDiff = maxDiffAt (early, fade, base, 20 * msSamples + fade, lenSamples - 30 * msSamples);
            float fadeOver = 0.0f;
            for (int i = 0; i < fade; ++i)
                fadeOver = juce::jmax (fadeOver, std::abs (early.getSample (0, i)) - std::abs (base.getSample (0, 20 * msSamples + i)));
            if (early.getSample (0, 0) != 0.0f || fadeOver > 1.0e-6f)
                fail (juce::String (c.name) + " -20 ms: the cut doesn't fade in from silence (first sample "
                      + juce::String (early.getSample (0, 0)) + ", over by " + juce::String (fadeOver) + ")");
            if (earlyDiff > 1.0e-5f)
                fail (juce::String (c.name) + " -20 ms: differs from the un-offset layer 20 ms in by " + juce::String (earlyDiff));
            if (peakOf (base) < 0.01f)
                fail (juce::String (c.name) + ": test layer is silent");

            report << c.name << " " << juce::String (lateDiff, 7) << "/" << juce::String (earlyDiff, 7) << "  ";
        }
        std::printf ("offsets: +30 ms / -20 ms against the un-offset layer, max diff: %s\n", report.toRawUTF8());
    }

    // --- the part past the end is lost too: an early layer ends early -------------------
    {
        auto p = engineParams (true, false, false);
        p.lengthMs = 300.0f;
        p.bodyOffsetMs = -40.0f;
        const auto hit = renderVoice (p, flatTables, sr, total);
        const int last = lastAbove (hit, 0.0f);
        const double lastMs = last / (double) msSamples;
        if (lastMs > 260.0 || lastMs < 257.0)
            fail ("body at -40 ms with Length 300 ms ends at " + juce::String (lastMs, 2) + " ms (expected 260)");
    }

    // --- the voice lives until its latest layer ends ---------------------------------------
    {
        auto p = engineParams (true, true, false);
        p.lengthMs = 200.0f;
        p.bodyOffsetMs = 100.0f;
        p.noiseOffsetMs = -30.0f;
        juce::String report;

        DrumVoice voice;
        voice.prepare (sr);
        voice.start (60, 1.0f, 0x5EEDu, p);
        juce::AudioBuffer<float> out (2, total);
        out.clear();
        int endedAt = -1;
        for (int pos = 0; pos < total; pos += 64)
        {
            voice.render (out.getWritePointer (0, pos), out.getWritePointer (1, pos), 64, p, flatTables, {});
            if (! voice.isActive())
            {
                endedAt = pos + 64;
                break;
            }
        }
        const double lastMs = lastAbove (out, 1.0e-3f) / (double) msSamples;
        const double endMs = endedAt / (double) msSamples;
        const double noiseEndMs = lastAbove (renderVoice ([&] { auto q = p; q.bodyOn = false; return q; }(), flatTables, sr, total), 0.0f)
                                  / (double) msSamples;
        if (lastMs < 297.0 || endMs < 300.0 || endMs > 304.0)
            fail ("Length 200 + body at +100 ms: last sound at " + juce::String (lastMs, 2) + " ms, voice ended at "
                  + juce::String (endMs, 2) + " ms (expected ~300)");
        if (noiseEndMs > 170.0 || noiseEndMs < 167.0)
            fail ("noise at -30 ms with Length 200: ends at " + juce::String (noiseEndMs, 2) + " ms (expected 170)");

        const auto hit = renderHit (p, flatTables, sr, 60, 1.0f, 0x5EEDu, {});
        const double hitMs = hit.getNumSamples() / (double) msSamples;
        if (hitMs < 330.0 - 0.1)
            fail ("renderHit with a +100 ms layer is only " + juce::String (hitMs, 2) + " ms long (expected Length + 100 + 30)");
        if (std::abs (DrumEngine::hitLengthSeconds (p) - 0.3) > 1.0e-9 || DrumVoice::latestOffsetMs (p) != 100.0f)
            fail ("hitLengthSeconds / latestOffsetMs don't include the +100 ms layer");

        auto muted = p;
        muted.bodyOn = false;
        if (DrumVoice::latestOffsetMs (muted) != 0.0f)
            fail ("a muted layer's Offset still lengthens the hit");

        std::printf ("offsets: Length 200 + body +100 ms -> last sound %.2f ms, voice ends %.2f ms, renderHit %.2f ms; "
                     "noise -30 ms ends at %.2f ms\n", lastMs, endMs, hitMs, noiseEndMs);
    }

    // --- block-size independence with offsets on every layer ----------------------------------
    {
        struct Setup { int body, noise, click; float offsets[6]; };
        const Setup setups[] { { bodyClassic, noiseWires, clickSnap, { -23.7f, 11.1f, -3.3f, -50.0f, 30.0f, -7.0f } },
                               { body808, noiseMetallic, clickClap, { 17.0f, -100.0f, 4.4f, 100.0f, -0.4f, 0.0f } },
                               { bodyMembrane, noiseSample, clickSample, { -100.0f, -99.9f, -12.5f, -1.0f, 2.0f, -65.0f } },
                               { bodyFM, noiseCrackle, clickBeater, { 0.0f, 0.0f, 0.0f, -30.0f, -30.0f, 60.0f } } };
        const auto sample = makeSineSample (220.0, 44100.0, 1.0, true, true);
        SampleSet all;
        for (auto& s : all.slots)
            s = sample.get();

        float worst = 0.0f;
        for (const auto& s : setups)
        {
            DrumParams p;
            p.bodyStyle = s.body;
            p.noiseStyle = s.noise;
            p.clickType = s.click;
            p.clickLengthMs = 20.0f;
            p.bodyOffsetMs = s.offsets[0];
            p.noiseOffsetMs = s.offsets[1];
            p.clickOffsetMs = s.offsets[2];
            for (int k = 0; k < kNumExtraLayers; ++k)
            {
                auto& l = p.layers[(size_t) k];
                l.on = true;
                l.source = k + 1; // sine, noise, click
                l.noiseStyle = noiseWires;
                l.clickType = clickClap;
                l.env = k;
                l.gain = 0.5f;
                l.delayMs = s.offsets[3 + k];
            }
            constexpr int length = 40000;
            const auto big = renderBlocks (p, defTables, all, sr, length, 300, { 4096 });
            const auto small = renderBlocks (p, defTables, all, sr, length, 300, { 1, 37, 64, 65, 500, 13, 128 });
            const float d = maxDiffAt (big, 0, small, 0, length);
            worst = juce::jmax (worst, d);
            if (! allFinite (big) || peakOf (big) < 0.01f)
                fail ("offsets on every layer: silent or non-finite");
            if (d > 1.0e-5f)
                fail ("block-size dependence with offsets (" + bodyStyleNames[s.body] + "): max diff " + juce::String (d));
        }
        std::printf ("block-size independence (offsets on every layer, -100..+100 ms): max diff %.2e\n", worst);
    }

    return failures;
}

/** Solo: only soloed layers sound, mute wins, a soloed Noise keeps its coupling
    to the body, and solo changes mid-hit are smoothed and keep the layer in time. */
/** Every factory preset, layer by layer: each layer that's on is heard (muting it
    changes the hit, soloing it leaves sound), each one that's off is silent (its mute
    holds), and the M buttons (Body's and each tab's) switch exactly their own layer
    on and off and back, and survive nothing across a preset load (presets set them). */
int checkPresetLayers()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: preset layers: %s\n", msg.toRawUTF8()); ++failures; };
    constexpr double sr = 48000.0;

    DrumFoundryProcessor proc;
    proc.prepareToPlay (sr, 512);
    auto& presets = proc.getPresets();
    const int numFactory = (int) factoryPresets().size();

    const auto energy = [] (const juce::AudioBuffer<float>& b)
    {
        double e = 0.0;
        for (int c = 0; c < b.getNumChannels(); ++c)
            for (int i = 0; i < b.getNumSamples(); ++i)
                e += (double) b.getSample (c, i) * b.getSample (c, i);
        return e;
    };
    const auto db = [] (double e) { return 10.0 * std::log10 (e + 1.0e-30); };

    struct LayerRef { juce::String name; std::function<bool& (DrumParams&)> on; std::function<bool& (DrumParams&)> solo; };
    std::vector<LayerRef> layerRefs {
        { "Body",  [] (DrumParams& p) -> bool& { return p.bodyOn; },  [] (DrumParams& p) -> bool& { return p.bodySolo; } },
        { "Noise", [] (DrumParams& p) -> bool& { return p.noiseOn; }, [] (DrumParams& p) -> bool& { return p.noiseSolo; } },
        { "Click", [] (DrumParams& p) -> bool& { return p.clickOn; }, [] (DrumParams& p) -> bool& { return p.clickSolo; } } };
    for (int k = 0; k < kNumExtraLayers; ++k)
        layerRefs.push_back ({ "Layer " + juce::String (k + 1),
                               [k] (DrumParams& p) -> bool& { return p.layers[(size_t) k].on; },
                               [k] (DrumParams& p) -> bool& { return p.layers[(size_t) k].solo; } });

    int checkedLayers = 0;
    for (int i = 0; i < numFactory; ++i)
    {
        presets.load (i);
        const auto name = factoryPresets()[(size_t) i].name;
        EnvTables tables;
        proc.bakeTables (tables);
        std::array<SamplePtr, numSampleSlots> holder;
        const auto samples = currentSampleSet (proc, holder);
        auto p = proc.readParams();
        p.clipMode = 0; // nothing squashes a layer's difference away
        const double full = energy (renderHit (p, tables, sr, 60, 1.0f, 0x5EEDu, samples));
        if (! (full > 1.0e-8))
        {
            fail (juce::String (name) + ": silent");
            continue;
        }

        juce::String row;
        for (auto& ref : layerRefs)
        {
            if (! ref.on (p))
                continue;
            ++checkedLayers;
            auto muted = p;
            ref.on (muted) = false;
            const auto mutedHit = renderHit (muted, tables, sr, 60, 1.0f, 0x5EEDu, samples);
            const auto fullHit = renderHit (p, tables, sr, 60, 1.0f, 0x5EEDu, samples);
            double diff = 0.0;
            for (int c = 0; c < 2; ++c)
                for (int s = 0; s < juce::jmin (mutedHit.getNumSamples(), fullHit.getNumSamples()); ++s)
                {
                    const double d = (double) fullHit.getSample (c, s) - mutedHit.getSample (c, s);
                    diff += d * d;
                }
            auto soloed = p;
            ref.solo (soloed) = true;
            const double alone = energy (renderHit (soloed, tables, sr, 60, 1.0f, 0x5EEDu, samples));
            const double contribution = db (diff) - db (full);
            row << ref.name << " " << juce::String (contribution, 0) << " dB  ";
            if (contribution < -50.0)
                fail (juce::String (name) + ": " + ref.name + " is on but muting it changes nothing (" + juce::String (contribution, 1) + " dB)");
            if (! (alone > full * 1.0e-6))
                fail (juce::String (name) + ": " + ref.name + " soloed is silent");
        }
        std::printf ("  %-24s %s\n", name, row.toRawUTF8());
    }
    std::printf ("preset layers: %d presets, %d layers that are on, each heard, muted and soloed\n", numFactory, checkedLayers);

    // The M buttons, through the editor, on a preset with every core layer on.
    presets.load (3);
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    auto& state = proc.getState();
    const auto onValue = [&state] (const juce::String& id) { return state.getRawParameterValue (id)->load() > 0.5f; };
    const auto press = [] (juce::Button& b) { if (b.onClick) b.onClick(); }; // (a click: triggerClick() is asynchronous)
    {
        auto* body = findChild<ui::LayerPanel> (*editor);
        auto* layers = findChild<ui::LayersSection> (*editor);
        if (body == nullptr || layers == nullptr || body->getMuteButton() == nullptr)
            fail ("no Body panel or Layers section");
        else
        {
            struct Case { juce::Button* button; juce::String id; const char* what; };
            const Case cases[] { { body->getMuteButton(), pid::bodyOn, "Body's M" },
                                 { &layers->getMuteButton (ui::LayersSection::noiseTab), pid::noiseOn, "Noise tab's M" },
                                 { &layers->getMuteButton (ui::LayersSection::clickTab), pid::clickOn, "Click tab's M" } };
            for (const auto& c : cases)
            {
                std::map<juce::String, bool> before;
                for (auto* id : { pid::bodyOn, pid::noiseOn, pid::clickOn })
                    before[id] = onValue (id);
                press (*c.button);
                bool othersSame = true;
                for (auto& [id, v] : before)
                    if (id != c.id && onValue (id) != v)
                        othersSame = false;
                const bool flipped = onValue (c.id) != before[c.id];
                const bool lit = static_cast<ui::MuteButton*> (c.button)->isMuted() == ! onValue (c.id);
                press (*c.button);
                const bool back = onValue (c.id) == before[c.id];
                if (! flipped || ! othersSame || ! lit || ! back)
                    fail (juce::String (c.what) + ": flipped " + (flipped ? "yes" : "NO") + ", others untouched " + (othersSame ? "yes" : "NO")
                          + ", lit when off " + (lit ? "yes" : "NO") + ", back on a second click " + (back ? "yes" : "NO"));
            }
        }
    }

    // A preset load sets every layer's on/off from the preset (mutes don't carry over).
    {
        presets.load (3);
        state.getParameter (pid::bodyOn)->setValueNotifyingHost (0.0f);
        presets.load (3);
        if (! onValue (pid::bodyOn))
            fail ("loading a preset kept a mute instead of the preset's");

        // ...and the effects chain: the preset's, or the default one when it has none.
        proc.setFxChain (FxChain::fromString ("comp1 clip1 reverb"));
        presets.load (3);
        if (proc.getFxChain() != (factoryPresets()[3].chain != nullptr ? FxChain::fromString (factoryPresets()[3].chain) : FxChain::defaults()))
            fail ("loading a preset kept the previous effects chain: \"" + proc.getFxChain().toString() + "\"");
    }
    return failures;
}

int checkSolo()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: solo: %s\n", msg.toRawUTF8()); ++failures; };

    constexpr double sr = 48000.0;
    const auto defTables = engineTables (false);
    const int total = (int) (0.6 * sr);

    // Everything on: body, noise (no coupling), click and a sine layer.
    auto full = [] ()
    {
        auto p = engineParams (true, true, true);
        p.noiseCouple = 0.0f;
        auto& l = p.layers[0];
        l.on = true;
        l.source = layerSine;
        l.env = layerEnvBody;
        l.tuneSt = -12.0f;
        return p;
    };
    auto render = [&] (const DrumParams& p) { return renderVoice (p, defTables, sr, total); };
    auto same = [&] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b) { return maxDiffAt (a, 0, b, 0, total); };

    juce::String report;
    {
        auto solo = full();
        solo.noiseSolo = true;
        auto alone = full();
        alone.bodyOn = alone.clickOn = alone.layers[0].on = false;
        const float d = same (render (solo), render (alone));
        report << "noise " << juce::String (d, 8) << "  ";
        if (d > 1.0e-7f || peakOf (render (alone)) < 0.01f)
            fail ("solo Noise doesn't sound like Noise alone (diff " + juce::String (d) + ")");
    }
    {
        // Coupling survives solo: soloed Wires with Coupling = the same with a silent body.
        auto solo = full();
        solo.noiseSolo = true;
        solo.noiseCouple = 0.8f;
        auto silentBody = solo;
        silentBody.noiseSolo = false;
        silentBody.bodyGain = 0.0f;
        silentBody.clickOn = silentBody.layers[0].on = false;
        auto noBody = silentBody;
        noBody.bodyOn = false;
        const float d = same (render (solo), render (silentBody));
        const float coupling = same (render (solo), render (noBody));
        report << "noise+coupling " << juce::String (d, 8) << "  ";
        if (d > 1.0e-7f || coupling < 1.0e-3f)
            fail ("solo Noise lost its coupling to the body (diff " + juce::String (d) + ", coupling effect " + juce::String (coupling) + ")");
    }
    {
        auto solo = full();
        solo.bodySolo = solo.clickSolo = true;
        auto alone = full();
        alone.noiseOn = alone.layers[0].on = false;
        const float d = same (render (solo), render (alone));
        report << "body+click " << juce::String (d, 8) << "  ";
        if (d > 1.0e-7f)
            fail ("solo Body + Click doesn't sound like Body + Click alone (diff " + juce::String (d) + ")");
    }
    {
        auto solo = full();
        solo.layers[0].solo = true;
        auto alone = full();
        alone.bodyOn = alone.noiseOn = alone.clickOn = false;
        const float d = same (render (solo), render (alone));
        report << "layer " << juce::String (d, 8) << "  ";
        if (d > 1.0e-7f)
            fail ("solo on an extra layer doesn't sound like that layer alone (diff " + juce::String (d) + ")");
    }
    {
        // Mute wins: a soloed layer that is off stays silent, and so does the rest.
        auto p = full();
        p.noiseSolo = true;
        p.noiseOn = false;
        auto q = full();
        q.layers[1].solo = true; // an extra layer that isn't on
        const float a = peakOf (render (p)), b = peakOf (render (q));
        if (a != 0.0f || b != 0.0f)
            fail ("soloing a muted layer isn't silent (" + juce::String (a) + ", " + juce::String (b) + ")");
    }

    // --- solo changes mid-hit: smoothed, and the layer comes back in time ---------------------
    {
        // Body (a flat 100 Hz sine) and a click that is over by 100 ms. At 100 ms
        // the click is soloed (the body fades out), at 200 ms unsoloed again.
        auto p = engineParams (true, false, true);
        p.tuneHz = 100.0f;
        p.bodyShape = 0.0f;
        p.otGain = 0.0f;
        p.modes = 0.0f;
        p.lengthMs = 400.0f;
        const auto flat = engineTables (true);
        const int block = 480;

        auto run = [&] (bool toggle)
        {
            DrumVoice voice;
            voice.prepare (sr);
            voice.start (60, 1.0f, 0x5EEDu, p);
            juce::AudioBuffer<float> out (2, total);
            out.clear();
            auto q = p;
            for (int pos = 0; pos + block <= total && voice.isActive(); pos += block)
            {
                q.clickSolo = toggle && pos >= 4800 && pos < 9600;
                voice.render (out.getWritePointer (0, pos), out.getWritePointer (1, pos), block, q, flat, {});
            }
            return out;
        };
        const auto ref = run (false), toggled = run (true);
        float maxStep = 0.0f;
        for (int i = 4700; i < 5100; ++i)
            maxStep = juce::jmax (maxStep, std::abs (toggled.getSample (0, i) - toggled.getSample (0, i - 1)));
        const float refStep = std::abs (ref.getSample (0, 4801) - ref.getSample (0, 4800));
        const float gone = peakIn (toggled, 4800 + 125, 9600);
        const float fading = peakIn (toggled, 4800 + 30, 4800 + 90);
        const float back = maxDiffAt (toggled, 9600 + 125, ref, 9600 + 125, (int) (0.15 * sr));
        report << "| mid-hit: body gone " << juce::String (gone, 8) << ", max step " << juce::String (maxStep, 4)
               << ", back in time diff " << juce::String (back, 8);
        if (gone > 1.0e-6f || fading < 0.05f || fading > 0.95f)
            fail ("soloing mid-hit: the body isn't faded out over ~2.5 ms (" + juce::String (fading) + " at 1 ms, "
                  + juce::String (gone) + " after)");
        if (maxStep > 0.05f)
            fail ("soloing mid-hit steps the output by " + juce::String (maxStep) + " (sine step " + juce::String (refStep) + ")");
        if (back > 1.0e-6f)
            fail ("unsoloed body isn't back in time with the un-soloed hit (diff " + juce::String (back) + ")");
    }
    std::printf ("solo (against the same layers alone): %s\n", report.toRawUTF8());
    return failures;
}

/** The Clap click: 3-4 bursts where the seed puts them, a tail, deterministic
    per seed, as the Click layer and as an extra Click layer. */
int checkClap()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: clap: %s\n", msg.toRawUTF8()); ++failures; };

    constexpr double sr = 48000.0;
    const auto tables = engineTables (false);
    const int total = (int) (0.3 * sr);

    auto clap = [&] (uint32_t seed, bool asLayer, float tone, float lengthMs)
    {
        auto p = engineParams (false, false, ! asLayer);
        p.clickType = clickClap;
        p.clickGain = 1.0f;
        p.clickTone = tone;
        p.clickLengthMs = lengthMs;
        if (asLayer)
        {
            auto& l = p.layers[0];
            l.on = true;
            l.source = layerClick;
            l.clickType = clickClap;
            l.clickTone = tone;
            l.clickLengthMs = lengthMs;
        }
        return renderVoice (p, tables, sr, total, { 512 }, seed);
    };

    // Burst onsets in ms: a 1 ms peak envelope that jumps to 4x the quietest of
    // the 3 ms before it (and over 12% of the loudest), at least 5 ms apart.
    auto onsets = [&] (const juce::AudioBuffer<float>& b)
    {
        const int win = (int) (0.001 * sr);
        std::vector<float> env;
        for (int i = 0; i + win <= (int) (0.06 * sr); i += win)
            env.push_back (b.getMagnitude (0, i, win));
        const float top = *std::max_element (env.begin(), env.end());
        std::vector<double> t;
        for (size_t j = 0; j < env.size(); ++j)
        {
            float before = 0.0f;
            if (j > 0)
                before = *std::min_element (env.begin() + (long) (j >= 3 ? j - 3 : 0), env.begin() + (long) j);
            if (env[j] > 0.12f * top && env[j] > 4.0f * before && (t.empty() || (double) j - t.back() >= 5.0))
                t.push_back ((double) j);
        }
        return t;
    };

    juce::String report;
    std::set<int> counts;
    std::set<juce::String> patterns;
    for (uint32_t seed = 1; seed <= 12; ++seed)
        for (bool asLayer : { false, true })
        {
            const uint32_t voiceSeed = seed * 7919u;
            const auto hit = clap (voiceSeed, asLayer, 1500.0f, 6.0f);
            const auto tag = juce::String (asLayer ? "extra layer" : "Click layer") + " seed " + juce::String (seed);
            const float peak = peakOf (hit);
            if (! allFinite (hit) || peak < 0.05f || peak > 4.0f)
                fail (tag + ": peak " + juce::String (peak));

            // Where the generator put the bursts (a layer gets its own seed).
            ClickGen gen;
            const uint32_t genSeed = asLayer ? dsp::mixSeed (voiceSeed, 0x3C6EF372u) : voiceSeed;
            gen.start (clickClap, 1500.0f, 6.0f, true, genSeed, (float) sr);
            const auto t = onsets (hit);
            juce::String pattern;
            bool match = (int) t.size() == gen.clapBursts;
            for (size_t k = 0; k < t.size() && match; ++k)
                match = std::abs (t[k] - gen.clapBurstAt[k] * 1000.0 / sr) <= 1.0;
            for (auto x : t)
                pattern << juce::String (x, 0) << " ";
            if (! match || gen.clapBursts < 3 || gen.clapBursts > 4)
                fail (tag + ": burst onsets at " + pattern + "ms, expected " + juce::String (gen.clapBursts) + " bursts");
            if (asLayer == false)
            {
                counts.insert ((int) t.size());
                patterns.insert (pattern);
                if (seed <= 3)
                    report << "seed " << juce::String (seed) << ": " << pattern << "ms  ";
            }

            if (maxDiffAt (hit, 0, clap (voiceSeed, asLayer, 1500.0f, 6.0f), 0, total) != 0.0f)
                fail (tag + ": not deterministic");
        }
    if (counts.size() < 2 || patterns.size() < 6)
        fail ("burst count / spacing doesn't vary from hit to hit (" + juce::String ((int) patterns.size()) + " patterns)");

    // Tone moves the band, Length the tail.
    {
        auto centroid = [&] (const juce::AudioBuffer<float>& b)
        {
            double num = 0.0, den = 0.0;
            for (float hz : { 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f })
            {
                dsp::Svf bp;
                bp.setCoefficients (hz, 1.41f, (float) sr);
                double e = 0.0;
                for (int i = 0; i < b.getNumSamples(); ++i)
                    e += juce::square ((double) bp.bandpass (b.getSample (0, i)));
                num += e * std::log2 (hz);
                den += e;
            }
            return std::exp2 (num / den);
        };
        const double lo = centroid (clap (7u, false, 1000.0f, 6.0f)), hi = centroid (clap (7u, false, 4000.0f, 6.0f));
        const auto shortTail = clap (7u, false, 1500.0f, 2.0f), longTail = clap (7u, false, 1500.0f, 40.0f);
        const double shortMs = lastAbove (shortTail, 1.0e-3f) * 1000.0 / sr, longMs = lastAbove (longTail, 1.0e-3f) * 1000.0 / sr;
        report << "| centroid " << juce::String (lo, 0) << " / " << juce::String (hi, 0) << " Hz at Tone 1k / 4k, -60 dB at "
               << juce::String (shortMs, 0) << " / " << juce::String (longMs, 0) << " ms at Length 2 / 40";
        if (hi < lo * 2.0)
            fail ("Tone doesn't move the clap's band (" + juce::String (lo) + " -> " + juce::String (hi) + " Hz)");
        if (longMs < shortMs + 100.0)
            fail ("Length doesn't lengthen the clap's tail (" + juce::String (shortMs) + " -> " + juce::String (longMs) + " ms)");
    }

    // The type list, captions and Sample's index agree with the enum.
    if (clickTypeNames[clickClap] != "Clap" || clickTypeNames[clickSample] != "Sample"
        || sampleStyleIndex (clickTypeNames) != clickSample || clickTypeNames.size() != numClickTypes
        || layerClickTypeNames[clickClap] != "Clap" || layerClickTypeNames.size() != clickSample
        || juce::String (clickLengthCaption (clickClap)) != "Tail" || ! clickUsesSample (clickSample) || clickUsesSample (clickClap)
        || juce::String (layerClickLengthCaption (layerClick, clickClap)) != "Tail")
        fail ("click type names, captions or the Sample index disagree with StyleInfo's ClickType");

    std::printf ("clap bursts (ms): %s\n", report.toRawUTF8());
    return failures;
}

/** The pitch curve in semitones: +12 st plays an octave up, extreme curves and
    Tune 16 kHz stay finite, bounded and alias-free, pitches under 5 Hz. Envelope
    levels over 1: +12 dB at 4, through every layer. */
/** The modelled click types (Felt .. Tok, ImpactGen): names, level, ending, Tone / Length
    extremes, the pitch curve, hit-to-hit variation with a repeatable seed, block-size
    independence through the engine, and as an extra layer. */
int checkImpactClicks()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: %s\n", msg.toRawUTF8()); ++failures; };
    constexpr double sr = 48000.0;

    if (clickTypeNames[clickFelt] != "Felt" || clickTypeNames[clickTok] != "Tok" || clickTypeNames[clickSample] != "Sample"
        || clickSample != clickTypeNames.size() - 1 || layerClickTypeNames.size() != clickSample
        || layerClickTypeNames[clickTok] != "Tok")
        fail ("click type names and the ClickType enum disagree");

    struct Run { std::vector<float> x; bool ended = false; float peak = 0.0f; bool finite = true; };
    const auto run = [] (int type, float tone, float lengthMs, uint32_t seed, float st = 0.0f)
    {
        Run r;
        ClickGen g;
        g.start (type, tone, lengthMs, true, seed, (float) sr);
        while (r.x.size() < (size_t) (4.0 * sr))
        {
            if (! g.active)
            {
                r.ended = true;
                break;
            }
            g.follow (st);
            const float v = g.next();
            r.finite = r.finite && std::isfinite (v);
            r.peak = juce::jmax (r.peak, std::abs (v));
            r.x.push_back (v);
        }
        return r;
    };
    const auto rms = [] (const std::vector<float>& x)
    {
        double e = 0.0;
        for (auto v : x)
            e += (double) v * v;
        return std::sqrt (e / (double) juce::jmax ((size_t) 1, x.size()));
    };

    juce::String report;
    for (int type = clickFelt; type <= clickTok; ++type)
    {
        const auto& name = clickTypeNames[type];
        const float tone = ImpactGen::material (type - clickFelt).refTone;

        const auto a = run (type, tone, 8.0f, 0x5EEDu);
        if (! a.finite || ! a.ended || a.peak < 0.4f || a.peak > 1.6f)
            fail (name + " at its own Tone: peak " + juce::String (a.peak, 3) + (a.ended ? "" : ", never ends")
                  + (a.finite ? "" : ", not finite"));
        report << name << " " << juce::String (a.peak, 2) << " / " << juce::roundToInt (1000.0 * (double) a.x.size() / sr) << " ms  ";

        for (float t : { 300.0f, 12000.0f, 20000.0f })
            for (float len : { 0.5f, 40.0f })
                for (float st : { -36.0f, 0.0f, 24.0f })
                {
                    const auto e = run (type, t, len, 0x1234u, st);
                    if (! e.finite || ! e.ended || e.peak > 4.0f)
                        fail (name + " at Tone " + juce::String (t, 0) + ", Length " + juce::String (len, 1) + ", pitch "
                              + juce::String (st, 0) + " st: peak " + juce::String (e.peak, 3) + (e.ended ? "" : ", never ends"));
                }

        // The same seed gives the same hit; another seed a different one, at about the same level.
        const auto again = run (type, tone, 8.0f, 0x5EEDu), other = run (type, tone, 8.0f, 0x5EEDu + 7919u);
        if (again.x != a.x)
            fail (name + ": the same seed gave a different hit");
        float diff = 0.0f;
        for (size_t i = 0; i < juce::jmin (a.x.size(), other.x.size()); ++i)
            diff = juce::jmax (diff, std::abs (a.x[i] - other.x[i]));
        const double levelDb = 20.0 * std::log10 (rms (other.x) / juce::jmax (1.0e-12, rms (a.x)));
        if (diff < 0.01f || std::abs (levelDb) > 4.0)
            fail (name + ": hits don't vary as they should (max diff " + juce::String (diff, 4) + ", level "
                  + juce::String (levelDb, 2) + " dB)");

        // Through the engine, in odd block sizes.
        auto p = engineParams (false, false, true);
        p.clickType = type;
        p.clickTone = tone;
        p.clickLengthMs = 8.0f;
        const auto tables = engineTables (false);
        const int total = (int) (0.4 * sr);
        const auto big = renderBlocks (p, tables, {}, sr, total, 300, { 4096 });
        const auto small = renderBlocks (p, tables, {}, sr, total, 300, { 1, 37, 64, 65, 500, 13, 128 });
        float maxDiff = 0.0f, peak = 0.0f;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < total; ++i)
            {
                maxDiff = juce::jmax (maxDiff, std::abs (big.getSample (c, i) - small.getSample (c, i)));
                peak = juce::jmax (peak, std::abs (big.getSample (c, i)));
            }
        if (! allFinite (big) || maxDiff > 1.0e-5f || peak < 1.0e-3f)
            fail (name + " through the engine: block-size diff " + juce::String (maxDiff) + ", peak " + juce::String (peak, 4));

        // As an extra Click layer.
        auto q = engineParams (false, false, false);
        q.layers[0].on = true;
        q.layers[0].source = layerClick;
        q.layers[0].clickType = type;
        q.layers[0].clickTone = tone;
        q.layers[0].clickLengthMs = 8.0f;
        const auto layer = renderBlocks (q, tables, {}, sr, total, 300, { 512 });
        if (! allFinite (layer) || layer.getMagnitude (0, total) < 1.0e-3f)
            fail (name + " as an extra Click layer: silent or not finite");
    }
    std::printf ("modelled clicks (peak / length at their own Tone, Length 8 ms): %s\n", report.toRawUTF8());
    return failures;
}

/** renderHit()'s pre-clip capture (the Level tab's clipped-off shading): with the
    clipper Off it is the output sample for sample; with Hard at a -6 dB ceiling the
    output stays under the ceiling while the capture goes over it, and the two agree
    wherever nothing was clipped. */
/** The master chain: any order of any modules renders finite at one fixed latency,
    the order changes the sound, changing it mid-stream crossfades without a jump, the
    order survives a session, a preset and undo, a chain without the reverb has no
    tail, and the output stage's Comp compresses and shades like the clippers. */
int checkFxChain()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: %s\n", msg.toRawUTF8()); ++failures; };
    constexpr double sr = 48000.0;
    const auto tables = engineTables (false);

    // FxChain itself: text and packing round trips, and junk in the text.
    {
        FxChain c;
        for (int m : { (int) fxClip2, (int) fxComp1, (int) fxReverb, (int) fxDrive2, (int) fxEq })
            c.add (m);
        if (FxChain::fromString (c.toString()) != c || FxChain::unpack (c.pack()) != c
            || FxChain::fromString ("reverb nonsense reverb eq") != FxChain::fromString ("reverb eq")
            || FxChain::fromString ("").length != 0)
            fail ("FxChain text / packing round trip");
    }

    // Every module alone, all of them, and a few orders: finite, same latency.
    {
        DrumEngine reference;
        reference.prepare (sr, 512);
        const int latency = reference.getLatencySamples();
        std::vector<FxChain> chains;
        for (int m = 0; m < numFxModules; ++m)
        {
            FxChain c;
            c.add (m);
            chains.push_back (c);
        }
        FxChain all;
        for (int m = numFxModules; --m >= 0;)
            all.add (m);
        chains.push_back (all);
        chains.push_back (FxChain {});
        chains.push_back (FxChain::fromString ("clip1 comp1 eq clip2 drive2 reverb drive1 comp2"));

        auto p = engineParams (true, true, true);
        p.bodyGain = 2.0f;
        p.revMix = 0.3f;
        p.comp[0].thresholdDb = -30.0f;
        p.comp[1].ratio = 10.0f;
        p.chainClip[0].driveDb = 12.0f;
        p.drive2.amount = 0.6f;
        for (const auto& c : chains)
        {
            p.chain = c;
            DrumEngine e;
            e.prepare (sr, 512);
            e.setParams (p);
            const auto hit = renderHit (p, tables, sr);
            if (! allFinite (hit) || hit.getMagnitude (0, hit.getNumSamples()) < 1.0e-3f || e.getLatencySamples() != latency)
                fail ("chain \"" + c.toString() + "\": peak " + juce::String (hit.getMagnitude (0, hit.getNumSamples()))
                      + ", latency " + juce::String (e.getLatencySamples()) + " (expected " + juce::String (latency) + ")");
        }
        std::printf ("FxChain: %d chains render finite at a fixed latency of %d samples (48 kHz)\n", (int) chains.size(), latency);
    }

    // Order matters: compress -> clip and clip -> compress differ.
    {
        auto p = engineParams (true, false, false);
        p.bodyGain = 2.0f;
        p.comp[0] = { true, -24.0f, 8.0f, 20.0f, 100.0f, 6.0f, 12.0f, 1.0f };
        p.chainClip[0] = { true, 1, 6.0f, -3.0f };
        p.chain = FxChain::fromString ("comp1 clip1");
        const auto a = renderHit (p, tables, sr);
        p.chain = FxChain::fromString ("clip1 comp1");
        const auto b = renderHit (p, tables, sr);
        float diff = 0.0f;
        for (int i = 0; i < juce::jmin (a.getNumSamples(), b.getNumSamples()); ++i)
            diff = juce::jmax (diff, std::abs (a.getSample (0, i) - b.getSample (0, i)));
        // The clipper last caps the peak at its ceiling; first, the compressor's makeup can go past it.
        const float ceiling = juce::Decibels::decibelsToGain (-3.0f);
        if (diff < 0.05f || a.getMagnitude (0, a.getNumSamples()) > ceiling * 1.01f)
            fail ("comp -> clip vs clip -> comp: max difference " + juce::String (diff) + ", comp -> clip peak "
                  + juce::String (a.getMagnitude (0, a.getNumSamples())));
    }

    // A chain change while sound is going: no step bigger than the signal's own.
    {
        auto p = engineParams (true, false, false);
        p.lengthMs = 2000.0f;
        p.chain = FxChain::defaults();
        DrumEngine e;
        e.prepare (sr, 256);
        e.setTables (engineTables (true)); // flat: the hit holds, so the change lands mid-sound
        e.setParams (p);
        juce::AudioBuffer<float> block (2, 256);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        float maxStep = 0.0f, maxSignalStep = 0.0f, last = 0.0f;
        for (int b = 0; b < 200; ++b)
        {
            if (b == 60)
            {
                p.chain = FxChain::fromString ("comp1 drive2 clip1 drive1 eq reverb");
                e.setParams (p);
            }
            block.clear();
            e.process (block, midi);
            midi.clear();
            for (int i = 0; i < 256; ++i)
            {
                const float v = block.getSample (0, i);
                const float step = std::abs (v - last);
                (b >= 55 && b < 70 ? maxStep : maxSignalStep) = juce::jmax (b >= 55 && b < 70 ? maxStep : maxSignalStep, step);
                last = v;
            }
        }
        if (maxStep > juce::jmax (0.02f, maxSignalStep * 1.5f))
            fail ("chain change mid-stream jumps by " + juce::String (maxStep) + " (the signal's own steps are " + juce::String (maxSignalStep) + ")");
    }

    // The order is part of the patch: session, preset file and undo.
    {
        DrumFoundryProcessor proc;
        const auto order = FxChain::fromString ("clip1 comp1 eq comp2 reverb");
        proc.setFxChain (order);
        juce::MemoryBlock session;
        proc.getStateInformation (session);
        DrumFoundryProcessor recalled;
        recalled.setStateInformation (session.getData(), (int) session.getSize());
        if (recalled.getFxChain() != order || recalled.readParams().chain != order)
            fail ("session recall lost the chain: \"" + recalled.getFxChain().toString() + "\"");

        // An old session (no fxChain property) gets the chain it always had.
        auto tree = proc.createStateTree();
        tree.removeProperty (DrumFoundryProcessor::fxChainId, nullptr);
        tree.setProperty (DrumFoundryProcessor::formatVersionId, 5, nullptr);
        recalled.loadStateTree (tree);
        if (recalled.getFxChain() != FxChain::defaults())
            fail ("an old session didn't get the default chain: \"" + recalled.getFxChain().toString() + "\"");

        // Undo puts the order back.
        auto& undo = proc.getUndoHistory();
        undo.clear();
        undo.noteUserActivity();
        proc.setFxChain (FxChain::fromString ("drive2 reverb"));
        undo.poll (false);
        if (! undo.canUndo() || ! undo.undo() || proc.getFxChain() != order)
            fail ("undo of a chain change: \"" + proc.getFxChain().toString() + "\"");
    }

    // No reverb in the chain: no tail, whatever its Mix.
    {
        auto p = engineParams (true, false, false);
        p.revMix = 0.8f;
        p.revGate = false;
        p.chain = FxChain::fromString ("drive1 eq");
        if (Reverb::tailSeconds (p) != 0.0)
            fail ("a chain without the reverb still has a reverb tail");
    }

    // The output stage's Comp: it compresses, and its shading capture is input x makeup.
    {
        auto p = engineParams (true, false, false);
        p.bodyGain = 2.0f;
        p.clipMode = clipModeComp;
        p.outComp = { true, -20.0f, 8.0f, 1.0f, 80.0f, 6.0f, 6.0f, 1.0f };
        juce::AudioBuffer<float> pre;
        const auto out = renderHit (p, tables, sr, 60, 1.0f, 0x5EEDu, {}, &pre);
        // Over the body (a fast attack still lets the very first peak through).
        const int from = (int) (0.02 * sr), to = juce::jmin (out.getNumSamples(), (int) (0.2 * sr));
        const float outRms = out.getRMSLevel (0, from, to - from), preRms = pre.getRMSLevel (0, from, to - from);
        const float reductionDb = juce::Decibels::gainToDecibels (outRms / juce::jmax (1.0e-9f, preRms));
        if (! allFinite (out) || reductionDb > -6.0f)
            fail ("output Comp: only " + juce::String (reductionDb, 1) + " dB of reduction over the body");
        else
            std::printf ("output Comp: %.1f dB of reduction over the body (20-200 ms) against in x makeup\n", (double) reductionDb);
    }
    return failures;
}

/** The reverb's engines: Cloud (Cloud Seed) and Classic render finite, repeatable,
    block-size independent tails of about the same loudness; Decay is the tail's RT60;
    a patch from before format 8 loads on Classic and a new one on Cloud. */
int checkReverbEngines()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: %s\n", msg.toRawUTF8()); ++failures; };
    constexpr double sr = 48000.0;
    const auto tables = engineTables (false);

    auto base = engineParams (true, false, true);
    base.lengthMs = 200.0f;
    base.revMix = 0.5f;
    base.revGate = false;
    base.revDecayMs = 1500.0f;
    base.revSize = 0.6f;
    base.clipMode = 0;
    auto dry = base;
    dry.revMix = 0.0f;
    const auto dryHit = renderHit (dry, tables, sr);

    // How much the tail rings: its spectrum's peaks over a third-octave average (dB, the
    // 95th percentile over 200 Hz - 6 kHz). Recirculating delays ring at their modes, a
    // metallic sound; a room of noise shouldn't.
    const auto ringing = [sr] (const std::vector<float>& wet)
    {
        constexpr int order = 15, size = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<float> data ((size_t) (2 * size), 0.0f);
        for (int i = 0; i < size && i < (int) wet.size(); ++i)
            data[(size_t) i] = wet[(size_t) i] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size));
        fft.performFrequencyOnlyForwardTransform (data.data());
        const auto bin = [sr] (double hz) { return (int) (hz * size / sr); };
        std::vector<double> db ((size_t) (size / 2));
        for (int k = 1; k < size / 2; ++k)
            db[(size_t) k] = 20.0 * std::log10 (data[(size_t) k] + 1.0e-12);
        std::vector<double> over;
        for (int k = bin (200.0); k < bin (6000.0); ++k)
        {
            const int lo = juce::jmax (1, (int) (k / 1.12)), hi = juce::jmin (size / 2 - 1, (int) (k * 1.12));
            double avg = 0.0;
            for (int j = lo; j <= hi; ++j)
                avg += db[(size_t) j];
            over.push_back (db[(size_t) k] - avg / (hi - lo + 1));
        }
        std::sort (over.begin(), over.end());
        return over.empty() ? 0.0 : over[(size_t) (0.95 * (double) (over.size() - 1))];
    };

    float levels[3] {};
    double rings[3] {};
    for (int engine : { (int) revEngineClassic, (int) revEngineCloud, (int) revEngineNatural })
    {
        auto p = base;
        p.revEngine = engine;
        const auto a = renderHit (p, tables, sr), b = renderHit (p, tables, sr);
        const auto name = revEngineNames[engine];
        if (! allFinite (a) || a.getNumSamples() != b.getNumSamples())
        {
            fail (name + " reverb: not finite, or renders differ in length");
            continue;
        }
        float diff = 0.0f;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < a.getNumSamples(); ++i)
                diff = juce::jmax (diff, std::abs (a.getSample (c, i) - b.getSample (c, i)));
        if (diff > 0.0f)
            fail (name + " reverb isn't repeatable (renders differ by " + juce::String (diff) + ")");

        // The wet part: the render minus the dry one, 300 ms to 1 s (the tail, past the hit).
        const int from = (int) (0.3 * sr), to = juce::jmin (a.getNumSamples(), dryHit.getNumSamples() + (int) (2.0 * sr), (int) (1.0 * sr));
        double e = 0.0;
        std::vector<float> wet;
        for (int i = from; i < to; ++i)
        {
            const float d = i < dryHit.getNumSamples() ? dryHit.getSample (0, i) : 0.0f;
            const float w = a.getSample (0, i) - d;
            e += (double) w * w;
            wet.push_back (w);
        }
        levels[engine] = (float) (10.0 * std::log10 (e / juce::jmax (1, to - from) + 1.0e-20));
        rings[engine] = ringing (wet);

        // RT60: the wet level 0.4 s apart, early in the tail.
        const auto windowDb = [&] (double t0)
        {
            double acc = 0.0;
            const int s0 = (int) (t0 * sr), s1 = juce::jmin (a.getNumSamples(), s0 + (int) (0.05 * sr));
            for (int i = s0; i < s1; ++i)
                acc += (double) a.getSample (0, i) * a.getSample (0, i);
            return 10.0 * std::log10 (acc / juce::jmax (1, s1 - s0) + 1.0e-20);
        };
        const double slope = (windowDb (0.4) - windowDb (0.8)) / 0.4; // dB per second
        const double rt60 = slope > 0.0 ? 60.0 / slope : 0.0;
        if (engine != revEngineClassic && (rt60 < 1.2 || rt60 > 1.8))
            fail (name + " reverb: Decay 1.5 s measures an RT60 of " + juce::String (rt60, 2) + " s");
        std::printf ("reverb %s: tail level %.1f dB, RT60 %.2f s (Decay 1.5 s), ringing %.1f dB, repeatable\n", name.toRawUTF8(),
                     (double) levels[engine], rt60, rings[engine]);

        // Block sizes.
        p.lengthMs = 400.0f;
        const auto big = renderBlocks (p, tables, {}, sr, (int) (0.8 * sr), 300, { 4096 });
        const auto small = renderBlocks (p, tables, {}, sr, (int) (0.8 * sr), 300, { 1, 37, 64, 65, 500, 13, 128 });
        float maxDiff = 0.0f;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < big.getNumSamples(); ++i)
                maxDiff = juce::jmax (maxDiff, std::abs (big.getSample (c, i) - small.getSample (c, i)));
        if (maxDiff > 1.0e-4f)
            fail (name + " reverb depends on the block size (max diff " + juce::String (maxDiff) + ")");
    }
    for (int engine : { (int) revEngineCloud, (int) revEngineNatural })
        if (std::abs (levels[engine] - levels[revEngineClassic]) > 3.0f)
            fail (revEngineNames[engine] + " and Classic tails differ by " + juce::String (levels[engine] - levels[revEngineClassic], 1) + " dB in level");
    if (! (rings[revEngineNatural] < juce::jmin (rings[revEngineClassic], rings[revEngineCloud])))
        fail ("the Natural reverb rings as much as the others (" + juce::String (rings[revEngineNatural], 1) + " dB)");

    // Where ringing is worst: a small, short room (the DnB presets' kind), 30-330 ms.
    {
        double small[3] {};
        auto room = base;
        room.revSize = 0.25f;
        room.revDecayMs = 600.0f;
        room.lengthMs = 60.0f;
        auto roomDry = room;
        roomDry.revMix = 0.0f;
        const auto roomDryHit = renderHit (roomDry, tables, sr);
        for (int engine : { (int) revEngineClassic, (int) revEngineCloud, (int) revEngineNatural })
        {
            room.revEngine = engine;
            const auto hit = renderHit (room, tables, sr);
            std::vector<float> wet;
            for (int i = (int) (0.03 * sr); i < juce::jmin (hit.getNumSamples(), (int) (0.33 * sr)); ++i)
                wet.push_back (hit.getSample (0, i) - (i < roomDryHit.getNumSamples() ? roomDryHit.getSample (0, i) : 0.0f));
            small[engine] = ringing (wet);
        }
        std::printf ("reverb ringing, small room (Size 25%%, Decay 600 ms): Classic %.1f dB, Cloud %.1f dB, Natural %.1f dB\n",
                     small[0], small[1], small[2]);
        if (! (small[revEngineNatural] < juce::jmin (small[revEngineClassic], small[revEngineCloud])))
            fail ("the Natural reverb rings as much as the others in a small room (" + juce::String (small[revEngineNatural], 1) + " dB)");
    }

    // Old patches load on Classic; new ones (and factory presets) on Cloud.
    {
        DrumFoundryProcessor proc;
        auto tree = proc.createStateTree();
        tree.setProperty (DrumFoundryProcessor::formatVersionId, 7, nullptr);
        proc.loadStateTree (tree);
        const int old = juce::roundToInt (proc.getState().getRawParameterValue (pid::revEngine)->load());
        DrumFoundryProcessor fresh;
        const int now = juce::roundToInt (fresh.getState().getRawParameterValue (pid::revEngine)->load());
        if (old != revEngineClassic || now != revEngineCloud)
            fail ("reverb engine migration: format 7 -> " + juce::String (old) + ", new -> " + juce::String (now));
    }
    return failures;
}

/** The plug-in's Natural reverb builds its rooms on a background thread: a room arrives,
    the tail is there, and a new Decay builds a new room. */
int checkNaturalLive()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: Natural reverb, live: %s\n", msg.toRawUTF8()); ++failures; };
    constexpr double sr = 48000.0;
    constexpr int block = 512;

    DrumFoundryProcessor proc;
    proc.prepareToPlay (sr, block);
    setParam (proc, pid::revOn, 1.0f);
    setParam (proc, pid::revEngine, (float) revEngineNatural);
    setParam (proc, pid::revMix, 0.5f);
    setParam (proc, pid::revGate, 0.0f);
    setParam (proc, pid::length, 120.0f);

    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    // Tail energy (from..to seconds after a hit), after giving the room time to arrive.
    const auto tailDb = [&] (double from = 0.3, double to = 0.8)
    {
        for (int i = 0; i < 50; ++i) // ~0.5 s of blocks, with the room thread running
        {
            proc.processBlock (buf, midi);
            juce::Thread::sleep (10);
        }
        proc.triggerAudition (0.9f);
        double e = 0.0;
        for (int b = 0; b * block < (int) (to * sr); ++b)
        {
            proc.processBlock (buf, midi);
            if (b * block >= (int) (from * sr))
                for (int i = 0; i < block; ++i)
                    e += (double) buf.getSample (0, i) * buf.getSample (0, i);
        }
        return 10.0 * std::log10 (e + 1.0e-20);
    };

    setParam (proc, pid::revDecay, 400.0f);
    const double shortRoom = tailDb();
    setParam (proc, pid::revDecay, 2500.0f);
    const double longRoom = tailDb();
    if (! (longRoom > -80.0))
        fail ("no tail (" + juce::String (longRoom, 1) + " dB)");
    if (! (shortRoom > -120.0))
        fail ("the first hit after a new room has no tail (" + juce::String (shortRoom, 1) + " dB)");
    if (! (longRoom > shortRoom + 10.0))
        fail ("Decay 2.5 s isn't a longer room than 0.4 s (" + juce::String (longRoom, 1) + " vs " + juce::String (shortRoom, 1) + " dB)");
    if (failures == 0)
        std::printf ("Natural reverb, live: rooms built off the audio thread; tail 300-800 ms at Decay 0.4 s %.1f dB, 2.5 s %.1f dB\n",
                     shortRoom, longRoom);
    return failures;
}

int checkPreClipCapture()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: %s\n", msg.toRawUTF8()); ++failures; };
    constexpr double sr = 48000.0;
    const auto tables = engineTables (false);

    auto p = engineParams (true, false, true);
    p.bodyGain = 2.0f;           // +6 dB: well into the clipper
    p.clipMode = Clipper::off;
    juce::AudioBuffer<float> pre;
    const auto out = renderHit (p, tables, sr, 60, 1.0f, 0x5EEDu, {}, &pre);
    float worst = 0.0f;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < out.getNumSamples(); ++i)
            worst = juce::jmax (worst, std::abs (out.getSample (c, i) - pre.getSample (c, i)));
    if (pre.getNumSamples() != out.getNumSamples() || worst > 1.0e-6f)
        fail ("pre-clip capture with the clipper Off differs from the output by " + juce::String (worst));

    p.clipMode = Clipper::hard;
    p.clipCeilingDb = -6.0f;
    p.clipDriveDb = 0.0f;
    const auto hard = renderHit (p, tables, sr, 60, 1.0f, 0x5EEDu, {}, &pre);
    const float ceiling = juce::Decibels::decibelsToGain (-6.0f);
    const float outPeak = hard.getMagnitude (0, hard.getNumSamples());
    const float prePeak = pre.getMagnitude (0, pre.getNumSamples());
    // Where the signal is well under the ceiling (the tail), the two match.
    float tailDiff = 0.0f;
    for (int i = hard.getNumSamples() / 2; i < hard.getNumSamples(); ++i)
        if (std::abs (pre.getSample (0, i)) < 0.5f * ceiling)
            tailDiff = juce::jmax (tailDiff, std::abs (hard.getSample (0, i) - pre.getSample (0, i)));
    if (outPeak > ceiling * 1.001f || prePeak < ceiling * 1.5f || tailDiff > 1.0e-3f)
        fail ("pre-clip capture, Hard at -6 dB: output peak " + juce::String (outPeak, 4) + ", capture peak "
              + juce::String (prePeak, 4) + ", tail difference " + juce::String (tailDiff));
    else
        std::printf ("pre-clip capture: Off matches the output (max diff %.1e); Hard at -6 dB: out %.3f, in %.3f\n",
                     (double) worst, (double) outPeak, (double) prePeak);
    return failures;
}

int checkPitchAndLevels()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: pitch / level: %s\n", msg.toRawUTF8()); ++failures; };

    constexpr double sr = 48000.0;
    const auto defTables = engineTables (false);
    auto flatPitch = [] (float st, float bodyLevel = 1.0f, float noiseLevel = 1.0f)
    {
        auto t = engineTables (true);
        t.data[(size_t) envPitch].fill (st);
        t.data[(size_t) envBody].fill (bodyLevel);
        t.data[(size_t) envNoise].fill (noiseLevel);
        return t;
    };
    juce::String report;

    // --- +12 st = 2 x Tune -------------------------------------------------------------------
    {
        const auto octave = flatPitch (12.0f);
        for (int b : { (int) bodyClassic, (int) bodyKick, (int) body808, (int) bodyFM, -1 })
        {
            auto p = engineParams (b >= 0, false, false);
            p.tuneHz = 100.0f;
            p.bodyStyle = juce::jmax (0, b);
            p.bodyShape = 0.0f;
            p.otGain = 0.0f;
            p.modes = 0.0f;
            if (b < 0)
            {
                p.layers[0].on = true;
                p.layers[0].source = layerSine;
                p.layers[0].env = layerEnvFlat;
                p.layers[0].pitchEnv = layerEnvBody; // coupled to the Body pitch curve
            }
            const auto hit = renderVoice (p, octave, sr, (int) (0.4 * sr));
            const double hz = zeroCrossingHz (hit, 0, sr, (int) (0.05 * sr), (int) (0.35 * sr));
            const auto name = b >= 0 ? bodyStyleNames[b] : juce::String ("sine layer");
            report << name << " " << juce::String (hz, 2) << " Hz  ";
            if (std::abs (hz / 200.0 - 1.0) > 0.002)
                fail (name + " with the pitch curve at +12 st, Tune 100 Hz: " + juce::String (hz, 2) + " Hz (expected 200)");
        }
        std::printf ("pitch curve +12 st at Tune 100 Hz: %s\n", report.toRawUTF8());
        report.clear();
    }

    // --- extreme curves: finite and bounded, every style and layer -------------------------------
    {
        const CurveEnvelope wild ({ { 0.0f, 100.0f, -0.7f }, { 0.15f, -40.0f, 0.4f }, { 0.3f, 120.0f, 0.0f },
                                    { 0.45f, -120.0f, 0.0f }, { 0.6f, 60.0f, -0.9f }, { 1.0f, 0.0f, 0.0f } },
                                  envelopeRange (envPitch));
        auto tables = defTables;
        tables.bake (envPitch, wild);
        if (std::abs (tables.lookup (envPitch, 0.0f) - 100.0f) > 1.0e-3f || std::abs (tables.lookup (envPitch, 0.45f) + 120.0f) > 0.5f)
            fail ("the extreme test curve didn't bake as drawn");

        int renders = 0;
        float worstPeak = 0.0f;
        for (double rate : { 44100.0, 96000.0 })
            for (int b = 0; b < numBodyStyles - 1; ++b)
                for (float tune : { 20.0f, 185.0f, 16000.0f })
                    for (int note : { 0, 60, 127 })
                        for (float shape : { 0.0f, 1.0f })
                        {
                            DrumParams p;
                            p.clipMode = 0;
                            p.driveAmount = 0.0f;
                            p.bodyStyle = b;
                            p.tuneHz = tune;
                            p.bodyShape = shape;
                            p.modes = 1.0f;
                            p.otGain = 1.0f;
                            p.otRatio = shape > 0.5f ? 4.0f : 0.5f;
                            p.noiseStyle = noiseMetallic;
                            p.clickType = clickClap;
                            p.clickTone = 12000.0f;
                            auto& l = p.layers[0];
                            l.on = true;
                            l.source = layerSine;
                            l.tuneSt = 24.0f;
                            auto& m = p.layers[1];
                            m.on = true;
                            m.source = layerNoise;
                            m.noiseStyle = noiseMetallic;
                            m.tuneSt = 24.0f;
                            m.color = 1.0f;
                            const auto hit = renderVoice (p, tables, rate, (int) (0.5 * rate), { 512 }, 0x5EEDu, note);
                            const float peak = peakOf (hit);
                            worstPeak = juce::jmax (worstPeak, peak);
                            ++renders;
                            if (! allFinite (hit) || peak > 12.0f)
                                fail ("extreme pitch curve, " + bodyStyleNames[b] + " Tune " + juce::String (tune) + " note "
                                      + juce::String (note) + " @" + juce::String (rate) + ": peak " + juce::String (peak));
                        }
        std::printf ("extreme pitch curve (+100/-40/+120/-120/+60 st) over %d renders of every style + sine and Metallic "
                     "layers (+24 st), Tune 20 Hz..16 kHz, notes 0..127: finite, peak %.2f\n", renders, worstPeak);
    }

    // --- Tune 16 kHz: the waveshaped styles turn into clean sines; the 5 Hz floor -----------------
    {
        const auto flat = flatPitch (0.0f);
        for (int b : { (int) bodyClassic, (int) body909, (int) bodyKick, (int) bodyFM, (int) bodyRound, (int) bodyTaper,
                       (int) bodyHard, (int) bodyFold, (int) bodyTonal })
        {
            auto p = engineParams (true, false, false);
            p.bodyStyle = b;
            p.tuneHz = 16000.0f;
            p.bodyShape = 1.0f;
            p.otGain = 0.0f;
            p.modes = 0.0f;
            const auto hit = renderVoice (p, flat, sr, (int) (0.2 * sr));

            // Share of the power at 16 kHz: a sine has it all, an aliased square ~81%.
            const int start = (int) (0.05 * sr), n = 4800;
            double re = 0.0, im = 0.0, power = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double x = hit.getSample (0, start + i);
                const double w = juce::MathConstants<double>::twoPi * 16000.0 / sr * i;
                re += x * std::cos (w);
                im += x * std::sin (w);
                power += x * x;
            }
            const double share = 2.0 * (re * re + im * im) / ((double) n * power + 1.0e-30);
            report << bodyStyleNames[b] << " " << juce::String (share, 4) << "  ";
            if (! allFinite (hit) || share < 0.995)
                fail (bodyStyleNames[b] + " at Tune 16 kHz, Shape 1: only " + juce::String (share, 4) + " of the power at 16 kHz");
        }

        auto p = engineParams (true, false, false);
        p.bodyStyle = bodyKick;
        p.tuneHz = 20.0f;
        p.lengthMs = 2000.0f;
        // Under the old 5 Hz floor: Tune 20 Hz at -36 st plays 2.5 Hz (checkPitchFloor() goes to 0 Hz).
        const auto hit = renderVoice (p, flatPitch (-36.0f), sr, (int) (2.0 * sr));
        const double hz = zeroCrossingHz (hit, 0, sr, 0, hit.getNumSamples());
        report << "| Tune 20 Hz at -36 st: " << juce::String (hz, 2) << " Hz";
        if (std::abs (hz - 2.5) > 0.2)
            fail ("the pitch floor: Tune 20 Hz at -36 st plays " + juce::String (hz, 2) + " Hz (expected 2.5)");
        std::printf ("Tune 16 kHz, Shape 1, share of power at 16 kHz: %s\n", report.toRawUTF8());
        report.clear();
    }

    // --- envelope levels up to 4: +12 dB, through every layer ----------------------------------------
    {
        struct Case { const char* name; std::function<void (DrumParams&)> setup; };
        const Case cases[] {
            { "Classic", [] (DrumParams& p) { p.bodyOn = true; } },
            { "FM", [] (DrumParams& p) { p.bodyOn = true; p.bodyStyle = bodyFM; p.bodyShape = 0.6f; } },
            { "Kick", [] (DrumParams& p) { p.bodyOn = true; p.bodyStyle = bodyKick; p.bodyShape = 0.8f; } },
            { "Wires (Color 0.8)", [] (DrumParams& p) { p.noiseOn = true; p.noiseColor = 0.8f; } },
            { "sine layer", [] (DrumParams& p) { p.layers[0].on = true; p.layers[0].source = layerSine; p.layers[0].env = layerEnvBody; } },
            { "noise layer", [] (DrumParams& p)
              {
                  p.layers[0].on = true;
                  p.layers[0].source = layerNoise;
                  p.layers[0].env = layerEnvNoise;
                  p.layers[0].color = 1.0f;
              } },
        };
        for (const auto& c : cases)
        {
            auto p = engineParams (false, false, false);
            c.setup (p);
            const auto unity = renderVoice (p, flatPitch (0.0f, 1.0f, 1.0f), sr, (int) (0.3 * sr));
            const auto hot = renderVoice (p, flatPitch (0.0f, 4.0f, 4.0f), sr, (int) (0.3 * sr));
            float worst = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < unity.getNumSamples(); ++i)
                    worst = juce::jmax (worst, std::abs (hot.getSample (ch, i) - 4.0f * unity.getSample (ch, i)));
            const int end = (int) (0.25 * sr);
            const float db = juce::Decibels::gainToDecibels (rmsIn (hot, 0, end) / rmsIn (unity, 0, end));
            report << c.name << " " << juce::String (db, 2) << " dB  ";
            if (std::abs (db - 12.04f) > 0.01f || worst > 1.0e-4f * peakOf (hot))
                fail (juce::String (c.name) + " at envelope level 4: " + juce::String (db, 3) + " dB over level 1 (expected +12.04), "
                      "differs from 4 x the level-1 hit by " + juce::String (worst));
        }
        std::printf ("envelope at 4 against 1 (voice output): %s\n", report.toRawUTF8());
    }
    return failures;
}

//==============================================================================
// Per-layer curves (Noise / Click pitch, Click level), extra Body layers, phase
// invert, and pitches down to 0 Hz

/** Centre of the spectrum of the L+R mix of `hits`: the frequency with half the power
    below it, from the power spectra of every whole 8192-sample window (Hann) of each hit.
    Unlike a power-weighted centroid it doesn't lean on the tail up at Nyquist, which a
    transposed band loses more of. Several hits (seeds) keep the estimate steady. */
double spectralCentre (const std::vector<juce::AudioBuffer<float>>& hits, double sr)
{
    constexpr int order = 13, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> data ((size_t) size * 2);
    std::vector<double> power ((size_t) size / 2, 0.0);
    for (const auto& b : hits)
        for (int start = 0; start + size <= b.getNumSamples(); start += size)
        {
            std::fill (data.begin(), data.end(), 0.0f);
            for (int i = 0; i < size; ++i)
            {
                const float w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size);
                data[(size_t) i] = 0.5f * (b.getSample (0, start + i) + b.getSample (1, start + i)) * w;
            }
            fft.performFrequencyOnlyForwardTransform (data.data());
            for (int k = 1; k < size / 2; ++k)
                power[(size_t) k] += (double) data[(size_t) k] * (double) data[(size_t) k];
        }
    double total = 0.0;
    for (auto pw : power)
        total += pw;
    double below = 0.0;
    for (int k = 1; k < size / 2; ++k)
    {
        const double pw = power[(size_t) k];
        if (below + pw >= 0.5 * total && pw > 0.0)
            return ((double) k - 0.5 + (0.5 * total - below) / pw) * sr / size;
        below += pw;
    }
    return 0.0;
}

/** The Noise and Click pitch curves and the Click level curve, on the core
    layers and on extra layers; extra Body layers against the Body layer and
    the pitch they play; phase invert; pitches down to 0 Hz; neutral defaults;
    block-size independence with all of it. */
int checkLayerCurves()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: layer curves: %s\n", msg.toRawUTF8()); ++failures; };

    constexpr double sr = 48000.0;
    const auto defTables = engineTables (false);
    const auto flatTables = engineTables (true);
    auto with = [] (EnvTables t, int env, float value)
    {
        t.data[(size_t) env].fill (value);
        return t;
    };
    auto identical = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        return a.getNumSamples() == b.getNumSamples() && firstDifference (a, b) < 0;
    };
    const auto sine220 = makeSineSample (220.0, 44100.0, 1.0, false);
    SampleSet set;
    for (auto& s : set.slots)
        s = sine220.get();

    // --- neutral defaults: the default Click curves leave a Click or Noise layer on the Click
    //     envelope exactly as it is on Flat (its own decay / full level, no transposition) ------------
    {
        auto layer = [&] (int source, int env)
        {
            auto p = engineParams (false, false, false);
            auto& l = p.layers[0];
            l.on = true;
            l.source = source;
            l.env = l.pitchEnv = env; // both curves from the same layer
            l.clickType = clickRim;
            l.noiseStyle = noiseMetallic;
            l.gain = 0.5f;
            return renderVoice (p, defTables, sr, (int) (0.1 * sr));
        };
        if (! identical (layer (layerClick, layerEnvClick), layer (layerClick, layerEnvFlat)))
            fail ("a Click layer on the default Click curves differs from one on Flat");
        if (! identical (layer (layerNoise, layerEnvClick), layer (layerNoise, layerEnvFlat)))
            fail ("a Noise layer on the default Click curves differs from one on Flat");

        // The default tables are the neutral ones: Noise / Click pitch 0 and Click level 1.
        bool neutral = true;
        for (int e : { (int) envNoisePitch, (int) envClickPitch, (int) envClick })
            for (float v : defTables.data[(size_t) e])
                neutral = neutral && juce::exactlyEqual (v, e == envClick ? 1.0f : 0.0f);
        if (! neutral)
            fail ("the default Noise pitch / Click pitch / Click curves aren't flat 0 / 0 / 1");
        std::printf ("layer curves: default Noise / Click curves are neutral (Click and Noise layers on Click = on Flat, bit for bit)\n");
    }

    // --- Noise pitch: +12 st doubles the noise band, Metallic's bank and a sample's rate ------------
    {
        juce::String report;
        for (int style : { (int) noiseWhite, (int) noiseWires, (int) noiseMetallic, (int) noiseCrackle })
        {
            auto p = engineParams (false, true, false);
            p.lengthMs = 1500.0f;
            p.noiseStyle = style;
            p.noiseGain = 0.2f;
            p.noiseColor = 0.0f;
            p.noiseCouple = 0.0f;
            p.noiseHP = style == noiseMetallic ? 20.0f : 500.0f;
            p.noiseLP = style == noiseMetallic ? 6000.0f : 2000.0f;
            p.noiseGrit = style == noiseCrackle ? 0.3f : 0.0f;
            std::vector<juce::AudioBuffer<float>> at0, at12;
            for (uint32_t seed = 1; seed <= 4; ++seed)
            {
                at0.push_back (renderVoice (p, flatTables, sr, (int) (1.4 * sr), { 512 }, seed));
                at12.push_back (renderVoice (p, with (flatTables, envNoisePitch, 12.0f), sr, (int) (1.4 * sr), { 512 }, seed));
            }
            const double ratio = spectralCentre (at12, sr) / spectralCentre (at0, sr);
            report << noiseStyleNames[style] << " x" << juce::String (ratio, 3) << "  ";
            // (Within 5%: the filters' bilinear warping bends the transposed band a little lower.)
            if (! allFinite (at12[0]) || std::abs (ratio - 2.0) > 0.1)
                fail ("Noise " + noiseStyleNames[style] + " at +12 st: spectrum centre x" + juce::String (ratio, 3) + " (expected x2)");
        }

        auto p = engineParams (false, true, false);
        p.noiseStyle = noiseSample;
        p.noiseHP = 20.0f;
        p.noiseLP = 20000.0f;
        p.noiseGain = 0.5f;
        const auto hit = renderVoice (p, with (flatTables, envNoisePitch, 12.0f), sr, (int) (0.3 * sr), { 512 }, 0x5EEDu, 60, set);
        const double hz = zeroCrossingHz (hit, 0, sr, (int) (0.01 * sr), (int) (0.25 * sr));
        report << "| Sample 220 Hz at +12 st: " << juce::String (hz, 1) << " Hz";
        if (std::abs (hz / 440.0 - 1.0) > 0.005)
            fail ("Noise Sample at +12 st plays " + juce::String (hz, 1) + " Hz (expected 440)");
        std::printf ("noise pitch +12 st, spectrum centre (half-power frequency): %s\n", report.toRawUTF8());
    }

    // --- Click pitch doubles the Stick / Pop / Sample tone; the Click curve scales the level ----------
    {
        juce::String report;
        auto click = [&] (int type, const EnvTables& t, float tone = 1000.0f)
        {
            auto p = engineParams (false, false, true);
            p.clickType = type;
            p.clickTone = tone;
            p.clickLengthMs = 40.0f;
            p.clickGain = 0.5f;
            return renderVoice (p, t, sr, (int) (0.1 * sr), { 512 }, 0x5EEDu, 60, set);
        };
        for (float st : { 0.0f, 12.0f, -12.0f })
        {
            const double hz = zeroCrossingHz (click (clickStick, with (flatTables, envClickPitch, st)), 0, sr,
                                              (int) (0.001 * sr), (int) (0.02 * sr));
            const double expected = 1000.0 * std::exp2 (st / 12.0);
            report << "Stick " << juce::String (st, 0) << " st " << juce::String (hz, 1) << " Hz  ";
            if (std::abs (hz / expected - 1.0) > 0.005)
                fail ("Stick at click pitch " + juce::String (st) + " st: " + juce::String (hz, 1) + " Hz, expected " + juce::String (expected, 1));
        }

        // Pop sweeps down as it decays; an octave up, its 2k-th rising zero crossing lands on the k-th at 0 st.
        auto crossings = [&] (const juce::AudioBuffer<float>& hit)
        {
            std::vector<double> t;
            for (int i = (int) (0.0005 * sr); i < (int) (0.03 * sr); ++i)
            {
                const float a = hit.getSample (0, i - 1), b = hit.getSample (0, i);
                if (a < 0.0f && b >= 0.0f)
                    t.push_back ((double) (i - 1) + (double) (a / (a - b)));
            }
            return t;
        };
        const auto pop0 = crossings (click (clickPop, flatTables, 800.0f));
        const auto pop12 = crossings (click (clickPop, with (flatTables, envClickPitch, 12.0f), 800.0f));
        double worstMs = 1.0e9;
        if (pop0.size() >= 6 && pop12.size() >= 12)
        {
            worstMs = 0.0;
            for (size_t k = 1; k <= 6; ++k)
                worstMs = juce::jmax (worstMs, std::abs (pop12[2 * k - 1] - pop0[k - 1]) * 1000.0 / sr);
        }
        report << "Pop +12: within " << juce::String (worstMs, 3) << " ms  ";
        if (worstMs > 0.03)
            fail ("Pop at click pitch +12 st isn't an octave up (crossings off by " + juce::String (worstMs, 3) + " ms)");

        const double smpHz = zeroCrossingHz (click (clickSample, with (flatTables, envClickPitch, 12.0f), 300.0f), 0, sr,
                                             (int) (0.002 * sr), (int) (0.035 * sr));
        report << "Sample 220 Hz +12: " << juce::String (smpHz, 1) << " Hz  ";
        if (std::abs (smpHz / 440.0 - 1.0) > 0.01)
            fail ("Click Sample at click pitch +12 st plays " + juce::String (smpHz, 1) + " Hz (expected 440)");

        // Every type stays finite and audible over the whole range, and a moving curve doesn't blow up.
        EnvTables sweep = flatTables;
        sweep.bake (envClickPitch, CurveEnvelope ({ { 0.0f, 120.0f, 0.0f }, { 0.02f, -120.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } },
                                                  envelopeRange (envClickPitch)));
        for (int type = 0; type < numClickTypes; ++type)
            for (const EnvTables* t : std::initializer_list<const EnvTables*> { &sweep, &flatTables })
            {
                const auto hit = click (type, *t, 3500.0f);
                if (! allFinite (hit) || peakOf (hit) > 8.0f || peakOf (hit) < 1.0e-3f)
                    fail ("click " + clickTypeNames[type] + " under a +-120 st click pitch sweep: peak " + juce::String (peakOf (hit)));
            }

        // Click level 0.5 halves every type (it multiplies the click's own decay).
        float worstDb = 0.0f;
        for (int type = 0; type < numClickTypes; ++type)
        {
            const auto full = click (type, flatTables, 2000.0f);
            const auto half = click (type, with (flatTables, envClick, 0.5f), 2000.0f);
            const int end = full.getNumSamples();
            const float db = juce::Decibels::gainToDecibels (rmsIn (half, 0, end) / rmsIn (full, 0, end));
            worstDb = juce::jmax (worstDb, std::abs (db + 6.0206f));
            if (std::abs (db + 6.0206f) > 0.01f)
                fail ("click " + clickTypeNames[type] + " at Click level 0.5: " + juce::String (db, 3) + " dB (expected -6.02)");
        }
        report << "| Click level 0.5: -6.02 dB within " << juce::String (worstDb, 4);
        std::printf ("click pitch / level: %s\n", report.toRawUTF8());
    }

    // --- extra Body layers: each style at Tune x 2^(st/12) under its pitch curve, and like the Body --
    {
        juce::String report;
        auto bodyLayer = [&] (int style, int env, float tuneSt, const EnvTables& t, float shape = 0.0f)
        {
            auto p = engineParams (false, false, false);
            p.tuneHz = 100.0f;
            p.lengthMs = 400.0f;
            auto& l = p.layers[0];
            l.on = true;
            l.source = layerBody;
            l.bodyStyle = style;
            l.env = l.pitchEnv = env; // both curves from the same layer
            l.tuneSt = tuneSt;
            l.color = shape;
            l.gain = 0.5f;
            return renderVoice (p, t, sr, (int) (0.4 * sr));
        };
        const auto bodyUp = with (flatTables, envPitch, 12.0f);
        for (int style = 0; style < layerBodyStyleNames.size(); ++style)
        {
            // Body env: the Body pitch curve (+12) on top of Tune 7 st; Noise env: the Noise one (flat).
            const double hzBody = zeroCrossingHz (bodyLayer (style, layerEnvBody, 7.0f, bodyUp), 0, sr,
                                                  (int) (0.05 * sr), (int) (0.35 * sr), 330.0f);
            const double hzNoise = zeroCrossingHz (bodyLayer (style, layerEnvNoise, 7.0f, bodyUp), 0, sr,
                                                   (int) (0.05 * sr), (int) (0.35 * sr), 165.0f);
            const double expBody = 100.0 * std::exp2 (19.0 / 12.0), expNoise = 100.0 * std::exp2 (7.0 / 12.0);
            report << layerBodyStyleNames[style] << " " << juce::String (hzBody, 1) << " / " << juce::String (hzNoise, 1) << "  ";
            if (std::abs (hzBody / expBody - 1.0) > 0.01 || std::abs (hzNoise / expNoise - 1.0) > 0.01)
                fail ("Body layer " + layerBodyStyleNames[style] + ": " + juce::String (hzBody, 1) + " Hz on Body (expected "
                      + juce::String (expBody, 1) + "), " + juce::String (hzNoise, 1) + " Hz on Noise (expected " + juce::String (expNoise, 1) + ")");

            // The same style and settings as the Body layer sound the same (default curves, sweep and all).
            auto core = engineParams (true, false, false);
            const auto tone = layerBodyTone (style);
            core.bodyStyle = style;
            core.bodyShape = 0.4f;
            core.bodyGain = 0.5f;
            core.otGain = tone.overtone;
            core.otRatio = tone.ratio;
            core.modes = tone.modes;
            core.tuneHz = 100.0f;
            core.lengthMs = 400.0f;
            const auto a = renderVoice (core, defTables, sr, (int) (0.4 * sr));
            const auto b = bodyLayer (style, layerEnvBody, 0.0f, defTables, 0.4f);
            const float diff = maxDiffAt (a, 0, b, 0, a.getNumSamples());
            if (! allFinite (b) || peakOf (b) < 0.05f || diff > 1.0e-5f * peakOf (a))
                fail ("Body layer " + layerBodyStyleNames[style] + " against the Body layer in that style: max diff "
                      + juce::String (diff) + " (peak " + juce::String (peakOf (a)) + ")");
        }
        std::printf ("Body layers, Hz on Body env (+12 st curve, Tune +7: 299.7) / Noise env (flat: 149.8): %s\n", report.toRawUTF8());

        const bool captionsOk = layerLevelEnvelope (0, layerEnvBody) == envBody && layerLevelEnvelope (2, layerEnvClick) == envClick
                             && layerLevelEnvelope (1, layerEnvFlat) < 0 && layerLevelEnvelope (1, layerEnvOwn) == layerOwnLevelEnvelope (1)
                             && layerPitchEnvelope (0, layerEnvBody) == envPitch && layerPitchEnvelope (0, layerEnvFlat) < 0
                             && layerPitchEnvelope (1, layerEnvNoise) == envNoisePitch && layerPitchEnvelope (2, layerEnvOwn) == layerOwnPitchEnvelope (2)
                             && isPitchEnvelope (layerOwnPitchEnvelope (2)) && ! isPitchEnvelope (layerOwnLevelEnvelope (0))
                             && layerOfEnvelope (layerOwnLevelEnvelope (2)) == 2 && layerOfEnvelope (envClick) < 0
                             // Older patches' single Envelope choice, split into Pitch and Level.
                             && legacyLayerPitchEnvelope (layerSine, layerEnvFlat) == layerEnvBody
                             && legacyLayerPitchEnvelope (layerNoise, layerEnvBody) == layerEnvFlat
                             && legacyLayerPitchEnvelope (layerBody, layerEnvNoise) == layerEnvNoise
                             && legacyLayerPitchEnvelope (layerSample, layerEnvClick) == layerEnvClick
                             && legacyLayerLevelEnvelope (layerClick, layerEnvBody) == layerEnvFlat
                             && legacyLayerLevelEnvelope (layerClick, layerEnvClick) == layerEnvClick
                             && legacyLayerLevelEnvelope (layerSample, layerEnvNoise) == layerEnvNoise
                             && layerUsesBodyControls (layerBody) && ! layerUsesBodyControls (layerSine)
                             && juce::String (layerShapeCaption (layerBody, body808)) == "Punch" && layerShapeCaption (layerNoise, 0) == nullptr
                             && layerBodyStyleNames.size() == bodySample && layerEnvNames.size() == layerEnvOwn + 1
                             && layerSourceNames[layerBody] == "Body" && layerEnvNames[layerEnvClick] == "Click";
        for (int s = 0; s < layerBodyStyleNames.size(); ++s)
            if (layerBodyStyleNames[s] != bodyStyleNames[s])
                fail ("layerBodyStyleNames[" + juce::String (s) + "] doesn't match bodyStyleNames");
        if (! captionsOk)
            fail ("layer envelope / Body helpers in StyleInfo.h disagree with the engine");
    }

    // --- extra layers on the Click envelope follow the Click curves -------------------------------------
    {
        juce::String report;
        auto layer = [&] (int source, int env, const EnvTables& t)
        {
            auto p = engineParams (false, false, false);
            p.tuneHz = 100.0f;
            auto& l = p.layers[0];
            l.on = true;
            l.source = source;
            l.env = l.pitchEnv = env; // both curves from the same layer
            l.clickType = source == layerClick ? clickStick : clickSnap;
            l.clickTone = 1000.0f;
            l.clickLengthMs = 40.0f;
            l.noiseStyle = noiseWhite;
            l.hp = 500.0f;
            l.lp = 2000.0f;
            l.color = 0.0f;
            l.gain = 0.5f;
            return renderVoice (p, t, sr, (int) (0.4 * sr), { 512 }, 0x5EEDu, 60, set);
        };
        const auto up = with (flatTables, envClickPitch, 12.0f);
        const auto half = with (flatTables, envClick, 0.5f);

        const double onClick = zeroCrossingHz (layer (layerClick, layerEnvClick, up), 0, sr, (int) (0.001 * sr), (int) (0.02 * sr));
        const double onFlat = zeroCrossingHz (layer (layerClick, layerEnvFlat, up), 0, sr, (int) (0.001 * sr), (int) (0.02 * sr));
        report << "Stick: " << juce::String (onClick, 0) << " Hz on Click, " << juce::String (onFlat, 0) << " on Flat  ";
        if (std::abs (onClick / 2000.0 - 1.0) > 0.005 || std::abs (onFlat / 1000.0 - 1.0) > 0.005)
            fail ("Click layer, click pitch +12: " + juce::String (onClick, 1) + " Hz on the Click envelope (expected 2000), "
                  + juce::String (onFlat, 1) + " on Flat (expected 1000)");

        const double sineHz = zeroCrossingHz (layer (layerSine, layerEnvClick, up), 0, sr, (int) (0.02 * sr), (int) (0.35 * sr));
        const double smpHz = zeroCrossingHz (layer (layerSample, layerEnvClick, up), 0, sr, (int) (0.02 * sr), (int) (0.35 * sr));
        report << "Sine " << juce::String (sineHz, 1) << " Hz, Sample " << juce::String (smpHz, 1) << " Hz  ";
        if (std::abs (sineHz / 200.0 - 1.0) > 0.005 || std::abs (smpHz / 440.0 - 1.0) > 0.005)
            fail ("Sine / Sample layer on Click, click pitch +12: " + juce::String (sineHz, 1) + " / " + juce::String (smpHz, 1)
                  + " Hz (expected 200 / 440)");

        const double centroid = spectralCentre ({ layer (layerNoise, layerEnvClick, up) }, sr)
                              / spectralCentre ({ layer (layerNoise, layerEnvClick, flatTables) }, sr);
        report << "Noise centre x" << juce::String (centroid, 3) << "  ";
        if (std::abs (centroid - 2.0) > 0.15)
            fail ("Noise layer on Click, click pitch +12: centroid x" + juce::String (centroid, 3) + " (expected x2)");

        juce::String levels;
        for (int source : { (int) layerClick, (int) layerNoise, (int) layerSine, (int) layerSample, (int) layerBody })
        {
            const auto full = layer (source, layerEnvClick, flatTables);
            const auto halved = layer (source, layerEnvClick, half);
            const int end = full.getNumSamples();
            const float db = juce::Decibels::gainToDecibels (rmsIn (halved, 0, end) / rmsIn (full, 0, end));
            levels << layerSourceNames[source] << " " << juce::String (db, 2) << "  ";
            if (std::abs (db + 6.0206f) > 0.02f)
                fail (layerSourceNames[source] + " layer on Click, Click level 0.5: " + juce::String (db, 3) + " dB (expected -6.02)");
        }
        if (! identical (layer (layerClick, layerEnvFlat, half), layer (layerClick, layerEnvFlat, flatTables)))
            fail ("the Click level curve changes a Click layer on Flat");
        std::printf ("extra layers on the Click envelope: %s| Click level 0.5: %s\n", report.toRawUTF8(), levels.toRawUTF8());
    }

    // --- phase invert: inverted + not inverted = 0, coupling untouched, a smooth flip ------------------
    {
        struct Case { const char* name; std::function<void (DrumParams&, bool)> set; };
        const std::vector<Case> cases {
            { "Body",        [] (DrumParams& p, bool inv) { p.bodyOn = true; p.bodyInvert = inv; } },
            { "Body Sample", [] (DrumParams& p, bool inv) { p.bodyOn = true; p.bodyStyle = bodySample; p.bodyInvert = inv; } },
            { "Noise",       [] (DrumParams& p, bool inv) { p.noiseOn = true; p.bodyOn = true; p.bodySolo = false; p.noiseSolo = true; p.noiseInvert = inv; } },
            { "Click",       [] (DrumParams& p, bool inv) { p.clickOn = true; p.clickInvert = inv; } },
            { "Click Sample",[] (DrumParams& p, bool inv) { p.clickOn = true; p.clickType = clickSample; p.clickTone = 300.0f; p.clickInvert = inv; } },
        };
        auto layerCase = [] (int source)
        {
            return [source] (DrumParams& p, bool inv)
            {
                auto& l = p.layers[1];
                l.on = true;
                l.source = source;
                l.env = layerEnvBody;
                l.invert = inv;
                l.gain = 0.5f;
            };
        };
        juce::String report;
        float worst = 0.0f;
        auto test = [&] (const juce::String& name, const std::function<void (DrumParams&, bool)>& setup)
        {
            auto p = engineParams (false, false, false);
            setup (p, false);
            const auto a = renderVoice (p, defTables, sr, (int) (0.2 * sr), { 512 }, 0x5EEDu, 60, set);
            setup (p, true);
            const auto b = renderVoice (p, defTables, sr, (int) (0.2 * sr), { 512 }, 0x5EEDu, 60, set);
            float sum = 0.0f;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < a.getNumSamples(); ++i)
                    sum = juce::jmax (sum, std::abs (a.getSample (c, i) + b.getSample (c, i)));
            worst = juce::jmax (worst, sum);
            if (peakOf (a) < 1.0e-3f || sum > 1.0e-7f)
                fail (name + " inverted + not inverted: max |sum| " + juce::String (sum) + " (peak " + juce::String (peakOf (a)) + ")");
        };
        for (const auto& c : cases)
            test (c.name, c.set);
        for (int source = 0; source < numLayerSources; ++source)
            test (layerSourceNames[source] + " layer", layerCase (source));
        report << "inverted + not inverted, every layer and source: max |sum| " << juce::String (worst);

        // Wires coupling hears the body as it is: inverting the Body leaves a soloed Wires alone.
        auto wires = engineParams (true, true, false);
        wires.noiseCouple = 1.0f;
        wires.noiseSolo = true;
        const auto upright = renderVoice (wires, defTables, sr, (int) (0.2 * sr));
        wires.bodyInvert = true;
        if (! identical (upright, renderVoice (wires, defTables, sr, (int) (0.2 * sr))))
            fail ("inverting the Body changes the Wires coupling");

        // Flipping mid-hit swings through zero over 2.5 ms, with no step.
        auto p = engineParams (true, false, false);
        p.bodyStyle = bodyClassic;
        p.bodyShape = 0.0f;
        const auto t = with (flatTables, envPitch, 0.0f);
        const int total = (int) (0.1 * sr), flipAt = 64 * 40;
        DrumVoice v1, v2;
        v1.prepare (sr);
        v2.prepare (sr);
        v1.start (60, 1.0f, 0x5EEDu, p);
        v2.start (60, 1.0f, 0x5EEDu, p);
        juce::AudioBuffer<float> a (2, total), b (2, total);
        a.clear();
        b.clear();
        for (int pos = 0; pos < total; pos += 64)
        {
            auto flipped = p;
            flipped.bodyInvert = pos >= flipAt;
            v1.render (a.getWritePointer (0, pos), a.getWritePointer (1, pos), 64, p, t, {});
            v2.render (b.getWritePointer (0, pos), b.getWritePointer (1, pos), 64, flipped, t, {});
        }
        float maxStep = 0.0f, normalStep = 0.0f;
        for (int i = 1; i < total; ++i)
        {
            const bool flipping = i >= flipAt - 8 && i < flipAt + (int) (0.003 * sr);
            float& largest = flipping ? maxStep : normalStep;
            largest = juce::jmax (largest, std::abs (b.getSample (0, i) - b.getSample (0, i - 1)));
        }
        // A swing from +1 to -1 over 2.5 ms adds at most 2 / 120 samples (at 48 kHz) to a step.
        const float allowed = 1.05f * (normalStep + 2.0f / (0.0025f * (float) sr));
        const int settled = flipAt + (int) std::ceil (0.0025 * sr) + 1;
        const float after = maxDiffAt (b, settled, a, settled, total - settled);
        float negated = 0.0f;
        for (int i = settled; i < total; ++i)
            negated = juce::jmax (negated, std::abs (a.getSample (0, i) + b.getSample (0, i)));
        report << ", flip mid-hit: largest step " << juce::String (maxStep, 4) << " (else " << juce::String (normalStep, 4)
               << "), inverted 2.5 ms later within " << juce::String (negated);
        if (maxStep > allowed || negated > 1.0e-6f || after < 0.1f)
            fail ("flipping the Body's polarity mid-hit: largest step " + juce::String (maxStep) + " against " + juce::String (normalStep)
                  + " elsewhere; 2.5 ms later off the inverted signal by " + juce::String (negated));
        std::printf ("phase invert: %s\n", report.toRawUTF8());
    }

    // --- pitches down to 0 Hz: every style stays finite and bounded, and holds still at 0 -------------
    {
        juce::String report;
        EnvTables dive = defTables;
        dive.bake (envPitch, CurveEnvelope ({ { 0.0f, 0.0f, 0.0f }, { 0.2f, -120.0f, 0.0f }, { 0.6f, -120.0f, 0.0f },
                                              { 0.8f, 60.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } }, envelopeRange (envPitch)));
        dive.bake (envNoisePitch, CurveEnvelope ({ { 0.0f, 120.0f, 0.0f }, { 0.5f, -120.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } },
                                                 envelopeRange (envNoisePitch)));
        dive.bake (envClickPitch, CurveEnvelope ({ { 0.0f, -120.0f, 0.0f }, { 0.01f, 120.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } },
                                                 envelopeRange (envClickPitch)));
        const auto floor = with (flatTables, envPitch, -120.0f);
        float worstPeak = 0.0f;
        int worstCrossings = 0;
        for (int style = 0; style < bodySample; ++style)
            for (float tune : { 0.0f, 20.0f })
            {
                auto p = engineParams (true, true, true);
                p.bodyStyle = style;
                p.bodyShape = 0.7f;
                p.modes = 0.5f;
                p.tuneHz = tune;          // 0: a corrupt snapshot; every oscillator holds still
                p.lengthMs = 2000.0f;
                p.noiseStyle = noiseMetallic;
                for (int k = 0; k < kNumExtraLayers; ++k)
                {
                    auto& l = p.layers[(size_t) k];
                    l.on = true;
                    l.source = k == 0 ? layerSine : layerBody;
                    l.bodyStyle = (style + k) % bodySample;
                    l.color = 0.6f;
                    l.tuneSt = -24.0f;
                    l.gain = 0.3f;
                }
                for (const EnvTables* t : std::initializer_list<const EnvTables*> { &dive, &floor })
                {
                    const auto hit = renderVoice (p, *t, sr, (int) (2.0 * sr));
                    worstPeak = juce::jmax (worstPeak, peakOf (hit));
                    if (! allFinite (hit) || peakOf (hit) > 12.0f)
                        fail (bodyStyleNames[style] + " at Tune " + juce::String (tune) + " Hz down to -120 st: "
                              + (allFinite (hit) ? "peak " + juce::String (peakOf (hit)) : juce::String ("non-finite")));
                }

                // On its own at -120 st (Tune 20 Hz: 0.02 Hz) the body barely moves: no full cycle in 2 s.
                auto alone = engineParams (true, false, false);
                alone.bodyStyle = style;
                alone.tuneHz = tune;
                alone.lengthMs = 2000.0f;
                alone.otGain = 0.0f;
                alone.modes = 0.0f;
                alone.bodyShape = 0.0f;
                const auto still = renderVoice (alone, floor, sr, (int) (2.0 * sr));
                int crossings = 0;
                for (int i = 1; i < still.getNumSamples(); ++i)
                    if (still.getSample (0, i - 1) < 0.0f && still.getSample (0, i) >= 0.0f)
                        ++crossings;
                worstCrossings = juce::jmax (worstCrossings, crossings);
                if (! allFinite (still) || crossings > 1)
                    fail (bodyStyleNames[style] + " at Tune " + juce::String (tune) + " Hz, -120 st: " + juce::String (crossings)
                          + " rising zero crossings in 2 s (expected at most 1: a cycle takes 51 s)");
            }
        report << "every style (plus Sine / Body layers, Metallic, every click moving +-120 st) at Tune 0 and 20 Hz: finite, peak "
               << juce::String (worstPeak, 2) << "; alone at 20 Hz - 120 st: at most " << juce::String (worstCrossings) << " crossing in 2 s";
        std::printf ("pitch floor 0 Hz: %s\n", report.toRawUTF8());
    }

    // --- block-size independence with all of the above moving ------------------------------------------
    {
        EnvTables t = defTables;
        t.bake (envNoisePitch, CurveEnvelope ({ { 0.0f, 12.0f, -0.5f }, { 0.4f, -7.0f, 0.3f }, { 1.0f, 0.0f, 0.0f } }, envelopeRange (envNoisePitch)));
        t.bake (envClickPitch, CurveEnvelope ({ { 0.0f, -5.0f, 0.0f }, { 0.03f, 24.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } }, envelopeRange (envClickPitch)));
        t.bake (envClick, CurveEnvelope ({ { 0.0f, 2.0f, 0.0f }, { 0.02f, 0.3f, 0.0f }, { 1.0f, 1.0f, 0.0f } }, envelopeRange (envClick)));
        struct Setup { int body, noise, click; int src[3]; int env[3]; };
        const Setup setups[] { { bodyMembrane, noiseMetallic, clickClap, { layerBody, layerClick, layerSample }, { layerEnvNoise, layerEnvClick, layerEnvNoise } },
                               { bodyFM, noiseCrackle, clickSample, { layerBody, layerBody, layerNoise }, { layerEnvBody, layerEnvFlat, layerEnvClick } },
                               { body808, noiseSample, clickRim, { layerSine, layerBody, layerClick }, { layerEnvClick, layerEnvClick, layerEnvClick } } };
        float worst = 0.0f;
        for (const auto& s : setups)
        {
            DrumParams p;
            p.bodyStyle = s.body;
            p.noiseStyle = s.noise;
            p.clickType = s.click;
            p.clickTone = 2000.0f;
            p.noiseGrit = 0.4f;
            p.noiseInvert = true;
            for (int k = 0; k < kNumExtraLayers; ++k)
            {
                auto& l = p.layers[(size_t) k];
                l.on = true;
                l.source = s.src[k];
                l.env = s.env[k];
                l.bodyStyle = (s.body + 2 * k + 1) % bodySample;
                l.clickType = clickPop;
                l.noiseStyle = noiseWhite;
                l.gain = 0.5f;
                l.tuneSt = 3.0f * (float) k;
                l.delayMs = 7.3f * (float) k - 5.0f;
                l.invert = k == 1;
                l.hp = 60.0f;
                l.lp = 12000.0f;
            }
            constexpr int total = 30000;
            const auto big = renderBlocks (p, t, set, sr, total, 300, { 4096 });
            const auto small = renderBlocks (p, t, set, sr, total, 300, { 1, 37, 64, 65, 500, 13, 128 });
            const float diff = maxDiffAt (big, 0, small, 0, total);
            worst = juce::jmax (worst, diff);
            if (! allFinite (big) || peakOf (big) < 0.01f)
                fail ("pitch curves, Body layers and invert: silent or non-finite");
            if (diff > 1.0e-5f)
                fail ("block-size dependence with pitch curves, Body layers and invert: max diff " + juce::String (diff));
        }
        std::printf ("block-size independence (Noise / Click pitch and Click level curves, Body layers, invert): max diff %.2e\n", worst);
    }

    return failures;
}

/** Example hits for listening, with the measured fundamental 30 ms in. */
int cmdStyles (const juce::File& outDir)
{
    outDir.createDirectory();
    constexpr double sr = 48000.0;
    const auto defTables = engineTables (false);

    // A kick's pitch drops within ~40 ms, then holds; its level decays over the Length.
    // Each example's pitch curve is one of these shapes (0..1) x its depth in semitones.
    const std::vector<EnvPoint> kickPitch { { 0.0f, 1.0f, -0.8f }, { 0.07f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } };
    EnvTables kickTables = defTables;
    kickTables.bake (envBody, CurveEnvelope ({ { 0.0f, 1.0f, -0.3f }, { 0.7f, 0.3f, -0.4f }, { 1.0f, 0.0f, 0.0f } },
                                             envelopeRange (envBody)));

    const auto sample = makeSineSample (200.0, 44100.0, 1.0, false);
    SampleSet set;
    for (auto& s : set.slots)
        s = sample.get();

    struct Example { juce::String name; int body; float tune, depth, shape, lengthMs; bool kick; int noise, click; };
    std::vector<Example> examples;
    for (int b = 0; b < numBodyStyles; ++b)
        examples.push_back ({ "snare_" + bodyStyleNames[b], b, 200.0f, b == bodyMembrane ? 5.0f : 14.0f, 0.3f, 450.0f, false,
                              noiseWires, clickSnap });
    examples.push_back ({ "kick_Kick_50Hz_36st", bodyKick, 50.0f, 36.0f, 0.5f, 600.0f, true, -1, clickBeater });
    examples.push_back ({ "kick_808_45Hz_12st", body808, 45.0f, 12.0f, 0.3f, 900.0f, true, -1, clickBeater });
    examples.push_back ({ "kick_909_55Hz_24st", body909, 55.0f, 24.0f, 0.4f, 500.0f, true, -1, clickBeater });
    examples.push_back ({ "kick_FM_50Hz_30st", bodyFM, 50.0f, 30.0f, 0.3f, 500.0f, true, -1, clickBeater });
    examples.push_back ({ "kick_Round_50Hz_24st", bodyRound, 50.0f, 24.0f, 0.6f, 600.0f, true, -1, clickBeater });
    examples.push_back ({ "kick_Taper_52Hz_30st", bodyTaper, 52.0f, 30.0f, 0.8f, 450.0f, true, -1, clickBeater });
    examples.push_back ({ "kick_Hard_55Hz_36st", bodyHard, 55.0f, 36.0f, 0.7f, 600.0f, true, -1, clickBeater });
    examples.push_back ({ "kick_Fold_50Hz_24st", bodyFold, 50.0f, 24.0f, 0.6f, 500.0f, true, -1, clickBeater });
    examples.push_back ({ "kick_Tonal_50Hz_24st", bodyTonal, 50.0f, 24.0f, 0.5f, 500.0f, true, -1, clickBeater });
    examples.push_back ({ "tom_Membrane_110Hz", bodyMembrane, 110.0f, 4.0f, 0.5f, 900.0f, false, -1, clickStick });
    examples.push_back ({ "hat_Metallic", bodyClassic, 200.0f, 0.0f, 0.0f, 250.0f, false, noiseMetallic, clickTick });
    examples.push_back ({ "clap_Clap", bodyClassic, 200.0f, 0.0f, 0.0f, 300.0f, false, -1, clickClap });

    std::printf ("%-24s %8s %10s %10s %10s %10s %8s\n", "example", "tune", "f@30ms", "curve", "f@150ms", "curve", "peak");
    for (const auto& e : examples)
    {
        const auto tables = e.kick ? withPitch (kickTables, kickPitch, e.depth) : withPitch (defTables, kUnitPitchCurve, e.depth);
        DrumParams p;
        p.bodyStyle = e.body;
        p.tuneHz = e.tune;
        p.bodyShape = e.shape;
        p.lengthMs = e.lengthMs;
        p.modes = e.body == bodyMembrane ? 0.6f : e.body == body909 ? 0.3f : 0.25f;
        p.otRatio = e.body == body808 ? 1.75f : e.body == body909 ? 1.7f : 1.594f;
        p.noiseOn = e.noise >= 0;
        p.noiseStyle = juce::jmax (0, e.noise);
        p.clickType = e.click;
        if (e.noise == noiseMetallic)
        {
            p.bodyOn = false;
            p.noiseHP = 7000.0f;
            p.noiseLP = 16000.0f;
            p.noiseColor = 0.6f;
            p.noiseGain = 1.4f;
        }
        if (e.click == clickClap)
        {
            p.bodyOn = false;
            p.clickGain = 1.0f;
            p.clickTone = 1200.0f;
            p.clickLengthMs = 30.0f;
        }
        if (e.kick)
        {
            p.otGain = 0.15f;
            p.clickGain = 0.3f;
            p.clickTone = 4000.0f;
            p.driveAmount = 0.1f;
        }

        const auto hit = renderHit (p, tables, sr, 60, 1.0f, 0x5EEDu, set);
        writeWav (hit, sr, outDir.getChildFile (e.name + ".wav"));
        writePng (hit, sr, e.name, outDir.getChildFile (e.name + ".png"));

        // The fundamental, from the body alone, against what Tune and the drawn
        // pitch curve ask for at that moment. Zero crossings average over two
        // periods, so a falling pitch measures a little under the curve.
        juce::String cols;
        for (float ms : { 30.0f, 150.0f })
        {
            if (! p.bodyOn || ms >= e.lengthMs)
            {
                cols << juce::String ("-").paddedLeft (' ', 10) << juce::String ("-").paddedLeft (' ', 10);
                continue;
            }
            auto q = p;
            q.noiseOn = q.clickOn = false;
            q.clipMode = 0;
            q.driveAmount = 0.0f;
            const auto body = renderHit (q, tables, sr, 60, 1.0f, 0x5EEDu, set);
            const double expected = e.body == bodySample ? 200.0
                                  : e.tune * std::exp2 (tables.lookup (envPitch, ms / e.lengthMs) / 12.0f);
            const double hz = zeroCrossingHz (body, 0, sr, (int) (ms * 0.001 * sr), body.getNumSamples(), (float) expected * 1.25f, 2);
            cols << juce::String (hz, 1).paddedLeft (' ', 10) << juce::String (expected, 1).paddedLeft (' ', 10);
        }
        std::printf ("%-24s %6.0fHz %s %6.1fdB\n", e.name.toRawUTF8(), e.tune, cols.toRawUTF8(),
                     juce::Decibels::gainToDecibels (peakOf (hit)));
    }
    return 0;
}

/** Realtime cost of the engine with each body style, and with everything on. */
void benchEngineStyles()
{
    constexpr double sr = 48000.0;
    constexpr int block = 128;
    const auto tables = engineTables (false);
    const auto sample = makeSineSample (220.0, 44100.0, 2.0, true);
    SampleSet set;
    for (auto& s : set.slots)
        s = sample.get();

    auto run = [&] (const char* name, const DrumParams& p, const EnvTables& t)
    {
        DrumEngine engine;
        engine.prepare (sr, block);
        engine.setParams (p);
        engine.setTables (t);
        engine.setSamples (set);
        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer midi;
        const int totalBlocks = (int) (sr * 30.0 / block);
        const int hitEvery = (int) (sr * 0.06);
        int64_t samplePos = 0;
        const auto start = juce::Time::getHighResolutionTicks();
        for (int b = 0; b < totalBlocks; ++b)
        {
            midi.clear();
            const int64_t next = ((samplePos / hitEvery) + 1) * hitEvery;
            if (next < samplePos + block)
                midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), (int) (next - samplePos));
            engine.process (buf, midi);
            samplePos += block;
        }
        const double secs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
        std::printf ("%-32s 30 s of dense hits in %.3f s  ->  %.2f%% of one core (%.0fx realtime)\n",
                     name, secs, 100.0 * secs / 30.0, 30.0 / secs);
    };

    run ("engine, defaults", DrumParams(), tables);
    for (int b = 1; b < numBodyStyles; ++b)
    {
        DrumParams p;
        p.bodyStyle = b;
        p.modes = 1.0f;
        const auto name = "engine, body " + bodyStyleNames[b];
        run (name.toRawUTF8(), p, tables);
    }

    DrumParams heavy;
    heavy.bodyStyle = bodyMembrane;
    heavy.modes = 1.0f;
    heavy.noiseStyle = noiseMetallic;
    heavy.clickType = clickBeater;
    for (int k = 0; k < kNumExtraLayers; ++k)
    {
        auto& l = heavy.layers[(size_t) k];
        l.on = true;
        l.source = k;
        l.delayMs = 5.0f;
        l.hp = 150.0f;
        l.lp = 6000.0f;
        l.pan = 0.3f;
    }
    run ("engine, Membrane+Metallic+3 layers", heavy, tables);

    // Three extra layers as noise / click voices, on top of the defaults.
    DrumParams layered;
    const int sources[] { layerNoise, layerNoise, layerClick };
    const int styles[] { noiseWires, noiseMetallic, clickRim };
    for (int k = 0; k < kNumExtraLayers; ++k)
    {
        auto& l = layered.layers[(size_t) k];
        l.on = true;
        l.source = sources[k];
        l.noiseStyle = styles[k];
        l.clickType = styles[k];
        l.env = layerEnvNoise;
        l.delayMs = 5.0f * (float) k;
        l.pan = -0.3f + 0.3f * (float) k;
    }
    run ("engine, 2 noise + 1 click layers", layered, tables);

    for (auto& l : layered.layers)
    {
        l.source = layerNoise;
        l.noiseStyle = noiseMetallic;
        l.clickType = clickRim;
    }
    run ("engine, 3 Metallic noise layers", layered, tables);

    // Body layers: three more bodies (the heaviest styles), on the Body curves.
    DrumParams bodies;
    const int bodyStyles[] { bodyMembrane, body808, bodyFM };
    for (int k = 0; k < kNumExtraLayers; ++k)
    {
        auto& l = bodies.layers[(size_t) k];
        l.on = true;
        l.source = layerBody;
        l.bodyStyle = bodyStyles[k];
        l.tuneSt = 7.0f * (float) k - 12.0f;
        l.color = 0.5f;
        l.pan = -0.3f + 0.3f * (float) k;
    }
    run ("engine, 3 Body layers", bodies, tables);

    // Drawn Noise and Click pitch curves (filters retuned every 16 samples) and
    // a Click level curve, on the core layers and on two extra layers that follow them.
    EnvTables moving = tables;
    moving.bake (envNoisePitch, CurveEnvelope ({ { 0.0f, 12.0f, -0.5f }, { 0.5f, -12.0f, 0.3f }, { 1.0f, 0.0f, 0.0f } },
                                               envelopeRange (envNoisePitch)));
    moving.bake (envClickPitch, CurveEnvelope ({ { 0.0f, 24.0f, -0.5f }, { 0.3f, -12.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } },
                                               envelopeRange (envClickPitch)));
    moving.bake (envClick, CurveEnvelope ({ { 0.0f, 2.0f, -0.5f }, { 1.0f, 0.5f, 0.0f } }, envelopeRange (envClick)));
    DrumParams curves = layered;
    curves.noiseStyle = noiseMetallic;
    curves.clickType = clickClap;
    curves.layers[0].source = layerNoise;
    curves.layers[0].noiseStyle = noiseWhite;
    curves.layers[0].env = layerEnvNoise;
    curves.layers[1].source = layerClick;
    curves.layers[1].env = layerEnvClick;
    curves.layers[2].on = false;
    run ("engine, pitch curves: flat", curves, tables);
    run ("engine, pitch curves: drawn", curves, moving);

    // Offsets: early layers are rendered from their own start when the hit
    // lands (thrown away up to the hit), late ones keep the voice alive longer.
    DrumParams offsets = heavy;
    offsets.clickType = clickClap;
    offsets.bodyOffsetMs = offsets.noiseOffsetMs = offsets.clickOffsetMs = -100.0f;
    for (auto& l : offsets.layers)
        l.delayMs = -100.0f;
    run ("engine, heavy, every layer -100 ms", offsets, tables);
    for (auto& l : offsets.layers)
        l.delayMs = 100.0f;
    run ("engine, heavy, core -100 / layers +100", offsets, tables);

    // The worst single hit: what the first block after a note-on costs when
    // every layer starts 100 ms early (all of that is rendered at the hit).
    for (double rate : { 48000.0, 96000.0 })
    {
        double worst = 0.0, sum = 0.0;
        constexpr int trials = 50;
        float l[64] {}, r[64] {};
        offsets.layers[0].delayMs = offsets.layers[1].delayMs = offsets.layers[2].delayMs = -100.0f;
        for (int k = 0; k < trials; ++k)
        {
            DrumVoice voice;
            voice.prepare (rate);
            const auto start = juce::Time::getHighResolutionTicks();
            voice.start (60, 1.0f, 0x5EEDu + (uint32_t) k, offsets);
            voice.render (l, r, 64, offsets, tables, set);
            const double us = 1.0e6 * juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
            worst = juce::jmax (worst, us);
            sum += us;
        }
        std::printf ("note-on + first 64 samples, every layer -100 ms, %.0f kHz: %.0f us average, %.0f us worst "
                     "(a 64-sample block lasts %.0f us)\n", rate / 1000.0, sum / trials, worst, 64.0e6 / rate);
    }
}

/** The editor's layers: Body's mute, the Layers section's tabs (Noise, Click, extra
    layers), adding each kind of layer from "+", muting from a tab, removing, which
    tabs survive closing the editor and loading a preset, relabelling knobs per
    style, the browser drawer, the KEY badge following the last note, and opening /
    closing the editor repeatedly. */
int checkEditor()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: editor: %s\n", msg.toRawUTF8()); ++failures; };

    DrumFoundryProcessor proc;
    proc.prepareToPlay (48000.0, 512);

    const auto knobWithCaption = [] (juce::Component& root, const juce::String& caption)
    {
        std::function<ui::Knob* (juce::Component&)> find = [&] (juce::Component& c) -> ui::Knob*
        {
            for (auto* child : c.getChildren())
            {
                if (auto* k = dynamic_cast<ui::Knob*> (child); k != nullptr && k->getCaption() == caption && k->isVisible())
                    return k;
                if (! child->isVisible())
                    continue; // e.g. a Layers page that isn't selected
                if (auto* k = find (*child))
                    return k;
            }
            return nullptr;
        };
        return find (root);
    };
    const auto updateEditor = [] (juce::AudioProcessorEditor& editor) // as the editor's timer would
    {
        if (auto* body = findChild<ui::LayerPanel> (editor))
            body->update();
        if (auto* layers = findChild<ui::LayersSection> (editor))
            layers->update();
    };
    using LS = ui::LayersSection;
    enum { sample = 0, sine, noise, click };

    for (int round = 0; round < 4; ++round)
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        auto* ed = dynamic_cast<DrumFoundryEditor*> (editor.get());
        auto* layers = findChild<LS> (*editor);
        auto* body = findChild<ui::LayerPanel> (*editor); // the first one is Body's panel
        if (ed == nullptr || layers == nullptr || body == nullptr || body->getMuteButton() == nullptr)
        {
            fail ("no editor / layers section / body panel");
            return failures;
        }

        if (round == 0)
        {
            if (layers->getSelectedTab() != LS::noiseTab || layers->hasTab (0) || layers->hasTab (1) || layers->hasTab (2))
                fail ("a new patch doesn't start on NOISE with no extra tabs");
            if (layers->getTabName (LS::noiseTab) != "NOISE" || layers->getTabName (LS::clickTab) != "CLICK")
                fail ("core tabs aren't named NOISE and CLICK");

            // "+": Noise, Click, Sample, then nothing is free.
            if (ed->addExtraLayer (noise) != 0 || ed->addExtraLayer (click) != 1)
                fail ("adding Noise and Click layers didn't use layers 1 and 2");
            if (rawParam (proc, "lay1On") < 0.5f || ! juce::approximatelyEqual (rawParam (proc, "lay1Source"), (float) noise)
                || ! juce::approximatelyEqual (rawParam (proc, "lay1Env"), (float) layerEnvOwn) || ! juce::approximatelyEqual (rawParam (proc, "lay1NoiseStyle"), 1.0f))
                fail ("a Noise layer isn't on / Noise / its own curves / White");
            if (rawParam (proc, "lay2On") < 0.5f || ! juce::approximatelyEqual (rawParam (proc, "lay2Source"), (float) click)
                || ! juce::approximatelyEqual (rawParam (proc, "lay2ClickType"), 1.0f))
                fail ("a Click layer isn't on / Click / Snap");
            if (layers->getSelectedExtraLayer() != 1 || layers->getTabName (LS::firstExtraTab + 1) != "CLICK 2"
                || layers->getTabName (LS::firstExtraTab) != "NOISE 2")
                fail ("added layers aren't selected / named NOISE 2 and CLICK 2");

            if (ed->addExtraLayer (sample) != 2 || ! juce::approximatelyEqual (rawParam (proc, "lay3Source"), (float) sample))
                fail ("adding a Sample layer didn't use layer 3");
            if (! ed->isBrowserVisible() || layers->getSelectedExtraLayer() != 2 || layers->getTabName (LS::firstExtraTab + 2) != "SAMPLE")
                fail ("a Sample layer didn't open the browser on its tab");
            ed->setBrowserVisible (false);
            if (ed->addExtraLayer (sine) != -1)
                fail ("a fourth extra layer was added");

            // Mute from a tab: the layer goes off, keeps its tab, and its tab is the one shown.
            layers->getMuteButton (LS::firstExtraTab).onClick();
            if (rawParam (proc, "lay1On") > 0.5f || ! layers->hasTab (0) || layers->getSelectedExtraLayer() != 0)
                fail ("muting NOISE 2 from its tab didn't switch it off / keep its tab / switch to it");
            layers->getMuteButton (LS::clickTab).onClick();
            if (rawParam (proc, pid::clickOn) > 0.5f || ! layers->getMuteButton (LS::clickTab).isMuted())
                fail ("muting CLICK from its tab didn't switch it off");
            layers->getMuteButton (LS::clickTab).onClick();
            if (rawParam (proc, pid::clickOn) < 0.5f)
                fail ("unmuting CLICK didn't switch it on");

            body->getMuteButton()->onClick();
            if (rawParam (proc, pid::bodyOn) > 0.5f || ! body->getMuteButton()->isMuted())
                fail ("Body's mute didn't switch it off");
            body->getMuteButton()->onClick();

            // Muted by something else (automation): the tab stays too.
            setParam (proc, "lay3On", 0.0f);
            updateEditor (*editor);
            if (! layers->hasTab (2))
                fail ("a layer muted by automation lost its tab");
            setParam (proc, "lay3On", 1.0f);

            // Remove CLICK 2: off, tab gone, back to the tab on its left.
            layers->selectExtraLayer (1);
            layers->removeLayer (1);
            updateEditor (*editor);
            if (rawParam (proc, "lay2On") > 0.5f || layers->hasTab (1) || layers->getSelectedExtraLayer() != 0)
                fail ("removing CLICK 2 didn't switch it off / close its tab / show NOISE 2");

            // The free slot takes a sine sub.
            if (ed->addExtraLayer (sine) != 1 || ! juce::approximatelyEqual (rawParam (proc, "lay2Source"), (float) sine)
                || ! juce::approximatelyEqual (rawParam (proc, "lay2Env"), (float) layerEnvOwn) || ! juce::approximatelyEqual (rawParam (proc, "lay2Tune"), -12.0f))
                fail ("a Sine layer isn't Sine / its own curves / -12 st in the freed slot");
            if (layers->getTabName (LS::firstExtraTab + 1) != "SINE")
                fail ("the sine layer's tab isn't SINE");

            // Tabs switch pages, and styles relabel the knobs of the page on show.
            setParam (proc, pid::bodyStyle, (float) bodyFM);
            setParam (proc, pid::noiseStyle, (float) noiseCrackle);
            setParam (proc, pid::clickType, (float) clickSample);
            updateEditor (*editor);
            ed->selectLayerTab (LS::noiseTab);
            for (const char* caption : { "Index", "Feedback", "Mod Ratio", "Index Decay", "Density" })
                if (knobWithCaption (*editor, caption) == nullptr)
                    fail (juce::String ("no knob captioned ") + caption);
            if (knobWithCaption (*editor, "Shape") != nullptr)
                fail ("Body Shape still captioned Shape in FM");
            if (knobWithCaption (*editor, "Max Length") != nullptr)
                fail ("the Click page shows while NOISE is selected");
            ed->selectLayerTab (LS::clickTab);
            if (knobWithCaption (*editor, "Max Length") == nullptr || knobWithCaption (*editor, "Density") != nullptr)
                fail ("the CLICK tab doesn't show the Click page");
            ed->selectExtraLayer (0);
            if (knobWithCaption (*editor, "Pitch") == nullptr || knobWithCaption (*editor, "Envelope") != nullptr)
                fail ("the NOISE 2 page doesn't show Pitch");

            ed->browseSlot (slotNoise);
            if (! ed->isBrowserVisible() || layers->getSelectedTab() != LS::noiseTab)
                fail ("BROWSE on Noise didn't open the browser on the NOISE tab");
            ed->browseSlot (slotLayer3);
            if (layers->getSelectedExtraLayer() != 2)
                fail ("BROWSE on layer 3 didn't show its tab");
            ed->setBrowserVisible (false);
        }
        else if (round == 1)
        {
            // Reopened: the same tabs (NOISE 2 muted but kept), on the same tab.
            if (! layers->hasTab (0) || ! layers->hasTab (1) || ! layers->hasTab (2) || rawParam (proc, "lay1On") > 0.5f)
                fail ("reopened editor lost a tab (or NOISE 2 isn't muted)");
            if (layers->getSelectedExtraLayer() != 2)
                fail ("reopened editor isn't on the same tab");

            // The KEY badge and the ruler follow the last note played.
            juce::AudioBuffer<float> buf (2, 256);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 36, 0.8f), 0);
            proc.processBlock (buf, midi);
            auto* badge = findChild<ui::KeyBadge> (*editor);
            if (badge == nullptr)
            {
                fail ("no KEY badge");
            }
            else
            {
                ed->refreshWaveform(); // follows the note first, as the editor's timer does
                if (! badge->isTransposed() || ! badge->getText().contains ("C1") || ! badge->getText().contains ("24 st"))
                    fail ("KEY badge doesn't show C1, -24 st: " + badge->getText());
                setParam (proc, pid::keytrack, 0.0f);
                ed->refreshWaveform(); // follows the note first, as the editor's timer does
                if (badge->isTransposed() || badge->getText() != "KEY OFF")
                    fail ("KEY badge doesn't follow KEY off");
                setParam (proc, pid::keytrack, 1.0f);
                proc.setDisplayNote (60);
            }
            ed->setBrowserVisible (true);
        }
        else if (round == 2)
        {
            // Loading a preset forgets the muted layer's tab.
            proc.getPresets().load (0);
            if (! layers->hasTab (0))
                fail ("muted NOISE 2 lost its tab before the load reached the editor");
            ed->setBrowserVisible (false);
        }
        else
        {
            if (layers->hasTab (0) || layers->hasTab (1) || layers->hasTab (2) || layers->getSelectedTab() != LS::noiseTab)
                fail ("after loading Init the extra tabs are still there");
        }
        editor = nullptr;
    }

    std::printf ("editor: layers, tabs, mute, add / remove, relabelling, browser, KEY badge, reopen %s\n",
                 failures == 0 ? "ok" : "FAILED");
    return failures;
}

/** The editor's newer controls: solo buttons (and the implicit mute they give the
    other layers), the pitch axis' FOCUS dial, dragging the pitch curve's end point
    (moves Tune, keeps the other points' absolute pitch), dragging amplitude over
    0 dB, dragging a timing lane (a layer's Offset) and resetting it, the clip mode
    shown in two places, Clap in the click menus, and the settings page. Every round
    closes and reopens the editor. */
int checkEditorControls()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: editor controls: %s\n", msg.toRawUTF8()); ++failures; };

    DrumFoundryProcessor proc;
    proc.prepareToPlay (48000.0, 512);
    using LS = ui::LayersSection;

    const auto updateEditor = [] (juce::AudioProcessorEditor& editor) // as the editor's timer would
    {
        if (auto* body = findChild<ui::LayerPanel> (editor))
            body->update();
        if (auto* layers = findChild<LS> (editor))
            layers->update();
        if (auto* canvas = findChild<ui::EnvelopeCanvas> (editor))
            canvas->updateLanes();
    };
    const auto drag = [] (juce::Component& c, juce::Point<float> from, juce::Point<float> to, int steps = 6)
    {
        c.mouseDown (mouseEventAt (c, from));
        for (int i = 1; i <= steps; ++i)
            c.mouseDrag (mouseEventAt (c, from + (to - from) * ((float) i / (float) steps), {}, 1, from, true));
        c.mouseUp (mouseEventAt (c, to, {}, 1, from, true));
    };
    const auto comboBoxes = [] (juce::Component& root)
    {
        std::vector<juce::ComboBox*> out;
        std::function<void (juce::Component&)> find = [&] (juce::Component& c)
        {
            for (auto* child : c.getChildren())
            {
                if (auto* box = dynamic_cast<juce::ComboBox*> (child))
                    out.push_back (box);
                find (*child);
            }
        };
        find (root);
        return out;
    };

    for (int round = 0; round < 3; ++round)
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        auto* ed = dynamic_cast<DrumFoundryEditor*> (editor.get());
        auto* layers = findChild<LS> (*editor);
        auto* body = findChild<ui::LayerPanel> (*editor);
        auto* display = findChild<ui::DisplayPanel> (*editor);
        auto* canvas = findChild<ui::EnvelopeCanvas> (*editor);
        if (ed == nullptr || layers == nullptr || body == nullptr || display == nullptr || canvas == nullptr
            || body->getSoloButton() == nullptr)
        {
            fail ("no editor / layers / body / display / canvas / body solo");
            return failures;
        }

        if (round == 0)
        {
            // --- Solo -----------------------------------------------------------------
            layers->getSoloButton (LS::noiseTab).onClick();
            updateEditor (*editor);
            if (rawParam (proc, pid::noiseSolo) < 0.5f || ! layers->getSoloButton (LS::noiseTab).isSoloed())
                fail ("S on the NOISE tab didn't solo Noise");
            if (! body->isImplicitlyMuted() || ! body->getMuteButton()->isImplicitlyMuted()
                || ! layers->getMuteButton (LS::clickTab).isImplicitlyMuted() || layers->getMuteButton (LS::noiseTab).isImplicitlyMuted())
                fail ("soloing Noise didn't show Body and Click (only) as silenced");
            if (rawParam (proc, pid::bodyOn) < 0.5f)
                fail ("solo switched Body off");
            // Solo is exclusive: soloing Body takes it from Noise.
            body->getSoloButton()->onClick();
            updateEditor (*editor);
            if (rawParam (proc, pid::bodySolo) < 0.5f || body->isImplicitlyMuted())
                fail ("S on Body didn't solo it / it still reads silenced");
            if (rawParam (proc, pid::noiseSolo) > 0.5f || ! layers->getMuteButton (LS::noiseTab).isImplicitlyMuted())
                fail ("soloing Body didn't take the solo from Noise");
            if (ed->addExtraLayer (2) != 0)
                fail ("couldn't add a Noise layer");
            layers->getSoloButton (LS::firstExtraTab).onClick();
            updateEditor (*editor);
            if (rawParam (proc, "lay1Solo") < 0.5f || layers->getMuteButton (LS::firstExtraTab).isImplicitlyMuted()
                || rawParam (proc, pid::bodySolo) > 0.5f || ! body->isImplicitlyMuted())
                fail ("S on the NOISE 2 tab didn't solo it alone");

            // Shift- or Cmd-click adds to the soloed layers.
            juce::ModifierKeys::currentModifiers = juce::ModifierKeys (juce::ModifierKeys::shiftModifier);
            layers->getSoloButton (LS::clickTab).onClick();
            juce::ModifierKeys::currentModifiers = juce::ModifierKeys (juce::ModifierKeys::commandModifier);
            body->getSoloButton()->onClick();
            juce::ModifierKeys::currentModifiers = juce::ModifierKeys();
            updateEditor (*editor);
            if (rawParam (proc, "lay1Solo") < 0.5f || rawParam (proc, pid::clickSolo) < 0.5f || rawParam (proc, pid::bodySolo) < 0.5f
                || body->isImplicitlyMuted() || ! layers->getMuteButton (LS::noiseTab).isImplicitlyMuted())
                fail ("Shift- / Cmd-click didn't add Click and Body to the soloed layers");

            // A plain click on a soloed layer unsolos just that one; on another, it takes every solo.
            layers->getSoloButton (LS::clickTab).onClick();
            if (rawParam (proc, pid::clickSolo) > 0.5f || rawParam (proc, "lay1Solo") < 0.5f || rawParam (proc, pid::bodySolo) < 0.5f)
                fail ("unsoloing Click changed the other solos");
            layers->getSoloButton (LS::noiseTab).onClick();
            if (rawParam (proc, pid::noiseSolo) < 0.5f || rawParam (proc, "lay1Solo") > 0.5f || rawParam (proc, pid::bodySolo) > 0.5f)
                fail ("soloing Noise didn't take the solo from NOISE 2 and Body");
            layers->getSoloButton (LS::noiseTab).onClick();
            updateEditor (*editor);
            for (const auto& id : ui::soloParameterIds())
                if (rawParam (proc, id.toRawUTF8()) > 0.5f)
                    fail ("a solo is still on: " + id);
            if (body->isImplicitlyMuted() || layers->getMuteButton (LS::clickTab).isImplicitlyMuted())
                fail ("unsoloing everything left a layer silenced");

            // --- Focus ----------------------------------------------------------------
            showCurve (*display, envPitch);
            display->getFocusDial().getSlider().setValue (0.0, juce::sendNotificationSync);
            const auto full = canvas->getPitchViewHz();
            if (std::abs (full.getStart()) > 0.01f || std::abs (full.getEnd() - 16000.0f) > 100.0f)
                fail ("Focus 0 doesn't show 0 Hz - 16 kHz: " + juce::String (full.getStart()) + " - " + juce::String (full.getEnd()));
            // The band is around the pitch curve on show: Body's.
            display->getFocusDial().getSlider().setValue (1.0, juce::sendNotificationSync);
            const auto band = canvas->getPitchViewHz();
            const float tune = rawParam (proc, pid::tune);
            if (! juce::approximatelyEqual (proc.uiState.pitchFocus, 1.0f) || band.getEnd() / band.getStart() > 16.0f
                || band.getStart() > tune || band.getEnd() < tune)
                fail ("Focus 1 isn't a band around the curve (with Tune in it), or wasn't stored: "
                      + juce::String (band.getStart()) + " - " + juce::String (band.getEnd()));
            display->getFocusDial().getSlider().setValue (0.5, juce::sendNotificationSync); // left for the reopened editor

            // --- The end point is Tune -----------------------------------------------------
            const auto before = proc.getEnvelope (envPitch);
            const int last = before.size() - 1;
            const float firstHz = tune * std::exp2 (before.getPoints()[0].y / 12.0f);
            const auto endPos = canvas->getPointPosition (last);
            drag (*canvas, endPos, endPos.translated (0.0f, -25.0f));
            const float newTune = rawParam (proc, pid::tune);
            const auto after = proc.getEnvelope (envPitch);
            const float firstHzAfter = newTune * std::exp2 (after.getPoints()[0].y / 12.0f);
            if (! (newTune > tune * 1.05f))
                fail ("dragging the end point up didn't raise Tune: " + juce::String (tune) + " -> " + juce::String (newTune));
            if (std::abs (firstHzAfter / firstHz - 1.0f) > 0.005f)
                fail ("the first pitch point moved with Tune: " + juce::String (firstHz) + " -> " + juce::String (firstHzAfter) + " Hz");
            if (! juce::approximatelyEqual (after.getPoints()[(size_t) last].y, 0.0f) || ! (after == canvas->getEnvelope (envPitch)))
                fail ("after the end-point drag the end isn't 0 st, or the canvas and processor disagree");

            // A middle point lands at the absolute pitch it's dragged to.
            if (after.size() > 2)
            {
                const auto midPos = canvas->getPointPosition (1);
                drag (*canvas, midPos, midPos.translated (0.0f, -15.0f));
                const float hz = newTune * std::exp2 (proc.getEnvelope (envPitch).getPoints()[1].y / 12.0f);
                if (! (hz > newTune * std::exp2 (after.getPoints()[1].y / 12.0f)) || ! juce::approximatelyEqual (rawParam (proc, pid::tune), newTune))
                    fail ("dragging a middle point up didn't raise its pitch, or moved Tune");
            }

            // --- Amplitude over 0 dB ------------------------------------------------------
            showCurve (*display, envBody);
            const auto p0 = canvas->getPointPosition (0);
            drag (*canvas, p0, p0.translated (0.0f, -60.0f));
            const float gain = proc.getEnvelope (envBody).getPoints()[0].y;
            if (! (gain > 1.5f && gain <= kMaxEnvGain))
                fail ("dragging the body's first point up didn't go over 0 dB: " + juce::String (gain));

            // --- Offset lanes ----------------------------------------------------------------
            canvas->updateLanes();
            int clickLane = -1;
            for (int i = 0; i < canvas->getNumLanes(); ++i)
                if (canvas->getLaneParamId (i) == pid::clickOffset)
                    clickLane = i;
            if (clickLane < 0 || canvas->getNumLanes() != 4) // Body, Noise, Click, NOISE 2
            {
                fail ("no Click lane, or not one lane per layer: " + juce::String (canvas->getNumLanes()));
            }
            else
            {
                const auto lane = canvas->getLaneBounds (clickLane);
                const juce::Point<float> from (lane.getCentreX(), lane.getCentreY());
                const float pxPerMs = canvas->msToX (1.0f) - canvas->msToX (0.0f);
                drag (*canvas, from, from.translated (-12.0f * pxPerMs, 0.0f));
                const float offset = rawParam (proc, pid::clickOffset);
                if (std::abs (offset + 12.0f) > 0.5f)
                    fail ("dragging the Click lane 12 ms left didn't give -12 ms: " + juce::String (offset));
                if (canvas->msToX (-12.0f) < canvas->getLaneBounds (clickLane).getX() - 1.0f)
                    fail ("the view doesn't reach back to the Click lane's early start");
                drag (*canvas, from, from.translated (12.0f * pxPerMs + 2.0f, 0.0f)); // within the snap: back on 0
                if (std::abs (rawParam (proc, pid::clickOffset)) > 0.01f)
                    fail ("a lane dragged next to 0 ms didn't snap to it");
                setParam (proc, pid::clickOffset, 20.0f);
                canvas->updateLanes();
                canvas->mouseDoubleClick (mouseEventAt (*canvas, from, {}, 2));
                if (std::abs (rawParam (proc, pid::clickOffset)) > 0.01f)
                    fail ("double-clicking a lane didn't reset its offset");
            }

            // --- Clip mode in two places, and Clap in the click menus -------------------------
            setParam (proc, pid::clip, 2.0f); // Hard
            int showingHard = 0;
            for (auto* box : comboBoxes (*editor))
                if (box->getText() == "Hard")
                    ++showingHard;
            if (showingHard < 2 || display->getClipBox().getSelectedId() != 3)
                fail ("the clip mode isn't shown in the display and the Output section");
            int clapMenus = 0;
            for (auto* box : comboBoxes (*editor))
                for (int i = 0; i < box->getNumItems(); ++i)
                    if (box->getItemText (i) == "Clap")
                        ++clapMenus;
            if (clapMenus < 2) // the Click page, and the extra layer's click types
                fail ("Clap isn't in the click type menus");

            // --- Settings --------------------------------------------------------------------
            ed->setSettingsVisible (true);
            auto* settings = findChild<ui::SettingsOverlay> (*editor);
            if (! ed->isSettingsVisible() || settings == nullptr || findChild<ui::ToggleSwitch> (*settings) == nullptr)
                fail ("the settings don't open, or don't hold the sidechain switch");
            else if (settings->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)); ed->isSettingsVisible())
                fail ("Escape didn't close the settings");
        }
        else
        {
            // Reopened: Focus is where it was left.
            if (! juce::approximatelyEqual (canvas->getPitchFocus(), 0.5f)
                || ! juce::approximatelyEqual ((float) display->getFocusDial().getSlider().getValue(), 0.5f))
                fail ("the reopened editor lost the Focus setting");
            if (ed->isSettingsVisible())
                fail ("the reopened editor opened with the settings showing");
        }
        editor = nullptr;
    }

    std::printf ("editor controls: solo, focus, end point = Tune, over 0 dB, offset lanes, clip mode, Clap, settings, reopen %s\n",
                 failures == 0 ? "ok" : "FAILED");
    return failures;
}

/** The display's curves and what goes with them: the Pitch / Level tabs and their line
    buttons (each tab keeps its own), picking a curve by clicking it, the chart following
    the layer the user touches (and the setting that stops it), the pitch axis down to
    0 Hz, the Noise and Click pitch curves (their edits, their end points, undo), the
    reverb gate marker's drag, an extra Body layer, the polarity (Ø) buttons, and all of
    it surviving the editor closing and a session round trip. */
/** The display's ALL: with it on, the other playing layers' curves are clickable and a
    click on one shows that layer; with it off, the same clicks change nothing. */
/** Every choice drop-down shows its parameter's choice, through preset loads in a row (the
    Body selector once showed Taper for 808 Kick's 808: its grouped list was matched by
    position), and picking an item sets that item's choice. */
int checkChoiceSelectors()
{
    int failures = 0, checked = 0;
    DrumFoundryProcessor proc;
    proc.prepareToPlay (48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    std::vector<ui::ChoiceSelector*> selectors;
    std::function<void (juce::Component&)> collect = [&] (juce::Component& c)
    {
        if (auto* sel = dynamic_cast<ui::ChoiceSelector*> (&c))
            selectors.push_back (sel);
        for (auto* child : c.getChildren())
            collect (*child);
    };
    collect (*editor);

    const auto verify = [&] (const juce::String& when)
    {
        for (auto* sel : selectors)
        {
            auto* param = sel->getParameter();
            const auto& box = sel->getComboBox();
            if (param == nullptr || box.getNumItems() == 0)
                continue;
            ++checked;
            const auto want = param->getCurrentChoiceName();
            const bool listed = box.indexOfItemId (param->getIndex() + 1) >= 0;
            if (listed && box.getText() != want)
            {
                std::printf ("FAIL: choice selectors: %s shows %s, not %s (%s)\n", param->paramID.toRawUTF8(),
                             box.getText().toRawUTF8(), want.toRawUTF8(), when.toRawUTF8());
                ++failures;
            }
        }
    };

    auto& presets = proc.getPresets();
    juce::StringArray order { "Tapered Kick", "808 Kick", "Psy Kick", "909 Kick", "FM Snare", "DnB Snare", "Init" };
    for (const auto& name : order)
        for (int i = 0; i < (int) presets.getEntries().size(); ++i)
            if (presets.getEntries()[(size_t) i].name == name)
            {
                presets.load (i);
                verify ("after loading " + name);
            }

    // Picking each listed item sets exactly that choice.
    for (auto* sel : selectors)
    {
        auto* param = sel->getParameter();
        auto& box = sel->getComboBox();
        if (param == nullptr)
            continue;
        for (int k = 0; k < box.getNumItems(); ++k)
        {
            const int id = box.getItemId (k);
            box.setSelectedId (id, juce::sendNotificationSync);
            if (param->getIndex() != id - 1)
            {
                std::printf ("FAIL: choice selectors: picking %s in %s set %s\n", box.getItemText (k).toRawUTF8(),
                             param->paramID.toRawUTF8(), param->getCurrentChoiceName().toRawUTF8());
                ++failures;
                break;
            }
        }
    }
    if (failures == 0)
        std::printf ("choice selectors: %zu drop-downs show their parameter through preset loads (%d checks), and picking sets it\n",
                     selectors.size(), checked);
    return failures;
}

/** Every factory preset loads with exactly the values it lists (a choice, such as Body
    Style, to the entry; a number to within its range's rounding). */
int checkPresetValues()
{
    int failures = 0, checked = 0;
    DrumFoundryProcessor proc;
    proc.prepareToPlay (48000.0, 512);
    auto& presets = proc.getPresets();
    const auto& factory = factoryPresets();
    for (int i = 0; i < (int) presets.getEntries().size(); ++i)
    {
        const auto& entry = presets.getEntries()[(size_t) i];
        if (! entry.isFactory) // a user preset can share a factory preset's name and differ
            continue;
        const FactoryPreset* def = nullptr;
        for (const auto& f : factory)
            if (entry.name == f.name)
                def = &f;
        if (def == nullptr)
            continue;
        presets.load (i);
        for (const auto& [id, want] : def->values)
        {
            auto* param = dynamic_cast<juce::RangedAudioParameter*> (proc.getState().getParameter (id));
            if (param == nullptr)
            {
                std::printf ("FAIL: preset values: %s lists %s, which isn't a parameter\n", def->name, id);
                ++failures;
                continue;
            }
            const float got = proc.getState().getRawParameterValue (id)->load();
            const auto& range = param->getNormalisableRange();
            float expected = range.snapToLegalValue (want);
            // An extra layer coupled to a core layer's curve loads on its own copy of it.
            const juce::String idText (id);
            if (idText.startsWith ("lay") && (idText.endsWith (pid::layEnv) || idText.endsWith (pid::layPitchEnv))
                && juce::roundToInt (expected) >= layerEnvBody && juce::roundToInt (expected) <= layerEnvClick)
                expected = (float) layerEnvOwn;
            const bool choice = dynamic_cast<juce::AudioParameterChoice*> (param) != nullptr
                             || dynamic_cast<juce::AudioParameterBool*> (param) != nullptr;
            const float tolerance = choice ? 0.0f : 1.0e-3f * std::max (1.0f, std::abs (expected));
            ++checked;
            if (std::abs (got - expected) > tolerance)
            {
                std::printf ("FAIL: preset values: %s loads %s = %s, not the %s it lists\n", def->name, id,
                             param->getText (param->convertTo0to1 (got), 32).toRawUTF8(),
                             param->getText (param->convertTo0to1 (expected), 32).toRawUTF8());
                ++failures;
            }
        }
    }
    if (failures == 0)
        std::printf ("preset values: every factory preset loads the %d values it lists\n", checked);
    return failures;
}

/** Each envelope curve, pitch and level alike, is drawn in its layer's colour: Body cyan,
    Noise pink, Click yellow, the extra layers green. (Body pitch was once drawn orange.) */
int checkChartColours()
{
    int failures = 0;
    const std::pair<EnvId, juce::Colour> expected[] {
        { envPitch, ui::colours::body },        { envBody, ui::colours::body },
        { envNoise, ui::colours::noise },       { envNoisePitch, ui::colours::noise },
        { envClickPitch, ui::colours::click },  { envClick, ui::colours::click },
        { envLayer1Pitch, ui::colours::layer }, { envLayer1Level, ui::colours::layer },
        { envLayer2Pitch, ui::colours::layer }, { envLayer2Level, ui::colours::layer },
        { envLayer3Pitch, ui::colours::layer }, { envLayer3Level, ui::colours::layer },
    };
    static_assert (std::size (expected) == (size_t) numEnvs, "a new envelope needs its layer colour here");
    for (const auto& [env, colour] : expected)
        if (ui::colours::forEnvelope (env) != colour)
        {
            std::printf ("FAIL: chart colours: envelope %d is drawn in %s, not its layer's %s\n", (int) env,
                         ui::colours::forEnvelope (env).toDisplayString (false).toRawUTF8(),
                         colour.toDisplayString (false).toRawUTF8());
            ++failures;
        }
    // The display draws the selected layer's curve in that layer's colour, grouped or not.
    const juce::Colour layerColours[] { ui::colours::body, ui::colours::noise, ui::colours::click,
                                        ui::colours::layer, ui::colours::layer, ui::colours::layer };
    for (int l = 0; l < (int) std::size (layerColours); ++l)
        if (ui::colours::forLayer (l) != layerColours[l])
        {
            std::printf ("FAIL: chart colours: chart layer %d is drawn in %s\n", l, ui::colours::forLayer (l).toDisplayString (false).toRawUTF8());
            ++failures;
        }
    if (failures == 0)
        std::printf ("chart colours: every pitch and level curve is its layer's colour\n");
    return failures;
}

int checkChartAll()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: chart ALL: %s\n", msg.toRawUTF8()); ++failures; };

    DrumFoundryProcessor proc;
    proc.prepareToPlay (48000.0, 512);
    proc.getPresets().load (3); // a snare with Body, Noise and Click all playing
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    auto* display = findChild<ui::DisplayPanel> (*editor);
    auto* canvas = display != nullptr ? findChild<ui::EnvelopeCanvas> (*display) : nullptr;
    if (canvas == nullptr)
    {
        fail ("no display");
        return failures;
    }

    for (const bool all : { false, true })
    {
        auto& allButton = display->getAllButton();
        allButton.setToggleState (all, juce::dontSendNotification); // (triggerClick() is asynchronous)
        if (allButton.onClick)
            allButton.onClick();
        for (int tab : { (int) ui::EnvelopeCanvas::pitchTab, (int) ui::EnvelopeCanvas::levelTab })
        {
            display->focusLayer (0, true);
            display->showTab (tab);
            // Click down a column of the plot, away from the Body curve's points, until something changes.
            const auto b = canvas->getLocalBounds().toFloat();
            const float x = b.getX() + b.getWidth() * 0.45f;
            int picked = 0;
            for (float y = b.getY() + 4.0f; y < b.getBottom() - 40.0f && picked == 0; y += 2.0f)
            {
                canvas->mouseDown (mouseEventAt (*canvas, { x, y }));
                canvas->mouseUp (mouseEventAt (*canvas, { x, y }));
                picked = display->getLayer();
            }
            const auto tag = juce::String (tab == ui::EnvelopeCanvas::pitchTab ? "pitch" : "level") + (all ? ", ALL on" : ", ALL off");
            if (all && picked == 0)
                fail (tag + ": clicking down the plot never picked another layer's curve");
            if (! all && picked != 0)
                fail (tag + ": a click picked layer " + juce::String (picked) + " with ALL off");
            if (all && picked != 0)
                std::printf ("chart ALL, %s: a click on a dimmed curve shows layer %d\n", tag.toRawUTF8(), picked);
        }
    }
    if (! proc.uiState.chartShowAll)
        fail ("ALL isn't remembered in UiState");
    return failures;
}

int checkEditorChart()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: editor chart: %s\n", msg.toRawUTF8()); ++failures; };

    DrumFoundryProcessor proc;
    proc.prepareToPlay (48000.0, 512);
    using LS = ui::LayersSection;
    using EC = ui::EnvelopeCanvas;

    const auto drag = [] (juce::Component& c, juce::Point<float> from, juce::Point<float> to, int steps = 6)
    {
        c.mouseDown (mouseEventAt (c, from));
        for (int i = 1; i <= steps; ++i)
            c.mouseDrag (mouseEventAt (c, from + (to - from) * ((float) i / (float) steps), {}, 1, from, true));
        c.mouseUp (mouseEventAt (c, to, {}, 1, from, true));
    };
    const auto knobWithCaption = [] (juce::Component& root, const juce::String& caption)
    {
        std::function<ui::Knob* (juce::Component&)> find = [&] (juce::Component& c) -> ui::Knob*
        {
            for (auto* child : c.getChildren())
            {
                if (auto* k = dynamic_cast<ui::Knob*> (child); k != nullptr && k->getCaption() == caption && k->isVisible())
                    return k;
                if (! child->isVisible())
                    continue;
                if (auto* k = find (*child))
                    return k;
            }
            return nullptr;
        };
        return find (root);
    };
    const auto near = [] (float a, float b, float tolerance) { return std::abs (a - b) <= tolerance; };

    int savedActive = envPitch;

    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        auto* ed = dynamic_cast<DrumFoundryEditor*> (editor.get());
        auto* layers = findChild<LS> (*editor);
        auto* body = findChild<ui::LayerPanel> (*editor);
        auto* display = findChild<ui::DisplayPanel> (*editor);
        auto* canvas = findChild<EC> (*editor);
        if (ed == nullptr || layers == nullptr || body == nullptr || display == nullptr || canvas == nullptr)
        {
            fail ("no editor / layers / body / display / canvas");
            return failures;
        }
        const auto active = [&] { return canvas->getActiveEnvelope(); };
        const auto name = [] (int env) { return env >= 0 ? EC::curveName (env) : juce::String ("none"); };

        // --- A fresh editor: Focus fully left (the whole axis), Body pitch ------------------------
        if (! juce::approximatelyEqual (canvas->getPitchFocus(), 0.0f) || ! juce::approximatelyEqual (display->getFocusDial().getSlider().getValue(), 0.0))
            fail ("a fresh editor's Focus isn't fully left");
        if (display->getTab() != EC::pitchTab || active() != envPitch || ! display->getTabButton (EC::pitchTab).getToggleState()
            || display->getLayer() != EC::bodyLayer || display->getSourceBox().isVisible())
            fail ("a fresh editor isn't on the Pitch tab, editing Body pitch (with no curve source menu)");

        // --- The pitch axis reaches 0 Hz ---------------------------------------------------------
        {
            const auto plot = canvas->getPlotArea();
            const auto view = canvas->getPitchViewHz();
            if (! near (view.getStart(), 0.0f, 1.0e-3f) || ! near (view.getEnd(), 16000.0f, 1.0f))
                fail ("the full pitch axis isn't 0 Hz - 16 kHz: " + juce::String (view.getStart()) + " - " + juce::String (view.getEnd()));
            if (! near (canvas->pitchYToHz (plot.getBottom()), 0.0f, 1.0e-3f) || ! near (canvas->pitchHzToY (0.0f), plot.getBottom(), 0.01f))
                fail ("the bottom of the pitch axis isn't 0 Hz");
            for (float hz : { 0.0f, 3.0f, 20.0f, 55.0f, 440.0f, 1000.0f, 9000.0f, 16000.0f })
                if (const float back = canvas->pitchYToHz (canvas->pitchHzToY (hz)); ! near (back, hz, 1.0e-3f * juce::jmax (1.0f, hz)))
                    fail ("pitch axis round trip: " + juce::String (hz) + " Hz -> " + juce::String (back));
            // Log-like above ~100 Hz (an octave takes about the same height), linear-ish near 0.
            const float octLow = canvas->pitchHzToY (200.0f) - canvas->pitchHzToY (400.0f);
            const float octHigh = canvas->pitchHzToY (4000.0f) - canvas->pitchHzToY (8000.0f);
            if (! (octLow > 0.0f && std::abs (octLow / octHigh - 1.0f) < 0.12f))
                fail ("the pitch axis isn't an octave scale above 200 Hz: " + juce::String (octLow) + " vs " + juce::String (octHigh) + " px");
            if (! (canvas->pitchHzToY (0.0f) - canvas->pitchHzToY (10.0f) > 0.5f * (canvas->pitchHzToY (10.0f) - canvas->pitchHzToY (20.0f))))
                fail ("the pitch axis squeezes 0 - 10 Hz like a log scale");

            // A point dragged to the bottom lands on (about) 0 Hz, and is drawn there.
            auto pitch = proc.getEnvelope (envPitch);
            if (pitch.size() < 3)
            {
                pitch.addPoint (0.3f, 5.0f);
                proc.setEnvelope (envPitch, pitch);
                canvas->reloadFromProcessor();
            }
            const auto pos = canvas->getPointPosition (1);
            drag (*canvas, pos, { pos.x, plot.getBottom() + 40.0f });
            const float st = proc.getEnvelope (envPitch).getPoints()[1].y;
            const float hz = rawParam (proc, pid::tune) * std::exp2 (st / 12.0f);
            if (! (st <= -kPitchRangeSt + 0.01f) || hz > 0.5f)
                fail ("a pitch point dragged to the bottom isn't at 0 Hz: " + juce::String (st) + " st, " + juce::String (hz) + " Hz");
            if (! near (canvas->getPointPosition (1).y, plot.getBottom(), 1.5f))
                fail ("a pitch point at ~0 Hz isn't drawn at the bottom of the axis");
            proc.setEnvelope (envPitch, pitch);
            canvas->reloadFromProcessor();
        }

        // --- Tabs, and the layer on show ------------------------------------------------------------
        display->showTab (EC::levelTab);
        if (active() != envBody || ! display->getTabButton (EC::levelTab).getToggleState() || display->getTabButton (EC::pitchTab).getToggleState())
            fail ("the Level tab doesn't edit Body level");
        display->focusLayer (EC::noiseLayer);
        if (active() != envNoise || proc.uiState.chartLayer != EC::noiseLayer || proc.uiState.chartTab != EC::levelTab)
            fail ("showing Noise on the Level tab doesn't edit Noise level: " + name (active()));
        display->showTab (EC::pitchTab);
        if (active() != envNoisePitch || display->getSourceBox().isVisible())
            fail ("Noise on the Pitch tab doesn't edit Noise pitch (or shows a source menu): " + name (active()));

        // --- The chart follows the layer you touch -----------------------------------------------
        display->showTab (EC::levelTab);
        ed->touchLayer (LS::clickTab);
        if (active() != envClick || layers->getSelectedTab() != LS::clickTab)
            fail ("touching the CLICK tab didn't show and edit Click's level: " + name (active()));
        ed->touchLayer (-1);
        if (active() != envBody)
            fail ("touching Body didn't edit Body's curves");
        ed->touchLayer (LS::clickTab);
        display->showTab (EC::pitchTab);
        if (active() != envClickPitch)
            fail ("after touching CLICK, the Pitch tab doesn't edit Click pitch: " + name (active()));
        ed->touchLayer (-1);
        display->showTab (EC::levelTab);

        const float noiseLow0 = rawParam (proc, pid::noiseHP), tune0 = rawParam (proc, pid::tune);

        // --- An extra layer: its own curves, coupling, Flat -----------------------------------------
        {
            const auto noiseLevel = proc.getEnvelope (envNoise);
            const auto noisePitch = proc.getEnvelope (envNoisePitch);
            const int k = ed->addExtraLayer (layerNoise);
            if (k != 0)
                fail ("couldn't add a Noise layer");
            const int ownLevel = layerOwnLevelEnvelope (0), ownPitch = layerOwnPitchEnvelope (0);
            if (! juce::approximatelyEqual (rawParam (proc, "lay1Env"), (float) layerEnvOwn)
                || ! juce::approximatelyEqual (rawParam (proc, "lay1PitchEnv"), (float) layerEnvOwn))
                fail ("a new layer doesn't play its own curves");
            if (proc.getEnvelope ((EnvId) ownLevel).getPoints().size() != noiseLevel.getPoints().size()
                || ! juce::approximatelyEqual (proc.getEnvelope ((EnvId) ownLevel).getPoints()[1].y, noiseLevel.getPoints()[1].y)
                || proc.getEnvelope ((EnvId) ownPitch).getPoints().size() != noisePitch.getPoints().size())
                fail ("a new Noise layer's own curves don't start as the Noise layer's");

            ed->touchLayer (LS::firstExtraTab);
            ed->refreshWaveform();
            auto& box = display->getSourceBox();
            if (active() != ownLevel || canvas->isCoupled() || ! box.isVisible() || box.getSelectedId() != layerEnvOwn + 1
                || canvas->getCurveName() != "Noise 2 level")
                fail ("touching NOISE 2 didn't show its own level curve with OWN in the source menu: " + name (active())
                      + ", \"" + canvas->getCurveName() + "\"");

            // Editing it changes the layer's curve alone.
            const auto p1 = canvas->getPointPosition (1);
            drag (*canvas, p1, p1.translated (0.0f, -30.0f));
            if (proc.getEnvelope ((EnvId) ownLevel) == noiseLevel || ! (proc.getEnvelope (envNoise) == noiseLevel))
                fail ("dragging NOISE 2's level point didn't change its own curve (or changed Noise's)");

            // Coupled to Body: it shows (and edits) the Body level curve.
            box.setSelectedId (layerEnvBody + 1, juce::sendNotificationSync);
            if (! juce::approximatelyEqual (rawParam (proc, "lay1Env"), (float) layerEnvBody) || active() != envBody || ! canvas->isCoupled())
                fail ("BODY in the source menu didn't couple NOISE 2's level to Body's: " + name (active()));
            if (! juce::approximatelyEqual (rawParam (proc, "lay1PitchEnv"), (float) layerEnvOwn))
                fail ("coupling the level changed the pitch curve's source");

            // Back to Own: its curve starts as a copy of Body's.
            box.setSelectedId (layerEnvOwn + 1, juce::sendNotificationSync);
            const auto own = proc.getEnvelope ((EnvId) ownLevel), bodyLevel = proc.getEnvelope (envBody);
            bool same = own.size() == bodyLevel.size();
            for (int i = 0; same && i < own.size(); ++i)
                same = juce::approximatelyEqual (own.getPoints()[(size_t) i].y, bodyLevel.getPoints()[(size_t) i].y);
            if (active() != ownLevel || ! same || ! (canvas->getEnvelope ((EnvId) ownLevel) == own))
                fail ("switching from Body to OWN didn't start the own curve as a copy of Body's");

            // Flat: no curve to edit.
            box.setSelectedId (layerEnvFlat + 1, juce::sendNotificationSync);
            if (active() != -1)
                fail ("on Flat, the display still edits a curve: " + name (active()));
            const auto plot = canvas->getPlotArea();
            const auto before = proc.getEnvelopeEditCount();
            canvas->mouseDoubleClick (mouseEventAt (*canvas, plot.getCentre(), {}, 2));
            drag (*canvas, plot.getCentre(), plot.getCentre().translated (0.0f, -20.0f));
            if (proc.getEnvelopeEditCount() != before)
                fail ("on Flat, the display still edits a curve");
            box.setSelectedId (layerEnvOwn + 1, juce::sendNotificationSync);

            // Its own pitch curve's end point is its band: dragging it moves its Low and High Cut together.
            display->showTab (EC::pitchTab);
            if (active() != ownPitch || box.getSelectedId() != layerEnvOwn + 1)
                fail ("NOISE 2's Pitch tab doesn't edit its own pitch curve: " + name (active()));
            setParam (proc, "lay1HP", 1000.0f);
            setParam (proc, "lay1LP", 8000.0f);
            canvas->updateLanes(); // as the editor's timer does: it re-reads the parameters
            const float lo0 = rawParam (proc, "lay1HP"), hi0 = rawParam (proc, "lay1LP");
            if (! near (canvas->getBaseHz (ownPitch), std::sqrt (lo0 * hi0), 0.5f))
                fail ("NOISE 2's pitch base isn't its band's centre");
            const auto end = canvas->getPointPosition (proc.getEnvelope ((EnvId) ownPitch).size() - 1);
            drag (*canvas, end, end.translated (0.0f, -12.0f));
            const float lo1 = rawParam (proc, "lay1HP"), hi1 = rawParam (proc, "lay1LP");
            if (! (lo1 > lo0 * 1.03f) || ! near (lo1 / lo0, hi1 / hi0, 0.01f) || ! juce::approximatelyEqual (rawParam (proc, pid::noiseHP), noiseLow0))
                fail ("dragging NOISE 2's pitch end point up didn't move its band up (alone): " + juce::String (lo0) + "/" + juce::String (hi0)
                      + " -> " + juce::String (lo1) + "/" + juce::String (hi1));

            // A Sine layer's end point is its Tune.
            setParam (proc, "lay1Source", (float) layerSine);
            canvas->updateLanes();
            display->update();
            const float st0 = rawParam (proc, "lay1Tune");
            const auto sineEnd = canvas->getPointPosition (proc.getEnvelope ((EnvId) ownPitch).size() - 1);
            drag (*canvas, sineEnd, sineEnd.translated (0.0f, -20.0f));
            const float st1 = rawParam (proc, "lay1Tune");
            if (! (st1 > st0 + 0.5f) || ! juce::approximatelyEqual (rawParam (proc, pid::tune), tune0))
                fail ("dragging a Sine layer's pitch end point up didn't raise its Tune (alone): " + juce::String (st0) + " -> " + juce::String (st1));
            setParam (proc, "lay1Source", (float) layerNoise);
            setParam (proc, "lay1Tune", st0);
            display->showTab (EC::levelTab);
        }

        // --- Click pitch: an edit, and undo ------------------------------------------------------
        auto& history = proc.getUndoHistory();
        history.clear();
        showCurve (*display, envClickPitch);
        {
            const auto before = proc.getEnvelope (envClickPitch);
            const auto p0 = canvas->getPointPosition (0);
            history.noteUserActivity();
            drag (*canvas, p0, p0.translated (0.0f, -40.0f));
            history.poll (false);
            const auto after = proc.getEnvelope (envClickPitch);
            if (! (after.getPoints()[0].y > 1.0f) || ! (after == canvas->getEnvelope (envClickPitch)))
                fail ("dragging the Click pitch curve's first point up didn't raise it: " + juce::String (after.getPoints()[0].y) + " st");
            if (! history.canUndo() || ! history.getUndoDescription().containsIgnoreCase ("click pitch"))
                fail ("the Click pitch edit isn't an undo step: \"" + history.getUndoDescription() + "\"");
            ed->undo();
            if (! (proc.getEnvelope (envClickPitch) == before) || ! (canvas->getEnvelope (envClickPitch) == before))
                fail ("undo didn't put the Click pitch curve back (in the processor and on the display)");
            ed->redo();
            if (! (proc.getEnvelope (envClickPitch) == after) || ! (canvas->getEnvelope (envClickPitch) == after))
                fail ("redo didn't bring the Click pitch edit back");
            if (active() != envClickPitch)
                fail ("undo / redo changed the curve being edited");
        }

        // Its end point is Click Tone: dragging it retunes the click, the rest keeps its pitch.
        {
            const auto env0 = proc.getEnvelope (envClickPitch);
            const float tone0 = rawParam (proc, pid::clickTone);
            const float firstHz = tone0 * std::exp2 (env0.getPoints()[0].y / 12.0f);
            const auto end = canvas->getPointPosition (env0.size() - 1);
            drag (*canvas, end, end.translated (0.0f, -20.0f));
            const float tone1 = rawParam (proc, pid::clickTone);
            const auto env1 = proc.getEnvelope (envClickPitch);
            if (! (tone1 > tone0 * 1.05f) || ! near (env1.getPoints().back().y, 0.0f, 1.0e-4f))
                fail ("dragging the Click pitch end point up didn't raise Click Tone: " + juce::String (tone0) + " -> " + juce::String (tone1));
            if (! near (tone1 * std::exp2 (env1.getPoints()[0].y / 12.0f) / firstHz, 1.0f, 0.005f))
                fail ("the Click pitch curve's first point moved with Click Tone");
            if (! near (canvas->getBaseHz (envClickPitch), tone1, 0.01f * tone1))
                fail ("the display's Click pitch base isn't Click Tone");
        }

        // Noise pitch's end point is the noise band: dragging it moves Low and High Cut together.
        showCurve (*display, envNoisePitch);
        {
            const float lo0 = rawParam (proc, pid::noiseHP), hi0 = rawParam (proc, pid::noiseLP);
            if (! near (canvas->getBaseHz (envNoisePitch), std::sqrt (lo0 * hi0), 0.5f))
                fail ("the Noise pitch base isn't the band's centre");
            const auto env0 = proc.getEnvelope (envNoisePitch);
            const auto end = canvas->getPointPosition (env0.size() - 1);
            drag (*canvas, end, end.translated (0.0f, 12.0f));
            const float lo1 = rawParam (proc, pid::noiseHP), hi1 = rawParam (proc, pid::noiseLP);
            if (! (lo1 < lo0 * 0.97f) || ! near (lo1 / lo0, hi1 / hi0, 0.01f))
                fail ("dragging the Noise pitch end point down didn't move the band down: " + juce::String (lo0) + "/" + juce::String (hi0)
                      + " -> " + juce::String (lo1) + "/" + juce::String (hi1));
        }

        // --- The reverb gate marker --------------------------------------------------------------
        showCurve (*display, envBody);
        setParam (proc, pid::revMix, 0.3f);
        setParam (proc, pid::revGate, 1.0f);
        setParam (proc, pid::revGateTime, 180.0f);
        setParam (proc, pid::revGateFade, 15.0f);
        ed->refreshWaveform(); // the marker follows the parameters, as the editor's timer does
        {
            const auto plot = canvas->getPlotArea();
            const float gx = canvas->getGateMarkerX();
            const float pxPerMs = canvas->msToX (1.0f) - canvas->msToX (0.0f);
            if (gx < 0.0f)
            {
                fail ("no gate marker with the gate on and Mix up");
            }
            else
            {
                const float y = plot.getY() + 42.0f;
                history.clear();
                history.noteUserActivity();
                drag (*canvas, { gx, y }, { gx + 40.0f * pxPerMs, y });
                history.poll (false);
                if (! near (rawParam (proc, pid::revGateTime), 220.0f, 1.5f))
                    fail ("dragging the gate 40 ms right didn't set Gate Time to 220 ms: " + juce::String (rawParam (proc, pid::revGateTime)));
                if (history.getUndoDescription() != "Gate Time")
                    fail ("the gate drag isn't an undo step \"Gate Time\": \"" + history.getUndoDescription() + "\"");

                const float fx = canvas->msToX (220.0f + 15.0f);
                drag (*canvas, { fx, y }, { fx + 20.0f * pxPerMs, y });
                if (! near (rawParam (proc, pid::revGateFade), 35.0f, 1.5f) || ! near (rawParam (proc, pid::revGateTime), 220.0f, 0.5f))
                    fail ("dragging the fade's end 20 ms right didn't set Gate Fade to 35 ms (only): " + juce::String (rawParam (proc, pid::revGateFade)));

                canvas->mouseDoubleClick (mouseEventAt (*canvas, { canvas->getGateMarkerX(), y }, {}, 2));
                if (! near (rawParam (proc, pid::revGateTime), 180.0f, 0.5f))
                    fail ("double-clicking the gate didn't reset Gate Time");
            }
            setParam (proc, pid::revMix, 0.0f);
            ed->refreshWaveform();
            if (canvas->getGateMarkerX() >= 0.0f)
                fail ("the gate marker shows with Mix at 0");
        }

        // --- An extra Body layer -----------------------------------------------------------------
        {
            const int mainStyle = juce::roundToInt (rawParam (proc, pid::bodyStyle));
            const int b = ed->addExtraLayer (layerBody);
            const auto tabName = layers->getTabName (LS::firstExtraTab + b);
            const int expected = mainStyle < layerBodyStyleNames.size() ? mainStyle : 0;
            if (b != 1 || ! juce::approximatelyEqual (rawParam (proc, "lay2Source"), (float) layerBody)
                || ! juce::approximatelyEqual (rawParam (proc, "lay2Env"), (float) layerEnvOwn)
                || ! juce::approximatelyEqual (rawParam (proc, "lay2BodyStyle"), (float) expected)
                || ! near (rawParam (proc, "lay2Level"), -6.0f, 0.01f) || rawParam (proc, "lay2On") < 0.5f)
                fail ("a Body layer isn't on / Body / its own curves / the main style / -6 dB");
            if (tabName != "BODY 2" || layers->getSelectedExtraLayer() != 1)
                fail ("the Body layer's tab isn't BODY 2, or isn't shown: " + tabName);
            const auto* shape = bodyShapeCaption (expected);
            for (const char* caption : { "Level", "Tune", "Low Cut", "High Cut", "Pan", "Offset", shape != nullptr ? shape : "Shape" })
                if (knobWithCaption (*layers, caption) == nullptr)
                    fail (juce::String ("the BODY 2 page has no knob captioned ") + caption);
            setParam (proc, "lay2BodyStyle", (float) bodyFM);
            layers->update();
            if (knobWithCaption (*layers, "Index") == nullptr)
                fail ("the Body layer's Shape isn't captioned for FM (Index)");

            // Played by a sample, the main body gives a Classic layer.
            setParam (proc, pid::bodyStyle, (float) bodySample);
            if (ed->addExtraLayer (layerBody) != 2 || ! juce::approximatelyEqual (rawParam (proc, "lay3BodyStyle"), (float) bodyClassic)
                || layers->getTabName (LS::firstExtraTab + 2) != "BODY 3")
                fail ("a Body layer added while the body plays a sample isn't a Classic BODY 3");
            setParam (proc, pid::bodyStyle, (float) mainStyle);

            // Its own curves start as the body's.
            showCurve (*display, envNoise);
            ed->touchLayer (LS::firstExtraTab + 1);
            if (active() != layerOwnLevelEnvelope (1) || ! (proc.getEnvelope ((EnvId) layerOwnLevelEnvelope (1)).getPoints().size()
                                                             == proc.getEnvelope (envBody).getPoints().size()))
                fail ("touching BODY 2 didn't edit its own level curve (a copy of Body's)");
        }

        // --- Polarity (Ø) buttons ---------------------------------------------------------------
        {
            auto& bodyInvert = body->getInvertButton();
            bodyInvert.onClick();
            if (rawParam (proc, pid::bodyInvert) < 0.5f || ! bodyInvert.isInverted())
                fail ("Body's Ø didn't invert it");
            bodyInvert.onClick();
            if (rawParam (proc, pid::bodyInvert) > 0.5f || bodyInvert.isInverted())
                fail ("Body's Ø didn't switch back");
            const std::pair<int, const char*> pages[] { { LS::noiseTab, pid::noiseInvert }, { LS::clickTab, pid::clickInvert },
                                                        { LS::firstExtraTab + 1, "lay2Invert" } };
            for (const auto& [tab, id] : pages)
            {
                layers->getInvertButton (tab).onClick();
                if (rawParam (proc, id) < 0.5f || ! layers->getInvertButton (tab).isInverted())
                    fail (juce::String ("Ø on a page didn't set ") + id);
            }
            layers->getInvertButton (LS::clickTab).onClick();
            if (rawParam (proc, pid::clickInvert) > 0.5f)
                fail ("Click's Ø didn't switch back");
        }

        // Left for the reopened editor: Level tab, editing Click level.
        showCurve (*display, envClick);
        savedActive = active();
        editor = nullptr;
    }

    // --- Reopened, and after a session round trip: the same layer and tab ---------------------
    juce::MemoryBlock session;
    proc.getStateInformation (session);
    DrumFoundryProcessor restored;
    restored.setStateInformation (session.getData(), (int) session.getSize());

    for (auto* p : { &proc, &restored })
    {
        const auto where = p == &proc ? juce::String ("reopened editor") : juce::String ("restored session");
        std::unique_ptr<juce::AudioProcessorEditor> editor (p->createEditor());
        auto* display = findChild<ui::DisplayPanel> (*editor);
        auto* canvas = findChild<EC> (*editor);
        auto* layers = findChild<LS> (*editor);
        if (display == nullptr || canvas == nullptr || layers == nullptr)
        {
            fail (where + ": no display / canvas / layers");
            continue;
        }
        if (canvas->getActiveEnvelope() != savedActive || display->getTab() != EC::levelTab)
            fail (where + ": not on the Level tab editing " + EC::curveName (savedActive) + " (" + EC::curveName (canvas->getActiveEnvelope()) + ")");
        display->showTab (EC::pitchTab);
        if (canvas->getActiveEnvelope() != envClickPitch)
            fail (where + ": the Pitch tab doesn't edit the layer's pitch curve");
        if (! layers->getInvertButton (LS::noiseTab).isInverted())
            fail (where + ": Noise's Ø isn't lit");
        editor = nullptr;
    }

    std::printf ("editor chart: Pitch / Level tabs, the selected layer's curves, extra layers' own / coupled / flat curves, 0 Hz axis, "
                 "click / noise / layer pitch (end points, undo), gate drag, Body layer, polarity, reopen / session %s\n",
                 failures == 0 ? "ok" : "FAILED");
    return failures;
}

//==============================================================================
/** The LFOs: the modulation maths, what the engine does with a route (and that an
    unused or zero-amount slot changes nothing), Retrig / One Shot / Free and tempo
    sync, older patches loading with the LFOs off, dropping an LFO's handle on a knob
    (its slot, the ring, the full case, undo), the LFO tab, and a tab's M / S picking
    the layer and bringing its curves back from the LFO tab. */
int checkLfos()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: lfos: %s\n", msg.toRawUTF8()); ++failures; };
    const auto near = [] (float a, float b, float tolerance) { return std::abs (a - b) <= tolerance; };
    const auto id = [] (int lfo, const char* suffix, int slot = -1) { return lfoParamId (lfo, suffix, slot); };
    const auto set = [] (DrumFoundryProcessor& p, const juce::String& pid, float v) { setParam (p, pid.toRawUTF8(), v); };
    const int tuneTarget = modTargetIndex (pid::tune);

    // --- The maths ----------------------------------------------------------------------------
    if (modTargetChoiceNames().size() != modTargetIds().size() + 1 || tuneTarget < 0 || modTargetIndex (pid::clickType) >= 0)
        fail ("the target list is off (Tune should be in it, Click Type, a choice, not)");
    {
        DrumParams base;
        base.numRoutes = 2;
        base.routes[0] = { modTargetIndex (pid::driveAmount), 0, 0.25f, 0.5f, 0.0f, 1.0f, 1.0f };
        base.routes[1] = { modTargetIndex (pid::driveAmount), 1, 0.5f, 0.5f, 0.0f, 1.0f, 1.0f };
        auto p = base;
        const float lfo[] { 1.0f, 0.5f, 0.0f };
        applyModulation (p, base, lfo);
        if (! near (p.driveAmount, 1.0f, 1.0e-5f))
            fail ("two routes on Drive don't add up (and clamp): " + juce::String (p.driveAmount));
        const float down[] { -1.0f, 0.0f, 0.0f };
        applyModulation (p, base, down);
        if (! near (p.driveAmount, 0.25f, 1.0e-5f))
            fail ("LFO 1 at -1 doesn't take Drive down by its amount: " + juce::String (p.driveAmount));
    }
    if (! near ((float) LfoBank::cyclesPerSecond ({ 0, 4.0f, lfoSyncNames.indexOf ("1/4"), 0, 0.0f }, 120.0f), 2.0f, 1.0e-4f)
        || ! near ((float) LfoBank::cyclesPerSecond ({ 0, 4.0f, lfoSyncNames.indexOf ("1/8 T"), 0, 0.0f }, 120.0f), 6.0f, 1.0e-4f)
        || ! near ((float) LfoBank::cyclesPerSecond ({ 0, 4.0f, 0, 0, 0.0f }, 120.0f), 4.0f, 1.0e-4f))
        fail ("synced rates are wrong");

    // --- The engine -----------------------------------------------------------------------------
    {
        DrumFoundryProcessor proc;
        proc.prepareToPlay (48000.0, 512);
        EnvTables tables;
        proc.bakeTables (tables);
        const auto render = [&] { return renderHit (proc.readParams(), tables, 48000.0); };
        const auto maxDiff = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            if (a.getNumSamples() != b.getNumSamples())
                return 1.0e9f;
            float d = 0.0f;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < a.getNumSamples(); ++i)
                    d = juce::jmax (d, std::abs (a.getSample (c, i) - b.getSample (c, i)));
            return d;
        };

        const auto plain = render();
        set (proc, id (0, pid::lfoDest, 0), (float) (tuneTarget + 1)); // a target, but no amount
        if (proc.readParams().numRoutes != 0 || maxDiff (render(), plain) > 0.0f)
            fail ("a slot with a target and no amount changes the sound");

        set (proc, id (0, pid::lfoAmt, 0), 0.08f);
        set (proc, id (0, pid::lfoShape), (float) lfoShapeNames.indexOf ("Square"));
        set (proc, id (0, pid::lfoRate), 20.0f);
        const auto modulated = render();
        if (proc.readParams().numRoutes != 1 || maxDiff (modulated, plain) < 1.0e-3f)
            fail ("routing LFO 1 to Tune doesn't change the sound");
        if (maxDiff (render(), modulated) > 0.0f)
            fail ("a modulated render isn't repeatable");

        // The engine's live Tune follows the square: up for the first half cycle, down for the second.
        DrumEngine engine;
        engine.prepare (48000.0, 512);
        engine.setParams (proc.readParams());
        engine.setTables (tables);
        juce::AudioBuffer<float> buf (2, 16);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        engine.process (buf, midi);
        const float baseHz = proc.readParams().tuneHz;
        const float up = engine.getLiveParams().tuneHz;
        if (! (up > baseHz * 1.05f) || ! near (engine.getLfoValue (0), 1.0f, 1.0e-6f))
            fail ("LFO 1 doesn't start the hit at the top of its square: " + juce::String (up) + " Hz over " + juce::String (baseHz));
        midi.clear();
        for (int i = 0; i < 100; ++i) // 33 ms: a third of the way into the second half cycle
            engine.process (buf, midi);
        if (! (engine.getLiveParams().tuneHz < baseHz * 0.95f) || ! near (engine.getLfoValue (0), -1.0f, 1.0e-6f))
            fail ("LFO 1 isn't at the bottom of its square 33 ms in: " + juce::String (engine.getLiveParams().tuneHz) + " Hz");
        if (! juce::exactlyEqual (engine.getParams().tuneHz, baseHz))
            fail ("modulation leaked into the patch the engine was given");

        // One Shot holds where its cycle ends; Retrig starts at Phase on every hit; Free doesn't restart.
        set (proc, id (0, pid::lfoShape), (float) lfoShapeNames.indexOf ("Saw Down"));
        set (proc, id (0, pid::lfoMode), (float) lfoModeNames.indexOf ("One Shot"));
        engine.setParams (proc.readParams());
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        engine.process (buf, midi);
        midi.clear();
        for (int i = 0; i < 400; ++i) // 133 ms: well past one 50 ms cycle
            engine.process (buf, midi);
        if (! near (engine.getLfoValue (0), -1.0f, 1.0e-3f))
            fail ("a One Shot saw down doesn't hold at its bottom: " + juce::String (engine.getLfoValue (0)));

        set (proc, id (0, pid::lfoMode), (float) lfoModeNames.indexOf ("Retrig"));
        set (proc, id (0, pid::lfoShape), (float) lfoShapeNames.indexOf ("Sine"));
        set (proc, id (0, pid::lfoPhase), 0.25f);
        engine.setParams (proc.readParams());
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        engine.process (buf, midi);
        if (! near (engine.getLfoValue (0), 1.0f, 1.0e-3f))
            fail ("Retrig with Phase 90 degrees doesn't start a sine at its top: " + juce::String (engine.getLfoValue (0)));

        set (proc, id (0, pid::lfoMode), (float) lfoModeNames.indexOf ("Free"));
        engine.setParams (proc.readParams());
        midi.clear();
        for (int i = 0; i < 7; ++i)
            engine.process (buf, midi);
        const float before = engine.getLfoValue (0);
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        engine.process (buf, midi);
        if (near (engine.getLfoValue (0), 1.0f, 1.0e-3f) || near (engine.getLfoValue (0), before, 1.0e-6f))
            fail ("a Free LFO restarted with the hit (or stood still)");

        // A patch saved before the LFOs existed loads with them off.
        auto tree = proc.createStateTree();
        for (int i = tree.getNumChildren(); --i >= 0;)
            if (tree.getChild (i).getProperty ("id").toString().startsWith ("lfo"))
                tree.removeChild (i, nullptr);
        proc.loadStateTree (tree);
        if (proc.readParams().numRoutes != 0 || rawParam (proc, id (0, pid::lfoDest, 0).toRawUTF8()) != 0.0f)
            fail ("a patch without LFO settings keeps the routes it replaced");
    }

    // --- The editor -----------------------------------------------------------------------------
    {
        DrumFoundryProcessor proc;
        proc.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        auto* display = findChild<ui::DisplayPanel> (*editor);
        auto* canvas = findChild<ui::EnvelopeCanvas> (*editor);
        auto* layers = findChild<ui::LayersSection> (*editor);
        auto* body = findChild<ui::LayerPanel> (*editor);
        if (display == nullptr || canvas == nullptr || layers == nullptr || body == nullptr)
        {
            fail ("no display / canvas / layers / body");
            return failures;
        }

        std::function<ui::Knob* (juce::Component&, const juce::String&)> knobFor = [&] (juce::Component& root, const juce::String& paramId) -> ui::Knob*
        {
            for (auto* child : root.getChildren())
            {
                if (auto* k = dynamic_cast<ui::Knob*> (child); k != nullptr && k->getParamId() == paramId)
                    return k;
                if (auto* k = knobFor (*child, paramId))
                    return k;
            }
            return nullptr;
        };

        display->showTab (ui::DisplayPanel::lfoTab);
        if (! display->isLfoTabShown() || canvas->isVisible() || ! display->getLfoPage().isVisible()
            || ! display->getTabButton (ui::DisplayPanel::lfoTab).getToggleState() || display->getTabButton (0).getToggleState())
            fail ("the LFO tab doesn't swap the curves for the LFOs");

        // Drop LFO 2's handle on Body's Level knob.
        auto* level = knobFor (*editor, pid::bodyLevel);
        auto* rate = knobFor (display->getLfoPage(), id (0, pid::lfoRate));
        auto& handle = display->getLfoPage().getHandle (1);
        const juce::DragAndDropTarget::SourceDetails details (ui::lfoDragDescription (1), &handle, {});
        if (level == nullptr || rate == nullptr)
        {
            fail ("no Body Level / LFO 1 Rate knob");
        }
        else
        {
            if (! level->isInterestedInDragSource (details) || rate->isInterestedInDragSource (details))
                fail ("the knobs take the wrong drops (Level should, an LFO's Rate shouldn't)");

            auto& history = proc.getUndoHistory();
            history.clear();
            history.noteUserActivity();
            level->itemDropped (details);
            history.poll (false);
            const int target = modTargetIndex (pid::bodyLevel);
            if (juce::roundToInt (rawParam (proc, id (1, pid::lfoDest, 0).toRawUTF8())) != target + 1
                || ! near (rawParam (proc, id (1, pid::lfoAmt, 0).toRawUTF8()), 0.25f, 1.0e-6f) || level->getModulatingLfos() != 2)
                fail ("dropping LFO 2 on Body Level didn't route it there at +25%");

            level->itemDropped (details); // again: the same slot, nothing new
            if (rawParam (proc, id (1, pid::lfoDest, 1).toRawUTF8()) != 0.0f)
                fail ("dropping LFO 2 on Body Level twice took a second slot");

            for (auto* other : { pid::noiseLevel, pid::clickLevel, pid::driveAmount })
                ui::routeLfo (proc.getState(), 1, modTargetIndex (other));
            if (ui::routeLfo (proc.getState(), 1, modTargetIndex (pid::eqLow)) >= 0)
                fail ("a fifth target found a slot on LFO 2");

            for (int k = 1; k < kLfoSlots; ++k)
                ui::clearLfoSlot (proc.getState(), 1, k);
            history.poll (false); // not the user's: becomes part of the drop's "after"
            if (! history.undo())
                fail ("couldn't undo the drop");
            if (rawParam (proc, id (1, pid::lfoDest, 0).toRawUTF8()) != 0.0f || level->getModulatingLfos() != 0)
                fail ("undoing the drop didn't remove the route");
        }

        // M on the Click tab, while the LFO tab is up: the Click tab and its curves come forward.
        display->showTab (ui::DisplayPanel::lfoTab);
        layers->selectTab (ui::LayersSection::noiseTab);
        layers->getMuteButton (ui::LayersSection::clickTab).onClick();
        if (layers->getSelectedTab() != ui::LayersSection::clickTab)
            fail ("M on the Click tab didn't switch to it");
        if (display->isLfoTabShown() || ! canvas->isVisible()
            || (canvas->getActiveEnvelope() != envClickPitch && canvas->getActiveEnvelope() != envClick))
            fail ("M on the Click tab didn't bring its curves forward: " + ui::EnvelopeCanvas::curveName (canvas->getActiveEnvelope()));
        if (rawParam (proc, pid::clickOn) != 0.0f)
            fail ("M on the Click tab didn't mute it");
        layers->getMuteButton (ui::LayersSection::clickTab).onClick();

        // S on the Noise tab: the same.
        layers->getSoloButton (ui::LayersSection::noiseTab).onClick();
        if (layers->getSelectedTab() != ui::LayersSection::noiseTab
            || (canvas->getActiveEnvelope() != envNoisePitch && canvas->getActiveEnvelope() != envNoise))
            fail ("S on the Noise tab didn't switch to it and its curves");
        layers->getSoloButton (ui::LayersSection::noiseTab).onClick();

        // Body's own M, from the LFO tab.
        display->showTab (ui::DisplayPanel::lfoTab);
        if (auto* m = body->getMuteButton())
        {
            m->onClick();
                if (display->isLfoTabShown() || (canvas->getActiveEnvelope() != envPitch && canvas->getActiveEnvelope() != envBody))
                fail ("Body's M didn't bring Body's curves forward");
            m->onClick();
        }

        // The LFO tab survives closing the editor.
        display->showTab (ui::DisplayPanel::lfoTab);
        editor = nullptr;
        editor.reset (proc.createEditor());
        display = findChild<ui::DisplayPanel> (*editor);
        if (display == nullptr || ! display->isLfoTabShown())
            fail ("reopening the editor left the LFO tab");
    }

    std::printf ("lfos: maths, sync, engine (Retrig / One Shot / Free), old patches, knob drops, slots, undo, LFO tab, tab focus %s\n",
                 failures == 0 ? "ok" : "FAILED");
    return failures;
}

//==============================================================================
// Output clipper checks

/** The soft clipper as it was before the clip modes existed (for comparison). */
float oldSoftClip (float x)
{
    constexpr float knee = 0.891f;
    const float a = std::abs (x);
    if (a <= knee)
        return x;
    const float y = knee + (1.0f - knee) * std::tanh ((a - knee) / (1.0f - knee));
    return std::copysign (y, x);
}

struct ClipSettings { int mode = Clipper::soft; float driveDb = 0.0f, ceilingDb = 0.0f; };

/** Runs a stereo signal through a fresh Clipper in uneven blocks; settings may change per block. */
juce::AudioBuffer<float> runClipper (const juce::AudioBuffer<float>& in, double sr,
                                     const std::function<ClipSettings (int)>& settingsAt)
{
    Clipper clipper;
    clipper.prepare (sr);
    auto out = in;
    const int sizes[] { 512, 37, 256, 1, 480, 129 };
    int k = 0;
    for (int pos = 0; pos < out.getNumSamples();)
    {
        const int n = juce::jmin (sizes[k++ % 6], out.getNumSamples() - pos);
        const auto s = settingsAt (pos);
        clipper.process (out.getWritePointer (0, pos), out.getWritePointer (1, pos), n, s.mode, s.driveDb, s.ceilingDb);
        pos += n;
    }
    return out;
}

juce::AudioBuffer<float> sineBuffer (double sr, int total, double hz, float amp)
{
    juce::AudioBuffer<float> b (2, total);
    for (int i = 0; i < total; ++i)
    {
        const float v = amp * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / sr);
        b.setSample (0, i, v);
        b.setSample (1, i, v);
    }
    return b;
}

float maxStep (const juce::AudioBuffer<float>& b, int from = 1)
{
    float step = 0.0f;
    for (int c = 0; c < 2; ++c)
        for (int i = juce::jmax (1, from); i < b.getNumSamples(); ++i)
            step = juce::jmax (step, std::abs (b.getSample (c, i) - b.getSample (c, i - 1)));
    return step;
}

/** Spectrum of the left channel from `start` (32768 points, Blackman-Harris):
    the fundamental's level and the strongest component that isn't within a few
    bins of a harmonic of f0 (or of DC), below and above 20 kHz, in dB relative
    to the fundamental. */
struct AliasReport { double fundamentalDb = 0, audibleDb = -300, audibleHz = 0, topDb = -300, topHz = 0; };

AliasReport measureAliasing (const juce::AudioBuffer<float>& y, int start, double sr, double f0)
{
    constexpr int order = 15, n = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) (2 * n), 0.0f);
    for (int i = 0; i < n; ++i)
    {
        const double t = juce::MathConstants<double>::twoPi * i / (n - 1);
        const double w = 0.35875 - 0.48829 * std::cos (t) + 0.14128 * std::cos (2.0 * t) - 0.01168 * std::cos (3.0 * t);
        buf[(size_t) i] = (float) (y.getSample (0, start + i) * w);
    }
    fft.performFrequencyOnlyForwardTransform (buf.data());

    const double binHz = sr / n;
    constexpr int guard = 8;
    const auto db = [&] (int b) { return 20.0 * std::log10 (juce::jmax (1.0e-20, (double) buf[(size_t) b])); };

    AliasReport r;
    const int f0Bin = (int) std::lround (f0 / binHz);
    double fund = -400.0;
    for (int b = f0Bin - guard; b <= f0Bin + guard; ++b)
        fund = juce::jmax (fund, db (b));
    r.fundamentalDb = fund;

    for (int b = guard; b < n / 2; ++b)
    {
        const double f = b * binHz;
        const double nearestHarmonic = std::round (f / f0) * f0;
        if (std::abs (f - nearestHarmonic) <= guard * binHz)
            continue;
        const double level = db (b) - fund;
        if (f < 20000.0 && level > r.audibleDb) { r.audibleDb = level; r.audibleHz = f; }
        if (f >= 20000.0 && level > r.topDb)    { r.topDb = level; r.topHz = f; }
    }
    return r;
}

int checkClipper()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: %s\n", msg.toRawUTF8()); ++failures; };
    const auto modeName = [] (int m) { return clipModeNames[m]; };

    // --- Static curves ----------------------------------------------------------------------
    {
        int softMismatches = 0;
        for (int i = -80000; i <= 80000; ++i)
        {
            const float x = (float) i * 1.0e-4f;
            if (! juce::exactlyEqual (Clipper::transfer (Clipper::soft, x, 0.0f, 0.0f), oldSoftClip (x)))
                ++softMismatches;
            if (! juce::exactlyEqual (Clipper::transfer (Clipper::off, x, 12.0f, -6.0f), x))
                fail ("transfer(Off) isn't the identity at " + juce::String (x));
        }
        if (softMismatches > 0)
            fail ("transfer(Soft, 0 dB, 0 dBFS) differs from the old soft clip at " + juce::String (softMismatches) + " points");

        float worstRatio = 0.0f, worstJump = 0.0f, worstSlopeErr = 0.0f;
        for (int m = Clipper::soft; m < Clipper::numModes; ++m)
            for (float drive : { 0.0f, 6.0f, 12.0f, 24.0f })
                for (float ceilingDb : { 0.0f, -6.0f, -12.0f })
                {
                    const float c = juce::Decibels::decibelsToGain (ceilingDb);
                    float prev = Clipper::transfer (m, -32.0f * c, drive, ceilingDb);
                    for (int i = -320000; i <= 320000; ++i)
                    {
                        const float x = (float) i * 1.0e-4f * c;
                        const float y = Clipper::transfer (m, x, drive, ceilingDb);
                        if (! std::isfinite (y))
                            fail ("transfer(" + modeName (m) + ") non-finite at " + juce::String (x));
                        worstRatio = juce::jmax (worstRatio, std::abs (y) / c);
                        worstJump = juce::jmax (worstJump, std::abs (y - prev));
                        prev = y;
                    }
                    // Unit slope times the drive at small levels, whatever the mode.
                    const float g = juce::Decibels::decibelsToGain (drive);
                    const float slope = Clipper::transfer (m, 1.0e-4f * c, drive, ceilingDb) / (1.0e-4f * c);
                    worstSlopeErr = juce::jmax (worstSlopeErr, std::abs (slope / g - 1.0f));
                }
        if (worstRatio > 1.000001f)
            fail ("a static curve goes over its ceiling (" + juce::String (worstRatio) + " x)");
        if (worstJump > 0.01f)
            fail ("a static curve jumps by " + juce::String (worstJump) + " between neighbouring inputs");
        if (worstSlopeErr > 0.01f)
            fail ("small-signal gain differs from the drive by " + juce::String (100.0f * worstSlopeErr) + "%");
        std::printf ("clipper curves: Soft at 0 dB / 0 dBFS == old soft clip at 160001 points (%d differ); all modes <= ceiling "
                     "(worst %.6f x) for inputs to +30 dB over, continuous (largest step %.4f per 1e-4), small-signal gain = drive "
                     "(within %.3f%%)\n", softMismatches, worstRatio, worstJump, 100.0f * worstSlopeErr);
    }

    // --- Latency: per sample rate, the same for every mode -----------------------------------
    {
        juce::String line;
        for (double sr : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
        {
            MasterChain chain;
            chain.prepare (sr, 512);
            Clipper clipper;
            clipper.prepare (sr);
            const int latency = clipper.getLatencySamples();

            // An impulse comes out at the same place in every mode (and exactly, in Off and Soft).
            const int total = 4096, at = 1000;
            juce::AudioBuffer<float> impulse (2, total);
            impulse.clear();
            impulse.setSample (0, at, 0.25f);
            impulse.setSample (1, at, 0.25f);
            for (int m = 0; m < Clipper::numModes; ++m)
            {
                const auto out = runClipper (impulse, sr, [m] (int) { return ClipSettings { m, 0.0f, 0.0f }; });
                int peakAt = 0;
                for (int i = 0; i < total; ++i)
                    if (std::abs (out.getSample (0, i)) > std::abs (out.getSample (0, peakAt)))
                        peakAt = i;
                if (peakAt != at + latency)
                    fail (modeName (m) + " @" + juce::String (sr) + ": impulse came out at +" + juce::String (peakAt - at)
                          + ", latency is " + juce::String (latency));
                if (m <= Clipper::soft && (! juce::exactlyEqual (out.getSample (0, at + latency), 0.25f)
                                           || ! juce::exactlyEqual (out.getMagnitude (0, 0, total), 0.25f)))
                    fail (modeName (m) + " @" + juce::String (sr) + ": impulse not passed exactly");
            }
            line << juce::String (sr / 1000.0, 1) << " kHz: " << clipper.getOversamplingFactor() << "x, clipper "
                 << latency << " (" << juce::String (1000.0 * latency / sr, 2) << " ms), chain " << chain.getLatencySamples() << "   ";
        }
        std::printf ("clipper latency (samples, the same in every mode): %s\n", line.toRawUTF8());
    }

    constexpr double sr = 48000.0;
    const int total = (int) sr;

    // --- Off and Soft at its defaults: bit-for-bit the delayed input where nothing clips ----
    {
        Clipper probe;
        probe.prepare (sr);
        const int latency = probe.getLatencySamples();
        const auto delayedEqual = [&] (const juce::AudioBuffer<float>& in, const juce::AudioBuffer<float>& out)
        {
            for (int c = 0; c < 2; ++c)
                for (int i = latency; i < in.getNumSamples(); ++i)
                    if (! juce::exactlyEqual (out.getSample (c, i), in.getSample (c, i - latency)))
                        return false;
            return true;
        };

        auto hot = chainTestSignal (sr, total);
        hot.applyGain (3.0f); // peaks around +6 dBFS
        const auto offOut = runClipper (hot, sr, [] (int) { return ClipSettings { Clipper::off, 12.0f, -6.0f }; });
        if (! delayedEqual (hot, offOut))
            fail ("Off isn't the delayed input");
        if (offOut.getMagnitude (0, total) < 1.5f)
            fail ("Off didn't pass a hot signal over 0 dBFS");

        auto quiet = chainTestSignal (sr, total);
        quiet.applyGain (0.8f / quiet.getMagnitude (0, total)); // peak -1.9 dBFS, under the knee
        const auto softOut = runClipper (quiet, sr, [] (int) { return ClipSettings {}; });
        if (! delayedEqual (quiet, softOut))
            fail ("Soft (0 dB, 0 dBFS) changes a signal that stays under its knee");

        // Over the knee, low frequencies: the oversampled Soft against the old 1x one.
        auto sub = sineBuffer (sr, total, 55.0, 1.41f); // +3 dB
        const auto subOut = runClipper (sub, sr, [] (int) { return ClipSettings {}; });
        float diff = 0.0f;
        for (int i = latency; i < total; ++i)
            diff = juce::jmax (diff, std::abs (subOut.getSample (0, i) - oldSoftClip (sub.getSample (0, i - latency))));
        if (diff > 0.01f)
            fail ("Soft on a +3 dB 55 Hz sine differs from the old soft clip by " + juce::String (diff));
        std::printf ("clipper Off: bit-identical to the delayed input, passes %.2f (%+.1f dBFS); Soft at defaults: bit-identical "
                     "under the knee; +3 dB 55 Hz sine vs the old 1x soft clip: max diff %.2e (%.1f dB)\n",
                     offOut.getMagnitude (0, total), juce::Decibels::gainToDecibels (offOut.getMagnitude (0, total)),
                     diff, juce::Decibels::gainToDecibels (diff, -200.0f));
    }

    // --- Bounded at the ceiling for inputs up to +24 dB over it ---------------------------------
    {
        juce::String worstTag;
        float worst = 0.0f;
        for (double rate : { 44100.0, 48000.0, 96000.0 })
        {
            const int n = (int) (rate * 0.5);
            std::vector<std::pair<juce::String, juce::AudioBuffer<float>>> signals;
            auto hits = chainTestSignal (rate, n);
            hits.applyGain (1.0f / hits.getMagnitude (0, n));
            signals.emplace_back ("hits", hits);
            for (double hz : { 50.0, 1000.0, 5000.0, 15000.0 })
                signals.emplace_back (juce::String (hz) + " Hz", sineBuffer (rate, n, hz, 1.0f));
            juce::Random rng (7);
            juce::AudioBuffer<float> noise (2, n);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i)
                    noise.setSample (c, i, rng.nextFloat() * 2.0f - 1.0f);
            signals.emplace_back ("noise", noise);

            for (int m = Clipper::soft; m < Clipper::numModes; ++m)
                for (float ceilingDb : { 0.0f, -6.0f, -12.0f })
                    for (float drive : { 0.0f, 12.0f, 24.0f })
                        for (float overDb : { 0.0f, 12.0f, 24.0f })
                            for (const auto& [name, sig] : signals)
                            {
                                const float c = juce::Decibels::decibelsToGain (ceilingDb);
                                auto in = sig;
                                in.applyGain (c * juce::Decibels::decibelsToGain (overDb));
                                const auto out = runClipper (in, rate, [&] (int) { return ClipSettings { m, drive, ceilingDb }; });
                                if (! allFinite (out))
                                    fail (modeName (m) + ": non-finite output");
                                const float ratio = juce::jmax (out.getMagnitude (0, n), out.getMagnitude (1, 0, n)) / c;
                                if (ratio > worst)
                                {
                                    worst = ratio;
                                    worstTag = modeName (m) + " " + name + " +" + juce::String (overDb) + " dB, drive "
                                             + juce::String (drive) + ", ceiling " + juce::String (ceilingDb) + " @" + juce::String (rate);
                                }
                            }
        }
        if (worst > 1.000001f)
            fail ("clipper output over its ceiling: " + juce::String (worst) + " x (" + worstTag + ")");
        std::printf ("clipper bounds: every mode, ceiling 0/-6/-12, drive 0/12/24 dB, input to +24 dB over (hits, sines "
                     "50 Hz-15 kHz, noise) at 44.1/48/96 kHz: peak <= %.6f x ceiling\n", worst);
    }

    // --- Aliasing ---------------------------------------------------------------------------------
    {
        const int n = (int) (sr * 1.5);
        const auto sine = sineBuffer (sr, n, 5000.0, 4.0f); // +12 dB
        const int start = n - 32768;

        juce::String line;
        for (int m = Clipper::soft; m < Clipper::numModes; ++m)
        {
            const auto out = runClipper (sine, sr, [m] (int) { return ClipSettings { m, 0.0f, 0.0f }; });
            const auto r = measureAliasing (out, start, sr, 5000.0);
            if (m == Clipper::hard && r.audibleDb > -60.0)
                fail ("Hard: 5 kHz at +12 dB aliases at " + juce::String (r.audibleDb, 1) + " dB (" + juce::String (r.audibleHz, 0) + " Hz)");
            line << modeName (m) << " " << juce::String (r.audibleDb, 1) << " dB";
            if (m == Clipper::hard)
                line << " (" << juce::String (r.audibleHz, 0) << " Hz; 20-24 kHz: " << juce::String (r.topDb, 1) << " dB at "
                     << juce::String (r.topHz, 0) << " Hz)";
            line << "  ";
        }

        // For scale: the same hard clip at 1x.
        auto naive = sine;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                naive.setSample (c, i, juce::jlimit (-1.0f, 1.0f, naive.getSample (c, i)));
        const auto r1 = measureAliasing (naive, start, sr, 5000.0);
        std::printf ("clipper aliasing, 5 kHz sine at +12 dB @48k, strongest non-harmonic component under 20 kHz rel. the "
                     "fundamental: %s(1x hard clip: %.1f dB)\n", line.toRawUTF8(), r1.audibleDb);
    }

    // --- Tube: even harmonics, and no DC left behind ---------------------------------------------
    {
        const int n = (int) (sr * 1.5);
        auto in = sineBuffer (sr, n, 1000.0, 1.0f);
        const auto evenLevel = [&] (int m)
        {
            const auto out = runClipper (in, sr, [m] (int) { return ClipSettings { m, 6.0f, 0.0f }; });
            const auto r = measureAliasing (out, n - 32768, sr, 1000.0);
            juce::ignoreUnused (r);
            // 2nd harmonic relative to the fundamental.
            constexpr int order = 15, len = 1 << order;
            juce::dsp::FFT fft (order);
            std::vector<float> buf ((size_t) (2 * len), 0.0f);
            for (int i = 0; i < len; ++i)
                buf[(size_t) i] = out.getSample (0, n - len + i) * (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (len - 1)));
            fft.performFrequencyOnlyForwardTransform (buf.data());
            const auto peakNear = [&] (double hz)
            {
                const int b = (int) std::lround (hz * len / sr);
                float v = 0.0f;
                for (int k = b - 4; k <= b + 4; ++k)
                    v = juce::jmax (v, buf[(size_t) k]);
                return v;
            };
            return juce::Decibels::gainToDecibels (peakNear (2000.0) / peakNear (1000.0), -200.0f);
        };
        const float tubeEven = evenLevel (Clipper::tube), softEven = evenLevel (Clipper::soft);
        if (tubeEven < -40.0f)
            fail ("Tube's 2nd harmonic is only " + juce::String (tubeEven, 1) + " dB");

        // A hot 50 Hz burst, then silence: the mean stays near zero and nothing is left after.
        const int burst = (int) (sr * 0.6);
        auto bass = sineBuffer (sr, total, 50.0, 2.0f);
        for (int c = 0; c < 2; ++c)
            bass.clear (c, burst, total - burst);
        const auto out = runClipper (bass, sr, [] (int) { return ClipSettings { Clipper::tube, 12.0f, 0.0f }; });
        double mean = 0.0;
        const int from = (int) (sr * 0.2), to = (int) (sr * 0.6); // 20 whole cycles
        for (int i = from; i < to; ++i)
            mean += out.getSample (0, i);
        mean /= (to - from);
        const float after = out.getMagnitude (0, burst + (int) (sr * 0.15), total - burst - (int) (sr * 0.15));
        if (std::abs (mean) > 0.01 || after > 1.0e-3f)
            fail ("Tube DC: mean " + juce::String (mean) + " while playing, " + juce::String (after) + " 150 ms after");
        std::printf ("clipper Tube: 2nd harmonic %.1f dB (Soft %.1f dB) on a 0 dBFS 1 kHz sine at 6 dB drive; DC on a +6 dB "
                     "50 Hz burst at 12 dB drive: mean %.1e while playing, peak %.1e 150 ms after it stops\n",
                     tubeEven, softEven, mean, after);
    }

    // --- Click-free mode switches and drive / ceiling automation ------------------------------
    {
        const auto in = sineBuffer (sr, total, 110.0, 2.0f); // +6 dB
        // The largest step each steady setting makes on its own: modes at 0 dB drive
        // (for the mode switches), and anything up to 24 dB drive (for the jumps).
        // A 10 ms linear crossfade between two modes adds its own slope, at most
        // (largest difference between the two outputs) / (fade length) per sample.
        float reference0 = 0.0f, reference = 0.0f;
        std::vector<juce::AudioBuffer<float>> steady;
        for (int m = 0; m < Clipper::numModes; ++m)
            for (float drive : { 0.0f, 24.0f })
                for (float ceilingDb : { 0.0f, -12.0f })
                {
                    auto out = runClipper (in, sr, [&] (int) { return ClipSettings { m, drive, ceilingDb }; });
                    const float step = maxStep (out, 2000);
                    reference = juce::jmax (reference, step);
                    if (juce::exactlyEqual (drive, 0.0f) && juce::exactlyEqual (ceilingDb, 0.0f))
                    {
                        reference0 = juce::jmax (reference0, step);
                        steady.push_back (std::move (out));
                    }
                }
        float modeGap = 0.0f;
        for (const auto& a : steady)
            for (const auto& b : steady)
                for (int i = 2000; i < total; ++i)
                    modeGap = juce::jmax (modeGap, std::abs (a.getSample (0, i) - b.getSample (0, i)));
        const float fadeSlope = modeGap / (float) (0.01 * sr);

        juce::Random rng (3);
        std::vector<int> modes;
        for (int k = 0; k < 64; ++k)
            modes.push_back (rng.nextInt (Clipper::numModes));
        const int every = (int) (sr * 0.03);
        const auto switching = runClipper (in, sr, [&] (int pos) { return ClipSettings { modes[(size_t) (pos / every) % modes.size()], 0.0f, 0.0f }; });
        const float switchStep = maxStep (switching, 2000);
        const auto driving = runClipper (in, sr, [&] (int pos) { return ClipSettings { Clipper::soft, (pos / every) % 2 == 0 ? 0.0f : 24.0f, 0.0f }; });
        const auto ceilingMoves = runClipper (in, sr, [&] (int pos) { return ClipSettings { Clipper::hard, 6.0f, (pos / every) % 2 == 0 ? 0.0f : -12.0f }; });
        const float driveStep = maxStep (driving, 2000), ceilingStep = maxStep (ceilingMoves, 2000);

        if (switchStep > (reference0 + fadeSlope) * 1.02f || driveStep > reference * 1.1f || ceilingStep > reference * 1.1f)
            fail ("clipper clicks: largest step " + juce::String (switchStep) + " (mode switches; steady " + juce::String (reference0)
                  + "), " + juce::String (driveStep) + " (drive jumps), " + juce::String (ceilingStep) + " (ceiling jumps); steady "
                  + juce::String (reference));
        std::printf ("clipper automation (+6 dB 110 Hz sine): largest sample step %.4f switching modes every 30 ms (steady modes "
                     "at 0 dB drive: up to %.4f, + %.4f for the fade's own slope), %.4f with drive jumping 0/24 dB, %.4f with ceiling "
                     "jumping 0/-12 dBFS (steady settings: up to %.4f)\n", switchStep, reference0, fadeSlope, driveStep, ceilingStep, reference);
    }

    // --- Random automation: finite, and bounded unless Off is in play --------------------------
    {
        for (bool withOff : { true, false })
        {
            juce::Random rng (withOff ? 11 : 12);
            const int n = (int) (sr * 6.0);
            juce::AudioBuffer<float> in (2, n);
            float level = 1.0f;
            for (int i = 0; i < n; ++i)
            {
                if (i % 2400 == 0)
                    level = juce::Decibels::decibelsToGain (rng.nextFloat() * 36.0f - 12.0f);
                const float s = (float) std::sin (0.02 * i) * 0.7f + (rng.nextFloat() - 0.5f) * 0.6f;
                in.setSample (0, i, s * level);
                in.setSample (1, i, (rng.nextFloat() - 0.5f) * level);
            }
            std::vector<ClipSettings> settings;
            for (int k = 0; k < 4000; ++k)
                settings.push_back ({ withOff ? rng.nextInt (Clipper::numModes) : 1 + rng.nextInt (Clipper::numModes - 1),
                                      rng.nextFloat() * 24.0f, -12.0f * rng.nextFloat() });
            int call = 0;
            const auto out = runClipper (in, sr, [&] (int) { return settings[(size_t) (call++ % 4000)]; });
            if (! allFinite (out))
                fail ("clipper random automation: non-finite output");
            const float peak = juce::jmax (out.getMagnitude (0, 0, n), out.getMagnitude (1, 0, n));
            if (! withOff && peak > 1.000001f)
                fail ("clipper random automation without Off: peak " + juce::String (peak));
            std::printf ("clipper random automation (6 s, %s): finite, peak %.4f\n", withOff ? "all modes" : "no Off", peak);
        }
    }

    // --- Through the engine: every mode puts the hit in the same place ---------------------------
    {
        DrumFoundryProcessor proc;
        EnvTables tables;
        proc.bakeTables (tables);
        auto p = proc.readParams();
        int firstOnset = -1;
        juce::String line;
        for (int m = 0; m < Clipper::numModes; ++m)
        {
            p.clipMode = m;
            const auto hit = renderHit (p, tables, sr);
            int onset = 0;
            while (onset < hit.getNumSamples() && std::abs (hit.getSample (0, onset)) < 1.0e-3f)
                ++onset;
            if (firstOnset < 0)
                firstOnset = onset;
            else if (onset != firstOnset)
                fail ("rendered hit starts at " + juce::String (onset) + " in " + modeName (m) + ", " + juce::String (firstOnset) + " in Off");
            line << modeName (m) << " " << juce::String (hit.getMagnitude (0, hit.getNumSamples()), 3) << "  ";
        }
        std::printf ("clipper through renderHit: same onset (sample %d) in every mode; peaks %s\n", firstOnset, line.toRawUTF8());
    }

    return failures;
}

//==============================================================================
// Undo / redo

int checkUndo()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: undo: %s\n", msg.toRawUTF8()); ++failures; };

    DrumFoundryProcessor proc;
    proc.prepareToPlay (48000.0, 512);
    auto& history = proc.getUndoHistory();
    history.clear();

    const auto near = [] (float a, float b) { return std::abs (a - b) < 1.0e-3f; };
    const auto sameEnv = [&] (EnvId id, const CurveEnvelope& env) { return proc.getEnvelope (id) == env; };
    // A user gesture: activity, the change, then the poll once it's over.
    const auto gesture = [&] (const std::function<void()>& change)
    {
        history.noteUserActivity();
        change();
        history.poll (false);
    };

    // Nothing to undo: a no-op.
    {
        const auto before = proc.createStateTree();
        if (history.undo() || history.redo() || history.canUndo() || history.canRedo())
            fail ("undo / redo did something with an empty history");
        if (! proc.createStateTree().isEquivalentTo (before))
            fail ("an empty undo changed the patch");
    }

    // A parameter: undo, then redo.
    {
        const float tune0 = rawParam (proc, pid::tune);
        gesture ([&] { setParam (proc, pid::tune, 80.0f); });
        if (history.getNumUndoSteps() != 1 || history.getUndoDescription() != "Tune")
            fail ("a Tune change gave " + juce::String (history.getNumUndoSteps()) + " step(s), \"" + history.getUndoDescription() + "\"");
        if (! history.undo() || ! near (rawParam (proc, pid::tune), tune0))
            fail ("undo didn't put Tune back");
        if (! history.canRedo() || history.getRedoDescription() != "Tune" || history.canUndo())
            fail ("after undo: redo should be Tune, and nothing left to undo");
        if (! history.redo() || ! near (rawParam (proc, pid::tune), 80.0f))
            fail ("redo didn't put Tune back to 80");
        if (history.canRedo())
            fail ("redo list not empty after redo");
        // Undo and redo aren't steps themselves.
        history.poll (false);
        if (history.getNumUndoSteps() != 1)
            fail ("undo / redo made steps of their own");
    }

    // A switch reads as what it did.
    {
        gesture ([&] { setParam (proc, pid::revGate, 0.0f); });
        if (history.getUndoDescription() != "Reverb Gate off")
            fail ("switch described as \"" + history.getUndoDescription() + "\"");
        history.undo();
    }

    // An envelope edit, with Tune (dragging the pitch curve's end point), is one step.
    {
        history.clear();
        const auto env0 = proc.getEnvelope (envPitch);
        const float tune0 = rawParam (proc, pid::tune);
        auto env = env0;
        env.movePoint (1, env.getPoints()[1].x, env.getPoints()[1].y + 7.0f);
        gesture ([&]
        {
            proc.setEnvelope (envPitch, env);
            setParam (proc, pid::tune, tune0 * 1.5f);
        });
        if (history.getNumUndoSteps() != 1 || history.getUndoDescription() != "pitch curve edit")
            fail ("envelope edit gave " + juce::String (history.getNumUndoSteps()) + " step(s), \"" + history.getUndoDescription() + "\"");
        history.undo();
        if (! sameEnv (envPitch, env0) || ! near (rawParam (proc, pid::tune), tune0))
            fail ("undo didn't restore the pitch curve and Tune");
        history.redo();
        if (! sameEnv (envPitch, env) || ! near (rawParam (proc, pid::tune), tune0 * 1.5f))
            fail ("redo didn't restore the edited pitch curve and Tune");
    }

    // A burst of changes during one gesture is one step.
    {
        history.clear();
        history.noteUserActivity();
        auto env = proc.getEnvelope (envBody);
        for (int i = 0; i < 40; ++i)
        {
            setParam (proc, pid::bodyLevel, -20.0f + (float) i * 0.3f);
            env.movePoint (0, 0.0f, 1.0f - (float) i * 0.01f);
            proc.setEnvelope (envBody, env);
            history.poll (true); // the mouse is still down
        }
        if (history.canUndo())
            fail ("a step was committed while the gesture was going on");
        history.poll (false);
        if (history.getNumUndoSteps() != 1)
            fail ("a 40-change drag made " + juce::String (history.getNumUndoSteps()) + " steps");
    }

    // Automation (changes without user activity) only moves the baseline.
    {
        history.clear();
        for (int i = 0; i < 20; ++i)
        {
            setParam (proc, pid::driveAmount, (float) i / 20.0f);
            history.poll (false);
        }
        if (history.canUndo())
            fail ("automation made " + juce::String (history.getNumUndoSteps()) + " undo steps");
        const float automated = rawParam (proc, pid::driveAmount);
        gesture ([&] { setParam (proc, pid::eqLow, 6.0f); });
        history.undo();
        if (! near (rawParam (proc, pid::driveAmount), automated) || ! near (rawParam (proc, pid::eqLow), 0.0f))
            fail ("undo after automation didn't go back to the automated state");

        // Session-only parameters are neither steps nor undone.
        history.clear();
        gesture ([&] { setParam (proc, pid::scTrigger, 1.0f); setParam (proc, pid::bodySolo, 1.0f); });
        if (history.canUndo())
            fail ("a sidechain / solo change became a step");
        gesture ([&] { setParam (proc, pid::eqHigh, 3.0f); });
        history.undo();
        if (rawParam (proc, pid::scTrigger) < 0.5f || rawParam (proc, pid::bodySolo) < 0.5f)
            fail ("undo changed the sidechain / solo");
        setParam (proc, pid::scTrigger, 0.0f);
        setParam (proc, pid::bodySolo, 0.0f);
    }

    // Activity that changes nothing expires, unless a menu is still open.
    {
        history.clear();
        history.noteUserActivity();
        history.poll (false); // mouse-up, nothing changed yet (a menu is opening)
        juce::Thread::sleep ((int) UndoHistory::activityGraceMs + 100);
        history.poll (true);  // the menu is still open
        setParam (proc, pid::clip, 3.0f); // the choice lands
        history.poll (false);
        if (history.getNumUndoSteps() != 1)
            fail ("a menu choice that landed after the grace period wasn't a step");

        history.clear();
        history.noteUserActivity(); // a click that did nothing
        juce::Thread::sleep ((int) UndoHistory::activityGraceMs + 100);
        history.poll (false);
        setParam (proc, pid::clip, 1.0f); // then automation
        history.poll (false);
        if (history.canUndo())
            fail ("stale activity turned automation into a step");
    }

    // A preset load is one step, and undo brings back the whole patch.
    {
        auto env = proc.getEnvelope (envNoise);
        env.addPoint (0.3f, 0.7f);
        proc.setEnvelope (envNoise, env);
        history.clear();
        const auto before = proc.createStateTree();
        const auto envs = [&proc]
        {
            std::array<CurveEnvelope, numEnvs> a;
            for (int e = 0; e < numEnvs; ++e)
                a[(size_t) e] = proc.getEnvelope ((EnvId) e);
            return a;
        }();

        proc.getPresets().load (2);
        history.poll (false);
        const auto loaded = proc.createStateTree();
        if (history.getNumUndoSteps() != 1 || history.getUndoDescription() != "preset load")
            fail ("preset load gave " + juce::String (history.getNumUndoSteps()) + " step(s), \"" + history.getUndoDescription() + "\"");
        history.undo();
        if (! proc.createStateTree().isEquivalentTo (before))
            fail ("undoing a preset load didn't restore the previous patch");
        for (int e = 0; e < numEnvs; ++e)
            if (! sameEnv ((EnvId) e, envs[(size_t) e]))
                fail ("undoing a preset load didn't restore envelope " + juce::String (e));
        history.redo();
        if (! proc.createStateTree().isEquivalentTo (loaded))
            fail ("redoing a preset load didn't restore the preset");

        // Saving only renames the patch: not a step.
        history.clear();
        history.noteUserActivity();
        proc.getState().state.setProperty (DrumFoundryProcessor::presetNameId, "Renamed", nullptr);
        history.poll (false);
        if (history.canUndo())
            fail ("a rename became a step");
    }

    // A sample load (asynchronous) is one step, style switch included.
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("DrumFoundryUndoCheck");
        dir.deleteRecursively();
        dir.createDirectory();
        juce::WavAudioFormat wav;
        const auto file = dir.getChildFile ("Undo Kick.wav");
        writeTestAudio (wav, file, 44100.0, 1, 0.2, 70.0f);

        proc.getPresets().load (0);
        history.clear();
        const int style0 = juce::roundToInt (rawParam (proc, pid::bodyStyle));

        proc.loadSample (slotBody, file);
        history.poll (false); // may still be decoding: must wait
        proc.getSampleBank().waitForPendingLoads (10000);
        history.poll (false);
        if (history.getNumUndoSteps() != 1 || history.getUndoDescription() != "sample load (Body)")
            fail ("sample load gave " + juce::String (history.getNumUndoSteps()) + " step(s), \"" + history.getUndoDescription() + "\"");

        const auto loaded = proc.getSample (slotBody);
        if (loaded == nullptr || juce::roundToInt (rawParam (proc, pid::bodyStyle)) != sampleStyleIndex (bodyStyleNames))
            fail ("the sample didn't load");

        // An unrelated undo keeps the decoded sample.
        gesture ([&] { setParam (proc, pid::bodyLevel, -3.0f); });
        history.undo();
        if (proc.getSample (slotBody) != loaded)
            fail ("undoing a knob decoded the sample again");

        history.undo();
        if (proc.getSamplePath (slotBody).isNotEmpty() || proc.getSample (slotBody) != nullptr
            || juce::roundToInt (rawParam (proc, pid::bodyStyle)) != style0)
            fail ("undoing the sample load didn't empty the slot and restore the style");
        history.redo();
        if (proc.getSamplePath (slotBody) != file.getFullPathName() || proc.getSample (slotBody) == nullptr
            || juce::roundToInt (rawParam (proc, pid::bodyStyle)) != sampleStyleIndex (bodyStyleNames))
            fail ("redoing the sample load didn't bring it back");

        proc.clearSample (slotBody);
        history.poll (false);
        if (history.getUndoDescription() != "sample clear (Body)")
            fail ("clearing a sample described as \"" + history.getUndoDescription() + "\"");
        history.undo();
        if (proc.getSample (slotBody) == nullptr)
            fail ("undoing a clear didn't bring the sample back");

        proc.getPresets().load (0);
        dir.deleteRecursively();
    }

    // The cap.
    {
        history.clear();
        for (int i = 0; i < UndoHistory::maxSteps + 30; ++i)
            gesture ([&] { setParam (proc, pid::outGain, -20.0f + (float) i * 0.1f); });
        if (history.getNumUndoSteps() != UndoHistory::maxSteps)
            fail ("history holds " + juce::String (history.getNumUndoSteps()) + " steps, not " + juce::String (UndoHistory::maxSteps));
        int undone = 0;
        while (history.undo())
            ++undone;
        if (undone != UndoHistory::maxSteps || ! near (rawParam (proc, pid::outGain), -20.0f + 29.0f * 0.1f))
            fail ("undid " + juce::String (undone) + " steps, down to " + juce::String (rawParam (proc, pid::outGain)));
        if (history.getNumRedoSteps() != UndoHistory::maxSteps)
            fail ("redo list holds " + juce::String (history.getNumRedoSteps()));
        // A new edit clears the redo list.
        gesture ([&] { setParam (proc, pid::outGain, 1.0f); });
        if (history.canRedo() || history.getNumUndoSteps() != 1)
            fail ("a new edit after undo didn't clear the redo list");
    }

    // Host recall: a different state forgets the history, the same one doesn't.
    {
        history.clear();
        gesture ([&] { setParam (proc, pid::eqMidGain, 4.0f); });
        juce::MemoryBlock same;
        proc.getStateInformation (same);
        proc.setStateInformation (same.getData(), (int) same.getSize());
        history.poll (false);
        if (history.getNumUndoSteps() != 1)
            fail ("re-sending the same state forgot the history");

        DrumFoundryProcessor other;
        other.getPresets().load (3);
        juce::MemoryBlock different;
        other.getStateInformation (different);
        proc.setStateInformation (different.getData(), (int) different.getSize());
        history.poll (true); // even mid-gesture
        if (history.canUndo() || history.canRedo())
            fail ("a host recall of another patch kept the history");
    }

    // The editor: buttons, shortcuts, layer tabs, and the history outliving it.
    {
        proc.getPresets().load (0);
        history.clear();
        using LS = ui::LayersSection;
        enum { noise = 2 };

        {
            std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
            auto* ed = dynamic_cast<DrumFoundryEditor*> (editor.get());
            auto* layers = findChild<LS> (*editor);
            auto* header = findChild<ui::HeaderBar> (*editor);
            if (ed == nullptr || layers == nullptr || header == nullptr)
            {
                fail ("no editor / layers / header");
                return failures;
            }
            if (header->getUndoButton().isEnabled() || header->getRedoButton().isEnabled())
                fail ("undo / redo enabled with an empty history");

            ed->addExtraLayer (noise);
            history.poll (false);
            if (history.getUndoDescription() != "add Noise layer")
                fail ("adding a layer described as \"" + history.getUndoDescription() + "\"");

            // Mute it (it keeps its tab), then edit something else and undo that.
            history.noteUserActivity(); // as the click would
            layers->getMuteButton (LS::firstExtraTab).onClick();
            history.poll (false);
            if (history.getUndoDescription() != "Layer 1 off")
                fail ("muting a layer described as \"" + history.getUndoDescription() + "\"");
            layers->update();
            gesture ([&] { setParam (proc, pid::eqLow, -4.0f); });
            ed->undo();
            if (! layers->hasTab (0))
                fail ("undoing an edit closed a muted layer's tab");
            ed->undo(); // the mute
            ed->undo(); // the add
            if (rawParam (proc, "lay1On") > 0.5f || layers->hasTab (0))
                fail ("undoing \"add layer\" left the layer or its tab");
            // The tooltips name the keys set in the settings (KeyMap), which may not be the defaults here.
            juce::SharedResourcePointer<ui::KeyMap> keys;
            const auto withKeys = [&keys] (const juce::String& text, ui::KeyAction action)
            {
                const auto names = keys->describeKeys (action);
                return names.isEmpty() ? text : text + " (" + names + ")";
            };
            if (! header->getRedoButton().isEnabled()
                || header->getRedoButton().getTooltip() != withKeys ("Redo add Noise layer", ui::KeyAction::redo))
                fail ("redo button: " + header->getRedoButton().getTooltip());
            ed->redo();
            if (rawParam (proc, "lay1On") < 0.5f || ! layers->hasTab (0))
                fail ("redoing \"add layer\" didn't bring the layer and its tab back");
            if (! header->getUndoButton().isEnabled()
                || header->getUndoButton().getTooltip() != withKeys ("Undo add Noise layer", ui::KeyAction::undo))
                fail ("undo button: " + header->getUndoButton().getTooltip());
        }

        // Reopened: the history is still there, and the undo / redo keys work (whichever keys
        // the settings have for them: Cmd+Z and Cmd+Shift+Z by default).
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        auto* ed = dynamic_cast<DrumFoundryEditor*> (editor.get());
        auto* header = findChild<ui::HeaderBar> (*editor);
        if (! history.canUndo() || header == nullptr || ! header->getUndoButton().isEnabled())
            fail ("the history didn't survive reopening the editor");
        juce::SharedResourcePointer<ui::KeyMap> keys;
        const auto undoKey = keys->getKey (ui::KeyAction::undo, 0), redoKey = keys->getKey (ui::KeyAction::redo, 0);
        if (undoKey.isValid()
            && (ed == nullptr || ! ed->keyPressed (undoKey) || rawParam (proc, "lay1On") > 0.5f))
            fail (ui::KeyMap::describe (undoKey) + " didn't undo");
        if (undoKey.isValid() && redoKey.isValid()
            && (ed == nullptr || ! ed->keyPressed (redoKey) || rawParam (proc, "lay1On") < 0.5f))
            fail (ui::KeyMap::describe (redoKey) + " didn't redo");
        if (ed != nullptr && ! keys->find (juce::KeyPress ('z', {}, 0)) && ed->keyPressed (juce::KeyPress ('z', {}, 0)))
            fail ("a plain Z was taken");
    }

    std::printf (failures == 0 ? "undo / redo: ok\n" : "undo / redo: %d failure(s)\n", failures);
    return failures;
}

//==============================================================================
/** Extra layers' own curves: on Own the engine reads layer k's own pitch and level curve
    (and no other layer's), coupling picks a core layer's, Flat neither; and patches from
    before format 4 (one Envelope choice per layer) load with Pitch and Level set so they
    sound the same. */
int checkLayerOwnCurves()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: layer own curves: %s\n", msg.toRawUTF8()); ++failures; };

    constexpr double sr = 48000.0;
    const auto flat = engineTables (true);
    const auto with = [] (EnvTables t, int env, float value)
    {
        t.data[(size_t) env].fill (value);
        return t;
    };
    const auto sineLayer = [&] (int k, int pitchLink, int levelLink, const EnvTables& t)
    {
        auto p = engineParams (false, false, false);
        p.tuneHz = 100.0f;
        auto& l = p.layers[(size_t) k];
        l.on = true;
        l.source = layerSine;
        l.pitchEnv = pitchLink;
        l.env = levelLink;
        l.gain = 0.5f;
        return renderVoice (p, t, sr, (int) (0.3 * sr));
    };
    const auto hzOf = [&] (const juce::AudioBuffer<float>& hit) { return zeroCrossingHz (hit, 0, sr, (int) (0.02 * sr), (int) (0.28 * sr)); };

    // --- Own: each layer its own curves ----------------------------------------------------------
    juce::String report;
    for (int k = 0; k < kNumExtraLayers; ++k)
    {
        const auto silenced = sineLayer (k, layerEnvOwn, layerEnvOwn, with (flat, layerOwnLevelEnvelope (k), 0.0f));
        const auto other = sineLayer (k, layerEnvOwn, layerEnvOwn, with (flat, layerOwnLevelEnvelope ((k + 1) % kNumExtraLayers), 0.0f));
        if (peakOf (silenced) > 0.0f || peakOf (other) < 0.1f)
            fail ("layer " + juce::String (k + 1) + " on Own doesn't follow its own level curve (alone)");
        const double up = hzOf (sineLayer (k, layerEnvOwn, layerEnvOwn, with (flat, layerOwnPitchEnvelope (k), 12.0f)));
        const double otherUp = hzOf (sineLayer (k, layerEnvOwn, layerEnvOwn, with (flat, layerOwnPitchEnvelope ((k + 1) % kNumExtraLayers), 12.0f)));
        report << "layer " << (k + 1) << " own +12: " << juce::String (up, 1) << " Hz  ";
        if (std::abs (up / 200.0 - 1.0) > 0.005 || std::abs (otherUp / 100.0 - 1.0) > 0.005)
            fail ("layer " + juce::String (k + 1) + " on Own doesn't follow its own pitch curve (alone): " + juce::String (up, 1)
                  + " / " + juce::String (otherUp, 1) + " Hz (expected 200 / 100)");
    }

    // --- Coupled, and Flat ---------------------------------------------------------------------------
    {
        const auto bodyUp = with (with (flat, envPitch, 12.0f), layerOwnPitchEnvelope (0), -12.0f);
        const double coupled = hzOf (sineLayer (0, layerEnvBody, layerEnvOwn, bodyUp));
        const double flatHz = hzOf (sineLayer (0, layerEnvFlat, layerEnvOwn, bodyUp));
        const double own = hzOf (sineLayer (0, layerEnvOwn, layerEnvOwn, bodyUp));
        report << "| coupled to Body +12: " << juce::String (coupled, 1) << " Hz, Flat " << juce::String (flatHz, 1) << ", own -12 " << juce::String (own, 1);
        if (std::abs (coupled / 200.0 - 1.0) > 0.005 || std::abs (flatHz / 100.0 - 1.0) > 0.005 || std::abs (own / 50.0 - 1.0) > 0.005)
            fail ("Pitch Body / Flat / Own play " + juce::String (coupled, 1) + " / " + juce::String (flatHz, 1) + " / " + juce::String (own, 1)
                  + " Hz (expected 200 / 100 / 50)");

        const auto noiseQuiet = with (with (flat, envNoise, 0.0f), layerOwnLevelEnvelope (0), 0.0f);
        if (peakOf (sineLayer (0, layerEnvFlat, layerEnvNoise, noiseQuiet)) > 0.0f || peakOf (sineLayer (0, layerEnvFlat, layerEnvFlat, noiseQuiet)) < 0.1f)
            fail ("Level Noise doesn't follow the Noise level curve, or Flat follows a curve");
    }
    std::printf ("layer own curves: %s\n", report.toRawUTF8());

    // --- Format 1-3 patches -------------------------------------------------------------------------
    {
        struct Case { int source, oldEnv, pitch, level; };
        // Old Envelope (format 2-3 indices: Body, Noise, Click, Flat) -> Pitch, Level.
        const Case cases[] { { layerSine, layerEnvFlat, layerEnvBody, layerEnvFlat },
                             { layerClick, layerEnvBody, layerEnvFlat, layerEnvFlat },
                             { layerNoise, layerEnvNoise, layerEnvNoise, layerEnvNoise } };
        DrumFoundryProcessor proc;
        for (int k = 0; k < kNumExtraLayers; ++k)
        {
            setParam (proc, layerParamId (k, pid::layOn).toRawUTF8(), 1.0f);
            setParam (proc, layerParamId (k, pid::laySource).toRawUTF8(), (float) cases[k].source);
            setParam (proc, layerParamId (k, pid::layEnv).toRawUTF8(), (float) cases[k].oldEnv);
        }
        auto tree = proc.createStateTree();
        tree.setProperty (DrumFoundryProcessor::formatVersionId, 3, nullptr);
        for (int i = tree.getNumChildren(); --i >= 0;)
            if (tree.getChild (i).getProperty ("id").toString().endsWith (pid::layPitchEnv))
                tree.removeChild (i, nullptr);
        auto envs = tree.getChildWithName (DrumFoundryProcessor::envelopesId);
        for (int e = numCoreEnvs; e < numEnvs; ++e)
            envs.removeChild (envs.getChildWithName (DrumFoundryProcessor::envelopeTreeId (e)), nullptr);

        DrumFoundryProcessor loaded;
        loaded.loadStateTree (tree);
        for (int k = 0; k < kNumExtraLayers; ++k)
        {
            const int pitch = juce::roundToInt (rawParam (loaded, layerParamId (k, pid::layPitchEnv).toRawUTF8()));
            const int level = juce::roundToInt (rawParam (loaded, layerParamId (k, pid::layEnv).toRawUTF8()));
            if (pitch != cases[k].pitch || level != cases[k].level)
                fail ("a format 3 " + layerSourceNames[cases[k].source] + " layer on " + layerEnvNames[cases[k].oldEnv] + " loaded as Pitch "
                      + layerEnvNames[pitch] + ", Level " + layerEnvNames[level] + " (expected " + layerEnvNames[cases[k].pitch] + ", "
                      + layerEnvNames[cases[k].level] + ")");
        }
        if (! (loaded.getEnvelope ((EnvId) layerOwnLevelEnvelope (0)) == defaultEnvelope ((EnvId) layerOwnLevelEnvelope (0))))
            fail ("a format 3 patch's layers don't get the default own curves");

        // Format 1 had Flat at index 2: a Sine layer on it still follows the Body pitch.
        auto old = tree.createCopy();
        old.setProperty (DrumFoundryProcessor::formatVersionId, 1, nullptr);
        for (auto child : old)
            if (child.getProperty ("id").toString() == layerParamId (0, pid::layEnv))
                child.setProperty ("value", 2.0, nullptr);
        loaded.loadStateTree (old);
        if (juce::roundToInt (rawParam (loaded, "lay1PitchEnv")) != layerEnvBody || juce::roundToInt (rawParam (loaded, "lay1Env")) != layerEnvFlat)
            fail ("a format 1 Sine layer on Flat didn't load as Pitch Body, Level Flat");

        // A session saved now round-trips as it is (no migration).
        setParam (loaded, "lay1PitchEnv", (float) layerEnvOwn);
        DrumFoundryProcessor again;
        again.loadStateTree (loaded.createStateTree());
        if (juce::roundToInt (rawParam (again, "lay1PitchEnv")) != layerEnvOwn)
            fail ("a format 4 patch's Pitch choice didn't survive a round trip");
        std::printf ("layer own curves: format 1-3 patches load with Pitch / Level set as they played\n");
    }
    return failures;
}

int cmdCheck()
{
    int failures = 0;
    auto fail = [&] (const juce::String& msg) { std::printf ("FAIL: %s\n", msg.toRawUTF8()); ++failures; };

    DrumFoundryProcessor proc;
    const auto& entries = proc.getPresets().getEntries();
    for (int i = 0; i < (int) entries.size(); ++i)
    {
        if (! entries[(size_t) i].isFactory)
            continue;
        proc.getPresets().load (i);
        for (double sr : { 44100.0, 96000.0 })
            for (int note : { 36, 60, 84 })
            {
                const auto hit = renderCurrent (proc, sr, note, 1.0f);
                bool finite = true;
                float peak = 0;
                for (int c = 0; c < 2; ++c)
                    for (int s = 0; s < hit.getNumSamples(); ++s)
                    {
                        const float v = hit.getSample (c, s);
                        finite &= std::isfinite (v);
                        peak = juce::jmax (peak, std::abs (v));
                    }
                const auto tag = entries[(size_t) i].name + " @" + juce::String (sr) + " note " + juce::String (note);
                if (! finite)      fail (tag + ": non-finite samples");
                if (peak > 1.001f) fail (tag + ": peak " + juce::String (peak) + " exceeds clipper ceiling");
                if (peak < 0.01f)  fail (tag + ": nearly silent (" + juce::String (peak) + ")");
            }
    }

    // State round trip
    proc.getPresets().load (6);
    auto env = proc.getEnvelope (envNoise);
    env.addPoint (0.33f, 0.77f);
    proc.setEnvelope (envNoise, env);
    juce::MemoryBlock state;
    proc.getStateInformation (state);

    DrumFoundryProcessor other;
    other.setStateInformation (state.getData(), (int) state.getSize());
    if (! (other.getEnvelope (envNoise) == env))
        fail ("envelope did not survive state round trip");
    if (other.getPresets().getCurrentName() != proc.getPresets().getCurrentName())
        fail ("preset name did not survive state round trip");
    for (const auto& id : allParameterIds())
    {
        const float a = proc.getState().getRawParameterValue (id)->load();
        const float b = other.getState().getRawParameterValue (id)->load();
        if (std::abs (a - b) > 1.0e-5f * juce::jmax (1.0f, std::abs (a)))
            fail ("param " + id + " differs after round trip (" + juce::String (a) + " vs " + juce::String (b) + ")");
    }

    // Corrupt session / preset data must never produce non-finite audio.
    {
        const auto checkLoaded = [&] (DrumFoundryProcessor& p, const juce::String& tag)
        {
            for (const auto& id : allParameterIds())
                if (const float v = p.getState().getRawParameterValue (id)->load(); ! std::isfinite (v))
                    fail (tag + ": param " + id + " is non-finite after load (" + juce::String (v) + ")");

            const auto sp = p.readParams();
            if (! allFinite (sp))
                fail (tag + ": readParams() returned non-finite values");
            if (! juce::isPositiveAndBelow (sp.clickType, clickTypeNames.size()))
                fail (tag + ": clickType " + juce::String (sp.clickType) + " out of range");
            if (! juce::isPositiveAndBelow (sp.driveType, driveTypeNames.size()))
                fail (tag + ": driveType " + juce::String (sp.driveType) + " out of range");

            for (int e = 0; e < numEnvs; ++e)
            {
                const auto loaded = p.getEnvelope ((EnvId) e);
                if (! allFinite (loaded) || loaded.size() < 2)
                    fail (tag + ": " + envelopeName ((EnvId) e) + " envelope has non-finite or too few points");
            }

            EnvTables tables;
            p.bakeTables (tables);
            if (! allFinite (tables))
                fail (tag + ": baked envelope tables contain non-finite values");

            if (! allFinite (renderCurrent (p, 48000.0)))
                fail (tag + ": offline render has non-finite samples");

            // And through the real-time path.
            p.prepareToPlay (48000.0, 512);
            juce::AudioBuffer<float> buf (2, 512);
            bool finite = true;
            for (int b = 0; b < 200; ++b) // ~2 s, longer than the maximum Length
            {
                juce::MidiBuffer m;
                if (b == 0)
                    m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 10);
                p.processBlock (buf, m);
                finite &= allFinite (buf);
            }
            if (! finite)
                fail (tag + ": processBlock produced non-finite samples");
        };

        const auto asString = [] (const char* s) { return juce::var (s); };
        const auto asFloat  = [] (const char* s) { return juce::var (juce::String (s).getDoubleValue()); };

        // Through setStateInformation, as a host would restore a damaged session.
        {
            DrumFoundryProcessor p;
            juce::MemoryBlock data;
            if (auto xml = makeCorruptState (p, asString).createXml())
                juce::AudioProcessor::copyXmlToBinary (*xml, data);
            p.setStateInformation (data.getData(), (int) data.getSize());
            checkLoaded (p, "corrupt state (setStateInformation)");
        }

        // Straight into loadStateTree with real NaN / inf floats in the vars.
        {
            DrumFoundryProcessor p;
            p.loadStateTree (makeCorruptState (p, asFloat));
            checkLoaded (p, "corrupt state (loadStateTree)");
        }
    }

    // ParamReader on its own: poke non-finite / out-of-range values straight into
    // the raw atomics (bypassing any load-time sanitising) and read a snapshot.
    {
        DrumFoundryProcessor p;
        auto& s = p.getState();
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        const auto ids = allParameterIds();
        for (int i = 0; i < ids.size(); ++i)
            s.getRawParameterValue (ids[i])->store (i % 3 == 0 ? nan : i % 3 == 1 ? inf : -inf);
        s.getRawParameterValue (pid::tune)->store (nan);

        const auto sp = p.readParams();
        if (! allFinite (sp))
            fail ("ParamReader passed non-finite raw values through");
        if (! juce::isPositiveAndBelow (sp.clickType, clickTypeNames.size())
             || ! juce::isPositiveAndBelow (sp.driveType, driveTypeNames.size()))
            fail ("ParamReader gave out-of-range choice indices (" + juce::String (sp.clickType) + ", "
                  + juce::String (sp.driveType) + ")");
        if (! juce::approximatelyEqual (sp.tuneHz, DrumParams().tuneHz))
            fail ("ParamReader did not fall back to the default for a NaN tune (" + juce::String (sp.tuneHz) + ")");

        s.getRawParameterValue (pid::clickType)->store (99.0f);
        s.getRawParameterValue (pid::driveType)->store (-7.0f);
        const auto clamped = p.readParams();
        if (clamped.clickType != clickTypeNames.size() - 1 || clamped.driveType != 0)
            fail ("ParamReader did not clamp choice indices (" + juce::String (clamped.clickType) + ", "
                  + juce::String (clamped.driveType) + ")");

        if (! allFinite (renderCurrent (p, 48000.0)))
            fail ("render from non-finite raw parameter values has non-finite samples");
    }

    // Envelopes built directly from non-finite points must bake finite tables.
    {
        constexpr float nan = std::numeric_limits<float>::quiet_NaN();
        constexpr float inf = std::numeric_limits<float>::infinity();

        const CurveEnvelope mixed ({ { 0.0f, 1.0f, nan }, { nan, 0.5f, 0.0f }, { 0.3f, inf, 0.2f },
                                     { 0.5f, 0.4f, -inf }, { -inf, 0.2f, 0.0f }, { 1.0f, 0.0f, 0.0f } });
        const CurveEnvelope allBad ({ { nan, nan, nan }, { inf, -inf, inf }, { 0.5f, nan, 0.0f } });

        EnvTables tables;
        tables.bake (envPitch, mixed);
        tables.bake (envBody, allBad);
        tables.bake (envNoise, CurveEnvelope ({ { nan, nan, nan } }));
        if (! allFinite (tables))
            fail ("envelope built from non-finite points baked a non-finite table");
        if (! allFinite (mixed) || mixed.size() != 3)
            fail ("non-finite points survived sanitise (" + juce::String (mixed.size()) + " points left, expected 3)");
        if (! (allBad == CurveEnvelope()))
            fail ("envelope with no finite points did not fall back to the default shape");

        for (float x : { nan, inf, -inf })
            if (! std::isfinite (tables.lookup (envPitch, x)))
                fail ("EnvTables::lookup returned non-finite for x = " + juce::String (x));
        if (! std::isfinite (CurveEnvelope::shape (0.5f, nan)))
            fail ("CurveEnvelope::shape is non-finite for a NaN curve");

        auto edited = mixed;
        edited.movePoint (1, nan, 0.5f);
        edited.movePoint (1, 0.5f, inf);
        edited.setCurve (0, nan);
        if (edited.addPoint (nan, 0.5f) >= 0 || edited.addPoint (0.7f, -inf) >= 0 || ! (edited == mixed))
            fail ("addPoint / movePoint / setCurve accepted non-finite input");
    }

    // A host may call processBlock before prepareToPlay; that must not crash
    // or produce garbage.
    {
        DrumFoundryProcessor fresh;
        juce::AudioBuffer<float> buf (2, 512);
        bool finite = true;
        float peak = 0.0f;
        for (int b = 0; b < 8; ++b)
        {
            juce::MidiBuffer m;
            if (b == 0)
                m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
            for (int c = 0; c < 2; ++c)
                juce::FloatVectorOperations::fill (buf.getWritePointer (c), 0.25f, 512); // stale host data
            fresh.processBlock (buf, m);
            finite &= allFinite (buf);
            peak = juce::jmax (peak, buf.getMagnitude (0, 512));
        }
        if (! finite)
            fail ("processBlock before prepareToPlay produced non-finite samples");
        else
            std::printf ("processBlock before prepareToPlay: finite, peak %.3g%s\n", peak, peak > 0.0f ? " (not silent)" : " (silent)");
    }

    // Arbitrary host block sizes must give identical output to one big block,
    // with the reverb off and with it on (gated, and ringing freely).
    for (int mode = 0; mode < 3; ++mode)
    {
        constexpr int total = 24000;
        DrumFoundryProcessor a, b;
        for (auto* p : { &a, &b })
        {
            if (mode > 0)
            {
                setParam (*p, pid::revMix, 0.7f);
                setParam (*p, pid::revGate, mode == 1 ? 1.0f : 0.0f);
            }
            p->prepareToPlay (48000.0, total);
        }
        const int hits[] { 100, 9000 };
        juce::AudioBuffer<float> big (2, total), small (2, total);
        juce::MidiBuffer m;
        for (int h : hits)
            m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), h);
        a.processBlock (big, m);
        for (int pos = 0, sz = 1; pos < total; pos += sz, sz = sz % 700 + 37)
        {
            const int len = juce::jmin (sz, total - pos);
            juce::AudioBuffer<float> chunk (2, len);
            juce::MidiBuffer cm;
            for (int h : hits)
                if (pos <= h && h < pos + len)
                    cm.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), h - pos);
            b.processBlock (chunk, cm);
            small.copyFrom (0, pos, chunk, 0, 0, len);
            small.copyFrom (1, pos, chunk, 1, 0, len);
        }
        float maxDiff = 0;
        for (int c = 0; c < 2; ++c)
            for (int s = 0; s < total; ++s)
                maxDiff = juce::jmax (maxDiff, std::abs (big.getSample (c, s) - small.getSample (c, s)));
        const char* name = mode == 0 ? "reverb off" : mode == 1 ? "reverb on, gated" : "reverb on, gate off";
        if (maxDiff > 1.0e-3f)
            fail (juce::String ("block-size dependence (") + name + "): max diff " + juce::String (maxDiff));
        else
            std::printf ("block-size independence (%s): max diff %.2e\n", name, maxDiff);
    }

    failures += checkSidechain();
    failures += checkReverb();
    failures += checkSizeAndWidth();
    failures += checkOutputFilter();
    failures += checkEffectSwitches();
    failures += checkClipper();
    failures += checkAnalyzer();
    failures += checkSamples();
    failures += checkEngineStyles();
    failures += checkLayerSources();
    failures += checkLayerOffsets();
    failures += checkSolo();
    failures += checkPresetLayers();
    failures += checkClap();
    failures += checkImpactClicks();
    failures += checkPreClipCapture();
    failures += checkReverbEngines();
    failures += checkFxChain();
    failures += checkPitchAndLevels();
    failures += checkLayerCurves();
    failures += checkLayerOwnCurves();
    failures += drum::eqchecks::run();
    failures += drum::compchecks::run();
    failures += drum::orderchecks::run();
    failures += checkEditor();
    failures += checkEditorControls();
    failures += checkEditorChart();
    failures += checkChartColours();
    failures += checkPresetValues();
    failures += checkChoiceSelectors();
    failures += checkChartAll();
    failures += checkLfos();
    failures += checkUndo();
    failures += checkNaturalLive();

    std::printf (failures == 0 ? "all checks passed\n" : "%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}

} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::ScopedNoDenormals noDenormals;

    const juce::String cmd = argc > 1 ? argv[1] : "";
    const auto cwd = juce::File::getCurrentWorkingDirectory();

    if (cmd == "render")
        return cmdRender (cwd.getChildFile (argc > 2 ? argv[2] : "renders"),
                          argc > 3 && juce::String (argv[3]) == "float" ? 32 : 24);
    if (cmd == "snapshot")
        return cmdSnapshot (cwd.getChildFile (argc > 2 ? argv[2] : "snapshot.png"),
                            argc > 3 ? juce::String (argv[3]).getIntValue() : -1,
                            argc > 4 ? juce::String (argv[4]).getFloatValue() : 1.0f,
                            argc > 5 ? juce::String (argv[5]).getIntValue() : 0,
                            argc > 6 ? juce::StringArray (argv + 6, argc - 6) : juce::StringArray());
    if (cmd == "bench")
        return cmdBench();
    if (cmd == "check")
        return cmdCheck();
    if (cmd == "analyzer")
        return cmdAnalyzer (cwd.getChildFile (argc > 2 ? argv[2] : "shots"),
                            argc > 3 ? juce::String (argv[3]).getIntValue() : 4,
                            argc > 4 ? juce::String (argv[4]).getIntValue() : 520,
                            argc > 5 ? juce::String (argv[5]).getIntValue() : 240);

    if (cmd == "measure" || cmd == "measure-presets" || cmd == "emit-preset")
        return drum::measure::run (cmd, juce::StringArray (argv + 2, argc - 2));
    if (cmd == "check-eq")
        return drum::eqchecks::run() == 0 ? 0 : 1;
    if (cmd == "check-comp")
        return drum::compchecks::run() == 0 ? 0 : 1;
    if (cmd == "check-order")
        return drum::orderchecks::run() == 0 ? 0 : 1;
    if (cmd == "measure-clicks")
        return drum::clicks::run (juce::StringArray (argv + 2, argc - 2));
    if (cmd == "styles")
        return cmdStyles (cwd.getChildFile (argc > 2 ? argv[2] : "renders-engine"));
    if (cmd == "check-clipper")
        return checkClipper() == 0 ? 0 : 1;
    if (cmd == "check-undo")
        return checkUndo() == 0 ? 0 : 1;
    if (cmd == "check-editor")
        return checkEditor() + checkEditorControls() + checkEditorChart() == 0 ? 0 : 1;
    if (cmd == "check-samples")
        return checkSamples() == 0 ? 0 : 1;
    if (cmd == "samples-ui")
        return cmdSamplesUi (cwd.getChildFile (argc > 2 ? argv[2] : "shots"));

    std::printf ("usage: drumfoundry-tools render <dir> [float] | snapshot <png> [preset] [scale] [tab] [param=value | browser | browse=<slot> | samples | layer=<n> | hit]... | bench | check | analyzer <dir> [preset] [w] [h] | check-samples | check-clipper | samples-ui [dir] | styles <dir> | measure <files> | measure-presets [kick|snare|all] | emit-preset <files>\n");
    return 1;
}
