// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

// Click analysis for the Drum Foundry dev tool. Not shipped.
//
//   measure-clicks <file-or-dir>... [--csv out.csv] [--ours]
//       Measures short transients (kick clicks, beaters, ticks) and prints a per-group
//       summary (median of each feature; the group is the file's parent folder).
//       --ours adds the plug-in's own synthesised click types (ClickGen) as groups,
//       so they can be compared with real ones on the same features.
//
// Features (all from the onset, see findOnset()):
//   len40     ms until the 0.5 ms RMS envelope has fallen 40 dB for good
//   atk       ms from the onset to the envelope's peak
//   crest     peak / RMS over the first 20 ms, dB
//   c0 c1 c2  spectral centroid (Hz) over 0-2, 2-6 and 6-20 ms: how fast it darkens
//   flat      spectral flatness 1-10 kHz over the first 20 ms (1 = white noise, 0 = pure tones)
//   dLo..dAir ms to fall 20 dB in the bands <300, 300-1.5k, 1.5-6k, >6 kHz
//   lowPct    % of the first 50 ms energy under 150 Hz
//   impulses  separate peaks in the first 15 ms (over 30% of the biggest, 0.7 ms apart)
//   peaks     up to 3 resonances: spectrum bins standing over 8 dB above the
//             1/3-octave smoothed spectrum (first 50 ms), with their prominence
//   bands     half-octave band levels over the first 30 ms, dB under the loudest band
//             (63 Hz .. 16 kHz), for the group's average spectral shape

#include "dsp/NoiseClickGen.h"
#include "dsp/StyleInfo.h"
#include "Parameters.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <complex>
#include <map>

namespace drum::clicks
{

namespace
{

constexpr int kNumBands = 19;   // half octaves, 63 Hz .. 16 kHz
constexpr double kMaxSeconds = 2.0;

using Mono = std::vector<float>;

struct Features
{
    float len40 = 0, atk = 0, crest = 0, c0 = 0, c1 = 0, c2 = 0, flat = 0;
    float decay[4] {};
    float lowPct = 0;
    int impulses = 0;
    float peakHz[3] {}, peakDb[3] {};
    float bands[kNumBands] {};
};

double bandCentre (int b) { return 63.0 * std::pow (2.0, 0.5 * b); }

juce::dsp::FFT& fftFor (int order)
{
    static std::map<int, std::unique_ptr<juce::dsp::FFT>> cache;
    auto& f = cache[order];
    if (f == nullptr)
        f = std::make_unique<juce::dsp::FFT> (order);
    return *f;
}

/** Power spectrum (|X|^2 per bin, bins 0 .. size/2) of x[start, start + len), with a
    short raised-cosine fade at both ends, zero-padded to 2^order. */
std::vector<double> powerSpectrum (const Mono& x, int start, int len, int order)
{
    const int size = 1 << order;
    len = juce::jmin (len, size);
    std::vector<std::complex<float>> in ((size_t) size), out ((size_t) size);
    const int fade = juce::jmax (1, len / 16);
    for (int i = 0; i < len; ++i)
    {
        const int s = start + i;
        float v = s >= 0 && s < (int) x.size() ? x[(size_t) s] : 0.0f;
        if (i < fade)
            v *= 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) i / (float) fade);
        else if (i >= len - fade)
            v *= 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (len - 1 - i) / (float) fade);
        in[(size_t) i] = v;
    }
    fftFor (order).perform (in.data(), out.data(), false);
    std::vector<double> p ((size_t) size / 2 + 1);
    for (size_t k = 0; k < p.size(); ++k)
        p[k] = std::norm (out[k]);
    return p;
}

double centroid (const std::vector<double>& p, double binHz, double loHz, double hiHz)
{
    double num = 0, den = 0;
    for (size_t k = 1; k < p.size(); ++k)
    {
        const double f = (double) k * binHz;
        if (f < loHz || f > hiHz)
            continue;
        num += f * p[k];
        den += p[k];
    }
    return den > 0 ? num / den : 0.0;
}

struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    static Biquad make (bool high, double hz, double sr)
    {
        Biquad q;
        const double w = juce::MathConstants<double>::twoPi * juce::jlimit (1.0, sr * 0.49, hz) / sr;
        const double cw = std::cos (w), alpha = std::sin (w) / (2.0 * 0.7071), a0 = 1.0 + alpha;
        q.b0 = (high ? 1.0 + cw : 1.0 - cw) * 0.5 / a0;
        q.b1 = (high ? -(1.0 + cw) : 1.0 - cw) / a0;
        q.b2 = q.b0;
        q.a1 = -2.0 * cw / a0;
        q.a2 = (1.0 - alpha) / a0;
        return q;
    }

    float process (float x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float) y;
    }
};

/** x through a band (4th-order Butterworth-ish edges; lo 0 = no low cut, hi 0 = none). */
Mono band (const Mono& x, double lo, double hi, double sr)
{
    std::vector<Biquad> st;
    if (lo > 0) { st.push_back (Biquad::make (true, lo, sr)); st.push_back (Biquad::make (true, lo, sr)); }
    if (hi > 0) { st.push_back (Biquad::make (false, hi, sr)); st.push_back (Biquad::make (false, hi, sr)); }
    Mono y = x;
    for (auto& v : y)
        for (auto& q : st)
            v = q.process (v);
    return y;
}

/** 0.5 ms RMS envelope in dB, every 0.25 ms from `onset`. */
std::vector<float> envDb (const Mono& x, int onset, double sr)
{
    const int hop = juce::jmax (1, (int) (0.00025 * sr)), win = juce::jmax (2, (int) (0.0005 * sr));
    std::vector<float> e;
    for (int s = onset; s + win <= (int) x.size(); s += hop)
    {
        double acc = 0;
        for (int i = s; i < s + win; ++i)
            acc += (double) x[(size_t) i] * x[(size_t) i];
        e.push_back ((float) (10.0 * std::log10 (acc / win + 1.0e-20)));
    }
    return e;
}

/** ms (0.25 ms frames) until the envelope has fallen `drop` dB from its peak for good. */
float fallMs (const std::vector<float>& e, float drop)
{
    if (e.empty())
        return 0;
    const auto pk = std::max_element (e.begin(), e.end());
    const float thr = *pk - drop;
    int last = (int) (pk - e.begin());
    for (int k = last; k < (int) e.size(); ++k)
        if (e[(size_t) k] >= thr)
            last = k;
    return 0.25f * (float) last;
}

int findOnset (const Mono& x)
{
    float peak = 0;
    for (auto v : x)
        peak = juce::jmax (peak, std::abs (v));
    if (peak <= 0)
        return 0;
    int i = 0;
    while (i < (int) x.size() && std::abs (x[(size_t) i]) < peak * 0.1f)
        ++i;
    const int limit = juce::jmax (0, i - 240);
    while (i > limit && std::abs (x[(size_t) i - 1]) >= peak * 0.01f)
        --i;
    return i;
}

