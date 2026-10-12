// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "../dsp/Envelope.h"
#include <utility>
#include <vector>

namespace drum
{

/** A built-in patch. Parameters not listed keep their default value; values
    are in real units (Hz, dB, ms, 0..1, choice index). */
struct FactoryPreset
{
    const char* name;
    std::vector<std::pair<const char*, float>> values;
    std::vector<EnvPoint> pitch, body, noise;
    /** Optional: left empty, these take their defaults (flat pitch, full click level). */
    std::vector<EnvPoint> noisePitch = {}, clickPitch = {}, click = {};
    /** Optional: the effects chain's order (FxChain::toString(), e.g. "drive1 comp1 clip1 eq
        reverb"). Left out, the preset gets the default chain (Drive -> EQ -> Reverb). Every
        parameter a preset doesn't list takes its default too, so a preset sets everything. */
    const char* chain = nullptr;

    /** The points for envelope `env` (an EnvId); empty = its default. A factory preset
        has no curves for the extra layers: it couples them to a core layer's curves, and
        they load on their own copies of those (see PresetManager::buildFactoryState()). */
    const std::vector<EnvPoint>& points (int env) const noexcept
    {
        static const std::vector<EnvPoint> none;
        const std::vector<EnvPoint>* all[] { &pitch, &body, &noise, &noisePitch, &clickPitch, &click };
        return env >= 0 && env < (int) std::size (all) ? *all[env] : none;
    }
};

const std::vector<FactoryPreset>& factoryPresets();

} // namespace drum
