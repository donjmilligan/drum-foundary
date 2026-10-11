// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "EqPanel.h"
#include "FoundryLookAndFeel.h"
#include <juce_dsp/juce_dsp.h>

namespace drum::ui
{

namespace
{
    constexpr float kMinHz = 20.0f, kMaxHz = 20000.0f;
    constexpr float kNodeRadius = 9.0f;
    constexpr float kRulerLeft = 40.0f, kAxisBottom = 18.0f;
    constexpr float kSpectrumFloorDb = -72.0f; // the spectrum's range under its loudest point
    constexpr float kListenQ = 4.0f;            // the Alt-drag sweep's band pass

    const juce::String minusSign (juce::CharPointer_UTF8 ("\xe2\x88\x92"));

    juce::String formatHz (float hz)
    {
        if (hz >= 1000.0f)
            return juce::String (hz / 1000.0f, hz >= 10000.0f ? 1 : 2) + " kHz";
        return juce::String (juce::roundToInt (hz)) + " Hz";
    }

    juce::String formatDb (float db)
    {
        if (std::abs (db) < 0.05f)
            return "0.0 dB";
        return (db > 0.0f ? juce::String ("+") : minusSign) + juce::String (std::abs (db), 1) + " dB";
    }

    juce::String shortHz (float hz)
    {
        return hz >= 1000.0f ? juce::String (juce::roundToInt (hz / 1000.0f)) + "k" : juce::String (juce::roundToInt (hz));
    }
}

juce::Colour eqBandColour (int band)
{
    static const juce::Colour colours[] { juce::Colour (0xffff7a59), juce::Colour (0xffffc24b), juce::Colour (0xffb8e04f),
                                          juce::Colour (0xff4fe3a1), juce::Colour (0xff4fd2ea), juce::Colour (0xff6f9bff),
                                          juce::Colour (0xffb98cff), juce::Colour (0xffff6fb5) };
    return colours[juce::jlimit (0, kNumEqBands - 1, band)];
}

std::vector<EqPeak> findResonances (const std::vector<float>& hz, const std::vector<float>& levelDb, int maxPeaks)
{
    std::vector<EqPeak> out;
    const int n = (int) juce::jmin (hz.size(), levelDb.size());
    if (n < 8)
        return out;

    // Points per octave on the grid, from its spacing.
    const float perOctave = (float) (n - 1) / std::log2 (hz[(size_t) n - 1] / juce::jmax (1.0e-3f, hz[0]));
    const int third = juce::jmax (2, juce::roundToInt (perOctave / 3.0f));
    const float loudest = *std::max_element (levelDb.begin(), levelDb.begin() + n);

    std::vector<EqPeak> candidates;
    for (int i = 2; i < n - 2; ++i)
    {
        const float f = hz[(size_t) i], v = levelDb[(size_t) i];
        if (f < 30.0f || f > 16000.0f || v < loudest - 40.0f)
            continue;
        if (! (v >= levelDb[(size_t) i - 1] && v >= levelDb[(size_t) i + 1] && v > levelDb[(size_t) i - 2] && v > levelDb[(size_t) i + 2]))
            continue;

        double sum = 0.0;
        int count = 0;
        for (int j = juce::jmax (0, i - third); j <= juce::jmin (n - 1, i + third); ++j)
        {
            sum += levelDb[(size_t) j];
            ++count;
        }
        const float prominence = v - (float) (sum / count);
        if (prominence < 3.0f)
            continue;

        // Width at 3 dB down, for its Q.
        int lo = i, hi = i;
        while (lo > 0 && levelDb[(size_t) lo] > v - 3.0f) --lo;
        while (hi < n - 1 && levelDb[(size_t) hi] > v - 3.0f) ++hi;
        const float bw = juce::jmax (1.0e-3f, hz[(size_t) hi] - hz[(size_t) lo]);
        candidates.push_back ({ f, prominence, juce::jlimit (2.0f, 16.0f, f / bw) });
    }

    std::sort (candidates.begin(), candidates.end(), [] (const EqPeak& a, const EqPeak& b) { return a.prominenceDb > b.prominenceDb; });
    for (const auto& c : candidates)
    {
        bool apart = true;
        for (const auto& kept : out)
            apart = apart && std::abs (std::log2 (c.hz / kept.hz)) >= 0.25f;
        if (apart)
            out.push_back (c);
        if ((int) out.size() >= maxPeaks)
            break;
    }
    return out;
}

//==============================================================================
EqGraph::EqGraph (DrumFoundryProcessor& p) : processor (p)
{
    params = processor.readParams();
    setRepaintsOnMouseActivity (false);
    startTimerHz (30);
}

EqGraph::~EqGraph()
{
    stopListen();
    for (auto* param : gestureParams)
        param->endChangeGesture();
}

double EqGraph::displayRate() const noexcept
{
    const double sr = processor.getSampleRate();
    return sr > 8000.0 ? sr : 48000.0;
}

void EqGraph::timerCallback()
{
    // The parameters can change anywhere (knobs, automation, LFOs): follow them.
    params = processor.readParams();
    if (isShowing())
        repaint();
}

void EqGraph::setHitAudio (const juce::AudioBuffer<float>& hit, double sampleRate)
{
    const int len = hit.getNumSamples();
    if (len < 64 || hit.getNumChannels() == 0 || sampleRate <= 0.0)
        return;

    // The whole hit (it decays on its own), zero-padded to a power of two.
    int order = 12;
    while ((1 << order) < len && order < 16)
        ++order;
    const int size = 1 << order;
    std::vector<float> buf ((size_t) size * 2, 0.0f);
    const int used = juce::jmin (len, size);
    for (int c = 0; c < hit.getNumChannels(); ++c)
        for (int i = 0; i < used; ++i)
            buf[(size_t) i] += hit.getSample (c, i) / (float) hit.getNumChannels();
    juce::dsp::FFT fft (order);
    fft.performFrequencyOnlyForwardTransform (buf.data());

    // Onto a 1/48 octave grid from 20 Hz to 20 kHz (the loudest bin in each cell).
    const double binHz = sampleRate / size;
    const int points = (int) std::ceil (48.0 * std::log2 (kMaxHz / kMinHz)) + 1;
    spectrumHz.assign ((size_t) points, 0.0f);
    spectrumDb.assign ((size_t) points, -200.0f);
    spectrumMax = -200.0f;
    for (int k = 0; k < points; ++k)
    {
        const double f = kMinHz * std::exp2 (k / 48.0);
        const double f0 = f * std::exp2 (-0.5 / 48.0), f1 = f * std::exp2 (0.5 / 48.0);
        int b0 = (int) std::floor (f0 / binHz), b1 = (int) std::ceil (f1 / binHz);
        b0 = juce::jlimit (1, size / 2 - 1, b0);
        b1 = juce::jlimit (b0, size / 2 - 1, b1);
        float best = 0.0f;
        for (int b = b0; b <= b1; ++b)
            best = juce::jmax (best, buf[(size_t) b]);
        const float level = 20.0f * std::log10 (best + 1.0e-12f);
        spectrumHz[(size_t) k] = (float) f;
        spectrumDb[(size_t) k] = level;
        spectrumMax = juce::jmax (spectrumMax, level);
    }

    if (! peaks.empty())
        findPeaks(); // they follow the new render
    repaint();
}

void EqGraph::selectBand (int band)
{
    band = juce::jlimit (0, kNumEqBands - 1, band);
    if (band == selected)
        return;
    selected = band;
    repaint();
    if (onBandSelected)
        onBandSelected (band);
}

void EqGraph::setCompact (bool shouldBeCompact)
{
    compact = shouldBeCompact;
    if (compact)
        rangeDb = 18.0f;
    setTooltip (compact ? "Drag a node: frequency and gain. Scroll: Q. Double-click: the full EQ" : juce::String());
    repaint();
}

void EqGraph::setRangeDb (float db)
{
    rangeDb = juce::jlimit (6.0f, 48.0f, db);
    repaint();
}

int EqGraph::findPeaks()
{
    peaks = findResonances (spectrumHz, spectrumDb);
    hoveredPeak = -1;
    repaint();
    return (int) peaks.size();
}

int EqGraph::fixPeak (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) peaks.size()))
        return -1;
    int band = -1;
    for (int b = 0; b < kNumEqBands && band < 0; ++b)
        if (! params.eqBands[(size_t) b].on)
            band = b;
    if (band < 0)
        return -1;

    const auto peak = peaks[(size_t) index];
    processor.getUndoHistory().noteUserActivity(); // the new band is one step
    setBandParam (band, pid::eqBandType, (float) eqBell);
    setBandParam (band, pid::eqBandFreq, peak.hz);
    setBandParam (band, pid::eqBandQ, peak.q);
    setBandParam (band, pid::eqBandGain, -juce::jmin (12.0f, peak.prominenceDb));
    setBandParam (band, pid::eqBandDyn, 0.0f);
    setBandParam (band, pid::eqBandOn, 1.0f);
    params = processor.readParams();
    peaks.erase (peaks.begin() + index);
    selectBand (band);
    repaint();
    return band;
}

