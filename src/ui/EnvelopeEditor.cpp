// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "EnvelopeEditor.h"
#include "ClipTransferPreview.h"
#include "../dsp/Compressor.h"
#include "FoundryLookAndFeel.h"

namespace drum::ui
{

namespace
{
    constexpr float kRulerWidth = 56.0f;
    constexpr float kTimeAxisHeight = 18.0f;
    constexpr float kPadTop = 12.0f;
    constexpr float kPadRight = 12.0f;
    constexpr float kLaneGap = 6.0f;          // between the plot and the timing lanes
    constexpr float kLaneStripHeight = 40.0f; // at least; more lanes (extra layers) make it taller
    constexpr float kLaneSpacing = 1.5f;
    constexpr float kMaxLaneHeight = 11.0f;
    constexpr float kMinLaneHeight = 9.0f;    // still room for the lane's name
    constexpr float kPointHitRadius = 9.0f;
    constexpr float kHandleHitRadius = 8.0f;
    constexpr float kGateHitDistance = 5.0f;
    constexpr float kMinViewSpan = 0.01f;
    constexpr float kOffsetSnapPixels = 5.0f;  // a lane dragged this close to 0 ms snaps to it

    // Pitch axis: 0 Hz - 16 kHz on a warped scale, w = log2 (1 + f / kWarpHz). Near 0 Hz
    // it's close to linear (so 0 Hz is on it); above about 50 Hz one unit is about an octave.
    constexpr float kWarpHz = 20.0f;
    constexpr float kMaxHz = 16000.0f;
    constexpr float kFullLo = 0.0f;       // w (0 Hz)
    constexpr float kFullHi = 9.645658f;  // w (16 kHz) = log2 (801)
    constexpr float kMinBandUnits = 1.0f;

    float hzToW (float hz) noexcept { return std::log2 (1.0f + juce::jmax (0.0f, hz) / kWarpHz); }
    float wToHz (float w) noexcept  { return juce::jmax (0.0f, kWarpHz * (std::exp2 (w) - 1.0f)); }

    // Amplitude axis: 0 dB sits this far up; the rest is the clip zone, up to +12 dB (in dB).
    constexpr float kUnityFrac = 0.78f;
    constexpr float kHeadroomDb = 12.0f;

    const juce::String minusSign (juce::CharPointer_UTF8 ("\xe2\x88\x92"));
    const juce::String midDot (juce::CharPointer_UTF8 ("  \xc2\xb7  "));

    juce::String formatMs (float ms)
    {
        if (std::abs (ms) < 0.01f) return "0";
        const auto sign = ms < 0.0f ? minusSign : juce::String();
        ms = std::abs (ms);
        if (ms >= 1000.0f) return sign + juce::String (ms / 1000.0f, 2) + " s";
        if (ms < 10.0f)    return sign + juce::String (ms, 1) + " ms";
        return sign + juce::String (juce::roundToInt (ms)) + " ms";
    }

    /** "+10 ms", "−8.5 ms", "0 ms". */
    juce::String formatOffset (float ms)
    {
        if (std::abs (ms) < 0.05f)
            return "0 ms";
        const float a = std::abs (ms);
        const bool whole = std::abs (a - std::round (a)) < 0.05f;
        return (ms < 0.0f ? minusSign : juce::String ("+"))
             + (a >= 10.0f || whole ? juce::String (juce::roundToInt (a)) : juce::String (a, 1)) + " ms";
    }

    /** Ruler label: "0", "50", "200", "1k", "2.5k", "16k". */
    juce::String formatHz (float hz)
    {
        if (hz >= 1000.0f)
        {
            const float k = hz / 1000.0f;
            return (std::abs (k - std::round (k)) < 0.01f ? juce::String (juce::roundToInt (k)) : juce::String (k, 1)) + "k";
        }
        return juce::String (juce::roundToInt (hz));
    }

    /** Readout: "0.4 Hz", "213 Hz", "1.25 kHz". */
    juce::String formatHzLong (float hz)
    {
        if (hz < 9.95f)
            return juce::String (juce::jmax (0.0f, hz), 1) + " Hz";
        return hz >= 1000.0f ? juce::String (hz / 1000.0f, hz >= 10000.0f ? 1 : 2) + " kHz"
                             : juce::String (juce::roundToInt (hz)) + " Hz";
    }

    /** "  ·  G0" for a frequency with a MIDI note (8.2 Hz and up), otherwise nothing. */
    juce::String noteSuffix (float hz)
    {
        return hz >= 8.1f ? midDot + noteName (frequencyToMidi (hz)) : juce::String();
    }

    juce::String formatGain (float v)
    {
        if (v <= 1.0e-4f)
            return "-inf dB";
        const float db = 20.0f * std::log10 (v);
        if (std::abs (db) < 0.05f)
            return "0.0 dB";
        return (db > 0.0f ? juce::String ("+") : minusSign) + juce::String (std::abs (db), 1) + " dB";
    }

    float ampToFrac (float v) noexcept
    {
        if (v <= 1.0f)
            return juce::jmax (0.0f, v) * kUnityFrac;
        const float db = 20.0f * std::log10 (v);
        return kUnityFrac + (1.0f - kUnityFrac) * juce::jmin (1.0f, db / kHeadroomDb);
    }

    float fracToAmp (float f) noexcept
    {
        if (f <= kUnityFrac)
            return juce::jmax (0.0f, f / kUnityFrac);
        const float db = (f - kUnityFrac) / (1.0f - kUnityFrac) * kHeadroomDb;
        return std::pow (10.0f, db / 20.0f);
    }

    /** "NOISE 2" -> "Noise 2". */
    juce::String titleCase (const juce::String& s)
    {
        return s.substring (0, 1) + s.substring (1).toLowerCase();
    }

    /** The core layer (an EnvelopeCanvas layer: 0 Body, 1 Noise, 2 Click) a core curve belongs to. */
    int layerOfCore (int env) noexcept
    {
        return env == envNoise || env == envNoisePitch ? 1 : env == envClick || env == envClickPitch ? 2 : 0;
    }


