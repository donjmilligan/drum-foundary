// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "NaturalReverb.h"
#include <cmath>
#include <vector>

namespace drum
{

namespace
{
    constexpr int kHeadSize = 256;                            // the convolver's first partitions
    constexpr double kCrossovers[] { 350.0, 1800.0, 6000.0 }; // the tail's four bands

    /** The background thread every Natural reverb in the process builds its rooms on. */
    struct RoomThread : juce::TimeSliceThread
    {
        RoomThread() : juce::TimeSliceThread ("Drum Foundry rooms") { startThread (juce::Thread::Priority::low); }
        ~RoomThread() override { stopThread (2000); }
    };
}

bool NaturalReverb::Settings::operator== (const Settings& o) const noexcept
{
    return juce::exactlyEqual (size, o.size) && juce::exactlyEqual (decaySeconds, o.decaySeconds)
        && juce::exactlyEqual (preDelayMs, o.preDelayMs) && juce::exactlyEqual (diffusion, o.diffusion)
        && juce::exactlyEqual (width, o.width) && juce::exactlyEqual (early, o.early)
        && juce::exactlyEqual (damping, o.damping);
}

//==============================================================================
/** Builds rooms on the RoomThread for the audio thread. The audio thread only ever
    try-locks (it never waits): a request or a pick-up that finds the lock taken is
    retried on the next block. Buffers are created and freed on the RoomThread. */
class NaturalReverb::Builder : private juce::TimeSliceClient
{
public:
    explicit Builder (double sr) : sampleRate (sr) { thread->addTimeSliceClient (this); }
    ~Builder() override { thread->removeTimeSliceClient (this); }

    /** Audio thread: asks for a room. False if it couldn't (try again later). */
    bool request (const Settings& s) noexcept
    {
        const juce::SpinLock::ScopedTryLockType lock (mutex);
        if (! lock.isLocked())
            return false;
        wanted = s;
        hasRequest = true;
        return true;
    }

    /** Audio thread: the newest finished room, if there is one. Hand the emptied
        buffer back with recycle(). */
    std::unique_ptr<juce::AudioBuffer<float>> take() noexcept
    {
        const juce::SpinLock::ScopedTryLockType lock (mutex);
        return lock.isLocked() ? std::move (ready) : nullptr;
    }

    /** Audio thread: a buffer whose samples have been moved out, to be freed here. False
        if it couldn't be taken yet (keep it and try again). */
    bool recycle (std::unique_ptr<juce::AudioBuffer<float>>& spentBuffer) noexcept
    {
        const juce::SpinLock::ScopedTryLockType lock (mutex);
        if (! lock.isLocked() || spent != nullptr)
            return false;
        spent = std::move (spentBuffer);
        return true;
    }

private:
    int useTimeSlice() override
    {
        Settings s;
        std::unique_ptr<juce::AudioBuffer<float>> toFree;
        {
            const juce::SpinLock::ScopedLockType lock (mutex);
            toFree = std::move (spent);
            if (! hasRequest)
                return 20;
            s = wanted;
            hasRequest = false;
        }
        toFree.reset();

        auto room = std::make_unique<juce::AudioBuffer<float>> (makeImpulse (s, sampleRate));
        {
            const juce::SpinLock::ScopedLockType lock (mutex);
            std::swap (ready, room); // an older room nobody took is freed below, here
        }
        return 5;
    }

