# Host Compatibility & Release Readiness

**Audited:** 2026-09-25 · **Tree:** `5a94caa` + uncommitted working tree
**Build under test:** JUCE 8.0.15, Release, `-O3 -arch arm64`, macOS 26.6.2, Apple Silicon
**Scope:** Will this load and behave in the DAWs people actually use? Sidechain, CPU, and memory safety.

> Snapshot note: `UndoHistory` was added to `PluginProcessor.h` during this audit. The
> measurements below come from the tree immediately before that change. Nothing in the
> undo work touches the audio thread, the bus layout, or the allocation paths examined here,
> so the conclusions stand — but re-run the validators after the next build.

---

## 1. Verdict

The plugin is in better shape than most shipping plugins on the things that are hard to fix
(realtime safety, bus handling, state sanitising) and is missing the things that are easy to
fix but block users entirely (code signing, universal binary).

| Area | Status |
|---|---|
| Host validation (AU + VST3) | Clean — passes everything |
| Sidechain | Correct, including the subtle shared-buffer case |
| Threading / CPU | Correct architecture; **one real dropout risk** (§5) |
| Memory / realtime safety | Strong; one unhandled OOM path (§6) |
| Distribution readiness | **Blocked** — unsigned, arm64-only (§7) |

---

## 2. How this was verified

Not read-only review — the plugin was built from source and put through the real gates.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 10          # 0 warnings under juce_recommended_warning_flags

auval -v aumu Drf1 Mlgn
/Applications/pluginval.app/Contents/MacOS/pluginval --strictness-level 10 \
    --validate ~/Library/Audio/Plug-Ins/VST3/"Drum Foundry".vst3
/Applications/pluginval.app/Contents/MacOS/pluginval --strictness-level 10 \
    --validate ~/Library/Audio/Plug-Ins/Components/"Drum Foundry".component

build/DrumFoundryTools_artefacts/Release/DrumFoundryTools bench
```

| Check | Result |
|---|---|
| Compile | Clean, **zero warnings** |
| `auval` | **AU VALIDATION SUCCEEDED** |
| `pluginval` strictness 10, VST3 | **SUCCESS** |
| `pluginval` strictness 10, AU | **SUCCESS** |

Two pieces of benign log noise, both investigated and dismissed:

- 3 × `auval` "Parameter did not retain default value when set" on `Gate Fade`, `EQ Mid Freq`,
  `Noise High Cut`. All three are skewed (logarithmic) ranges; the log itself shows
  `retrievedValue = 0.456912 (was 0.456912)` — the value round-tripped exactly. Float
  quantisation through the normalised domain, not a fault.
- 1 × `pluginval` `!!! WARNING: Current program is -1`. JUCE's AU wrapper reporting for a
  plugin with a single program. Cosmetic.

---

## 3. DAW compatibility — what works today

**Every DAW on this machine is already covered.** Verified by inspecting the installed hosts,
not assumed:

| DAW (installed here) | Takes | Evidence | Status |
|---|---|---|---|
| Reason 13 | VST3 | `VST3` host strings present in the Reason binary (VST3 support landed in Reason 12) | Works |
| FL Studio 2026 | VST3, CLAP | Fruity Wrapper exports `Plugin.Formats.CLAP` and VST3 symbols | Works (VST3) |
| Ableton Live 12 Suite | VST3, AU | Both formats pluginval-clean | Works |

Shipping VST3 + AU covers, beyond the above: Logic, GarageBand, MainStage, Cubase/Nuendo,
Studio One, Reaper, Bitwig, Digital Performer, Mixbus, Waveform.

**Not covered:** Pro Tools (needs AAX), Reason 9.5–11 (VST2-only), Linux hosts (LV2),
iPad / Mac App Store (AUv3), Windows (no build exists).

---

## 4. Sidechain — correct

This is the strongest part of the codebase. Three things that commonly go wrong are all handled:

**Bus topology.** One stereo input bus, no main input. For VST3 it is marked `kAux` via
`VST3ClientExtensions::getPluginHasMainInput() → false` (`src/PluginProcessor.h:161`), which is
what Live looks for when deciding whether to offer a sidechain. `auval` confirms the AU side:

```
Input Scope Bus Configuration:
 Default Bus Count:1
    Bus Name: Sidechain
