// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cstdint>

namespace drum
{

/** The effects that can go in the master chain, each at most once: two drives,
    the EQ, two compressors, two clippers and the reverb. Their order is the
    patch's FxChain (saved with it as text, see toString()). After the chain come
    the fixed stages: the output filter, the output gain and the output stage
    (Off, a clipper shape, or Comp: see clipModeNames). */
enum FxModule : int
{
    fxDrive1 = 0,
    fxDrive2,
    fxEq,
    fxComp1,
    fxComp2,
    fxClip1,
    fxClip2,
    fxReverb,
    numFxModules
};

/** Saved keys, one per FxModule. Never rename one: patches store them. */
inline const char* fxModuleKey (int m) noexcept
{
    static constexpr const char* keys[numFxModules] { "drive1", "drive2", "eq", "comp1", "comp2", "clip1", "clip2", "reverb" };
    return juce::isPositiveAndBelow (m, (int) numFxModules) ? keys[m] : "";
}

/** What the chain strip calls each one. */
inline const char* fxModuleName (int m) noexcept
{
    static constexpr const char* names[numFxModules] { "DRIVE", "DRIVE 2", "EQ", "COMP", "COMP 2", "CLIP", "CLIP 2", "REVERB" };
    return juce::isPositiveAndBelow (m, (int) numFxModules) ? names[m] : "";
}

/** The kind of effect a module is (its two instances share it). */
enum FxKind { fxKindDrive, fxKindEq, fxKindComp, fxKindClip, fxKindReverb };

inline FxKind fxKindOf (int m) noexcept
{
    switch (m)
    {
        case fxDrive1: case fxDrive2: return fxKindDrive;
        case fxComp1:  case fxComp2:  return fxKindComp;
        case fxClip1:  case fxClip2:  return fxKindClip;
        case fxReverb:                return fxKindReverb;
        case fxEq:
        default:                      return fxKindEq;
    }
}

/** The master chain's order: modules in signal order, each at most once. */
struct FxChain
{
    std::array<int8_t, numFxModules> order {};
    int length = 0;

    /** Drive -> EQ -> Reverb: the chain as it was before it could be changed. */
    static FxChain defaults() noexcept
    {
        FxChain c;
        c.add (fxDrive1);
        c.add (fxEq);
        c.add (fxReverb);
        return c;
    }

    int indexOf (int m) const noexcept
    {
        for (int i = 0; i < length; ++i)
            if (order[(size_t) i] == m)
                return i;
        return -1;
    }
    bool contains (int m) const noexcept { return indexOf (m) >= 0; }
    int operator[] (int i) const noexcept { return order[(size_t) i]; }

    /** Appends m (or inserts it at `at`), if it's a module and not already in. */
    bool add (int m, int at = -1) noexcept
    {
        if (! juce::isPositiveAndBelow (m, (int) numFxModules) || contains (m) || length >= numFxModules)
            return false;
        at = at < 0 ? length : juce::jmin (at, length);
        for (int i = length; i > at; --i)
            order[(size_t) i] = order[(size_t) i - 1];
        order[(size_t) at] = (int8_t) m;
        ++length;
        return true;
    }

    bool remove (int m) noexcept
    {
        const int i = indexOf (m);
        if (i < 0)
            return false;
        for (int k = i; k + 1 < length; ++k)
            order[(size_t) k] = order[(size_t) k + 1];
        --length;
        return true;
    }

    /** Moves m to position `to` (0 .. length - 1) in the chain. */
    void move (int m, int to) noexcept
    {
        if (remove (m))
            add (m, to);
    }

    bool operator== (const FxChain& o) const noexcept
    {
        if (length != o.length)
            return false;
        for (int i = 0; i < length; ++i)
            if (order[(size_t) i] != o.order[(size_t) i])
                return false;
        return true;
    }
    bool operator!= (const FxChain& o) const noexcept { return ! (*this == o); }

    /** Packed into 64 bits (the length, then 4 bits per module), so the audio thread
        can read it from an atomic. */
    uint64_t pack() const noexcept
    {
        uint64_t v = (uint64_t) length;
        for (int i = 0; i < length; ++i)
            v |= (uint64_t) (order[(size_t) i] & 0xf) << (4 + 4 * i);
        return v;
    }

    static FxChain unpack (uint64_t v) noexcept
    {
        FxChain c;
        const int n = (int) (v & 0xf);
        for (int i = 0; i < n && i < numFxModules; ++i)
            c.add ((int) ((v >> (4 + 4 * i)) & 0xf));
        return c;
    }

    /** "drive1 eq reverb". An empty chain is "". */
    juce::String toString() const
    {
        juce::StringArray keys;
        for (int i = 0; i < length; ++i)
            keys.add (fxModuleKey (order[(size_t) i]));
        return keys.joinIntoString (" ");
    }

    /** Reads toString()'s text. Unknown keys and repeats are skipped, so anything
        it returns is a valid chain. */
    static FxChain fromString (const juce::String& text)
    {
        FxChain c;
        for (const auto& key : juce::StringArray::fromTokens (text, " ,", ""))
            for (int m = 0; m < numFxModules; ++m)
                if (key.trim() == fxModuleKey (m))
                    c.add (m);
        return c;
    }
};

//==============================================================================
// Settings of the modules that come in twos (the first Drive keeps the original
// driveOn / driveType / driveAmount / driveMix fields of DrumParams).

struct DriveSettings
{
    bool  on = true;
    int   type = 0;           // index into driveTypeNames
    float amount = 0.2f;
    float mix = 1.0f;
};

/** A feed-forward compressor (Compressor.h). Off fades its wet signal out. */
struct CompSettings
{
    bool  on = true;
    float thresholdDb = -18.0f;
    float ratio = 4.0f;       // 1 .. 20
    float attackMs = 10.0f;
    float releaseMs = 120.0f;
    float kneeDb = 6.0f;
    float makeupDb = 0.0f;
    float mix = 1.0f;         // dry / compressed (parallel compression below 1)
    int   style = 0;          // index into compStyleNames (CompModels.h)
};

/** A clipper in the chain: a Clipper (Clipper.h) at one of its shapes. Off is the
    Clipper's Off: the signal goes through untouched, only delayed. */
struct ChainClipSettings
{
    bool  on = true;
    int   shape = 0;          // index into chainClipShapeNames: Soft .. Fold (Clipper::soft + shape)
    float driveDb = 0.0f;
    float ceilingDb = 0.0f;
};

} // namespace drum
