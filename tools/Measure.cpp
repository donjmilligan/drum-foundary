// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

// Measurement commands for the Drum Foundry dev tool. Not shipped.
//
//   measure <file-or-dir>... [--list paths.txt] [--csv out.csv]
//       Analyses one-shots (WAV/AIFF/FLAC/MP3): onset, body fundamental (FFT and
//       autocorrelation), pitch trajectory and sweep, body/wire/overall decay,
//       band energy, centroid, flatness, level. Skips files over 3 s and
//       anything with "loop" or "bpm" in the name.
//
//   measure-presets [kick|snare|all] [--preset name]... [--set id=value]...
//                   [--overrides file] [--csv out.csv] [--wav dir] [--emit]
//       Renders factory presets with renderHit() (as the render command does),
//       optionally with parameter / envelope overrides, and runs the same
//       analysis. --emit prints each preset, overrides merged, in the
//       FactoryPresets.cpp format. A second table (and extra CSV columns) gives
//       what the plug-in's spectrum analyzer shows: the HIT PEAK (Hz, note, dB),
//       the next distinct peaks, the LIVE readout, and both over other noise seeds.
//
// Overrides file: "[Preset Name]" (or "[*]" for all) starts a section; each line
// is "paramId = value" or "pitch|body|noise = { x, y, curve }, { ... }". The
// point list accepts the FactoryPresets.cpp syntax as-is. Pitch y is in
// semitones relative to Tune (the last point is pinned to 0 = Tune); body and
// noise y are linear gain, 0..4 (+12 dB). Points are clamped to those ranges.

#include "Measure.h"
#include "PluginProcessor.h"
#include "dsp/FxChain.h"
#include "dsp/OfflineRenderer.h"
#include "presets/FactoryPresets.h"
#include "ui/SpectrumAnalyzer.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <complex>
#include <map>

namespace drum::measure
{

namespace
{

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

constexpr double kMaxLengthSeconds = 3.0;

// Body fundamental: strongest tonal peak in this range, in this window after the onset.
constexpr double kBodyLoHz = 40.0, kBodyHiHz = 500.0;
constexpr double kBodyWindowStartMs = 5.0, kBodyWindowEndMs = 80.0;

// Pitch trajectory: dense track every 1 ms up to kTrackEndMs, reported at these times.
constexpr int kTrackEndMs = 80;
constexpr int kNumTrackTimes = 6;
constexpr int kTrackTimesMs[kNumTrackTimes] { 2, 5, 10, 20, 40, 80 };
constexpr double kMaxSweepSt = 30.0;

// Tail pitch (mainly for kicks, whose pitch is still falling at 80 ms).
constexpr double kTailWindowStartMs = 80.0, kTailWindowEndMs = 200.0;
constexpr double kTailLoHz = 30.0;

// Band energy split over the first 200 ms: <120, 120-400, 400-2k, 2k-8k, >8k Hz.
constexpr int kNumBands = 5;
constexpr double kBandEdgesHz[kNumBands - 1] { 120.0, 400.0, 2000.0, 8000.0 };
constexpr double kSpectrumWindowMs = 200.0;

// Low (<400 Hz) vs high (>1 kHz) energy, in these windows after the onset.
constexpr int kNumBalance = 5;
constexpr double kBalanceEdgesMs[kNumBalance + 1] { 0.0, 10.0, 30.0, 60.0, 120.0, 250.0 };

using Channels = std::vector<std::vector<float>>;

//==============================================================================
struct Metrics
{
    Metrics()
    {
        track.fill (kNaN);
        balanceDb.fill (kNaN);
        bands.fill (0.0f);
    }

    double sampleRate = 0.0;
    float lengthS = 0.0f, onsetMs = 0.0f, peakDb = -120.0f, rmsDb = -120.0f;
    float f0Fft = kNaN, f0Acf = kNaN, f0DiffSt = kNaN, tonalDb = kNaN, fTail = kNaN;
    std::array<float, kNumTrackTimes> track;
    float f20to40 = kNaN, sweepSt = kNaN, settleMs = kNaN;
    float bodyT20 = kNaN, bodyT40 = kNaN, wireT20 = kNaN, wireT40 = kNaN, allT20 = kNaN, allT40 = kNaN;
    std::array<float, kNumBands> bands;
    float centroidHz = 0.0f, flatness = kNaN;
    std::array<float, kNumBalance> balanceDb;
};

float semitones (double f, double ref) { return (float) (12.0 * std::log2 (f / ref)); }
float toDb (double power) { return (float) (10.0 * std::log10 (juce::jmax (power, 1.0e-20))); }

//==============================================================================
// FFT helpers

juce::dsp::FFT& fftFor (int order)
{
    static std::map<int, std::unique_ptr<juce::dsp::FFT>> cache;
    auto& f = cache[order];
    if (f == nullptr)
        f = std::make_unique<juce::dsp::FFT> (order);
    return *f;
}

int orderFor (int n)
{
    int order = 1;
    while ((1 << order) < n)
        ++order;
    return order;
}

/** Complex spectrum of x[start, start + len) (zero outside the signal), times
    `window` if it's non-empty, zero-padded to 2^order. */
std::vector<std::complex<float>> spectrumOf (const std::vector<float>& x, int start, int len,
                                             const std::vector<float>& window, int order)
{
    const int size = 1 << order;
    std::vector<std::complex<float>> in ((size_t) size), out ((size_t) size);
    for (int i = 0; i < juce::jmin (len, size); ++i)
    {
        const int s = start + i;
        const float v = s >= 0 && s < (int) x.size() ? x[(size_t) s] : 0.0f;
        in[(size_t) i] = window.empty() ? v : v * window[(size_t) i];
    }
    fftFor (order).perform (in.data(), out.data(), false);
    return out;
}

std::vector<float> hann (int n)
{
    std::vector<float> w ((size_t) n);
    for (int i = 0; i < n; ++i)
        w[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * ((float) i + 0.5f) / (float) n);
    return w;
}

struct Peak
{
    double hz = kNaN;
    double db = -400.0;      // amplitude of the peak (sinusoid amplitude, dB re full scale)
    double prominenceDb = 0; // peak minus the median of the search band
};

/** Strongest local maximum of a Hann-windowed spectrum between lo and hi Hz,
    with parabolic interpolation on the dB magnitudes. */
Peak findPeak (const std::vector<std::complex<float>>& spec, int order, double sr, double windowSum,
               double loHz, double hiHz)
{
    const int size = 1 << order;
    const double binHz = sr / size;
    const int lo = juce::jmax (1, (int) std::ceil (loHz / binHz));
    const int hi = juce::jmin (size / 2 - 2, (int) std::floor (hiHz / binHz));
    Peak p;
    if (hi <= lo + 1)
        return p;

    std::vector<double> db ((size_t) (hi - lo + 3));
    for (int k = lo - 1; k <= hi + 1; ++k)
        db[(size_t) (k - lo + 1)] = 20.0 * std::log10 (juce::jmax (1.0e-12, 2.0 * std::abs (spec[(size_t) k]) / windowSum));

    int best = -1;
    for (int k = lo; k <= hi; ++k)
    {
        const double b = db[(size_t) (k - lo + 1)];
        if (b >= db[(size_t) (k - lo)] && b >= db[(size_t) (k - lo + 2)] && (best < 0 || b > db[(size_t) (best - lo + 1)]))
            best = k;
    }
    if (best < 0)
        return p;

    const double a = db[(size_t) (best - lo)], b = db[(size_t) (best - lo + 1)], c = db[(size_t) (best - lo + 2)];
    const double denom = a - 2.0 * b + c;
    const double d = std::abs (denom) > 1.0e-12 ? juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / denom) : 0.0;
    p.hz = (best + d) * binHz;
    p.db = b - 0.25 * (a - c) * d;

