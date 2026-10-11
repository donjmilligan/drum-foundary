// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "ParametricEq.h"
#include <complex>

namespace drum
{

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    /** An analog prototype section, in s normalised to the band's frequency (s = j f / fc):
        H(s) = (b2 s^2 + b1 s + b0) / (a2 s^2 + a1 s + a0). A first-order one has b2 = a2 = 0. */
    struct Proto
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;
        bool firstOrder() const noexcept { return a2 == 0.0 && b2 == 0.0; }

        /** |H|^2 at x = f / fc. */
        double magnitudeSquared (double x) const noexcept
        {
            const std::complex<double> s (0.0, x);
            const auto h = (b2 * s * s + b1 * s + b0) / (a2 * s * s + a1 * s + a0);
            return std::norm (h);
        }
    };

    struct Protos
    {
        std::array<Proto, EqDesign::kMaxSections> p {};
        int count = 0;
        void add (const Proto& x) noexcept { if (count < EqDesign::kMaxSections) p[(size_t) count++] = x; }
    };

    double clampFreq (double hz, double sampleRate) noexcept
    {
        return juce::jlimit (10.0, sampleRate * 0.49, std::isfinite (hz) ? hz : 1000.0);
    }

    /** The band's analog sections at `gainDb`, and the Q the style gives it. */
    Protos prototypes (const DrumParams::EqBand& band, int style, float gainDb)
    {
        Protos out;
        const int type = juce::jlimit (0, numEqTypes - 1, band.type);
        double q = juce::jlimit (0.05, 40.0, std::isfinite (band.q) ? (double) band.q : 0.7071);
        const double g = std::isfinite (gainDb) ? (double) gainDb : 0.0;

        if (eqTypeHasGain (type) && std::abs (g) < 1.0e-3)
            return out; // flat

        if (style == eqStyleBritish)
            q *= type == eqBell ? 1.0 + std::abs (g) / 12.0 : type == eqLowShelf || type == eqHighShelf ? 1.15 : 1.0;
        else if (style == eqStylePultec)
            q *= type == eqBell ? 0.6 : type == eqLowShelf || type == eqHighShelf ? 2.0 : 1.0;

        const double A = std::pow (10.0, g / 40.0);
        const double sa = std::sqrt (A);
        switch (type)
        {
            case eqBell:      out.add ({ 1.0, A / q, 1.0, 1.0, 1.0 / (A * q), 1.0 }); break;
            case eqLowShelf:  out.add ({ A * A, A * sa / q, A, 1.0, sa / q, A }); break;
            case eqHighShelf: out.add ({ A, A * sa / q, A * A, A, sa / q, 1.0 }); break;
            case eqNotch:     out.add ({ 1.0, 0.0, 1.0, 1.0, 1.0 / q, 1.0 }); break;
            case eqBandPass:  out.add ({ 0.0, 1.0 / q, 0.0, 1.0, 1.0 / q, 1.0 }); break;
            case eqTilt:      out.add ({ 1.0, A, 0.0, A, 1.0, 0.0 }); break; // (A s + 1) / (s + A): 1/A at the bottom, A at the top
            case eqLowCut:
            case eqHighCut:
            {
                // Butterworth of the slope's order; Q above 0.71 lifts the sharpest section.
                const bool high = type == eqLowCut;
                const int order = eqSlopeOrder (band.slope);
                const double resonance = q / 0.70710678;
                if (order % 2 == 1)
                    out.add (high ? Proto { 0.0, 1.0, 0.0, 1.0, 1.0, 0.0 } : Proto { 1.0, 0.0, 0.0, 1.0, 1.0, 0.0 });
                const int pairs = order / 2;
                for (int k = 1; k <= pairs; ++k)
                {
                    const double angle = order % 2 == 1 ? kPi * k / order : kPi * (2 * k - 1) / (2.0 * order);
                    double qk = 1.0 / (2.0 * std::cos (angle));
                    if (k == pairs) // the sharpest (highest Q) section
                        qk *= resonance;
                    out.add (high ? Proto { 0.0, 0.0, 1.0, 1.0, 1.0 / qk, 1.0 } : Proto { 1.0, 0.0, 0.0, 1.0, 1.0 / qk, 1.0 });
                }
                break;
            }
            default: break;
        }
        return out;
    }

    /** Bilinear transform, prewarped so the band's frequency lands where it should. */
    EqSection bilinear (const Proto& p, double fc, double sampleRate)
    {
        const double K = std::tan (kPi * fc / sampleRate);
        const double k1 = 1.0 / K, k2 = k1 * k1;
        double n0, n1, n2, d0, d1, d2;
        if (p.firstOrder())
        {
            n0 = p.b1 * k1 + p.b0; n1 = p.b0 - p.b1 * k1; n2 = 0.0;
            d0 = p.a1 * k1 + p.a0; d1 = p.a0 - p.a1 * k1; d2 = 0.0;
        }
        else
        {
            n0 = p.b2 * k2 + p.b1 * k1 + p.b0; n1 = 2.0 * (p.b0 - p.b2 * k2); n2 = p.b2 * k2 - p.b1 * k1 + p.b0;
            d0 = p.a2 * k2 + p.a1 * k1 + p.a0; d1 = 2.0 * (p.a0 - p.a2 * k2); d2 = p.a2 * k2 - p.a1 * k1 + p.a0;
        }
        const double inv = 1.0 / d0;
        return { n0 * inv, n1 * inv, n2 * inv, d1 * inv, d2 * inv };
    }

    /** Matched magnitude (Vicanek): the analog poles by impulse invariance, then zeros
        that give the analog |H| at DC, at Nyquist and at the matching frequency. */
    EqSection matched (const Proto& p, double fc, double sampleRate)
    {
        const double nyquistX = 0.5 * sampleRate / fc;
        const double h0 = p.magnitudeSquared (0.0), hn = p.magnitudeSquared (nyquistX);

        if (p.firstOrder())
        {
            const double wp = 2.0 * kPi * fc * (p.a0 / p.a1) / sampleRate;
            const double pole = std::exp (-wp);
            const double r0 = std::sqrt (h0) * (1.0 - pole), r1 = std::sqrt (hn) * (1.0 + pole);
            return { 0.5 * (r0 + r1), 0.5 * (r0 - r1), 0.0, -pole, 0.0 };
        }

        const double wn = std::sqrt (p.a0 / p.a2);                // pole frequency, in units of fc
        const double zeta = p.a1 / (2.0 * std::sqrt (p.a0 * p.a2));
        const double w = 2.0 * kPi * fc * wn / sampleRate;        // rad / sample
        const double decay = std::exp (-zeta * w);
        const double a1 = zeta <= 1.0 ? -2.0 * decay * std::cos (w * std::sqrt (1.0 - zeta * zeta))
                                      : -2.0 * decay * std::cosh (w * std::sqrt (zeta * zeta - 1.0));
        const double a2 = decay * decay;

        // |A(e^jw)|^2 = A0 phi0 + A1 phi1 + A2 phi2, with phi1 = sin^2 (w / 2), phi0 = 1 - phi1, phi2 = 4 phi0 phi1.
        const double A0 = (1.0 + a1 + a2) * (1.0 + a1 + a2), A1 = (1.0 - a1 + a2) * (1.0 - a1 + a2), A2 = -4.0 * a2;
        const double fm = std::min (fc, 0.45 * sampleRate);
        const double s = std::sin (kPi * fm / sampleRate);
        const double phi1 = s * s, phi0 = 1.0 - phi1, phi2 = 4.0 * phi0 * phi1;
        const double hm = p.magnitudeSquared (fm / fc);

        const double B0 = h0 * A0, B1 = hn * A1;
        const double B2 = (hm * (A0 * phi0 + A1 * phi1 + A2 * phi2) - B0 * phi0 - B1 * phi1) / phi2;

        const double r0 = std::sqrt (B0), r1 = std::sqrt (B1);
        const double W = 0.5 * (r0 + r1);
        const double disc = std::max (0.0, W * W + B2);
        const double b0 = 0.5 * (W + std::sqrt (disc));
        const double b2 = b0 > 0.0 ? -B2 / (4.0 * b0) : 0.0;
        return { b0, 0.5 * (r0 - r1), b2, a1, a2 };
    }

    bool sameShape (const DrumParams::EqBand& a, const DrumParams::EqBand& b) noexcept
    {
        return a.type == b.type && juce::exactlyEqual (a.freq, b.freq) && juce::exactlyEqual (a.q, b.q) && a.slope == b.slope;
    }

    inline double tick (const EqSection& s, double x, double* z) noexcept
    {
        const double y = s.b0 * x + z[0];
        z[0] = s.b1 * x - s.a1 * y + z[1];
        z[1] = s.b2 * x - s.a2 * y;
        return y;
    }
}