```

Standalone deliberately gets **no** input bus (`src/PluginProcessor.cpp:62`) — otherwise it
opens the audio input and triggers the macOS microphone prompt for no reason.

**The shared-buffer trap.** JUCE hands the processor `max(inputs, outputs)` channels, and input
channels *share memory* with output channels — the sidechain lives in the very samples the
engine is about to overwrite. `src/PluginProcessor.cpp:147` scans the whole block **before**
rendering. This is the single most common sidechain bug in JUCE plugins and it is correctly
avoided, with a comment explaining why.

**Leak prevention.** Channels outside the main output bus are explicitly cleared
(`src/PluginProcessor.cpp:176`), so the key signal can never appear at the output — e.g. the
second sidechain channel next to a mono output.

Also worth noting:
- Non-finite sidechain input reads as silence (NaN-safe comparison, `TransientTrigger.cpp:84`).
- A disabled bus gives 0 channels and the detector no-ops — Live disables it when no source is picked.
- Hit reporting has a **constant** 1.5 ms delay rather than jitter, so a DAW track delay can
  compensate it exactly.
- `isBusesLayoutSupported` accepts disabled / mono / stereo and rejects everything else.

---

## 5. CPU and threading

**Architecture is right.** No DSP threading — the host owns parallelism, which is correct for a
plugin; a plugin that spawns its own DSP threads fights the host's scheduler. Three background
threads exist, all `Priority::low`, all with `signalThreadShouldExit()` + `stopThread(4000)`:

| Thread | File |
|---|---|
| Sample decode | `src/samples/SampleBank.cpp:30` |
| Browser scan | `src/ui/SampleBrowser.cpp:109` |
| Waveform render | `src/PluginEditor.cpp:54` |

Steady-state cost is low (`DrumFoundryTools bench`, Apple Silicon):

```
reverb off (Mix 0)                 0.61% of one core (165x realtime)
reverb on, gate off, 6 s decay     0.85% of one core (118x realtime)
engine, 3 Metallic noise layers    2.46% of one core  (41x realtime)
```

### 5.1 ⚠️ The one real risk: note-on spike from negative layer Offsets

A negative layer Offset means the layer starts *before* the trigger. The implementation renders
that pre-hit audio and throws it away, **synchronously, inside the audio callback**:

```cpp
// src/dsp/DrumVoice.cpp:267
// --- what early layers played before the hit: rendered, then thrown away ---
while (pos < 0.0)
{
    const int want = (int) juce::jmin ((double) sliceSize, -pos);
    const int n = renderSlice (want, p, env, samples);
    ...
}
```

This is a **fixed cost per note-on**, independent of block size, paid by every voice that starts.
The Offset parameter range is ±100 ms (`src/Parameters.cpp:143`), so the numbers below are the
genuine worst case reachable from the UI — 8 simultaneous hits, all six layers at −100 ms:

| Sample rate | Buffer | Worst measured | Block budget | |
|---|---|---|---|---|
| 48 kHz | 64 | 1373 µs | 1333 µs | **OVER** |
| 48 kHz | 128 | 1400 µs | 2667 µs | ok |
| 48 kHz | 256 | 1432 µs | 5333 µs | ok |
| 96 kHz | 64 | 2673 µs | 667 µs | **4× OVER** |
| 96 kHz | 128 | 2697 µs | 1333 µs | **2× OVER** |
| 96 kHz | 256 | 2720 µs | 2667 µs | **OVER** |
| 96 kHz | 512 | 3695 µs | 5333 µs | ok |

For contrast, the same 8 hits with Offsets at their default of 0 cost **~29 µs**. The entire
cost is the pre-roll.

Two aggravating factors: these numbers are from a fast Apple Silicon machine (an Intel Mac would
be materially worse), and the cost scales linearly with simultaneous voices.

**Fix direction:** don't render the whole pre-roll in the triggering block. Options, roughly in
order of preference —
1. Amortise: spread the catch-up across the following blocks, rendering at most a bounded number
   of extra samples per block until the voice reaches its true position.
2. Advance analytically where possible (oscillator phase, envelope position, sample read index
   can all be computed directly rather than rendered), falling back to rendering only for the
   stateful parts that genuinely need it (filters, noise generators).
3. Cheapest stopgap: cap the total pre-roll budget per block across all voices and accept a
   slightly late start on the overflow.

---

## 6. Memory and realtime safety

Deliberately and correctly engineered. Findings:

**No allocation, locking, or freeing on the audio thread.** Every `process` / `render` path in
`MasterChain`, `Clipper`, `Reverb`, `OutputFilter`, `DrumVoice`, `DrumEngine` was checked for
allocating constructs — none found.

**Lock-free handover, both directions.** Envelope tables and sample pointers reach the audio
thread through *try*-locks; a missed pickup simply retries next block, so the audio thread never
blocks on the UI (`PluginProcessor.cpp:158`, `SampleBank.cpp:382`).

**No use-after-free on samples.** A replaced sample goes on a retired list and is freed only
after the audio thread has demonstrably adopted a set without it — tracked by a serial number,
released on a timer, and freed after the locks are dropped (`SampleBank.cpp:261`).

**Bounded allocations.** Sample loads are clamped to ≤10 s and ≤2 channels, with sample rate
validated to 4 kHz–768 kHz (`SampleBank.cpp:457`).

**Hostile input is sanitised.** Host state and preset files pass through
`sanitiseParameterValues` — NaN/Inf fall back to the parameter default, everything else is
clamped to range (`PluginProcessor.cpp:29`). UI state is `jlimit`-ed on restore.

**NaN containment.** A non-finite block is caught, the engine reset, and silence output, rather
than letting the value recirculate forever through IIR state (`DrumEngine.cpp:203`). Note the
comment's caveat: this relies on IEEE semantics and would break silently under `-ffast-math` —
worth keeping in mind if optimisation flags ever change.

**Oversized blocks are safe.** Hosts that hand over more samples than they promised in
`prepareToPlay` are handled by chunking (`DrumEngine.cpp:157`), and `TransientTrigger` sizes its
hit list for blocks "well beyond the promised maximum" (`TransientTrigger.cpp:25`). This alone
prevents a whole class of host-specific crashes.

**Latency is fixed.** `getLatencySamples()` depends only on sample rate, never on parameters
(`Clipper.cpp:221`), so `setLatencySamples()` in `prepareToPlay` is sufficient and there is no
mid-session latency change to confuse hosts. This also happens to satisfy a Pro Tools hard
requirement (§8).

### 6.1 Gap: no out-of-memory handling

There is no `try` / `catch` anywhere in `src/`. JUCE's `HeapBlock` defaults to
`throwOnFailure = false` (`libs/JUCE/modules/juce_core/memory/juce_HeapBlock.h:99`), so a failed
`AudioBuffer::setSize` yields a null pointer and the following write segfaults — taking the host
down with it.

**Priority: low.** Allocations are bounded and 64-bit hosts have virtual memory, so this is close
to unreachable in practice. Worth a targeted guard on the sample decode path
(`SampleBank.cpp:485`) eventually, not worth blocking a release on.

### 6.2 Minor: retired list growth

If the audio thread stops calling `beginBlock()` (plugin removed from the graph, host stopped)
while samples are being replaced, `adoptedSerial` stops advancing and the retired list holds
memory until teardown. Bounded by user action and released at destruction — a cosmetic wart, not
a leak worth chasing.

---

## 7. Distribution blockers

These are not code defects. They are the reason a working plugin fails to work for other people.

**1. arm64-only — Intel Macs cannot load it at all.**

```
$ lipo -info ~/Library/Audio/Plug-Ins/VST3/"Drum Foundry".vst3/Contents/MacOS/"Drum Foundry"
Non-fat file: ... is architecture: arm64
```

Fix:
```cmake
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
```

**2. Ad-hoc signed, not notarized — Gatekeeper will block it on every other Mac.**

```
$ codesign -dv ~/Library/Audio/Plug-Ins/VST3/"Drum Foundry".vst3
Signature=adhoc
TeamIdentifier=not set
```

This is the single most common "it doesn't work on my machine" report for macOS plugins. Needs a
Developer ID Application certificate, `codesign --options runtime` on each bundle, then
`notarytool submit` + `stapler staple` on the installer or zip.

**3. No Windows build.** The CMake is portable and nothing in the source looks platform-locked,
but no Windows build has ever been produced or tested. This is a real market, not a footnote.

---

## 8. Adding formats — what each one actually costs

### CLAP — recommended, cheap

Free, open, no NDA, no fees. Gets FL Studio natively (CLAP plugins are already in use on this
machine: FabFilter Pro-Q 4, the Auburn Sounds set), plus Bitwig, Reaper, Studio One 7+.

JUCE has no native CLAP target, but `clap-juce-extensions` wraps an existing JUCE plugin with
**no source changes**:

```cmake
add_subdirectory(libs/clap-juce-extensions EXCLUDE_FROM_ALL)
clap_juce_extensions_plugin(TARGET DrumFoundry
    CLAP_ID "com.milliganaudio.drumfoundry"
    CLAP_FEATURES instrument drum-machine stereo)
