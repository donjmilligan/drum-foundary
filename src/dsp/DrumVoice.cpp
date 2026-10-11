// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "DrumVoice.h"

namespace drum
{

namespace
{
    // Extra Noise layers have no Coupling, Width or Grit knobs: Wires couples to
    // the body at the core layer's default, every style gets the default Width,
    // and Crackle a medium density (800 impulses per second). Grit is off otherwise.
    constexpr float kLayerWireCoupling = 0.3f;
    constexpr float kLayerNoiseWidth = 0.35f;
    constexpr float kLayerCrackleDensity = 0.5f;

    using dsp::kMaxInc;   // partials stay under 0.45 x the sample rate
    using dsp::mixSeed;
    using dsp::wrap01;

    inline void copySamples (float* dst, const float* src, int n) noexcept { std::copy (src, src + n, dst); }
    inline void clearSamples (float* dst, int n) noexcept { std::fill (dst, dst + n, 0.0f); }

    // Pitch: the curves are in semitones; oscillators go down to 0 Hz (see BodyGen).
    constexpr float kOctavesPerSt = 1.0f / 12.0f;
    inline float pitchInc (float base, float octaves) noexcept { return BodyGen::pitchInc (base, octaves); }

    /** A layer's polarity at the hit: -1 when inverted. */
    inline float polarity (bool invert) noexcept { return invert ? -1.0f : 1.0f; }

    // Offsets past this (a corrupt snapshot; the parameters stop at +-100 ms)
    // are clamped, which bounds what a hit renders ahead of time.
    constexpr float kMaxOffsetMs = 1000.0f;

    inline float safeOffsetMs (float ms) noexcept
    {
        return std::isfinite (ms) ? juce::jlimit (-kMaxOffsetMs, kMaxOffsetMs, ms) : 0.0f;
    }