//==============================================================================
double EqDesign::magnitude (double hz, double sampleRate) const noexcept
{
    const double w = 2.0 * kPi * hz / sampleRate;
    const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
    double m = 1.0;
    for (int i = 0; i < count; ++i)
    {
        const auto& s = sections[(size_t) i];
        m *= std::abs ((s.b0 + s.b1 * z1 + s.b2 * z2) / (1.0 + s.a1 * z1 + s.a2 * z2));
    }
    return m;
}

EqDesign ParametricEq::design (const DrumParams::EqBand& band, int style, double sampleRate, float gainDb)
{
    const double fc = clampFreq (band.freq, sampleRate);
    const auto protos = prototypes (band, style, gainDb);

    const auto build = [&] (bool match)
    {
        // Matched, a cut is the matching boost turned upside down: a cut's poles are heavily
        // damped (one can sit far above Nyquist, where impulse invariance loses it), a boost's
        // aren't. For a bell, shelf or tilt the analog cut is exactly 1 / the boost.
        const bool invert = match && eqTypeHasGain (band.type) && gainDb < 0.0f;
        const auto use = invert ? prototypes (band, style, -gainDb) : protos;
        EqDesign d;
        for (int i = 0; i < use.count; ++i)
        {
            auto sec = match ? matched (use.p[(size_t) i], fc, sampleRate) : bilinear (use.p[(size_t) i], fc, sampleRate);
            if (invert && std::abs (sec.b0) > 1.0e-12)
                sec = { 1.0 / sec.b0, sec.a1 / sec.b0, sec.a2 / sec.b0, sec.b1 / sec.b0, sec.b2 / sec.b0 };
            d.sections[(size_t) i] = sec;
        }
        d.count = use.count;
        return d;
    };

    // Digital is the bilinear transform. Well below Nyquist that's already the analog curve
    // (and matching there would divide tiny numbers).
    // A notch is bilinear too: its zeros land exactly on the band's frequency.
    if (style == eqStyleDigital || band.type == eqNotch || fc < sampleRate / 64.0 || protos.count == 0)
        return build (false);

    // The others: whichever of matched and bilinear is closer to the analog curve, where the
    // curve matters (above -24 dB), from 20 Hz to 95% of Nyquist. Matched wins for most;
    // bilinear can for a notch or a resonant shelf near the top.
    const auto error = [&] (const EqDesign& d)
    {
        double worst = 0.0;
        const double top = 0.475 * sampleRate;
        for (int k = 0; k <= 31; ++k)
        {
            const double hz = 20.0 * std::pow (top / 20.0, k / 31.0);
            double analog = 1.0;
            for (int i = 0; i < protos.count; ++i)
                analog *= protos.p[(size_t) i].magnitudeSquared (hz / fc);
            const double analogDb = 10.0 * std::log10 (std::max (1.0e-30, analog));
            if (analogDb < -24.0)
                continue;
            worst = std::max (worst, std::abs (20.0 * std::log10 (std::max (1.0e-15, d.magnitude (hz, sampleRate))) - analogDb));
        }
        return worst;
    };
    const auto m = build (true), b = build (false);
    return error (m) <= error (b) ? m : b;
}