    std::vector<double> band (db.begin() + 1, db.end() - 1);
    std::nth_element (band.begin(), band.begin() + (long) band.size() / 2, band.end());
    p.prominenceDb = p.db - band[band.size() / 2];
    return p;
}

/** Hann-windowed peak search of mono[centre - len/2, centre + len/2). */
Peak framePeak (const std::vector<float>& mono, double sr, int centre, int len, double loHz, double hiHz)
{
    const auto w = hann (len);
    double wsum = 0.0;
    for (float v : w)
        wsum += v;
    const int order = juce::jlimit (14, 17, orderFor (len * 8));
    const auto spec = spectrumOf (mono, centre - len / 2, len, w, order);
    return findPeak (spec, order, sr, wsum, loHz, hiHz);
}

/** Pitch by autocorrelation of x[start, start + len), band-limited to lo..hi Hz
    in the frequency domain. Takes the shortest lag whose peak is within 85 % of
    the best one, so it doesn't lock onto a subharmonic. */
float acfPitch (const std::vector<float>& x, int start, int len, double sr, double loHz, double hiHz)
{
    const int order = orderFor (len * 2);
    const int size = 1 << order;
    auto spec = spectrumOf (x, start, len, {}, order);
    const double binHz = sr / size;
    for (int k = 0; k < size; ++k)
    {
        const double f = juce::jmin (k, size - k) * binHz;
        const double p = std::norm (spec[(size_t) k]);
        spec[(size_t) k] = f >= loHz && f <= hiHz ? std::complex<float> ((float) p, 0.0f) : std::complex<float>();
    }
    std::vector<std::complex<float>> r ((size_t) size);
    fftFor (order).perform (spec.data(), r.data(), true);

    const double r0 = r[0].real();
    if (! (r0 > 0.0))
        return kNaN;

    const int minLag = juce::jmax (2, (int) std::floor (sr / hiHz));
    const int maxLag = juce::jmin (len - 2, (int) std::ceil (sr / loHz));
    if (maxLag <= minLag + 2)
        return kNaN;

    // Unbiased, normalised: divide out the shrinking overlap of the rectangular window.
    auto acf = [&] (int lag) { return r[(size_t) lag].real() / r0 * (double) len / (double) (len - lag); };

    auto isPeak = [&] (int lag) { return acf (lag) >= acf (lag - 1) && acf (lag) >= acf (lag + 1); };

    double best = 0.0;
    for (int lag = minLag + 1; lag < maxLag; ++lag)
        if (isPeak (lag))
            best = juce::jmax (best, acf (lag));
    if (best <= 0.0)
        return kNaN;

    for (int lag = minLag + 1; lag < maxLag; ++lag)
    {
        const double a = acf (lag - 1), b = acf (lag), c = acf (lag + 1);
        if (isPeak (lag) && b >= 0.85 * best)
        {
            const double denom = a - 2.0 * b + c;
            const double d = std::abs (denom) > 1.0e-12 ? juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / denom) : 0.0;
            return (float) (sr / (lag + d));
        }
    }
    return kNaN;
}

//==============================================================================
// Time-domain filters and envelopes

struct Biquad
{
    enum Type { lowpass, highpass, bandpass };

    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    Biquad (Type type, double hz, double q, double sr)
    {
        const double w = juce::MathConstants<double>::twoPi * juce::jlimit (1.0, sr * 0.49, hz) / sr;
        const double cw = std::cos (w), alpha = std::sin (w) / (2.0 * q), a0 = 1.0 + alpha;
        switch (type)
        {
            case lowpass:  b0 = (1.0 - cw) * 0.5; b1 = 1.0 - cw;    b2 = b0; break;
            case highpass: b0 = (1.0 + cw) * 0.5; b1 = -(1.0 + cw); b2 = b0; break;
            case bandpass: b0 = alpha;            b1 = 0.0;         b2 = -alpha; break;
        }
        b0 /= a0; b1 /= a0; b2 /= a0;
        a1 = -2.0 * cw / a0;
        a2 = (1.0 - alpha) / a0;
    }

    float process (float x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float) y;
    }
};

Channels filtered (const Channels& in, const std::vector<Biquad>& stages)
{
    Channels out = in;
    for (auto& ch : out)
    {
        auto s = stages;
        for (auto& v : ch)
            for (auto& bq : s)
                v = bq.process (v);
    }
    return out;
}

/** Power summed over channels, as 5 ms RMS frames every 1 ms from the onset, in dB. */
std::vector<float> envelopeDb (const Channels& x, double sr, int onset)
{
    const int hop = juce::jmax (1, (int) std::round (0.001 * sr));
    const int win = (int) std::round (0.005 * sr);
    const int n = (int) x[0].size();
    std::vector<float> env;
    for (int start = onset; start + win <= n; start += hop)
    {
        double e = 0.0;
        for (const auto& ch : x)
            for (int i = start; i < start + win; ++i)
                e += (double) ch[(size_t) i] * ch[(size_t) i];
        env.push_back (toDb (e / win));
    }
    return env;
}

/** Time (ms after onset) until the envelope last sits within `dropDb` of its
    peak, i.e. the time to fall `dropDb` for good. NaN if it never gets there
    before the file ends. Frame k covers k .. k + 5 ms; the centre is used. */
float decayMs (const std::vector<float>& env, float dropDb)
{
    if (env.empty())
        return kNaN;
    const auto peakIt = std::max_element (env.begin(), env.end());
    const float thr = *peakIt - dropDb;
    int last = -1;
    for (int k = (int) (peakIt - env.begin()); k < (int) env.size(); ++k)
        if (env[(size_t) k] >= thr)
            last = k;
    if (last >= (int) env.size() - 1)
        return kNaN;
    return (float) last + 3.5f;
}

double energy (const Channels& x, int start, int end)
{
    double e = 0.0;
    for (const auto& ch : x)
        for (int i = juce::jmax (0, start); i < juce::jmin (end, (int) ch.size()); ++i)
            e += (double) ch[(size_t) i] * ch[(size_t) i];
    return e;
}

//==============================================================================
int findOnset (const Channels& x)
{
    const int n = (int) x[0].size();
    std::vector<float> mag ((size_t) n, 0.0f);
    float peak = 0.0f;
    for (const auto& ch : x)
        for (int i = 0; i < n; ++i)
        {
            mag[(size_t) i] = juce::jmax (mag[(size_t) i], std::abs (ch[(size_t) i]));
            peak = juce::jmax (peak, mag[(size_t) i]);
        }
    if (peak <= 0.0f)
        return 0;

    // First sample within 20 dB of the peak, then back to where the signal
    // rises out of the floor (40 dB under the peak), at most 5 ms earlier.
    int i = 0;
    while (i < n && mag[(size_t) i] < peak * 0.1f)
        ++i;
    const int limit = juce::jmax (0, i - 240);
    while (i > limit && mag[(size_t) i - 1] >= peak * 0.01f)
        --i;
    return i;
}