//==============================================================================
juce::Rectangle<float> EqGraph::plotArea() const
{
    auto r = getLocalBounds().toFloat();
    r.removeFromLeft (compact ? 26.0f : kRulerLeft);
    r.removeFromRight (compact ? 6.0f : 10.0f);
    r.removeFromTop (compact ? 6.0f : 10.0f);
    r.removeFromBottom (compact ? 13.0f : kAxisBottom);
    return r;
}

float EqGraph::freqToX (float hz) const noexcept
{
    const auto r = plotArea();
    return r.getX() + r.getWidth() * std::log (juce::jmax (1.0f, hz) / kMinHz) / std::log (kMaxHz / kMinHz);
}

float EqGraph::xToFreq (float x) const noexcept
{
    const auto r = plotArea();
    return kMinHz * std::pow (kMaxHz / kMinHz, (x - r.getX()) / r.getWidth());
}

float EqGraph::dbToY (float db) const noexcept
{
    const auto r = plotArea();
    return r.getCentreY() - db / rangeDb * r.getHeight() * 0.5f;
}

float EqGraph::yToDb (float y) const noexcept
{
    const auto r = plotArea();
    return (r.getCentreY() - y) / (r.getHeight() * 0.5f) * rangeDb;
}

juce::Point<float> EqGraph::getNodePosition (int band) const
{
    const auto& b = params.eqBands[(size_t) juce::jlimit (0, kNumEqBands - 1, band)];
    const float x = freqToX (b.freq);
    if (! eqTypeHasGain (b.type))
        return { x, dbToY (0.0f) };
    // A tilt's node sits on its gain at the top end (half of the difference).
    const float db = b.type == eqTilt ? 0.5f * b.gainDb : b.gainDb;
    return { x, juce::jlimit (plotArea().getY(), plotArea().getBottom(), dbToY (db)) };
}

float EqGraph::getCurveDb (float hz) const
{
    double total = 0.0;
    const double sr = displayRate();
    if (! params.eqOn)
        return 0.0f;
    for (const auto& b : params.eqBands)
        if (b.on)
            total += 20.0 * std::log10 (std::max (1.0e-9, ParametricEq::design (b, params.eqStyle, sr).magnitude (hz, sr)));
    return (float) total;
}