    void hatch (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour, float spacing)
    {
        if (area.isEmpty())
            return;
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (area.toNearestInt());
        g.setColour (colour);
        for (float hx = area.getX() - area.getHeight(); hx < area.getRight(); hx += spacing)
            g.drawLine (hx, area.getBottom(), hx + area.getHeight(), area.getY(), 1.0f);
    }
}

//==============================================================================
juce::String EnvelopeCanvas::curveName (int env)
{
    switch (env)
    {
        case envPitch:      return "Body pitch";
        case envBody:       return "Body level";
        case envNoise:      return "Noise level";
        case envNoisePitch: return "Noise pitch";
        case envClickPitch: return "Click pitch";
        case envClick:      return "Click level";
        default:            break;
    }
    if (const int k = layerOfEnvelope (env); k >= 0)
        return "Layer " + juce::String (k + 1) + (isPitchEnvelope (env) ? " pitch" : " level");
    return {};
}

//==============================================================================
EnvelopeCanvas::EnvelopeCanvas (DrumFoundryProcessor& p) : processor (p)
{
    reloadFromProcessor();
    updateLanes();
}

EnvelopeCanvas::~EnvelopeCanvas()
{
    endGestures(); // a drag the editor closed under
}

void EnvelopeCanvas::endGestures()
{
    for (auto* param : gestureParams)
        param->endChangeGesture();
    gestureParams.clear();
}

void EnvelopeCanvas::reloadFromProcessor()
{
    seenEnvelopeEdits = processor.getEnvelopeEditCount();
    for (int e = 0; e < numEnvs; ++e)
        envs[(size_t) e] = processor.getEnvelope ((EnvId) e);

    endGestures();
    hover = drag = {};
    refreshParams();
    refreshActiveCurve();
    updateFocusBand();
    repaint();
}

void EnvelopeCanvas::syncEnvelopes()
{
    if (processor.getEnvelopeEditCount() == seenEnvelopeEdits)
        return;
    seenEnvelopeEdits = processor.getEnvelopeEditCount();
    for (int e = 0; e < numEnvs; ++e)
        envs[(size_t) e] = processor.getEnvelope ((EnvId) e);
    if (drag.kind == Hit::point || drag.kind == Hit::handle)
        drag = {};
    hover = {};
    repaint();
}

void EnvelopeCanvas::refreshParams()
{
    auto& state = processor.getState();
    const auto read = [&state] (const char* id, float& dest, float lo, float hi)
    {
        if (const auto* v = state.getRawParameterValue (id))
            if (const float x = v->load(); std::isfinite (x))
                dest = juce::jlimit (lo, hi, x);
    };
    read (pid::tune, tuneHz, 1.0f, kMaxHz);
    read (pid::length, lengthMs, 1.0f, 1.0e6f);
    read (pid::clickTone, clickToneHz, 1.0f, 30000.0f);
    read (pid::noiseHP, noiseLowHz, 1.0f, 30000.0f);
    read (pid::noiseLP, noiseHighHz, 1.0f, 30000.0f);

    const auto raw = [&state] (const juce::String& id, float fallback)
    {
        const auto* v = state.getRawParameterValue (id);
        const float x = v != nullptr ? v->load() : fallback;
        return std::isfinite (x) ? x : fallback;
    };
    for (int k = 0; k < kNumExtraLayers; ++k)
    {
        auto& x = extras[(size_t) k];
        x.on = raw (layerParamId (k, pid::layOn), 0.0f) >= 0.5f;
        x.source = juce::jlimit (0, numLayerSources - 1, juce::roundToInt (raw (layerParamId (k, pid::laySource), 0.0f)));
        x.pitchLink = juce::jlimit (0, layerEnvNames.size() - 1, juce::roundToInt (raw (layerParamId (k, pid::layPitchEnv), layerEnvOwn)));
        x.levelLink = juce::jlimit (0, layerEnvNames.size() - 1, juce::roundToInt (raw (layerParamId (k, pid::layEnv), layerEnvOwn)));
        x.tuneSt = raw (layerParamId (k, pid::layTune), 0.0f);
        x.lowHz = juce::jlimit (1.0f, 30000.0f, raw (layerParamId (k, pid::layHP), 20.0f));
        x.highHz = juce::jlimit (1.0f, 30000.0f, raw (layerParamId (k, pid::layLP), 20000.0f));
        x.toneHz = juce::jlimit (1.0f, 30000.0f, raw (layerParamId (k, pid::layTone), 4500.0f));
    }

    // Extra layers are named as their tabs are ("NOISE 2", "BODY 2", "SINE", "SAMPLE 1").
    // One that's off is named by its source alone.
    std::array<int, numLayerSources> total {}, count {};
    for (const auto& x : extras)
        if (x.on)
            ++total[(size_t) x.source];
    for (auto& x : extras)
    {
        x.name = layerSourceNames[x.source].toUpperCase();
        if (! x.on)
            continue;
        const int n = ++count[(size_t) x.source];
        if (x.source == layerNoise || x.source == layerClick || x.source == layerBody) // numbered after the core layer
            x.name << " " << (n + 1);
        else if (total[(size_t) x.source] > 1)
            x.name << " " << n;
    }
}

int EnvelopeCanvas::resolveEnvelope (int which, int onTab) const
{
    const bool pitch = onTab == pitchTab;
    switch (which)
    {
        case bodyLayer:  return pitch ? envPitch : envBody;
        case noiseLayer: return pitch ? envNoisePitch : envNoise;
        case clickLayer: return pitch ? envClickPitch : envClick;
        default: break;
    }
    const int k = which - firstExtraLayer;
    if (! juce::isPositiveAndBelow (k, kNumExtraLayers))
        return -1;
    const auto& x = extras[(size_t) k];
    return pitch ? layerPitchEnvelope (k, x.pitchLink) : layerLevelEnvelope (k, x.levelLink);
}

juce::String EnvelopeCanvas::getLayerName (int which) const
{
    static const char* core[] { "BODY", "NOISE", "CLICK" };
    if (juce::isPositiveAndBelow (which, firstExtraLayer))
        return core[which];
    const int k = which - firstExtraLayer;
    return juce::isPositiveAndBelow (k, kNumExtraLayers) ? extras[(size_t) k].name : juce::String();
}

juce::String EnvelopeCanvas::getCurveName() const
{
    if (! hasCurve)
        return {};
    if (layerOfEnvelope (active) >= 0)
        return titleCase (getLayerName (layer)) + (isPitchEnvelope (active) ? " pitch" : " level");
    return curveName (active);
}

void EnvelopeCanvas::setActive (int env, bool curve)
{
    const auto id = (EnvId) juce::jlimit (0, numEnvs - 1, env);
    if (id == active && curve == hasCurve)
        return;
    endGestures();
    active = id;
    hasCurve = curve;
    hover = drag = {};
    updateFocusBand();
    setPanClamped (pitchPan);
    repaint();
}

void EnvelopeCanvas::showLayer (int newLayer, int newTab)
{
    layer = juce::jlimit (0, numLayers - 1, newLayer);
    tab = juce::jlimit (0, 1, newTab);
    refreshParams();
    refreshActiveCurve();
    repaint();
}

void EnvelopeCanvas::setShowAll (bool shouldShowAll)
{
    if (showAll != shouldShowAll)
    {
        showAll = shouldShowAll;
        repaint();
    }
}

std::vector<EnvelopeCanvas::Ghost> EnvelopeCanvas::ghostCurves (const DrumParams& p) const
{
    // Body, Noise and Click always (switched off or not: they're still part of the patch),
    // extra layers once added; through resolveEnvelope (a coupled extra layer's curve is
    // its core layer's, a Flat one has none), each curve once, not the one being edited.
    std::vector<Ghost> out;
    if (! showAll)
        return out;
    for (int l = 0; l < numLayers; ++l)
    {
        if (l == layer)
            continue;
        const bool exists = l < firstExtraLayer || extras[(size_t) (l - firstExtraLayer)].on;
        const bool playing = l == bodyLayer ? p.bodyOn : l == noiseLayer ? p.noiseOn : l == clickLayer ? p.clickOn : exists;
        const int env = exists ? resolveEnvelope (l, tab) : -1;
        if (env < 0 || (hasCurve && env == (int) active))
            continue;
        if (std::find_if (out.begin(), out.end(), [env] (const Ghost& g) { return g.env == env; }) == out.end())
            out.push_back ({ l, env, playing });
    }
    return out;
}

float EnvelopeCanvas::ghostY (int envIndex, float x) const
{
    const float t = juce::jlimit (0.0f, 1.0f, xToTime (x));
    return valueToY (envIndex, envs[(size_t) envIndex].evaluate (t));
}

void EnvelopeCanvas::drawGhosts (juce::Graphics& g, juce::Rectangle<float> plot, const DrumParams& p) const
{
    g.setFont (uiFont (9.5f, true));
    for (const auto& ghost : ghostCurves (p))
    {
        const int l = ghost.layer, env = ghost.env;
        const auto colour = colours::forEnvelope (env).withMultipliedAlpha (ghost.playing ? 1.0f : 0.55f);
        juce::Path line;
        const float x0 = juce::jmax (plot.getX(), timeToX (0.0f)), x1 = juce::jmin (plot.getRight(), timeToX (1.0f));
        line.startNewSubPath (x0, ghostY (env, x0));
        for (float x = x0 + 2.0f; x <= x1; x += 2.0f)
            line.lineTo (x, ghostY (env, x));
        g.setColour (colour.withMultipliedAlpha (0.5f));
        if (ghost.playing)
            g.strokePath (line, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        else
        {
            juce::Path dashed;
            const float dashes[] { 5.0f, 4.0f };
            juce::PathStrokeType (1.3f).createDashedStroke (dashed, line, dashes, 2);
            g.fillPath (dashed);
        }

        // Its name, a third of the way along (clear of the ruler and the edited curve's labels).
        const float xl = x0 + 0.33f * (x1 - x0);
        const float y = juce::jlimit (plot.getY() + 14.0f, plot.getBottom() - 2.0f, ghostY (env, xl));
        g.setColour (colour.withMultipliedAlpha (0.8f));
        g.drawText (getLayerName (l) + (ghost.playing ? "" : " (OFF)"), juce::Rectangle<float> (xl + 4.0f, y - 14.0f, 110.0f, 12.0f),
                    juce::Justification::centredLeft);
    }
}

int EnvelopeCanvas::ghostAt (juce::Point<float> p) const
{
    int best = -1;
    float bestDist = 7.0f;
    for (const auto& ghost : ghostCurves (processor.readParams()))
    {
        const float d = std::abs (ghostY (ghost.env, p.x) - p.y);
        if (d < bestDist)
        {
            bestDist = d;
            best = ghost.layer;
        }
    }
    return best;
}

void EnvelopeCanvas::refreshActiveCurve()
{
    // With no curve (Flat), stay on the layer's own one, which keeps the axis and colour right.
    const int env = resolveEnvelope (layer, tab);
    if (env >= 0)
        setActive (env, true);
    else if (const int k = layer - firstExtraLayer; juce::isPositiveAndBelow (k, kNumExtraLayers))
        setActive (tab == pitchTab ? layerOwnPitchEnvelope (k) : layerOwnLevelEnvelope (k), false);
}

void EnvelopeCanvas::setWaveform (const juce::AudioBuffer<float>& hit, double sampleRate, float newLengthMs)
{
    // The lowest and highest sample of either channel: exactly what's played (what a
    // recording of it shows), top and bottom, offsets and all.
    const int n = hit.getNumChannels() > 0 ? hit.getNumSamples() : 0;
    waveLo.resize ((size_t) n);
    waveHi.resize ((size_t) n);
    float peak = 0.0f;
    if (n > 0)
    {
        const float* l = hit.getReadPointer (0);
        const float* r = hit.getReadPointer (hit.getNumChannels() > 1 ? 1 : 0);
        for (int i = 0; i < n; ++i)
        {
            waveLo[(size_t) i] = juce::jmin (l[i], r[i]);
            waveHi[(size_t) i] = juce::jmax (l[i], r[i]);
            peak = juce::jmax (peak, -waveLo[(size_t) i], waveHi[(size_t) i]);
        }
    }

    const auto samplesPerUnit = (double) newLengthMs * 0.001 * sampleRate;
    waveSamplesPerUnit = std::isfinite (samplesPerUnit) && samplesPerUnit >= 1.0 ? (float) samplesPerUnit
                                                                                  : (float) juce::jmax (1, n);

    // The view can reach as far as the audio is visible: 60 dB down is under a pixel.
    int end = n;
    while (end > 0 && ! (juce::jmax (-waveLo[(size_t) end - 1], waveHi[(size_t) end - 1]) > peak * 0.001f))
        --end;

    const float oldLimit = timeLimit;
    const float limit = (float) end / waveSamplesPerUnit;
    timeLimit = std::isfinite (limit) ? juce::jmax (1.0f, limit) : 1.0f;

    // Zoomed out to the whole tail? Then keep showing all of it as it changes.
    if (oldLimit > 1.0f && viewStart <= timeMin && viewEnd >= oldLimit)
        viewEnd = timeLimit;
    clampView();

    repaint();
}

void EnvelopeCanvas::setGateMarker (float newGateMs, float newFadeMs)
{
    if (! std::isfinite (newGateMs) || ! std::isfinite (newFadeMs))
        newGateMs = newFadeMs = -1.0f;
    newFadeMs = juce::jmax (0.0f, newFadeMs);

    if (! juce::approximatelyEqual (newGateMs, gateMs) || ! juce::approximatelyEqual (newFadeMs, gateFadeMs))
    {
        gateMs = newGateMs;
        gateFadeMs = newFadeMs;
        if (! isGateShown() && (hover.kind == Hit::gate || hover.kind == Hit::gateFade))
            hover = {};
        repaint();
    }
}

float EnvelopeCanvas::getGateMarkerX() const noexcept
{
    return isGateShown() ? msToX (gateMs) : -1.0f;
}

void EnvelopeCanvas::setPlayhead (float pos)
{
    if (std::abs (pos - playhead) > 1.0e-4f)
    {
        playhead = pos;
        repaint();
    }
}

void EnvelopeCanvas::fitView()
{
    // Length first (from the earliest layer start): it's where the envelopes are, and a
    // long tail would squash them.
    const bool fitsLength = viewStart <= timeMin + 1.0e-5f && juce::approximatelyEqual (viewEnd, 1.0f);
    viewStart = timeMin;
    viewEnd = fitsLength ? timeLimit : 1.0f;
    repaint();
}

void EnvelopeCanvas::setNoteOffset (int semitones)
{
    if (noteOffset != semitones)
    {
        noteOffset = semitones;
        updateFocusBand();
        repaint();
    }
}

void EnvelopeCanvas::clampView()
{
    const float span = juce::jlimit (kMinViewSpan, timeLimit - timeMin, viewEnd - viewStart);
    viewStart = juce::jlimit (timeMin, timeLimit - span, viewStart);
    viewEnd = viewStart + span;
}

//==============================================================================
void EnvelopeCanvas::setPitchFocus (float focus)
{
    focus = std::isfinite (focus) ? juce::jlimit (0.0f, 1.0f, focus) : 0.0f;
    if (juce::approximatelyEqual (focus, pitchFocus))
        return;
    pitchFocus = focus;
    setPanClamped (pitchPan);
    repaint();
}

void EnvelopeCanvas::setPitchPan (float units)
{
    setPanClamped (std::isfinite (units) ? units : 0.0f);
    repaint();
}

void EnvelopeCanvas::setPanClamped (float units)
{
    // Where the view would be with no pan, and how far it may move within 0 Hz - 16 kHz.
    const float lo = kFullLo + (focusBand.getStart() - kFullLo) * pitchFocus;
    const float hi = kFullHi + (focusBand.getEnd() - kFullHi) * pitchFocus;
    const float span = hi - lo;
    const float newLo = juce::jmax (kFullLo, juce::jmin (kFullHi - span, lo + units));
    pitchPan = newLo - lo;
}

juce::Range<float> EnvelopeCanvas::pitchView() const noexcept
{
    // Focus slides each edge from the full range to the band around the curves.
    float lo = kFullLo + (focusBand.getStart() - kFullLo) * pitchFocus;
    const float hi = kFullHi + (focusBand.getEnd() - kFullHi) * pitchFocus;
    const float span = juce::jmax (0.05f, hi - lo);
    lo = juce::jmax (kFullLo, juce::jmin (kFullHi - span, lo + pitchPan));
    return { lo, lo + span };
}

juce::Range<float> EnvelopeCanvas::getPitchViewHz() const
{
    const auto v = pitchView();
    return { wToHz (v.getStart()), wToHz (v.getEnd()) };
}

void EnvelopeCanvas::updateFocusBand()
{
    // The absolute range the pitch curve on show covers (its base included: it ends
    // on it), padded, and never narrower than about an octave.
    float lo = kFullHi, hi = kFullLo;
    const int env = tab == pitchTab && hasCurve ? (int) active
                  : resolveEnvelope (layer, pitchTab) >= 0 ? resolveEnvelope (layer, pitchTab) : -1;
    if (env >= 0)
    {
        for (const auto& p : envs[(size_t) env].getPoints())
        {
            const float w = hzToW (semisToHz (env, p.y));
            lo = juce::jmin (lo, w);
            hi = juce::jmax (hi, w);
        }
    }
    else
    {
        lo = hi = hzToW (getBaseHz (tab == pitchTab ? (int) active : (int) envPitch));
    }

    const float a = juce::jlimit (kFullLo, kFullHi, lo);
    const float b = juce::jlimit (kFullLo, kFullHi, hi);
    float span = juce::jmax (kMinBandUnits, b - a);
    span = juce::jmin (kFullHi - kFullLo, span * 1.3f + 0.3f);
    const float start = juce::jmax (kFullLo, juce::jmin (kFullHi - span, 0.5f * (a + b) - 0.5f * span));
    focusBand = { start, start + span };
}

float EnvelopeCanvas::getBaseHz (int env) const noexcept
{
    if (env == envClickPitch)
        return clickToneHz;
    if (env == envNoisePitch)
        return std::sqrt (noiseLowHz * noiseHighHz);
    const float keyedTune = tuneHz * std::exp2 ((float) noteOffset / 12.0f);
    if (const int k = layerOfEnvelope (env); k >= 0)
    {
        // An extra layer's own pitch: its band (Noise), its Tone (Click), or Tune (Sample, Sine, Body),
        // the last two times its Tune.
        const auto& x = extras[(size_t) k];
        if (x.source == layerNoise)
            return std::sqrt (x.lowHz * x.highHz);
        return (x.source == layerClick ? x.toneHz : keyedTune) * std::exp2 (x.tuneSt / 12.0f);
    }
    return keyedTune;
}

EnvelopeCanvas::BaseParams EnvelopeCanvas::baseParamsFor (int env) const
{
    BaseParams b;
    if (env == envPitch)
    {
        b.ids = { pid::tune };
        b.ratio = std::exp2 ((float) noteOffset / 12.0f); // Tune plays times KEY's transposition
    }
    else if (env == envClickPitch)
    {
        b.ids = { pid::clickTone };
    }
    else if (env == envNoisePitch)
    {
        b.kind = BaseParams::band;
        b.ids = { pid::noiseHP, pid::noiseLP };
    }
    else if (const int k = layerOfEnvelope (env); k >= 0)
    {
        const auto& x = extras[(size_t) k];
        if (x.source == layerNoise)
        {
            b.kind = BaseParams::band;
            b.ids = { layerParamId (k, pid::layHP), layerParamId (k, pid::layLP) };
        }
        else if (x.source == layerClick)
        {
            b.ids = { layerParamId (k, pid::layTone) };
            b.ratio = std::exp2 (x.tuneSt / 12.0f);
        }
        else
        {
            b.kind = BaseParams::semitones;
            b.ids = { layerParamId (k, pid::layTune) };
            b.ratio = tuneHz * std::exp2 ((float) noteOffset / 12.0f);
        }
    }
    return b;
}

float EnvelopeCanvas::semisToHz (int env, float semis) const noexcept
{
    return getBaseHz (env) * std::exp2 (semis / 12.0f);
}

float EnvelopeCanvas::hzToSemis (int env, float hz) const noexcept
{
    // 0 Hz lands on the bottom of the curve's range (a small fraction of a Hz).
    const float st = 12.0f * std::log2 (juce::jmax (1.0e-6f, hz) / getBaseHz (env));
    return juce::jlimit (-kPitchRangeSt, kPitchRangeSt, st);
}

float EnvelopeCanvas::pitchHzToY (float hz) const noexcept
{
    const auto plot = plotArea();
    const auto view = pitchView();
    const float frac = (hzToW (hz) - view.getStart()) / view.getLength();
    return plot.getBottom() - frac * plot.getHeight();
}

float EnvelopeCanvas::pitchYToHz (float y) const noexcept
{
    const auto plot = plotArea();
    const auto view = pitchView();
    return wToHz (view.getStart() + (plot.getBottom() - y) / plot.getHeight() * view.getLength());
}

//==============================================================================
juce::Rectangle<float> EnvelopeCanvas::plotArea() const
{
    auto r = getLocalBounds().toFloat();
    r.removeFromLeft (kRulerWidth);
    r.removeFromBottom (kTimeAxisHeight + laneStripHeight() + kLaneGap);
    r.removeFromTop (kPadTop);
    r.removeFromRight (kPadRight);
    return r;
}

juce::Rectangle<float> EnvelopeCanvas::laneStrip() const
{
    const auto plot = plotArea();
    return { plot.getX(), plot.getBottom() + kLaneGap, plot.getWidth(), laneStripHeight() };
}

float EnvelopeCanvas::laneStripHeight() const noexcept
{
    const auto n = (float) juce::jmax ((size_t) 1, lanes.size());
    return juce::jmax (kLaneStripHeight, n * kMinLaneHeight + (n - 1.0f) * kLaneSpacing + 4.0f);
}

juce::Rectangle<float> EnvelopeCanvas::getLaneBounds (int lane) const
{
    const auto strip = laneStrip();
    const int n = juce::jmax (1, (int) lanes.size());
    const float h = juce::jmin (kMaxLaneHeight, (strip.getHeight() - 4.0f - kLaneSpacing * (float) (n - 1)) / (float) n);
    const float total = h * (float) n + kLaneSpacing * (float) (n - 1);
    const float y0 = strip.getY() + 0.5f * (strip.getHeight() - total);
    return { strip.getX(), y0 + (float) lane * (h + kLaneSpacing), strip.getWidth(), h };
}

juce::String EnvelopeCanvas::getLaneParamId (int lane) const
{
    return juce::isPositiveAndBelow (lane, (int) lanes.size()) ? lanes[(size_t) lane].paramId : juce::String();
}

juce::String EnvelopeCanvas::getLaneName (int lane) const
{
    return juce::isPositiveAndBelow (lane, (int) lanes.size()) ? lanes[(size_t) lane].name : juce::String();
}

float EnvelopeCanvas::timeToX (float t) const noexcept
{
    const auto plot = plotArea();
    return plot.getX() + (t - viewStart) / (viewEnd - viewStart) * plot.getWidth();
}

float EnvelopeCanvas::xToTime (float x) const noexcept
{
    const auto plot = plotArea();
    return viewStart + (x - plot.getX()) / plot.getWidth() * (viewEnd - viewStart);
}

float EnvelopeCanvas::msToX (float ms) const noexcept
{
    return timeToX (ms / lengthMs);
}

float EnvelopeCanvas::valueToY (int env, float v) const noexcept
{
    if (isPitchEnvelope (env))
        return pitchHzToY (semisToHz (env, v));

    const auto plot = plotArea();
    return plot.getBottom() - ampToFrac (v) * plot.getHeight();
}

float EnvelopeCanvas::yToValue (int env, float y) const noexcept
{
    if (isPitchEnvelope (env))
        return hzToSemis (env, pitchYToHz (y));

    const auto plot = plotArea();
    return fracToAmp ((plot.getBottom() - y) / plot.getHeight());
}

juce::Point<float> EnvelopeCanvas::pointPositionOf (int env, int index) const
{
    const auto& p = envs[(size_t) env].getPoints()[(size_t) index];
    return { timeToX (p.x), valueToY (env, p.y) };
}

juce::Point<float> EnvelopeCanvas::handlePosition (int index) const
{
    const auto& env = envs[(size_t) active];
    const auto& pts = env.getPoints();
    const float t = 0.5f * (pts[(size_t) index].x + pts[(size_t) index + 1].x);
    return { timeToX (t), valueToY (active, env.segmentMidY (index)) };
}

//==============================================================================
void EnvelopeCanvas::updateLanes()
{
    refreshParams();
    auto& state = processor.getState();
    const auto raw = [&state] (const juce::String& id)
    {
        const auto* v = state.getRawParameterValue (id);
        return v != nullptr ? v->load() : 0.0f;
    };

    bool anySoloed = raw (pid::bodySolo) >= 0.5f || raw (pid::noiseSolo) >= 0.5f || raw (pid::clickSolo) >= 0.5f;
    for (int k = 0; k < kNumExtraLayers; ++k)
        anySoloed = anySoloed || raw (layerParamId (k, pid::laySolo)) >= 0.5f;

    std::vector<Lane> next;
    const auto add = [&] (const juce::String& name, const juce::String& onId, const juce::String& soloId,
                          const juce::String& offsetId, juce::Colour colour)
    {
        const float offset = raw (offsetId);
        const bool silent = raw (onId) < 0.5f || (anySoloed && raw (soloId) < 0.5f);
        next.push_back ({ name, offsetId, colour, std::isfinite (offset) ? offset : 0.0f, silent });
    };

    add ("BODY", pid::bodyOn, pid::bodySolo, pid::bodyOffset, colours::body);
    add ("NOISE", pid::noiseOn, pid::noiseSolo, pid::noiseOffset, colours::noise);
    add ("CLICK", pid::clickOn, pid::clickSolo, pid::clickOffset, colours::click);

    // Extra layers that are on, named as their tabs are ("NOISE 2", "BODY 2", "SINE").
    for (int k = 0; k < kNumExtraLayers; ++k)
        if (extras[(size_t) k].on)
            add (extras[(size_t) k].name, layerParamId (k, pid::layOn), layerParamId (k, pid::laySolo),
                 layerParamId (k, pid::layDelay), colours::layer);

    if (next != lanes)
    {
        if (drag.kind == Hit::lane && next.size() != lanes.size())
            drag = {}; // the lane under the mouse went away
        lanes = std::move (next);
        repaint();
    }

    // The view may reach back to the earliest layer start, so its cut-off part shows.
    float earliest = 0.0f;
    for (const auto& lane : lanes)
        earliest = juce::jmin (earliest, lane.offsetMs);
    const float newMin = earliest < 0.0f ? (earliest - 3.0f) / lengthMs : 0.0f;
    if (drag.kind != Hit::lane && ! juce::approximatelyEqual (newMin, timeMin))
    {
        const bool fitted = viewStart <= timeMin + 1.0e-5f; // showing everything from the start
        timeMin = newMin;
        if (fitted)
            viewStart = timeMin;
        clampView();
        repaint();
    }
}

void EnvelopeCanvas::setLaneOffset (int lane, float ms)
{
    if (! juce::isPositiveAndBelow (lane, (int) lanes.size()))
        return;

    auto* param = processor.getState().getParameter (lanes[(size_t) lane].paramId);
    if (param == nullptr)
        return;

    ms = param->getNormalisableRange().getRange().clipValue (ms);
    lanes[(size_t) lane].offsetMs = ms;
    param->setValueNotifyingHost (param->convertTo0to1 (ms));
    repaint();
    if (onEdited)
        onEdited();
}

void EnvelopeCanvas::setGateParam (bool fade, float ms)
{
    auto* param = processor.getState().getParameter (fade ? pid::revGateFade : pid::revGateTime);
    if (param == nullptr)
        return;

    ms = param->getNormalisableRange().getRange().clipValue (ms);
    param->setValueNotifyingHost (param->convertTo0to1 (ms));
    // Draw it where it is now, rather than when the editor's timer next reads it.
    const float now = param->convertFrom0to1 (param->getValue());
    (fade ? gateFadeMs : gateMs) = now;
    repaint();
    if (onEdited)
        onEdited();
}

//==============================================================================
int EnvelopeCanvas::findLane (juce::Point<float> p) const
{
    const auto strip = laneStrip();
    if (p.x < strip.getX() - 4.0f || p.x > strip.getRight() + 4.0f)
        return -1;

    for (int i = 0; i < (int) lanes.size(); ++i)
        if (getLaneBounds (i).expanded (0.0f, kLaneSpacing * 0.5f).contains (p.withX (strip.getCentreX())))
            return i;
    return -1;
}

EnvelopeCanvas::Hit EnvelopeCanvas::findHit (juce::Point<float> p) const
{
    if (const int lane = findLane (p); lane >= 0)
        return { Hit::lane, lane };

    const auto plot = plotArea();
    if (tab == pitchTab && p.x < plot.getX() - 6.0f && p.y >= plot.getY() - 4.0f && p.y <= plot.getBottom() + 4.0f)
        return { Hit::ruler, 0 };

    // The curve's points, then its bend handles.
    if (hasCurve)
    {
        const auto& env = envs[(size_t) active];
        const auto& pts = env.getPoints();

        Hit best;
        float bestDist = kPointHitRadius;
        for (int i = 0; i < (int) pts.size(); ++i)
        {
            const float d = p.getDistanceFrom (pointPosition (i));
            if (d < bestDist)
            {
                bestDist = d;
                best = { Hit::point, i };
            }
        }
        if (best.kind != Hit::none)
            return best;

        bestDist = kHandleHitRadius;
        for (int i = 0; i + 1 < (int) pts.size(); ++i)
        {
            if (std::abs (pts[(size_t) i + 1].y - pts[(size_t) i].y) < 0.002f)
                continue;

            const float d = p.getDistanceFrom (handlePosition (i));
            if (d < bestDist)
            {
                bestDist = d;
                best = { Hit::handle, i };
            }
        }
        if (best.kind != Hit::none)
            return best;
    }

    // The gate marker: its line (Gate Time), or the end of its fade (Gate Fade).
    if (isGateShown() && p.y >= plot.getY() && p.y <= plot.getBottom())
    {
        const float x = msToX (gateMs), fadeX = msToX (gateMs + gateFadeMs);
        const float dGate = std::abs (p.x - x), dFade = std::abs (p.x - fadeX);
        const bool fadeApart = fadeX - x >= 2.0f * kGateHitDistance;
        if (fadeApart && dFade <= kGateHitDistance && dFade < dGate && fadeX <= plot.getRight())
            return { Hit::gateFade, 0 };
        if (dGate <= kGateHitDistance && x >= plot.getX() - 1.0f && x <= plot.getRight() + 1.0f)
            return { Hit::gate, 0 };
    }

    return {};
}

void EnvelopeCanvas::setHover (Hit h)
{
    if (h == hover)
        return;

    hover = h;
    setMouseCursor (h.kind == Hit::point                ? juce::MouseCursor::PointingHandCursor
                  : h.kind == Hit::handle ? juce::MouseCursor::UpDownResizeCursor
                  : h.kind == Hit::lane || h.kind == Hit::gate || h.kind == Hit::gateFade
                                          ? juce::MouseCursor::LeftRightResizeCursor
                  : h.kind == Hit::ruler  ? juce::MouseCursor::UpDownResizeCursor
                                          : juce::MouseCursor::NormalCursor);
    repaint();
}

void EnvelopeCanvas::commit()
{
    processor.setEnvelope (active, envs[(size_t) active]);
    seenEnvelopeEdits = processor.getEnvelopeEditCount();
    repaint();
    if (onEdited)
        onEdited();
}

//==============================================================================
void EnvelopeCanvas::mouseMove (const juce::MouseEvent& e)
{
    setHover (findHit (e.position));
}

void EnvelopeCanvas::mouseExit (const juce::MouseEvent&)
{
    setHover ({});
}

void EnvelopeCanvas::mouseDown (const juce::MouseEvent& e)
{
    refreshParams();
    const auto h = findHit (e.position);

    if (h.kind == Hit::lane)
    {
        if (e.mods.isPopupMenu())
        {
            // Right-click puts the layer back on the hit.
            if (auto* param = processor.getState().getParameter (lanes[(size_t) h.index].paramId))
            {
                param->beginChangeGesture();
                setLaneOffset (h.index, 0.0f);
                param->endChangeGesture();
            }
            return;
        }

        drag = h;
        dragStartX = e.position.x;
        dragStartValue = lanes[(size_t) h.index].offsetMs;
        if (auto* param = processor.getState().getParameter (lanes[(size_t) h.index].paramId))
        {
            param->beginChangeGesture();
            gestureParams.push_back (param);
        }
        repaint();
        return;
    }

    if (h.kind == Hit::ruler)
    {
        drag = h;
        dragStartY = e.position.y;
        dragStartValue = pitchPan;
        return;
    }

    if (h.kind == Hit::gate || h.kind == Hit::gateFade)
    {
        if (e.mods.isPopupMenu())
            return;
        const bool fade = h.kind == Hit::gateFade;
        drag = h;
        dragStartX = e.position.x;
        dragStartValue = fade ? gateFadeMs : gateMs;
        if (auto* param = processor.getState().getParameter (fade ? pid::revGateFade : pid::revGateTime))
        {
            param->beginChangeGesture();
            gestureParams.push_back (param);
        }
        repaint();
        return;
    }

    // ALL: a click on another layer's dimmed curve (not on the edited curve) picks that layer.
    if (h.kind == Hit::none && showAll && ! e.mods.isPopupMenu())
        if (const int picked = ghostAt (e.position); picked >= 0)
        {
            showLayer (picked, tab);
            if (onLayerPicked)
                onLayerPicked (picked);
            return;
        }

    if (! hasCurve)
        return;

    auto& env = envs[(size_t) active];

    if (e.mods.isPopupMenu())
    {
        if (h.kind == Hit::point && env.removePoint (h.index))
        {
            setHover ({});
            commit();
        }
        else if (h.kind == Hit::handle)
        {
            env.setCurve (h.index, 0.0f);
            commit();
        }
        return;
    }

    drag = h;
    dragStartY = e.position.y;
    if (h.kind == Hit::handle)
    {
        dragStartCurve = env.getPoints()[(size_t) h.index].curve;
    }
    else if (h.kind == Hit::point)
    {
        dragStartValue = env.getPoints()[(size_t) h.index].y;
        dragStartHz = semisToHz (active, dragStartValue);

        // A pitch curve's end point is its base: dragging it moves Tune, Click Tone, the
        // noise band (both of its cuts), or an extra layer's Tune, Tone or band.
        if (isPitchEnvelope (active) && h.index == env.size() - 1)
        {
            auto& state = processor.getState();
            dragBase = baseParamsFor (active);
            std::vector<juce::RangedAudioParameter*> params;
            for (const auto& id : dragBase.ids)
                params.push_back (state.getParameter (id));
            if (std::find (params.begin(), params.end(), nullptr) == params.end() && ! params.empty())
            {
                const auto plain = [] (juce::RangedAudioParameter* param) { return param->convertFrom0to1 (param->getValue()); };
                dragStartLow = plain (params.front());
                dragStartHigh = plain (params.back());
                for (auto* param : params)
                {
                    param->beginChangeGesture();
                    gestureParams.push_back (param);
                }
            }
        }
    }
}

void EnvelopeCanvas::moveBase (float targetHz)
{
    // The end point plays the curve's base: set the base from where it's dragged, and
    // move every other point back by the same interval, so they keep their absolute pitch.
    const float old = getBaseHz (active);
    const auto setPlain = [] (juce::RangedAudioParameter* param, float value)
    {
        param->setValueNotifyingHost (param->convertTo0to1 (value));
        return param->convertFrom0to1 (param->getValue());
    };

    if (dragBase.kind == BaseParams::hz && gestureParams.size() == 1)
    {
        // Tune plays times KEY's transposition, a Click layer's Tone times its Tune.
        setPlain (gestureParams.front(), targetHz / juce::jmax (1.0e-6f, dragBase.ratio));
    }
    else if (dragBase.kind == BaseParams::semitones && gestureParams.size() == 1)
    {
        // An extra layer's Tune, in semitones from the (keyed) global Tune.
        setPlain (gestureParams.front(), 12.0f * std::log2 (juce::jmax (1.0e-3f, targetHz) / juce::jmax (1.0e-3f, dragBase.ratio)));
    }
    else if (dragBase.kind == BaseParams::band && gestureParams.size() == 2)
    {
        // A band: both cuts by the same ratio, from where they were at the start.
        const float ratio = targetHz / juce::jmax (1.0e-3f, std::sqrt (dragStartLow * dragStartHigh));
        setPlain (gestureParams[0], dragStartLow * ratio);
        setPlain (gestureParams[1], dragStartHigh * ratio);
    }

    refreshParams();
    const float now = getBaseHz (active);
    if (! (now > 0.0f) || juce::approximatelyEqual (now, old))
        return;

    envs[(size_t) active].offsetAll (-12.0f * std::log2 (now / old));
    commit();
}

void EnvelopeCanvas::mouseDrag (const juce::MouseEvent& e)
{
    auto& env = envs[(size_t) active];
    const auto plot = plotArea();
    const bool fine = e.mods.isShiftDown();

    if (drag.kind == Hit::lane || drag.kind == Hit::gate || drag.kind == Hit::gateFade)
    {
        // Relative, so it can go past the edge of the view.
        const float pxPerMs = plot.getWidth() / ((viewEnd - viewStart) * lengthMs);
        float ms = dragStartValue + (e.position.x - dragStartX) / pxPerMs * (fine ? 0.2f : 1.0f);
        if (drag.kind == Hit::lane)
        {
            // Snaps to 0 on the way past.
            if (! fine && std::abs (ms) * pxPerMs < kOffsetSnapPixels)
                ms = 0.0f;
            setLaneOffset (drag.index, ms);
        }
        else
        {
            setGateParam (drag.kind == Hit::gateFade, ms);
        }
        return;
    }

    if (drag.kind == Hit::ruler)
    {
        // Grab the axis: dragging down brings higher pitches into view.
        setPanClamped (dragStartValue + (e.position.y - dragStartY) / plot.getHeight() * pitchView().getLength());
        repaint();
        if (onPitchViewChanged)
            onPitchViewChanged();
        return;
    }

    if (! hasCurve)
        return;

    if (drag.kind == Hit::point)
    {
        const float t = xToTime (e.position.x);

        if (isPitchEnvelope (active))
        {
            // Absolute pitch, anywhere from 0 Hz to 16 kHz.
            float hz = pitchYToHz (e.position.y);
            if (fine)
                hz = wToHz (hzToW (dragStartHz) + (hzToW (pitchYToHz (e.position.y)) - hzToW (pitchYToHz (dragStartY))) * 0.15f);
            hz = juce::jlimit (0.0f, kMaxHz, hz);

            if (! gestureParams.empty())
            {
                moveBase (hz);
                return;
            }
            env.movePoint (drag.index, t, hzToSemis (active, hz));
        }
        else
        {
            float v = yToValue (active, e.position.y);
            if (fine) // fine vertical adjustment
                v = dragStartValue + (yToValue (active, e.position.y) - yToValue (active, dragStartY)) * 0.15f;
            env.movePoint (drag.index, t, v);
        }
        commit();
    }
    else if (drag.kind == Hit::handle)
    {
        const auto& pts = env.getPoints();
        const bool falling = pts[(size_t) drag.index + 1].y < pts[(size_t) drag.index].y;
        const float dy = (dragStartY - e.position.y) / plot.getHeight();
        env.setCurve (drag.index, dragStartCurve + dy * 2.5f * (falling ? 1.0f : -1.0f));
        commit();
    }
}

void EnvelopeCanvas::mouseUp (const juce::MouseEvent& e)
{
    endGestures();

    const bool wasLane = drag.kind == Hit::lane;
    drag = {};

    if (wasLane)
    {
        // The view may now reach further back (or less far). If a layer starts before
        // the view does, bring its start into view.
        updateLanes();
        for (const auto& lane : lanes)
            if (lane.offsetMs / lengthMs < viewStart)
                viewStart = timeMin;
        clampView();
    }

    updateFocusBand();
    setPanClamped (pitchPan);
    setHover (findHit (e.position));
    repaint();
}

void EnvelopeCanvas::mouseDoubleClick (const juce::MouseEvent& e)
{
    auto& env = envs[(size_t) active];
    const auto h = findHit (e.position);

    if (h.kind == Hit::lane)
    {
        if (auto* param = processor.getState().getParameter (lanes[(size_t) h.index].paramId))
        {
            param->beginChangeGesture();
            setLaneOffset (h.index, 0.0f);
            param->endChangeGesture();
        }
        return;
    }

    if (h.kind == Hit::gate || h.kind == Hit::gateFade)
    {
        const bool fade = h.kind == Hit::gateFade;
        if (auto* param = processor.getState().getParameter (fade ? pid::revGateFade : pid::revGateTime))
        {
            param->beginChangeGesture();
            setGateParam (fade, param->convertFrom0to1 (param->getDefaultValue()));
            param->endChangeGesture();
        }
        return;
    }

    if (h.kind == Hit::ruler)
    {
        // Back to the whole range.
        setPitchFocus (0.0f);
        setPitchPan (0.0f);
        if (onPitchViewChanged)
            onPitchViewChanged();
        return;
    }

    if (! hasCurve)
        return;

    if (h.kind == Hit::point)
    {
        if (env.removePoint (h.index))
        {
            hover = {};
            commit();
        }
    }
    else if (h.kind == Hit::handle)
    {
        env.setCurve (h.index, 0.0f);
        commit();
    }
    else if (plotArea().expanded (4.0f).contains (e.position)
             && e.position.x >= timeToX (0.0f) - 4.0f && e.position.x <= timeToX (1.0f) + 4.0f)
    {
        float v = yToValue (active, e.position.y);
        if (isPitchEnvelope (active))
            v = hzToSemis (active, juce::jlimit (0.0f, kMaxHz, pitchYToHz (e.position.y)));

        const int index = env.addPoint (xToTime (e.position.x), v);
        if (index >= 0)
        {
            hover = { Hit::point, index };
            commit();
        }
    }
}

void EnvelopeCanvas::zoomAround (float anchor, float factor)
{
    const float span = viewEnd - viewStart;
    const float newSpan = juce::jlimit (kMinViewSpan, timeLimit - timeMin, span * factor);
    viewStart = anchor - (anchor - viewStart) * (newSpan / span);
    viewEnd = viewStart + newSpan;
    clampView();
    repaint();
}

void EnvelopeCanvas::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    // Pitch axis: scroll over the ruler focuses it, alt-scroll anywhere pans it.
    if (tab == pitchTab && (e.mods.isAltDown() || findHit (e.position).kind == Hit::ruler))
    {
        const float dy = (std::abs (w.deltaY) >= std::abs (w.deltaX) ? w.deltaY : w.deltaX) * (w.isReversed ? -1.0f : 1.0f);
        if (e.mods.isAltDown())
            setPanClamped (pitchPan + dy * pitchView().getLength() * 0.5f);
        else
            setPitchFocus (pitchFocus + dy * 0.5f);
        repaint();
        if (onPitchViewChanged)
            onPitchViewChanged();
        return;
    }

    const float span = viewEnd - viewStart;
    if (std::abs (w.deltaX) > std::abs (w.deltaY))
    {
        viewStart -= w.deltaX * span * 0.6f;
        viewEnd = viewStart + span;
        clampView();
        repaint();
        return;
    }

    zoomAround (xToTime (e.position.x), std::exp2 (-w.deltaY * (w.isReversed ? -2.5f : 2.5f)));
}

void EnvelopeCanvas::mouseMagnify (const juce::MouseEvent& e, float scaleFactor)
{
    if (scaleFactor > 0.0f)
        zoomAround (xToTime (e.position.x), 1.0f / scaleFactor);
}

//==============================================================================
juce::String EnvelopeCanvas::baseName (int env) const
{
    if (env == envClickPitch) return "CLICK TONE";
    if (env == envNoisePitch) return "NOISE BAND";
    if (const int k = layerOfEnvelope (env); k >= 0)
    {
        const auto& x = extras[(size_t) k];
        return x.name + (x.source == layerNoise ? " BAND" : x.source == layerClick ? " TONE" : " TUNE");
    }
    return "TUNE";
}

juce::String EnvelopeCanvas::baseDragName (int env) const
{
    if (env == envClickPitch) return "Click Tone";
    if (env == envNoisePitch) return "the noise band";
    if (const int k = layerOfEnvelope (env); k >= 0)
    {
        const auto& x = extras[(size_t) k];
        return titleCase (x.name) + (x.source == layerNoise ? "'s band" : x.source == layerClick ? "'s Tone" : "'s Tune");
    }
    return "Tune";
}

juce::String EnvelopeCanvas::describeValue (int env, int pointIndex, float y) const
{
    if (isPitchEnvelope (env))
    {
        const float hz = semisToHz (env, y);
        auto text = formatHzLong (hz) + noteSuffix (hz) + midDot;
        if (pointIndex == envs[(size_t) env].size() - 1)
            return text + baseName (env);
        return text + (y >= 0.0f ? juce::String ("+") : minusSign) + juce::String (std::abs (y), 1) + " st";
    }

    auto text = formatGain (y);
    if (y > 1.0005f)
        text << "  into the clipper";
    else
        text << "  (" << juce::roundToInt (y * 100.0f) << "%)";
    return text;
}

void EnvelopeCanvas::paint (juce::Graphics& g)
{
    refreshParams();
    if (drag.kind == Hit::none)
    {
        updateFocusBand(); // the band follows the curves and their bases, except mid-drag
        setPanClamped (pitchPan);
    }

    const auto bounds = getLocalBounds().toFloat();
    const auto plot = plotArea();
    const auto params = processor.readParams();

    g.setColour (colours::display);
    g.fillRoundedRectangle (bounds, 6.0f);

    drawGrid (g, plot, params);

    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (plot.withX (plot.getX() - 1.0f).withRight (plot.getRight() + 1.0f)
                                .withY (bounds.getY()).withBottom (plot.getBottom() + 1.0f).toNearestInt());
        drawPastLength (g, plot);
        if (tab == levelTab)
            drawLevelWaveform (g, plot, params);
        else
            drawWaveform (g, plot);
        drawGateMarker (g, plot, params);
        if (tab == pitchTab)
            drawPitchLabels (g, plot);
    }

