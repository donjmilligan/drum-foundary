// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "SpectrumAnalyzer.h"
#include "FoundryLookAndFeel.h"
#include "../dsp/OutputFilter.h"

namespace drum::ui
{

namespace
{
    constexpr float kMinHz = 20.0f, kMaxHz = 20000.0f;
    constexpr float kMinDb = -90.0f, kMaxDb = 6.0f;
    constexpr float kFloorDb = -200.0f;

    constexpr int kFrameHz = 30;
    constexpr float kFrameSeconds = 1.0f / (float) kFrameHz;
    constexpr float kFallDbPerSecond = 40.0f;   // spectrum release
    constexpr float kHoldSeconds = 1.5f;        // peak-hold line, then...
    constexpr float kHoldFallDbPerSecond = 20.0f;
    constexpr float kMeterFallDbPerSecond = 30.0f;
    constexpr float kRmsSeconds = 0.3f;
    constexpr float kIdleSeconds = 1.0f;        // LIVE shows the hit after this long without signal
    constexpr float kReadoutFloorDb = -80.0f;
    constexpr float kReadoutFallDbPerSecond = 12.0f;

    // HIT: the first 200 ms of the hit, flat for 100 ms and then a cosine taper.
    constexpr double kHitFlatSeconds = 0.1, kHitSeconds = 0.2;

    constexpr float kHeaderHeight = 26.0f;
    constexpr float kDbLabelWidth = 32.0f;
    constexpr float kFreqLabelHeight = 16.0f;
    constexpr float kMeterWidth = 26.0f;

    const juce::Colour kFilterColour = colours::pitch;
    const juce::Colour kLiveColour = colours::body;
    const juce::Colour kHitColour = colours::trigger;

    juce::String formatHz (float hz)
    {
        if (hz >= 1000.0f)
            return juce::String (hz / 1000.0f, hz >= 10000.0f ? 1 : 2) + " kHz";
        return (hz < 100.0f ? juce::String (hz, 1) : juce::String (juce::roundToInt (hz))) + " Hz";
    }

    juce::String formatFreqLabel (float hz)
    {
        return hz >= 1000.0f ? juce::String (juce::roundToInt (hz / 1000.0f)) + "k" : juce::String (juce::roundToInt (hz));
    }

    /** dB of a magnitude, with a floor instead of -inf. */
    inline float toDb (float magnitude) noexcept
    {
        return magnitude > 1.0e-10f ? 20.0f * std::log10 (magnitude) : kFloorDb;
    }
}

//==============================================================================
SpectrumAnalyzer::SpectrumAnalyzer (DrumFoundryProcessor& p)
    : tap (p.getAnalyzerTap()),
      pullLeft ((size_t) AnalyzerTap::capacity),
      pullRight ((size_t) AnalyzerTap::capacity)
{
    auto& state = p.getState();
    hpParam = state.getParameter (pid::outHP);
    hpSlopeParam = state.getParameter (pid::outHPSlope);
    lpParam = state.getParameter (pid::outLP);
    lpSlopeParam = state.getParameter (pid::outLPSlope);
    filter = readFilter();

    const auto setUpButton = [this] (juce::TextButton& b, juce::Colour accent, const juce::String& tip)
    {
        b.getProperties().set ("tab", true);
        b.setColour (juce::TextButton::buttonOnColourId, accent);
        b.setTooltip (tip);
        addAndMakeVisible (b);
    };
    setUpButton (liveButton, kLiveColour, "The output as it plays. Without signal for a second, the last hit is shown instead.");
    setUpButton (hitButton, kHitColour, "The spectrum of the whole hit (its first 200 ms), rendered offline: no need to play it.");
    liveButton.onClick = [this]
    {
        // Clicking LIVE while the hit stands in for a silent output means "really show the output".
        if (showingHit && mode == Mode::live)
            liveSuppressesAuto = true;
        setMode (Mode::live);
    };
    hitButton.onClick = [this] { setMode (Mode::hit); };

    // Whatever piled up while nobody was reading is stale.
    tap.discard();
    configureLive (tap.sampleRate.load());
    updateModeButtons();
    startTimerHz (kFrameHz);
}

SpectrumAnalyzer::~SpectrumAnalyzer()
{
    stopTimer();
    if (dragging != Handle::none)
        if (auto* param = cutoffParam (dragging))
            param->endChangeGesture();
}

//==============================================================================
void SpectrumAnalyzer::configureLive (double sampleRate)
{
    if (sampleRate <= 0.0)
        sampleRate = 48000.0;
    liveRate = sampleRate;

    // About the same resolution in Hz at any rate: 8192 points at 44.1/48 kHz.
    fftOrder = sampleRate <= 50000.0 ? 13 : (sampleRate <= 100000.0 ? 14 : 15);
    const int n = 1 << fftOrder;
    fft = std::make_unique<juce::dsp::FFT> (fftOrder);

    window.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) n);

    fftData.assign ((size_t) (2 * n), 0.0f);
    history.assign ((size_t) n, 0.0f);
    historyPos = 0;

    live.db.assign ((size_t) (n / 2 + 1), kFloorDb);
    live.sampleRate = sampleRate;
    live.fftSize = n;
    liveReadout = {};
    readoutLevel = kFloorDb;
}