double ParametricEq::analogMagnitudeDb (const DrumParams::EqBand& band, int style, double hz)
{
    const auto protos = prototypes (band, style, band.gainDb);
    double m2 = 1.0;
    for (int i = 0; i < protos.count; ++i)
        m2 *= protos.p[(size_t) i].magnitudeSquared (hz / juce::jmax (1.0e-3, (double) band.freq));
    return 10.0 * std::log10 (juce::jmax (1.0e-30, m2));
}

//==============================================================================
void ParametricEq::prepare (double sr)
{
    sampleRate = sr;
    reset();
}

void ParametricEq::reset()
{
    for (auto& b : bands)
    {
        std::memset (b.z, 0, sizeof (b.z));
        b.dz[0] = b.dz[1] = 0.0;
        b.mix = 0.0f;
        b.hasDesign = b.hasDetector = false;
        b.env = 0.0f;
        b.dynAmount = 1.0f;
        b.appliedGainDb.store (0.0f, std::memory_order_relaxed);
    }
    std::memset (lz, 0, sizeof (lz));
    listenMix = 0.0f;
    fresh = true;
}

float ParametricEq::getAppliedGainDb (int band) const noexcept
{
    return juce::isPositiveAndBelow (band, kNumEqBands) ? bands[(size_t) band].appliedGainDb.load (std::memory_order_relaxed) : 0.0f;
}