    if (showAll)
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (plot.toNearestInt());
        drawGhosts (g, plot, params);
    }

    if (hasCurve)
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (plot.expanded (8.0f, kPadTop).withBottom (plot.getBottom() + 8.0f).toNearestInt());
        drawEnvelope (g, plot, (int) active);
    }

    drawLanes (g, plot);

    if (playhead >= 0.0f && playhead <= 1.0f)
    {
        const float x = timeToX (playhead);
        if (x >= plot.getX() && x <= plot.getRight())
        {
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.drawVerticalLine (juce::roundToInt (x), plot.getY() - kPadTop * 0.5f, plot.getBottom());
        }
    }

    drawHud (g, plot, params);

    g.setColour (colours::panelBorder);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);
}

void EnvelopeCanvas::drawGrid (juce::Graphics& g, juce::Rectangle<float> plot, const DrumParams& p) const
{
    // --- time grid ---------------------------------------------------------------
    const auto strip = laneStrip();
    const float axisY = strip.getBottom() + 1.0f;
    const float spanMs = (viewEnd - viewStart) * p.lengthMs;
    const float targetLines = juce::jmax (2.0f, plot.getWidth() / 95.0f);
    static constexpr float steps[] { 0.5f, 1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 5000 };
    float step = steps[std::size (steps) - 1];
    for (float s : steps)
        if (spanMs / s <= targetLines) { step = s; break; }

    // At the right end of the axis, say how much tail runs past the view (by default
    // it stops at Length), and keep the time labels clear of it.
    float labelsRight = plot.getRight() + kPadRight;
    if (const float hiddenMs = (timeLimit - viewEnd) * p.lengthMs; hiddenMs >= 1.0f)
    {
        g.setFont (uiFont (10.5f, true));
        const auto text = "TAIL +" + formatMs (hiddenMs);
        const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 2.0f;
        const float ax = plot.getRight() - 10.0f, ay = axisY + (kTimeAxisHeight - 2.0f) * 0.5f;
        const auto box = juce::Rectangle<float> (ax - 6.0f - w, axisY, w, kTimeAxisHeight - 4.0f);
        g.setColour (colours::textDim);
        g.drawText (text, box, juce::Justification::centredRight);

        juce::Path arrow;
        arrow.addTriangle (ax - 3.0f, ay - 3.5f, ax - 3.0f, ay + 3.5f, ax + 1.5f, ay);
        g.fillPath (arrow);
        labelsRight = box.getX() - 8.0f;
    }

    g.setFont (uiFont (10.5f));
    const float startMs = viewStart * p.lengthMs;
    for (float ms = std::ceil (startMs / step) * step; ms <= startMs + spanMs + 0.001f; ms += step)
    {
        const float x = timeToX (ms / p.lengthMs);
        g.setColour (colours::grid);
        g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
        g.setColour (colours::grid.withAlpha (0.6f));
        g.drawVerticalLine (juce::roundToInt (x), strip.getY(), strip.getBottom());

        const auto label = formatMs (ms);
        const float labelW = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), label) + 4.0f;
        if (x + 3.0f + labelW <= labelsRight)
        {
            g.setColour (colours::textDim);
            g.drawText (label, juce::Rectangle<float> (x + 3.0f, axisY, labelW, kTimeAxisHeight - 4.0f),
                        juce::Justification::centredLeft);
        }
    }

    // --- value grid + ruler ---------------------------------------------------------
    if (tab == pitchTab)
        drawPitchGrid (g, plot);
    else
        drawAmpGrid (g, plot, p);
}