void SpectrumAnalyzer::setHitAudio (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    const int total = audio.getNumSamples();
    if (total <= 0 || audio.getNumChannels() == 0 || sampleRate <= 0.0)
    {
        hasHit = false;
        return;
    }

    const int len = juce::jmin (total, (int) std::lround (kHitSeconds * sampleRate));
    const int flat = juce::jmin (len / 2, (int) std::lround (kHitFlatSeconds * sampleRate));

    // Zero-padded to ~340 ms at 48 kHz, for a fine grid to read peaks from.
    const int order = juce::jlimit (12, 16, (int) std::ceil (std::log2 (juce::jmax ((double) len, sampleRate * 0.3))));
    const int n = 1 << order;
    if (hitFft == nullptr || hitFft->getSize() != n)
        hitFft = std::make_unique<juce::dsp::FFT> (order);
    auto& data = hitData;
    data.assign ((size_t) (2 * n), 0.0f);

    const float* l = audio.getReadPointer (0);
    const float* r = audio.getReadPointer (juce::jmin (1, audio.getNumChannels() - 1));
    double windowSum = 0.0;
    for (int i = 0; i < len; ++i)
    {
        const float w = i < flat ? 1.0f
                                 : 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (i - flat) / (float) (len - flat));
        data[(size_t) i] = 0.5f * (l[i] + r[i]) * w;
        windowSum += w;
    }
    hitFft->performFrequencyOnlyForwardTransform (data.data(), true);

    // Normalised so a steady sine reads its amplitude, as in LIVE; a decaying hit
    // reads its average level over the window.
    const float scale = (float) (2.0 / juce::jmax (1.0, windowSum));
    hit.db.resize ((size_t) (n / 2 + 1));
    for (int k = 0; k <= n / 2; ++k)
        hit.db[(size_t) k] = toDb (data[(size_t) k] * scale);
    hit.sampleRate = sampleRate;
    hit.fftSize = n;
    hasHit = true;

    hitReadout = findPeak (hit);
    mapToColumns (hit, hitColumns);
    repaint();
}

void SpectrumAnalyzer::setMode (Mode newMode)
{
    mode = newMode;
    if (mode == Mode::hit)
        liveSuppressesAuto = false;
    showingHit = hasHit && (mode == Mode::hit || (idleSeconds > kIdleSeconds && ! liveSuppressesAuto));
    updateModeButtons();
    repaint();
}

void SpectrumAnalyzer::updateModeButtons()
{
    liveButton.setToggleState (! showingHit, juce::dontSendNotification);
    hitButton.setToggleState (showingHit, juce::dontSendNotification);
}

//==============================================================================
void SpectrumAnalyzer::pollNow()
{
    if (const double sr = tap.sampleRate.load(); sr > 0.0 && ! juce::exactlyEqual (sr, liveRate))
        configureLive (sr);

    // --- Pull everything new: meter levels, and the mono sum into the FFT history --
    const int n = live.fftSize;
    int got = 0;
    float framePeak[2] {};
    double sumSquares[2] {};
    for (;;)
    {
        const int count = tap.pull (pullLeft.data(), pullRight.data(), (int) pullLeft.size());
        if (count == 0)
            break;

        const float* ch[2] { pullLeft.data(), pullRight.data() };
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < count; ++i)
            {
                const float x = ch[c][i];
                framePeak[c] = juce::jmax (framePeak[c], std::abs (x));
                sumSquares[c] += (double) x * x;
            }

        for (int i = 0; i < count; ++i)
        {
            history[(size_t) historyPos] = 0.5f * (pullLeft[(size_t) i] + pullRight[(size_t) i]);
            if (++historyPos == n)
                historyPos = 0;
        }
        got += count;
    }

    // --- Meter ---------------------------------------------------------------------------
    const float rmsCoeff = 1.0f - std::exp (-kFrameSeconds / kRmsSeconds);
    bool dirty = false;
    for (int c = 0; c < 2; ++c)
    {
        const float before = meterPeakDb[c] + meterRms[c];
        meterPeakDb[c] = juce::jmax (toDb (framePeak[c]), meterPeakDb[c] - kMeterFallDbPerSecond * kFrameSeconds);
        const float ms = got > 0 ? (float) (sumSquares[c] / got) : 0.0f;
        meterRms[c] += (ms - meterRms[c]) * rmsCoeff;
        if (meterRms[c] < 1.0e-12f)
            meterRms[c] = 0.0f;
        dirty = dirty || (meterPeakDb[c] > kMinDb - 10.0f && ! juce::exactlyEqual (before, meterPeakDb[c] + meterRms[c]));
    }
    const float loudest = juce::jmax (framePeak[0], framePeak[1]);
    if (loudest > maxPeak)
    {
        maxPeak = loudest;
        clipped = clipped || maxPeak >= 1.0f;
        dirty = true;
    }

    // --- Live spectrum and peak readout ----------------------------------------------------
    const bool quiet = got == 0 || loudest < 1.0e-5f;
    idleSeconds = quiet ? juce::jmin (60.0f, idleSeconds + kFrameSeconds) : 0.0f;
    if (! quiet)
        liveSuppressesAuto = false;

    readoutLevel = juce::jmax (kFloorDb, readoutLevel - kReadoutFallDbPerSecond * kFrameSeconds);
    if (got > 0)
    {
        analyseLive();
        mapToColumns (live, liveColumns);

        // Hold the loudest moment's peak, so the number doesn't chase the decay.
        if (const auto peak = findPeak (live); peak.valid && peak.db > kReadoutFloorDb && peak.db >= readoutLevel)
        {
            liveReadout = peak;
            readoutLevel = peak.db;
            dirty = true;
        }
    }
    else
    {
        std::fill (liveColumns.begin(), liveColumns.end(), kFloorDb);
    }

    // --- Which spectrum to show -------------------------------------------------------------
    const bool wasShowingHit = showingHit;
    showingHit = hasHit && (mode == Mode::hit || (idleSeconds > kIdleSeconds && ! liveSuppressesAuto));
    if (showingHit != wasShowingHit)
    {
        updateModeButtons();
        dirty = true;
    }

    dirty = updateDisplay (showingHit ? hitColumns : liveColumns, ! showingHit) || dirty;

    if (const auto f = readFilter(); ! (f == filter))
    {
        filter = f;
        dirty = true;
    }

    if (dirty)
        repaint();
}