Features analyse (const Mono& x, double sr)
{
    Features f;
    const int onset = findOnset (x);
    const auto ms = [sr] (double t) { return (int) std::round (t * 0.001 * sr); };

    // Envelope, attack, length, crest.
    const auto e = envDb (x, onset, sr);
    if (e.empty())
        return f;
    f.atk = 0.25f * (float) (std::max_element (e.begin(), e.end()) - e.begin());
    f.len40 = fallMs (e, 40.0f);
    {
        float pk = 0;
        double acc = 0;
        const int n = juce::jmin ((int) x.size() - onset, ms (20));
        for (int i = 0; i < n; ++i)
        {
            const float v = x[(size_t) (onset + i)];
            pk = juce::jmax (pk, std::abs (v));
            acc += (double) v * v;
        }
        f.crest = n > 0 ? (float) (20.0 * std::log10 (pk / std::sqrt (acc / n + 1.0e-20) + 1.0e-9)) : 0.0f;
    }

    // Centroids over 0-2, 2-6, 6-20 ms.
    constexpr int ord = 13;
    const double binHz = sr / (double) (1 << ord);
    f.c0 = (float) centroid (powerSpectrum (x, onset, ms (2), ord), binHz, 40, 20000);
    f.c1 = (float) centroid (powerSpectrum (x, onset + ms (2), ms (4), ord), binHz, 40, 20000);
    f.c2 = (float) centroid (powerSpectrum (x, onset + ms (6), ms (14), ord), binHz, 40, 20000);

    // Flatness 1-10 kHz, first 20 ms.
    {
        const auto p = powerSpectrum (x, onset, ms (20), ord);
        double logSum = 0, sum = 0;
        int n = 0;
        for (size_t k = 1; k < p.size(); ++k)
        {
            const double hz = (double) k * binHz;
            if (hz < 1000 || hz > 10000)
                continue;
            logSum += std::log (p[k] + 1.0e-20);
            sum += p[k];
            ++n;
        }
        f.flat = n > 0 && sum > 0 ? (float) (std::exp (logSum / n) / (sum / n)) : 0.0f;
    }

    // Band decays.
    const double edges[5] { 0, 300, 1500, 6000, 0 };
    for (int b = 0; b < 4; ++b)
        f.decay[b] = fallMs (envDb (band (x, edges[b], edges[b + 1], sr), onset, sr), 20.0f);

    // Low share, resonances (first 50 ms).
    {
        const auto p = powerSpectrum (x, onset, ms (50), 14);
        const double bh = sr / (double) (1 << 14);
        double low = 0, all = 0;
        for (size_t k = 1; k < p.size(); ++k)
        {
            all += p[k];
            if ((double) k * bh < 150)
                low += p[k];
        }
        f.lowPct = all > 0 ? (float) (100.0 * low / all) : 0.0f;

        // dB spectrum, smoothed over 1/3 octave, peaks standing out of it.
        std::vector<double> db (p.size()), sm (p.size());
        for (size_t k = 0; k < p.size(); ++k)
            db[k] = 10.0 * std::log10 (p[k] + 1.0e-20);
        std::vector<double> cum (p.size() + 1, 0.0);
        for (size_t k = 0; k < p.size(); ++k)
            cum[k + 1] = cum[k] + db[k];
        for (size_t k = 1; k < p.size(); ++k)
        {
            const int lo = juce::jmax (1, (int) ((double) k / 1.122)), hi = juce::jmin ((int) p.size() - 1, (int) ((double) k * 1.122) + 1);
            sm[k] = (cum[(size_t) hi + 1] - cum[(size_t) lo]) / (double) (hi - lo + 1);
        }
        const double maxDb = *std::max_element (db.begin() + 1, db.end());
        struct P { double hz, prom; };
        std::vector<P> found;
        for (size_t k = 2; k + 2 < p.size(); ++k)
        {
            const double hz = (double) k * bh;
            if (hz < 150 || hz > 12000)
                continue;
            const double prom = db[k] - sm[k];
            if (prom > 8 && db[k] > maxDb - 40 && db[k] >= db[k - 1] && db[k] >= db[k + 1] && db[k] >= db[k - 2] && db[k] >= db[k + 2])
                found.push_back ({ hz, prom });
        }
        std::sort (found.begin(), found.end(), [] (const P& a, const P& b) { return a.prom > b.prom; });
        int taken = 0;
        for (const auto& pk : found)
        {
            bool near = false;
            for (int t = 0; t < taken; ++t)
                near = near || std::abs (std::log2 (pk.hz / f.peakHz[t])) < 0.1;
            if (near)
                continue;
            f.peakHz[taken] = (float) pk.hz;
            f.peakDb[taken] = (float) pk.prom;
            if (++taken == 3)
                break;
        }
    }

    // Half-octave bands over the first 30 ms.
    {
        const auto p = powerSpectrum (x, onset, ms (30), 14);
        const double bh = sr / (double) (1 << 14);
        double e2[kNumBands] {};
        for (size_t k = 1; k < p.size(); ++k)
        {
            const double hz = (double) k * bh;
            const int b = (int) std::floor (2.0 * std::log2 (hz / 63.0) + 0.5);
            if (b >= 0 && b < kNumBands)
                e2[b] += p[k];
        }
        double mx = 0;
        for (auto v : e2)
            mx = juce::jmax (mx, v);
        for (int b = 0; b < kNumBands; ++b)
            f.bands[b] = (float) (10.0 * std::log10 ((e2[b] + 1.0e-20) / (mx + 1.0e-20)));
    }

    // Sub-impulses in the first 15 ms: peaks of a 0.25 ms max-hold of |x|.
    {
        const int hold = juce::jmax (1, ms (0.25)), n = juce::jmin ((int) x.size() - onset, ms (15)), gap = ms (0.7);
        std::vector<float> h;
        for (int s = 0; s + hold <= n; s += hold)
        {
            float m = 0;
            for (int i = s; i < s + hold; ++i)
                m = juce::jmax (m, std::abs (x[(size_t) (onset + i)]));
            h.push_back (m);
        }
        const float big = h.empty() ? 0.0f : *std::max_element (h.begin(), h.end());
        int lastAt = -1000000;
        for (int k = 1; k + 1 < (int) h.size(); ++k)
            if (h[(size_t) k] > 0.3f * big && h[(size_t) k] >= h[(size_t) k - 1] && h[(size_t) k] > h[(size_t) k + 1]
                && (k - lastAt) * hold >= gap)
            {
                ++f.impulses;
                lastAt = k;
            }
    }
    return f;
}