void EnvelopeCanvas::drawPitchGrid (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    const auto view = pitchView();
    const float loHz = wToHz (view.getStart()), hiHz = wToHz (view.getEnd());

    // Hz lines: the finest set of round numbers whose lines stay at least 20 px apart
    // (above 50 Hz; below that the axis turns linear, and crowded lines are skipped).
    static const std::vector<std::vector<float>> levels {
        { 1.0f, 2.0f, 5.0f },
        { 1.0f, 1.5f, 2.0f, 3.0f, 5.0f, 7.0f },
        { 1.0f, 1.2f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f },
        { 1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f, 1.7f, 1.8f, 1.9f, 2.0f, 2.2f, 2.4f, 2.6f, 2.8f, 3.0f, 3.25f,
          3.5f, 3.75f, 4.0f, 4.5f, 5.0f, 5.5f, 6.0f, 6.5f, 7.0f, 7.5f, 8.0f, 8.5f, 9.0f, 9.5f },
    };
    const auto candidates = [&] (const std::vector<float>& mantissas)
    {
        std::vector<float> out;
        for (float decade = 1.0f; decade <= 10000.0f; decade *= 10.0f)
            for (float m : mantissas)
                if (const float hz = m * decade; hz >= loHz * 0.999f && hz <= hiHz * 1.001f && hz < kMaxHz * 0.999f)
                    out.push_back (hz);
        return out;
    };
    const auto spacious = [&] (const std::vector<float>& c)
    {
        for (size_t i = 1; i < c.size(); ++i)
            if (c[i - 1] >= 50.0f && pitchHzToY (c[i - 1]) - pitchHzToY (c[i]) < 20.0f)
                return false;
        return true;
    };

    std::vector<float> lines = candidates (levels.front());
    for (int level = (int) levels.size() - 1; level > 0; --level)
    {
        auto c = candidates (levels[(size_t) level]);
        if (c.size() <= 2 || spacious (c))
        {
            lines = std::move (c);
            break;
        }
    }

    // Thin out the linear end, where round numbers bunch up.
    {
        std::vector<float> kept;
        float lastY = plot.getBottom() + 100.0f;
        if (loHz <= 0.5f)
            lastY = plot.getBottom(); // 0 Hz is the bottom edge
        for (float hz : lines)
        {
            const float y = pitchHzToY (hz);
            if (lastY - y >= 7.0f)
            {
                kept.push_back (hz);
                lastY = y;
            }
        }
        lines = std::move (kept);
    }

    const auto isDecade = [] (float hz)
    {
        return juce::approximatelyEqual (hz, 100.0f) || juce::approximatelyEqual (hz, 1000.0f)
            || juce::approximatelyEqual (hz, 10000.0f);
    };

    // Labels by importance: the ends of the full range, the decades, 20 Hz, then the
    // rest, skipping any label that would crowd one already placed.
    std::vector<std::pair<float, bool>> order; // hz, is an end of the full range
    if (loHz <= 0.5f)              order.push_back ({ 0.0f, true });
    if (hiHz >= kMaxHz * 0.99f)    order.push_back ({ kMaxHz, true });
    for (float hz : lines) if (isDecade (hz)) order.push_back ({ hz, false });
    for (float hz : lines) if (juce::approximatelyEqual (hz, 20.0f)) order.push_back ({ hz, false });
    for (float hz : lines) if (! isDecade (hz) && ! juce::approximatelyEqual (hz, 20.0f)) order.push_back ({ hz, false });

    for (float hz : lines)
    {
        g.setColour (isDecade (hz) ? colours::gridStrong : colours::grid);
        g.drawHorizontalLine (juce::roundToInt (pitchHzToY (hz)), plot.getX(), plot.getRight());
    }

    std::vector<float> labelled;
    g.setFont (uiFont (10.5f));
    for (const auto& [hz, isEnd] : order)
    {
        const float y = pitchHzToY (hz);
        bool crowded = false;
        for (float other : labelled)
            crowded = crowded || std::abs (other - y) < 14.0f;
        if (crowded)
            continue;
        labelled.push_back (y);

        const auto text = hz <= 0.0f ? juce::String ("0 Hz") : formatHz (hz);
        const bool strong = isDecade (hz) || isEnd;
        g.setColour (strong ? colours::text.withAlpha (0.75f) : colours::textDim);
        // The ends sit inside the ruler, clear of the plot's edges.
        const float ty = isEnd ? juce::jlimit (plot.getY() - 4.0f, plot.getBottom() - 10.0f, y - 7.0f) : y - 7.0f;
        g.drawText (text, juce::Rectangle<float> (2.0f, ty, kRulerWidth - 8.0f, 14.0f), juce::Justification::centredRight);
    }
}