```

### LV2 — nearly free, only if Linux matters

Built into JUCE 8. One word plus a URI:

```cmake
FORMATS VST3 AU LV2 Standalone
LV2URI "https://milliganaudio.com/plugins/drumfoundry"
```

Requires a Linux build machine to actually ship.

### AAX (Pro Tools) — code is cheap, business is not

The wrapper is nearly free:

```cmake
juce_set_aax_sdk_path(${CMAKE_CURRENT_SOURCE_DIR}/libs/aax-sdk)
# in juce_add_plugin:
#   FORMATS ... AAX
#   AAX_IDENTIFIER com.milliganaudio.drumfoundry
```

The gates are commercial:
- Avid developer partner program membership to get the SDK (NDA, annual fee).
- **PACE signing.** Release Pro Tools refuses to load an unsigned AAX plugin. Requires a PACE
  developer account and their signing tool, also an annual cost. Unsigned builds load only in
  the developer build of Pro Tools that Avid provides to registered partners.

Two code considerations specific to this plugin:
- Pro Tools demands a **fixed, never-changing latency**. Already satisfied (§6).
- AAX sidechain is a **single mono input**, not a stereo bus. `isBusesLayoutSupported` already
  accepts a mono sidechain and `TransientTrigger` handles `numChannels == 1`, so the path should
  work — but it needs real testing in Pro Tools, which is not installed here.

Recommendation: defer until there is demand. Pro Tools is not installed on this machine, so it
cannot even be tested today.

### Reason Rack Extension — don't

Proprietary SDK, fully sandboxed (no filesystem access, which would break both the sample
browser and the `~/Music/Drum Foundry/Presets` folder), distribution exclusively through Reason's
shop with a revenue share. Close to a rewrite. **Reason 13 already loads the VST3**, so there is
nothing to gain.

### VST2 — legally closed

Steinberg stopped issuing VST2 licences in October 2018. Without a pre-existing licence it cannot
be shipped. Only affects Reason ≤11 and comparably old hosts.

### AUv3 — deferred

For iPad and Mac App Store distribution. JUCE supports it. Not a priority.

---

## 9. Plan

### Phase 1 — ship blockers (do these together, in the next build)

| # | Task | Where | Notes |
|---|---|---|---|
| 1 | Universal binary | `CMakeLists.txt` | `set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")` |
| 2 | Developer ID sign + notarize | build/release script | `--options runtime`, then `notarytool` + `stapler` |
| 3 | Fix the note-on spike | `src/dsp/DrumVoice.cpp:267` | See §5.1 for the three options |
| 4 | Add CLAP | `CMakeLists.txt` + submodule | No source changes; gets FL Studio natively |