float median (std::vector<float> v)
{
    v.erase (std::remove_if (v.begin(), v.end(), [] (float a) { return ! std::isfinite (a); }), v.end());
    if (v.empty())
        return 0;
    std::sort (v.begin(), v.end());
    return v[v.size() / 2];
}

/** One of our click types, rendered through ClickGen as the Click layer plays it
    (no envelope curve), at Tone `tone` and Length `lenMs`. */
Mono renderOurs (int type, float tone, float lenMs, uint32_t seed, double sr)
{
    ClickGen g;
    g.start (type, tone, lenMs, true, seed, (float) sr);
    Mono y;
    while (g.active && y.size() < (size_t) (sr * 0.5))
    {
        g.follow (0.0f);
        y.push_back (g.next());
    }
    y.resize (y.size() + (size_t) (0.05 * sr), 0.0f);
    return y;
}

} // namespace

int run (const juce::StringArray& args)
{
    const auto cwd = juce::File::getCurrentWorkingDirectory();
    juce::File csvFile;
    bool ours = false;
    juce::String traceTarget;   // a click type name (ours) or a file: print its first 12 ms
    juce::File wavDir;          // --wav: write the modelled types (4 hits each, and Tone / Length variants) here
    juce::Array<juce::File> files;

    for (int i = 0; i < args.size(); ++i)
    {
        if (args[i] == "--csv" && i + 1 < args.size())
            csvFile = cwd.getChildFile (args[++i]);
        else if (args[i] == "--ours")
            ours = true;
        else if (args[i] == "--trace" && i + 1 < args.size())
            traceTarget = args[++i];
        else if (args[i] == "--wav" && i + 1 < args.size())
            wavDir = cwd.getChildFile (args[++i]);
        else
        {
            const auto f = cwd.getChildFile (args[i]);
            if (f.isDirectory())
            {
                for (const auto& entry : juce::RangedDirectoryIterator (f, true, "*", juce::File::findFiles))
                    if (entry.getFile().hasFileExtension ("wav;aif;aiff;flac"))
                        files.add (entry.getFile());
            }
            else if (f.existsAsFile())
                files.add (f);
        }
    }

    std::map<juce::String, std::vector<Features>> groups;
    juce::StringArray csv;
    {
        juce::String h = "group,file,len40,atk,crest,c0,c1,c2,flat,dLo,dMid,dHi,dAir,lowPct,impulses,pk1,pk1db,pk2,pk2db,pk3,pk3db";
        for (int b = 0; b < kNumBands; ++b)
            h << ",b" << juce::String ((int) std::round (bandCentre (b)));
        csv.add (h);
    }
    const auto addRow = [&] (const juce::String& group, const juce::String& name, const Features& f)
    {
        groups[group].push_back (f);
        juce::String r;
        r << "\"" << group << "\",\"" << name << "\"," << f.len40 << "," << f.atk << "," << f.crest << "," << f.c0 << "," << f.c1
          << "," << f.c2 << "," << f.flat << "," << f.decay[0] << "," << f.decay[1] << "," << f.decay[2] << "," << f.decay[3]
          << "," << f.lowPct << "," << f.impulses;
        for (int k = 0; k < 3; ++k)
            r << "," << f.peakHz[k] << "," << f.peakDb[k];
        for (auto b : f.bands)
            r << "," << b;
        csv.add (r);
    };

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    if (wavDir != juce::File())
    {
        constexpr double sr = 48000.0;
        wavDir.createDirectory();
        const auto write = [&] (const Mono& x, const juce::String& name)
        {
            juce::AudioBuffer<float> b (1, (int) x.size());
            for (int i = 0; i < (int) x.size(); ++i)
                b.setSample (0, i, 0.8f * x[(size_t) i]);
            const auto file = wavDir.getChildFile (name + ".wav");
            file.deleteFile();
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
            if (auto w = juce::WavAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sr)
                                                                              .withNumChannels (1).withBitsPerSample (24)))
                w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        };
        for (int type = clickFelt; type <= clickTok; ++type)
        {
            const float tone = ImpactGen::material (type - clickFelt).refTone;
            // Four hits in a row (every hit differs), 250 ms apart.
            Mono row ((size_t) sr, 0.0f);
            for (uint32_t h = 0; h < 4; ++h)
            {
                const auto x = renderOurs (type, tone, 8.0f, 0x5EEDu + h * 7919u, sr);
                for (size_t i = 0; i < x.size() && h * 12000 + i < row.size(); ++i)
                    row[h * 12000 + i] += x[i];
            }
            write (row, clickTypeNames[type] + " x4");
            write (renderOurs (type, tone * 0.6f, 8.0f, 0x5EEDu, sr), clickTypeNames[type] + " Tone low");
            write (renderOurs (type, tone * 1.6f, 8.0f, 0x5EEDu, sr), clickTypeNames[type] + " Tone high");
            write (renderOurs (type, tone, 20.0f, 0x5EEDu, sr), clickTypeNames[type] + " Length 20");
        }
        for (int type : { (int) clickBeater, (int) clickTick })
        {
            Mono row ((size_t) sr, 0.0f);
            for (uint32_t h = 0; h < 4; ++h)
            {
                const auto x = renderOurs (type, 4000.0f, 6.0f, 0x5EEDu + h * 7919u, sr);
                for (size_t i = 0; i < x.size() && h * 12000 + i < row.size(); ++i)
                    row[h * 12000 + i] += x[i];
            }
            write (row, "old " + clickTypeNames[type] + " x4");
        }
        std::printf ("wrote %s\n", wavDir.getFullPathName().toRawUTF8());
        return 0;
    }

    if (traceTarget.isNotEmpty())
    {
        Mono x;
        double sr = 48000.0;
        const int type = clickTypeNames.indexOf (traceTarget);
        if (type >= clickFelt && type <= clickTok)
            x = renderOurs (type, ImpactGen::material (type - clickFelt).refTone, 8.0f, 0x5EEDu, sr);
        else if (std::unique_ptr<juce::AudioFormatReader> r { formats.createReaderFor (cwd.getChildFile (traceTarget)) })
        {
            sr = r->sampleRate;
            juce::AudioBuffer<float> b (1, (int) r->lengthInSamples);
            r->read (&b, 0, b.getNumSamples(), 0, true, false);
            x.assign (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
        }
        const int onset = findOnset (x), frame = (int) (0.00025 * sr);
        float peak = 0;
        for (auto v : x)
            peak = juce::jmax (peak, std::abs (v));
        std::printf ("trace %s (onset at %d, peak %.3f)\n  ms    dB  centroid\n", traceTarget.toRawUTF8(), onset, peak);
        for (int k = 0; k < 48 && onset + (k + 1) * frame < (int) x.size(); ++k)
        {
            double e = 0;
            for (int i = 0; i < frame; ++i)
                e += (double) x[(size_t) (onset + k * frame + i)] * x[(size_t) (onset + k * frame + i)];
            const double c = centroid (powerSpectrum (x, onset + k * frame - frame, 3 * frame, 12), sr / 4096.0, 40, 20000);
            std::printf ("%5.2f %6.1f %6.0f %s\n", 0.25 * k, 10.0 * std::log10 (e / frame / (peak * peak) + 1e-12), c,
                         std::string ((size_t) juce::jmax (0.0, 40.0 + 10.0 * std::log10 (e / frame / (peak * peak) + 1e-12)), '#').c_str());
        }
        return 0;
    }

    for (const auto& file : files)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr || reader->sampleRate <= 0 || reader->lengthInSamples < 64
            || (double) reader->lengthInSamples / reader->sampleRate > kMaxSeconds)
            continue;
        const int ch = juce::jlimit (1, 2, (int) reader->numChannels);
        juce::AudioBuffer<float> b (ch, (int) reader->lengthInSamples);
        reader->read (&b, 0, b.getNumSamples(), 0, true, ch > 1);
        Mono m ((size_t) b.getNumSamples());
        for (int i = 0; i < b.getNumSamples(); ++i)
            m[(size_t) i] = ch > 1 ? 0.5f * (b.getSample (0, i) + b.getSample (1, i)) : b.getSample (0, i);
        addRow (file.getParentDirectory().getFileName(), file.getFileName(), analyse (m, reader->sampleRate));
    }

    if (ours)
    {
        constexpr double sr = 48000.0;
        for (int type : { (int) clickTick, (int) clickSnap, (int) clickStick, (int) clickRim, (int) clickPop, (int) clickBeater })
            for (float tone : { 1500.0f, 3000.0f, 5000.0f, 8000.0f })
                for (uint32_t s = 0; s < 3; ++s)
                    addRow ("OURS " + clickTypeNames[type], juce::String (tone, 0) + " Hz",
                            analyse (renderOurs (type, tone, 5.0f, 0x5EEDu + s * 7919u, sr), sr));

        // The modelled types at their own Tone and Length 8 ms (the material as designed),
        // over several seeds, since every hit differs.
        for (int type = clickFelt; type <= clickTok; ++type)
        {
            const float tone = ImpactGen::material (type - clickFelt).refTone;
            for (uint32_t s = 0; s < 8; ++s)
                addRow ("OURS " + clickTypeNames[type], juce::String (tone, 0) + " Hz",
                        analyse (renderOurs (type, tone, 8.0f, 0x5EEDu + s * 7919u, sr), sr));
        }
    }

    std::printf ("%-28s %4s %6s %5s %5s %6s %6s %6s %5s %5s %5s %5s %5s %5s %4s | %s\n", "group", "n", "len40", "atk", "crest",
                 "c0", "c1", "c2", "flat", "dLo", "dMid", "dHi", "dAir", "low%", "imp", "spectrum 63 Hz .. 32 kHz, half octaves (dB under the top band, /3)");
    for (const auto& [name, list] : groups)
    {
        const auto med = [&list] (auto get) { std::vector<float> v; for (const auto& f : list) v.push_back (get (f)); return median (v); };
        std::printf ("%-28s %4d %6.1f %5.2f %5.1f %6.0f %6.0f %6.0f %5.2f %5.1f %5.1f %5.1f %5.1f %5.1f %4.0f | ",
                     name.substring (0, 28).toRawUTF8(), (int) list.size(),
                     med ([] (const Features& f) { return f.len40; }), med ([] (const Features& f) { return f.atk; }),
                     med ([] (const Features& f) { return f.crest; }), med ([] (const Features& f) { return f.c0; }),
                     med ([] (const Features& f) { return f.c1; }), med ([] (const Features& f) { return f.c2; }),
                     med ([] (const Features& f) { return f.flat; }), med ([] (const Features& f) { return f.decay[0]; }),
                     med ([] (const Features& f) { return f.decay[1]; }), med ([] (const Features& f) { return f.decay[2]; }),
                     med ([] (const Features& f) { return f.decay[3]; }), med ([] (const Features& f) { return f.lowPct; }),
                     med ([] (const Features& f) { return (float) f.impulses; }));
        for (int b = 0; b < kNumBands; ++b)
        {
            const float v = med ([b] (const Features& f) { return f.bands[b]; });
            std::printf ("%3d", (int) std::round (-v / 3.0f));
        }
        std::printf ("\n");
    }
    std::printf ("\nlen40: ms to -40 dB. atk: ms to peak. crest: dB over 20 ms. c0/c1/c2: centroid (Hz) 0-2 / 2-6 / 6-20 ms.\n"
                 "flat: 1-10 kHz flatness (1 = white). dLo..dAir: ms to -20 dB in <300 / 300-1.5k / 1.5-6k / >6k Hz.\n"
                 "low%%: energy under 150 Hz. imp: separate peaks in the first 15 ms. Spectrum: half-octave bands, each\n"
                 "digit = 3 dB under the loudest band (0 = loudest); the last two (23, 32 kHz) are mostly past Nyquist.\n");

    if (csvFile != juce::File())
        csvFile.replaceWithText (csv.joinIntoString ("\n") + "\n");
    return 0;
}

} // namespace drum::clicks