void EnvelopeCanvas::drawPitchLabels (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    const auto view = pitchView();
    const float loHz = wToHz (view.getStart()), hiHz = wToHz (view.getEnd());

    // Note names just inside the plot, over the waveform: the Cs, then (zoomed in) the
    // notes between them, each only where it has room (the axis squeezes low notes).
    const float loMidi = juce::jmax (0.0f, frequencyToMidi (juce::jmax (8.2f, loHz)));
    const float hiMidi = frequencyToMidi (juce::jmax (8.2f, hiHz));
    const float topMidi = juce::jmax (loMidi, hiMidi - 12.0f);
    const float pxPerSemi = (pitchHzToY (440.0f * std::exp2 ((topMidi - 69.0f) / 12.0f))
                             - pitchHzToY (440.0f * std::exp2 ((hiMidi - 69.0f) / 12.0f))) / juce::jmax (0.01f, hiMidi - topMidi);
    static constexpr int semiSteps[] { 1, 2, 3, 4, 6, 12 };
    int noteStep = 12;
    for (int s : semiSteps)
        if (pxPerSemi * (float) s >= 17.0f) { noteStep = s; break; }

    const auto yOf = [this] (int m) { return pitchHzToY (440.0f * std::exp2 (((float) m - 69.0f) / 12.0f)); };
    std::vector<float> placed;
    const auto fits = [&placed] (float y)
    {
        for (float other : placed)
            if (std::abs (other - y) < 15.0f)
                return false;
        return true;
    };

    g.setFont (uiFont (9.5f, true));
    for (const bool cs : { true, false })
    {
        for (int m = (int) std::ceil (loMidi); (float) m <= hiMidi; ++m)
        {
            const bool c = m % 12 == 0;
            if (m < 0 || c != cs || (m % 12) % noteStep != 0)
                continue;
            const float y = yOf (m);
            if (y < plot.getY() + 4.0f || y > plot.getBottom() - 4.0f || ! fits (y))
                continue;
            placed.push_back (y);

            const auto name = noteName ((float) m);
            const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), name) + 6.0f;
            const auto chip = juce::Rectangle<float> (plot.getX() + 6.0f, y - 6.0f, w, 12.0f);
            g.setColour (colours::display.withAlpha (0.75f));
            g.fillRoundedRectangle (chip, 2.5f);
            g.setColour (colours::pitch.withAlpha (c ? 0.75f : 0.5f));
            g.fillRect (plot.getX(), y - 0.5f, c ? 5.0f : 3.0f, 1.0f);
            g.drawText (name, chip, juce::Justification::centred);
        }
    }

    // The curve's base (where it lands): dashed, and named.
    if (hasCurve)
    {
        const int env = (int) active;
        const float hz = getBaseHz (env);
        const float y = pitchHzToY (hz);
        if (y < plot.getY() || y > plot.getBottom())
            return;

        const auto colour = colours::forEnvelope (env);
        const float dashes[] { 5.0f, 4.0f };
        g.setColour (colour.withAlpha (0.35f));
        g.drawDashedLine ({ plot.getX(), y, plot.getRight(), y }, dashes, 2, 1.0f);

        const auto text = baseName (env) + "  " + formatHzLong (hz) + noteSuffix (hz);
        g.setFont (uiFont (10.5f, true));
        const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 4.0f;
        const bool above = y - 16.0f > plot.getY() + 18.0f;
        g.setColour (colour.withAlpha (0.85f));
        g.drawText (text, juce::Rectangle<float> (plot.getRight() - 14.0f - w, above ? y - 15.0f : y + 3.0f, w, 12.0f),
                    juce::Justification::centredRight);
    }
}

void EnvelopeCanvas::drawAmpGrid (juce::Graphics& g, juce::Rectangle<float> plot, const DrumParams& p) const
{
    // The clip zone: above 0 dB, up to +12 dB. What goes there drives the output clipper.
    const float unityY = valueToY (active, 1.0f);
    const auto zone = plot.withBottom (unityY);
    g.setGradientFill (juce::ColourGradient (colours::clipZone.withAlpha (0.13f), 0.0f, zone.getY(),
                                             colours::clipZone.withAlpha (0.04f), 0.0f, zone.getBottom(), false));
    g.fillRect (zone);

    const auto accent = colours::forEnvelope ((int) active);
    struct Mark { float gain; const char* text; };
    static constexpr Mark marks[] { { 4.0f, "+12" }, { 2.0f, "+6" }, { 1.0f, "0 dB" }, { 0.5f, "-6" }, { 0.25f, "-12" }, { 0.0f, "-inf" } };

    g.setFont (uiFont (10.5f));
    for (const auto& m : marks)
    {
        const float y = valueToY (active, m.gain);
        const bool unity = juce::approximatelyEqual (m.gain, 1.0f);
        const bool over = m.gain > 1.0f;
        if (unity)
        {
            g.setColour (colours::clipZone.withAlpha (0.7f));
            g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
        }
        else if (m.gain > 0.0f && m.gain < 4.0f)
        {
            g.setColour (over ? colours::clipZone.withAlpha (0.14f) : colours::grid);
            g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
        }

        g.setColour (unity ? colours::clipZone : over ? colours::clipZone.withAlpha (0.7f)
                                                      : (m.gain <= 0.0f ? accent.withAlpha (0.9f) : colours::textDim));
        g.setFont (uiFont (10.5f, unity));
        g.drawText (m.text, juce::Rectangle<float> (2.0f, y - 7.0f, kRulerWidth - 8.0f, 14.0f), juce::Justification::centredRight);
    }

    // Name the zone, and what happens in it.
    const int mode = juce::jlimit (0, clipModeNames.size() - 1, p.clipMode);
    const auto text = mode == 0              ? juce::String ("HEADROOM +12 dB") + midDot + "OUTPUT STAGE OFF: THE OUTPUT CAN GO OVER 0 dBFS"
                    : mode == clipModeComp ? juce::String ("HEADROOM +12 dB") + midDot + "INTO THE OUTPUT COMPRESSOR"
                                           : juce::String ("HEADROOM +12 dB") + midDot + "INTO THE " + clipModeNames[mode].toUpperCase() + " CLIPPER";
    g.setFont (uiFont (10.0f, true));
    g.setColour (colours::clipZone.withAlpha (0.75f));
    const float left = juce::jlimit (plot.getX(), plot.getCentreX(), timeToX (0.0f)) + 8.0f;
    g.drawText (text, juce::Rectangle<float> (left, zone.getY() + 3.0f, plot.getWidth() * 0.5f, 12.0f),
                juce::Justification::centredLeft, true);
}
void EnvelopeCanvas::drawWaveform (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    // Pitch tab: full scale fills the plot around its centre.
    drawWaveformAt (g, plot, plot.getCentreY(), plot.getHeight() * 0.48f);
}

void EnvelopeCanvas::drawWaveformAt (juce::Graphics& g, juce::Rectangle<float> plot, float cy, float amp) const
{
    // Each pixel column spans the lowest to the highest sample under it (of either
    // channel), so what's drawn is the played waveform as it is, never smoothed.
    if (waveHi.empty())
        return;

    const int x0 = (int) std::floor (juce::jmax (plot.getX(), timeToX (0.0f)));
    const int x1 = (int) std::ceil (plot.getRight());
    if (x1 <= x0)
        return;
    const int count = juce::jmax (1, x1 - x0);
    const auto n = (int) waveHi.size();

    std::vector<float> lo ((size_t) count), hi ((size_t) count);
    for (int c = 0; c < count; ++c)
    {
        const float s0 = xToTime ((float) (x0 + c)) * waveSamplesPerUnit;
        const float s1 = xToTime ((float) (x0 + c + 1)) * waveSamplesPerUnit;
        float mn = 0.0f, mx = 0.0f;

        if (s1 - s0 < 1.0f)
        {
            const int i = (int) std::floor (s0);
            if (i >= 0 && i + 1 < n)
            {
                const float frac = s0 - (float) i;
                mn = waveLo[(size_t) i] + frac * (waveLo[(size_t) i + 1] - waveLo[(size_t) i]);
                mx = waveHi[(size_t) i] + frac * (waveHi[(size_t) i + 1] - waveHi[(size_t) i]);
            }
        }
        else
        {
            const int i0 = juce::jmax (0, (int) std::floor (s0));
            const int i1 = juce::jmin (n, (int) std::ceil (s1));
            if (i0 < i1)
            {
                mn = waveLo[(size_t) i0];
                mx = waveHi[(size_t) i0];
                for (int i = i0 + 1; i < i1; ++i)
                {
                    mn = juce::jmin (mn, waveLo[(size_t) i]);
                    mx = juce::jmax (mx, waveHi[(size_t) i]);
                }
            }
        }

        // Keep at least a hairline so silence and thin detail stay visible.
        const float yHi = cy - mx * amp, yLo = cy - mn * amp;
        const float mid = 0.5f * (yHi + yLo);
        hi[(size_t) c] = juce::jmin (yHi, mid - 0.5f);
        lo[(size_t) c] = juce::jmax (yLo, mid + 0.5f);
    }

    juce::Path path;
    path.startNewSubPath ((float) x0, hi[0]);
    for (int c = 1; c < count; ++c)
        path.lineTo ((float) (x0 + c), hi[(size_t) c]);
    for (int c = count - 1; c >= 0; --c)
        path.lineTo ((float) (x0 + c), lo[(size_t) c]);
    path.closeSubPath();

    // Past Length (a reverb tail) it's dimmer: it's heard, but no envelope shapes it.
    const auto clip = g.getClipBounds();
    const int split = juce::jlimit (clip.getX(), clip.getRight(), juce::roundToInt (timeToX (1.0f)));
    for (const bool tail : { false, true })
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (tail ? clip.withLeft (split) : clip.withRight (split));
        g.setColour (colours::waveform.withAlpha (tail ? 0.45f : 0.85f));
        g.fillPath (path);
    }

    g.setColour (colours::grid);
    g.drawHorizontalLine (juce::roundToInt (cy), plot.getX(), plot.getRight());
}

void EnvelopeCanvas::drawLevelWaveform (juce::Graphics& g, juce::Rectangle<float> plot, const DrumParams& p) const
{
    // Level tab: the waveform as it's played, top and bottom, with full scale (0 dBFS)
    // on the 0 dB line and its mirror at the bottom of the plot: the clip lines. What
    // the output stage does shows as it is (flat tops at a Hard ceiling, Tube's lopsided
    // squash, Fold's folds), and only Off can go past them, as its output can.
    const float top = valueToY (envBody, 1.0f);
    const float bottom = plot.getBottom() - 1.0f;
    const float cy = 0.5f * (top + bottom), amp = 0.5f * (bottom - top);
    drawWaveformAt (g, plot, cy, amp);

    const auto mirrored = [&] (float gain, const juce::String& label, float alpha)
    {
        g.setColour (colours::clipZone.withAlpha (alpha));
        const float dashes[] { 4.0f, 3.0f };
        for (const float y : { cy - gain * amp, cy + gain * amp })
            g.drawDashedLine (juce::Line<float> (plot.getX(), y, plot.getRight(), y), dashes, 2, 1.0f);
        if (label.isNotEmpty())
        {
            g.setFont (uiFont (9.5f, true));
            g.drawText (label, juce::Rectangle<float> (plot.getRight() - 140.0f, cy - gain * amp - 13.0f, 134.0f, 12.0f),
                        juce::Justification::centredRight);
        }
    };

    // Full scale below, the 0 dB line's mirror (the grid draws the 0 dB line itself).
    g.setColour (colours::clipZone.withAlpha (0.45f));
    g.drawHorizontalLine (juce::roundToInt (bottom), plot.getX(), plot.getRight());

    const int mode = juce::jlimit (0, clipModeNames.size() - 1, p.clipMode);
    if (mode == clipModeComp)
        mirrored (juce::Decibels::decibelsToGain (p.outComp.thresholdDb),
                  "THRESHOLD " + formatGain (juce::Decibels::decibelsToGain (p.outComp.thresholdDb)), 0.4f);
    else if (mode != 0 && p.clipCeilingDb < -0.05f)
        mirrored (juce::Decibels::decibelsToGain (p.clipCeilingDb),
                  "CEILING " + formatGain (juce::Decibels::decibelsToGain (p.clipCeilingDb)), 0.55f);
}

