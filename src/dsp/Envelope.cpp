// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "Envelope.h"

namespace drum
{

namespace
{
    constexpr float kMinGap = 0.0005f;
    constexpr float kCurvePower = 9.0f;

    const juce::Identifier idPoint ("P");
    const juce::Identifier idX ("x");
    const juce::Identifier idY ("y");
    const juce::Identifier idC ("c");

    bool isFinitePoint (const EnvPoint& p) noexcept
    {
        return std::isfinite (p.x) && std::isfinite (p.y);
    }
}

EnvRange envelopeRange (int envId) noexcept
{
    if (isPitchEnvelope (envId))
        return { -kPitchRangeSt, kPitchRangeSt, true };
    return { 0.0f, kMaxEnvGain, false };
}

CurveEnvelope::CurveEnvelope (std::vector<EnvPoint> pts, EnvRange r) : points (std::move (pts)), range (r)
{
    sanitise();
}

float CurveEnvelope::shape (float t, float curve) noexcept
{
    const float p = juce::jlimit (-1.0f, 1.0f, curve) * kCurvePower;
    if (! (std::abs (p) >= 1.0e-3f)) // written this way round so a NaN curve also gives a straight line
        return t;
    return (std::exp (p * t) - 1.0f) / (std::exp (p) - 1.0f);
}

float CurveEnvelope::evaluate (float x) const noexcept
{
    if (x <= points.front().x) return points.front().y;
    if (x >= points.back().x)  return points.back().y;

    for (size_t i = 0; i + 1 < points.size(); ++i)
    {
        const auto& a = points[i];
        const auto& b = points[i + 1];
        if (x <= b.x)
        {
            const float w = b.x - a.x;
            const float t = w > 0.0f ? (x - a.x) / w : 1.0f;
            return a.y + (b.y - a.y) * shape (t, a.curve);
        }
    }
    return points.back().y;
}

void CurveEnvelope::renderTable (float* dest, int size) const noexcept
{
    size_t seg = 0;
    for (int k = 0; k < size; ++k)
    {
        const float x = (float) k / (float) (size - 1);
        while (seg + 2 < points.size() && x > points[seg + 1].x)
            ++seg;

        const auto& a = points[seg];
        const auto& b = points[seg + 1];
        const float w = b.x - a.x;
        const float t = w > 0.0f ? juce::jlimit (0.0f, 1.0f, (x - a.x) / w) : 1.0f;
        dest[k] = a.y + (b.y - a.y) * shape (t, a.curve);
    }
}

int CurveEnvelope::addPoint (float x, float y)
{
    if ((int) points.size() >= maxPoints || ! std::isfinite (x) || ! std::isfinite (y))
        return -1;

    x = juce::jlimit (kMinGap, 1.0f - kMinGap, x);
    y = juce::jlimit (range.lo, range.hi, y);

    size_t insertAt = 1;
    while (insertAt < points.size() - 1 && points[insertAt].x < x)
        ++insertAt;

    const auto& prev = points[insertAt - 1];
    const auto& next = points[insertAt];
    if (x - prev.x < kMinGap || next.x - x < kMinGap)
        return -1;

    points.insert (points.begin() + (long) insertAt, EnvPoint { x, y, prev.curve });
    return (int) insertAt;
}

bool CurveEnvelope::removePoint (int index)
{
    if (index <= 0 || index >= (int) points.size() - 1)
        return false;

    points.erase (points.begin() + index);
    return true;
}

void CurveEnvelope::movePoint (int index, float x, float y)
{
    if (! juce::isPositiveAndBelow (index, (int) points.size()) || ! std::isfinite (x) || ! std::isfinite (y))
        return;

    auto& p = points[(size_t) index];
    const bool pinnedY = range.pinEndToZero && index == (int) points.size() - 1;
    p.y = pinnedY ? 0.0f : juce::jlimit (range.lo, range.hi, y);

    if (index == 0 || index == (int) points.size() - 1)
        return; // ends are pinned in time

    const float lo = points[(size_t) index - 1].x + kMinGap;
    const float hi = points[(size_t) index + 1].x - kMinGap;
    p.x = juce::jlimit (lo, juce::jmax (lo, hi), x);
}

void CurveEnvelope::offsetAll (float deltaY)
{
    if (! std::isfinite (deltaY))
        return;

    for (size_t i = 0; i < points.size(); ++i)
        if (! (range.pinEndToZero && i == points.size() - 1))
            points[i].y = juce::jlimit (range.lo, range.hi, points[i].y + deltaY);
}

void CurveEnvelope::setCurve (int index, float curve)
{
    if (juce::isPositiveAndBelow (index, (int) points.size() - 1) && std::isfinite (curve))
        points[(size_t) index].curve = juce::jlimit (-1.0f, 1.0f, curve);
}

float CurveEnvelope::segmentMidY (int index) const noexcept
{
    if (! juce::isPositiveAndBelow (index, (int) points.size() - 1))
        return 0.0f;

    const auto& a = points[(size_t) index];
    const auto& b = points[(size_t) index + 1];
    return a.y + (b.y - a.y) * shape (0.5f, a.curve);
}

juce::ValueTree CurveEnvelope::toValueTree (const juce::Identifier& type) const
{
    juce::ValueTree tree (type);
    for (const auto& p : points)
    {
        juce::ValueTree child (idPoint);
        child.setProperty (idX, p.x, nullptr);
        child.setProperty (idY, p.y, nullptr);
        child.setProperty (idC, p.curve, nullptr);
        tree.appendChild (child, nullptr);
    }
    return tree;
}

CurveEnvelope CurveEnvelope::fromValueTree (const juce::ValueTree& tree, const CurveEnvelope& fallback)
{
    if (! tree.isValid())
        return fallback;

    // Strings like "nan" or "inf" parse to non-finite floats. Drop those points
    // here, so a curve with fewer than two usable points falls back to the
    // caller's default for this envelope rather than the generic shape.
    std::vector<EnvPoint> pts;
    for (const auto& child : tree)
    {
        if (! child.hasType (idPoint))
            continue;

        const EnvPoint p { (float) child.getProperty (idX, 0.0f),
                           (float) child.getProperty (idY, 0.0f),
                           (float) child.getProperty (idC, 0.0f) };
        if (isFinitePoint (p))
            pts.push_back (p);
    }

    if (pts.size() < 2)
        return fallback;

    return CurveEnvelope (std::move (pts), fallback.range);
}

bool CurveEnvelope::operator== (const CurveEnvelope& other) const noexcept
{
    if (points.size() != other.points.size())
        return false;

    for (size_t i = 0; i < points.size(); ++i)
        if (! juce::exactlyEqual (points[i].x, other.points[i].x)
             || ! juce::exactlyEqual (points[i].y, other.points[i].y)
             || ! juce::exactlyEqual (points[i].curve, other.points[i].curve))
            return false;

    return true;
}

void CurveEnvelope::sanitise()
{
    // Non-finite values must go before anything compares them: jlimit lets NaN
    // straight through, and NaN breaks the strict weak ordering stable_sort needs.
    points.erase (std::remove_if (points.begin(), points.end(), [] (const EnvPoint& p) { return ! isFinitePoint (p); }),
                  points.end());

    for (auto& p : points)
        if (! std::isfinite (p.curve))
            p.curve = 0.0f;

    if (points.size() < 2)
        points = { { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } };

    if ((int) points.size() > maxPoints)
        points.resize ((size_t) maxPoints);

    std::stable_sort (points.begin(), points.end(), [] (const EnvPoint& a, const EnvPoint& b) { return a.x < b.x; });

    for (auto& p : points)
    {
        p.x = juce::jlimit (0.0f, 1.0f, p.x);
        p.y = juce::jlimit (range.lo, range.hi, p.y);
        p.curve = juce::jlimit (-1.0f, 1.0f, p.curve);
    }

    points.front().x = 0.0f;
    points.back().x = 1.0f;
    points.back().curve = 0.0f;
    if (range.pinEndToZero)
        points.back().y = 0.0f;
}

CurveEnvelope defaultEnvelope (EnvId id)
{
    switch (id)
    {
        // Pitch in semitones over Tune (what used to be a 14 st Pitch Depth).
        case envPitch: return CurveEnvelope ({ { 0.0f, 14.0f, -0.7f }, { 0.035f, 1.68f, -0.5f }, { 1.0f, 0.0f, 0.0f } },
                                             envelopeRange (envPitch));
        case envBody:  return CurveEnvelope ({ { 0.0f, 1.0f, -0.6f }, { 0.2f, 0.12f, -0.4f },
                                               { 0.6f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } }, envelopeRange (envBody));
        case envNoise: return CurveEnvelope ({ { 0.0f, 1.0f, -0.5f }, { 0.1f, 0.45f, -0.35f },
                                               { 0.6f, 0.05f, -0.2f }, { 1.0f, 0.0f, 0.0f } }, envelopeRange (envNoise));
        // Noise and Click pitch start flat (no transposition), Click level at full:
        // until they're drawn, those layers sound exactly as they would without them.
        case envNoisePitch:
        case envClickPitch: return CurveEnvelope ({ { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } }, envelopeRange (id));
        case envClick:      return CurveEnvelope ({ { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } }, envelopeRange (id));
        // An extra layer's own curves: no transposition, and the Body level's decay.
        // Adding a layer from the editor starts them as copies of a core layer's.
        case envLayer1Pitch:
        case envLayer2Pitch:
        case envLayer3Pitch: return CurveEnvelope ({ { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } }, envelopeRange (id));
        case envLayer1Level:
        case envLayer2Level:
        case envLayer3Level: return CurveEnvelope ({ { 0.0f, 1.0f, -0.6f }, { 0.2f, 0.12f, -0.4f },
                                                     { 0.6f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } }, envelopeRange (id));
        case numEnvs:  break;
    }
    return {};
}

const char* envelopeName (EnvId id)
{
    switch (id)
    {
        case envPitch: return "Pitch";
        case envBody:  return "Body";
        case envNoise: return "Noise";
        case envNoisePitch: return "Noise Pitch";
        case envClickPitch: return "Click Pitch";
        case envClick: return "Click";
        case envLayer1Pitch: return "Layer 1 Pitch";
        case envLayer1Level: return "Layer 1 Level";
        case envLayer2Pitch: return "Layer 2 Pitch";
        case envLayer2Level: return "Layer 2 Level";
        case envLayer3Pitch: return "Layer 3 Pitch";
        case envLayer3Level: return "Layer 3 Level";
        case numEnvs:  break;
    }
    return "";
}

} // namespace drum