int EqGraph::findNode (juce::Point<float> p) const
{
    // The selected band first (it's drawn on top), then the rest.
    int best = -1;
    float bestDist = kNodeRadius + 3.0f;
    for (int i = 0; i < kNumEqBands; ++i)
    {
        const int b = (selected + i) % kNumEqBands;
        if (! params.eqBands[(size_t) b].on)
            continue;
        const float d = p.getDistanceFrom (getNodePosition (b));
        if (d < bestDist)
        {
            bestDist = d;
            best = b;
        }
    }
    return best;
}

juce::Rectangle<float> EqGraph::peakMarkBounds (int index) const
{
    const auto& pk = peaks[(size_t) index];
    return juce::Rectangle<float> (64.0f, 30.0f).withCentre ({ freqToX (pk.hz), plotArea().getY() + 22.0f });
}

int EqGraph::findPeakMark (juce::Point<float> p) const
{
    for (int i = 0; i < (int) peaks.size(); ++i)
        if (peakMarkBounds (i).reduced (12.0f, 0.0f).contains (p))
            return i;
    return -1;
}

float EqGraph::bandParam (int band, const char* suffix) const
{
    const auto* v = processor.getState().getRawParameterValue (eqBandParamId (band, suffix));
    return v != nullptr ? v->load() : 0.0f;
}

void EqGraph::setBandParam (int band, const char* suffix, float plainValue)
{
    if (auto* param = processor.getState().getParameter (eqBandParamId (band, suffix)))
    {
        const bool inGesture = std::find (gestureParams.begin(), gestureParams.end(), param) != gestureParams.end();
        if (! inGesture)
            param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
        if (! inGesture)
            param->endChangeGesture();
    }
}

void EqGraph::startListen (float hz, float q)
{
    listening = true;
    processor.setEqListen (juce::jlimit (kMinHz, kMaxHz, hz), q);
}

void EqGraph::stopListen()
{
    if (listening)
        processor.setEqListen (0.0f);
    listening = false;
}

int EqGraph::addBandAt (juce::Point<float> position)
{
    int band = -1;
    for (int b = 0; b < kNumEqBands && band < 0; ++b)
        if (! params.eqBands[(size_t) b].on)
            band = b;
    if (band < 0)
        return -1;

    const float hz = juce::jlimit (kMinHz, kMaxHz, xToFreq (position.x));
    const int type = hz < 30.0f ? eqLowCut : hz > 16000.0f ? eqHighCut : eqBell;
    processor.getUndoHistory().noteUserActivity();
    setBandParam (band, pid::eqBandType, (float) type);
    setBandParam (band, pid::eqBandFreq, type == eqLowCut ? juce::jmax (30.0f, hz) : type == eqHighCut ? juce::jmin (16000.0f, hz) : hz);
    setBandParam (band, pid::eqBandGain, type == eqBell ? juce::jlimit (-24.0f, 24.0f, yToDb (position.y)) : 0.0f);
    setBandParam (band, pid::eqBandQ, type == eqBell ? 1.0f : 0.70710678f);
    setBandParam (band, pid::eqBandDyn, 0.0f);
    setBandParam (band, pid::eqBandOn, 1.0f);
    params = processor.readParams();
    selectBand (band);
    repaint();
    return band;
}

//==============================================================================
void EqGraph::mouseMove (const juce::MouseEvent& e)
{
    const int node = findNode (e.position);
    const int mark = node < 0 ? findPeakMark (e.position) : -1;
    if (node != hovered || mark != hoveredPeak)
    {
        hovered = node;
        hoveredPeak = mark;
        setMouseCursor (node >= 0 || mark >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::CrosshairCursor);
        repaint();
    }
}

void EqGraph::mouseExit (const juce::MouseEvent&)
{
    hovered = hoveredPeak = -1;
    repaint();
}

void EqGraph::mouseDown (const juce::MouseEvent& e)
{
    const int node = findNode (e.position);

    if (compact && (e.mods.isPopupMenu() || e.mods.isAltDown()))
        return;
    if (node >= 0 && e.mods.isPopupMenu())
    {
        selectBand (node);
        showBandMenu (node);
        return;
    }

    if (node < 0)
    {
        if (const int mark = findPeakMark (e.position); mark >= 0 && ! e.mods.isPopupMenu())
        {
            fixPeak (mark);
            return;
        }
        if (e.mods.isAltDown())
        {
            sweeping = true;
            startListen (xToFreq (e.position.x), kListenQ);
            repaint();
        }
        return;
    }

    selectBand (node);
    dragging = node;
    dragStart = e.position;
    const auto& b = params.eqBands[(size_t) node];
    dragStartFreqX = freqToX (b.freq);
    dragStartDb = b.gainDb;
    for (auto* suffix : { pid::eqBandFreq, pid::eqBandGain })
        if (auto* param = processor.getState().getParameter (eqBandParamId (node, suffix)))
        {
            param->beginChangeGesture();
            gestureParams.push_back (param);
        }
    if (e.mods.isAltDown())
        startListen (b.freq, juce::jmax (1.0f, b.q));
    repaint();
}

