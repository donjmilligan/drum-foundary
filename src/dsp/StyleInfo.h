// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#pragma once

#include "Envelope.h"

namespace drum
{

/** Indices into bodyStyleNames (Parameters.h). */
enum BodyStyle
{
    bodyClassic = 0,
    bodyMembrane,
    body808,
    body909,
    bodyKick,
    bodyFM,
    bodyRound,
    bodyTaper,
    bodyHard,
    bodyFold,
    bodyTonal,
    bodySine,       // the basic waveforms: neither kick nor snare
    bodySaw,
    bodySquare,
    bodyRoundSquare,
    bodyTriangle,
    bodyWood,       // a snare head, after the waveforms so older patches keep their indices
    bodySample,
    numBodyStyles
};

/** Indices into noiseStyleNames. */
enum NoiseStyle
{
    noiseWires = 0,
    noiseWhite,
    noisePink,
    noiseMetallic,
    noiseCrackle,
    noiseSample,
    numNoiseStyles
};

/** Indices into clickTypeNames. */
enum ClickType
{
    clickTick = 0,
    clickSnap,
    clickStick,
    clickRim,
    clickPop,
    clickBeater,
    clickClap,
    clickFelt,
    clickPlastic,
    clickSkin,
    clickWood,
    clickTok,
    clickSample,
    numClickTypes
};

/** Indices into layerSourceNames: what an extra layer plays. */
enum LayerSource
{
    layerSample = 0,
    layerSine,
    layerNoise,
    layerClick,
    layerBody,
    numLayerSources
};

/** Indices into layerEnvNames: where an extra layer's pitch or level curve comes from. */
enum LayerEnv
{
    layerEnvBody = 0,  // coupled to the Body's curve
    layerEnvNoise,     // coupled to the Noise layer's
    layerEnvClick,     // coupled to the Click layer's
    layerEnvFlat,      // none: full level, or no pitch movement
    layerEnvOwn        // the layer's own curve
};

// An extra Noise layer's style is an index into layerNoiseStyleNames, which is
// noiseStyleNames without Sample, so it takes the same NoiseStyle values
// (noiseWires .. noiseCrackle). Likewise a Click layer's type is a ClickType
// from clickTick to clickTok, and a Body layer's style a BodyStyle from
// bodyClassic to bodyTriangle. Sample is always the last entry of the core
// lists (see sampleStyleIndex() in Parameters.h), so always use the enum. A Body
// layer can also be Wood (bodyWood), the entry after Triangle.

/*  What each knob does per style. The UI relabels the knobs with these captions,
    and dims a knob whose caption is nullptr (it has no effect in that style).

    Body (the drawn pitch curve, in semitones around Tune, applies to every synthesised style; the
    frequency stays between 0 Hz (the oscillators hold still; every style stays stable down there) and
    0.45 x the sample rate, partials above that are muted, and Classic's Shape and the 909's Drive
    fade to a sine above ~1.5 kHz so nothing aliases):
      Classic   Shape = sine -> triangle -> square, Overtone = level of a sine at OT Ratio (decays at 0.3 x Length),
                Head Modes = six inharmonic membrane partials.
      Wood      A wooden shell and head: a resonator at the fundamental and eight damped wood modes (at
                x1.61 .. x5.79, measured from wood snares), rung by a stick strike plus a little grain.
                Shape = Hardness: a shorter strike (2 ms .. 0.1 ms) that rings the upper modes more.
                Overtone = Knock: the wood modes' level (0 dB = eight times the fundamental). OT Ratio = Spread:
                where the main knock mode sits (x2.54 as measured), every mode stretched along. Head Modes =
                Damping: from ringing (0) to a dead, dry knock (100%), the fundamental and modes together.
      Membrane  Shape = Strike: stick hardness. Brightens the upper modes and sets the head's own fast pitch drop
                (tension glide, ~5 ms, scaled by velocity). Overtone = level of the (1,1) mode. OT Ratio = Spread:
                the (1,1) mode's ratio, with every other mode stretched along (x1.59 = an ideal membrane).
                Head Modes = level of 14 further Bessel modes; higher modes die faster.
      808       Shape = Punch: the trigger pulse, from a soft 2 ms push (0) to a sharp 0.15 ms click (1) that
                also leaks through. Overtone / OT Ratio = second resonator (808 snare: x1.6-1.9), which decays
                twice as fast. Head Modes unused.
      909       Shape = Drive: rounded -> hard triangle. Overtone / OT Ratio = second oscillator (909 snare: ~x1.7),
                decaying faster. Head Modes = Snap: a built-in pitch drop of up to +1 octave falling in ~5 ms.
      Kick      Shape = Drive: saturation harmonics that fade out as the hit decays (clean sub tail).
                Overtone / OT Ratio = Knock: a short partial at OT Ratio. Head Modes unused.
      FM        Shape = Index (0..8), which falls with the body envelope. Overtone = modulator Feedback.
                OT Ratio = modulator ratio. Head Modes = Index Decay: makes the index fall faster than the body.
      The kick bodies (each pairs with a tail shape drawn in its factory preset):
      Round     Shape = Round: a sine soft-saturated into fat, rounded peaks (0 = pure sine). It doesn't fade
                out, so the whole tail stays full. Overtone = Warmth: an even 2nd harmonic that makes the wave
                fuller on one side, like tape or a tube. OT Ratio, Head Modes unused.
      Taper     Shape = Point: sine -> triangle -> narrow pointed peaks. Head Modes = Taper: how fast the point
                eases back to a clean sine (0 = stays pointed; 100% = within ~2% of Length). Overtone / OT Ratio
                = Knock, as on Kick.
      Hard      Shape = Drive: up to ~30 dB of tanh into a squared-off, sustained tail (hardstyle, gabber).
                Overtone / OT Ratio = Bark: a partial added before the clipper (EQ-before-distortion), which
                the clipper turns into the tail's dense harmonics. Head Modes = Crunch: a second clipping stage.
                The drive eases off as the pitch rises (the sweep's start), so the clipping stays alias-free.
      Fold      Shape = Fold: a sine wavefolder, up to five folds. Head Modes = Fold Decay: the folding fades
                out over the hit (0 = stays folded). Overtone / OT Ratio = Knock, as on Kick.
      Tonal     A sine plus harmonics 2-8 (a tonal / knocking techno body). Shape = Bright: how strong the upper
                harmonics are. Overtone = level of the harmonics. OT Ratio = Stretch: x2 = exact harmonics,
                lower squeezes them together and higher stretches them apart (inharmonic, metallic). Head Modes
                = Harm Decay: higher harmonics die away faster (0 = they last as long as the body).
      The basic waveforms follow the pitch and level curves like the rest; Overtone, OT Ratio and Head
      Modes do nothing. Saw and Square are band-limited (PolyBLEP); Round Square's and Triangle's
      shaping fades to a sine at high pitches, as Classic's does, so sweeps don't alias.
      Sine      A pure sine. Shape = Drive: soft saturation (0 = pure).
      Saw       A falling saw, starting from 0 mid-ramp. Shape = Drive, as Sine's.
      Square    Shape = Width: the pulse width, 50% (0) down to 10% (1). Fades in over 0.5 ms (it doesn't
                pass through 0, so it would otherwise start with a click).
      Round Sq  A square with rounded corners (a sine driven into tanh). Shape = Corner: rounder (0) to
                sharper (1).
      Triangle  Shape = Skew: a symmetric triangle (0) leaning towards a saw (1).
      Sample    Plays the body sample. The pitch curve is ignored (Sample Tune transposes).

    Noise (all run through Low Cut / High Cut, the noise envelope, and Width, except where noted.
    Color's darkening follows the envelope up to 0 dB; above that the noise is fully open. The Noise
    pitch curve transposes the layer: Low Cut and High Cut move together (x 2^(st/12), followed every
    16 samples), and so do Metallic's bank, Crackle's density and the sample's playback rate; Coupling
    and Crush don't move):
      Wires     Color = tail darkening, Coupling = body AM (wire rattle, from the body as it sounds,
                with its Offset and mute but not its solo), Grit = sparse crackle.
      White     Color, Coupling as Wires. Grit = Crush: sample-and-hold, from clean to grainy.
      Pink      as White, with a -3 dB/octave spectrum.
      Metallic  Six square waves at 808 cymbal ratios. Color = Pitch of the bank (keytracked), Grit = blend of
                white noise into the bank. Set Low Cut to ~6-8 kHz for hats.
      Crackle   Sparse dusty impulses. Color = tail darkening, Grit = Density (100 to 6400 per second).
      Sample    Plays the noise sample. Color darkens its tail, Coupling applies, Width and Grit are unused
                (the sample's own stereo is kept).

    Click (the Click pitch curve transposes it from Tone: every filter and oscillator frequency, and
    the sample's playback rate, x 2^(st/12), followed every 16 samples. The Click level curve
    multiplies the click's own decay):
      Tick..Pop Tone = filter / pitch, Length = decay (unchanged from Snare One).
      Beater    Tone = brightness of a kick beater's knock and low-passed click. Length = decay.
      Clap      3 or 4 band-passed noise bursts 8-12 ms apart (the spacing varies from hit to hit), then
                a diffuse tail. Tone = band-pass centre (~1-2 kHz for a clap). Length = Tail: the tail
                falls ~36 dB over 5 x Length (2.5 to 200 ms).
      Felt..Tok Modelled impacts (ImpactGen in NoiseClickGen.h): a contact with bounces and early
                reflections rings a bank of modes, plus a snap and a noise texture that darkens as it
                decays. Every hit varies a little. Tone = the main resonance (a higher Tone also hardens
                the contact); Length scales every decay, 8 ms = the material as measured (the low tail
                lasts several times Length). Felt = soft kick beater (knock ~0.9 kHz, thump), Plastic =
                hard beater (ting ~7 kHz), Skin = miked head top end, Wood = stick on rim, Tok = hardstyle tok.
      Sample    Tone = Low Cut on the sample (fully left = off). Length = maximum length, with a short fade.

    Every layer (Body, Noise, Click, extra layers):
      Invert    Flips the layer's polarity (its output only: Wires coupling still hears the body as it is).
                Read each block; the flip swings through zero over 2.5 ms.
      Offset    -100..+100 ms. + starts the layer later; - starts it early, already that far into its
                timeline, and loses the part before the hit (with a 0.5 ms fade-in at the cut). Read
                when the hit starts. The hit lasts Length + the largest positive Offset.
      Solo      With any layer soloed only the soloed ones sound; a muted layer stays silent.

    Extra layers (Level, Offset, Pan, Low Cut and High Cut apply to every source; Pan is equal-power
    and balances a stereo source rather than narrowing it). Each has its own pitch and level curve,
    and Pitch / Level pick the curve it follows (layerPitchEnvelope() / layerLevelEnvelope() below):
    Own (its own curve), Body, Noise or Click (coupled to that core layer's curve: editing it from
    either tab edits the same curve), or Flat (full level, or no pitch movement).
      Level     Scales the layer. A Click layer's own decay is its envelope; the curve multiplies it.
      Pitch     Semitones from the layer's base, ending on it: Tune x 2^(Tune/12) for Sample, Sine
                and Body, the band's centre (the geometric mean of Low and High Cut) for Noise, and
                Tone x 2^(Tune/12) for Click. It transposes the whole layer: the sample's playback, the sine, the
                body, the noise generator (filters, Metallic) or the click's tone. Coupled to Body,
                it follows the Body pitch curve (semitones over Tune) as a transposition.
    Sessions saved before the extra layers had their own curves had one Envelope choice for both,
    whose pitch depended on the source; they are mapped onto Pitch and Level when they load (see
    legacyLayerPitchEnvelope()).
      Sample    Tune = transpose (semitones, plus key tracking).
      Sine      Tune = semitones from the global Tune.
      Body      A second body in its own Body Style (Classic .. Tonal, no Sample), pitched at the global
                Tune x 2^(Tune/12) under its pitch curve, its level curve inside it as on the Body layer
                (the FM index follows it too). Color = the style's Shape knob (Shape, Strike, Punch, Drive,
                Drive, Index, Round, Point, Drive, Fold, Bright; layerShapeCaption()); what it does to the strike is read when the hit starts,
                as on the Body layer. Overtone, OT Ratio and Head Modes are fixed per style
                (layerBodyTone()), not shared with the Body layer. Mono.
      Noise     The Noise layer's generator in the chosen style, at the same level as the Noise layer.
                Color = as on the Noise layer: tail darkening, which follows the layer's Level curve (so it does
                nothing with Flat), or the bank's Pitch for Metallic. Tune transposes Metallic (and is unused
                by the other styles). Low / High Cut are the generator's own filters, as on the Noise layer.
                Fixed: Width 35%, Coupling 30% for Wires (0 for the others; it needs the Body on), Grit off,
                except Crackle's Density at 50% (800 impulses per second). Couple it to the Noise curves for wires.
      Click     The Click layer's generator in the chosen type. Tone and Length as on the Click layer, and
                Tune transposes Tone (Tone x 2^(Tune/12)). The click's own decay is its envelope; the Level
                curve multiplies it. Tone, Length and Tune are read when the hit starts.
*/

/** Caption for Body Shape, or nullptr when the knob has no effect. */
inline const char* bodyShapeCaption (int style) noexcept
{
    switch (style)
    {
        case bodyClassic:  return "Shape";
        case bodyMembrane: return "Strike";
        case body808:      return "Punch";
        case body909:      return "Drive";
        case bodyKick:     return "Drive";
        case bodyFM:       return "Index";
        case bodyRound:    return "Round";
        case bodyTaper:    return "Point";
        case bodyHard:     return "Drive";
        case bodyFold:     return "Fold";
        case bodyTonal:    return "Bright";
        case bodySine:     return "Drive";
        case bodySaw:      return "Drive";
        case bodySquare:   return "Width";
        case bodyRoundSquare: return "Corner";
        case bodyTriangle: return "Skew";
        case bodyWood:     return "Hardness";
        default:           return nullptr;
    }
}

/** Caption for Overtone Level, or nullptr. */
inline const char* bodyOvertoneCaption (int style) noexcept
{
    switch (style)
    {
        case bodyClassic:  return "Overtone";
        case bodyMembrane: return "Mode 1,1";
        case body808:      return "Tone 2";
        case body909:      return "Osc 2";
        case bodyKick:     return "Knock";
        case bodyFM:       return "Feedback";
        case bodyRound:    return "Warmth";
        case bodyTaper:    return "Knock";
        case bodyHard:     return "Bark";
        case bodyFold:     return "Knock";
        case bodyTonal:    return "Harmonics";
        case bodyWood:     return "Knock";
        default:           return nullptr;
    }
}

/** Caption for Overtone Ratio, or nullptr. */
inline const char* bodyRatioCaption (int style) noexcept
{
    switch (style)
    {
        case bodyClassic:  return "OT Ratio";
        case bodyMembrane: return "Spread";
        case body808:      return "Ratio 2";
        case body909:      return "Ratio 2";
        case bodyKick:     return "Knock Ratio";
        case bodyFM:       return "Mod Ratio";
        case bodyTaper:    return "Knock Ratio";
        case bodyHard:     return "Bark Ratio";
        case bodyFold:     return "Knock Ratio";
        case bodyTonal:    return "Stretch";
        case bodyWood:     return "Spread";
        default:           return nullptr;
    }
}

/** Caption for Head Modes, or nullptr. */
inline const char* bodyModesCaption (int style) noexcept
{
    switch (style)
    {
        case bodyClassic:  return "Head Modes";
        case bodyMembrane: return "Head Modes";
        case body909:      return "Snap";
        case bodyFM:       return "Index Decay";
        case bodyTaper:    return "Taper";
        case bodyHard:     return "Crunch";
        case bodyFold:     return "Fold Decay";
        case bodyTonal:    return "Harm Decay";
        case bodyWood:     return "Damping";
        default:           return nullptr;
    }
}

/** Caption for Noise Color, or nullptr. */
inline const char* noiseColorCaption (int style) noexcept
{
    return style == noiseMetallic ? "Pitch" : "Color";
}

/** Caption for Wire Coupling, or nullptr. Coupling needs the body layer on. */
inline const char* noiseCouplingCaption (int style) noexcept
{
    return style == noiseWires ? "Coupling" : "Body AM";
}

/** Caption for Noise Grit, or nullptr. */
inline const char* noiseGritCaption (int style) noexcept
{
    switch (style)
    {
        case noiseWires:    return "Grit";
        case noiseWhite:    return "Crush";
        case noisePink:     return "Crush";
        case noiseMetallic: return "Noise";
        case noiseCrackle:  return "Density";
        default:            return nullptr;
    }
}

/** Caption for Noise Width, or nullptr (Sample keeps its own stereo). */
inline const char* noiseWidthCaption (int style) noexcept
{
    return style == noiseSample ? nullptr : "Width";
}

/** Caption for Click Tone, or nullptr. */
inline const char* clickToneCaption (int type) noexcept
{
    return type == clickSample ? "Low Cut" : "Tone";
}

/** Caption for Click Length, or nullptr. */
inline const char* clickLengthCaption (int type) noexcept
{
    switch (type)
    {
        case clickClap:   return "Tail";
        case clickSample: return "Max Length";
        default:          return "Length";
    }
}

/** Caption for an extra layer's Tune, or nullptr where it has no effect. */
inline const char* layerTuneCaption (int source, int noiseStyle) noexcept
{
    if (source == layerNoise)
        return noiseStyle == noiseMetallic ? "Tune" : nullptr;
    return "Tune";
}

/** The drawn level curve (an EnvId) extra layer `layer` (0-based) follows, for its
    Level choice `link` (a LayerEnv), or -1 for none (full level; a Click layer then
    has its own decay alone). */
inline int layerLevelEnvelope (int layer, int link) noexcept
{
    switch (link)
    {
        case layerEnvBody:  return envBody;
        case layerEnvNoise: return envNoise;
        case layerEnvClick: return envClick;
        case layerEnvOwn:   return juce::isPositiveAndBelow (layer, kNumLayerEnvelopePairs) ? layerOwnLevelEnvelope (layer) : -1;
        default:            return -1;
    }
}

/** The pitch curve (an EnvId, in semitones) that transposes extra layer `layer`
    (0-based), for its Pitch choice `link` (a LayerEnv), or -1 for none. */
inline int layerPitchEnvelope (int layer, int link) noexcept
{
    switch (link)
    {
        case layerEnvBody:  return envPitch;
        case layerEnvNoise: return envNoisePitch;
        case layerEnvClick: return envClickPitch;
        case layerEnvOwn:   return juce::isPositiveAndBelow (layer, kNumLayerEnvelopePairs) ? layerOwnPitchEnvelope (layer) : -1;
        default:            return -1;
    }
}

/** Format 1-3 patches had one Envelope choice per extra layer (Body, Noise, Click or
    Flat), and which pitch curve it moved depended on the source. Returns the Pitch
    choice (a LayerEnv) that sounds the same. Their Envelope becomes the Level choice,
    except that a Click layer only ever followed the Click level curve. */
inline int legacyLayerPitchEnvelope (int source, int env) noexcept
{
    const int transpose = env == layerEnvNoise || env == layerEnvClick ? env : -1;
    switch (source)
    {
        case layerSine:
        case layerBody:   return transpose >= 0 ? transpose : (int) layerEnvBody;
        case layerSample:
        case layerNoise:  return transpose >= 0 ? transpose : (int) layerEnvFlat;
        case layerClick:  return env == layerEnvClick ? (int) layerEnvClick : (int) layerEnvFlat;
        default:          return layerEnvFlat;
    }
}

inline int legacyLayerLevelEnvelope (int source, int env) noexcept
{
    if (source == layerClick)
        return env == layerEnvClick ? (int) layerEnvClick : (int) layerEnvFlat;
    return juce::jlimit ((int) layerEnvBody, (int) layerEnvFlat, env);
}

/** What an extra Body layer uses for the knobs it doesn't have: Overtone (linear
    gain), OT Ratio and Head Modes, per style. Middle-of-the-road settings for each
    style, in the ranges of its factory presets. */
struct LayerBodyTone
{
    float overtone, ratio, modes;
};

inline LayerBodyTone layerBodyTone (int style) noexcept
{
    switch (style)
    {
        case bodyMembrane: return { 0.3f,  1.593f, 0.35f }; // (1,1) mode -10 dB, ideal membrane spread
        case body808:      return { 0.5f,  1.8f,   0.0f };  // Tone 2 at x1.8, -6 dB
        case body909:      return { 0.35f, 1.7f,   0.15f }; // Osc 2 at x1.7, -9 dB; a little Snap
        case bodyKick:     return { 0.25f, 2.6f,   0.0f };  // Knock at x2.6, -12 dB
        case bodyFM:       return { 0.2f,  1.41f,  0.6f };  // Feedback 0.2, Mod Ratio x1.41, Index Decay 60%
        case bodyRound:    return { 0.18f, 2.0f,   0.0f };  // Warmth -15 dB
        case bodyTaper:    return { 0.25f, 2.6f,   0.5f };  // Knock at x2.6, -12 dB; Taper 50%
        case bodyHard:     return { 0.4f,  3.0f,   0.4f };  // Bark at x3, -8 dB; Crunch 40%
        case bodyFold:     return { 0.25f, 2.6f,   0.5f };  // Knock at x2.6, -12 dB; Fold Decay 50%
        case bodyTonal:    return { 0.5f,  2.0f,   0.5f };  // Harmonics -6 dB, exact (x2), Harm Decay 50%
        case bodyWood:     return { 0.5f,  2.54f,  0.5f };  // Knock -6 dB, as measured, Damping 50%
        case bodyClassic:
        default:           return { 0.3f,  1.594f, 0.25f }; // the Body layer's defaults
    }
}

/** True when the layer's Body Style menu applies, and its Color knob is the
    style's Shape (see layerShapeCaption()). */
inline bool layerUsesBodyControls (int source) noexcept { return source == layerBody; }

/** Caption for an extra layer's Color knob when it is a Body layer's Shape, or
    nullptr for other sources (see layerColorCaption()). */
inline const char* layerShapeCaption (int source, int bodyStyle) noexcept
{
    return source == layerBody ? bodyShapeCaption (bodyStyle) : nullptr;
}

/** True when the layer plays its imported sample (and shows its sample strip). */
inline bool layerUsesSample (int source) noexcept { return source == layerSample; }

/** True when the layer's Noise Style menu and Color knob apply. */
inline bool layerUsesColor (int source) noexcept { return source == layerNoise; }

/** True when the layer's Click Type menu and its Tone / Length knobs apply. */
inline bool layerUsesClickControls (int source) noexcept { return source == layerClick; }

/** Caption for an extra Noise layer's Color, or nullptr. `env` is the layer's Level choice:
    with Flat, Color's tail darkening has nothing to follow (Metallic's Pitch still works).
    A Body layer's Color is its Shape: see layerShapeCaption(). */
inline const char* layerColorCaption (int source, int noiseStyle, int env = layerEnvOwn) noexcept
{
    if (source != layerNoise)
        return nullptr;
    if (noiseStyle == noiseMetallic)
        return "Pitch";
    return env == layerEnvFlat ? nullptr : "Color";
}

/** Caption for an extra layer's Click Tone, or nullptr. */
inline const char* layerClickToneCaption (int source, int type) noexcept
{
    if (source != layerClick)
        return nullptr;
    switch (type)
    {
        case clickStick:
        case clickRim:
        case clickPop:  return "Pitch";
        default:        return "Tone";
    }
}

/** Caption for an extra layer's Click Length, or nullptr. */
inline const char* layerClickLengthCaption (int source, int type) noexcept
{
    if (source != layerClick)
        return nullptr;
    return type == clickClap ? "Tail" : "Length";
}

/** True for the styles that play an imported sample (and so use their Sample Tune). */
inline bool bodyUsesSample (int style) noexcept  { return style == bodySample; }
inline bool noiseUsesSample (int style) noexcept { return style == noiseSample; }
inline bool clickUsesSample (int type) noexcept  { return type == clickSample; }

} // namespace drum