void SpectrumAnalyzer::analyseLive()
{
    const int n = live.fftSize;
    double windowSum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        int idx = historyPos + i;
        if (idx >= n)
            idx -= n;
        fftData[(size_t) i] = history[(size_t) idx] * window[(size_t) i];
        windowSum += window[(size_t) i];
    }
    std::fill (fftData.begin() + n, fftData.end(), 0.0f);
    fft->performFrequencyOnlyForwardTransform (fftData.data(), true);

    // A sine of amplitude A peaks at A * sum (window) / 2, so this reads 0 dB for
    // a full-scale sine.
    const float scale = (float) (2.0 / windowSum);
    for (int k = 0; k <= n / 2; ++k)
        live.db[(size_t) k] = toDb (fftData[(size_t) k] * scale);
}

SpectrumAnalyzer::PeakReadout SpectrumAnalyzer::findPeak (const Spectrum& s)
{
    PeakReadout out;
    const int bins = (int) s.db.size();
    if (bins < 4 || s.fftSize <= 0)
        return out;

    const double binHz = s.sampleRate / s.fftSize;
    const int lo = juce::jmax (1, (int) std::ceil (kMinHz / binHz));
    const int hi = juce::jmin (bins - 2, (int) std::floor (kMaxHz / binHz));
    int best = -1;
    for (int k = lo; k <= hi; ++k)
        if (best < 0 || s.db[(size_t) k] > s.db[(size_t) best])
            best = k;
    if (best < 0)
        return out;

    // Parabola through the peak bin and its neighbours (in dB).
    const float a = s.db[(size_t) best - 1], b = s.db[(size_t) best], c = s.db[(size_t) best + 1];
    const float denom = a - 2.0f * b + c;
    const float delta = denom < 0.0f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / denom) : 0.0f;
    out.hz = (float) (((double) best + (double) delta) * binHz);
    out.db = b - 0.25f * (a - c) * delta;
    out.valid = out.db > kFloorDb + 1.0f;
    return out;
}

void SpectrumAnalyzer::mapToColumns (const Spectrum& s, std::vector<float>& dest) const
{
    const int columns = (int) shown.size();
    dest.assign ((size_t) columns, kFloorDb);
    const int bins = (int) s.db.size();
    if (bins < 4 || s.fftSize <= 0 || plot.getWidth() <= 0.0f)
        return;

    const double binHz = s.sampleRate / s.fftSize;
    const double ratio = std::log (kMaxHz / kMinHz) / plot.getWidth();
    const auto binAt = [&] (double column) { return kMinHz * std::exp (column * ratio) / binHz; };
    const auto dbAt = [&] (int k) { return s.db[(size_t) juce::jlimit (0, bins - 1, k)]; };

    for (int col = 0; col < columns; ++col)
    {
        const double lo = binAt (col - 0.5), hi = binAt (col + 0.5);
        const int first = (int) std::ceil (lo), last = juce::jmin (bins - 1, (int) std::floor (hi));
        float v = kFloorDb;
        if (last >= first)
        {
            // Several bins share this pixel: the loudest one.
            for (int k = first; k <= last; ++k)
                v = juce::jmax (v, s.db[(size_t) k]);
        }
        else
        {
            // Fewer bins than pixels (the low end): Catmull-Rom between bins.
            const double pos = binAt (col);
            const int k = (int) std::floor (pos);
            const float t = (float) (pos - k);
            const float p0 = dbAt (k - 1), p1 = dbAt (k), p2 = dbAt (k + 1), p3 = dbAt (k + 2);
            v = p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
            v = juce::jlimit (juce::jmin (p1, p2) - 6.0f, juce::jmax (p1, p2), v);
        }
        dest[(size_t) col] = v;
    }
}

