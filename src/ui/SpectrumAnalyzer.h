// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "../PluginProcessor.h"
#include <juce_dsp/juce_dsp.h>

namespace drum::ui
{

/** Spectrum of the plug-in's output, with the output low cut / high cut drawn on
    top and a stereo output meter at the right edge.

    LIVE shows the realtime output (from the processor's AnalyzerTap): a Hann-windowed
    FFT of the newest ~170 ms, 30 times a second, on a log axis from 20 Hz to 20 kHz
    and -90 to +6 dBFS (a full-scale sine reads 0 dB), falling off smoothly, with a
    thin peak-hold line. HIT shows the spectrum of the most recent offline render of
    the whole hit (see setHitAudio()), so the sound can be read without playing it.
    In LIVE, the view switches to the hit by itself after a second without signal,
    and back as soon as audio arrives.

    The header reads out the strongest peak (frequency, note and cents), for tuning.
    The HP and LP handles set the output filter: drag sideways (Shift for fine),
    scroll over a handle to change its slope, double-click to switch it off. The
    meter shows peak (bright) and RMS (solid) per channel; the number above it is
    the highest peak since the last click on it, and turns red over 0 dBFS. */
class SpectrumAnalyzer : public juce::Component,
                         private juce::Timer
{
public:
    explicit SpectrumAnalyzer (DrumFoundryProcessor& processor);
    ~SpectrumAnalyzer() override;

    /** The newest offline render of the whole hit, starting at the hit (the same
        audio the waveform display gets). Message thread. */
    void setHitAudio (const juce::AudioBuffer<float>& hit, double sampleRate);

    enum class Mode { live, hit };
    /** The mode the user picked (the LIVE / HIT buttons). */
    void setMode (Mode newMode);
    Mode getMode() const noexcept { return mode; }
    /** True while the hit spectrum is on screen (picked, or shown for lack of signal). */
    bool isShowingHit() const noexcept { return showingHit; }

    /** The strongest peak in the spectrum on screen (parabolic interpolation between
        FFT bins), as the header reads it out. In LIVE it holds the loudest recent frame. */
    struct PeakReadout
    {
        float hz = 0.0f, db = -200.0f;
        bool valid = false;
    };
    PeakReadout getPeak() const noexcept { return showingHit ? hitReadout : liveReadout; }

    /** One analysis frame, as the timer runs it: pulls whatever the tap has, then
        updates the spectrum, meter and readout. Public for the dev tool, which has
        no running message loop. */
    void pollNow();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    enum class Handle { none, lowCut, highCut };

    struct FilterSettings
    {
        float hp = 20.0f, lp = 20000.0f;
        int hpSlope = 1, lpSlope = 1;
        bool operator== (const FilterSettings& o) const noexcept
        {
            return juce::exactlyEqual (hp, o.hp) && juce::exactlyEqual (lp, o.lp) && hpSlope == o.hpSlope && lpSlope == o.lpSlope;
        }
    };

    /** A magnitude spectrum in dB per bin, 0 .. fftSize / 2. */
    struct Spectrum
    {
        std::vector<float> db;
        double sampleRate = 48000.0;
        int fftSize = 0;
    };

    void timerCallback() override { pollNow(); }

    void configureLive (double sampleRate);
    void analyseLive();
    static PeakReadout findPeak (const Spectrum& s);
    void mapToColumns (const Spectrum& s, std::vector<float>& dest) const;
    /** Moves the shown spectrum toward target (instant-ish rise, smooth fall); true if it moved. */
    bool updateDisplay (const std::vector<float>& target, bool holdPeaks);
    void updateModeButtons();

    FilterSettings readFilter() const;
    float filterSampleRate() const noexcept;

    float freqToX (float hz) const noexcept;
    float xToFreq (float x) const noexcept;
    float dbToY (float db) const noexcept;
    juce::Point<float> handlePosition (Handle h) const;
    Handle handleAt (juce::Point<float> pos) const;
    juce::RangedAudioParameter* cutoffParam (Handle h) const noexcept;
    juce::RangedAudioParameter* slopeParam (Handle h) const noexcept;
    void setParam (juce::RangedAudioParameter* param, float plainValue);

    void renderGrid (float scale);
    void drawSpectrum (juce::Graphics&) const;
    void drawFilter (juce::Graphics&) const;
    void drawHeader (juce::Graphics&) const;
    void drawMeter (juce::Graphics&) const;
    juce::String describeHandle (Handle h) const;

    AnalyzerTap& tap;
    juce::RangedAudioParameter* hpParam = nullptr;
    juce::RangedAudioParameter* hpSlopeParam = nullptr;
    juce::RangedAudioParameter* lpParam = nullptr;
    juce::RangedAudioParameter* lpSlopeParam = nullptr;

    // Live analysis (message thread only).
    double liveRate = 0.0;
    int fftOrder = 13;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, fftData, history, pullLeft, pullRight;
    int historyPos = 0;
    Spectrum live, hit;
    std::unique_ptr<juce::dsp::FFT> hitFft;
    std::vector<float> hitData;
    bool hasHit = false;

    // Display, one value per pixel column of the plot.
    std::vector<float> shown, hold, holdAge, liveColumns, hitColumns;
    PeakReadout liveReadout, hitReadout;
    float readoutLevel = -200.0f;
    float idleSeconds = 10.0f;
    bool liveSuppressesAuto = false; // LIVE clicked while the hit was shown for lack of signal
    Mode mode = Mode::live;
    bool showingHit = false;

    // Meter.
    float meterPeakDb[2] { -200.0f, -200.0f };
    float meterRms[2] {}; // mean square
    float maxPeak = 0.0f;
    bool clipped = false;

    FilterSettings filter;
    Handle hover = Handle::none, dragging = Handle::none, lastDragged = Handle::highCut;
    float dragStartLog = 0.0f;
    float dragStartX = 0.0f;
    bool dragFine = false;
    float wheelAccum = 0.0f;
    bool hoverPlot = false;

    juce::Rectangle<float> headerArea, plot, meterArea, peakBox;
    juce::Image grid;
    float gridScale = 0.0f;

    juce::TextButton liveButton { "LIVE" }, hitButton { "HIT" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumAnalyzer)
};

} // namespace drum::ui