void EnvelopeCanvas::drawPastLength (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    // Hatch the time before the hit and past Length (nothing there can be edited), and
    // mark where Length ends.
    if (const float x = timeToX (0.0f); x > plot.getX())
        hatch (g, plot.withRight (juce::jmin (plot.getRight(), x)), colours::grid.withAlpha (0.6f), 9.0f);

    const float x = timeToX (1.0f);
    if (x >= plot.getRight())
        return;

    hatch (g, plot.withLeft (juce::jmax (plot.getX(), x)), colours::grid.withAlpha (0.6f), 9.0f);
    if (x > plot.getX())
    {
        g.setColour (colours::textDim.withAlpha (0.6f));
        g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
    }
}

void EnvelopeCanvas::drawGateMarker (juce::Graphics& g, juce::Rectangle<float> plot, const DrumParams& p) const
{
    if (! isGateShown() || p.lengthMs <= 0.0f)
        return;

    const float x = msToX (gateMs);
    const float fadeEnd = msToX (gateMs + gateFadeMs);
    if (fadeEnd < plot.getX() - kGateHitDistance || x > plot.getRight())
        return;

    const bool gateHot = hover.kind == Hit::gate || drag.kind == Hit::gate;
    const bool fadeHot = hover.kind == Hit::gateFade || drag.kind == Hit::gateFade;
    const bool dragging = drag.kind == Hit::gate || drag.kind == Hit::gateFade;

    // A faint band over the fade, a dashed line where it starts, and a label.
    g.setColour (colours::reverb.withAlpha (fadeHot ? 0.2f : 0.10f));
    g.fillRect (juce::Rectangle<float>::leftTopRightBottom (x, plot.getY(), juce::jmax (x + 1.0f, fadeEnd), plot.getBottom())
                    .getIntersection (plot));
    if (fadeHot)
    {
        g.setColour (colours::reverb.withAlpha (0.8f));
        g.drawVerticalLine (juce::roundToInt (fadeEnd), plot.getY(), plot.getBottom());
    }

    if (x >= plot.getX() - 1.0f)
    {
        if (gateHot)
        {
            g.setColour (colours::reverb);
            g.fillRect (juce::Rectangle<float> (x - 0.75f, plot.getY(), 1.5f, plot.getHeight()));
        }
        else
        {
            const float dashes[] { 4.0f, 3.0f };
            g.setColour (colours::reverb.withAlpha (0.8f));
            g.drawDashedLine ({ x, plot.getY(), x, plot.getBottom() }, dashes, 2, 1.0f);
        }

        // A grip under the label says it can be dragged (up there it's clear of the curves' tails).
        const auto grip = juce::Rectangle<float> (gateHot ? 9.0f : 7.0f, gateHot ? 16.0f : 13.0f)
                              .withCentre ({ x, plot.getY() + 42.0f });
        g.setColour (gateHot ? colours::reverb : colours::display);
        g.fillRoundedRectangle (grip, 2.5f);
        g.setColour (colours::reverb.withAlpha (0.9f));
        g.drawRoundedRectangle (grip, 2.5f, 1.2f);
        g.setColour (gateHot ? colours::display : colours::reverb.withAlpha (0.8f));
        for (float dy : { -2.5f, 0.0f, 2.5f })
            g.fillRect (juce::Rectangle<float> (grip.getWidth() - 4.0f, 1.0f).withCentre (grip.getCentre().translated (0.0f, dy)));

        // Below the HUD line, on whichever side of the line has room. It reads the
        // value while hovered or dragged.
        g.setFont (uiFont (10.5f, true));
        const auto label = gateHot || fadeHot || dragging
                               ? (fadeHot ? "GATE FADE " + formatMs (gateFadeMs) : "GATE " + formatMs (gateMs))
                               : juce::String ("GATE");
        const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), label) + 2.0f;
        const float anchor = fadeHot ? fadeEnd : x;
        const bool right = anchor + 4.0f + w <= plot.getRight();
        const auto box = juce::Rectangle<float> (right ? anchor + 4.0f : anchor - 4.0f - w, plot.getY() + 16.0f, w, 14.0f);
        if (gateHot || fadeHot)
        {
            g.setColour (colours::display.withAlpha (0.85f));
            g.fillRoundedRectangle (box.expanded (3.0f, 0.0f), 3.0f);
        }
        g.setColour (colours::reverb.withAlpha (gateHot || fadeHot ? 1.0f : 0.8f));
        g.drawText (label, box, right ? juce::Justification::centredLeft : juce::Justification::centredRight);
    }
}

void EnvelopeCanvas::drawEnvelope (juce::Graphics& g, juce::Rectangle<float> plot, int envIndex) const
{
    const auto& env = envs[(size_t) envIndex];
    const auto& pts = env.getPoints();
    const auto colour = colours::forEnvelope (envIndex);

    juce::Path curve;
    bool started = false;
    float firstX = 0.0f, lastX = 0.0f;

    for (size_t s = 0; s + 1 < pts.size(); ++s)
    {
        const auto& a = pts[s];
        const auto& b = pts[s + 1];
        const float xa = timeToX (a.x), xb = timeToX (b.x);
        if (xb < plot.getX() - 2.0f || xa > plot.getRight() + 2.0f)
            continue;

        const int steps = juce::jlimit (1, 600, (int) ((xb - xa) / 2.0f));
        for (int k = 0; k <= steps; ++k)
        {
            const float u = (float) k / (float) steps;
            const float t = a.x + (b.x - a.x) * u;
            const float v = a.y + (b.y - a.y) * CurveEnvelope::shape (u, a.curve);
            const juce::Point<float> pt (timeToX (t), valueToY (envIndex, v));
            if (! started)
            {
                curve.startNewSubPath (pt);
                firstX = pt.x;
                started = true;
            }
            else
            {
                curve.lineTo (pt);
            }
            lastX = pt.x;
        }
    }

    if (! started)
        return;

    juce::Path fill (curve);
    fill.lineTo (lastX, plot.getBottom());
    fill.lineTo (firstX, plot.getBottom());
    fill.closeSubPath();
    // Lighter under a pitch curve: it's high up, so its fill covers most of the waveform.
    const float fillAlpha = isPitchEnvelope (envIndex) ? 0.10f : 0.20f;
    g.setGradientFill (juce::ColourGradient (colour.withAlpha (fillAlpha), 0.0f, plot.getY(),
                                             colour.withAlpha (0.01f), 0.0f, plot.getBottom(), false));
    g.fillPath (fill);

    g.setColour (colour);
    g.strokePath (curve, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Bend handles
    for (int i = 0; i + 1 < (int) pts.size(); ++i)
    {
        if (std::abs (pts[(size_t) i + 1].y - pts[(size_t) i].y) < 0.002f)
            continue;

        const auto hp = handlePosition (i);
        const bool hot = (hover == Hit { Hit::handle, i }) || (drag == Hit { Hit::handle, i });
        const float r = hot ? 4.5f : 3.2f;
        const auto rect = juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (hp);
        g.setColour (hot ? colour : colours::display);
        g.fillEllipse (rect);
        g.setColour (colour.withAlpha (0.8f));
        g.drawEllipse (rect, 1.2f);
    }

    // Points. One outside the pitch view gets an arrow at the edge, pointing to it.
    for (int i = 0; i < (int) pts.size(); ++i)
    {
        const auto pp = pointPosition (i);
        if (pp.y < plot.getY() - 6.0f || pp.y > plot.getBottom() + 6.0f)
        {
            const bool up = pp.y < plot.getY();
            const float ey = up ? plot.getY() + 1.0f : plot.getBottom() - 1.0f;
            const float d = up ? 1.0f : -1.0f;
            juce::Path arrow;
            arrow.addTriangle (pp.x - 5.0f, ey + d * 7.0f, pp.x + 5.0f, ey + d * 7.0f, pp.x, ey);
            g.setColour (colour.withAlpha (0.85f));
            g.fillPath (arrow);
            continue;
        }

        const bool hot = (hover == Hit { Hit::point, i }) || (drag == Hit { Hit::point, i });
        const bool isBase = isPitchEnvelope (envIndex) && i == (int) pts.size() - 1;
        const float r = hot ? 6.5f : 4.8f;
        const auto rect = juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (pp);
        g.setColour (hot ? colour : colours::display);
        g.fillEllipse (rect);
        g.setColour (colour);
        g.drawEllipse (rect, 2.0f);
        if (isBase && ! hot) // the base's point (Tune, Click Tone, the noise band): a dot in the middle
            g.fillEllipse (juce::Rectangle<float> (3.6f, 3.6f).withCentre (pp));
    }
}

void EnvelopeCanvas::drawLanes (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    const auto strip = laneStrip();

    g.setFont (uiFont (9.5f, true));
    g.setColour (colours::textDim);
    g.drawText ("OFFSET", juce::Rectangle<float> (2.0f, strip.getY(), kRulerWidth - 8.0f, strip.getHeight()),
                juce::Justification::centredRight);

    const float zeroX = msToX (0.0f);
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (strip.expanded (0.0f, 1.0f).toNearestInt());

        for (int i = 0; i < (int) lanes.size(); ++i)
        {
            const auto& lane = lanes[(size_t) i];
            const auto r = getLaneBounds (i);
            const bool hot = hover == Hit { Hit::lane, i } || drag == Hit { Hit::lane, i };
            const float alpha = lane.silent ? 0.38f : 1.0f;

            g.setColour (colours::grid.withAlpha (hot ? 0.9f : 0.55f));
            g.fillRoundedRectangle (r, 2.0f);

            // The layer plays from its start for Length, fading as the envelopes do.
            const float xs = msToX (lane.offsetMs);
            const float xe = msToX (lane.offsetMs + lengthMs);
            const auto audible = juce::Rectangle<float>::leftTopRightBottom (juce::jmax (xs, zeroX), r.getY(), juce::jmax (xs, xe), r.getBottom());
            g.setGradientFill (juce::ColourGradient (lane.colour.withAlpha ((hot ? 0.5f : 0.36f) * alpha), audible.getX(), 0.0f,
                                                     lane.colour.withAlpha (0.05f * alpha), audible.getRight(), 0.0f, false));
            g.fillRoundedRectangle (audible, 2.0f);

            // Started before the hit: that part is cut off.
            if (xs < zeroX)
            {
                const auto cut = juce::Rectangle<float>::leftTopRightBottom (xs, r.getY(), zeroX, r.getBottom());
                g.setColour (colours::mute.withAlpha (0.14f * alpha));
                g.fillRect (cut);
                hatch (g, cut, colours::mute.withAlpha (0.55f * alpha), 4.0f);
                g.setColour (colours::mute.withAlpha (0.9f * alpha));
                g.fillRect (juce::Rectangle<float> (zeroX - 0.75f, r.getY(), 1.5f, r.getHeight()));
            }

            // The grip: where the layer starts.
            g.setColour (lane.colour.withAlpha ((hot ? 1.0f : 0.85f) * alpha));
            g.fillRoundedRectangle (juce::Rectangle<float> (xs - 1.5f, r.getY(), 3.0f, r.getHeight()), 1.0f);

            if (r.getHeight() >= 8.5f)
            {
                g.setFont (uiFont (juce::jmin (9.5f, r.getHeight() - 1.0f), true));
                const auto text = lane.name + "  " + formatOffset (lane.offsetMs);
                g.setColour (colours::text.withAlpha ((hot ? 1.0f : 0.8f) * alpha));
                g.drawText (text, juce::Rectangle<float> (juce::jmax (xs, zeroX) + 5.0f, r.getY(), 160.0f, r.getHeight()),
                            juce::Justification::centredLeft, false);
            }
        }
    }

    // 0 ms, where the hit is, through the plot and the lanes (unless it's the plot's edge).
    if (zeroX > plot.getX() + 1.0f && zeroX < plot.getRight())
    {
        g.setColour (colours::text.withAlpha (0.4f));
        g.drawVerticalLine (juce::roundToInt (zeroX), plot.getY(), strip.getBottom());
    }
}

void EnvelopeCanvas::drawHud (juce::Graphics& g, juce::Rectangle<float> plot, const DrumParams& p) const
{
    const auto accent = colours::forEnvelope ((int) active);
    const auto hudArea = juce::Rectangle<float> (plot.getRight() - 606.0f, plot.getY() - 2.0f, 600.0f, 16.0f);

    const Hit focus = drag.kind != Hit::none ? drag : hover;
    const auto& pts = envs[(size_t) active].getPoints();
    const bool isPitch = isPitchEnvelope (active);

    auto font = uiFont (11.5f, true);
    auto colour = accent;
    juce::String text;
    if (focus.kind == Hit::point && juce::isPositiveAndBelow (focus.index, (int) pts.size()))
    {
        const auto& pt = pts[(size_t) focus.index];
        text = getCurveName() + "   " + formatMs (pt.x * p.lengthMs) + "   " + describeValue (active, focus.index, pt.y);
        if (isPitch && focus.index == (int) pts.size() - 1 && drag.kind == Hit::none)
            text << "   drag: " << baseDragName (active);
    }
    else if (focus.kind == Hit::handle)
    {
        text = "Drag to bend  -  double-click to straighten";
    }
    else if (focus.kind == Hit::gate || focus.kind == Hit::gateFade)
    {
        colour = colours::reverb;
        if (focus.kind == Hit::gate)
        {
            text = "Reverb gate " + formatMs (gateMs) + " (fade " + formatMs (gateFadeMs) + ")";
            if (drag.kind == Hit::none)
                text << "   Drag: Gate Time   Double-click: default";
        }
        else
        {
            text = "Reverb gate fade " + formatMs (gateFadeMs);
            if (drag.kind == Hit::none)
                text << "   Drag: Gate Fade   Double-click: default";
        }
    }
    else if (focus.kind == Hit::lane && juce::isPositiveAndBelow (focus.index, (int) lanes.size()))
    {
        const auto& lane = lanes[(size_t) focus.index];
        colour = lane.colour;
        text = titleCase (lane.name) + " offset " + formatOffset (lane.offsetMs);
        if (lane.offsetMs < -0.05f)
            text << ": the first " << formatOffset (-lane.offsetMs).trimCharactersAtStart ("+") << " are cut";
        else if (lane.offsetMs > 0.05f)
            text << ": starts late";
        if (drag.kind == Hit::none)
            text << "   Drag: earlier / later   Double-click: 0";
    }
    else if (focus.kind == Hit::ruler)
    {
        text = pitchFocus > 0.001f ? "Drag: pan the pitch axis   Scroll: focus   Double-click: whole range"
                                   : "Scroll: focus in on the curves";
    }
    else if (! hasCurve)
    {
        font = uiFont (11.0f);
        colour = colours::textDim.withAlpha (0.8f);
        text = titleCase (getLayerName (layer)) + (tab == pitchTab ? " pitch is Flat: no pitch movement"
                                                                   : " level is Flat: full level for the whole Length")
             + "   Pick OWN above to draw its curve";
    }
    else if (isCoupled())
    {
        font = uiFont (11.0f);
        colour = accent.withAlpha (0.85f);
        text = "Coupled to the " + curveName (active) + " curve: editing it changes "
             + titleCase (getLayerName (layerOfCore (active))) + " too   Pick OWN above for a curve of its own";
    }
    else
    {
        font = uiFont (11.0f);
        colour = colours::textDim.withAlpha (0.8f);
        text = isPitch ? "Double-click: add / remove point   Drag the end point: " + baseDragName (active)
                       : "Double-click: add / remove point   Over 0 dB drives the clipper";
    }

    // A backing, so markers under the text (the gate line) don't cut through it.
    const float w = juce::jmin (hudArea.getWidth(), juce::GlyphArrangement::getStringWidth (font, text));
    g.setColour (colours::display.withAlpha (0.85f));
    g.fillRoundedRectangle (hudArea.withLeft (hudArea.getRight() - w).expanded (4.0f, 0.0f), 3.0f);

    g.setFont (font);
    g.setColour (colour);
    g.drawText (text, hudArea, juce::Justification::centredRight);
}