bool SpectrumAnalyzer::updateDisplay (const std::vector<float>& target, bool holdPeaks)
{
    bool changed = false;
    for (size_t c = 0; c < shown.size(); ++c)
    {
        const float t = c < target.size() ? juce::jmax (target[c], kMinDb - 30.0f) : kMinDb - 30.0f;
        const float s = shown[c];
        const float next = t >= s ? s + (t - s) * 0.7f : juce::jmax (t, s - kFallDbPerSecond * kFrameSeconds);
        changed = changed || (std::abs (next - s) > 0.01f && juce::jmax (next, s) > kMinDb - 2.0f);
        shown[c] = next;

        if (! holdPeaks)
        {
            hold[c] = kFloorDb;
        }
        else if (next >= hold[c])
        {
            hold[c] = next;
            holdAge[c] = 0.0f;
        }
        else if ((holdAge[c] += kFrameSeconds) > kHoldSeconds)
        {
            const float before = hold[c];
            hold[c] = juce::jmax (next, hold[c] - kHoldFallDbPerSecond * kFrameSeconds);
            changed = changed || (before > kMinDb && ! juce::exactlyEqual (before, hold[c]));
        }
    }
    return changed;
}

//==============================================================================
SpectrumAnalyzer::FilterSettings SpectrumAnalyzer::readFilter() const
{
    FilterSettings f;
    if (hpParam != nullptr)      f.hp = hpParam->convertFrom0to1 (hpParam->getValue());
    if (lpParam != nullptr)      f.lp = lpParam->convertFrom0to1 (lpParam->getValue());
    if (hpSlopeParam != nullptr) f.hpSlope = juce::roundToInt (hpSlopeParam->convertFrom0to1 (hpSlopeParam->getValue()));
    if (lpSlopeParam != nullptr) f.lpSlope = juce::roundToInt (lpSlopeParam->convertFrom0to1 (lpSlopeParam->getValue()));
    return f;
}

float SpectrumAnalyzer::filterSampleRate() const noexcept
{
    return (float) (liveRate > 0.0 ? liveRate : 48000.0);
}

float SpectrumAnalyzer::freqToX (float hz) const noexcept
{
    return plot.getX() + plot.getWidth() * std::log (juce::jmax (1.0f, hz) / kMinHz) / std::log (kMaxHz / kMinHz);
}

float SpectrumAnalyzer::xToFreq (float x) const noexcept
{
    return kMinHz * std::exp ((x - plot.getX()) / juce::jmax (1.0f, plot.getWidth()) * std::log (kMaxHz / kMinHz));
}

float SpectrumAnalyzer::dbToY (float db) const noexcept
{
    return plot.getY() + plot.getHeight() * (kMaxDb - db) / (kMaxDb - kMinDb);
}

juce::Point<float> SpectrumAnalyzer::handlePosition (Handle h) const
{
    const bool low = h == Handle::lowCut;
    const bool off = low ? OutputFilter::isHighPassOff (filter.hp) : OutputFilter::isLowPassOff (filter.lp);
    const float hz = off ? (low ? kMinHz : kMaxHz) : (low ? filter.hp : filter.lp);
    const float db = off ? 0.0f
                         : OutputFilter::responseDb (hz, filter.hp, filter.hpSlope, filter.lp, filter.lpSlope, filterSampleRate());
    return { freqToX (hz), dbToY (juce::jmax (kMinDb, db)) };
}

SpectrumAnalyzer::Handle SpectrumAnalyzer::handleAt (juce::Point<float> pos) const
{
    Handle best = Handle::none;
    float bestScore = 1.0e9f;
    for (auto h : { lastDragged, lastDragged == Handle::lowCut ? Handle::highCut : Handle::lowCut })
    {
        const auto hp = handlePosition (h);
        float score = pos.getDistanceFrom (hp);
        if (score > 12.0f)
        {
            // Anywhere on the handle's vertical line counts too, a bit less.
            const bool onLine = plot.expanded (6.0f).contains (pos) && std::abs (pos.x - hp.x) <= 5.0f;
            score = onLine ? 12.0f + std::abs (pos.x - hp.x) : 1.0e9f;
        }
        if (score < bestScore)
        {
            bestScore = score;
            best = h;
        }
    }
    return best;
}

juce::RangedAudioParameter* SpectrumAnalyzer::cutoffParam (Handle h) const noexcept
{
    return h == Handle::lowCut ? hpParam : (h == Handle::highCut ? lpParam : nullptr);
}

juce::RangedAudioParameter* SpectrumAnalyzer::slopeParam (Handle h) const noexcept
{
    return h == Handle::lowCut ? hpSlopeParam : (h == Handle::highCut ? lpSlopeParam : nullptr);
}

void SpectrumAnalyzer::setParam (juce::RangedAudioParameter* param, float plainValue)
{
    if (param == nullptr)
        return;
    const auto range = param->getNormalisableRange();
    param->setValueNotifyingHost (param->convertTo0to1 (juce::jlimit (range.start, range.end, plainValue)));
    filter = readFilter();
    repaint();
}