Metrics analyse (const Channels& x, double sr)
{
    Metrics m;
    m.sampleRate = sr;
    const int n = (int) x[0].size();
    m.lengthS = (float) (n / sr);
    if (n < (int) (0.02 * sr))
        return m;

    std::vector<float> mono ((size_t) n, 0.0f);
    float peak = 0.0f;
    for (const auto& ch : x)
        for (int i = 0; i < n; ++i)
        {
            mono[(size_t) i] += ch[(size_t) i] / (float) x.size();
            peak = juce::jmax (peak, std::abs (ch[(size_t) i]));
        }
    m.peakDb = juce::Decibels::gainToDecibels (peak, -120.0f);

    const int onset = findOnset (x);
    m.onsetMs = (float) (onset * 1000.0 / sr);
    auto ms = [sr] (double t) { return (int) std::round (t * 0.001 * sr); };

    const int specLen = juce::jmin (n - onset, ms (kSpectrumWindowMs));
    m.rmsDb = toDb (energy (x, onset, onset + specLen) / juce::jmax (1, specLen) / (double) x.size());

    // --- Body fundamental ---------------------------------------------------
    {
        const int start = onset + ms (kBodyWindowStartMs);
        const int len = juce::jmin (ms (kBodyWindowEndMs - kBodyWindowStartMs), n - start);
        if (len > ms (10.0))
        {
            const auto w = hann (len);
            double wsum = 0.0;
            for (float v : w)
                wsum += v;
            const int order = juce::jlimit (15, 17, orderFor (len * 8));
            const auto p = findPeak (spectrumOf (mono, start, len, w, order), order, sr, wsum, kBodyLoHz, kBodyHiHz);
            m.f0Fft = (float) p.hz;
            m.tonalDb = (float) p.prominenceDb;
            m.f0Acf = acfPitch (mono, start, len, sr, kBodyLoHz, kBodyHiHz);
            if (std::isfinite (m.f0Fft) && std::isfinite (m.f0Acf))
                m.f0DiffSt = semitones (m.f0Acf, m.f0Fft);

            // --- Pitch trajectory ---------------------------------------------
            // Tracked backwards from 80 ms (where the body has settled) towards
            // the onset. The window spans 3 periods near the onset, growing to 6
            // by ~25 ms (so a 1.6x overtone can't pull the peak once the sweep
            // is over), so an early value averages the first few ms of the sweep.
            if (std::isfinite (m.f0Fft))
            {
                std::array<float, kTrackEndMs + 1> dense;
                dense.fill (kNaN);
                const double bodyDb = p.db;
                double ref = m.f0Fft;
                for (int t = kTrackEndMs; t >= 1; --t)
                {
                    const double periods = juce::jmin (6.0, 3.0 + t / 8.0);
                    const int win = juce::jlimit (ms (8.0), ms (60.0), (int) std::round (periods * sr / ref));
                    // Going back in time the pitch can only rise quickly in the
                    // first ~10 ms; the ceiling keeps a weak body from drifting
                    // up into the noise.
                    const double stepUp = t == kTrackEndMs ? 3.0 : t > 12 ? 2.0 : 6.0;
                    const double lo = ref * std::exp2 ((t == kTrackEndMs ? -5.0 : -1.5) / 12.0);
                    const double hi = juce::jmin (ref * std::exp2 (stepUp / 12.0), m.f0Fft * std::exp2 (kMaxSweepSt / 12.0));
                    auto fp = framePeak (mono, sr, onset + ms (t), win, juce::jmax (30.0, lo), juce::jmin (3000.0, hi));

                    // Second pass with the window sized to this frame's own pitch,
                    // which is shorter while the sweep is still high.
                    if (std::isfinite (fp.hz))
                    {
                        const int win2 = juce::jlimit (ms (6.0), ms (60.0), (int) std::round (periods * sr / fp.hz));
                        if (win2 < win)
                        {
                            const auto fp2 = framePeak (mono, sr, onset + ms (t), win2, fp.hz * std::exp2 (-3.0 / 12.0),
                                                        juce::jmin (3000.0, fp.hz * std::exp2 (3.0 / 12.0)));
                            if (std::isfinite (fp2.hz))
                                fp = fp2;
                        }
                    }

                    if (std::isfinite (fp.hz) && fp.db >= bodyDb - 20.0)
                    {
                        dense[(size_t) t] = (float) fp.hz;
                        ref = fp.hz;
                    }
                }

                for (int i = 0; i < kNumTrackTimes; ++i)
                    m.track[(size_t) i] = dense[(size_t) kTrackTimesMs[i]];

                std::vector<float> mid;
                for (int t = 20; t <= 40; ++t)
                    if (std::isfinite (dense[(size_t) t]))
                        mid.push_back (dense[(size_t) t]);
                if (! mid.empty())
                {
                    std::nth_element (mid.begin(), mid.begin() + (long) mid.size() / 2, mid.end());
                    m.f20to40 = mid[mid.size() / 2];
                }

                // Settled pitch: median of the valid frames from 60 to 80 ms.
                float settled = kNaN;
                std::vector<float> tail;
                for (int t = 60; t <= kTrackEndMs; ++t)
                    if (std::isfinite (dense[(size_t) t]))
                        tail.push_back (dense[(size_t) t]);
                if (! tail.empty())
                {
                    std::nth_element (tail.begin(), tail.begin() + (long) tail.size() / 2, tail.end());
                    settled = tail[tail.size() / 2];
                }
                float first = kNaN;
                for (int t = 2; t <= 20 && ! std::isfinite (first); ++t)
                    first = dense[(size_t) t];

                if (std::isfinite (settled))
                {
                    if (std::isfinite (first))
                        m.sweepSt = semitones (first, settled);
                    int settle = 1;
                    for (int t = 1; t <= kTrackEndMs; ++t)
                        if (std::isfinite (dense[(size_t) t]) && std::abs (semitones (dense[(size_t) t], settled)) > 1.0f)
                            settle = t + 1;
                    m.settleMs = (float) settle;
                }
            }
        }
    }

    // --- Tail pitch --------------------------------------------------------------
    {
        const int start = onset + ms (kTailWindowStartMs);
        const int len = juce::jmin (ms (kTailWindowEndMs - kTailWindowStartMs), n - start);
        if (len > ms (40.0))
        {
            const auto w = hann (len);
            double wsum = 0.0;
            for (float v : w)
                wsum += v;
            const int order = juce::jlimit (15, 17, orderFor (len * 8));
            const auto p = findPeak (spectrumOf (mono, start, len, w, order), order, sr, wsum, kTailLoHz, kBodyHiHz);
            if (p.prominenceDb >= 10.0)
                m.fTail = (float) p.hz;
        }
    }

    // --- Decays ---------------------------------------------------------------
    m.allT20 = decayMs (envelopeDb (x, sr, onset), 20.0f);
    m.allT40 = decayMs (envelopeDb (x, sr, onset), 40.0f);
    if (std::isfinite (m.f0Fft))
    {
        const auto body = envelopeDb (filtered (x, { Biquad (Biquad::bandpass, m.f0Fft, 2.5, sr),
                                                     Biquad (Biquad::bandpass, m.f0Fft, 2.5, sr) }), sr, onset);
        m.bodyT20 = decayMs (body, 20.0f);
        m.bodyT40 = decayMs (body, 40.0f);
    }
    const auto wires = filtered (x, { Biquad (Biquad::highpass, 2000.0, 0.707, sr), Biquad (Biquad::highpass, 2000.0, 0.707, sr),
                                      Biquad (Biquad::lowpass, 8000.0, 0.707, sr), Biquad (Biquad::lowpass, 8000.0, 0.707, sr) });
    const auto wireEnv = envelopeDb (wires, sr, onset);
    m.wireT20 = decayMs (wireEnv, 20.0f);
    m.wireT40 = decayMs (wireEnv, 40.0f);

    // --- Low vs high balance over time ---------------------------------------
    {
        const auto low = filtered (x, { Biquad (Biquad::lowpass, 400.0, 0.707, sr), Biquad (Biquad::lowpass, 400.0, 0.707, sr) });
        const auto high = filtered (x, { Biquad (Biquad::highpass, 1000.0, 0.707, sr), Biquad (Biquad::highpass, 1000.0, 0.707, sr) });
        for (int b = 0; b < kNumBalance; ++b)
        {
            const int s = onset + ms (kBalanceEdgesMs[b]), e = onset + ms (kBalanceEdgesMs[b + 1]);
            if (e <= n)
                m.balanceDb[(size_t) b] = toDb (energy (low, s, e)) - toDb (energy (high, s, e));
        }
    }

    // --- Spectrum of the first 200 ms: bands, centroid ------------------------
    if (specLen > 0)
    {
        const int order = orderFor (specLen);
        const int size = 1 << order;
        std::array<double, kNumBands> bandE {};
        double num = 0.0, den = 0.0;
        for (const auto& ch : x)
        {
            const auto spec = spectrumOf (ch, onset, specLen, {}, order);
            for (int k = 1; k < size / 2; ++k)
            {
                const double f = k * sr / size;
                const double e = std::norm (spec[(size_t) k]);
                int band = 0;
                while (band < kNumBands - 1 && f >= kBandEdgesHz[band])
                    ++band;
                bandE[(size_t) band] += e;
                num += f * e;
                den += e;
            }
        }
        for (int b = 0; b < kNumBands; ++b)
            m.bands[(size_t) b] = den > 0.0 ? (float) (100.0 * bandE[(size_t) b] / den) : 0.0f;
        m.centroidHz = den > 0.0 ? (float) (num / den) : 0.0f;
    }

    // --- Spectral flatness 1-8 kHz (Welch, ~21 ms frames) ---------------------
    {
        const int order = orderFor ((int) (0.021 * sr));
        const int frame = 1 << order;
        const auto w = hann (frame);
        std::vector<double> psd ((size_t) frame / 2, 0.0);
        int frames = 0;
        for (int s = onset; s + frame <= juce::jmin (n, onset + juce::jmax (specLen, frame)); s += frame / 2, ++frames)
            for (const auto& ch : x)
            {
                const auto spec = spectrumOf (ch, s, frame, w, order);
                for (int k = 0; k < frame / 2; ++k)
                    psd[(size_t) k] += std::norm (spec[(size_t) k]);
            }
        if (frames > 0)
        {
            double logSum = 0.0, sum = 0.0;
            int count = 0;
            for (int k = 1; k < frame / 2; ++k)
            {
                const double f = k * sr / frame;
                if (f < 1000.0 || f > 8000.0)
                    continue;
                logSum += std::log (psd[(size_t) k] + 1.0e-30);
                sum += psd[(size_t) k];
                ++count;
            }
            if (count > 0 && sum > 0.0)
                m.flatness = (float) (std::exp (logSum / count) / (sum / count));
        }
    }

    return m;
}

//==============================================================================
// Output

juce::String fmt (float v, int decimals = 1)
{
    if (! std::isfinite (v))
        return "nan";
    return decimals > 0 ? juce::String (v, decimals) : juce::String (juce::roundToInt (v)); // String (v, 0) means "all digits"
}