    const double sampleRate;
    juce::SpinLock mutex;
    Settings wanted;
    bool hasRequest = false;
    std::unique_ptr<juce::AudioBuffer<float>> ready, spent;
    juce::SharedResourcePointer<RoomThread> thread;
};

//==============================================================================
NaturalReverb::NaturalReverb() = default;
NaturalReverb::~NaturalReverb() = default;

void NaturalReverb::prepare (double sr)
{
    builder.reset();
    sampleRate = sr;
    convolution = std::make_unique<juce::dsp::Convolution> (juce::dsp::Convolution::NonUniform { kHeadSize }, *queue);
    convolution->prepare ({ sr, (juce::uint32) maxBlock, 2 });
    hasSettings = false;
    impulseLength = 0;
    heldBuffer.reset();
    if (async)
        builder = std::make_unique<Builder> (sr);
}

void NaturalReverb::reset() noexcept
{
    if (convolution != nullptr)
        convolution->reset();
}

void NaturalReverb::setAsync (bool shouldBeAsync)
{
    if (shouldBeAsync == async)
        return;
    async = shouldBeAsync;
    builder = async && sampleRate > 0.0 ? std::make_unique<Builder> (sampleRate) : nullptr; // or in prepare()
    hasSettings = false;
}

void NaturalReverb::setSettings (const Settings& s)
{
    if (convolution == nullptr || (hasSettings && s == current))
        return;

    if (builder == nullptr)
    {
        // Offline: build it now and make it active for the next process().
        auto room = makeImpulse (s, sampleRate);
        impulseLength = room.getNumSamples();
        convolution->loadImpulseResponse (std::move (room), sampleRate, juce::dsp::Convolution::Stereo::yes,
                                          juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
        convolution->prepare ({ sampleRate, (juce::uint32) maxBlock, 2 });
        current = s;
        hasSettings = true;
        return;
    }

    if (builder->request (s))
    {
        current = s;
        hasSettings = true;
    }
}

void NaturalReverb::process (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    jassert (numSamples <= maxBlock);
    if (convolution == nullptr)
    {
        juce::FloatVectorOperations::clear (outL, numSamples);
        juce::FloatVectorOperations::clear (outR, numSamples);
        return;
    }

    pickUpRoom();

    const float* ins[] { inL, inR };
    float* outs[] { outL, outR };
    juce::dsp::AudioBlock<const float> in (ins, 2, (size_t) numSamples);
    juce::dsp::AudioBlock<float> out (outs, 2, (size_t) numSamples);
    convolution->process (juce::dsp::ProcessContextNonReplacing<float> (in, out));

    // Before the first room is in place the convolver passes its input through.
    if (convolution->getCurrentIRSize() <= 1)
    {
        juce::FloatVectorOperations::clear (outL, numSamples);
        juce::FloatVectorOperations::clear (outR, numSamples);
    }
}

void NaturalReverb::pickUpRoom() noexcept
{
    if (builder == nullptr)
        return;
    if (heldBuffer != nullptr && builder->recycle (heldBuffer))
        heldBuffer.reset();
    if (heldBuffer != nullptr)
        return;
    if (auto room = builder->take())
    {
        impulseLength = room->getNumSamples();
        convolution->loadImpulseResponse (std::move (*room), sampleRate, juce::dsp::Convolution::Stereo::yes,
                                          juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
        heldBuffer = std::move (room); // its samples have moved on; free the rest off this thread
        if (builder->recycle (heldBuffer))
            heldBuffer.reset();
    }
}

void NaturalReverb::serviceWhileIdle() noexcept
{
    if (convolution == nullptr || builder == nullptr)
        return;
    pickUpRoom();
    if (impulseLength > 0 && convolution->getCurrentIRSize() != impulseLength)
    {
        const float* ins[] { silence.data(), silence.data() };
        float* outs[] { discard.data(), discard.data() };
        juce::dsp::AudioBlock<const float> in (ins, 2, 1);
        juce::dsp::AudioBlock<float> out (outs, 2, 1);
        convolution->process (juce::dsp::ProcessContextNonReplacing<float> (in, out));
    }
}

//==============================================================================
juce::AudioBuffer<float> NaturalReverb::makeImpulse (const Settings& s, double sr)
{
    const double size = juce::jlimit (0.0, 1.0, (double) s.size);
    const double diffusion = juce::jlimit (0.0, 1.0, (double) s.diffusion);
    const double width = juce::jlimit (0.0, 1.0, (double) s.width);
    const double early = juce::jlimit (0.0, 1.0, (double) s.early);
    const double damping = juce::jlimit (0.0, 1.0, (double) s.damping);
    const double decay = juce::jlimit (0.05, 20.0, (double) s.decaySeconds);

    const int pre = juce::jmax (0, juce::roundToInt (juce::jlimit (0.0, 1000.0, (double) s.preDelayMs) * 0.001 * sr));
    const int tailLength = juce::jmax (64, (int) std::ceil (juce::jmin (1.25 * decay, maxTailSeconds) * sr));

    juce::AudioBuffer<float> ir (2, pre + tailLength);
    ir.clear();
    juce::Random random (0x5EED2026);

    // --- The late tail's noise: echo density grows with time squared until the room is mixed.
    const double mixTime = (0.012 + 0.07 * size) * (1.6 - diffusion); // seconds until ~4000 echoes/s
    std::vector<float> noise[2] { std::vector<float> ((size_t) tailLength), std::vector<float> ((size_t) tailLength) };
    const auto gaussian = [&random]
    {
        return (random.nextFloat() + random.nextFloat() + random.nextFloat() + random.nextFloat() - 2.0f) * 1.7320508f;
    };
    const auto echo = [&] (double p)
    {
        if (p >= 1.0)
            return gaussian();
        if (random.nextDouble() >= p)
            return 0.0f;
        return (random.nextBool() ? 1.0f : -1.0f) / (float) std::sqrt (p); // unit variance
    };
    const float shared = (float) (1.0 - width), own = (float) width;
    const float widthNorm = 1.0f / std::sqrt (shared * shared + own * own);
    for (int i = 0; i < tailLength; ++i)
    {
        const double t = i / sr;
        const double density = 40.0 + 4000.0 * (t / mixTime) * (t / mixTime);
        const double p = density / sr;
        const float a = echo (p), b = echo (p);
        noise[0][(size_t) i] = a;
        noise[1][(size_t) i] = (shared * a + own * b) * widthNorm;
    }

    // --- Four bands, each with its own decay: the lows a little longer, the highs shorter with Damping.
    const double rt60[4] { decay * 1.1, decay, decay * (1.0 - 0.45 * damping), decay * (1.0 - 0.75 * damping) };
    const double rise = 0.003 + 0.03 * size;
    const int fadeFrom = tailLength - tailLength / 10;
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) tailLength, 1 };
    for (int c = 0; c < 2; ++c)
    {
        juce::dsp::LinkwitzRileyFilter<float> low[3], high[3];
        for (int k = 0; k < 3; ++k)
        {
            low[k].setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
            high[k].setType (juce::dsp::LinkwitzRileyFilterType::highpass);
            low[k].setCutoffFrequency ((float) juce::jmin (kCrossovers[k], 0.45 * sr));
            high[k].setCutoffFrequency ((float) juce::jmin (kCrossovers[k], 0.45 * sr));
            low[k].prepare (spec);
            high[k].prepare (spec);
        }
        double gain[4], step[4];
        for (int b = 0; b < 4; ++b)
        {
            gain[b] = 1.0;
            step[b] = std::exp (-6.907755 / (juce::jmax (0.05, rt60[b]) * sr)); // -60 dB over its RT60
        }

        auto* out = ir.getWritePointer (c, pre);
        for (int i = 0; i < tailLength; ++i)
        {
            float rest = noise[c][(size_t) i];
            float band[4];
            for (int k = 0; k < 3; ++k)
            {
                band[k] = low[k].processSample (0, rest);
                rest = high[k].processSample (0, rest);
            }
            band[3] = rest;

            double y = 0.0;
            for (int b = 0; b < 4; ++b)
            {
                y += band[b] * gain[b];
                gain[b] *= step[b];
            }

            const double t = i / sr;
            if (t < rise)
            {
                const double r = std::sin (juce::MathConstants<double>::halfPi * t / rise);
                y *= r * r;
            }
            if (i >= fadeFrom)
                y *= 0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * (i - fadeFrom) / (double) (tailLength - fadeFrom));
            out[i] = (float) y;
        }
    }