void EqGraph::mouseDrag (const juce::MouseEvent& e)
{
    if (sweeping)
    {
        startListen (xToFreq (e.position.x), kListenQ);
        repaint();
        return;
    }
    if (dragging < 0)
        return;

    const float scale = e.mods.isShiftDown() ? 0.15f : 1.0f;
    const auto& b = params.eqBands[(size_t) dragging];
    const float x = dragStartFreqX + (e.position.x - dragStart.x) * scale;
    const float hz = juce::jlimit (kMinHz, kMaxHz, xToFreq (x));
    setBandParam (dragging, pid::eqBandFreq, hz);
    if (eqTypeHasGain (b.type))
    {
        float db = dragStartDb + (yToDb (e.position.y) - yToDb (dragStart.y)) * scale * (b.type == eqTilt ? 2.0f : 1.0f);
        setBandParam (dragging, pid::eqBandGain, juce::jlimit (-24.0f, 24.0f, db));
    }
    params = processor.readParams();
    if (listening)
        startListen (hz, juce::jmax (1.0f, b.q));
    repaint();
}

void EqGraph::mouseUp (const juce::MouseEvent&)
{
    for (auto* param : gestureParams)
        param->endChangeGesture();
    gestureParams.clear();
    dragging = -1;
    sweeping = false;
    stopListen();
    repaint();
}

void EqGraph::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (compact)
    {
        if (onOpenFull)
            onOpenFull();
        return;
    }
    if (const int node = findNode (e.position); node >= 0)
    {
        processor.getUndoHistory().noteUserActivity();
        setBandParam (node, pid::eqBandOn, 0.0f);
        params = processor.readParams();
        repaint();
        return;
    }
    if (plotArea().contains (e.position) && findPeakMark (e.position) < 0)
        addBandAt (e.position);
}

void EqGraph::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    int band = findNode (e.position);
    if (band < 0)
        return;
    const float dy = (std::abs (w.deltaY) >= std::abs (w.deltaX) ? w.deltaY : w.deltaX) * (w.isReversed ? -1.0f : 1.0f);
    if (std::abs (dy) < 1.0e-4f)
        return;
    processor.getUndoHistory().noteUserActivity();
    const auto& b = params.eqBands[(size_t) band];
    if (eqTypeHasSlope (b.type) && ! e.mods.isShiftDown())
    {
        const int slope = juce::jlimit (0, eqSlopeNames.size() - 1, b.slope + (dy > 0.0f ? 1 : -1));
        setBandParam (band, pid::eqBandSlope, (float) slope);
    }
    else
    {
        setBandParam (band, pid::eqBandQ, juce::jlimit (0.1f, 18.0f, b.q * std::exp2 (dy * 2.0f)));
    }
    params = processor.readParams();
    repaint();
}

void EqGraph::showBandMenu (int band)
{
    const auto& b = params.eqBands[(size_t) band];
    juce::PopupMenu menu;
    menu.addSectionHeader ("Band " + juce::String (band + 1));
    for (int t = 0; t < eqTypeNames.size(); ++t)
        menu.addItem (100 + t, eqTypeNames[t], true, b.type == t);
    if (eqTypeHasSlope (b.type))
    {
        juce::PopupMenu slopes;
        for (int s = 0; s < eqSlopeNames.size(); ++s)
            slopes.addItem (200 + s, eqSlopeNames[s] + "/oct", true, b.slope == s);
        menu.addSubMenu ("Slope", slopes);
    }
    menu.addSeparator();
    menu.addItem (300, "Dynamic", eqTypeHasGain (b.type), b.dynamic);
    menu.addItem (301, "Turn off");

    juce::Component::SafePointer<EqGraph> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), [safe, band] (int result)
    {
        if (safe == nullptr || result == 0)
            return;
        auto& g = *safe;
        g.processor.getUndoHistory().noteUserActivity();
        if (result >= 100 && result < 100 + eqTypeNames.size())
            g.setBandParam (band, pid::eqBandType, (float) (result - 100));
        else if (result >= 200 && result < 200 + eqSlopeNames.size())
            g.setBandParam (band, pid::eqBandSlope, (float) (result - 200));
        else if (result == 300)
            g.setBandParam (band, pid::eqBandDyn, g.bandParam (band, pid::eqBandDyn) >= 0.5f ? 0.0f : 1.0f);
        else if (result == 301)
            g.setBandParam (band, pid::eqBandOn, 0.0f);
        g.params = g.processor.readParams();
        g.repaint();
    });
}