juce::String csvHeader()
{
    juce::StringArray h { "name", "group", "sr", "lengthS", "onsetMs", "peakDb", "rms200Db",
                          "f0Fft", "f0Acf", "f0DiffSt", "tonalDb" };
    for (int t : kTrackTimesMs)
        h.add ("f" + juce::String (t) + "ms");
    h.addArray ({ "fTail80to200", "f20to40", "sweepSt", "settleMs", "bodyT20", "bodyT40", "wireT20", "wireT40", "allT20", "allT40",
                  "e<120", "e120-400", "e400-2k", "e2k-8k", "e>8k", "centroidHz", "flatness1k8k" });
    for (int b = 0; b < kNumBalance; ++b)
        h.add ("lowHigh" + juce::String ((int) kBalanceEdgesMs[b]) + "-" + juce::String ((int) kBalanceEdgesMs[b + 1]));
    return h.joinIntoString (",");
}

juce::String csvQuote (const juce::String& s)
{
    return "\"" + s.replace ("\"", "\"\"") + "\"";
}

juce::String csvRow (const juce::String& name, const juce::String& group, const Metrics& m)
{
    juce::StringArray r { csvQuote (name), csvQuote (group), juce::String ((int) m.sampleRate), fmt (m.lengthS, 3),
                          fmt (m.onsetMs), fmt (m.peakDb), fmt (m.rmsDb), fmt (m.f0Fft), fmt (m.f0Acf),
                          fmt (m.f0DiffSt, 2), fmt (m.tonalDb) };
    for (float f : m.track)
        r.add (fmt (f));
    r.addArray ({ fmt (m.fTail), fmt (m.f20to40), fmt (m.sweepSt, 2), fmt (m.settleMs, 0), fmt (m.bodyT20, 0), fmt (m.bodyT40, 0),
                  fmt (m.wireT20, 0), fmt (m.wireT40, 0), fmt (m.allT20, 0), fmt (m.allT40, 0) });
    for (float b : m.bands)
        r.add (fmt (b));
    r.addArray ({ fmt (m.centroidHz, 0), fmt (m.flatness, 3) });
    for (float b : m.balanceDb)
        r.add (fmt (b));
    return r.joinIntoString (",");
}

void printTableHeader (const char* nameTitle)
{
    std::printf ("%-28s %6s %6s %4s %5s | %5s %5s %5s %5s %5s %5s %5s %5s | %5s %4s | %4s %4s %4s %4s %4s %4s | %4s %4s %4s %4s %4s | %5s %4s | %5s %5s %5s %5s %5s\n",
                 nameTitle, "f0fft", "f0acf", "dSt", "tonal", "f2", "f5", "f10", "f20", "f40", "f80", "f2040", "ftail",
                 "sweep", "setl", "bT20", "bT40", "wT20", "wT40", "aT20", "aT40",
                 "<120", "-400", "-2k", "-8k", ">8k", "centr", "flat", "LH10", "LH30", "LH60", "LH120", "LH250");
}

void printTableRow (const juce::String& name, const Metrics& m)
{
    const bool disagree = std::isfinite (m.f0DiffSt) && std::abs (m.f0DiffSt) > 1.0f;
    std::printf ("%-28s %6s %6s %4s%s%5s | %5s %5s %5s %5s %5s %5s %5s %5s | %5s %4s | %4s %4s %4s %4s %4s %4s | %4.0f %4.0f %4.0f %4.0f %4.0f | %5.0f %4s | %5s %5s %5s %5s %5s\n",
                 name.substring (0, 28).toRawUTF8(), fmt (m.f0Fft).toRawUTF8(), fmt (m.f0Acf).toRawUTF8(),
                 fmt (m.f0DiffSt).toRawUTF8(), disagree ? "!" : " ", fmt (m.tonalDb, 0).toRawUTF8(),
                 fmt (m.track[0], 0).toRawUTF8(), fmt (m.track[1], 0).toRawUTF8(), fmt (m.track[2], 0).toRawUTF8(),
                 fmt (m.track[3], 0).toRawUTF8(), fmt (m.track[4], 0).toRawUTF8(), fmt (m.track[5], 0).toRawUTF8(),
                 fmt (m.f20to40, 0).toRawUTF8(), fmt (m.fTail, 0).toRawUTF8(), fmt (m.sweepSt).toRawUTF8(), fmt (m.settleMs, 0).toRawUTF8(),
                 fmt (m.bodyT20, 0).toRawUTF8(), fmt (m.bodyT40, 0).toRawUTF8(), fmt (m.wireT20, 0).toRawUTF8(),
                 fmt (m.wireT40, 0).toRawUTF8(), fmt (m.allT20, 0).toRawUTF8(), fmt (m.allT40, 0).toRawUTF8(),
                 m.bands[0], m.bands[1], m.bands[2], m.bands[3], m.bands[4], m.centroidHz,
                 fmt (m.flatness, 2).toRawUTF8(), fmt (m.balanceDb[0], 0).toRawUTF8(), fmt (m.balanceDb[1], 0).toRawUTF8(),
                 fmt (m.balanceDb[2], 0).toRawUTF8(), fmt (m.balanceDb[3], 0).toRawUTF8(), fmt (m.balanceDb[4], 0).toRawUTF8());
}

void printLegend()
{
    std::printf ("\nf0fft/f0acf: body fundamental 40-500 Hz, 5-80 ms after onset (FFT peak / autocorrelation; ! = over 1 st apart)."
                 " tonal: FFT peak over band median, dB.\n"
                 "f2..f80: tracked body pitch (Hz) at that many ms; f2040: median 20-40 ms; ftail: peak 80-200 ms. sweep: st from the first tracked frame to"
                 " the settled pitch; setl: ms until it stays within 1 st.\n"
                 "bT/wT/aT: ms to fall 20/40 dB (body band at f0 / 2-8 kHz wires / overall). <120..>8k: %% energy in the first 200 ms."
                 " flat: spectral flatness 1-8 kHz. LHx: <400 Hz vs >1 kHz energy (dB) in the window ending at x ms.\n");
}

bool writeWav (const juce::AudioBuffer<float>& b, double sr, const juce::File& file)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                                     .withSampleRate (sr)
                                                                     .withNumChannels (b.getNumChannels())
                                                                     .withBitsPerSample (24));
    return writer != nullptr && writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

Channels toChannels (const juce::AudioBuffer<float>& b)
{
    Channels x ((size_t) b.getNumChannels());
    for (int c = 0; c < b.getNumChannels(); ++c)
        x[(size_t) c].assign (b.getReadPointer (c), b.getReadPointer (c) + b.getNumSamples());
    return x;
}

//==============================================================================
// measure

bool isAudioFile (const juce::File& f)
{
    return f.hasFileExtension ("wav;aif;aiff;flac;mp3");
}

bool looksLikeLoop (const juce::File& f)
{
    const auto name = f.getFileName();
    return name.containsIgnoreCase ("loop") || name.containsIgnoreCase ("bpm");
}

int cmdMeasure (const juce::StringArray& args)
{
    const auto cwd = juce::File::getCurrentWorkingDirectory();
    juce::File csvFile;
    juce::Array<juce::File> files;

    auto addPath = [&] (const juce::String& path)
    {
        const auto f = cwd.getChildFile (path.trim());
        if (f.isDirectory())
        {
            for (const auto& entry : juce::RangedDirectoryIterator (f, true, "*", juce::File::findFiles))
                if (isAudioFile (entry.getFile()))
                    files.add (entry.getFile());
        }
        else if (f.existsAsFile())
            files.add (f);
        else
            std::fprintf (stderr, "not found: %s\n", path.toRawUTF8());
    };

    for (int i = 0; i < args.size(); ++i)
    {
        if (args[i] == "--csv" && i + 1 < args.size())
            csvFile = cwd.getChildFile (args[++i]);
        else if (args[i] == "--list" && i + 1 < args.size())
        {
            juce::StringArray lines;
            lines.addLines (cwd.getChildFile (args[++i]).loadFileAsString());
            for (const auto& l : lines)
                if (l.trim().isNotEmpty())
                    addPath (l);
        }
        else
            addPath (args[i]);
    }

    if (files.isEmpty())
    {
        std::printf ("usage: measure <file-or-dir>... [--list paths.txt] [--csv out.csv]\n");
        return 1;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    juce::StringArray csv { csvHeader() };
    printTableHeader ("file");
    int measured = 0, skipped = 0;
    for (const auto& f : files)
    {
        if (looksLikeLoop (f))
        {
            ++skipped;
            continue;
        }
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));
        if (reader == nullptr || reader->sampleRate <= 0.0
            || (double) reader->lengthInSamples / reader->sampleRate > kMaxLengthSeconds || reader->lengthInSamples < 64)
        {
            ++skipped;
            continue;
        }

        const int channels = juce::jlimit (1, 2, (int) reader->numChannels);
        juce::AudioBuffer<float> b (channels, (int) reader->lengthInSamples);
        reader->read (&b, 0, b.getNumSamples(), 0, true, channels > 1);

        const auto m = analyse (toChannels (b), reader->sampleRate);
        printTableRow (f.getFileNameWithoutExtension(), m);
        csv.add (csvRow (f.getFileName(), f.getParentDirectory().getFileName(), m) + "," + csvQuote (f.getFullPathName()));
        ++measured;
    }
    printLegend();
    std::printf ("%d measured, %d skipped (loops, over %.0f s, or unreadable)\n", measured, skipped, kMaxLengthSeconds);

    if (csvFile != juce::File())
    {
        csv.set (0, csv[0] + ",path");
        csvFile.replaceWithText (csv.joinIntoString ("\n") + "\n");
    }
    return 0;
}