void ParametricEq::process (float* L, float* R, int numSamples, const DrumParams& p, bool on) noexcept
{
    const int style = juce::jlimit (0, numEqStyles - 1, p.eqStyle);
    const bool listening = p.eqListenHz > 0.0f;

    if (fresh)
    {
        fresh = false;
        for (int k = 0; k < kNumEqBands; ++k)
            bands[(size_t) k].mix = on && p.eqBands[(size_t) k].on ? 1.0f : 0.0f;
    }

    for (int start = 0; start < numSamples; start += kChunk)
    {
        const int n = juce::jmin (kChunk, numSamples - start);
        float* l = L + start;
        float* r = R + start;

        if (listening || listenMix > 0.0f)
        {
            std::copy (l, l + n, dryL);
            std::copy (r, r + n, dryR);
        }

        for (int k = 0; k < kNumEqBands; ++k)
            runBand (bands[(size_t) k], p.eqBands[(size_t) k], style, l, r, n, on);

        // Listen: a band pass of the EQ's input in place of its output (a 5 ms fade).
        if (listening || listenMix > 0.0f)
        {
            if (listening && (! juce::exactlyEqual (p.eqListenHz, listenHz) || ! juce::exactlyEqual (p.eqListenQ, listenQ)))
            {
                listenHz = p.eqListenHz;
                listenQ = p.eqListenQ;
                const double q = juce::jlimit (0.3, 30.0, (double) listenQ);
                listen = bilinear ({ 0.0, 1.0 / q, 0.0, 1.0, 1.0 / q, 1.0 }, clampFreq (listenHz, sampleRate), sampleRate);
            }
            const float from = listenMix;
            const float step = (float) n / (0.005f * (float) sampleRate);
            listenMix = listening ? juce::jmin (1.0f, listenMix + step) : juce::jmax (0.0f, listenMix - step);
            for (int i = 0; i < n; ++i)
            {
                const float m = from + (listenMix - from) * (float) (i + 1) / (float) n;
                const float bl = (float) tick (listen, dryL[i], lz[0]);
                const float br = (float) tick (listen, dryR[i], lz[1]);
                l[i] += (bl - l[i]) * m;
                r[i] += (br - r[i]) * m;
            }
            if (listenMix <= 0.0f)
                std::memset (lz, 0, sizeof (lz));
        }
    }
}

