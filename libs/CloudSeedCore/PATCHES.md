# Drum Foundry's changes to Cloud Seed Core

Upstream: https://github.com/GhostNoteAudio/CloudSeedCore at deb21ded9eb7dad9b3ff94ce1ba96a963716594e
(Sep 22, 2024), MIT licensed (see LICENSE). Copyright (c) 2024 Ghost Note Audio / Ghost Note Engineering Ltd.
Only the DSP folder and Parameters.h are used (not the demo program, Programs.h or the
Visual Studio files). Changes, each marked `[Drum Foundry patch]` in the source:

- `Utils.h`, `ModulatedDelay.h`, `ModulatedAllpass.h`: the modulation start phases come from a
  per-thread LCG (`Utils::NextPhaseRandom()`, seeded by the host before building a
  ReverbController) instead of `std::rand()`, so the same settings always render the same tail.
- `ModulatedDelay.h`, `ModulatedAllpass.h`, `MultitapDelay.h`: the fixed buffer sizes can be
  overridden with `CLOUDSEED_DELAY_BUFFER_SIZE`, `CLOUDSEED_ALLPASS_BUFFER_SIZE` and
  `CLOUDSEED_TAP_BUFFER_SIZE` (Drum Foundry uses smaller buffers and limits the delay times to match).
  `std::sinf` became `std::sin` (not in every standard library).
- `ReverbChannel.h`: `UpdateLines()` caches the random line seeds and only regenerates them when
  the seed or cross seed changes, so changing size, decay or modulation never allocates.
- `Parameters.h`: `strcpy_s` (Microsoft only) became `snprintf`.
- `ReverbController.h`: `SetParameterScaled()` sets a parameter in its own units (ms, seconds,
  dB, counts), skipping the 0..1 curve, so Drum Foundry can map its own parameters directly.