//==============================================================================
void EqGraph::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto plot = plotArea();
    const double sr = displayRate();

    g.setColour (colours::display);
    g.fillRoundedRectangle (bounds, 6.0f);

    // --- Grid -------------------------------------------------------------------------
    g.setFont (uiFont (10.5f));
    for (float hz : { 20.0f, 30.0f, 40.0f, 50.0f, 60.0f, 80.0f, 100.0f, 200.0f, 300.0f, 400.0f, 500.0f, 600.0f, 800.0f,
                      1000.0f, 2000.0f, 3000.0f, 4000.0f, 5000.0f, 6000.0f, 8000.0f, 10000.0f, 20000.0f })
    {
        const float x = freqToX (hz);
        const bool major = hz == 100.0f || hz == 1000.0f || hz == 10000.0f;
        g.setColour (major ? colours::gridStrong : colours::grid);
        g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
        const bool label = compact ? (major && hz != 10000.0f) || hz == 10000.0f
                                   : major || hz == 20.0f || hz == 50.0f || hz == 200.0f || hz == 500.0f || hz == 2000.0f || hz == 5000.0f || hz == 20000.0f;
        if (label)
        {
            g.setColour (colours::textDim);
            const bool last = hz == 20000.0f, first = hz == 20.0f;
            g.drawText (shortHz (hz), juce::Rectangle<float> (last ? x - 40.0f : first ? x : x - 20.0f, plot.getBottom() + 2.0f, 40.0f, kAxisBottom - 4.0f),
                        last ? juce::Justification::centredRight : first ? juce::Justification::centredLeft : juce::Justification::centred);
        }
    }
    const float step = compact ? 6.0f : rangeDb > 24.0f ? 12.0f : rangeDb > 12.0f ? 6.0f : 3.0f;
    for (float db = -rangeDb; db <= rangeDb + 0.01f; db += step)
    {
        const float y = dbToY (db);
        g.setColour (std::abs (db) < 0.01f ? colours::textDim.withAlpha (0.6f) : colours::grid);
        g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
        g.setColour (colours::textDim);
        if (! compact || std::abs (db) < 0.01f || std::abs (std::abs (db) - 12.0f) < 0.01f)
            g.drawText ((db > 0.0f ? "+" : "") + juce::String (juce::roundToInt (db)),
                        juce::Rectangle<float> (2.0f, y - 7.0f, (compact ? 26.0f : kRulerLeft) - 6.0f, 14.0f), juce::Justification::centredRight);
    }

    juce::Graphics::ScopedSaveState clip (g);
    g.reduceClipRegion (plot.toNearestInt());

    // --- The hit's spectrum -----------------------------------------------------------
    if (! spectrumDb.empty() && spectrumMax > -150.0f)
    {
        juce::Path spec;
        spec.startNewSubPath (plot.getX(), plot.getBottom());
        for (size_t i = 0; i < spectrumDb.size(); ++i)
        {
            const float rel = juce::jlimit (kSpectrumFloorDb, 0.0f, spectrumDb[i] - spectrumMax);
            spec.lineTo (freqToX (spectrumHz[i]), plot.getBottom() - (1.0f - rel / kSpectrumFloorDb) * plot.getHeight() * 0.92f);
        }
        spec.lineTo (plot.getRight(), plot.getBottom());
        spec.closeSubPath();
        g.setColour (colours::waveform.withAlpha (0.55f));
        g.fillPath (spec);
    }

    // --- Listen -------------------------------------------------------------------------
    if (const float lh = processor.getEqListenHz(); lh > 0.0f)
    {
        const float x = freqToX (lh);
        g.setColour (colours::solo.withAlpha (0.12f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (freqToX (lh / 1.19f), plot.getY(), freqToX (lh * 1.19f), plot.getBottom()));
        g.setColour (colours::solo.withAlpha (0.8f));
        g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
        g.setFont (uiFont (11.0f, true));
        g.drawText ("LISTENING  " + formatHz (lh), juce::Rectangle<float> (x + 6.0f, plot.getY() + 4.0f, 160.0f, 14.0f),
                    juce::Justification::centredLeft);
    }

    // --- Each band's curve, then the whole EQ's --------------------------------------------
    const int points = juce::jmax (64, (int) plot.getWidth() / 2);
    std::vector<float> total ((size_t) points + 1, 0.0f);
    for (int k = 0; k < kNumEqBands; ++k)
    {
        const auto& b = params.eqBands[(size_t) k];
        if (! b.on)
            continue;
        const auto d = ParametricEq::design (b, params.eqStyle, sr);
        juce::Path curve;
        for (int i = 0; i <= points; ++i)
        {
            const float x = plot.getX() + plot.getWidth() * (float) i / (float) points;
            const float db = (float) (20.0 * std::log10 (std::max (1.0e-9, d.magnitude (xToFreq (x), sr))));
            total[(size_t) i] += db;
            const float y = juce::jlimit (plot.getY() - 2.0f, plot.getBottom() + 2.0f, dbToY (db));
            if (i == 0) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
        }
        const auto colour = eqBandColour (k);
        const bool isSelected = k == selected;
        juce::Path fill (curve);
        fill.lineTo (plot.getRight(), dbToY (0.0f));
        fill.lineTo (plot.getX(), dbToY (0.0f));
        fill.closeSubPath();
        g.setColour (colour.withAlpha (isSelected ? 0.16f : 0.07f));
        g.fillPath (fill);
        g.setColour (colour.withAlpha (isSelected ? 0.9f : 0.45f));
        g.strokePath (curve, juce::PathStrokeType (isSelected ? 1.6f : 1.0f));

        // A dynamic band: the curve at the gain it's applying now, dashed.
        if (b.dynamic && eqTypeHasGain (b.type))
        {
            const float now = processor.getEqAppliedGainDb (k);
            const auto dn = ParametricEq::design (b, params.eqStyle, sr, now);
            juce::Path live;
            for (int i = 0; i <= points; ++i)
            {
                const float x = plot.getX() + plot.getWidth() * (float) i / (float) points;
                const float y = dbToY ((float) (20.0 * std::log10 (std::max (1.0e-9, dn.magnitude (xToFreq (x), sr)))));
                if (i == 0) live.startNewSubPath (x, y); else live.lineTo (x, y);
            }
            juce::Path dashed;
            const float dashes[] { 4.0f, 3.0f };
            juce::PathStrokeType (1.4f).createDashedStroke (dashed, live, dashes, 2);
            g.setColour (colour);
            g.fillPath (dashed);
        }
    }
    if (params.eqOn)
    {
        juce::Path curve;
        for (int i = 0; i <= points; ++i)
        {
            const float x = plot.getX() + plot.getWidth() * (float) i / (float) points;
            const float y = juce::jlimit (plot.getY() - 2.0f, plot.getBottom() + 2.0f, dbToY (total[(size_t) i]));
            if (i == 0) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
        }
        g.setColour (colours::text.withAlpha (0.9f));
        g.strokePath (curve, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // --- Peak marks ----------------------------------------------------------------------
    for (int i = 0; i < (int) peaks.size(); ++i)
    {
        const auto r = peakMarkBounds (i);
        const bool hot = i == hoveredPeak;
        juce::Path tri;
        const float cx = r.getCentreX(), ty = r.getBottom() - 9.0f;
        tri.addTriangle (cx - 5.0f, ty, cx + 5.0f, ty, cx, ty + 7.0f);
        g.setColour (colours::mute.withAlpha (hot ? 1.0f : 0.8f));
        g.fillPath (tri);
        g.setFont (uiFont (10.0f, true));
        g.drawText (formatHz (peaks[(size_t) i].hz), r.withHeight (12.0f), juce::Justification::centred);
        g.setColour (colours::mute.withAlpha (hot ? 0.7f : 0.3f));
        g.drawVerticalLine (juce::roundToInt (cx), ty + 7.0f, plot.getBottom());
    }

    // --- Nodes (the selected one on top) --------------------------------------------------
    for (int i = 1; i <= kNumEqBands; ++i)
    {
        const int k = (selected + i) % kNumEqBands;
        const auto& b = params.eqBands[(size_t) k];
        if (! b.on)
            continue;
        const auto pos = getNodePosition (k);
        const auto colour = eqBandColour (k);
        const bool hot = k == hovered || k == dragging;
        const float r = (compact ? 6.5f : kNodeRadius) * (hot ? 1.15f : 1.0f);
        g.setColour (k == selected ? colour : colours::display);
        g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (pos));
        g.setColour (colour);
        g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (pos), k == selected ? 2.2f : 1.6f);
        g.setFont (uiFont (compact ? 9.0f : 11.0f, true));
        g.setColour (k == selected ? colours::background : colour);
        g.drawText (juce::String (k + 1), juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (pos), juce::Justification::centred);

        if (b.dynamic && eqTypeHasGain (b.type))
        {
            // Where the dynamic gain is now.
            const float now = processor.getEqAppliedGainDb (k);
            const float y = dbToY (b.type == eqTilt ? 0.5f * now : now);
            g.setColour (colour.withAlpha (0.8f));
            g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ pos.x, y }));
        }
    }

    // --- Readout ---------------------------------------------------------------------------
    {
        juce::String text;
        const int show = dragging >= 0 ? dragging : hovered;
        if (show >= 0)
        {
            const auto& b = params.eqBands[(size_t) show];
            text << "Band " << (show + 1) << "  " << eqTypeNames[b.type] << "  " << formatHz (b.freq);
            if (eqTypeHasGain (b.type))
                text << "  " << formatDb (b.gainDb);
            if (eqTypeHasSlope (b.type))
                text << "  " << eqSlopeNames[b.slope] << "/oct";
            if (eqTypeHasQ (b.type))
                text << "  Q " << juce::String (b.q, 2);
            if (b.dynamic && eqTypeHasGain (b.type))
                text << "  dynamic, now " << formatDb (processor.getEqAppliedGainDb (show));
        }
        else if (hoveredPeak >= 0)
        {
            const auto& pk = peaks[(size_t) hoveredPeak];
            text << "Resonance at " << formatHz (pk.hz) << ", " << juce::String (pk.prominenceDb, 1)
                 << " dB over its surroundings. Click: notch it with a free band";
        }
        else if (! compact)
        {
            text = "Drag a node: frequency and gain   Scroll: Q   Double-click: add a band / turn one off   Alt-drag: listen   Right-click: type";
        }
        g.setFont (uiFont (11.0f, show >= 0 || hoveredPeak >= 0));
        g.setColour (show >= 0 ? eqBandColour (show) : hoveredPeak >= 0 ? colours::mute : colours::textDim);
        g.drawText (text, plot.reduced (10.0f, 6.0f).removeFromBottom (14.0f), juce::Justification::centredRight, true);
    }
}