//==============================================================================
// What the plug-in's spectrum analyzer shows (src/ui/SpectrumAnalyzer.cpp)
//
// HIT: the analyzer's own spectrum is private, so it is rebuilt here with the same
// code (first 200 ms, flat 100 ms then a cosine taper, zero-padded FFT, dB per bin),
// and the readout is cross-checked against a real SpectrumAnalyzer fed the same
// render through setHitAudio(). LIVE: the render is pushed through the processor's
// AnalyzerTap in 1600-sample chunks (30 frames/s at 48 kHz) into a real
// SpectrumAnalyzer, polled after each chunk, and its held readout is read back. The
// hit's position relative to the frame grid is arbitrary in the plug-in, so this
// runs at several offsets. The loudest single LIVE frame (8192-point Hann of the
// newest audio) is also rebuilt, for its second peak.

struct SpecPeak
{
    float hz = kNaN, db = -200.0f;
};

struct UiSpectrum
{
    std::vector<float> db;
    double sampleRate = 48000.0;
    int fftSize = 0;
};

constexpr float kUiFloorDb = -200.0f;

float uiToDb (float magnitude) noexcept
{
    return magnitude > 1.0e-10f ? 20.0f * std::log10 (magnitude) : kUiFloorDb;
}

/** SpectrumAnalyzer::setHitAudio's spectrum, step for step. */
UiSpectrum uiHitSpectrum (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    constexpr double kHitFlatSeconds = 0.1, kHitSeconds = 0.2;
    UiSpectrum s;
    const int total = audio.getNumSamples();
    if (total <= 0 || audio.getNumChannels() == 0)
        return s;

    const int len = juce::jmin (total, (int) std::lround (kHitSeconds * sampleRate));
    const int flat = juce::jmin (len / 2, (int) std::lround (kHitFlatSeconds * sampleRate));
    const int order = juce::jlimit (12, 16, (int) std::ceil (std::log2 (juce::jmax ((double) len, sampleRate * 0.3))));
    const int n = 1 << order;
    std::vector<float> data ((size_t) (2 * n), 0.0f);
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
    fftFor (order).performFrequencyOnlyForwardTransform (data.data(), true);
    const float scale = (float) (2.0 / juce::jmax (1.0, windowSum));
    s.db.resize ((size_t) (n / 2 + 1));
    for (int k = 0; k <= n / 2; ++k)
        s.db[(size_t) k] = uiToDb (data[(size_t) k] * scale);
    s.sampleRate = sampleRate;
    s.fftSize = n;
    return s;
}

/** SpectrumAnalyzer::analyseLive on the 8192 samples of mono ending at `end`. */
UiSpectrum uiLiveSpectrum (const std::vector<float>& mono, int end, double sampleRate)
{
    const int order = sampleRate <= 50000.0 ? 13 : (sampleRate <= 100000.0 ? 14 : 15);
    const int n = 1 << order;
    std::vector<float> data ((size_t) (2 * n), 0.0f);
    double windowSum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const float w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) n);
        const int s = end - n + i;
        data[(size_t) i] = (s >= 0 && s < (int) mono.size() ? mono[(size_t) s] : 0.0f) * w;
        windowSum += w;
    }
    fftFor (order).performFrequencyOnlyForwardTransform (data.data(), true);
    const float scale = (float) (2.0 / windowSum);
    UiSpectrum s;
    s.db.resize ((size_t) (n / 2 + 1));
    for (int k = 0; k <= n / 2; ++k)
        s.db[(size_t) k] = uiToDb (data[(size_t) k] * scale);
    s.sampleRate = sampleRate;
    s.fftSize = n;
    return s;
}

/** Parabolic peak at bin k, as SpectrumAnalyzer::findPeak interpolates. */
SpecPeak uiInterpolate (const UiSpectrum& s, int best)
{
    const double binHz = s.sampleRate / s.fftSize;
    const float a = s.db[(size_t) best - 1], b = s.db[(size_t) best], c = s.db[(size_t) best + 1];
    const float denom = a - 2.0f * b + c;
    const float delta = denom < 0.0f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / denom) : 0.0f;
    return { (float) (((double) best + (double) delta) * binHz), b - 0.25f * (a - c) * delta };
}

/** Distinct peaks 20 Hz - 20 kHz, loudest first. A distinct peak is the highest bin
    within +-2 semitones (at least +-3 bins), so window side lobes and the smeared
    skirt of a swept body don't count as peaks of their own. The first entry is
    exactly SpectrumAnalyzer::findPeak's (the global maximum). */
std::vector<SpecPeak> uiPeaks (const UiSpectrum& s)
{
    std::vector<SpecPeak> out;
    const int bins = (int) s.db.size();
    if (bins < 4 || s.fftSize <= 0)
        return out;
    const double binHz = s.sampleRate / s.fftSize;
    const int lo = juce::jmax (1, (int) std::ceil (20.0 / binHz));
    const int hi = juce::jmin (bins - 2, (int) std::floor (20000.0 / binHz));

    std::vector<int> found;
    int best = -1;
    for (int k = lo; k <= hi; ++k)
        if (best < 0 || s.db[(size_t) k] > s.db[(size_t) best])
            best = k;
    if (best < 0)
        return out;
    found.push_back (best);

    for (int k = lo; k <= hi; ++k)
    {
        if (k == best)
            continue;
        const int w = juce::jmax (3, (int) std::round (k * (std::exp2 (2.0 / 12.0) - 1.0)));
        bool isMax = true;
        for (int j = juce::jmax (lo, k - w); j <= juce::jmin (hi, k + w) && isMax; ++j)
            isMax = j == k || s.db[(size_t) j] < s.db[(size_t) k] || (j > k && ! (s.db[(size_t) j] > s.db[(size_t) k]));
        if (isMax)
            found.push_back (k);
    }
    for (int k : found)
        out.push_back (uiInterpolate (s, k));
    std::sort (out.begin() + 1, out.end(), [] (const SpecPeak& a, const SpecPeak& b) { return a.db > b.db; });
    return out;
}

struct AnalyzerReadout
{
    SpecPeak hit, hitUi, hit2;           // replicated HIT peak, the real analyzer's, the second distinct peak
    std::vector<SpecPeak> hitNear;       // other distinct peaks within 6 dB of the top
    std::vector<SpecPeak> hitTop;        // the ten loudest distinct peaks
    SpecPeak hitSub;                     // loudest distinct peak under 0.75 x the top (a subharmonic?)
    SpecPeak live, liveMin, liveMax;     // real LIVE readout (hit on the frame grid) and its range over offsets
    SpecPeak frame, frame2;              // loudest single LIVE frame (rebuilt): its peak and second peak
    float seedMinHz = kNaN, seedMaxHz = kNaN, seedMinMarginDb = kNaN; // HIT peak over other noise seeds (as live hits get)
};

juce::String noteOf (float hz)
{
    return std::isfinite (hz) && hz > 0.0f ? noteName (frequencyToMidi (hz)) : juce::String ("-");
}