void ParametricEq::runBand (Band& b, const DrumParams::EqBand& bp, int style, float* L, float* R, int n, bool on) noexcept
{
    const float target = on && bp.on ? 1.0f : 0.0f;
    const float from = b.mix;
    const float step = (float) n / (0.02f * (float) sampleRate);
    b.mix = target > b.mix ? juce::jmin (target, b.mix + step) : juce::jmax (target, b.mix - step);

    if (from <= 0.0f && b.mix <= 0.0f)
    {
        if (b.hasDesign)
        {
            std::memset (b.z, 0, sizeof (b.z));
            b.dz[0] = b.dz[1] = 0.0;
            b.hasDesign = b.hasDetector = false;
            b.env = 0.0f;
            b.dynAmount = 1.0f;
        }
        b.appliedGainDb.store (0.0f, std::memory_order_relaxed);
        return;
    }

    const bool gainType = eqTypeHasGain (bp.type);

    // The detector listens to what reaches this band.
    if (gainType && bp.dynamic)
    {
        if (! b.hasDetector || ! sameShape (b.detectorFor, bp))
        {
            const double fc = clampFreq (bp.freq, sampleRate);
            const double q = juce::jlimit (0.5, 30.0, (double) bp.q);
            const Proto proto = bp.type == eqLowShelf  ? Proto { 1.0, 0.0, 0.0, 1.0, 1.4142136, 1.0 }   // low pass
                              : bp.type == eqHighShelf ? Proto { 0.0, 0.0, 1.0, 1.0, 1.4142136, 1.0 }   // high pass
                                                       : Proto { 0.0, 1.0 / q, 0.0, 1.0, 1.0 / q, 1.0 }; // band pass
            b.detector = bilinear (proto, fc, sampleRate);
            b.detectorFor = bp;
            if (! b.hasDetector)
                b.dz[0] = b.dz[1] = 0.0;
            b.hasDetector = true;
        }
        const float attack = std::exp (-1.0f / (juce::jmax (0.05f, bp.attackMs) * 0.001f * (float) sampleRate));
        const float release = std::exp (-1.0f / (juce::jmax (1.0f, bp.releaseMs) * 0.001f * (float) sampleRate));
        float env = b.env;
        for (int i = 0; i < n; ++i)
        {
            const float a = (float) std::abs (tick (b.detector, 0.5 * ((double) L[i] + (double) R[i]), b.dz));
            const float c = a > env ? attack : release;
            env = a + (env - a) * c;
        }
        b.env = env;
        const float levelDb = 20.0f * std::log10 (env + 1.0e-9f);
        b.dynAmount = juce::jlimit (0.0f, 1.0f, (levelDb - bp.thresholdDb) / kDynamicRangeDb);
    }
    else
    {
        b.hasDetector = false;
        b.env = 0.0f;
        b.dynAmount = 1.0f;
    }

    // A gain type fades (and moves dynamically) by its gain; the rest cross-fade.
    const float gain = gainType ? bp.gainDb * b.mix * b.dynAmount : 0.0f;
    b.appliedGainDb.store (gainType ? bp.gainDb * b.dynAmount : 0.0f, std::memory_order_relaxed);

    if (! b.hasDesign || b.madeStyle != style || ! sameShape (b.made, bp) || (gainType && std::abs (gain - b.madeGain) > 1.0e-4f))
    {
        const int oldCount = b.hasDesign ? b.design.count : -1;
        b.design = design (bp, style, sampleRate, gain);
        if (oldCount != b.design.count)
            std::memset (b.z, 0, sizeof (b.z));
        b.made = bp;
        b.madeStyle = style;
        b.madeGain = gain;
        b.hasDesign = true;
    }

    const auto& d = b.design;
    if (d.count == 0)
        return;

    const bool crossFade = ! gainType && ! (from >= 1.0f && b.mix >= 1.0f);
    for (int i = 0; i < n; ++i)
    {
        double l = L[i], r = R[i];
        for (int s = 0; s < d.count; ++s)
        {
            l = tick (d.sections[(size_t) s], l, b.z[0][s]);
            r = tick (d.sections[(size_t) s], r, b.z[1][s]);
        }
        if (crossFade)
        {
            const float m = from + (b.mix - from) * (float) (i + 1) / (float) n;
            L[i] += ((float) l - L[i]) * m;
            R[i] += ((float) r - R[i]) * m;
        }
        else
        {
            L[i] = (float) l;
            R[i] = (float) r;
        }
    }
}

} // namespace drum