//==============================================================================
EqBandEditor::EqBandEditor (DrumFoundryProcessor& p, int b)
    : processor (p), band (b),
      on (p.getState(), eqBandParamId (b, pid::eqBandOn), "ON", eqBandColour (b), "Switch band " + juce::String (b + 1) + " on or off"),
      type (p.getState(), eqBandParamId (b, pid::eqBandType), "Type", eqBandColour (b)),
      slope (p.getState(), eqBandParamId (b, pid::eqBandSlope), "Slope", eqBandColour (b)),
      freq (p.getState(), eqBandParamId (b, pid::eqBandFreq), "Freq", eqBandColour (b)),
      gain (p.getState(), eqBandParamId (b, pid::eqBandGain), "Gain", eqBandColour (b), true),
      q (p.getState(), eqBandParamId (b, pid::eqBandQ), "Q", eqBandColour (b)),
      threshold (p.getState(), eqBandParamId (b, pid::eqBandThresh), "Threshold", eqBandColour (b)),
      attack (p.getState(), eqBandParamId (b, pid::eqBandAttack), "Attack", eqBandColour (b)),
      release (p.getState(), eqBandParamId (b, pid::eqBandRelease), "Release", eqBandColour (b)),
      dynamic (p.getState(), eqBandParamId (b, pid::eqBandDyn), "DYNAMIC", eqBandColour (b),
               "Dynamic: the band only reaches its Gain while its frequencies are loud: 0 dB at Threshold, "
               "the full Gain 10 dB above it, following Attack and Release")
{
    threshold.getSlider().setTooltip ("Threshold: the band's level where the dynamic gain starts");
    attack.getSlider().setTooltip ("Attack: how fast the dynamic gain moves in");
    release.getSlider().setTooltip ("Release: how fast it lets go");
    q.getSlider().setTooltip ("Q: the band's width (narrower as it goes up). On a cut, a resonant peak at the corner above 0.71");
    listen.setTooltip ("Hold to listen to this band's frequencies alone (also Alt-drag its node)");
    listen.setColour (juce::TextButton::buttonColourId, colours::panel);
    listen.onStateChange = [this]
    {
        const bool down = listen.isDown();
        if (down)
        {
            const auto* f = processor.getState().getRawParameterValue (eqBandParamId (band, pid::eqBandFreq));
            const auto* qv = processor.getState().getRawParameterValue (eqBandParamId (band, pid::eqBandQ));
            processor.setEqListen (f != nullptr ? f->load() : 1000.0f, juce::jmax (1.0f, qv != nullptr ? qv->load() : 1.0f));
        }
        else if (processor.getEqListenHz() > 0.0f)
        {
            processor.setEqListen (0.0f);
        }
    };

    for (auto* c : std::initializer_list<juce::Component*> { &on, &type, &slope, &freq, &gain, &q, &dynamic, &threshold, &attack, &release, &listen })
        addAndMakeVisible (c);
    update();
}