/** `proc` only hosts the AnalyzerTap; it must have been prepared at `sr`. */
AnalyzerReadout analyserReadout (DrumFoundryProcessor& proc, const juce::AudioBuffer<float>& hit, double sr,
                                 const std::vector<juce::AudioBuffer<float>>& otherSeeds)
{
    AnalyzerReadout a;

    // Live hits each get a new noise seed: the HIT peak and its margin over the
    // next distinct peak, for the display's seed and the others.
    for (size_t k = 0; k <= otherSeeds.size(); ++k)
    {
        const auto ps = uiPeaks (uiHitSpectrum (k == 0 ? hit : otherSeeds[k - 1], sr));
        if (ps.empty())
            continue;
        const float margin = ps.size() > 1 ? ps[0].db - ps[1].db : 99.0f;
        a.seedMinHz = std::isfinite (a.seedMinHz) ? juce::jmin (a.seedMinHz, ps[0].hz) : ps[0].hz;
        a.seedMaxHz = std::isfinite (a.seedMaxHz) ? juce::jmax (a.seedMaxHz, ps[0].hz) : ps[0].hz;
        a.seedMinMarginDb = std::isfinite (a.seedMinMarginDb) ? juce::jmin (a.seedMinMarginDb, margin) : margin;
    }

    const auto spec = uiHitSpectrum (hit, sr);
    const auto peaks = uiPeaks (spec);
    if (! peaks.empty())
    {
        a.hit = peaks[0];
        if (peaks.size() > 1)
            a.hit2 = peaks[1];
        for (size_t i = 1; i < peaks.size() && peaks[i].db >= peaks[0].db - 6.0f; ++i)
            a.hitNear.push_back (peaks[i]);
        for (size_t i = 0; i < juce::jmin ((size_t) 10, peaks.size()); ++i)
            a.hitTop.push_back (peaks[i]);
        for (const auto& p : peaks)
            if (p.hz < 0.75f * peaks[0].hz && p.db > a.hitSub.db)
                a.hitSub = p;
    }

    {
        ui::SpectrumAnalyzer analyzer (proc);
        analyzer.setHitAudio (hit, sr);
        analyzer.setMode (ui::SpectrumAnalyzer::Mode::hit);
        if (const auto p = analyzer.getPeak(); p.valid)
            a.hitUi = { p.hz, p.db };
    }

    // LIVE through the real analyzer, the hit at several offsets into a frame.
    const int frame = (int) std::lround (sr / 30.0);
    const int n = hit.getNumSamples();
    a.liveMin.hz = 1.0e9f;
    a.liveMax.hz = -1.0f;
    bool first = true;
    int run = 0;
    for (int offset : { 0, frame / 4, frame / 2, 3 * frame / 4 })
    {
        // The first run plays the display's render; the others a new seed each.
        const auto& src = run == 0 || otherSeeds.empty() ? hit : otherSeeds[(size_t) (run - 1) % otherSeeds.size()];
        ++run;
        ui::SpectrumAnalyzer analyzer (proc);
        const int total = offset + n + (int) (0.4 * sr);
        juce::AudioBuffer<float> chunk (2, frame);
        for (int pos = 0; pos < total; pos += frame)
        {
            chunk.clear();
            for (int i = 0; i < frame; ++i)
            {
                const int s = pos + i - offset;
                if (s >= 0 && s < juce::jmin (n, src.getNumSamples()))
                    for (int c = 0; c < 2; ++c)
                        chunk.setSample (c, i, src.getSample (juce::jmin (c, src.getNumChannels() - 1), s));
            }
            proc.getAnalyzerTap().push (chunk, frame);
            analyzer.pollNow();
        }
        const auto p = analyzer.getPeak();
        if (! p.valid)
            continue;
        if (first)
            a.live = { p.hz, p.db };
        first = false;
        if (p.hz < a.liveMin.hz) a.liveMin = { p.hz, p.db };
        if (p.hz > a.liveMax.hz) a.liveMax = { p.hz, p.db };
    }

    // Loudest single frame, rebuilt, on a finer grid (every 1/4 frame).
    std::vector<float> mono ((size_t) n);
    for (int i = 0; i < n; ++i)
        mono[(size_t) i] = 0.5f * (hit.getSample (0, i) + hit.getSample (juce::jmin (1, hit.getNumChannels() - 1), i));
    for (int end = frame / 4; end < n + frame; end += frame / 4)
    {
        const auto fp = uiPeaks (uiLiveSpectrum (mono, end, sr));
        if (! fp.empty() && fp[0].db > a.frame.db)
        {
            a.frame = fp[0];
            a.frame2 = fp.size() > 1 ? fp[1] : SpecPeak {};
        }
    }
    return a;
}

void printAnalyzerHeader()
{
    std::printf ("\n%-22s | %-12s %6s | %-11s | %-24s | %-16s | %-14s | %-15s | %5s %5s %5s %5s %5s | %4s %4s %5s | %5s\n",
                 "preset", "HIT peak", "dB", "2nd -dB", "others within 6 dB", "HIT over seeds", "LIVE readout", "loudest frame",
                 "f2", "f5", "f10", "f2040", "ovr10", "bT20", "wT20", "b-w", "pkdB");
}

void printAnalyzerRow (const juce::String& name, const AnalyzerReadout& a, const Metrics& m)
{
    juce::StringArray near;
    for (const auto& p : a.hitNear)
        near.add (juce::String (juce::roundToInt (p.hz)) + "/-" + juce::String (a.hit.db - p.db, 1));
    const bool uiMatch = std::abs (a.hit.hz - a.hitUi.hz) < 0.05f;
    auto hz = [] (float v) { return ! std::isfinite (v) ? juce::String ("nan") : v < 100.0f ? juce::String (v, 1) : juce::String (juce::roundToInt (v)); };
    const auto hitText = hz (a.hit.hz) + " " + noteOf (a.hit.hz) + (uiMatch ? "" : "?");
    const auto secondText = std::isfinite (a.hit2.hz) ? hz (a.hit2.hz) + " -" + juce::String (a.hit.db - a.hit2.db, 1) : juce::String ("-");
    const auto liveText = hz (a.live.hz) + " (" + hz (a.liveMin.hz) + "-" + hz (a.liveMax.hz) + ")";
    const auto frameText = hz (a.frame.hz) + " 2nd " + (std::isfinite (a.frame2.hz) ? hz (a.frame2.hz) + "/-" + juce::String (juce::roundToInt (a.frame.db - a.frame2.db)) : "-");
    const auto seedText = hz (a.seedMinHz) + "-" + hz (a.seedMaxHz) + " m" + juce::String (a.seedMinMarginDb, 1);

    // Worst overshoot of the tracked pitch over the 20-40 ms pitch, 5-10 ms.
    float over = kNaN;
    if (std::isfinite (m.f20to40))
        for (int i : { 1, 2 })
            if (std::isfinite (m.track[(size_t) i]))
                over = std::isfinite (over) ? juce::jmax (over, semitones (m.track[(size_t) i], m.f20to40))
                                            : semitones (m.track[(size_t) i], m.f20to40);

    std::printf ("%-22s | %-12s %6.1f | %-11s | %-24s | %-16s | %-14s | %-15s | %5s %5s %5s %5s %5s | %4s %4s %5s | %5.1f\n",
                 name.substring (0, 22).toRawUTF8(), hitText.toRawUTF8(), a.hit.db, secondText.toRawUTF8(),
                 near.joinIntoString (" ").toRawUTF8(), seedText.toRawUTF8(), liveText.toRawUTF8(), frameText.toRawUTF8(),
                 fmt (m.track[0], 0).toRawUTF8(), fmt (m.track[1], 0).toRawUTF8(), fmt (m.track[2], 0).toRawUTF8(),
                 fmt (m.f20to40, 1).toRawUTF8(), fmt (over, 1).toRawUTF8(), fmt (m.bodyT20, 0).toRawUTF8(),
                 fmt (m.wireT20, 0).toRawUTF8(), fmt (m.bodyT20 - m.wireT20, 0).toRawUTF8(), m.peakDb);
}

juce::String analyzerCsvHeader()
{
    return "hitPeakHz,hitNote,hitPeakDb,hitUiHz,hit2ndHz,hit2ndBelowDb,hitWithin6dB,liveHz,liveNote,liveDb,liveMinHz,liveMaxHz,"
           "frameHz,frame2ndHz,frame2ndBelowDb,seedMinHz,seedMaxHz,seedMinMarginDb,hitSubHz,hitSubBelowDb,hitTop10";
}