//==============================================================================
void SpectrumAnalyzer::mouseMove (const juce::MouseEvent& e)
{
    const auto h = handleAt (e.position);
    const bool inPlot = plot.contains (e.position);
    if (h != hover || inPlot != hoverPlot)
    {
        hover = h;
        hoverPlot = inPlot;
        repaint();
    }
    setMouseCursor (h != Handle::none ? juce::MouseCursor::LeftRightResizeCursor
                                      : (peakBox.contains (e.position) ? juce::MouseCursor::PointingHandCursor
                                                                       : juce::MouseCursor::NormalCursor));
}

void SpectrumAnalyzer::mouseExit (const juce::MouseEvent&)
{
    if (hover != Handle::none || hoverPlot)
    {
        hover = Handle::none;
        hoverPlot = false;
        repaint();
    }
}

void SpectrumAnalyzer::mouseDown (const juce::MouseEvent& e)
{
    if (peakBox.expanded (2.0f).contains (e.position) || meterArea.contains (e.position))
    {
        maxPeak = 0.0f;
        clipped = false;
        repaint();
        return;
    }

    const auto h = handleAt (e.position);
    auto* param = cutoffParam (h);
    if (param == nullptr)
        return;

    dragging = lastDragged = h;
    param->beginChangeGesture();
    dragStartLog = std::log (juce::jlimit (kMinHz, kMaxHz, param->convertFrom0to1 (param->getValue())));
    dragStartX = e.position.x;
    dragFine = e.mods.isShiftDown();
    repaint();
}

void SpectrumAnalyzer::mouseDrag (const juce::MouseEvent& e)
{
    auto* param = cutoffParam (dragging);
    if (param == nullptr)
        return;

    // Shift gives fine control; re-anchor when it changes mid-drag so nothing jumps.
    if (e.mods.isShiftDown() != dragFine)
    {
        dragFine = e.mods.isShiftDown();
        dragStartLog = std::log (juce::jlimit (kMinHz, kMaxHz, param->convertFrom0to1 (param->getValue())));
        dragStartX = e.position.x;
    }

    const float perPixel = std::log (kMaxHz / kMinHz) / juce::jmax (1.0f, plot.getWidth());
    const float logHz = dragStartLog + (e.position.x - dragStartX) * perPixel * (dragFine ? 0.15f : 1.0f);
    setParam (param, std::exp (logHz));
}

void SpectrumAnalyzer::mouseUp (const juce::MouseEvent&)
{
    if (auto* param = cutoffParam (dragging))
        param->endChangeGesture();
    dragging = Handle::none;
    repaint();
}

void SpectrumAnalyzer::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Back to the default, which is off. The second click's mouseDown has normally
    // opened a gesture already; this closes it, so the rest of the click is no drag.
    const auto h = handleAt (e.position);
    auto* param = cutoffParam (h);
    if (param == nullptr)
        return;

    if (dragging != h)
    {
        if (auto* other = cutoffParam (dragging))
            other->endChangeGesture();
        param->beginChangeGesture();
    }
    param->setValueNotifyingHost (param->getDefaultValue());
    param->endChangeGesture();
    dragging = Handle::none;
    filter = readFilter();
    repaint();
}

void SpectrumAnalyzer::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto h = dragging != Handle::none ? dragging : handleAt (e.position);
    auto* param = slopeParam (h);
    if (param == nullptr)
    {
        Component::mouseWheelMove (e, wheel);
        return;
    }
    if (wheel.isInertial)
        return;

    // A wheel notch is one step; a trackpad has to travel a little.
    int step = 0;
    const float delta = std::abs (wheel.deltaY) >= std::abs (wheel.deltaX) ? wheel.deltaY : -wheel.deltaX;
    if (! wheel.isSmooth)
    {
        step = delta > 0.0f ? 1 : (delta < 0.0f ? -1 : 0);
    }
    else
    {
        wheelAccum += delta;
        if (std::abs (wheelAccum) >= 0.12f)
        {
            step = wheelAccum > 0.0f ? 1 : -1;
            wheelAccum = 0.0f;
        }
    }
    if (step == 0)
        return;

    const int current = juce::roundToInt (param->convertFrom0to1 (param->getValue()));
    const int next = juce::jlimit (0, filterSlopeNames.size() - 1, current + step);
    if (next != current)
    {
        const bool gesture = dragging == Handle::none;
        if (gesture)
            param->beginChangeGesture();
        setParam (param, (float) next);
        if (gesture)
            param->endChangeGesture();
    }
}

//==============================================================================
void SpectrumAnalyzer::resized()
{
    auto bounds = getLocalBounds().toFloat();
    headerArea = bounds.removeFromTop (kHeaderHeight);

    const float meterX = bounds.getRight() - 9.0f - kMeterWidth;
    plot = juce::Rectangle<float>::leftTopRightBottom (bounds.getX() + kDbLabelWidth, bounds.getY() + 4.0f,
                                                       meterX - 12.0f, bounds.getBottom() - kFreqLabelHeight);
    plot = plot.withWidth (juce::jmax (10.0f, plot.getWidth())).withHeight (juce::jmax (10.0f, plot.getHeight()));
    meterArea = { meterX, plot.getY(), kMeterWidth, plot.getHeight() };
    peakBox = juce::Rectangle<float> (meterX - 7.0f, headerArea.getCentreY() - 8.0f, kMeterWidth + 14.0f, 16.0f);

    const float buttonH = 18.0f;
    auto buttons = juce::Rectangle<float> (peakBox.getX() - 10.0f - 80.0f, headerArea.getCentreY() - buttonH * 0.5f, 80.0f, buttonH);
    liveButton.setBounds (buttons.removeFromLeft (42.0f).toNearestInt());
    hitButton.setBounds (buttons.toNearestInt());

    const auto columns = (size_t) juce::roundToInt (plot.getWidth()) + 1;
    shown.assign (columns, kMinDb - 30.0f);
    hold.assign (columns, kFloorDb);
    holdAge.assign (columns, 0.0f);
    liveColumns.assign (columns, kFloorDb);
    if (hasHit)
        mapToColumns (hit, hitColumns);
    else
        hitColumns.assign (columns, kFloorDb);

    grid = {};
    gridScale = 0.0f;
}

