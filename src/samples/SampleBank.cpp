// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "SampleBank.h"

namespace drum
{

juce::String sampleSlotName (SampleSlot slot)
{
    switch (slot)
    {
        case slotBody:  return "Body";
        case slotNoise: return "Noise";
        case slotClick: return "Click";
        case slotLayer1:
        case slotLayer2:
        case slotLayer3: return "Layer " + juce::String ((int) slot - (int) slotLayer1 + 1);
        case numSampleSlots:
        default:         break;
    }
    return {};
}

juce::String sampleSlotKey (SampleSlot slot)
{
    return sampleSlotName (slot).toLowerCase().removeCharacters (" ");
}

//==============================================================================
/** Decodes files one at a time, newest request per entry only. The audition
    goes first, so browsing stays snappy while a load is decoding. */
class SampleBank::Loader : private juce::Thread
{
public:
    explicit Loader (SampleBank& b) : juce::Thread ("Drum Foundry samples"), bank (b)
    {
        startThread (juce::Thread::Priority::low);
    }

    ~Loader() override
    {
        signalThreadShouldExit();
        notify();
        stopThread (4000);
    }

    void add (int entry, const juce::File& file, uint32_t request)
    {
        {
            const juce::ScopedLock sl (lock);
            jobs[(size_t) entry] = Job { file, request };
        }
        notify();
    }

    void cancel (int entry)
    {
        const juce::ScopedLock sl (lock);
        jobs[(size_t) entry].reset();
    }

    bool isPending (int entry) const
    {
        const juce::ScopedLock sl (lock);
        return jobs[(size_t) entry].has_value() || busyEntry == entry;
    }

    bool isIdle() const
    {
        const juce::ScopedLock sl (lock);
        if (busyEntry >= 0)
            return false;
        for (const auto& j : jobs)
            if (j.has_value())
                return false;
        return true;
    }

private:
    struct Job
    {
        juce::File file;
        uint32_t request = 0;
    };

    void run() override
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        while (! threadShouldExit())
        {
            int entry = -1;
            Job job;
            {
                const juce::ScopedLock sl (lock);
                for (int e : { (int) previewEntry, 0, 1, 2, 3, 4, 5 })
                    if (jobs[(size_t) e].has_value())
                    {
                        entry = e;
                        job = *std::exchange (jobs[(size_t) e], std::nullopt);
                        break;
                    }
                busyEntry = entry;
            }

            if (entry < 0)
            {
                wait (-1);
                continue;
            }

            juce::String error;
            auto sample = decode (formats, job.file, error);
            bank.install (entry, job.request, std::move (sample), entry != previewEntry);

            const juce::ScopedLock sl (lock);
            busyEntry = -1;
        }
    }

    SampleBank& bank;
    mutable juce::CriticalSection lock;
    std::array<std::optional<Job>, numEntries> jobs;
    int busyEntry = -1;

    static_assert (numEntries == 7, "update the job order in run()");
};

//==============================================================================
SampleBank::SampleBank()
    : loader (std::make_unique<Loader> (*this))
{
    startTimer (1000);
}

SampleBank::~SampleBank()
{
    loader = nullptr; // joins the thread, so nothing installs from here on
    stopTimer();
    cancelPendingUpdate();
}

//==============================================================================
void SampleBank::load (SampleSlot slot, const juce::File& file)
{
    {
        const juce::ScopedLock sl (modelLock);
        loader->add (slot, file, ++entries[(size_t) slot].request);
    }
    ++generation; // shows as loading
}

void SampleBank::clear (SampleSlot slot)
{
    {
        const juce::ScopedLock sl (modelLock);
        auto& entry = entries[(size_t) slot];
        ++entry.request;
        loader->cancel (slot);
        entry.path = {};
        replaceLocked (slot, nullptr);
        publishLocked();
        ++generation;
    }
    releaseRetired();
}

void SampleBank::restore (const std::array<juce::String, numSampleSlots>& paths, bool keepUnchanged)
{
    // Decode first, without holding anything the audio side or the UI might want.
    std::array<SamplePtr, numSampleSlots> decoded;
    if (keepUnchanged)
    {
        const juce::ScopedLock sl (modelLock);
        for (size_t s = 0; s < paths.size(); ++s)
            if (entries[s].sample != nullptr && entries[s].path == paths[s])
                decoded[s] = entries[s].sample;
    }
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        for (size_t s = 0; s < paths.size(); ++s)
        {
            juce::String error;
            if (decoded[s] == nullptr && paths[s].isNotEmpty() && juce::File::isAbsolutePath (paths[s]))
                decoded[s] = decode (formats, juce::File (paths[s]), error);
        }
    }

    {
        const juce::ScopedLock sl (modelLock);
        for (int s = 0; s < numSampleSlots; ++s)
        {
            auto& entry = entries[(size_t) s];
            ++entry.request;
            loader->cancel (s);
            entry.path = paths[(size_t) s];
            replaceLocked (s, std::move (decoded[(size_t) s]));
        }
        publishLocked();
        ++generation;
    }
    releaseRetired();
}

