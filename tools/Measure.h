// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include <juce_core/juce_core.h>

namespace drum::measure
{

/** Handles the "measure", "measure-presets" and "emit-preset" commands of the dev tool.
    `args` is everything after the command name. Returns the process exit code.

      measure <file-or-dir>... [--list paths.txt] [--csv out.csv]
      measure-presets [kick|snare|all] [--preset name]... [--set id=value]...
                      [--overrides file] [--csv out.csv] [--wav dir] [--emit]
      measure-presets also reports the spectrum analyzer's PEAK readout (HIT and LIVE).
      emit-preset <file.drumpreset>...   saved presets in the FactoryPresets.cpp format
*/
int run (const juce::String& command, const juce::StringArray& args);

} // namespace drum::measure