juce::String analyzerCsv (const AnalyzerReadout& a)
{
    juce::StringArray top;
    for (const auto& p : a.hitTop)
        top.add (juce::String (p.hz, 1) + "/-" + juce::String (a.hit.db - p.db, 1));
    juce::StringArray near;
    for (const auto& p : a.hitNear)
        near.add (juce::String (p.hz, 1) + "/-" + juce::String (a.hit.db - p.db, 1));
    return juce::StringArray { fmt (a.hit.hz, 2), noteOf (a.hit.hz), fmt (a.hit.db, 1), fmt (a.hitUi.hz, 2), fmt (a.hit2.hz, 1),
                               fmt (a.hit.db - a.hit2.db, 1), csvQuote (near.joinIntoString (" ")), fmt (a.live.hz, 2),
                               noteOf (a.live.hz), fmt (a.live.db, 1), fmt (a.liveMin.hz, 1), fmt (a.liveMax.hz, 1),
                               fmt (a.frame.hz, 1), fmt (a.frame2.hz, 1), fmt (a.frame.db - a.frame2.db, 1),
                               fmt (a.seedMinHz, 1), fmt (a.seedMaxHz, 1), fmt (a.seedMinMarginDb, 1),
                               fmt (a.hitSub.hz, 1), fmt (a.hit.db - a.hitSub.db, 1), csvQuote (top.joinIntoString (" ")) }
        .joinIntoString (",");
}

//==============================================================================
// measure-presets

/** Parses "{ 0, 1, -0.75f }, { 0.03f, 0.1f, -0.4f }" (or plain numbers) into points. */
std::vector<EnvPoint> parsePoints (const juce::String& text)
{
    juce::StringArray nums;
    nums.addTokens (text.removeCharacters ("{}f"), ", \t", "");
    nums.removeEmptyStrings();
    std::vector<EnvPoint> pts;
    for (int i = 0; i + 2 < nums.size(); i += 3)
        pts.push_back ({ nums[i].getFloatValue(), nums[i + 1].getFloatValue(), nums[i + 2].getFloatValue() });
    return pts;
}

struct Overrides
{
    std::vector<std::pair<juce::String, float>> values;
    std::map<int, std::vector<EnvPoint>> envs;

    void set (const juce::String& key, const juce::String& value)
    {
        for (int e = 0; e < numEnvs; ++e)
            if (key.equalsIgnoreCase (envelopeName ((EnvId) e)))
            {
                envs[e] = parsePoints (value);
                return;
            }
        for (auto& [k, v] : values)
            if (k == key)
            {
                v = value.getFloatValue();
                return;
            }
        values.push_back ({ key, value.getFloatValue() });
    }

    void merge (const Overrides& other)
    {
        for (const auto& [k, v] : other.values)
            set (k, juce::String (v, 6));
        for (const auto& [e, pts] : other.envs)
            envs[e] = pts;
    }
};

/** Section name ("*" = every preset) -> overrides. */
std::map<juce::String, Overrides> loadOverridesFile (const juce::File& file)
{
    std::map<juce::String, Overrides> result;
    juce::String section = "*";
    juce::StringArray lines;
    lines.addLines (file.loadFileAsString());
    for (auto line : lines)
    {
        line = line.upToFirstOccurrenceOf ("#", false, false).upToFirstOccurrenceOf ("//", false, false).trim();
        if (line.isEmpty())
            continue;
        if (line.startsWith ("[") && line.endsWith ("]"))
            section = line.substring (1, line.length() - 1).trim();
        else if (line.contains ("="))
            result[section].set (line.upToFirstOccurrenceOf ("=", false, false).trim(),
                                 line.fromFirstOccurrenceOf ("=", false, false).trim());
    }
    return result;
}

bool isKickPreset (const juce::String& name)
{
    return name.containsIgnoreCase ("kick") || name.containsIgnoreCase ("bd ") || name.containsIgnoreCase ("808 boom");
}

juce::String num (float v)
{
    if (std::abs (v - std::round (v)) < 1.0e-4f)
        return juce::String ((int) std::round (v));
    auto s = juce::String (v, 4);
    while (s.endsWith ("0"))
        s = s.dropLastCharacters (1);
    return s + "f";
}

juce::String pointsCode (const std::vector<EnvPoint>& pts)
{
    juce::StringArray items;
    for (const auto& p : pts)
        items.add ("{ " + num (p.x) + ", " + num (p.y) + ", " + num (p.curve) + " }");
    return "{ " + items.joinIntoString (", ") + " }";
}

/** The preset with overrides merged, in FactoryPresets.cpp syntax. */
juce::String presetCode (const FactoryPreset& preset, const Overrides& o)
{
    std::vector<std::pair<juce::String, float>> values;
    for (const auto& [id, v] : preset.values)
        values.push_back ({ id, v });
    for (const auto& [id, v] : o.values)
    {
        auto it = std::find_if (values.begin(), values.end(), [&] (const auto& p) { return p.first == id; });
        if (it != values.end())
            it->second = v;
        else
            values.push_back ({ id, v });
    }

    juce::StringArray items;
    for (const auto& [id, v] : values)
    {
        const bool isLevel = id.endsWith ("Level");
        items.add ("{ " + id + ", " + (isLevel && v <= kMinLevelDb ? juce::String ("kMinLevelDb") : num (v)) + " }");
    }

    juce::String code = "        { \"" + juce::String (preset.name) + "\",\n          { " + items.joinIntoString (", ") + " },\n";
    // FactoryPreset lists the core layers' curves only.
    for (int e = 0; e < numCoreEnvs; ++e)
    {
        auto it = o.envs.find (e);
        const auto& pts = it != o.envs.end() ? it->second
                        : (preset.points (e).empty() ? defaultEnvelope ((EnvId) e).getPoints() : preset.points (e));
        code += "          " + pointsCode (pts) + (e + 1 < numCoreEnvs ? ",\n" : " },\n");
    }
    return code;
}