//==============================================================================
KeyBadge::KeyBadge (DrumFoundryProcessor& p) : processor (p)
{
    setRepaintsOnMouseActivity (true);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    updateTooltip();
}

void KeyBadge::setState (bool newKeyOn, int newNote)
{
    if (keyOn == newKeyOn && note == newNote)
        return;

    keyOn = newKeyOn;
    note = newNote;
    updateTooltip();
    repaint();
}

juce::String KeyBadge::getText() const
{
    if (! keyOn)
        return "KEY OFF";

    auto text = "KEY  " + noteName ((float) note);
    if (note != 60)
    {
        const int semis = note - 60;
        const juce::String sign = semis > 0 ? "+" : juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")); // minus sign
        text << juce::String (juce::CharPointer_UTF8 ("  \xc2\xb7  ")) << sign << std::abs (semis) << " st";
    }
    return text;
}

int KeyBadge::getIdealWidth() const
{
    return (int) std::ceil (juce::GlyphArrangement::getStringWidth (uiFont (11.5f, true), getText())) + 22;
}

void KeyBadge::updateTooltip()
{
    const auto played = noteName ((float) note);
    if (! keyOn)
    {
        setTooltip ("KEY is off: every note plays the drum at Tune, so the display shows what any note plays.\n"
                    "Click to switch KEY on.");
        return;
    }

    const auto tuneHz = processor.readParams().tuneHz;
    juce::String tip ("KEY is on: C3 plays the drum at Tune, and other notes transpose the whole drum from there.\n"
                      "The display, the pitch ruler and the trigger pad follow the last note played");
    if (note == 60)
    {
        tip << " (C3 so far).";
    }
    else
    {
        const int semis = note - 60;
        tip << ": " << played << ", " << std::abs (semis) << (std::abs (semis) == 1 ? " semitone " : " semitones ")
            << (semis < 0 ? "down" : "up") << ", so Tune's " << juce::roundToInt (tuneHz) << " Hz plays at "
            << juce::roundToInt (tuneHz * std::exp2 ((float) semis / 12.0f)) << " Hz.";
    }
    tip << "\nClick to go back to C3 or switch KEY off.";
    setTooltip (tip);
}

void KeyBadge::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    const bool hover = isMouseOver();
    const auto colour = isTransposed() ? colours::pitch : (keyOn ? colours::text : colours::textDim);

    g.setColour (isTransposed() ? colours::pitch.withAlpha (hover ? 0.18f : 0.12f)
                                : (hover ? colours::panelBorder.withAlpha (0.6f) : colours::display));
    g.fillRoundedRectangle (bounds, 4.0f);
    g.setColour (isTransposed() ? colours::pitch.withAlpha (0.7f) : (hover ? colours::textDim : colours::panelBorder));
    g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

    g.setFont (uiFont (11.5f, true));
    g.setColour (colour);
    g.drawText (getText(), bounds.translated (0.0f, 0.5f), juce::Justification::centred, false);
}

void KeyBadge::mouseDown (const juce::MouseEvent&)
{
    auto* keyParam = processor.getState().getParameter (pid::keytrack);

    juce::PopupMenu menu;
    menu.addSectionHeader ("Last note played: " + noteName ((float) processor.getDisplayNote()));
    menu.addItem ("Key tracking (KEY)", keyParam != nullptr, keyOn, [keyParam, on = keyOn]
    {
        keyParam->beginChangeGesture();
        keyParam->setValueNotifyingHost (on ? 0.0f : 1.0f);
        keyParam->endChangeGesture();
    });
    menu.addItem ("Show and play C3", processor.getDisplayNote() != 60, false,
                  [this] { processor.setDisplayNote (60); });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (190));
}

//==============================================================================
ClipCurveIcon::ClipCurveIcon (APVTS& state)
{
    mode = state.getRawParameterValue (pid::clip);
    drive = state.getRawParameterValue (pid::clipDrive);
    ceiling = state.getRawParameterValue (pid::clipCeiling);
    threshold = state.getRawParameterValue (compParamId (-1, pid::compThresh));
    ratio = state.getRawParameterValue (compParamId (-1, pid::compRatio));
    makeup = state.getRawParameterValue (compParamId (-1, pid::compMakeup));
    for (const auto& id : { juce::String (pid::clip), juce::String (pid::clipDrive), juce::String (pid::clipCeiling),
                            compParamId (-1, pid::compThresh), compParamId (-1, pid::compRatio), compParamId (-1, pid::compMakeup) })
        if (auto* param = state.getParameter (id))
            listeners.push_back (std::make_unique<juce::ParameterAttachment> (*param, [this] (float) { repaint(); }));

    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip ("The output clipper: what happens to anything over the ceiling (the curve: input across, output up).\n"
                "Amplitude drawn over 0 dB drives into it. Click to choose the mode.");
}

