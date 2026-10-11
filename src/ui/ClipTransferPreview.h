// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "../dsp/Clipper.h"

namespace drum::ui
{

/** The output clipper's transfer curve, for the UI's little pictures of it:
    exactly the DSP's settled 1x curve (see Clipper::transfer). */
inline float clipTransferPreview (int mode, float x, float driveDb, float ceilingDb)
{
    return Clipper::transfer (mode, x, driveDb, ceilingDb);
}

} // namespace drum::ui