void SpectrumAnalyzer::renderGrid (float scale)
{
    const auto bounds = getLocalBounds().toFloat();
    grid = juce::Image (juce::Image::ARGB, juce::jmax (1, juce::roundToInt (bounds.getWidth() * scale)),
                        juce::jmax (1, juce::roundToInt (bounds.getHeight() * scale)), true);
    gridScale = scale;

    juce::Graphics g (grid);
    g.addTransform (juce::AffineTransform::scale (scale));

    g.setColour (colours::display);
    g.fillRoundedRectangle (bounds, 6.0f);

    // Header divider.
    g.setColour (colours::grid);
    g.drawHorizontalLine (juce::roundToInt (headerArea.getBottom()) - 1, bounds.getX() + 6.0f, bounds.getRight() - 6.0f);

    // --- Frequency grid ---------------------------------------------------------------------
    g.setFont (uiFont (10.0f));
    for (float decade = 10.0f; decade <= 10000.0f; decade *= 10.0f)
        for (int m = 1; m <= 9; ++m)
        {
            const float hz = decade * (float) m;
            if (hz < kMinHz || hz > kMaxHz)
                continue;
            const bool labelled = m == 1 || m == 2 || m == 5;
            g.setColour (labelled ? colours::gridStrong : colours::grid.withAlpha (0.7f));
            const float x = freqToX (hz);
            g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());

            if (labelled && hz > kMinHz && hz < kMaxHz)
            {
                g.setColour (colours::textDim);
                g.drawText (formatFreqLabel (hz), juce::Rectangle<float> (x - 20.0f, plot.getBottom() + 2.0f, 40.0f, kFreqLabelHeight - 4.0f),
                            juce::Justification::centred);
            }
        }

    // --- Level grid (shared by the meter) ----------------------------------------------------
    for (float db = 0.0f; db > kMinDb; db -= 12.0f)
    {
        const float y = dbToY (db);
        const bool zero = db > -1.0f;
        g.setColour (zero ? colours::gridStrong : colours::grid);
        g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
        g.drawHorizontalLine (juce::roundToInt (y), meterArea.getX(), meterArea.getRight());

        g.setColour (colours::textDim);
        g.drawText (zero ? juce::String ("0") : juce::String (juce::roundToInt (db)),
                    juce::Rectangle<float> (bounds.getX() + 2.0f, y - 7.0f, kDbLabelWidth - 10.0f, 14.0f),
                    juce::Justification::centredRight);
    }

    // --- Meter tracks ------------------------------------------------------------------------
    const float barW = (meterArea.getWidth() - 3.0f) * 0.5f;
    for (int c = 0; c < 2; ++c)
    {
        const auto bar = meterArea.withX (meterArea.getX() + (float) c * (barW + 3.0f)).withWidth (barW);
        g.setColour (colours::grid.withAlpha (0.55f));
        g.fillRoundedRectangle (bar, 2.0f);
        g.setColour (colours::textDim);
        g.drawText (c == 0 ? "L" : "R", bar.withY (plot.getBottom() + 2.0f).withHeight (kFreqLabelHeight - 4.0f),
                    juce::Justification::centred);
    }
}

void SpectrumAnalyzer::paint (juce::Graphics& g)
{
    const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    if (! grid.isValid() || std::abs (scale - gridScale) > 0.01f)
        renderGrid (scale);
    g.drawImage (grid, getLocalBounds().toFloat());

    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (plot.toNearestInt());
        drawSpectrum (g);
    }
    drawFilter (g);
    drawMeter (g);
    drawHeader (g);

    g.setColour (colours::panelBorder);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 6.0f, 1.0f);
}

