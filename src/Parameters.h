// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "dsp/FxChain.h"
#include <array>
#include <vector>

namespace drum
{

namespace pid
{
    // Global
    inline constexpr auto length    = "length";
    inline constexpr auto tune      = "tune";
    inline constexpr auto keytrack  = "keytrack";
    inline constexpr auto velSens   = "velSens";
    inline constexpr auto choke     = "choke";

    // Body (tonal drum head)
    inline constexpr auto bodyOn     = "bodyOn";
    inline constexpr auto bodyStyle  = "bodyStyle";
    inline constexpr auto bodySmpTune = "bodySmpTune";
    inline constexpr auto bodyOffset = "bodyOffset";   // ms, - = earlier (the start is cut), + = later
    inline constexpr auto bodySolo   = "bodySolo";
    inline constexpr auto bodyInvert = "bodyInvert";  // polarity flip
    inline constexpr auto bodyLevel  = "bodyLevel";
    inline constexpr auto bodyShape  = "bodyShape";
    inline constexpr auto otLevel    = "otLevel";
    inline constexpr auto otRatio    = "otRatio";
    inline constexpr auto modes      = "modes";

    // Noise (snare wires)
    inline constexpr auto noiseOn     = "noiseOn";
    inline constexpr auto noiseStyle  = "noiseStyle";
    inline constexpr auto noiseSmpTune = "noiseSmpTune";
    inline constexpr auto noiseOffset  = "noiseOffset";
    inline constexpr auto noiseSolo    = "noiseSolo";
    inline constexpr auto noiseInvert  = "noiseInvert";
    inline constexpr auto noiseLevel  = "noiseLevel";
    inline constexpr auto noiseHP     = "noiseHP";
    inline constexpr auto noiseLP     = "noiseLP";
    inline constexpr auto noiseColor  = "noiseColor";
    inline constexpr auto noiseCouple = "noiseCouple";
    inline constexpr auto noiseWidth  = "noiseWidth";
    inline constexpr auto noiseGrit   = "noiseGrit";

    // Click (transient)
    inline constexpr auto clickOn     = "clickOn";
    inline constexpr auto clickType   = "clickType";
    inline constexpr auto clickSmpTune = "clickSmpTune";
    inline constexpr auto clickOffset  = "clickOffset";
    inline constexpr auto clickSolo    = "clickSolo";
    inline constexpr auto clickInvert  = "clickInvert";
    inline constexpr auto clickLevel  = "clickLevel";
    inline constexpr auto clickTone   = "clickTone";
    inline constexpr auto clickLength = "clickLength";

    // Drive
    inline constexpr auto driveOn     = "driveOn";
    inline constexpr auto driveType   = "driveType";
    inline constexpr auto driveAmount = "driveAmount";
    inline constexpr auto driveMix    = "driveMix";

    // EQ. Bands 2-8 have ids "eq<N><Suffix>" (see eqBandParamId()); bands 1-3 keep the
    // old 3-band EQ's ids for its knobs: eqLow is band 1's Gain, eqMidFreq / eqMidGain
    // band 2's Freq / Gain, eqHigh band 3's Gain.
    inline constexpr auto eqOn      = "eqOn";
    inline constexpr auto eqStyle   = "eqStyle";   // choice: eqStyleNames
    inline constexpr auto eqLow     = "eqLow";
    inline constexpr auto eqMidFreq = "eqMidFreq";
    inline constexpr auto eqMidGain = "eqMidGain";
    inline constexpr auto eqHigh    = "eqHigh";
    inline constexpr const char* eqBandOn      = "On";
    inline constexpr const char* eqBandType    = "Type";    // choice: eqTypeNames
    inline constexpr const char* eqBandFreq    = "Freq";
    inline constexpr const char* eqBandGain    = "Gain";
    inline constexpr const char* eqBandQ       = "Q";
    inline constexpr const char* eqBandSlope   = "Slope";   // choice: eqSlopeNames (the cuts)
    inline constexpr const char* eqBandDyn     = "Dyn";     // dynamic: the band's gain follows its level
    inline constexpr const char* eqBandThresh  = "Thresh";
    inline constexpr const char* eqBandAttack  = "Attack";
    inline constexpr const char* eqBandRelease = "Release";