void SampleBank::install (int e, uint32_t request, SamplePtr sample, bool fromUser)
{
    {
        const juce::ScopedLock sl (modelLock);
        auto& entry = entries[(size_t) e];
        if (entry.request != request)
            return; // superseded while it was decoding

        if (e == previewEntry)
        {
            if (sample == nullptr)
            {
                previewActive = false;
                return;
            }
            entry.path = sample->path;
            replaceLocked (e, std::move (sample));
            latestPreviewCount = ++previewCount; // start it from the top
            publishLocked();
            return;
        }

        // A file that won't decode leaves the slot as it was.
        if (sample != nullptr)
        {
            entry.path = sample->path;
            replaceLocked (e, std::move (sample));
            publishLocked();

            if (fromUser)
            {
                userLoaded.push_back ((SampleSlot) e);
                triggerAsyncUpdate();
            }
        }
        ++generation; // no longer loading, either way
    }
    releaseRetired();
}

void SampleBank::replaceLocked (int e, SamplePtr sample)
{
    auto& entry = entries[(size_t) e];
    if (entry.sample == sample)
        return;

    // publishLocked() always follows, and that publish is the first without it.
    if (entry.sample != nullptr)
        retired.emplace_back (std::move (entry.sample), serial + 1);
    entry.sample = std::move (sample);
}

void SampleBank::publishLocked()
{
    ++serial;
    const juce::SpinLock::ScopedLockType al (audioLock);
    for (size_t e = 0; e < entries.size(); ++e)
        published[e] = entries[e].sample.get();
    publishedSerial = serial;
    publishedPreviewCount = previewCount;
    audioDirty.store (true, std::memory_order_release);
}

void SampleBank::releaseRetired()
{
    std::vector<SamplePtr> toFree; // freed after the locks are dropped
    {
        const juce::ScopedLock sl (modelLock);
        if (retired.empty())
            return;

        // Until the audio thread has taken its first table it holds no pointer at
        // all, and anything it takes from now on won't include a retired sample.
        bool neverAdopted;
        {
            const juce::SpinLock::ScopedLockType al (audioLock);
            neverAdopted = ! audioHasAdopted;
        }
        const auto adopted = adoptedSerial.load (std::memory_order_acquire);

        for (auto it = retired.begin(); it != retired.end();)
        {
            if (neverAdopted || it->second <= adopted)
            {
                toFree.push_back (std::move (it->first));
                it = retired.erase (it);
            }
            else
            {
                ++it;
            }
        }
    }
}

void SampleBank::handleAsyncUpdate()
{
    std::vector<SampleSlot> slots;
    {
        const juce::ScopedLock sl (modelLock);
        slots.swap (userLoaded);
    }
    if (onUserLoad != nullptr)
        for (auto s : slots)
            onUserLoad (s);
}

bool SampleBank::waitForPendingLoads (int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (! loader->isIdle())
    {
        if (juce::Time::getMillisecondCounter() > end)
            return false;
        juce::Thread::sleep (2);
    }
    handleUpdateNowIfNeeded();
    return true;
}

//==============================================================================
SamplePtr SampleBank::get (SampleSlot slot) const
{
    const juce::ScopedLock sl (modelLock);
    return entries[(size_t) slot].sample;
}

juce::String SampleBank::getPath (SampleSlot slot) const
{
    const juce::ScopedLock sl (modelLock);
    return entries[(size_t) slot].path;
}

bool SampleBank::isMissing (SampleSlot slot) const
{
    {
        const juce::ScopedLock sl (modelLock);
        const auto& entry = entries[(size_t) slot];
        if (entry.path.isEmpty() || entry.sample != nullptr)
            return false;
    }
    return ! isLoading (slot);
}

bool SampleBank::isLoading (SampleSlot slot) const
{
    return loader->isPending (slot);
}

bool SampleBank::hasPendingUserLoads() const
{
    const juce::ScopedLock sl (modelLock);
    return ! userLoaded.empty();
}

//==============================================================================
void SampleBank::preview (const juce::File& file)
{
    const juce::ScopedLock sl (modelLock);
    auto& entry = entries[(size_t) previewEntry];

    // Cut the current audition now; the new one starts once it's decoded.
    loader->add (previewEntry, file, ++entry.request);
    previewActive = true;
    replaceLocked (previewEntry, nullptr);
    latestPreviewCount = ++previewCount;
    publishLocked();
}