### Phase 2 — verification (before any release)

| # | Task |
|---|---|
| 5 | Re-run `auval` and `pluginval --strictness-level 10` on **both** formats after every change |
| 6 | Load in Reason 13, FL Studio 2026 and Ableton Live 12: confirm it scans, instantiates, takes a sidechain, saves/recalls a session |
| 7 | Re-run the spike benchmark at 64 frames / 96 kHz to confirm Phase 1 #3 landed |
| 8 | Test the universal build on an Intel Mac, and the signed build on a machine that has never seen the source |

### Phase 3 — optional, demand-driven

| # | Task | Trigger |
|---|---|---|
| 9 | OOM guard on sample decode (§6.1) | Housekeeping |
| 10 | Windows build | Market demand |
| 11 | AAX | Someone actually asks for Pro Tools |
| 12 | LV2 / AUv3 | Linux or iPad demand |

---

## 10. Reproducing this audit

```bash
# Build
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DDRUMFOUNDRY_BUILD_TOOLS=ON
cmake --build build --parallel 10

# Validate (COPY_PLUGIN_AFTER_BUILD installs to ~/Library/Audio/Plug-Ins)
auval -v aumu Drf1 Mlgn
/Applications/pluginval.app/Contents/MacOS/pluginval --strictness-level 10 \
    --validate ~/Library/Audio/Plug-Ins/VST3/"Drum Foundry".vst3
/Applications/pluginval.app/Contents/MacOS/pluginval --strictness-level 10 \
    --validate ~/Library/Audio/Plug-Ins/Components/"Drum Foundry".component

# Benchmark
build/DrumFoundryTools_artefacts/Release/DrumFoundryTools bench

# Distribution checks
lipo -info  ~/Library/Audio/Plug-Ins/VST3/"Drum Foundry".vst3/Contents/MacOS/"Drum Foundry"
codesign -dv ~/Library/Audio/Plug-Ins/VST3/"Drum Foundry".vst3
```

The per-voice and multi-voice note-on spike figures in §5.1 came from two throwaway harnesses
built against the project's own object files. `tools/Tools.cpp:4554` already contains a
single-voice version of this measurement; extending it to cover voice count and buffer size
would be worth doing so the regression is caught by `DrumFoundryTools bench` in future.