void SpectrumAnalyzer::drawSpectrum (juce::Graphics& g) const
{
    if (shown.empty())
        return;

    const auto accent = showingHit ? kHitColour : kLiveColour;
    const float bottom = plot.getBottom() + 2.0f;
    const auto yFor = [&] (float db) { return juce::jmin (bottom, dbToY (db)); };

    juce::Path line, fill;
    fill.startNewSubPath (plot.getX(), bottom);
    for (size_t c = 0; c < shown.size(); ++c)
    {
        const float x = plot.getX() + (float) c, y = yFor (shown[c]);
        fill.lineTo (x, y);
        if (c == 0)
            line.startNewSubPath (x, y);
        else
            line.lineTo (x, y);
    }
    fill.lineTo (plot.getX() + (float) (shown.size() - 1), bottom);
    fill.closeSubPath();

    g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.30f), 0.0f, dbToY (0.0f),
                                             accent.withAlpha (0.03f), 0.0f, plot.getBottom(), false));
    g.fillPath (fill);
    g.setColour (accent.withAlpha (0.9f));
    g.strokePath (line, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved));

    if (! showingHit)
    {
        juce::Path peaks;
        bool drawing = false;
        for (size_t c = 0; c < hold.size(); ++c)
        {
            const float x = plot.getX() + (float) c;
            if (hold[c] > kMinDb)
            {
                if (drawing)
                    peaks.lineTo (x, dbToY (hold[c]));
                else
                    peaks.startNewSubPath (x, dbToY (hold[c]));
                drawing = true;
            }
            else
            {
                drawing = false;
            }
        }
        g.setColour (colours::text.withAlpha (0.35f));
        g.strokePath (peaks, juce::PathStrokeType (1.0f));
    }
}

void SpectrumAnalyzer::drawFilter (juce::Graphics& g) const
{
    const bool hpOn = ! OutputFilter::isHighPassOff (filter.hp);
    const bool lpOn = ! OutputFilter::isLowPassOff (filter.lp);
    const float sr = filterSampleRate();
    const float y0 = dbToY (0.0f);

    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (plot.toNearestInt());

        if (hpOn || lpOn)
        {
            juce::Path curve;
            for (float x = plot.getX(); x <= plot.getRight() + 1.0f; x += 1.5f)
            {
                const float db = OutputFilter::responseDb (xToFreq (x), filter.hp, filter.hpSlope, filter.lp, filter.lpSlope, sr);
                const float y = juce::jmin (plot.getBottom() + 2.0f, dbToY (db));
                if (x <= plot.getX())
                    curve.startNewSubPath (x, y);
                else
                    curve.lineTo (x, y);
            }

            // Shade what the filter takes away, between the curve and 0 dB.
            juce::Path cut (curve);
            cut.lineTo (plot.getRight() + 1.0f, y0);
            cut.lineTo (plot.getX(), y0);
            cut.closeSubPath();
            g.setGradientFill (juce::ColourGradient (kFilterColour.withAlpha (0.13f), 0.0f, y0,
                                                     kFilterColour.withAlpha (0.02f), 0.0f, plot.getBottom(), false));
            g.fillPath (cut);

            g.setColour (kFilterColour.withAlpha (0.95f));
            g.strokePath (curve, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved));
        }
        else
        {
            g.setColour (kFilterColour.withAlpha (0.3f));
            g.drawHorizontalLine (juce::roundToInt (y0), plot.getX(), plot.getRight());
        }
    }

    // --- Handles ------------------------------------------------------------------------------
    for (auto h : { Handle::lowCut, Handle::highCut })
    {
        const bool on = h == Handle::lowCut ? hpOn : lpOn;
        const bool hot = h == hover || h == dragging;
        const auto pos = handlePosition (h);

        if (hot)
        {
            g.setColour (kFilterColour.withAlpha (0.35f));
            g.drawVerticalLine (juce::roundToInt (pos.x), plot.getY(), plot.getBottom());
        }

        const auto pill = juce::Rectangle<float> (22.0f, 14.0f).withCentre (pos);
        if (on || hot)
        {
            g.setColour (on ? kFilterColour : kFilterColour.withAlpha (0.6f));
            g.fillRoundedRectangle (pill, 4.0f);
            g.setColour (colours::display);
        }
        else
        {
            g.setColour (colours::display);
            g.fillRoundedRectangle (pill, 4.0f);
            g.setColour (kFilterColour.withAlpha (0.55f));
            g.drawRoundedRectangle (pill.reduced (0.5f), 4.0f, 1.0f);
        }
        g.setFont (uiFont (9.5f, true));
        g.drawText (h == Handle::lowCut ? "HP" : "LP", pill, juce::Justification::centred);

        if (hot)
        {
            // The value, beside the handle on the side facing into the plot.
            const auto text = describeHandle (h);
            const auto font = uiFont (11.0f, true);
            const float w = juce::GlyphArrangement::getStringWidth (font, text) + 12.0f;
            auto tag = juce::Rectangle<float> (w, 18.0f).withY (pos.y + 11.0f);
            tag = h == Handle::lowCut ? tag.withX (pos.x - 6.0f) : tag.withRightX (pos.x + 6.0f);
            tag = tag.constrainedWithin (plot.expanded (2.0f));
            g.setColour (colours::display.withAlpha (0.92f));
            g.fillRoundedRectangle (tag, 4.0f);
            g.setColour (kFilterColour.withAlpha (0.5f));
            g.drawRoundedRectangle (tag.reduced (0.5f), 4.0f, 1.0f);
            g.setColour (kFilterColour);
            g.setFont (font);
            g.drawText (text, tag, juce::Justification::centred);
        }
    }
}