    bool anyLayerSoloed (const DrumParams& p) noexcept
    {
        bool solo = p.bodySolo || p.noiseSolo || p.clickSolo;
        for (const auto& l : p.layers)
            solo = solo || l.solo;
        return solo;
    }
}

void DrumVoice::prepare (double sr)
{
    sampleRate = (float) sr;
    gainSlew = 1.0f / (0.0025f * sampleRate);
    active = false;
}

float DrumVoice::latestOffsetMs (const DrumParams& p) noexcept
{
    float late = 0.0f;
    if (p.bodyOn)  late = juce::jmax (late, safeOffsetMs (p.bodyOffsetMs));
    if (p.noiseOn) late = juce::jmax (late, safeOffsetMs (p.noiseOffsetMs));
    if (p.clickOn) late = juce::jmax (late, safeOffsetMs (p.clickOffsetMs));
    for (const auto& l : p.layers)
        if (l.on)
            late = juce::jmax (late, safeOffsetMs (l.delayMs));
    return late;
}

DrumVoice::Clock DrumVoice::makeClock (float offsetMs) const noexcept
{
    Clock c;
    // (ms x rate / 1000 keeps a whole-sample offset exact: ms x 0.001 x rate
    // can land a hair past it and start the layer a sample late.)
    c.offset = (double) safeOffsetMs (offsetMs) * sampleRate / 1000.0;
    c.cut = c.offset < 0.0;
    c.endsFirst = c.offset < lateSamples;
    return c;
}

void DrumVoice::start (int midiNote, float vel, uint32_t seed, const DrumParams& p)
{
    active = true;
    fading = false;
    gain = 1.0f;
    lengthSamples = juce::jmax (1.0, (double) p.lengthMs * 0.001 * sampleRate);

    noteRatio = p.keytrack ? std::exp2 ((float) (midiNote - 60) / 12.0f) : 1.0f;

    const float v = juce::jlimit (0.0f, 1.0f, vel);
    velocity = v;
    velGain = 1.0f - p.velSens * (1.0f - v * v);

    bodyStyle = juce::jlimit (0, numBodyStyles - 1, p.bodyStyle);

    // Offsets: each layer's clock, and where the voice's own clock starts. An
    // early layer is rendered from its own start, so the voice starts that far
    // before the hit (pos < 0) and throws that part away (see render()).
    lateSamples = (double) latestOffsetMs (p) * sampleRate / 1000.0;
    bodyClock = makeClock (p.bodyOffsetMs);
    noiseClock = makeClock (p.noiseOffsetMs);
    clickClock = makeClock (p.clickOffsetMs);
    for (int k = 0; k < kNumExtraLayers; ++k)
        layers[(size_t) k].clock = makeClock (p.layers[(size_t) k].delayMs);

    double early = 0.0;
    if (p.bodyOn)  early = juce::jmax (early, -bodyClock.offset);
    if (p.noiseOn) early = juce::jmax (early, -noiseClock.offset);
    if (p.clickOn) early = juce::jmax (early, -clickClock.offset);
    for (int k = 0; k < kNumExtraLayers; ++k)
        if (p.layers[(size_t) k].on)
            early = juce::jmax (early, -layers[(size_t) k].clock.offset);
    pos = -std::ceil (early);

    // Mute, solo and polarity start where they are, with no fade.
    anySolo = anyLayerSoloed (p);
    const auto startFaders = [this] (Faders& f, bool on, bool solo, bool invert)
    {
        f.mute.set (on);
        f.solo.set (! anySolo || solo);
        f.polarity.value = f.polarity.target = polarity (invert);
    };
    startFaders (bodyFaders, p.bodyOn, p.bodySolo, p.bodyInvert);
    startFaders (noiseFaders, p.noiseOn, p.noiseSolo, p.noiseInvert);
    startFaders (clickFaders, p.clickOn, p.clickSolo, p.clickInvert);
    for (int k = 0; k < kNumExtraLayers; ++k)
    {
        const auto& lp = p.layers[(size_t) k];
        startFaders (layers[(size_t) k].faders, lp.on, lp.solo, lp.invert);
    }

    // Body ---------------------------------------------------------------------------
    body.start (bodyStyle, p.bodyShape, p.modes, v, sampleRate);
    bodySmpPos = 0.0;

    // Noise and click ----------------------------------------------------------------
    noise.start (p.noiseStyle, seed, sampleRate);
    click.start (p.clickType, p.clickTone, p.clickLengthMs, p.clickGain > 0.0f, seed, sampleRate);

    // Extra layers -------------------------------------------------------------------
    for (int k = 0; k < kNumExtraLayers; ++k)
    {
        auto& l = layers[(size_t) k];
        const auto& lp = p.layers[(size_t) k];
        l.on = lp.on;
        l.source = juce::jlimit (0, layerSourceNames.size() - 1, lp.source);
        l.phase = 0.0f;
        l.smpPos = 0.0;
        l.hpL.reset(); l.hpR.reset(); l.lpL.reset(); l.lpR.reset();

        // Noise, Click and Body layers run the core generators (noise with a seed
        // of their own). Their style (without Sample), the click's Tone and Length,
        // and a Body layer's Shape at the hit (what it does to the strike) are
        // fixed for the hit, as on the core layers.
        const uint32_t layerSeed = mixSeed (seed, 0x3C6EF372u + (uint32_t) k * 0x9E3779B9u);
        if (l.source == layerNoise)
            l.noise.start (juce::jlimit (0, noiseSample - 1, lp.noiseStyle), layerSeed, sampleRate);
        else if (l.source == layerClick)
            l.click.start (juce::jlimit (0, clickSample - 1, lp.clickType), lp.clickTone * std::exp2 (lp.tuneSt / 12.0f),
                           lp.clickLengthMs, true, layerSeed, sampleRate);
        else if (l.source == layerBody)
        {
            const int style = juce::jlimit (0, bodySample - 1, lp.bodyStyle);
            const auto tone = layerBodyTone (style);
            l.bodySettings.otGain = tone.overtone;
            l.bodySettings.otRatio = tone.ratio;
            l.bodySettings.modes = tone.modes;
            l.body.start (style, lp.color, tone.modes, v, sampleRate);
        }
    }
}

//==============================================================================
void DrumVoice::render (float* outL, float* outR, int numSamples, const DrumParams& p, const EnvTables& env,
                        const SampleSet& samples) noexcept
{
    if (! active)
        return;

    // --- per-block setup -----------------------------------------------------
    lengthSamples = juce::jmax (1.0, (double) p.lengthMs * 0.001 * sampleRate);
    blk.invLength = (float) (1.0 / lengthSamples);
    blk.endFadeSamples = 0.002f * sampleRate;
    blk.cutFadeSamples = kCutFadeMs * 0.001f * sampleRate;
    blk.lenF = (float) lengthSamples;
    blk.baseInc = p.tuneHz * noteRatio / sampleRate;

    anySolo = anyLayerSoloed (p);
    const auto aim = [this] (Faders& f, bool on, bool solo, bool invert)
    {
        f.mute.target = on ? 1.0f : 0.0f;
        f.solo.target = ! anySolo || solo ? 1.0f : 0.0f;
        f.polarity.target = polarity (invert);
    };
    aim (bodyFaders, p.bodyOn, p.bodySolo, p.bodyInvert);
    aim (noiseFaders, p.noiseOn, p.noiseSolo, p.noiseInvert);
    aim (clickFaders, p.clickOn, p.clickSolo, p.clickInvert);
    for (int k = 0; k < kNumExtraLayers; ++k)
    {
        const auto& lp = p.layers[(size_t) k];
        aim (layers[(size_t) k].faders, lp.on, lp.solo, lp.invert);
    }

    // --- what early layers played before the hit: rendered, then thrown away ---
    while (pos < 0.0)
    {
        const int want = (int) juce::jmin ((double) sliceSize, -pos);
        const int n = renderSlice (want, p, env, samples);
        pos += (double) n;
        if (n < want)
        {
            active = false;
            return;
        }
    }

    // --- per slice -------------------------------------------------------------
    for (int done = 0; done < numSamples;)
    {
        const int want = juce::jmin (sliceSize, numSamples - done);
        const int n = renderSlice (want, p, env, samples);

        float* L = outL + done;
        float* R = outR + done;
        if (layersWritten)
        {
            for (int i = 0; i < n; ++i)
            {
                const float g = ctlGain[i];
                const float monoL = (bodyL[i] + clickL[i]) * g;
                const float monoR = (bodyR[i] + clickR[i]) * g;
                L[i] += monoL + noiseL[i] * g + layerL[i] * g;
                R[i] += monoR + noiseR[i] * g + layerR[i] * g;
            }
        }
        else
        {
            for (int i = 0; i < n; ++i)
            {
                const float g = ctlGain[i];
                const float monoL = (bodyL[i] + clickL[i]) * g;
                const float monoR = (bodyR[i] + clickR[i]) * g;
                L[i] += monoL + noiseL[i] * g;
                R[i] += monoR + noiseR[i] * g;
            }
        }
        pos += (double) n;

        if (n < want)
        {
            active = false;
            return;
        }
        done += n;
    }
}

int DrumVoice::renderSlice (int want, const DrumParams& p, const EnvTables& env, const SampleSet& samples) noexcept
{
    layersWritten = false;
    const int n = computeControl (want);
    if (n <= 0)
        return n;

    // Body. Coupling follows the body as it sounds: its clock and mute, but not
    // solo (a soloed Noise sounds as it does in the mix) or its polarity.
    const bool bodyPlays = runFaders (bodyFaders, n);
    const int bodyFirst = bodyPlays ? runClock (bodyClock, n, clockX, clockGain) : n;
    clearSamples (bodyL, bodyFirst);
    clearSamples (bodyR, bodyFirst);
    clearSamples (coupleSig, bodyFirst);
    if (bodyFirst < n)
    {
        for (int i = bodyFirst; i < n; ++i)
        {
            ctlPitch[i] = env.lookup (envPitch, clockX[i]);
            ctlBody[i]  = env.lookup (envBody, clockX[i]);
        }
        renderBody (bodyFirst, n, p, samples);
        if (! bodyStereo)
            copySamples (bodyR + bodyFirst, bodyL + bodyFirst, n - bodyFirst);

        for (int i = bodyFirst; i < n; ++i)
        {
            const float audible = clockGain[i] * fadeMute[i];
            coupleSig[i] *= audible;
            const float g = audible * fadeSolo[i] * fadePol[i];
            bodyL[i] *= g;
            bodyR[i] *= g;
        }
    }

    // Noise
    const bool noisePlays = runFaders (noiseFaders, n) && p.noiseGain > 0.0f;
    const int noiseFirst = noisePlays ? runClock (noiseClock, n, clockX, clockGain) : n;
    clearSamples (noiseL, noiseFirst);
    clearSamples (noiseR, noiseFirst);
    if (noiseFirst < n)
    {
        for (int i = noiseFirst; i < n; ++i)
        {
            ctlNoise[i] = env.lookup (envNoise, clockX[i]);
            ctlNoisePitch[i] = env.lookup (envNoisePitch, clockX[i]);
        }
        renderNoise (noiseFirst, n, p, samples);

        for (int i = noiseFirst; i < n; ++i)
        {
            const float g = clockGain[i] * fadeMute[i] * fadeSolo[i] * fadePol[i];
            noiseL[i] *= g;
            noiseR[i] *= g;
        }
    }

    // Click
    const bool clickPlays = runFaders (clickFaders, n);
    const int clickFirst = clickPlays ? runClock (clickClock, n, clockX, clockGain) : n;
    clearSamples (clickL, clickFirst);
    clearSamples (clickR, clickFirst);
    if (clickFirst < n)
    {
        for (int i = clickFirst; i < n; ++i)
        {
            ctlClickPitch[i] = env.lookup (envClickPitch, clockX[i]);
            ctlClick[i] = env.lookup (envClick, clockX[i]);
        }
        renderClickSlice (clickFirst, n, p, samples);
        if (! clickStereo)
            copySamples (clickR + clickFirst, clickL + clickFirst, n - clickFirst);

        // The Click curve multiplies the click's own decay.
        for (int i = clickFirst; i < n; ++i)
        {
            const float g = clockGain[i] * fadeMute[i] * fadeSolo[i] * fadePol[i] * ctlClick[i];
            clickL[i] *= g;
            clickR[i] *= g;
        }
    }

    renderLayers (n, p, env, samples);
    return n;
}

int DrumVoice::computeControl (int n) noexcept
{
    // The voice ends with its latest layer, Length + lateSamples after the hit.
    const double end = lengthSamples + lateSamples;
    for (int i = 0; i < n; ++i)
    {
        const double at = pos + (double) i;
        const float remain = (float) (end - at);

        const float target = fading ? 0.0f : juce::jlimit (0.0f, 1.0f, remain / blk.endFadeSamples);
        gain += juce::jlimit (-gainSlew, gainSlew, target - gain);
        if (gain <= 0.0f && (fading || remain <= 0.0f))
            return i;

        ctlGain[i] = gain * velGain;
    }
    return n;
}

int DrumVoice::runClock (const Clock& c, int n, float* x, float* g) const noexcept
{
    // A layer that ended before the voice has nothing left to play.
    if (c.endsFirst && pos - c.offset >= lengthSamples)
        return n;

    int first = 0;
    while (first < n && pos + (double) first - c.offset < 0.0)
        ++first;

    for (int i = first; i < n; ++i)
    {
        const double at = pos + (double) i;
        const double local = at - c.offset;
        x[i] = juce::jmin (1.0f, (float) local * blk.invLength);

        // An S-shaped fade-in (smoothstep, from exactly 0 at the hit) where a
        // negative Offset cuts in, and a linear fade over the last 2 ms of a
        // layer that ends first.
        float gi = 1.0f;
        if (c.cut && at >= 0.0 && at < (double) blk.cutFadeSamples)
        {
            const float t = (float) at / blk.cutFadeSamples;
            gi = t * t * (3.0f - 2.0f * t);
        }
        if (c.endsFirst)
            gi *= juce::jlimit (0.0f, 1.0f, (float) (lengthSamples - local) / blk.endFadeSamples);
        g[i] = gi;
    }
    return first;
}

bool DrumVoice::runFaders (Faders& f, int n) noexcept
{
    const bool plays = ! f.mute.isSilent();
    f.mute.fill (fadeMute, n, gainSlew);
    f.solo.fill (fadeSolo, n, gainSlew);
    f.polarity.fill (fadePol, n, 2.0f * gainSlew); // +1 to -1 over the same 2.5 ms
    return plays;
}

//==============================================================================
// Body

void DrumVoice::renderBody (int from, int n, const DrumParams& p, const SampleSet& samples) noexcept
{
    bodyStereo = false;
    if (bodyStyle == bodySample)
    {
        renderBodySample (from, n, p, samples);
        return;
    }

    BodyGen::Settings bs;
    bs.gain = p.bodyGain;
    bs.shape = p.bodyShape;
    bs.otGain = p.otGain;
    bs.otRatio = p.otRatio;
    bs.modes = p.modes;
    bs.baseInc = blk.baseInc;
    bs.lengthSamples = blk.lenF;
    body.render (from, n, bs, ctlPitch, ctlBody, bodyL, coupleSig);
}

void DrumVoice::renderBodySample (int from, int n, const DrumParams& p, const SampleSet& samples) noexcept
{
    // The sample plays from its start under the body envelope. Sample Tune and
    // key tracking transpose it; the pitch curve doesn't apply.
    const auto view = dsp::SampleView::of (samples.get (slotBody));
    bodyStereo = view.stereo;

    if (view.isFinished (bodySmpPos))
    {
        clearSamples (bodyL + from, n - from);
        clearSamples (bodyR + from, n - from);
        clearSamples (coupleSig + from, n - from);
        return;
    }

    const double rate = view.rateFor (sampleRate, std::exp2 ((double) p.bodySmpTune / 12.0) * noteRatio);
    const float bodyGain = p.bodyGain;

    for (int i = from; i < n; ++i)
    {
        float l, r;
        view.read (bodySmpPos, l, r);
        bodySmpPos += rate;

        const float g = bodyGain * ctlBody[i];
        bodyL[i] = l * g;
        bodyR[i] = r * g;
        coupleSig[i] = juce::jlimit (-1.0f, 1.0f, 0.5f * (l + r));
    }
}

//==============================================================================
// Noise

void DrumVoice::renderNoise (int from, int n, const DrumParams& p, const SampleSet& samples) noexcept
{
    NoiseGen::Settings ns;
    ns.gain = p.noiseGain;
    ns.lowCut = p.noiseHP;
    ns.highCut = p.noiseLP;
    ns.color = p.noiseColor;
    ns.couple = p.noiseCouple;
    ns.width = p.noiseWidth;
    ns.grit = p.noiseGrit;
    ns.pitchRatio = noteRatio;
    if (noise.style == noiseSample)
    {
        ns.sample = dsp::SampleView::of (samples.get (slotNoise));
        ns.sampleInc = ns.sample.rateFor (sampleRate, std::exp2 ((double) p.noiseSmpTune / 12.0) * noteRatio);
    }
    noise.render (n - from, ns, ctlNoise + from, ctlNoisePitch + from, coupleSig + from, noiseL + from, noiseR + from);
}

//==============================================================================
// Click

void DrumVoice::renderClickSlice (int from, int n, const DrumParams& p, const SampleSet& samples) noexcept
{
    const float clickGain = p.clickGain;

    if (click.type != clickSample)
    {
        clickStereo = false;
        for (int i = from; i < n; ++i)
        {
            if (! click.active)
            {
                clickL[i] = 0.0f;
                continue;
            }
            click.follow (ctlClickPitch[i]);
            clickL[i] = click.next() * clickGain;
        }
        return;
    }

    // Sample: plays up to Click Length, fading out over the last 2 ms (or 30%).
    clickStereo = true;
    const auto view = dsp::SampleView::of (samples.get (slotClick));
    const double rate = view.rateFor (sampleRate, std::exp2 ((double) p.clickSmpTune / 12.0) * noteRatio);

    for (int i = from; i < n; ++i)
    {
        float l = 0.0f, r = 0.0f;
        if (click.active && click.age < click.maxLength && ! view.isFinished (click.smpPos))
        {
            click.follow (ctlClickPitch[i]);
            view.read (click.smpPos, l, r);
            click.smpPos += rate * (double) click.pitchRatio;

            if (click.highPass)
            {
                l = click.filter.highpass (l);
                r = view.stereo ? click.filterR.highpass (r) : l;
            }

            const float g = clickGain * juce::jmin (1.0f, (float) (click.maxLength - click.age) / (float) click.fadeLength);
            l *= g;
            r *= g;
        }
        ++click.age;
        clickL[i] = l;
        clickR[i] = r;
    }
}

//==============================================================================
// Extra layers

void DrumVoice::renderLayers (int n, const DrumParams& p, const EnvTables& env, const SampleSet& samples) noexcept
{
    for (int k = 0; k < kNumExtraLayers; ++k)
    {
        auto& st = layers[(size_t) k];
        const auto& lp = p.layers[(size_t) k];
        if (! st.on)
            continue;
        const bool plays = runFaders (st.faders, n);
        if (! plays || lp.gain <= 0.0f)
            continue;
        if (st.source == layerClick && ! st.click.active)
            continue;

        // The layer's own clock, then mute, solo and polarity on top: clockGain is its gain.
        const int first = runClock (st.clock, n, clockX, clockGain);
        if (first >= n)
            continue;
        for (int i = first; i < n; ++i)
            clockGain[i] *= fadeMute[i] * fadeSolo[i] * fadePol[i];

        if (! layersWritten)
        {
            clearSamples (layerL, n);
            clearSamples (layerR, n);
            layersWritten = true;
        }

        // Equal-power pan, normalised to unity in the centre.
        const float theta = (juce::jlimit (-1.0f, 1.0f, lp.pan) + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        const float gl = lp.gain * juce::MathConstants<float>::sqrt2 * std::cos (theta);
        const float gr = lp.gain * juce::MathConstants<float>::sqrt2 * std::sin (theta);

        // The drawn curves it follows, its own or a core layer's: a level curve (-1:
        // full level) and a pitch curve (-1: none, so no transposition). See
        // layerLevelEnvelope() in StyleInfo.h.
        const int envId = layerLevelEnvelope (k, lp.env);
        const int pitchId = layerPitchEnvelope (k, lp.pitchEnv);
        for (int i = first; i < n; ++i)
            layerPitch[i] = pitchId >= 0 ? env.lookup (pitchId, clockX[i]) : 0.0f;

        if (st.source == layerNoise)
        {
            // The noise generator's own Low / High Cut are the layer's; Color
            // darkens it as the layer's envelope falls. Width, Grit and Coupling
            // are fixed (see StyleInfo.h), Tune transposes Metallic, and the pitch
            // curve transposes the whole layer.
            for (int i = first; i < n; ++i)
                layerEnv[i] = envId >= 0 ? env.lookup (envId, clockX[i]) : 1.0f;

            NoiseGen::Settings ns;
            ns.lowCut = lp.hp;
            ns.highCut = lp.lp;
            ns.color = lp.color;
            ns.couple = st.noise.style == noiseWires ? kLayerWireCoupling : 0.0f;
            ns.width = kLayerNoiseWidth;
            ns.grit = st.noise.style == noiseCrackle ? kLayerCrackleDensity : 0.0f;
            ns.pitchRatio = noteRatio * std::exp2 (lp.tuneSt / 12.0f);
            st.noise.render (n - first, ns, layerEnv + first, pitchId >= 0 ? layerPitch + first : nullptr, coupleSig + first,
                             layerTmpL + first, layerTmpR + first);

            for (int i = first; i < n; ++i)
            {
                layerL[i] += layerTmpL[i] * clockGain[i] * gl;
                layerR[i] += layerTmpR[i] * clockGain[i] * gr;
            }
            continue;
        }

        // Filters at their extremes are skipped (and at 20 Hz / 20 kHz they'd
        // barely do anything anyway).
        const bool doHp = lp.hp > 20.5f;
        const bool doLp = lp.lp < 19999.0f;
        if (doHp)
        {
            st.hpL.setCoefficients (lp.hp, 0.707f, sampleRate);
            st.hpR.a1 = st.hpL.a1; st.hpR.a2 = st.hpL.a2; st.hpR.a3 = st.hpL.a3; st.hpR.k = st.hpL.k;
        }
        if (doLp)
        {
            st.lpL.setCoefficients (lp.lp, 0.707f, sampleRate);
            st.lpR.a1 = st.lpL.a1; st.lpR.a2 = st.lpL.a2; st.lpR.a3 = st.lpL.a3; st.lpR.k = st.lpL.k;
        }

        if (st.source == layerBody)
        {
            // A second body in its own style: Tune x key x 2^(Tune/12) under the
            // chosen pitch curve, the chosen level curve inside it (the FM index
            // follows it too, as on the Body layer), Color as its Shape, and the
            // style's fixed Overtone / Ratio / Head Modes (layerBodyTone()). Mono.
            for (int i = first; i < n; ++i)
                layerEnv[i] = envId >= 0 ? env.lookup (envId, clockX[i]) : 1.0f;

            auto bs = st.bodySettings;
            bs.gain = 1.0f;
            bs.shape = lp.color;
            bs.baseInc = blk.baseInc * std::exp2 (lp.tuneSt / 12.0f);
            bs.lengthSamples = blk.lenF;
            st.body.render (first, n, bs, layerPitch, layerEnv, layerTmpL, layerCouple);

            for (int i = first; i < n; ++i)
            {
                float v = layerTmpL[i];
                if (doHp)
                    v = st.hpL.highpass (v);
                if (doLp)
                    v = st.lpL.lowpass (v);
                layerL[i] += v * clockGain[i] * gl;
                layerR[i] += v * clockGain[i] * gr;
            }
            continue;
        }

        const auto view = st.source == layerSample ? dsp::SampleView::of (samples.get (slotLayer1 + k)) : dsp::SampleView {};
        const bool stereo = view.stereo;
        const double smpRate = view.rateFor (sampleRate, std::exp2 ((double) lp.tuneSt / 12.0) * noteRatio);
        const float sineInc = blk.baseInc * std::exp2 (lp.tuneSt / 12.0f);

        if (st.source == layerSample && view.isFinished (st.smpPos))
            continue;

        for (int i = first; i < n; ++i)
        {
            float l = 0.0f, r = 0.0f;
            float a = clockGain[i];
            if (st.source == layerClick)
            {
                // The click's own decay is its envelope; the level curve scales it
                // and the pitch curve transposes its tone.
                if (! st.click.active)
                    break;
                if (pitchId >= 0)
                    st.click.follow (layerPitch[i]);
                l = r = st.click.next();
                if (envId >= 0)
                    a = env.lookup (envId, clockX[i]) * clockGain[i];
            }
            else
            {
                const float x = clockX[i];
                a = (envId >= 0 ? env.lookup (envId, x) : 1.0f) * clockGain[i];

                if (st.source == layerSample)
                {
                    view.read (st.smpPos, l, r);
                    st.smpPos += pitchId >= 0 ? smpRate * (double) dsp::transposeRatio (layerPitch[i]) : smpRate;
                }
                else
                {
                    const float inc = pitchInc (sineInc, layerPitch[i] * kOctavesPerSt);
                    st.phase = wrap01 (st.phase + inc);
                    l = r = dsp::fastSin01 (st.phase);
                }
            }

            if (doHp)
            {
                l = st.hpL.highpass (l);
                r = stereo ? st.hpR.highpass (r) : l;
            }
            if (doLp)
            {
                l = st.lpL.lowpass (l);
                r = stereo ? st.lpR.lowpass (r) : l;
            }

            layerL[i] += l * a * gl;
            layerR[i] += r * a * gr;
        }
    }
}

} // namespace drum