void ClipCurveIcon::paint (juce::Graphics& g)
{
    const bool hot = isMouseOver();
    const int m = mode != nullptr ? juce::roundToInt (mode->load()) : 0;
    const float driveDb = drive != nullptr ? drive->load() : 0.0f;
    const float ceilingDb = ceiling != nullptr ? ceiling->load() : 0.0f;
    const auto accent = m > 0 ? colours::clipZone : colours::textDim;

    auto area = getLocalBounds().toFloat();
    const auto box = area.removeFromRight (juce::jmin (area.getHeight(), 24.0f)).withSizeKeepingCentre (22.0f, 22.0f);

    const bool comp = m == clipModeComp;
    g.setFont (uiFont (11.0f, true));
    g.setColour (hot ? colours::text : colours::textDim);
    g.drawText (comp ? "COMP" : "CLIP", area.withTrimmedRight (6.0f), juce::Justification::centredRight);

    g.setColour (colours::display);
    g.fillRoundedRectangle (box, 3.0f);
    g.setColour (hot ? colours::textDim : colours::panelBorder);
    g.drawRoundedRectangle (box, 3.0f, 1.0f);

    const auto plot = box.reduced (3.0f);
    if (comp)
    {
        // The compressor's static curve in dB: input -48..0 across, output up; the threshold dashed.
        CompSettings s;
        s.thresholdDb = threshold != nullptr ? threshold->load() : -12.0f;
        s.ratio = ratio != nullptr ? ratio->load() : 4.0f;
        s.makeupDb = makeup != nullptr ? makeup->load() : 0.0f;
        const auto mapX = [plot] (float db) { return plot.getX() + (db + 48.0f) / 48.0f * plot.getWidth(); };
        const auto mapDbY = [plot] (float db) { return plot.getBottom() - juce::jlimit (0.0f, 1.0f, (db + 48.0f) / 48.0f) * plot.getHeight(); };
        g.setColour (colours::grid.brighter (0.3f));
        g.drawVerticalLine (juce::roundToInt (mapX (s.thresholdDb)), plot.getY(), plot.getBottom());
        juce::Path curve;
        for (int i = 0; i <= 40; ++i)
        {
            const float in = -48.0f + 48.0f * (float) i / 40.0f;
            const juce::Point<float> pt (mapX (in), mapDbY (Compressor::outputDb (in, s)));
            if (i == 0)
                curve.startNewSubPath (pt);
            else
                curve.lineTo (pt);
        }
        g.setColour (accent);
        g.strokePath (curve, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        return;
    }

    // Input -2..2 across, output -1.25..1.25 up; the ceiling dashed.
    const auto mapY = [plot] (float y) { return plot.getCentreY() - y / 1.25f * plot.getHeight() * 0.5f; };
    const float ceil = std::pow (10.0f, ceilingDb / 20.0f);
    g.setColour (colours::grid.brighter (0.3f));
    g.drawHorizontalLine (juce::roundToInt (mapY (ceil)), plot.getX(), plot.getRight());
    g.drawHorizontalLine (juce::roundToInt (mapY (-ceil)), plot.getX(), plot.getRight());

    juce::Path curve;
    constexpr int n = 40;
    for (int i = 0; i <= n; ++i)
    {
        const float x = -2.0f + 4.0f * (float) i / (float) n;
        const float y = juce::jlimit (-1.25f, 1.25f, clipTransferPreview (m, x, driveDb, ceilingDb));
        const juce::Point<float> pt (plot.getX() + plot.getWidth() * (float) i / (float) n, mapY (y));
        if (i == 0)
            curve.startNewSubPath (pt);
        else
            curve.lineTo (pt);
    }
    g.setColour (accent);
    g.strokePath (curve, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void ClipCurveIcon::mouseDown (const juce::MouseEvent&)
{
    if (onClick)
        onClick();
}

//==============================================================================
DisplayPanel::DisplayPanel (DrumFoundryProcessor& p)
    : processor (p), canvas (p), keyBadge (p), lfoPage (p),
      focusDial ("Focus", colours::pitch, 0.0,
                 [] (double v) { return v < 0.005 ? juce::String ("FULL") : juce::String (juce::roundToInt (v * 100.0)) + "%"; }),
      clipIcon (p.getState()),
      clipBox (p.getState(), pid::clip, {}, colours::clipZone),
      clipDrive (p.getState(), pid::clipDrive, "Drive", colours::clipZone),
      clipCeiling (p.getState(), pid::clipCeiling, "Ceiling", colours::clipZone),
      compThresh (p.getState(), compParamId (-1, pid::compThresh), "Thresh", colours::clipZone),
      compRatio (p.getState(), compParamId (-1, pid::compRatio), "Ratio", colours::clipZone),
      compAttack (p.getState(), compParamId (-1, pid::compAttack), "Attack", colours::clipZone),
      compRelease (p.getState(), compParamId (-1, pid::compRelease), "Release", colours::clipZone),
      compMakeup (p.getState(), compParamId (-1, pid::compMakeup), "Makeup", colours::clipZone),
      compStyle (p.getState(), compParamId (-1, pid::compStyle), {}, colours::clipZone)
{
    static const char* tabNames[] { "PITCH", "LEVEL", "LFO" };
    static const char* tabTips[] { "The selected layer's pitch curve (Body: from Tune; Click: from Click Tone; Noise: moves the noise band)",
                                   "The selected layer's level curve",
                                   "Three LFOs: drag one's name onto any knob to modulate it" };
    for (int t = 0; t < numTabs; ++t)
    {
        auto& tab = tabs[(size_t) t];
        tab.setButtonText (tabNames[t]);
        tab.getProperties().set ("tab", true);
        tab.setClickingTogglesState (true);
        tab.setRadioGroupId (1001);
        tab.setTooltip (tabTips[t]);
        tab.onClick = [this, t] { showTab (t); };
        addAndMakeVisible (tab);
    }

    // An extra layer's curve: its own, coupled to a core layer's, or none. Item ids are
    // LayerEnv + 1; it isn't attached to the parameter, since switching to Own also
    // copies the curve (DrumFoundryProcessor::setLayerCurveSource()).
    sourceBox.addItem ("OWN", layerEnvOwn + 1);
    sourceBox.addSeparator();
    sourceBox.addItem ("BODY", layerEnvBody + 1);
    sourceBox.addItem ("NOISE", layerEnvNoise + 1);
    sourceBox.addItem ("CLICK", layerEnvClick + 1);
    sourceBox.addSeparator();
    sourceBox.addItem ("FLAT", layerEnvFlat + 1);
    sourceBox.setColour (juce::ComboBox::textColourId, colours::layer);
    sourceBox.onChange = [this]
    {
        const int k = canvas.getLayer() - EnvelopeCanvas::firstExtraLayer;
        const int id = sourceBox.getSelectedId();
        if (! juce::isPositiveAndBelow (k, kNumExtraLayers) || id <= 0)
            return;
        processor.getUndoHistory().noteUserActivity(); // the choice (and any copy of the curve) is one step
        processor.setLayerCurveSource (k, canvas.getTab() == EnvelopeCanvas::pitchTab, id - 1);
        canvas.syncEnvelopes();
        canvas.showLayer (canvas.getLayer(), canvas.getTab());
        syncFromCanvas();
        if (canvas.onEdited)
            canvas.onEdited();
    };
    addChildComponent (sourceBox);

    lengthBar.setSliderStyle (juce::Slider::LinearBar);
    lengthBar.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    lengthBar.getProperties().set ("caption", "LENGTH");
    lengthBar.setColour (juce::Slider::trackColourId, colours::text);
    lengthBar.setTooltip ("Total length of the hit. Stretches all the curves.");
    addAndMakeVisible (lengthBar);
    lengthAttachment = std::make_unique<APVTS::SliderAttachment> (p.getState(), pid::length, lengthBar);
    if (auto* param = p.getState().getParameter (pid::length))
        lengthBar.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));

    fitButton.setTooltip ("Fit the view to Length (press again to include the reverb tail). "
                          "Scroll or pinch on the display to zoom.");
    fitButton.onClick = [this] { canvas.fitView(); };
    addAndMakeVisible (fitButton);

    // ALL: every playing layer's curve on this tab together; click a dimmed one to edit it.
    allButton.setClickingTogglesState (true);
    allButton.setColour (juce::TextButton::buttonOnColourId, colours::text.withAlpha (0.85f));
    allButton.setColour (juce::TextButton::textColourOnId, colours::background);
    allButton.setTooltip ("Show every sound element's curves on this tab together (pitch or level). "
                          "Click a dimmed one to edit it.");
    allButton.setToggleState (processor.uiState.chartShowAll, juce::dontSendNotification);
    canvas.setShowAll (processor.uiState.chartShowAll);
    allButton.onClick = [this]
    {
        processor.uiState.chartShowAll = allButton.getToggleState();
        canvas.setShowAll (allButton.getToggleState());
    };
    addAndMakeVisible (allButton);
    canvas.onLayerPicked = [this] (int picked)
    {
        syncFromCanvas();
        if (onLayerFocused)
            onLayerFocused (picked);
    };

    // Pitch: how much of 0 Hz - 16 kHz the axis shows (fully left, the default: all of it).
    auto& focus = focusDial.getSlider();
    focus.setTooltip ("Focus: zooms the pitch axis from the whole 0 Hz - 16 kHz range in to a band around the curves.\n"
                      "Scroll over the ruler to focus, drag it (or alt-scroll) to pan, double-click it for the whole range.");
    canvas.setPitchFocus (processor.uiState.pitchFocus);
    canvas.setPitchPan (processor.uiState.pitchPan);
    focus.setValue (canvas.getPitchFocus(), juce::dontSendNotification);
    focus.onValueChange = [this]
    {
        canvas.setPitchFocus ((float) focusDial.getSlider().getValue());
        processor.uiState.pitchFocus = canvas.getPitchFocus();
        processor.uiState.pitchPan = canvas.getPitchPan();
        focusDial.repaint();
    };
    canvas.onPitchViewChanged = [this]
    {
        focusDial.getSlider().setValue (canvas.getPitchFocus(), juce::dontSendNotification);
        processor.uiState.pitchFocus = canvas.getPitchFocus();
        processor.uiState.pitchPan = canvas.getPitchPan();
        focusDial.repaint();
    };

    // Level: the clipper that level over 0 dB drives (the same parameters as the Output section's).
    clipIcon.onClick = [this] { clipBox.getComboBox().showPopup(); };
    clipBox.getComboBox().setTooltip ("The output stage (the same as the Output section's Clip): a clipper shape, Comp (the output\n"
                                      "compressor) or Off. Level drawn over 0 dB drives into it; Off lets the output go over 0 dBFS.");
    clipDrive.getSlider().setTooltip ("Clip Drive: gain into the clipper");
    clipCeiling.getSlider().setTooltip ("Clip Ceiling: the most the clipper lets out");
    compThresh.getSlider().setTooltip ("Output compressor threshold: where gain reduction starts (the dashed line)");
    compRatio.getSlider().setTooltip ("Output compressor ratio");
    compAttack.getSlider().setTooltip ("Output compressor attack: a slower attack lets the transient through, then clamps the body");
    compRelease.getSlider().setTooltip ("Output compressor release");
    compMakeup.getSlider().setTooltip ("Output compressor makeup gain");
    compStyle.getComboBox().setTooltip ("Output compressor character: Clean, FET (1176-style), Opto (LA-2A-style), "
                                        "Bus (SSL-style glue) or Punch (dbx 160-style)");

    for (auto* c : std::initializer_list<juce::Component*> { &focusDial, &clipIcon, &clipBox, &clipDrive, &clipCeiling,
                                                             &compThresh, &compRatio, &compAttack, &compRelease, &compMakeup, &compStyle })
        addChildComponent (c);

    // Comp swaps the clipper's dials for the compressor's.
    if (auto* param = p.getState().getParameter (pid::clip))
        clipModeListener = std::make_unique<juce::ParameterAttachment> (*param, [this] (float) { updateTabControls(); });

    addAndMakeVisible (keyBadge);
    addAndMakeVisible (canvas);
    addChildComponent (lfoPage);

    // Where the editor was: the layer and tab on show.
    const auto& ui = processor.uiState;
    const bool lfo = ui.lfoTab;
    canvas.showLayer (ui.chartLayer, ui.chartTab);
    syncFromCanvas();
    if (lfo)
        showTab (lfoTab);
}

void DisplayPanel::setKeyState (bool keyOn, int midiNote)
{
    const int oldWidth = keyBadge.getIdealWidth();
    keyBadge.setState (keyOn, midiNote);
    canvas.setNoteOffset (keyOn ? midiNote - 60 : 0);
    if (keyBadge.getIdealWidth() != oldWidth)
        resized();
}

void DisplayPanel::setLfoShown (bool shouldShow)
{
    lfoShown = shouldShow;
    processor.uiState.lfoTab = shouldShow;
    canvas.setVisible (! shouldShow);
    lfoPage.setVisible (shouldShow);
}

void DisplayPanel::showTab (int tab)
{
    if (tab == lfoTab)
    {
        setLfoShown (true);
        syncFromCanvas();
        return;
    }

    lfoShown = false;
    canvas.showLayer (canvas.getLayer(), juce::jlimit (0, 1, tab));
    syncFromCanvas();
}

void DisplayPanel::focusLayer (int layer, bool bringToFront)
{
    if (bringToFront)
        lfoShown = false;
    if (! juce::isPositiveAndBelow (layer, (int) EnvelopeCanvas::numLayers))
        return;
    canvas.showLayer (layer, canvas.getTab());
    syncFromCanvas();
}

void DisplayPanel::update()
{
    canvas.syncEnvelopes();
    const auto before = canvas.getActiveEnvelope();
    const auto name = canvas.getLayerName (canvas.getLayer());
    canvas.refreshActiveCurve();

    // An extra layer's source menu follows its parameter (undo, automation).
    bool sourceMoved = false;
    if (const int k = canvas.getLayer() - EnvelopeCanvas::firstExtraLayer; juce::isPositiveAndBelow (k, kNumExtraLayers))
    {
        const auto* v = processor.getState().getRawParameterValue (layerParamId (k, canvas.getTab() == EnvelopeCanvas::pitchTab ? pid::layPitchEnv
                                                                                                                              : pid::layEnv));
        sourceMoved = v != nullptr && sourceBox.getSelectedId() != juce::roundToInt (v->load()) + 1;
    }

    if (canvas.getActiveEnvelope() != before || name != shownLayerName || sourceMoved)
        syncFromCanvas();
}

void DisplayPanel::syncFromCanvas()
{
    auto& ui = processor.uiState;
    const int tab = canvas.getTab();
    const int layer = canvas.getLayer();
    ui.chartLayer = layer;
    ui.chartTab = tab;

    // The tabs' underline takes the colour of the curve on show.
    const auto curveColour = colours::forEnvelope (canvas.resolveEnvelope (layer, tab) >= 0 ? canvas.resolveEnvelope (layer, tab)
                                                                                           : canvas.getActiveEnvelope());
    for (int t = 0; t < 2; ++t)
    {
        tabs[(size_t) t].setToggleState (t == tab && ! lfoShown, juce::dontSendNotification);
        const int env = canvas.resolveEnvelope (layer, t);
        tabs[(size_t) t].setColour (juce::TextButton::buttonOnColourId, env >= 0 ? colours::forEnvelope (env) : curveColour);
    }
    tabs[lfoTab].setToggleState (lfoShown, juce::dontSendNotification);
    tabs[lfoTab].setColour (juce::TextButton::buttonOnColourId, colours::lfo (0));
    setLfoShown (lfoShown);

    // An extra layer's curve source.
    const int k = layer - EnvelopeCanvas::firstExtraLayer;
    if (juce::isPositiveAndBelow (k, kNumExtraLayers))
    {
        const bool pitch = tab == EnvelopeCanvas::pitchTab;
        const auto* v = processor.getState().getRawParameterValue (layerParamId (k, pitch ? pid::layPitchEnv : pid::layEnv));
        const int source = v != nullptr ? juce::jlimit (0, layerEnvNames.size() - 1, juce::roundToInt (v->load())) : (int) layerEnvOwn;
        if (sourceBox.getSelectedId() != source + 1)
            sourceBox.setSelectedId (source + 1, juce::dontSendNotification);
        const auto who = juce::String (canvas.getLayerName (layer)).toLowerCase();
        sourceBox.setTooltip ("Where " + who + "'s " + (pitch ? "pitch" : "level") + " curve comes from:\n"
                              "OWN: a curve of its own\n"
                              "BODY, NOISE, CLICK: coupled to that layer's curve (editing it here edits that layer's too)\n"
                              + (pitch ? "FLAT: no pitch movement" : "FLAT: full level for the whole Length"));
    }

    const auto name = canvas.getLayerName (layer);
    if (name != shownLayerName)
    {
        shownLayerName = name;
        repaint();
    }
    updateTabControls();
}

void DisplayPanel::updateTabControls()
{
    // The LFO tab has none of the curves' controls.
    const bool pitch = canvas.getTab() == EnvelopeCanvas::pitchTab;
    focusDial.setVisible (pitch && ! lfoShown);
    const auto* clipMode = processor.getState().getRawParameterValue (pid::clip);
    const bool compStage = clipMode != nullptr && juce::roundToInt (clipMode->load()) == clipModeComp;
    for (auto* c : std::initializer_list<juce::Component*> { &clipIcon, &clipBox })
        c->setVisible (! pitch && ! lfoShown);
    for (auto* c : std::initializer_list<juce::Component*> { &clipDrive, &clipCeiling })
        c->setVisible (! pitch && ! lfoShown && ! compStage);
    for (auto* c : std::initializer_list<juce::Component*> { &compThresh, &compRatio, &compAttack, &compRelease, &compMakeup, &compStyle })
        c->setVisible (! pitch && ! lfoShown && compStage);
    sourceBox.setVisible (! lfoShown && canvas.getLayer() >= EnvelopeCanvas::firstExtraLayer);
    fitButton.setVisible (! lfoShown);
    allButton.setVisible (! lfoShown);
    resized();
    repaint();
}

void DisplayPanel::paint (juce::Graphics& g)
{
    // Dividers between the tabs, the line buttons and the tab's own controls.
    g.setColour (colours::panelBorder);
    for (int x : dividerX)
        if (x >= 0)
            g.drawVerticalLine (x, 6.0f, 22.0f);

    // The layer on show, in its colour.
    if (! lfoShown && ! layerNameArea.isEmpty())
    {
        const int layer = canvas.getLayer();
        const auto colour = layer == EnvelopeCanvas::bodyLayer  ? colours::body
                          : layer == EnvelopeCanvas::noiseLayer ? colours::noise
                          : layer == EnvelopeCanvas::clickLayer ? colours::click
                                                                : colours::layer;
        auto area = layerNameArea.toFloat();
        g.setColour (colour);
        g.fillRoundedRectangle (area.removeFromLeft (10.0f).withSizeKeepingCentre (10.0f, 2.5f), 1.0f);
        area.removeFromLeft (6.0f);
        g.setFont (uiFont (11.5f, true));
        g.drawFittedText (shownLayerName, area.toNearestInt(), juce::Justification::centredLeft, 1, 0.9f);
    }

    if (lfoShown && dividerX[0] >= 0)
    {
        g.setFont (uiFont (11.5f));
        g.setColour (colours::textDim);
        g.drawText ("Drag an LFO's name onto any knob to modulate it. Right-click a modulated knob to remove an LFO.",
                    juce::Rectangle<int> (dividerX[0] + 14, 0, keyBadge.getX() - dividerX[0] - 24, 28),
                    juce::Justification::centredLeft, true);
    }
}

void DisplayPanel::resized()
{
    auto area = getLocalBounds();
    auto top = area.removeFromTop (28);
    area.removeFromTop (6);

    for (int t = 0; t < numTabs; ++t)
    {
        tabs[(size_t) t].setBounds (top.removeFromLeft (t == lfoTab ? 64 : 84));
        top.removeFromLeft (4);
    }

    if (fitButton.isVisible())
    {
        fitButton.setBounds (top.removeFromRight (46).reduced (0, 2));
        top.removeFromRight (8);
    }
    lengthBar.setBounds (top.removeFromRight (190).reduced (0, 2));
    top.removeFromRight (8);
    const int badgeWidth = keyBadge.getIdealWidth();
    keyBadge.setBounds (top.removeFromRight (badgeWidth).reduced (0, 2));

    // The line buttons, after a divider.
    top.removeFromLeft (6);
    dividerX[0] = top.getX();
    dividerX[1] = -1;
    canvas.setBounds (area);
    lfoPage.setBounds (area);
    layerNameArea = {};
    if (lfoShown)
        return;

    // The layer's name, then (an extra layer) where the curve comes from.
    top.removeFromLeft (12);
    layerNameArea = top.removeFromLeft (84);
    if (sourceBox.isVisible())
    {
        sourceBox.setBounds (top.removeFromLeft (84).reduced (0, 3));
        top.removeFromLeft (4);
    }
    allButton.setBounds (top.removeFromLeft (40).reduced (0, 3));
    top.removeFromLeft (4);

    // The tab's own controls, after another.
    top.removeFromLeft (6);
    dividerX[1] = top.getX();
    top.removeFromLeft (14);
    if (focusDial.isVisible())
    {
        focusDial.setBounds (top.removeFromLeft (focusDial.getIdealWidth()));
    }
    else
    {
        clipIcon.setBounds (top.removeFromLeft (62));
        top.removeFromLeft (6);
        clipBox.setBounds (top.removeFromLeft (86).reduced (0, 2));
        if (compThresh.isVisible())
        {
            top.removeFromLeft (8);
            compStyle.setBounds (top.removeFromLeft (76).reduced (0, 2));
            for (auto* d : { &compThresh, &compRatio, &compAttack, &compRelease, &compMakeup })
            {
                top.removeFromLeft (d == &compThresh ? 12 : 10);
                d->setBounds (top.removeFromLeft (d->getIdealWidth()));
            }
        }
        else
        {
            top.removeFromLeft (16);
            clipDrive.setBounds (top.removeFromLeft (clipDrive.getIdealWidth()));
            top.removeFromLeft (12);
            clipCeiling.setBounds (top.removeFromLeft (clipCeiling.getIdealWidth()));
        }
    }
}

} // namespace drum::ui