void EqBandEditor::update()
{
    auto& state = processor.getState();
    const auto raw = [&state, this] (const char* suffix)
    {
        const auto* v = state.getRawParameterValue (eqBandParamId (band, suffix));
        return v != nullptr ? juce::roundToInt (v->load()) : 0;
    };
    const int t = juce::jlimit (0, numEqTypes - 1, raw (pid::eqBandType));
    const int dyn = raw (pid::eqBandDyn);
    if (t == shownType && dyn == shownDynamic)
        return;
    shownType = t;
    shownDynamic = dyn;

    const bool hasGain = eqTypeHasGain (t);
    gain.setAlpha (hasGain ? 1.0f : 0.35f);
    gain.setCaption (t == eqTilt ? "Tilt" : "Gain", t == eqTilt ? "Tilt: the top end against the bottom end (half each way)" : juce::String());
    q.setAlpha (eqTypeHasQ (t) ? 1.0f : 0.35f);
    q.setCaption (eqTypeHasSlope (t) ? "Resonance" : "Q");
    slope.setVisible (eqTypeHasSlope (t));
    dynamic.setAlpha (hasGain ? 1.0f : 0.35f);
    const float dynAlpha = hasGain && dyn != 0 ? 1.0f : 0.35f;
    for (auto* k : { &threshold, &attack, &release })
        k->setAlpha (dynAlpha);
    resized();
}

void EqBandEditor::paint (juce::Graphics& g)
{
    g.setColour (eqBandColour (band));
    g.setFont (uiFont (13.0f, true));
    g.drawText ("BAND " + juce::String (band + 1), getLocalBounds().removeFromLeft (80).removeFromTop (28), juce::Justification::centredLeft);
    // A divider before the dynamics.
    g.setColour (colours::panelBorder);
    g.drawVerticalLine (threshold.getX() - 10, 12.0f, (float) getHeight() - 10.0f);
}

void EqBandEditor::resized()
{
    auto r = getLocalBounds();
    auto left = r.removeFromLeft (110);
    left.removeFromTop (30);
    on.setBounds (left.removeFromTop (26).reduced (0, 1).withWidth (70));
    left.removeFromTop (8);
    listen.setBounds (left.removeFromTop (26).reduced (0, 1).withWidth (70));

    auto choices = r.removeFromLeft (120);
    type.setBounds (choices.removeFromTop (choices.getHeight() / 2));
    if (slope.isVisible())
        slope.setBounds (choices);
    r.removeFromLeft (8);

    const int knob = juce::jmin (90, r.getWidth() / 7);
    for (auto* k : { &freq, &gain, &q })
        k->setBounds (r.removeFromLeft (knob));
    r.removeFromLeft (20);
    auto dynColumn = r.removeFromLeft (100);
    dynamic.setBounds (dynColumn.withSizeKeepingCentre (96, 26));
    for (auto* k : { &threshold, &attack, &release })
        k->setBounds (r.removeFromLeft (knob));
}

//==============================================================================
/** A band's tab: its number and type, lit in its colour while it's on. Click selects it;
    double-click switches it on or off. */
class EqOverlay::BandTab : public juce::Button
{
public:
    BandTab (EqOverlay& o, int b) : juce::Button ("Band " + juce::String (b + 1)), overlay (o), band (b)
    {
        setTooltip ("Band " + juce::String (b + 1) + ": click to edit it, double-click to switch it on or off");
    }

    void setState (bool isOn, int type, bool isSelected)
    {
        if (isOn == on && type == shownType && isSelected == selected)
            return;
        on = isOn;
        shownType = type;
        selected = isSelected;
        repaint();
    }

    void clicked() override { overlay.selectBand (band); }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        overlay.processor.getUndoHistory().noteUserActivity();
        if (auto* param = overlay.processor.getState().getParameter (eqBandParamId (band, pid::eqBandOn)))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (on ? 0.0f : 1.0f);
            param->endChangeGesture();
        }
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto colour = eqBandColour (band);
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (selected ? colour.withAlpha (down ? 0.35f : 0.25f) : (highlighted ? colours::panelBorder : colours::panel));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (on ? colour : colours::panelBorder);
        g.drawRoundedRectangle (r, 4.0f, selected ? 1.6f : 1.0f);
        g.setFont (uiFont (11.5f, true));
        g.setColour (on ? colour : colours::textDim);
        g.drawFittedText (juce::String (band + 1) + "  " + (on ? eqTypeNames[shownType].toUpperCase() : juce::String ("OFF")),
                          getLocalBounds().reduced (6, 0), juce::Justification::centred, 1, 0.85f);
    }

private:
    EqOverlay& overlay;
    const int band;
    bool on = false, selected = false;
    int shownType = 0;
};