int cmdMeasurePresets (const juce::StringArray& args)
{
    const auto cwd = juce::File::getCurrentWorkingDirectory();
    juce::String kind = "all";
    juce::StringArray nameFilters;
    Overrides global;
    std::map<juce::String, Overrides> perPreset;
    juce::File csvFile, wavDir;
    bool emit = false;
    double sr = 48000.0;

    for (int i = 0; i < args.size(); ++i)
    {
        const auto& a = args[i];
        const bool hasNext = i + 1 < args.size();
        if (a == "kick" || a == "snare" || a == "all")
            kind = a;
        else if (a == "--preset" && hasNext)
            nameFilters.add (args[++i]);
        else if (a == "--set" && hasNext)
        {
            const auto kv = args[++i];
            global.set (kv.upToFirstOccurrenceOf ("=", false, false).trim(), kv.fromFirstOccurrenceOf ("=", false, false).trim());
        }
        else if (a == "--overrides" && hasNext)
        {
            const auto file = cwd.getChildFile (args[++i]);
            if (! file.existsAsFile())
            {
                std::fprintf (stderr, "overrides file not found: %s\n", file.getFullPathName().toRawUTF8());
                return 1;
            }
            perPreset = loadOverridesFile (file);
        }
        else if (a == "--csv" && hasNext)
            csvFile = cwd.getChildFile (args[++i]);
        else if (a == "--wav" && hasNext)
            wavDir = cwd.getChildFile (args[++i]);
        else if (a == "--sr" && hasNext)
            sr = args[++i].getDoubleValue();
        else if (a == "--emit")
            emit = true;
        else
        {
            std::printf ("usage: measure-presets [kick|snare|all] [--preset name]... [--set id=value]... [--overrides file]"
                         " [--csv out.csv] [--wav dir] [--emit] [--sr rate]\n");
            return 1;
        }
    }
    if (wavDir != juce::File())
        wavDir.createDirectory();

    DrumFoundryProcessor proc;
    auto& presets = proc.getPresets();
    const auto& entries = presets.getEntries();
    const auto& factory = factoryPresets();

    DrumFoundryProcessor tapHost; // hosts the AnalyzerTap for the LIVE readout
    tapHost.prepareToPlay (sr, 512);
    std::vector<std::pair<juce::String, std::pair<AnalyzerReadout, Metrics>>> analyzerRows;

    juce::StringArray csv { csvHeader() + ",tuneHz,pitchStartSt,lengthMs,nominal2ms,nominal20ms,nominal40ms,nominal80ms,"
                            + analyzerCsvHeader() };
    juce::StringArray code;
    printTableHeader ("preset");
    int failures = 0;

    for (int i = 0; i < (int) entries.size(); ++i)
    {
        const auto& entry = entries[(size_t) i];
        // Init, and user presets, are only measured when asked for by name (Init is handy as a
        // blank canvas for --set; a user preset to compare against).
        if ((! entry.isFactory || entry.name == "Init") && nameFilters.isEmpty())
            continue;
        if (kind != "all" && isKickPreset (entry.name) != (kind == "kick"))
            continue;
        if (! nameFilters.isEmpty())
        {
            bool match = false;
            for (const auto& f : nameFilters)
                match = match || entry.name.containsIgnoreCase (f);
            if (! match)
                continue;
        }

        Overrides o = global;
        if (auto it = perPreset.find ("*"); it != perPreset.end())
            o.merge (it->second);
        for (const auto& [section, ov] : perPreset)
            if (section != "*" && section.equalsIgnoreCase (entry.name))
                o.merge (ov);

        presets.load (i);
        for (const auto& [id, v] : o.values)
        {
            auto* param = proc.getState().getParameter (id);
            if (param == nullptr)
            {
                std::fprintf (stderr, "unknown parameter: %s\n", id.toRawUTF8());
                ++failures;
                continue;
            }
            param->setValueNotifyingHost (param->convertTo0to1 (v));
        }
        for (const auto& [e, pts] : o.envs)
            proc.setEnvelope ((EnvId) e, CurveEnvelope (pts, envelopeRange (e)));

        const auto params = proc.readParams();
        EnvTables tables;
        proc.bakeTables (tables);
        const auto hit = renderHit (params, tables, sr);
        const auto m = analyse (toChannels (hit), sr);

        const auto name = entry.name + (o.values.empty() && o.envs.empty() ? "" : " *");
        printTableRow (name, m);
        std::vector<juce::AudioBuffer<float>> otherSeeds;
        for (uint32_t k = 1; k <= 7; ++k)
            otherSeeds.push_back (renderHit (params, tables, sr, 60, 1.0f, 0x5EEDu + k * 7919u));
        const auto readout = analyserReadout (tapHost, hit, sr, otherSeeds);
        analyzerRows.push_back ({ name, { readout, m } });

        // What the synth is asked to play: Tune x 2^(pitchEnv / 12), the pitch curve being in
        // semitones relative to Tune (at note 60, i.e. without key tracking).
        const auto pitchEnv = proc.getEnvelope (envPitch);
        auto nominal = [&] (double t) {
            return (float) (params.tuneHz * std::exp2 (pitchEnv.evaluate ((float) (t / params.lengthMs)) / 12.0));
        };
        csv.add (csvRow (entry.name, o.values.empty() && o.envs.empty() ? "factory" : "override", m)
                 + "," + fmt (params.tuneHz) + "," + fmt (pitchEnv.evaluate (0.0f)) + "," + fmt (params.lengthMs)
                 + "," + fmt (nominal (2)) + "," + fmt (nominal (20)) + "," + fmt (nominal (40)) + "," + fmt (nominal (80))
                 + "," + analyzerCsv (readout));

        if (wavDir != juce::File())
            writeWav (hit, sr, wavDir.getChildFile (juce::File::createLegalFileName (entry.name).replaceCharacter (' ', '_') + ".wav"));
        if (emit && juce::isPositiveAndBelow (entry.factoryIndex, (int) factory.size()))
            code.add (presetCode (factory[(size_t) entry.factoryIndex], o));
    }

    printLegend();
    std::printf ("(* = rendered with overrides)\n");

    printAnalyzerHeader();
    for (const auto& [name, r] : analyzerRows)
        printAnalyzerRow (name, r.first, r.second);
    std::printf ("\nHIT peak: the analyzer's PEAK in HIT mode (first 200 ms of the render; '?' = differs from a real SpectrumAnalyzer)."
                 " 2nd / others: distinct spectral peaks (highest within +-2 st), dB under the top.\n"
                 "LIVE readout: real analyzer fed the hit at 30 frames/s, first offset (range over 4 offsets into a frame)."
                 " loudest frame: 8192-pt LIVE frame with the loudest peak, and its 2nd peak.\n"
                 "HIT over seeds: HIT peak range over 8 noise seeds (live hits reseed), and the smallest margin (dB) over the 2nd peak.\n"
                 "ovr10: worst tracked overshoot (st) at 5/10 ms over f2040. b-w: body minus wires -20 dB time (ms, < 0 = snare-like). pkdB: sample peak.\n");
    if (csvFile != juce::File())
        csvFile.replaceWithText (csv.joinIntoString ("\n") + "\n");
    if (emit)
        std::printf ("\n// FactoryPresets.cpp entries (overrides merged):\n%s", code.joinIntoString ("\n").toRawUTF8());
    return failures == 0 ? 0 : 1;
}

//==============================================================================
// emit-preset

/** Prints saved .drumpreset files in the FactoryPresets.cpp format, so a sound saved
    from the plug-in can become a factory preset. Each file is loaded the way the
    plug-in loads it (old formats are migrated); every parameter that differs from its
    default is listed (ids quoted: unquote the ones with a pid constant), then the core
    curves, and the chain when it isn't the default. What a factory preset can't hold
    (samples, an extra layer's own curves) is reported on stderr. */
int cmdEmitPreset (const juce::StringArray& args)
{
    if (args.isEmpty())
    {
        std::fprintf (stderr, "usage: emit-preset <file.drumpreset>...\n");
        return 1;
    }

    int failures = 0;
    for (const auto& arg : args)
    {
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (arg);
        const auto xml = juce::parseXML (file);
        if (xml == nullptr)
        {
            std::fprintf (stderr, "%s: not readable\n", file.getFullPathName().toRawUTF8());
            ++failures;
            continue;
        }

        DrumFoundryProcessor proc;
        proc.loadStateTree (juce::ValueTree::fromXml (*xml));
        const auto state = proc.createStateTree();
        const auto name = file.getFileNameWithoutExtension();

        juce::StringArray items;
        for (auto* param : proc.getParameters())
        {
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param);
            if (ranged == nullptr || sessionOnlyParameterIds().contains (ranged->paramID))
                continue;
            float v = ranged->convertFrom0to1 (ranged->getValue());
            const float def = ranged->convertFrom0to1 (ranged->getDefaultValue());
            if (std::abs (v - def) <= 1.0e-4f * juce::jmax (1.0f, std::abs (def)))
                continue;

            // Saved values carry float noise (900.0002): keep the fewest decimals that are within 0.02%.
            for (int decimals = 0; decimals <= 4; ++decimals)
            {
                const float scale = std::pow (10.0f, (float) decimals);
                const float r = std::round (v * scale) / scale;
                if (std::abs (r - v) <= 2.0e-4f * juce::jmax (1.0f, std::abs (v)))
                {
                    v = r;
                    break;
                }
            }
            const bool isLevel = ranged->paramID.endsWith ("Level");
            items.add ("{ \"" + ranged->paramID + "\", " + (isLevel && v <= kMinLevelDb ? juce::String ("kMinLevelDb") : num (v)) + " }");
        }

        // The optional curves are positional: list up to the last one that isn't default.
        juce::StringArray curves;
        int lastCurve = 2;
        for (int e = 3; e < numCoreEnvs; ++e)
            if (! (proc.getEnvelope ((EnvId) e) == defaultEnvelope ((EnvId) e)))
                lastCurve = e;
        for (int e = 0; e <= lastCurve; ++e)
        {
            const auto env = proc.getEnvelope ((EnvId) e);
            curves.add (e > 2 && env == defaultEnvelope ((EnvId) e) ? juce::String ("{}") : pointsCode (env.getPoints()));
        }

        const auto chain = state.getProperty (DrumFoundryProcessor::fxChainId).toString();
        const bool customChain = chain.isNotEmpty() && chain != FxChain::defaults().toString();
        if (customChain)
            while (curves.size() < numCoreEnvs)
                curves.add ("{}");

        juce::String code = "        { \"" + name + "\",\n          { " + items.joinIntoString (", ") + " },\n";
        for (int i = 0; i < curves.size(); ++i)
            code += "          " + curves[i] + (i + 1 < curves.size() || customChain ? ",\n" : " },\n");
        if (customChain)
            code += "          \"" + chain + "\" },\n";
        std::printf ("%s\n", code.toRawUTF8());

        for (int e = numCoreEnvs; e < numEnvs; ++e)
            if (! (proc.getEnvelope ((EnvId) e) == defaultEnvelope ((EnvId) e)))
                std::fprintf (stderr, "%s: the %s curve isn't the default, and a factory preset can't hold it\n",
                              name.toRawUTF8(), envelopeName ((EnvId) e));
        if (const auto smp = state.getChildWithName ("SAMPLES"); smp.isValid() && smp.getNumChildren() > 0)
            std::fprintf (stderr, "%s: uses samples, which a factory preset can't hold\n", name.toRawUTF8());
    }
    return failures == 0 ? 0 : 1;
}

} // namespace

int run (const juce::String& command, const juce::StringArray& args)
{
    if (command == "emit-preset")
        return cmdEmitPreset (args);
    return command == "measure-presets" ? cmdMeasurePresets (args) : cmdMeasure (args);
}

} // namespace drum::measure