    // --- Early reflections: discrete taps (softened over three samples), later and sparser with Size.
    if (early > 0.0)
    {
        const int taps = 6 + juce::roundToInt (10.0 * (1.0 - 0.5 * size));
        const double last = 0.005 + 0.04 * size;
        for (int c = 0; c < 2; ++c)
        {
            auto* out = ir.getWritePointer (c, pre);
            juce::Random tapRandom (c == 0 || width < 0.5 ? 0x7A9 : 0x7A9 + c);
            for (int k = 0; k < taps; ++k)
            {
                const double t = 0.001 + (last - 0.001) * tapRandom.nextDouble();
                const float a = (float) (early * 6.0 * (0.5 + 0.5 * tapRandom.nextDouble()) * std::exp (-t / 0.03))
                              * (tapRandom.nextBool() ? 1.0f : -1.0f);
                const int at = juce::jlimit (1, tailLength - 2, (int) (t * sr));
                out[at - 1] += 0.25f * a;
                out[at] += 0.5f * a;
                out[at + 1] += 0.25f * a;
            }
        }
    }

    // --- Unit energy per side.
    for (int c = 0; c < 2; ++c)
    {
        const auto* x = ir.getReadPointer (c);
        double energy = 0.0;
        for (int i = 0; i < ir.getNumSamples(); ++i)
            energy += (double) x[i] * x[i];
        if (energy > 0.0)
            ir.applyGain (c, 0, ir.getNumSamples(), (float) (1.0 / std::sqrt (energy)));
    }
    return ir;
}

} // namespace drum