juce::String SpectrumAnalyzer::describeHandle (Handle h) const
{
    const bool low = h == Handle::lowCut;
    const bool on = low ? ! OutputFilter::isHighPassOff (filter.hp) : ! OutputFilter::isLowPassOff (filter.lp);
    const juce::String name = low ? "Low cut " : "High cut ";
    if (! on)
        return name + "off";
    return name + formatHz (low ? filter.hp : filter.lp) + "   "
         + filterSlopeNames[low ? filter.hpSlope : filter.lpSlope] + "/oct";
}

void SpectrumAnalyzer::drawMeter (juce::Graphics& g) const
{
    const float barW = (meterArea.getWidth() - 3.0f) * 0.5f;
    const auto gradient = [&] (float alpha)
    {
        juce::ColourGradient grad (colours::eq.withAlpha (alpha), 0.0f, meterArea.getBottom(),
                                   colours::drive.withAlpha (alpha), 0.0f, dbToY (0.0f), false);
        grad.addColour (0.75, colours::click.withAlpha (alpha));
        return grad;
    };

    for (int c = 0; c < 2; ++c)
    {
        const auto bar = meterArea.withX (meterArea.getX() + (float) c * (barW + 3.0f)).withWidth (barW);
        const auto levelTop = [&] (float db) { return juce::jmax (bar.getY(), dbToY (db)); };

        if (meterPeakDb[c] > kMinDb)
        {
            const float top = levelTop (meterPeakDb[c]);
            g.setGradientFill (gradient (0.3f));
            g.fillRoundedRectangle (bar.withTop (top), 2.0f);
            g.setGradientFill (gradient (1.0f));
            g.fillRect (bar.withTop (top).withHeight (1.5f));
        }

        const float rmsDb = meterRms[c] > 0.0f ? 10.0f * std::log10 (meterRms[c]) : kFloorDb;
        if (rmsDb > kMinDb)
        {
            g.setGradientFill (gradient (0.95f));
            g.fillRoundedRectangle (bar.withTop (levelTop (rmsDb)), 2.0f);
        }
    }

    // Highest peak since the last click; red once it has gone over 0 dBFS.
    const float peakDb = juce::Decibels::gainToDecibels (maxPeak, -100.0f);
    const juce::String text = maxPeak <= 0.0f || peakDb <= -99.0f ? juce::String ("-inf")
                                                                  : (peakDb > 0.0f ? "+" : "") + juce::String (peakDb, 1);
    if (clipped)
    {
        g.setColour (colours::drive);
        g.fillRoundedRectangle (peakBox, 3.0f);
        g.setColour (colours::display);
    }
    else
    {
        g.setColour (colours::grid.withAlpha (0.6f));
        g.fillRoundedRectangle (peakBox, 3.0f);
        g.setColour (maxPeak > 0.0f ? colours::text : colours::textDim);
    }
    g.setFont (uiFont (10.5f, true));
    g.drawText (text, peakBox, juce::Justification::centred);
}

void SpectrumAnalyzer::drawHeader (juce::Graphics& g) const
{
    auto area = headerArea.reduced (10.0f, 0.0f).withRight (static_cast<float> (liveButton.getX()) - 8.0f);
    const auto accent = showingHit ? kHitColour : kLiveColour;
    const auto& readout = showingHit ? hitReadout : liveReadout;
    const bool stale = ! showingHit && idleSeconds > kIdleSeconds;

    // "PEAK  198 Hz  G3 -14 ct", in pieces of different weight.
    juce::AttributedString s;
    s.append ("PEAK   ", uiFont (10.5f, true), colours::textDim);
    if (readout.valid)
    {
        const float midi = frequencyToMidi (readout.hz);
        const int cents = juce::roundToInt ((midi - (float) juce::roundToInt (midi)) * 100.0f);
        const auto valueColour = stale ? colours::textDim : colours::text;
        s.append (formatHz (readout.hz) + "   ", uiFont (12.5f, true), valueColour);
        s.append (noteName (midi), uiFont (12.5f, true), stale ? colours::textDim : accent);
        s.append ((cents >= 0 ? "  +" : "  ") + juce::String (cents) + " ct", uiFont (10.5f), colours::textDim);
    }
    else
    {
        s.append ("-", uiFont (12.5f, true), colours::textDim);
    }
    s.setJustification (juce::Justification::centredLeft);
    s.setWordWrap (juce::AttributedString::none);

    juce::TextLayout layout;
    layout.createLayout (s, 1000.0f);
    const float readoutW = layout.getWidth();
    s.draw (g, area.withWidth (readoutW + 2.0f));

    // A hint, or why the hit is showing, in whatever room is left.
    juce::String hint;
    if (showingHit && mode == Mode::live)
        hint = "No signal: last hit";
    else if (hoverPlot && hover == Handle::none && dragging == Handle::none)
        hint = "Drag HP / LP  -  scroll: slope  -  double-click: off";

    const auto rest = area.withTrimmedLeft (readoutW + 16.0f);
    const auto font = uiFont (10.5f);
    if (hint.isNotEmpty() && juce::GlyphArrangement::getStringWidth (font, hint) <= rest.getWidth())
    {
        g.setFont (font);
        g.setColour (colours::textDim.withAlpha (0.8f));
        g.drawText (hint, rest, juce::Justification::centredRight);
    }
}

} // namespace drum::ui