EqOverlay::EqOverlay (DrumFoundryProcessor& p)
    : processor (p), power (p.getState(), pid::eqOn, colours::eq, "EQ"),
      style (p.getState(), pid::eqStyle, {}, colours::eq), graph (p)
{
    style.getComboBox().setTooltip ("Style:\n"
                                    "Clean: analog-matched curves (no squashing near 20 kHz)\n"
                                    "Digital: the classic digital EQ (the old 3-band EQ's sound)\n"
                                    "British: console-style, bells narrow as they boost or cut harder\n"
                                    "Pultec: resonant shelves (a dip next to the boost) and broad bells");
    range.setTooltip ("The graph's gain range");
    range.onClick = [this]
    {
        const float next = graph.getRangeDb() >= 36.0f ? 12.0f : graph.getRangeDb() + 12.0f;
        graph.setRangeDb (next);
        range.setButtonText (juce::String (juce::roundToInt (next)) + " dB");
    };
    findPeaks.setTooltip ("Marks the hit's resonances (narrow peaks). Click a mark to notch it with a free band");
    findPeaks.onClick = [this] { graph.findPeaks(); };
    closeButton.setTooltip ("Back to the main view (Esc)");
    closeButton.onClick = [this] { if (onClose) onClose(); };

    for (auto* c : std::initializer_list<juce::Component*> { &power, &style, &range, &findPeaks, &closeButton, &graph })
        addAndMakeVisible (c);

    for (int b = 0; b < kNumEqBands; ++b)
    {
        bandTabs[(size_t) b] = std::make_unique<BandTab> (*this, b);
        addAndMakeVisible (*bandTabs[(size_t) b]);
        editors[(size_t) b] = std::make_unique<EqBandEditor> (p, b);
        addChildComponent (*editors[(size_t) b]);
    }
    graph.onBandSelected = [this] (int b) { selectBand (b); };
    selectBand (graph.getSelectedBand());

    setWantsKeyboardFocus (true);
    setVisible (false);
}

juce::Button& EqOverlay::getBandTab (int band)
{
    return *bandTabs[(size_t) juce::jlimit (0, kNumEqBands - 1, band)];
}

EqOverlay::~EqOverlay()
{
    stopTimer();
}

void EqOverlay::visibilityChanged()
{
    if (isVisible())
        startTimerHz (15);
    else
        stopTimer();
}

void EqOverlay::timerCallback()
{
    const auto params = processor.readParams();
    for (int b = 0; b < kNumEqBands; ++b)
    {
        const auto& band = params.eqBands[(size_t) b];
        bandTabs[(size_t) b]->setState (band.on, band.type, b == graph.getSelectedBand());
        editors[(size_t) b]->update();
    }
}

void EqOverlay::selectBand (int band)
{
    band = juce::jlimit (0, kNumEqBands - 1, band);
    graph.selectBand (band);
    for (int b = 0; b < kNumEqBands; ++b)
        editors[(size_t) b]->setVisible (b == band);
    timerCallback();
}

juce::Rectangle<int> EqOverlay::boxBounds() const
{
    return getLocalBounds().reduced (14);
}

void EqOverlay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.7f));
    const auto box = boxBounds().toFloat();
    g.setColour (colours::panel);
    g.fillRoundedRectangle (box, 8.0f);
    g.setColour (colours::panelBorder);
    g.drawRoundedRectangle (box.reduced (0.5f), 8.0f, 1.0f);

    g.setFont (titleFont (15.0f));
    g.setColour (colours::eq);
    g.drawText ("EQ", titleArea, juce::Justification::centredLeft);
    g.setFont (uiFont (11.5f));
    g.setColour (colours::textDim);
    g.drawText ("STYLE", hintArea, juce::Justification::centredRight);
}

void EqOverlay::resized()
{
    auto box = boxBounds().reduced (16, 12);

    auto top = box.removeFromTop (30);
    power.setBounds (top.removeFromLeft (22).withSizeKeepingCentre (20, 20));
    top.removeFromLeft (8);
    titleArea = top.removeFromLeft (40);
    closeButton.setBounds (top.removeFromRight (70).reduced (0, 2));
    top.removeFromRight (12);
    findPeaks.setBounds (top.removeFromRight (100).reduced (0, 2));
    top.removeFromRight (8);
    range.setBounds (top.removeFromRight (64).reduced (0, 2));
    top.removeFromRight (16);
    style.setBounds (top.removeFromRight (110).reduced (0, 2));
    hintArea = top.removeFromRight (50);

    box.removeFromTop (10);
    auto tabs = box.removeFromTop (28);
    const int tabW = juce::jmin (130, (tabs.getWidth() - 7 * 6) / kNumEqBands);
    for (auto& t : bandTabs)
    {
        t->setBounds (tabs.removeFromLeft (tabW));
        tabs.removeFromLeft (6);
    }

    box.removeFromTop (10);
    auto bottom = box.removeFromBottom (120);
    for (auto& e : editors)
        e->setBounds (bottom);
    box.removeFromBottom (10);
    graph.setBounds (box);
}

void EqOverlay::mouseDown (const juce::MouseEvent& e)
{
    if (e.eventComponent == this && ! boxBounds().contains (e.getPosition()) && onClose)
        onClose();
}

bool EqOverlay::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        if (onClose)
            onClose();
        return true;
    }
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        processor.getUndoHistory().noteUserActivity();
        if (auto* param = processor.getState().getParameter (eqBandParamId (graph.getSelectedBand(), pid::eqBandOn)))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (0.0f);
            param->endChangeGesture();
        }
        return true;
    }
    return false;
}

} // namespace drum::ui