    // Output filter: the last thing before the output gain and clipper.
    inline constexpr auto outHP      = "outHP";
    inline constexpr auto outHPSlope = "outHPSlope";
    inline constexpr auto outLP      = "outLP";
    inline constexpr auto outLPSlope = "outLPSlope";

    // Output
    inline constexpr auto outGain = "outGain";
    inline constexpr auto clip    = "clip";        // choice: clipModeNames
    inline constexpr auto clipDrive   = "clipDrive";   // dB into the clipper
    inline constexpr auto clipCeiling = "clipCeiling"; // dBFS the clipper limits to

    // Reverb, with a gate that cuts the tail a set time after each hit
    inline constexpr auto revOn       = "revOn";
    inline constexpr auto revMix      = "revMix";
    inline constexpr auto revSize     = "revSize";
    inline constexpr auto revDecay    = "revDecay";
    inline constexpr auto revPreDelay = "revPreDelay";
    inline constexpr auto revLowCut   = "revLowCut";
    inline constexpr auto revHighCut  = "revHighCut";
    inline constexpr auto revGate     = "revGate";
    inline constexpr auto revGateTime = "revGateTime";
    inline constexpr auto revGateFade = "revGateFade";
    // The reverb's engine (revEngineNames) and the Cloud engine's own controls.
    inline constexpr auto revEngine    = "revEngine";
    inline constexpr auto revDiffusion = "revDiffusion";
    inline constexpr auto revModDepth  = "revModDepth";
    inline constexpr auto revModRate   = "revModRate";
    inline constexpr auto revWidth     = "revWidth";
    inline constexpr auto revEarly     = "revEarly";
    inline constexpr auto revDamp      = "revDamp";

    // The master chain's second Drive (the first is driveOn ..). Its order is the
    // patch's FxChain, which isn't a parameter (DrumFoundryProcessor::setFxChain()).
    inline constexpr auto drive2On     = "drive2On";
    inline constexpr auto drive2Type   = "drive2Type";
    inline constexpr auto drive2Amount = "drive2Amount";
    inline constexpr auto drive2Mix    = "drive2Mix";

    /** The chain's compressors ("comp1", "comp2") and the output stage's ("outComp", when
        Clip Mode is Comp; it has no On, Knee or Mix): ids are prefix + suffix (compParamId()). */
    inline constexpr const char* compOn      = "On";
    inline constexpr const char* compThresh  = "Thresh";
    inline constexpr const char* compRatio   = "Ratio";
    inline constexpr const char* compAttack  = "Attack";
    inline constexpr const char* compRelease = "Release";
    inline constexpr const char* compKnee    = "Knee";
    inline constexpr const char* compMakeup  = "Makeup";
    inline constexpr const char* compMix     = "Mix";
    inline constexpr const char* compStyle   = "Style";   // choice: compStyleNames (every compressor, the output's too)
    inline constexpr auto outComp = "outComp";

    /** The chain's clippers: "fxClip<N><Suffix>", e.g. "fxClip1Drive" (chainClipParamId()). */
    inline constexpr const char* chainClipOn      = "On";
    inline constexpr const char* chainClipShape   = "Shape";   // chainClipShapeNames
    inline constexpr const char* chainClipDrive   = "Drive";
    inline constexpr const char* chainClipCeiling = "Ceiling";

    // Sidechain trigger (not stored in presets)
    inline constexpr auto scTrigger   = "scTrigger";
    inline constexpr auto scThreshold = "scThreshold";
    inline constexpr auto scRetrigger = "scRetrigger";

    /** Extra layers 1..kNumExtraLayers. Ids are "lay<N><Suffix>", e.g. "lay1Level". */
    inline constexpr const char* layOn     = "On";
    inline constexpr const char* laySource = "Source";
    inline constexpr const char* layLevel  = "Level";
    inline constexpr const char* layTune   = "Tune";
    inline constexpr const char* layEnv    = "Env";      // where its level curve comes from (layerEnvNames)
    inline constexpr const char* layPitchEnv = "PitchEnv"; // where its pitch curve comes from (layerEnvNames)
    inline constexpr const char* layDelay  = "Delay";  // the layer's Offset: -100..+100 ms
    inline constexpr const char* laySolo   = "Solo";
    inline constexpr const char* layInvert = "Invert";
    inline constexpr const char* layBodyStyle = "BodyStyle"; // Body source only
    inline constexpr const char* layHP     = "HP";
    inline constexpr const char* layLP     = "LP";
    inline constexpr const char* layPan    = "Pan";
    inline constexpr const char* layNoiseStyle = "NoiseStyle"; // Noise source only
    inline constexpr const char* layClickType  = "ClickType";  // Click source only
    inline constexpr const char* layColor      = "Color";      // Noise source: like the Noise layer's Color
    inline constexpr const char* layTone       = "ClickTone";  // Click source: like the Click layer's Tone
    inline constexpr const char* layLength     = "ClickLength";// Click source: like the Click layer's Length