void SampleBank::stopPreview()
{
    const juce::ScopedLock sl (modelLock);
    auto& entry = entries[(size_t) previewEntry];
    ++entry.request;
    loader->cancel (previewEntry);
    previewActive = false;
    if (entry.sample == nullptr)
        return;

    replaceLocked (previewEntry, nullptr);
    latestPreviewCount = ++previewCount;
    publishLocked();
}

bool SampleBank::isPreviewing() const noexcept
{
    return previewActive.load();
}

//==============================================================================
const SampleSet& SampleBank::beginBlock() noexcept
{
    if (audioDirty.load (std::memory_order_acquire))
    {
        const juce::SpinLock::ScopedTryLockType al (audioLock);
        if (al.isLocked())
        {
            audioDirty.store (false, std::memory_order_relaxed);
            for (size_t s = 0; s < (size_t) numSampleSlots; ++s)
                audioSet.slots[s] = published[s];

            audioPreview = published[(size_t) previewEntry];
            if (publishedPreviewCount != audioPreviewCount)
            {
                audioPreviewCount = publishedPreviewCount;
                previewPos = audioPreview != nullptr ? 0.0 : -1.0;
            }
            else if (audioPreview == nullptr)
            {
                previewPos = -1.0;
            }

            audioHasAdopted = true;
            // From here on nothing on this thread touches the previous table (the
            // caller hands the engine the new set before it renders).
            adoptedSerial.store (publishedSerial, std::memory_order_release);
        }
    }
    return audioSet;
}

void SampleBank::mixPreview (juce::AudioBuffer<float>& out, int numSamples) noexcept
{
    if (previewPos < 0.0 || audioPreview == nullptr || numSamples <= 0
        || ! (outputRate > 0.0) || out.getNumChannels() == 0)
        return;

    const auto& src = audioPreview->audio;
    const int length = src.getNumSamples();
    const int srcChannels = src.getNumChannels();
    const int outChannels = juce::jmin (2, out.getNumChannels());
    const double step = audioPreview->sampleRate / outputRate;

    const float* in[2] { src.getReadPointer (0), src.getReadPointer (juce::jmin (1, srcChannels - 1)) };
    float* dst[2] { out.getWritePointer (0), out.getWritePointer (outChannels - 1) };

    for (int i = 0; i < numSamples; ++i)
    {
        if (previewPos >= (double) (length - 1))
        {
            previewPos = -1.0;
            if (audioPreviewCount == latestPreviewCount.load())
                previewActive = false;
            return;
        }

        // Linear interpolation is plenty for an audition.
        const int idx = (int) previewPos;
        const float frac = (float) (previewPos - (double) idx);
        const float l = in[0][idx] + frac * (in[0][idx + 1] - in[0][idx]);
        const float r = in[1][idx] + frac * (in[1][idx + 1] - in[1][idx]);

        if (outChannels == 1)
        {
            dst[0][i] += previewGain * 0.5f * (l + r);
        }
        else
        {
            dst[0][i] += previewGain * l;
            dst[1][i] += previewGain * r;
        }
        previewPos += step;
    }
}

//==============================================================================
SamplePtr SampleBank::decode (juce::AudioFormatManager& formats, const juce::File& file, juce::String& error)
{
    if (! file.existsAsFile())
    {
        error = "File not found";
        return {};
    }

    const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
    {
        error = "Not a readable audio file";
        return {};
    }

    const double rate = reader->sampleRate;
    if (! std::isfinite (rate) || rate < 4000.0 || rate > 768000.0
        || reader->numChannels == 0 || reader->lengthInSamples <= 0)
    {
        error = "Unsupported audio format";
        return {};
    }

    const int channels = juce::jmin (2, (int) reader->numChannels);
    const auto length = (int) juce::jmin (reader->lengthInSamples, (juce::int64) (maxSeconds * rate));

    auto data = std::make_shared<SampleData>();
    data->audio.setSize (channels, length);
    data->audio.clear();
    if (! reader->read (&data->audio, 0, length, 0, true, channels > 1))
    {
        error = "Couldn't read the file";
        return {};
    }

    // Float files can hold anything; the voices and the audition only ever see finite audio.
    for (int c = 0; c < channels; ++c)
    {
        float* x = data->audio.getWritePointer (c);
        for (int i = 0; i < length; ++i)
            if (! std::isfinite (x[i]))
                x[i] = 0.0f;
    }

    data->sampleRate = rate;
    data->path = file.getFullPathName();
    data->name = file.getFileNameWithoutExtension();
    return data;
}

juce::String SampleBank::audioFileWildcard()
{
    return "*.wav;*.wave;*.aif;*.aiff;*.aifc;*.flac;*.ogg;*.mp3;*.m4a;*.caf";
}

bool SampleBank::isAudioFile (const juce::File& file)
{
    return file.hasFileExtension ("wav;wave;aif;aiff;aifc;flac;ogg;mp3;m4a;caf");
}

} // namespace drum
