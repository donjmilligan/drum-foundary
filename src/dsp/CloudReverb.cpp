// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "CloudReverb.h"

#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>

// Cloud Seed's buffer sizes and block size (see libs/CloudSeedCore/PATCHES.md). Its
// headers are only ever included here, so these can't disagree between files.
#define BUFFER_SIZE 512
#define MAX_STR_SIZE 32
#define CLOUDSEED_DELAY_BUFFER_SIZE 65536   // line and pre-delay: ~1.36 s at 48 kHz, 341 ms at 192 kHz
#define CLOUDSEED_ALLPASS_BUFFER_SIZE 9600  // diffuser stages: 200 ms at 48 kHz, 50 ms at 192 kHz
#define CLOUDSEED_TAP_BUFFER_SIZE 8192      // the multitap stage, which Drum Foundry leaves off

#include "../../libs/CloudSeedCore/DSP/ReverbController.h"

namespace drum
{

namespace
{
    namespace P = Cloudseed::Parameter;
    static_assert (P::COUNT <= 64, "CloudReverb::applied is too small");

    // The seeds of Cloud Seed's own "Dark Plate" program (x 999.999, as ScaleParam does).
    constexpr int kSeedTap = 333, kSeedDiffusion = 185, kSeedDelay = 218, kSeedPostDiffusion = 365;
    constexpr int kLines = 8;
    constexpr double kMaxLineFactor = 1.5;   // a line is 0.5 .. 1.5 x the line size (its seed)
    constexpr double kMaxModMs = 1.5;