    /** LFOs 1..kNumLfos. Ids are "lfo<N><Suffix>", e.g. "lfo1Rate"; a target slot's
        are "lfo<N><Suffix><K>", e.g. "lfo1Dest2" (see lfoParamId()). */
    inline constexpr const char* lfoShape = "Shape";
    inline constexpr const char* lfoRate  = "Rate";
    inline constexpr const char* lfoSync  = "Sync";
    inline constexpr const char* lfoMode  = "Mode";
    inline constexpr const char* lfoPhase = "Phase";
    inline constexpr const char* lfoDest  = "Dest";   // per slot: what it modulates (0 = none, else modTargetIds()[n - 1])
    inline constexpr const char* lfoAmt   = "Amt";    // per slot: how far, -1..1 of the target's whole range
}

/** The EQ's bands (ParametricEq.h). */
inline constexpr int kNumEqBands = 8;

/** Compressor `n` of the chain (0 or 1) -> "comp1" / "comp2" + suffix; n < 0 -> the output stage's ("outComp" + suffix). */
juce::String compParamId (int n, const char* suffix);
/** Clipper `n` of the chain (0 or 1) -> "fxClip1" / "fxClip2" + suffix. */
juce::String chainClipParamId (int n, const char* suffix);

/** Parameter id for EQ band `band` (0-based), e.g. eqBandParamId (4, pid::eqBandQ) -> "eq5Q".
    Bands 1-3 keep the old 3-band EQ's ids where it had a knob (see pid::eqLow). */
juce::String eqBandParamId (int band, const char* suffix);

inline const juce::StringArray eqTypeNames  { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Band Pass", "Tilt" };
inline const juce::StringArray eqSlopeNames { "6 dB", "12 dB", "18 dB", "24 dB", "36 dB", "48 dB", "72 dB", "96 dB" };
/** The eqSlopeNames index a shelf starts at: 12 dB/oct (what every shelf was before Slope). */
inline constexpr int kEqShelfSlope = 1;
/** Clean: analog-matched curves. Digital: bilinear (the old EQ). British: bells narrow
    as they boost or cut. Pultec: resonant shelves, broad bells. See ParametricEq.h. */
inline const juce::StringArray eqStyleNames { "Clean", "Digital", "British", "Pultec" };

/** Extra layers the user can add on top of Body, Noise and Click. */
inline constexpr int kNumExtraLayers = 3;

/** Parameter id for extra layer `layer` (0-based), e.g. layerParamId (0, pid::layLevel) -> "lay1Level". */
juce::String layerParamId (int layer, const char* suffix);

/** LFOs, and how many parameters each one can modulate at once (its target slots). */
inline constexpr int kNumLfos = 3;
inline constexpr int kLfoSlots = 4;
inline constexpr int kNumModRoutes = kNumLfos * kLfoSlots;

/** Parameter id for LFO `lfo` (0-based), e.g. lfoParamId (0, pid::lfoRate) -> "lfo1Rate". With
    `slot` (0-based), a target slot's: lfoParamId (0, pid::lfoDest, 1) -> "lfo1Dest2". */
juce::String lfoParamId (int lfo, const char* suffix, int slot = -1);

inline const juce::StringArray lfoShapeNames { "Sine", "Triangle", "Saw Down", "Saw Up", "Square", "S&H", "Drift" };
/** Retrig: each hit starts the LFO at its Phase. One Shot: the same, but it runs one
    cycle and holds its last value (an extra envelope). Free: it never restarts. */
inline const juce::StringArray lfoModeNames  { "Retrig", "One Shot", "Free" };
/** Free: Rate in Hz. Anything else: one cycle per that note length at the host's tempo. */
inline const juce::StringArray lfoSyncNames  { "Free", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64",
                                               "1/4 T", "1/8 T", "1/16 T", "1/32 T", "1/4 D", "1/8 D", "1/16 D" };
/** Beats (quarter notes) per LFO cycle for each lfoSyncNames entry (0 for Free). */
float lfoSyncBeats (int sync) noexcept;

/** The parameters an LFO can modulate: every continuous knob of the patch. Their
    order is stored in sessions and presets (an LFO slot's Dest is index + 1 here,
    0 = none), so entries are only ever appended. */
const juce::StringArray& modTargetIds();
/** The Dest choice list: "None", then a name per modTargetIds() entry. */
const juce::StringArray& modTargetChoiceNames();
/** Index into modTargetIds(), or -1 when the parameter can't be modulated. */
int modTargetIndex (const juce::String& paramId);

// Built-in sound sources per layer. "Sample" is always last and plays the
// layer's imported sample (see SampleData.h); the other entries are synthesised.
inline const juce::StringArray bodyStyleNames  { "Classic", "Membrane", "808", "909", "Kick", "FM", "Round", "Taper", "Hard", "Fold", "Tonal",
                                                   "Sine", "Saw", "Square", "Round Square", "Triangle", "Sample" };
inline const juce::StringArray noiseStyleNames { "Wires", "White", "Pink", "Metallic", "Crackle", "Sample" };
inline const juce::StringArray clickTypeNames  { "Tick", "Snap", "Stick", "Rim", "Pop", "Beater", "Clap", "Felt", "Plastic", "Skin", "Wood", "Tok", "Sample" };
/** Extra layer source: an imported sample, or a simple synth voice. */
inline const juce::StringArray layerSourceNames { "Sample", "Sine", "Noise", "Click", "Body" };
/** Styles an extra Body layer can use: the core list without "Sample". */
inline const juce::StringArray layerBodyStyleNames { "Classic", "Membrane", "808", "909", "Kick", "FM", "Round", "Taper", "Hard", "Fold", "Tonal",
                                                    "Sine", "Saw", "Square", "Round Square", "Triangle" };
/** Styles an extra Noise / Click layer can use: the core lists without "Sample"
    (an extra layer plays a sample through its Sample source instead). */
inline const juce::StringArray layerNoiseStyleNames { "Wires", "White", "Pink", "Metallic", "Crackle" };
inline const juce::StringArray layerClickTypeNames  { "Tick", "Snap", "Stick", "Rim", "Pop", "Beater", "Clap", "Felt", "Plastic", "Skin", "Wood", "Tok" };
/** Where an extra layer's level curve (and, separately, its pitch curve) comes from:
    a core layer's curve (coupled to it), Flat (full level / no pitch movement), or the
    layer's own curve. Indices are LayerEnv (StyleInfo.h); Own came last so that old
    sessions keep their indices. */
inline const juce::StringArray layerEnvNames { "Body", "Noise", "Click", "Flat", "Own" };
inline const juce::StringArray filterSlopeNames { "12 dB", "24 dB", "48 dB" };
/** How the output clipper shapes what goes over the ceiling. Off = no clipper
    (the output can then exceed 0 dBFS). The old on/off switch maps 0 -> Off, 1 -> Soft. */
inline const juce::StringArray clipModeNames { "Off", "Soft", "Hard", "Tube", "Tape", "Fold", "Comp" };
/** The output stage's Comp: the output compressor (outComp) instead of a clipper. */
inline constexpr int clipModeComp = 6;
/** The compressor's characters (CompModels.h): Clean, and four modelled on classic units:
    FET (1176-style), Opto (LA-2A-style), Bus (SSL-style bus), Punch (dbx 160-style). */
inline const juce::StringArray compStyleNames { "Clean", "FET", "Opto", "Bus", "Punch" };
/** A chain clipper's shapes: Clipper::soft + index. */
inline const juce::StringArray chainClipShapeNames { "Soft", "Hard", "Tube", "Tape", "Fold" };

inline int sampleStyleIndex (const juce::StringArray& names) { return names.size() - 1; }
/** The reverb's engine: Classic (the original 8-line FDN; patches from before Cloud
    load on it) or Cloud (Ghost Note Audio's Cloud Seed, the default). */
inline const juce::StringArray revEngineNames { "Classic", "Cloud" };
inline constexpr int revEngineClassic = 0, revEngineCloud = 1;
inline const juce::StringArray driveTypeNames { "Soft", "Hard", "Tube", "Fold", "Crush" };

/** Level parameters bottom out at this value, which means silence. */
inline constexpr float kMinLevelDb = -60.0f;

inline float levelDbToGain (float db) noexcept
{
    return db <= kMinLevelDb + 0.01f ? 0.0f : juce::Decibels::decibelsToGain (db);
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

/** Plain-value snapshot of every parameter, taken once per block. The DSP only
    ever sees this struct, so the same engine can run live or offline. */
struct DrumParams
{
    float lengthMs    = 450.0f;
    float tuneHz      = 185.0f;
    bool  keytrack    = true;
    float velSens     = 0.6f;
    bool  choke       = false;

    bool  bodyOn      = true;
    int   bodyStyle   = 0;      // index into bodyStyleNames
    float bodySmpTune = 0.0f;   // semitones, Sample style only
    float bodyGain    = 1.0f;
    float bodyShape   = 0.1f;
    float bodyOffsetMs = 0.0f;  // - = the layer starts early (its start is cut off), + = late
    bool  bodySolo    = false;
    bool  bodyInvert  = false;
    float otGain      = 0.3f;
    float otRatio     = 1.594f;
    float modes       = 0.25f;

    bool  noiseOn      = true;
    int   noiseStyle   = 0;     // index into noiseStyleNames
    float noiseSmpTune = 0.0f;  // semitones, Sample style only
    float noiseOffsetMs = 0.0f;
    bool  noiseSolo    = false;
    bool  noiseInvert  = false;
    float noiseGain   = 0.7f;
    float noiseHP     = 450.0f;
    float noiseLP     = 9000.0f;
    float noiseColor  = 0.4f;
    float noiseCouple = 0.3f;
    float noiseWidth  = 0.35f;
    float noiseGrit   = 0.0f;

    bool  clickOn       = true;
    int   clickType     = 1;    // index into clickTypeNames
    float clickSmpTune  = 0.0f; // semitones, Sample type only
    float clickOffsetMs = 0.0f;
    bool  clickSolo     = false;
    bool  clickInvert   = false;
    float clickGain     = 0.4f;
    float clickTone     = 4500.0f;
    float clickLengthMs = 6.0f;

    bool  driveOn     = true;   // off: Mix goes to 0 (the oversampler still runs)
    int   driveType   = 0;
    float driveAmount = 0.2f;
    float driveMix    = 1.0f;

    bool  eqOn      = true;     // off: every band goes flat
    int   eqStyle   = 0;        // index into eqStyleNames
    struct EqBand
    {
        bool  on      = false;
        int   type    = 0;      // index into eqTypeNames
        float freq    = 1000.0f;
        float gainDb  = 0.0f;
        float q       = 1.0f;
        int   slope   = 3;      // index into eqSlopeNames
        bool  dynamic = false;
        float thresholdDb = -24.0f;
        float attackMs    = 5.0f;
        float releaseMs   = 80.0f;
    };
    /** Bands 1-3 start as the old 3-band EQ: a low shelf at 100 Hz, a bell at 900 Hz (Q 0.9)
        and a high shelf at 7 kHz, all at 0 dB. */
    std::array<EqBand, kNumEqBands> eqBands = []
    {
        std::array<EqBand, kNumEqBands> b {};
        b[0] = { true, 1, 100.0f, 0.0f, 0.70710678f, kEqShelfSlope };
        b[1] = { true, 0, 900.0f, 0.0f, 0.9f };
        b[2] = { true, 2, 7000.0f, 0.0f, 0.70710678f, kEqShelfSlope };
        return b;
    }();
    /** The EQ overlay's Listen: > 0 replaces the EQ's output with a band pass of its input
        at this frequency (and Q). Set by the processor from the UI, never stored. */
    float eqListenHz = 0.0f;
    float eqListenQ  = 4.0f;

    // Output filter. outHP <= 20 Hz and outLP >= 20 kHz mean that side is off.
    float outHP      = 20.0f;
    int   outHPSlope = 1;       // index into filterSlopeNames
    float outLP      = 20000.0f;
    int   outLPSlope = 1;

    float outGain = 1.0f;
    int   clipMode      = 1;    // index into clipModeNames; 0 = off, clipModeComp = outComp
    float clipDriveDb   = 0.0f;
    float clipCeilingDb = 0.0f;

    struct Layer
    {
        bool  on      = false;
        int   source  = 0;       // index into layerSourceNames
        float gain    = 1.0f;    // linear
        float tuneSt  = 0.0f;    // semitones (Sample: playback transpose; Sine: relative to Tune)
        int   env      = 4;      // level curve: index into layerEnvNames (4 = its own)
        int   pitchEnv = 4;      // pitch curve: index into layerEnvNames
        float delayMs = 0.0f;    // the layer's Offset, -100..+100 ms (- = early, start cut off)
        bool  solo    = false;
        bool  invert  = false;
        int   bodyStyle = 0;     // index into layerBodyStyleNames (Body source); `color` is its Shape
        float hp      = 20.0f;
        float lp      = 20000.0f;
        float pan     = 0.0f;    // -1..1
        int   noiseStyle = 1;    // index into layerNoiseStyleNames (Noise source)
        int   clickType  = 1;    // index into layerClickTypeNames (Click source)
        float color      = 0.4f; // Noise source
        float clickTone  = 4500.0f;
        float clickLengthMs = 6.0f;
    };
    std::array<Layer, kNumExtraLayers> layers {};

    // Reverb (parallel send after the EQ) and its gate. Gate Time counts from
    // the hit, not from the pre-delayed start of the wet signal.
    bool  revOn         = true;   // off: the wet signal fades out and the reverb stops
    float revMix        = 0.0f;   // wet level, linear; 0 = reverb off
    float revSize       = 0.6f;
    float revDecayMs    = 2500.0f; // RT60
    float revPreDelayMs = 10.0f;
    float revLowCut     = 200.0f;
    float revHighCut    = 8000.0f;
    bool  revGate       = true;
    float revGateMs     = 180.0f;
    float revFadeMs     = 15.0f;
    int   revEngine     = 1;      // revEngineNames
    float revDiffusion  = 0.7f;   // Cloud only, 0..1 (see CloudReverb::Settings)
    float revModDepth   = 0.3f;
    float revModRate    = 0.3f;
    float revWidth      = 1.0f;
    float revEarly      = 0.2f;
    float revDamp       = 0.4f;

    // The master chain: its order, and the modules that come in twos (the first Drive
    // is driveOn .. driveMix above, the EQ eqOn .., the reverb revOn ..).
    FxChain chain = FxChain::defaults();
    DriveSettings drive2;
    std::array<CompSettings, 2> comp {};
    std::array<ChainClipSettings, 2> chainClip {};
    CompSettings outComp;       // the output stage, when clipMode is clipModeComp

    /** The LFOs' own settings. */
    struct Lfo
    {
        int   shape  = 0;      // index into lfoShapeNames
        float rateHz = 4.0f;   // used when sync is 0 (Free)
        int   sync   = 0;      // index into lfoSyncNames
        int   mode   = 0;      // index into lfoModeNames
        float phase  = 0.0f;   // where a hit starts it, 0..1 of a cycle
    };
    std::array<Lfo, kNumLfos> lfos {};

    /** One LFO slot that modulates something. The target's unmodulated value and its
        range travel with it, so the engine can modulate a snapshot on its own (see
        applyModulation()). Only the first numRoutes entries are used. */
    struct ModRoute
    {
        int   target = -1;          // index into modTargetIds()
        int   lfo    = 0;
        float amount = 0.0f;        // -1..1 of the target's normalised range
        float base   = 0.0f;        // the target's own (plain) value
        float lo = 0.0f, hi = 1.0f, skew = 1.0f; // the target's NormalisableRange
    };
    std::array<ModRoute, kNumModRoutes> routes {};
    int numRoutes = 0;

    /** Host tempo, for synced LFOs (120 until a host reports one). */
    float bpm = 120.0f;
};

/** Writes the modulated value of every routed target into `p`: each target's
    normalised value moves by the sum of amount x LFO output (-1..1) over its routes,
    clamped to its range. `lfoValues` has kNumLfos entries. Doesn't allocate. */
void applyModulation (DrumParams& p, const DrumParams& base, const float* lfoValues) noexcept;

/** Sidechain trigger settings, read once per block. Kept apart from
    DrumParams because they drive the detector, not the voices, and because
    they are session settings rather than part of a patch. */
struct SidechainParams
{
    bool  enabled     = false;
    float thresholdDb = -24.0f;
    float retriggerMs = 50.0f;
};

/** Caches raw parameter pointers so reading a snapshot is lock-free and cheap.
    Safe to call from any thread. Every value it hands out is finite and inside
    its parameter's range, whatever a corrupt session managed to store. */
class ParamReader
{
public:
    explicit ParamReader (juce::AudioProcessorValueTreeState& state);
    DrumParams read() const noexcept;
    SidechainParams readSidechain() const noexcept;

private:
    /** One parameter's raw value plus its plain-value range and default, cached
        at construction so load() needs no lookups. */
    struct Param
    {
        std::atomic<float>* value = nullptr;
        float lo = 0.0f, hi = 1.0f, def = 0.0f, skew = 1.0f;

        /** Non-finite -> default, anything else clamped to the range. */
        float load() const noexcept
        {
            const float v = value->load();
            return std::isfinite (v) ? juce::jlimit (lo, hi, v) : def;
        }
    };

    static Param get (juce::AudioProcessorValueTreeState& state, const juce::String& id);

    Param length, tune, keytrack, velSens, choke,
          bodyLevel, bodyShape, otLevel, otRatio, modes,
          noiseLevel, noiseHP, noiseLP, noiseColor, noiseCouple, noiseWidth, noiseGrit,
          clickType, clickLevel, clickTone, clickLength,
          driveOn, driveType, driveAmount, driveMix,
          eqOn, eqStyle,
          outGain, clip,
          revOn, revMix, revSize, revDecay, revPreDelay, revLowCut, revHighCut, revGate, revGateTime, revGateFade,
          revEngine, revDiffusion, revModDepth, revModRate, revWidth, revEarly, revDamp,
          scTrigger, scThreshold, scRetrigger,
          bodyOn, bodyStyle, bodySmpTune, noiseOn, noiseStyle, noiseSmpTune, clickOn, clickSmpTune,
          outHP, outHPSlope, outLP, outLPSlope,
          bodyOffset, noiseOffset, clickOffset, bodySolo, noiseSolo, clickSolo, clipDrive, clipCeiling,
          bodyInvert, noiseInvert, clickInvert;

    struct EqBandParams { Param on, type, freq, gain, q, slope, dyn, thresh, attack, release; };
    std::array<EqBandParams, kNumEqBands> eqBands;

    struct DriveParams { Param on, type, amount, mix; };
    DriveParams drive2;
    struct CompParams { Param on, thresh, ratio, attack, release, knee, makeup, mix, style; };
    std::array<CompParams, 2> comps;
    struct OutCompParams { Param thresh, ratio, attack, release, makeup, style; };
    OutCompParams outComp;
    struct ChainClipParams { Param on, shape, drive, ceiling; };
    std::array<ChainClipParams, 2> chainClips;

    struct LayerParams { Param on, source, level, tune, env, delay, hp, lp, pan, noiseStyle, clickType, color, tone, length, solo, invert, bodyStyle, pitchEnv; };
    std::array<LayerParams, kNumExtraLayers> layers;

    struct LfoParams { Param shape, rate, sync, mode, phase; std::array<Param, kLfoSlots> dest, amount; };
    std::array<LfoParams, kNumLfos> lfos;
    std::vector<Param> targets; // modTargetIds() order
};

/** Every parameter ID, in layout order. Handy for attaching listeners. */
const juce::StringArray& allParameterIds();

/** Parameters that belong to the session (routing / performance), not to the
    patch: host recall restores them, but loading a preset leaves them alone
    and user preset files don't store them. */
const juce::StringArray& sessionOnlyParameterIds();

/** Name for a MIDI note number, e.g. 61 -> "C#3" (MIDI 60 = C3, as in Ableton). */
juce::String noteName (float midiNote);

/** Fractional MIDI note number for a frequency. */
float frequencyToMidi (float hz) noexcept;

} // namespace drum