    // How much of each late diffuser stage's (feedback-stretched) delay counts towards
    // the loop's length, fitted against measured RT60s over Size, Diffusion and Decay:
    // Decay then measures within about 20% (checkReverbEngines checks the default).
    constexpr double decayLoopK = 0.4;
}

CloudReverb::CloudReverb() = default;
CloudReverb::~CloudReverb() = default;

void CloudReverb::prepare (double sr)
{
    sampleRate = sr > 0.0 ? sr : 48000.0;

    // The modulation start phases come from this (per thread) generator: seed it so every
    // instance, live or offline, starts the same.
    Cloudseed::Utils::PhaseSeed() = 0x2545F491u;
    controller = std::make_unique<Cloudseed::ReverbController> ((int) std::lround (sampleRate));

    const double msPerSample = 1000.0 / sampleRate;
    lineMsLimit = (float) std::min (300.0, ((CLOUDSEED_DELAY_BUFFER_SIZE - 64) * msPerSample - kMaxModMs) / kMaxLineFactor);
    allpassMsLimit = (float) ((CLOUDSEED_ALLPASS_BUFFER_SIZE - 16) * msPerSample);

    applied.fill (-1.0e9);
    apply (Settings {}, true);
    reset();
    fresh = true;
}

void CloudReverb::reset() noexcept
{
    if (controller != nullptr)
        controller->ClearBuffers();
    fifoInL.fill (0.0f); fifoInR.fill (0.0f); fifoOutL.fill (0.0f); fifoOutR.fill (0.0f);
    fifoPos = 0;
}

void CloudReverb::set (int id, double value, bool force) noexcept
{
    if (! force && std::abs (applied[(size_t) id] - value) < 1.0e-6)
        return;
    applied[(size_t) id] = value;
    controller->SetParameterScaled (id, value);
}

void CloudReverb::apply (const Settings& s, bool force) noexcept
{
    const auto clamp01 = [] (float v) { return std::isfinite (v) ? std::clamp (v, 0.0f, 1.0f) : 0.0f; };
    const float size = clamp01 (s.size), diffusion = clamp01 (s.diffusion), depth = clamp01 (s.modDepth);
    const float rate = clamp01 (s.modRate), width = clamp01 (s.width), early = clamp01 (s.early), damping = clamp01 (s.damping);

    // Fixed: no dry (it's a send), no input filters or taps (the Reverb's own cuts and
    // pre-delay handle those), eight lines tapped after their diffusers, interpolated.
    set (P::Interpolation, 1.0, force);
    set (P::LowCutEnabled, 0.0, force);
    set (P::HighCutEnabled, 0.0, force);
    set (P::InputMix, 0.2, force);
    set (P::DryOut, -30.0, force);
    set (P::LateOut, 0.0, force);
    set (P::TapEnabled, 0.0, force);
    set (P::TapCount, 1.0, force);
    set (P::TapDecay, 1.0, force);
    set (P::TapLength, 10.0, force);
    set (P::LateMode, 1.0, force);
    set (P::LateLineCount, (double) kLines, force);
    set (P::EarlyDiffuseEnabled, 1.0, force);
    set (P::LateDiffuseEnabled, 1.0, force);
    set (P::EqLowShelfEnabled, 0.0, force);
    set (P::EqLowpassEnabled, 0.0, force);
    set (P::EqHighShelfEnabled, 1.0, force);
    set (P::EqHighFreq, 3000.0, force);
    set (P::SeedTap, kSeedTap, force);
    set (P::SeedDiffusion, kSeedDiffusion, force);
    set (P::SeedDelay, kSeedDelay, force);
    set (P::SeedPostDiffusion, kSeedPostDiffusion, force);

    // Size: the line length, and the diffusers' delays with it.
    const double lineMs = std::min ((double) lineMsLimit, 15.0 * std::pow (20.0, (double) size));
    set (P::LateLineSize, lineMs, force);
    set (P::EarlyDiffuseDelay, std::min ((double) allpassMsLimit, 5.0 + 25.0 * size), force);
    const int lateStages = 1 + (int) std::lround (4.0 * diffusion);
    const double lateDiffMs = std::min ((double) allpassMsLimit, 2.0 + 8.0 * size);
    set (P::LateDiffuseDelay, lateDiffMs, force);

    // Decay: Cloud Seed sets each line's feedback for its delay alone, but the line's
    // diffuser is in the loop too, so a trip round it takes longer and the tail lasts
    // longer than asked. Asking for less by that ratio makes Decay the RT60 it says.
    // (An allpass with gain g holds its signal for about d x (1 + g) / (1 - g) on average.)
    const double lateG = 0.2 + 0.4 * diffusion;
    const double loopRatio = lineMs / (lineMs + decayLoopK * lateStages * lateDiffMs * (1.0 + lateG) / (1.0 - lateG));
    set (P::LateLineDecay, std::clamp ((double) s.decaySeconds * loopRatio, 0.05, 60.0), force);
    set (P::TapPredelay, std::clamp ((double) s.preDelayMs, 0.0, 200.0), force);

    // Diffusion: allpass feedback and how many stages. Kept moderate (and the late,
    // in-loop diffusers short): a diffuser that rings on its own puts a floor under
    // the decay, so a short Decay couldn't be short.
    // The early diffuser isn't in the loop, but rings on its own: keep that under a third
    // of Decay (an allpass of delay d and gain g rings for d x ln 0.001 / ln g).
    const double earlyMs = std::min ((double) allpassMsLimit, 5.0 + 25.0 * size);
    const double earlyCap = std::exp (-6.908 * earlyMs / (1000.0 * std::max (0.05, 0.33 * (double) s.decaySeconds)));
    set (P::EarlyDiffuseFeedback, std::min (0.3 + 0.4 * diffusion, earlyCap), force);
    set (P::LateDiffuseFeedback, lateG, force);
    set (P::EarlyDiffuseCount, (double) (1 + (int) std::lround (7.0 * diffusion)), force);
    set (P::LateDiffuseCount, (double) lateStages, force);

    // Modulation (Cloud Seed switches a diffuser's modulation on above 0.5 ms).
    const double hz = 0.1 * std::pow (25.0, (double) rate);
    set (P::LateLineModAmount, kMaxModMs * depth, force);
    set (P::LateDiffuseModAmount, 1.0 * depth, force);
    set (P::EarlyDiffuseModAmount, 0.8 * depth, force);
    set (P::LateLineModRate, hz, force);
    set (P::LateDiffuseModRate, hz * 1.3, force);
    set (P::EarlyDiffuseModRate, hz * 0.8, force);

    // Width: Cloud Seed's cross seed is how alike the two sides' seeds are (1 = the same).
    set (P::EqCrossSeed, 1.0 - width, force);

    // Early part next to the late tail, and Damping: a high shelf in every line.
    set (P::EarlyOut, early <= 0.0316f ? -30.0 : 20.0 * std::log10 ((double) early), force);
    set (P::EqHighGain, -18.0 * damping, force);
}

void CloudReverb::runChunk() noexcept
{
    apply (settings, false);
    controller->Process (fifoInL.data(), fifoInR.data(), fifoOutL.data(), fifoOutR.data(), chunk);
    fifoPos = 0;
}

void CloudReverb::resume (int position) noexcept
{
    fifoInL.fill (0.0f); fifoInR.fill (0.0f); fifoOutL.fill (0.0f); fifoOutR.fill (0.0f);
    fifoPos = juce::jlimit (0, chunk - 1, position);
}

void CloudReverb::process (const float* inL, const float* inR, float* outL, float* outR, int n, const Settings& s) noexcept
{
    if (controller == nullptr || n <= 0)
        return;
    setSettings (s);
    for (int i = 0; i < n; ++i)
        processSample (inL[i], inR[i], outL[i], outR[i]);
}

} // namespace drum
