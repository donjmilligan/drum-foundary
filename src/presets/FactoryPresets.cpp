// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "FactoryPresets.h"
#include "../Parameters.h"
#include "../dsp/StyleInfo.h"

namespace drum
{

/*  Snares: the pitch curve (semitones above Tune) lands on Tune 10-20 ms after the
    hit and stays there, so the pitch you hear is the pitch you set: 200 Hz for most,
    190-220 Hz for the DnB and FG sets. The body is gone (about -20 dB by 50-100 ms)
    before the wires fade, as in real snares; a body that outlasts the wires reads as
    a tom. The body is also the loudest peak in the spectrum, so the analyzer's PEAK
    reads Tune (about 185-230 Hz, G2-A#2) in HIT and LIVE: clicks, rim tones,
    overtones and wire noise sit at least ~6 dB under it (check with
    `DrumFoundryTools measure-presets snare`).

    Kicks: Tune is the settled tail pitch (about 50 Hz). A pitch curve starting around two
    octaves (+24 st) above Tune that falls to Tune by ~200-250 ms gives roughly 140 / 100 /
    75 / 55 Hz at 10 / 20 / 40 / 80 ms, and the body outlasts everything else. The
    analyzer's PEAK reads the body a little above Tune (about +1 to +2 st), since
    the first 200 ms still carry some of the sweep.

    Extra layers have no pid constants, so they use their string ids ("lay1On"). */
const std::vector<FactoryPreset>& factoryPresets()
{
    using namespace pid;

    static const std::vector<FactoryPreset> presets {
        { "Init", {}, {}, {}, {} },

        // A plain kick to start from: Kick body, Beater, no wires.
        { "Init Kick",
          { { length, 500 }, { tune, 50 }, { bodyStyle, bodyKick }, { bodyShape, 0.1f }, { otLevel, kMinLevelDb },
            { noiseOn, 0 }, { clickType, clickBeater }, { clickLevel, -12 },
            { clickTone, 3000 }, { clickLength, 6 }, { driveAmount, 0 }, { outGain, 0.5f } },
          { { 0, 24, -1 }, { 0.4f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.3f }, { 0.28f, 0.1f, -0.4f }, { 0.46f, 0.01f, -0.3f }, { 0.7f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1f, 0.45f, -0.35f }, { 0.6f, 0.05f, -0.2f }, { 1, 0, 0 } } },

        { "Tight Studio",
          { { length, 380 }, { tune, 200 }, { bodyStyle, bodyMembrane }, { bodyLevel, -2 }, { bodyShape, 0.5f },
            { otLevel, -10 }, { modes, 0.4f }, { noiseLevel, -3.35f }, { noiseHP, 600 },
            { noiseLP, 11000 }, { noiseColor, 0.35f }, { noiseCouple, 0.35f }, { noiseWidth, 0.3f },
            { clickType, clickSnap }, { clickLevel, -9 }, { clickTone, 4200 }, { clickLength, 5 }, { driveType, 2 },
            { driveAmount, 0.2f }, { eqHigh, 2 }, { outGain, -3.5f } },
          { { 0, 5, -0.75f }, { 0.0263f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.6f }, { 0.1053f, 0.1f, -0.3f }, { 0.2632f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.55f }, { 0.0789f, 0.5f, -0.4f }, { 0.3421f, 0.05f, -0.2f }, { 0.4474f, 0, 0 }, { 1, 0, 0 } } },

        { "Fat Backbeat",
          { { length, 620 }, { tune, 200 }, { bodyShape, 0.35f }, { modes, 0.3f }, { noiseLevel, -3.73f },
            { noiseHP, 350 }, { noiseLP, 8500 }, { noiseColor, 0.5f }, { noiseCouple, 0.45f }, { noiseWidth, 0.4f },
            { clickLevel, -12 }, { clickTone, 3000 }, { clickLength, 7 }, { driveType, 2 }, { driveAmount, 0.35f },
            { eqLow, 2 }, { eqMidFreq, 2500 }, { eqMidGain, 1.5f }, { outGain, -2 } },
          { { 0, 6, -0.7f }, { 0.0161f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 0.9669f, 0.2807f }, { 0.0714f, 0.1273f, -0.3f }, { 0.2258f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.4f }, { 0.0823f, 0.5f, -0.35f }, { 0.3855f, 0.06f, -0.2f }, { 0.4855f, 0, 0 }, { 1, 0, 0 } } },

        { "808 Snare",
          { { length, 350 }, { tune, 200 }, { bodyStyle, body808 }, { bodyShape, 0.3f }, { otLevel, -6 },
            { otRatio, 1.8f }, { noiseLevel, -6.07f }, { noiseHP, 1800 }, { noiseLP, 14000 }, { noiseColor, 0.2f },
            { noiseCouple, 0 }, { noiseWidth, 0.15f }, { clickOn, 0 }, { driveAmount, 0.05f }, { outGain, -1.5f } },
          { { 0, 21.682f, -0.6f }, { 0.0308f, 1.12f, 0 }, { 1, 0, 0 } },
          { { 0, 1, 0.1466f }, { 0.1714f, 0.1f, -0.3f }, { 0.4286f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.35f }, { 0.3857f, 0.02f, -0.1f }, { 0.4857f, 0, 0 }, { 1, 0, 0 } } },

        { "909 Snare",
          { { length, 420 }, { tune, 200 }, { bodyStyle, body909 }, { bodyShape, 0.3f }, { otLevel, -6 },
            { otRatio, 1.7f }, { modes, 0.15f }, { noiseLevel, -2.22f }, { noiseHP, 700 }, { noiseLP, 13000 },
            { noiseColor, 0.45f }, { noiseCouple, 0.1f }, { noiseWidth, 0.25f }, { noiseGrit, 0.08f },
            { clickType, clickTick }, { clickLevel, -14 }, { clickTone, 6000 }, { clickLength, 2 },
            { driveAmount, 0.3f }, { eqHigh, 1.5f }, { outGain, -1.5f } },
          { { 0, 4, -0.8f }, { 0.0238f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, 0.1679f }, { 0.119f, 0.1f, -0.3f }, { 0.2857f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.0548f, 0.55f, -0.3f }, { 0.331f, 0.06f, -0.25f }, { 0.431f, 0, 0 }, { 1, 0, 0 } } },

        { "Trap Crack",
          { { length, 400 }, { tune, 200 }, { bodyLevel, -1.5f }, { bodyShape, 0.2f }, { otLevel, -12 },
            { modes, 0.2f }, { noiseLevel, -1.25f }, { noiseHP, 1500 }, { noiseLP, 16000 },
            { noiseColor, 0.25f }, { noiseCouple, 0.2f }, { noiseWidth, 0.5f }, { noiseGrit, 0.05f },
            { clickType, clickSnap }, { clickLevel, -6 }, { clickTone, 5000 }, { clickLength, 4 }, { driveType, 1 },
            { driveAmount, 0.45f }, { driveMix, 0.8f }, { eqLow, -2 }, { eqHigh, 3 }, { outGain, -6 } },
          { { 0, 12, -0.85f }, { 0.025f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.7f }, { 0.1125f, 0.1f, -0.3f }, { 0.275f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.6f }, { 0.045f, 0.6f, -0.3f }, { 0.2475f, 0.08f, -0.3f }, { 0.3475f, 0, 0 }, { 1, 0, 0 } } },

        { "Lo-Fi Dust",
          { { length, 480 }, { tune, 200 }, { bodyLevel, -6.5f }, { bodyShape, 0.3f }, { otLevel, -12 },
            { modes, 0.15f }, { noiseLevel, -9.13f }, { noiseHP, 500 }, { noiseLP, 7500 },
            { noiseColor, 0.45f }, { noiseCouple, 0.3f }, { noiseWidth, 0.2f }, { noiseGrit, 0.55f },
            { clickType, clickTick }, { clickLevel, -12 }, { clickTone, 2500 }, { clickLength, 3 }, { driveType, 4 },
            { driveAmount, 0.45f }, { driveMix, 0.7f }, { eqMidFreq, 1200 }, { eqMidGain, 3 }, { eqHigh, -1.5f },
            { outGain, -2.5f } },
          { { 0, 8, -0.7f }, { 0.0208f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.0833f, 0.1f, -0.3f }, { 0.2292f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.45f }, { 0.125f, 0.4f, -0.3f }, { 0.4125f, 0.04f, -0.2f }, { 0.5125f, 0, 0 }, { 1, 0, 0 } } },

        { "Rimshot",
          { { length, 300 }, { tune, 200 }, { bodyStyle, bodyMembrane }, { bodyLevel, -3 }, { bodyShape, 0.65f },
            { otLevel, -4 }, { modes, 0.6f }, { noiseLevel, -8.52f }, { noiseHP, 1200 },
            { noiseLP, 10000 }, { noiseColor, 0.5f }, { noiseCouple, 0.2f }, { clickType, clickRim },
            { clickLevel, -3 }, { clickTone, 1700 }, { clickLength, 8 }, { driveAmount, 0.25f },
            { eqMidFreq, 1800 }, { eqMidGain, 2 }, { outGain, 0 } },
          { { 0, 2, -0.8f }, { 0.0333f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.7f }, { 0.0667f, 0.1f, -0.3f }, { 0.2f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.6f }, { 0.1667f, 0.3f, -0.3f }, { 0.4667f, 0, 0 }, { 1, 0, 0 } } },

        { "Piccolo",
          { { length, 300 }, { tune, 200 }, { bodyStyle, bodyMembrane }, { bodyShape, 0.75f }, { otLevel, -6 },
            { modes, 0.5f }, { noiseLevel, -5.35f }, { noiseHP, 1400 }, { noiseLP, 14000 },
            { noiseColor, 0.3f }, { noiseCouple, 0.5f }, { noiseWidth, 0.3f }, { clickType, clickStick },
            { clickLevel, -10 }, { clickTone, 4500 }, { clickLength, 3 }, { driveAmount, 0.15f }, { eqHigh, 3 },
            { outGain, -4 } },
          { { 0, 5, -0.75f }, { 0.0333f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.6f }, { 0.1333f, 0.1f, -0.3f }, { 0.3333f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.0833f, 0.45f, -0.35f }, { 0.3333f, 0.05f, -0.2f }, { 0.45f, 0, 0 }, { 1, 0, 0 } } },

        { "Deep Wood",
          { { length, 600 }, { tune, 200 }, { bodyStyle, bodyMembrane }, { bodyLevel, -7 }, { bodyShape, 0.2f },
            { otLevel, -12 }, { modes, 0.3f }, { noiseLevel, -8.35f }, { noiseHP, 300 }, { noiseLP, 6500 },
            { noiseColor, 0.55f }, { noiseCouple, 0.55f }, { noiseWidth, 0.3f }, { clickType, clickStick },
            { clickLevel, -14 }, { clickTone, 1600 }, { clickLength, 8 }, { driveType, 2 }, { driveAmount, 0.3f },
            { eqLow, 1.5f }, { eqHigh, -2 } },
          { { 0, 6, -0.7f }, { 0.0167f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.4f }, { 0.0833f, 0.1f, -0.3f }, { 0.2167f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.45f }, { 0.1037f, 0.1844f, -0.35f }, { 0.4333f, 0.06f, -0.2f }, { 0.55f, 0, 0 }, { 1, 0, 0 } } },

        { "Brush Swish",
          { { length, 550 }, { tune, 200 }, { bodyLevel, -4 }, { bodyShape, 0 }, { otLevel, kMinLevelDb }, { modes, 0 },
            { noiseLevel, -6.88f }, { noiseHP, 900 }, { noiseLP, 7500 }, { noiseColor, 0.5f }, { noiseCouple, 0 },
            { noiseWidth, 0.7f }, { noiseGrit, 0.15f }, { clickOn, 0 }, { driveAmount, 0 }, { outGain, -1.5f } },
          { { 0, 2, -0.6f }, { 0.0182f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.0819f }, { 0.0909f, 0.1f, -0.3f }, { 0.2182f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 0.1f, -0.4f }, { 0.06f, 1, -0.3f }, { 0.3f, 0.35f, -0.2f }, { 1, 0, 0 } } },

        { "Gated '85",
          { { length, 520 }, { tune, 200 }, { otLevel, -8 }, { modes, 0.3f }, { noiseLevel, -3.76f },
            { noiseHP, 400 }, { noiseLP, 9000 }, { noiseColor, 0.15f }, { noiseCouple, 0.4f }, { noiseWidth, 0.8f },
            { clickType, clickSnap }, { clickLevel, -10 }, { clickTone, 3000 }, { driveType, 2 },
            { driveAmount, 0.35f }, { eqLow, 1 }, { eqHigh, 2 }, { outGain, -3 } },
          { { 0, 9, -0.7f }, { 0.0192f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1154f, 0.1f, -0.3f }, { 0.2885f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.6f }, { 0.06f, 0.55f, -0.2f }, { 0.8f, 0.4f, 0 }, { 0.86f, 0, 0 }, { 1, 0, 0 } } },

        { "Metal Fold",
          { { length, 450 }, { tune, 200 }, { bodyShape, 0.6f }, { otRatio, 2.4f },
            { otLevel, -6 }, { modes, 0.5f }, { noiseLevel, -4.52f }, { noiseHP, 800 }, { noiseLP, 12000 },
            { noiseColor, 0.4f }, { noiseCouple, 0.6f }, { clickType, clickPop }, { clickLevel, -8 },
            { clickTone, 2500 }, { clickLength, 10 }, { driveType, 3 }, { driveAmount, 0.5f }, { driveMix, 0.7f },
            { eqMidFreq, 3000 }, { eqMidGain, -3 }, { outGain, -1.5f } },
          { { 0, 12, -0.8f }, { 0.0222f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.55f }, { 0.1222f, 0.1f, -0.3f }, { 0.2889f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.0556f, 0.45f, -0.35f }, { 0.3311f, 0.05f, -0.2f }, { 0.4311f, 0, 0 }, { 1, 0, 0 } } },

        // Tight DnB snare with the body hit: a saturated Sine body at 237 Hz with a deep, slow pitch drop
        // into it, bright wires, and a short gated room behind it.
        { "DnB Gate Room",
          { { length, 300 }, { tune, 237 }, { bodyStyle, bodySine }, { bodyLevel, -1 }, { bodyShape, 0.45f },
            { otLevel, kMinLevelDb }, { modes, 0 }, { noiseLevel, -4.5f }, { noiseHP, 800 }, { noiseLP, 14000 },
            { noiseColor, 0.15f }, { noiseCouple, 0.35f }, { noiseWidth, 0.4f }, { clickLevel, -6 }, { clickLength, 4 },
            { driveType, 2 }, { driveAmount, 0.3f }, { eqHigh, 1.5f }, { outGain, -2 }, { revMix, 0.69f },
            { revSize, 0.35f }, { revDecay, 450 }, { revPreDelay, 12 }, { revLowCut, 220 }, { revHighCut, 9000 },
            { revGateTime, 130 } },
          { { 0, 23, -0.4f }, { 0.11f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 0.6f, -0.4f }, { 0.03f, 1, 0.6701f }, { 0.13f, 0.25f, -0.3f }, { 0.25f, 0.03f, 0 }, { 0.35f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.6f }, { 0.1f, 0.45f, -0.35f }, { 0.55f, 0.05f, -0.25f }, { 1, 0, 0 } } },

        // Modern neuro / roller snare with the body hit: a saturated Sine body at 234 Hz (about half the
        // energy is the body), a snap, bright wires and a short room.
        { "DnB Snare",
          { { length, 300 }, { tune, 198 }, { bodyStyle, bodyRoundSquare }, { bodyLevel, 0.4054f }, { bodyShape, 0.5f },
            { otLevel, kMinLevelDb }, { modes, 0 }, { noiseLevel, -2 }, { noiseHP, 20 }, { noiseLP, 1000 },
            { noiseColor, 0.3f }, { noiseCouple, 0.35f }, { noiseGrit, 0.05f }, { clickType, clickClap },
            { clickOffset, -29.58f }, { clickLevel, -4 }, { clickTone, 5000 }, { clickLength, 3 }, { driveType, 2 },
            { driveAmount, 0.35f }, { eqMidFreq, 3870 }, { eqMidGain, -2.437f }, { "eq2Q", 4.864f }, { eqHigh, 1 },
            { outHP, 90 }, { outGain, -5.5f }, { revMix, 0.29f }, { revSize, 0.3f }, { revDecay, 400 },
            { revPreDelay, 8 }, { revLowCut, 277.6f }, { revHighCut, 6905 }, { revGateTime, 126.8f },
            { revGateFade, 24.05f } },
          { { 0, 36.5344f, -0.55f }, { 0.0131f, 0.4327f, 0 }, { 1, 0, 0 } },
          { { 0, 0.8608f, 0.4281f }, { 0.0449f, 1.0203f, -0.1023f }, { 0.1264f, 0.3242f, -0.3f }, { 0.2196f, 0, 0 }, { 0.33f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.6f }, { 0.07f, 0.5f, -0.35f }, { 0.35f, 0.05f, -0.2f }, { 0.45f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 53.6198f, 0.4452f }, { 0.532f, -3.6452f, 0 }, { 1, 0, 0 } } },

        // Hardstep / jump-up: a hard-driven Sine body at 225 Hz pushed into the clipper, a big snap, dense
        // wires doubled by a white-noise layer, Hard drive and a short gated room. Still over half body.
        { "DnB Hard Style",
          { { length, 340 }, { tune, 225 }, { velSens, 0.5835f }, { bodyStyle, bodySine }, { bodyLevel, 4.466f },
            { bodyShape, 0.8f }, { otLevel, kMinLevelDb }, { otRatio, 1.7f }, { modes, 0 }, { noiseLevel, -5 },
            { noiseHP, 600 }, { noiseLP, 15000 }, { noiseColor, 0.25f }, { noiseCouple, 0.45f }, { noiseWidth, 0.5f },
            { noiseGrit, 0.12f }, { clickLevel, -3 }, { clickTone, 4000 }, { clickLength, 5 }, { driveType, 1 },
            { driveAmount, 0.28f }, { driveMix, 0.7354f }, { eqMidFreq, 2500 }, { eqMidGain, 2 }, { eqHigh, 3 },
            { outHP, 100 }, { outGain, -9 }, { revMix, 0.41f }, { revSize, 0.4f }, { revDecay, 600 },
            { revPreDelay, 6 }, { revLowCut, 300 }, { revHighCut, 11000 }, { revGateTime, 160 }, { revGateFade, 20 },
            { "lay1On", 1 }, { "lay1Source", 2 }, { "lay1Level", -12 }, { "lay1Env", 1 }, { "lay1HP", 2500 },
            { "lay1LP", 12000 }, { "lay1Color", 0.3f }, { "lay1PitchEnv", 1 } },
          { { 0, 14, -0.6f }, { 0.07f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1.6f, 0.1f }, { 0.12f, 0.35f, -0.3f }, { 0.24f, 0.05f, 0 }, { 0.33f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1.4f, -0.6f }, { 0.07f, 0.6f, -0.3f }, { 0.35f, 0.07f, -0.25f }, { 0.46f, 0, 0 }, { 1, 0, 0 } } },

        // Liquid: a round Sine body at 246 Hz with a big, quick drop and most of the energy in the body,
        // soft airy wires, a gentle stick and a small plate-like room.
        { "DnB Liquid",
          { { length, 420 }, { tune, 197.7f }, { bodyStyle, bodySine }, { bodyLevel, -1 }, { bodyShape, 0.3f },
            { otLevel, kMinLevelDb }, { modes, 0 }, { noiseLevel, -8.5f }, { noiseHP, 600 }, { noiseLP, 14000 },
            { noiseColor, 0.5f }, { noiseCouple, 0.5f }, { noiseWidth, 0.6f }, { clickType, clickStick },
            { clickTone, 3000 }, { clickLength, 3 }, { driveType, 2 }, { driveAmount, 0.15f }, { eqHigh, 1 },
            { outGain, -4.5f }, { revMix, 0.28f }, { revSize, 0.55f }, { revDecay, 1100 }, { revPreDelay, 15 },
            { revLowCut, 650 }, { revHighCut, 9000 }, { revGateTime, 138.2f }, { revGateFade, 55.67f } },
          { { 0, 20, -0.5f }, { 0.05f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 0.7f, -0.4f }, { 0.02f, 1, 0.2514f }, { 0.08f, 0.3f, -0.3f }, { 0.17f, 0.04f, 0 }, { 0.26f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1f, 0.5f, -0.35f }, { 0.4f, 0.06f, -0.2f }, { 0.52f, 0, 0 }, { 1, 0, 0 } } },

        // The 80s arena snare: a big hall cut short by the gate.
        { "Big 80s Gate",
          { { length, 240 }, { tune, 200 }, { bodyStyle, bodyMembrane }, { bodyShape, 0.4f }, { otLevel, -8 },
            { modes, 0.3f }, { noiseLevel, -3.85f }, { noiseHP, 400 }, { noiseColor, 0.3f }, { noiseCouple, 0.4f },
            { noiseWidth, 0.6f }, { clickTone, 3000 }, { driveType, 2 }, { driveAmount, 0.3f }, { eqLow, 1 },
            { eqHigh, 2 }, { outGain, -3 }, { revMix, 0.72f }, { revSize, 0.9f }, { revDecay, 2600 },
            { revPreDelay, 20 }, { revLowCut, 250 }, { revHighCut, 7500 }, { revGateTime, 180.34f },
            { revGateFade, 20 } },
          { { 0, 4, -0.7f }, { 0.0417f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.25f, 0.1f, -0.3f }, { 0.625f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.55f }, { 0.12f, 0.45f, -0.3f }, { 0.65f, 0.06f, -0.2f }, { 1, 0, 0 } } },

        // A miked wooden snare: Membrane head, stick, coupled wires and a small room.
        { "Acoustic Snare",
          { { length, 400 }, { tune, 200 }, { bodyStyle, bodyMembrane }, { bodyLevel, -1 }, { bodyShape, 0.55f },
            { otLevel, -9 }, { modes, 0.55f }, { noiseLevel, -3.52f }, { noiseHP, 400 }, { noiseLP, 10000 },
            { noiseColor, 0.45f }, { noiseCouple, 0.5f }, { noiseGrit, 0.03f }, { clickType, clickStick },
            { clickLevel, -12 }, { clickTone, 3000 }, { clickLength, 4 }, { driveType, 2 }, { driveAmount, 0.15f },
            { outGain, -2.5f }, { revMix, 0.21f }, { revSize, 0.25f }, { revDecay, 350 }, { revPreDelay, 5 },
            { revLowCut, 300 }, { revGate, 0 } },
          { { 0, 5, -0.75f }, { 0.025f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, 0.1834f }, { 0.1f, 0.1f, -0.3f }, { 0.275f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.0875f, 0.5f, -0.35f }, { 0.375f, 0.06f, -0.2f }, { 0.5f, 0, 0 }, { 1, 0, 0 } } },

        // Two-operator FM body with an inharmonic, fast-fading bite, over white noise.
        { "FM Snare",
          { { length, 360 }, { tune, 200 }, { bodyStyle, bodyFM }, { bodyLevel, -2 }, { bodyShape, 0.35f },
            { otLevel, -14 }, { otRatio, 3.6f }, { modes, 0.6f }, { noiseStyle, noiseWhite },
            { noiseLevel, -5.73f }, { noiseHP, 1500 }, { noiseLP, 14000 }, { noiseColor, 0.4f }, { noiseCouple, 0.2f },
            { noiseWidth, 0.4f }, { clickType, clickTick }, { clickLevel, -12 }, { clickTone, 7000 },
            { clickLength, 2 }, { driveAmount, 0.2f }, { eqHigh, 1 }, { outGain, -2 } },
          { { 0, 7, -0.8f }, { 0.0278f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.6f }, { 0.0833f, 0.1f, -0.3f }, { 0.2361f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.55f }, { 0.0694f, 0.55f, -0.35f }, { 0.3056f, 0.05f, -0.2f }, { 0.4167f, 0, 0 }, { 1, 0, 0 } } },

        // 909 body under a clap: white noise plus two noise layers 9 and 19 ms late, panned apart.
        { "Clap Snare",
          { { length, 380 }, { tune, 200 }, { bodyStyle, body909 }, { bodyLevel, -7 }, { bodyShape, 0.2f },
            { otRatio, 1.7f }, { modes, 0.12f }, { noiseStyle, noiseWhite }, { noiseLevel, -4.35f }, { noiseHP, 900 },
            { noiseColor, 0.3f }, { noiseCouple, 0.1f }, { noiseWidth, 0.3f }, { clickType, clickClap },
            { clickLevel, -5.6f }, { outGain, -0.5f }, { "lay1On", 1 }, { "lay1Source", 2 }, { "lay1Level", -15 },
            { "lay1Env", 1 }, { "lay1Delay", 9 }, { "lay1HP", 1000 }, { "lay1LP", 8000 }, { "lay1Pan", -0.35f },
            { "lay1Color", 0 }, { "lay1PitchEnv", 1 }, { "lay2On", 1 }, { "lay2Source", 2 }, { "lay2Level", -16 },
            { "lay2Env", 1 }, { "lay2Delay", 19 }, { "lay2HP", 1100 }, { "lay2LP", 7500 }, { "lay2Pan", 0.35f },
            { "lay2Color", 0 }, { "lay2PitchEnv", 1 } },
          { { 0, 4, -0.8f }, { 0.0263f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.6f }, { 0.1053f, 0.1f, -0.3f }, { 0.2632f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.7f }, { 0.0211f, 0.3f, -0.3f }, { 0.1579f, 0.1f, -0.2f }, { 0.4737f, 0, 0 }, { 1, 0, 0 } } },

        // Membrane body with metallic noise for a ringing steel shell.
        { "Steel Snare",
          { { length, 380 }, { tune, 200 }, { bodyStyle, bodyMembrane }, { bodyLevel, -2 }, { bodyShape, 0.7f },
            { otLevel, -6 }, { otRatio, 1.65f }, { modes, 0.6f }, { noiseStyle, noiseMetallic },
            { noiseLevel, -5 }, { noiseHP, 1800 }, { noiseLP, 14000 }, { noiseColor, 0.75f }, { noiseCouple, 0.3f },
            { noiseWidth, 0.4f }, { noiseGrit, 0.55f }, { clickType, clickStick }, { clickLevel, -10 },
            { clickTone, 4500 }, { clickLength, 3 }, { driveAmount, 0.2f }, { eqHigh, 1.5f }, { outGain, -2 } },
          { { 0, 3, -0.75f }, { 0.0263f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.6f }, { 0.1184f, 0.1f, -0.3f }, { 0.3026f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.0658f, 0.45f, -0.35f }, { 0.3158f, 0.05f, -0.2f }, { 0.4211f, 0, 0 }, { 1, 0, 0 } } },

        // A soft ghost note to sit under a main snare: gentle head, airy wires, barely any stick.
        { "Ghost Snare",
          { { length, 220 }, { tune, 200 }, { bodyStyle, bodyMembrane }, { bodyLevel, -2 }, { bodyShape, 0.3f },
            { otLevel, -12 }, { modes, 0.25f }, { noiseLevel, -5.25f }, { noiseHP, 1500 },
            { noiseLP, 12000 }, { noiseColor, 0.5f }, { noiseCouple, 0.4f }, { noiseWidth, 0.5f },
            { clickType, clickTick }, { clickLevel, -22 }, { clickTone, 5000 }, { clickLength, 2 },
            { driveAmount, 0 }, { outGain, -10.5f } },
          { { 0, 3, -0.75f }, { 0.0455f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.6f }, { 0.15f, 0.1f, -0.3f }, { 0.35f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1f, 0.55f, -0.35f }, { 0.6f, 0.08f, -0.2f }, { 0.8f, 0, 0 }, { 1, 0, 0 } } },

        // Future garage: a dusty wooden snare, more knock than boom. A Membrane head tuned up to 334 Hz
        // under a Wood click (a stick on the rim), so most of it sits at 400 Hz - 2 kHz, crackle for the
        // dust, a band of white noise (layer 1) for the wires, Crush drive, a dark room, band-limited.
        { "Future Garage Snare",
          { { length, 380 }, { tune, 334 }, { bodyStyle, bodyMembrane }, { bodyLevel, -9 }, { bodyShape, 0.6f },
            { otLevel, -8 },
            { modes, 0.6f }, { noiseStyle, noiseCrackle }, { noiseLevel, -10.25f }, { noiseHP, 700 }, { noiseLP, 9000 },
            { noiseColor, 0.5f }, { noiseCouple, 0.3f }, { noiseWidth, 0.5f }, { noiseGrit, 0.75f }, { clickType, clickWood },
            { clickLevel, 0 }, { clickTone, 900 }, { clickLength, 8 }, { driveType, 4 }, { driveAmount, 0.35f },
            { driveMix, 0.6f }, { eqHigh, -2 }, { outHP, 170 }, { outHPSlope, 0 }, { outLP, 4000 }, { outLPSlope, 1 },
            { outGain, -3 }, { revMix, 0.3f }, { revSize, 0.7f }, { revDecay, 1500 }, { revPreDelay, 18 }, { revLowCut, 350 },
            { revHighCut, 4500 }, { revGate, 0 }, { "lay1On", 1 }, { "lay1Source", 2 }, { "lay1Color", 0 }, { "lay1Level", -21 },
            { "lay1Env", 1 }, { "lay1PitchEnv", 1 }, { "lay1HP", 1500 }, { "lay1LP", 8000 } },
          { { 0, 5, -0.6f }, { 0.04f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.4f }, { 0.12f, 0.25f, -0.3f }, { 0.3f, 0.02f, 0 }, { 0.4f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.08f, 0.55f, -0.35f }, { 0.45f, 0.08f, -0.2f }, { 0.6f, 0, 0 }, { 1, 0, 0 } } },

        // FG Rim: a ghost rim hit. A quiet, high Membrane head under a loud Wood click, nothing but a little
        // crackle for wires, all of it between 400 Hz and 2 kHz and short, then Crush drive and a dark room.
        { "FG Rim",
          { { length, 300 }, { tune, 354 }, { bodyStyle, bodyMembrane }, { bodyLevel, -13 }, { bodyShape, 0.85f },
            { otLevel, -6 }, { modes, 0.8f }, { noiseStyle, noiseWhite }, { noiseLevel, -7.23f }, { noiseHP, 1200 },
            { noiseLP, 8000 }, { noiseColor, 0.5f }, { noiseCouple, 0.3f }, { noiseWidth, 0.4f }, { noiseGrit, 0.25f },
            { clickType, clickWood }, { clickLevel, 0 }, { clickTone, 950 }, { clickLength, 8 }, { driveType, 4 },
            { driveAmount, 0.45f }, { driveMix, 0.6f }, { eqHigh, -1 }, { outHP, 200 }, { outHPSlope, 0 }, { outLP, 2600 },
            { outLPSlope, 2 }, { outGain, -8 }, { revMix, 0.25f }, { revSize, 0.6f }, { revDecay, 1200 }, { revPreDelay, 15 },
            { revLowCut, 550 }, { revHighCut, 4000 }, { revGate, 0 }, { "lay1On", 1 }, { "lay1Source", 2 },
            { "lay1NoiseStyle", 4 }, { "lay1Color", 0.5f }, { "lay1Level", -20 }, { "lay1Env", 1 }, { "lay1PitchEnv", 1 },
            { "lay1HP", 1000 }, { "lay1LP", 4000 }, { noiseOn, 0 } },
          { { 0, 7, -0.7f }, { 0.035f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.6f }, { 0.07f, 0.15f, -0.3f }, { 0.2f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.6f }, { 0.1f, 0.5f, -0.35f }, { 0.45f, 0.06f, -0.2f }, { 0.6f, 0, 0 }, { 1, 0, 0 } } },

        // FG Ghost: the shuffled in-between notes, a soft rim. A Membrane head at 355 Hz, a gentle Wood
        // click, crackle dust with a little air, Tube drive and a dark wash of room.
        { "FG Ghost",
          { { length, 260 }, { tune, 355 }, { bodyStyle, bodyMembrane }, { bodyLevel, -3 }, { bodyShape, 0.6f },
            { otLevel, -8 }, { modes, 0.6f }, { noiseStyle, noiseCrackle }, { noiseLevel, -6 }, { noiseHP, 800 },
            { noiseLP, 12000 }, { noiseColor, 0.55f }, { noiseWidth, 0.6f }, { noiseGrit, 0.6f },
            { clickType, clickWood }, { clickLevel, -4 }, { clickTone, 1100 }, { clickLength, 7 }, { driveType, 2 },
            { driveAmount, 0.25f }, { driveMix, 0.7f }, { eqHigh, -2 }, { outHP, 220 }, { outHPSlope, 0 },
            { outLP, 12000 }, { outGain, -10 }, { revMix, 0.38f }, { revSize, 0.3815f }, { revDecay, 1300 },
            { revPreDelay, 20 }, { revLowCut, 500 }, { revHighCut, 3800 }, { revGateTime, 203.2f },
            { revGateFade, 200 }, { "lay1On", 1 }, { "lay1Source", 2 }, { "lay1Level", -18 }, { "lay1Env", 1 },
            { "lay1HP", 1800 }, { "lay1LP", 11000 }, { "lay1Color", 0 }, { "lay1PitchEnv", 1 } },
          { { 0, 5, -0.6f }, { 0.04f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, 0.154f }, { 0.1f, 0.2f, -0.3f }, { 0.28f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.09f, 0.5f, -0.35f }, { 0.45f, 0.06f, -0.2f }, { 0.62f, 0, 0 }, { 1, 0, 0 } } },

        // FG Wood: a dry wood knock, like a stick on a block. A barely-there head at 384 Hz under the Wood
        // click, no wires, crackle dust, and dark: almost all of it is the knock (400 Hz - 2 kHz).
        { "FG Wood",
          { { length, 240 }, { tune, 384 }, { bodyStyle, bodyMembrane }, { bodyLevel, -15 }, { bodyShape, 0.85f },
            { otLevel, -6 }, { otRatio, 1.594f }, { modes, 0.8f }, { noiseStyle, noiseWires }, { noiseLevel, -6.52f },
            { noiseHP, 1500 }, { noiseLP, 7000 }, { noiseColor, 0.55f }, { noiseCouple, 0.3f }, { noiseWidth, 0.35f },
            { noiseGrit, 0.3f }, { clickType, clickWood }, { clickLevel, 0 }, { clickTone, 1150 }, { clickLength, 6 },
            { driveType, 2 }, { driveAmount, 0.3f }, { eqHigh, -1.5f }, { outHP, 180 }, { outHPSlope, 0 }, { outLP, 2700 },
            { outLPSlope, 2 }, { outGain, -2 }, { revMix, 0.26f }, { revSize, 0.35f }, { revDecay, 700 }, { revPreDelay, 10 },
            { revLowCut, 550 }, { revHighCut, 4500 }, { revGate, 0 }, { "lay1On", 1 }, { "lay1Source", 2 },
            { "lay1NoiseStyle", 4 }, { "lay1Color", 0.5f }, { "lay1Level", -20 }, { "lay1Env", 1 }, { "lay1PitchEnv", 1 },
            { "lay1HP", 900 }, { "lay1LP", 4000 }, { noiseOn, 0 } },
          { { 0, 7, -0.7f }, { 0.035f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.6f }, { 0.06f, 0.12f, -0.3f }, { 0.16f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.6f }, { 0.1f, 0.5f, -0.35f }, { 0.45f, 0.06f, -0.2f }, { 0.62f, 0, 0 }, { 1, 0, 0 } } },

        // FG Knock: a high wooden knock (459 Hz) with a slow pitch fall, the Wood click, a little top and
        // crackle dust, Tube drive and a dark room.
        { "FG Knock",
          { { length, 320 }, { tune, 459 }, { bodyStyle, bodyMembrane }, { bodyLevel, -6 }, { bodyShape, 0.85f },
            { otLevel, -6 }, { modes, 0.8f }, { noiseLevel, -5.23f }, { noiseHP, 900 }, { noiseLP, 7000 }, { noiseColor, 0.6f },
            { noiseCouple, 0.5f }, { noiseWidth, 0.4f }, { noiseGrit, 0.2f }, { clickType, clickWood }, { clickLevel, 0 },
            { clickTone, 1400 }, { clickLength, 6 }, { driveType, 2 }, { driveAmount, 0.35f }, { eqLow, 1 }, { eqHigh, -2 },
            { outHP, 150 }, { outHPSlope, 0 }, { outLP, 4500 }, { outLPSlope, 2 }, { outGain, -4 }, { revMix, 0.25f },
            { revSize, 0.6f }, { revDecay, 1300 }, { revPreDelay, 20 }, { revLowCut, 500 }, { revHighCut, 4000 }, { revGate, 0 },
            { "lay1On", 1 }, { "lay1Source", 2 }, { "lay1NoiseStyle", 4 }, { "lay1Color", 0.5f }, { "lay1Level", -20 },
            { "lay1Env", 1 }, { "lay1PitchEnv", 1 }, { "lay1HP", 800 }, { "lay1LP", 4000 }, { noiseOn, 0 }, { otRatio, 1.594f } },
          { { 0, 12, -0.2f }, { 0.12f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.08f, 0.18f, -0.3f }, { 0.2f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1f, 0.55f, -0.35f }, { 0.5f, 0.06f, -0.2f }, { 0.65f, 0, 0 }, { 1, 0, 0 } } },

        // FG Wood Snare: a dry wooden snare, matched to a recorded one. The Wood body at 233 Hz drops 18 st
        // in ~20 ms; its wood modes (most of it, 400 Hz - 2 kHz, centred near 1 kHz) ring with the
        // fundamental for ~50 ms, under a Wood stick click and a thin band of wires. No room.
        { "FG Wood Snare",
          { { length, 160 }, { tune, 233 }, { bodyStyle, bodyWood }, { bodyShape, 0.6f }, { otLevel, -4 },
            { otRatio, 2.54f },
            { modes, 0.45f }, { noiseLevel, -12 }, { noiseHP, 1500 }, { noiseColor, 0.5f }, { clickType, clickWood },
            { clickLevel, -9 }, { clickTone, 1150 }, { driveAmount, 0 }, { outGain, -2 } },
          { { 0, 18, -0.2f }, { 0.031f, 10.5f, -0.2f }, { 0.0625f, 2.7f, -0.3f }, { 0.14f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.3f }, { 0.45f, 0.12f, -0.3f }, { 0.85f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.25f, 0.2f, -0.3f }, { 0.6f, 0, 0 }, { 1, 0, 0 } } },

        // FG Wood Crack: the brighter of the two wooden snares it was matched to. A harder strike on the
        // Wood body (233 Hz, a slower 12 st drop) with its modes spread higher (centred near 1.5 kHz), a
        // louder, higher stick click and more wires.
        { "FG Wood Crack",
          { { length, 160 }, { tune, 233 }, { bodyStyle, bodyWood }, { bodyShape, 0.85f }, { otLevel, -1 },
            { otRatio, 2.8f },
            { modes, 0.65f }, { noiseLevel, -7 }, { noiseHP, 1500 }, { noiseColor, 0.5f }, { clickType, clickWood },
            { clickLevel, -4 }, { clickTone, 1600 }, { driveAmount, 0 }, { outGain, -3 } },
          { { 0, 12, -0.1f }, { 0.028f, 11, -0.2f }, { 0.056f, 4.5f, -0.3f }, { 0.11f, 1.4f, -0.2f }, { 0.2f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.3f }, { 0.45f, 0.12f, -0.3f }, { 0.85f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.25f, 0.2f, -0.3f }, { 0.6f, 0, 0 }, { 1, 0, 0 } } },

        // Matched to a loud, limited snare tuned to G (198 Hz): a hard strike that drops ~10 st in
        // 12 ms, a crack of wires, wires that swell to 50-90 ms and are cut off around 190 ms, an
        // air layer above 9 kHz, and a short wide gated room for the tail.
        { "Power Snare G",
          { { length, 380 }, { tune, 198 }, { bodyStyle, bodyMembrane }, { bodyLevel, 3 }, { bodyShape, 0.6f },
            { otLevel, -20 },
            { modes, 0.05f }, { noiseLevel, -1.5f }, { noiseHP, 1500 }, { noiseLP, 7500 }, { noiseColor, 0.5f },
            { noiseCouple, 0.3f }, { noiseWidth, 0.3f }, { clickType, clickSnap }, { clickLevel, 0 }, { clickTone, 3500 },
            { clickLength, 4 }, { driveAmount, 0.2f }, { eqMidFreq, 1200 }, { eqMidGain, 4 }, { outGain, -1 },
            { revMix, 0.18f }, { revSize, 0.4f }, { revDecay, 450 }, { revPreDelay, 5 }, { revLowCut, 800 },
            { revHighCut, 9000 }, { revGate, 1 }, { revGateTime, 315 }, { revGateFade, 50 },
            { "lay1On", 1 }, { "lay1Source", 2 }, { "lay1NoiseStyle", 1 }, { "lay1Color", 0 }, { "lay1Level", -13.5f },
            { "lay1Env", 1 }, { "lay1PitchEnv", 1 }, { "lay1HP", 9000 } },
          { { 0, 10.5f, 0.15f }, { 0.03f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 0.03f, 0 }, { 0.024f, 1, -0.2f }, { 0.14f, 0.5f, -0.3f }, { 0.24f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 2.5f, 0 }, { 0.012f, 1.5f, -0.4f }, { 0.025f, 0.45f, 0 }, { 0.035f, 0.45f, 0.3f }, { 0.13f, 1, 0 }, { 0.24f, 0.85f, 0 }, { 0.45f, 0.3f, -0.2f }, { 0.53f, 0.02f, -0.3f }, { 0.8f, 0.001f, 0 }, { 1, 0, 0 } } },

        // Long 808 boom: the bridged-T body with a small pitch drop.
        { "808 Kick",
          { { length, 1000 }, { tune, 48 }, { bodyStyle, body808 }, { bodyShape, 0.5f }, { otLevel, kMinLevelDb },
            { noiseOn, 0 }, { clickOn, 0 }, { driveAmount, 0.1f },
            { outHP, 25 }, { outGain, -1 } },
          { { 0, 16, -1 }, { 0.15f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.2f }, { 0.35f, 0.1f, -0.4f }, { 0.65f, 0.01f, -0.3f }, { 0.9f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1f, 0.45f, -0.35f }, { 0.6f, 0.05f, -0.2f }, { 1, 0, 0 } } },

        // 909 body (hard triangle + Snap) with a beater.
        { "909 Kick",
          { { length, 450 }, { tune, 52 }, { bodyStyle, body909 }, { bodyShape, 0.3f }, { otLevel, kMinLevelDb },
            { modes, 0.5f }, { noiseOn, 0 }, { clickType, clickBeater },
            { clickLevel, -8 }, { clickTone, 4000 }, { clickLength, 5 }, { driveAmount, 0.15f }, { outHP, 25 },
            { outGain, -1.5f } },
          { { 0, 20, -1 }, { 0.5556f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.3f }, { 0.2889f, 0.1f, -0.4f }, { 0.4667f, 0.01f, -0.3f }, { 0.7111f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1f, 0.45f, -0.35f }, { 0.6f, 0.05f, -0.2f }, { 1, 0, 0 } } },

        // Kick body with Drive and Knock for a forward, compressed-sounding hit.
        { "Punch Kick",
          { { length, 400 }, { tune, 52 }, { bodyStyle, bodyKick }, { bodyShape, 0.55f }, { otLevel, -10 },
            { otRatio, 2.6f }, { noiseOn, 0 }, { clickType, clickBeater }, { clickLevel, -6 },
            { clickTone, 5000 }, { clickLength, 5 }, { driveAmount, 0.25f }, { outHP, 28 }, { outGain, -3 } },
          { { 0, 24, -1 }, { 0.5f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.3f }, { 0.3f, 0.1f, -0.4f }, { 0.5f, 0.01f, -0.3f }, { 0.75f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1f, 0.45f, -0.35f }, { 0.6f, 0.05f, -0.2f }, { 1, 0, 0 } } },

        // Clean, long sub with no click (the swell of the body's attack buries one); the output low cut
        // keeps the rumble in check.
        { "Sub Kick",
          { { length, 900 }, { tune, 45 }, { bodyStyle, bodyKick }, { bodyShape, 0.05f }, { otLevel, kMinLevelDb },
            { noiseOn, 0 }, { clickOn, 0 }, { driveAmount, 0 }, { outHP, 25 }, { outGain, -0.5f } },
          { { 0, 14, -1 }, { 0.2222f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 2.6116f, -0.2f }, { 0.0273f, 0.6124f, -0.2f }, { 0.1312f, 1.0386f, 0.3777f }, { 0.3333f, 0.1f, -0.4f }, { 0.5556f, 0.01f, -0.3f }, { 0.7778f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1f, 0.45f, -0.35f }, { 0.6f, 0.05f, -0.2f }, { 1, 0, 0 } } },

        // Driven kick with a short gated room and a steep output low cut.
        { "Techno Kick",
          { { tune, 50 }, { bodyStyle, bodyKick }, { bodyShape, 0.6f }, { otLevel, -14 }, { otRatio, 3 },
            { noiseOn, 0 }, { clickType, clickBeater }, { clickLevel, -8 }, { clickLength, 5 }, { driveType, 2 },
            { driveAmount, 0.3f }, { outHP, 30 }, { outHPSlope, 2 }, { outGain, -4.5f }, { revMix, 0.28f },
            { revSize, 0.35f }, { revDecay, 700 }, { revPreDelay, 8 }, { revLowCut, 250 }, { revHighCut, 6000 },
            { revGateTime, 220 }, { revGateFade, 30 } },
          { { 0, 45.3719f, -1 }, { 0.0199f, 9.8739f, -1 }, { 0.4444f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.3f }, { 0.2889f, 0.1f, -0.4f }, { 0.4444f, 0.01f, -0.3f }, { 0.6667f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1f, 0.45f, -0.35f }, { 0.6f, 0.05f, -0.2f }, { 1, 0, 0 } } },

        // Long, boomy trap 808 with some drive; the tail sits around 41 Hz.
        { "Trap 808 Kick",
          { { length, 1500 }, { tune, 41 }, { bodyStyle, body808 }, { bodyShape, 0.7f }, { otLevel, kMinLevelDb },
            { noiseOn, 0 }, { clickType, clickBeater }, { clickLevel, -14 }, { clickTone, 3000 }, { clickLength, 5 },
            { driveType, 2 }, { driveAmount, 0.4f }, { outHP, 25 }, { outGain, -2 } },
          { { 0, 47.032f, -1 }, { 0.12f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.15f }, { 0.4f, 0.1f, -0.4f }, { 0.7333f, 0.01f, -0.3f }, { 0.9f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1f, 0.45f, -0.35f }, { 0.6f, 0.05f, -0.2f }, { 1, 0, 0 } } },

        // Round four-on-the-floor kick: a little drive and knock, soft beater.
        { "House Kick",
          { { length, 450 }, { tune, 53 }, { bodyStyle, bodyKick }, { bodyShape, 0.3f }, { otLevel, -16 },
            { otRatio, 2.2f }, { noiseOn, 0 }, { clickType, clickBeater }, { clickLevel, -10 },
            { clickTone, 3500 }, { clickLength, 6 }, { driveAmount, 0.1f }, { outHP, 28 }, { outGain, -3 } },
          { { 0, 22, -1 }, { 0.4444f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.3f }, { 0.3333f, 0.1f, -0.4f }, { 0.5333f, 0.01f, -0.3f }, { 0.8f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1f, 0.45f, -0.35f }, { 0.6f, 0.05f, -0.2f }, { 1, 0, 0 } } },

        // Tight, clicky and a little higher, for fast breaks.
        { "DnB Kick",
          { { length, 300 }, { tune, 57 }, { bodyStyle, bodyKick }, { bodyShape, 0.4f }, { otLevel, -12 },
            { otRatio, 3.2f }, { noiseOn, 0 }, { clickType, clickBeater }, { clickLevel, -4 },
            { clickTone, 7000 }, { clickLength, 4 }, { driveAmount, 0.2f }, { eqHigh, 2 }, { outHP, 30 },
            { outGain, -3.5f } },
          { { 0, 26, -1.2f }, { 0.5f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.35f }, { 0.3333f, 0.1f, -0.4f }, { 0.5333f, 0.01f, -0.3f }, { 0.8f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1f, 0.45f, -0.35f }, { 0.6f, 0.05f, -0.2f }, { 1, 0, 0 } } },

        // Membrane head tuned low, felt beater and a breath of shell noise.
        { "Acoustic Kick",
          { { length, 450 }, { tune, 55 }, { bodyStyle, bodyMembrane }, { bodyShape, 0.3f }, { otLevel, -14 },
            { modes, 0.15f }, { noiseStyle, noisePink }, { noiseLevel, -18.13f }, { noiseHP, 200 },
            { noiseLP, 3000 }, { noiseColor, 0.5f }, { noiseCouple, 0.2f }, { noiseWidth, 0.2f },
            { clickType, clickBeater }, { clickLevel, -6 }, { clickTone, 2500 }, { clickLength, 8 },
            { driveAmount, 0.1f }, { outHP, 28 }, { outGain, -0.5f } },
          { { 0, 16, -1 }, { 0.4889f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.3f }, { 0.2889f, 0.1f, -0.4f }, { 0.4889f, 0.01f, -0.3f }, { 0.7333f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.6f }, { 0.0889f, 0.2f, -0.3f }, { 0.3333f, 0, 0 }, { 1, 0, 0 } } },

        // FM body: the index falls away fast, leaving a clean sine tail.
        { "FM Kick",
          { { length, 450 }, { tune, 50 }, { bodyStyle, bodyFM }, { bodyShape, 0.25f }, { otLevel, kMinLevelDb },
            { otRatio, 1 }, { modes, 0.6f }, { noiseOn, 0 }, { clickType, clickBeater },
            { clickLevel, -12 }, { clickTone, 3500 }, { clickLength, 5 }, { driveAmount, 0.1f }, { outHP, 28 },
            { outGain, -0.5f } },
          { { 0, 24, -1 }, { 0.5556f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.3f }, { 0.3111f, 0.1f, -0.4f }, { 0.5111f, 0.01f, -0.3f }, { 0.7778f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1f, 0.45f, -0.35f }, { 0.6f, 0.05f, -0.2f }, { 1, 0, 0 } } },

        // Matched to a punchy 152 ms hardware kick: Kick body swept from ~200 Hz to a 49 Hz tail,
        // Hard drive for the clipped edge, a beater, and a breath of crackle for the grit.
        { "HW Kick",
          { { length, 152 }, { tune, 49 }, { bodyStyle, bodyKick }, { bodyShape, 0.35f }, { otLevel, -10 },
            { otRatio, 2.6f },
            { noiseStyle, noiseCrackle }, { noiseLevel, -21 }, { noiseHP, 1500 }, { noiseLP, 12000 }, { noiseColor, 0 },
            { noiseCouple, 0 }, { noiseWidth, 0.3f }, { noiseGrit, 0.5f }, { clickType, clickBeater }, { clickLevel, -6 },
            { clickTone, 5000 }, { clickLength, 5 }, { driveType, 1 }, { driveAmount, 0.35f }, { eqMidFreq, 250 },
            { eqMidGain, 2 }, { outHP, 28 }, { outGain, -6.3f } },
          { { 0, 24.6f, -0.3f }, { 0.09f, 20, 0 }, { 0.16f, 13, 0 }, { 0.24f, 8.6f, -0.2f }, { 0.34f, 5.1f, 0 }, { 0.52f, 4.9f, 0 }, { 0.62f, 0.6f, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.2f }, { 0.35f, 0.5f, 0 }, { 0.75f, 0.3f, 0.3f }, { 0.92f, 0.05f, 0 }, { 1, 0, 0 } },
          { { 0, 0.3f, 0 }, { 0.18f, 0.3f, 0 }, { 0.22f, 1, 0 }, { 0.3f, 1, -0.4f }, { 0.42f, 0.08f, 0 }, { 0.9f, 0.06f, 0 }, { 1, 0, 0 } } },

        // The kick bodies, each with the tail shape that suits it. A level segment bent
        // up (curve > 0) holds and then drops late, a rounded shoulder; bent down
        // (curve < 0) it falls away from the start, a taper.

        // Round body, a gentle sweep, and a tail that stays full and then rolls off. No output low
        // cut: its phase shift at 50 Hz would tilt the broad tops into spikes.
        { "Rounded Kick",
          { { length, 500 }, { tune, 50 }, { bodyStyle, bodyRound }, { bodyShape, 0.55f }, { otLevel, -16 },
            { noiseOn, 0 }, { clickType, clickFelt }, { clickLevel, -16 }, { clickTone, 900 }, { clickLength, 8 },
            { driveAmount, 0.1f }, { outGain, -0.5f } },
          { { 0, 19, -0.8f }, { 0.4f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, 0.5f }, { 0.0256f, 0.2985f, -0.7884f }, { 0.2443f, 0.9807f, 0.4f }, { 0.7191f, 0.1603f, -1 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.0232f, 0.5322f, -0.35f }, { 0.1032f, 0.4809f, -0.35f }, { 0.6f, 0.05f, -0.2f }, { 1, 0, 0 } } },

        // Pointed Taper body that eases back to a sine, a fast sweep, and a tail that thins from the start.
        { "Tapered Kick",
          { { length, 380 }, { tune, 52 }, { bodyStyle, bodyTaper }, { bodyLevel, 2.683f }, { bodyShape, 0.849f },
            { otLevel, -14 }, { otRatio, 2.6f }, { modes, 0.6f }, { noiseOn, 0 }, { clickType, clickPlastic },
            { clickLevel, -12 }, { clickTone, 7000 }, { clickLength, 5 }, { driveAmount, 0.1f }, { outHP, 28 },
            { outGain, -0.5f } },
          { { 0, 30, -1 }, { 0.35f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 4, -0.55f }, { 0.0354f, 1.2349f, -0.55f }, { 0.85f, 0.02f, -0.3f }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1f, 0.45f, -0.35f }, { 0.6f, 0.05f, -0.2f }, { 1, 0, 0 } } },

        // A clean "tok" (the Tok click) over a Hard tail: the pitch falls ~3 octaves in 150 ms and
        // holds on Tune, Bark at x3 gives the clipper something to chew, and the tail sustains.
        { "Hardstyle Kick",
          { { length, 600 }, { tune, 55 }, { bodyStyle, bodyHard }, { bodyShape, 0.7f }, { otLevel, -6 }, { otRatio, 3 },
            { modes, 0.4f }, { noiseOn, 0 }, { clickType, clickTok }, { clickLevel, -8 }, { clickTone, 2200 },
            { clickLength, 5 }, { driveAmount, 0 }, { outHP, 30 }, { outGain, -7 } },
          { { 0, 36, -0.9f }, { 0.25f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, 0 }, { 0.08f, 0.9f, 0.2f }, { 0.75f, 0.8f, 0.5f }, { 0.95f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1f, 0.45f, -0.35f }, { 0.6f, 0.05f, -0.2f }, { 1, 0, 0 } } },

        // Hard body at full Drive and Crunch, Bark an octave up, a higher Tune and a shorter tail.
        { "Gabber Kick",
          { { length, 450 }, { tune, 60 }, { bodyStyle, bodyHard }, { bodyShape, 1 }, { otLevel, -4 }, { otRatio, 2 },
            { modes, 0.8f }, { noiseOn, 0 }, { clickType, clickPlastic }, { clickLevel, -10 }, { clickTone, 5000 },
            { clickLength, 5 }, { driveType, 1 }, { driveAmount, 0.2f }, { outHP, 30 }, { outGain, -8 } },
          { { 0, 30, -1 }, { 0.2f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, 0 }, { 0.6f, 0.85f, 0.4f }, { 0.9f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1f, 0.45f, -0.35f }, { 0.6f, 0.05f, -0.2f }, { 1, 0, 0 } } },

        // Psytrance: an eighth note at 145 BPM (207 ms) with the "fishtail" level, a snappy front that
        // ducks for a moment before the boom. The pitch is on Tune for the second half, so the
        // bass that follows lines up.
        { "Psy Kick",
          { { length, 207 }, { tune, 46 }, { bodyStyle, bodyTaper }, { bodyShape, 0.35f }, { otLevel, -18 },
            { otRatio, 2.6f },
            { modes, 0.8f }, { noiseOn, 0 }, { clickType, clickPlastic }, { clickLevel, -10 }, { clickTone, 7000 },
            { clickLength, 4 }, { driveAmount, 0.15f }, { outHP, 28 }, { outGain, 0 } },
          { { 0, 28, -1 }, { 0.5f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.6f }, { 0.08f, 0.4f, 0 }, { 0.14f, 0.95f, -0.2f }, { 0.6f, 0.6f, -0.3f }, { 0.95f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1f, 0.45f, -0.35f }, { 0.6f, 0.05f, -0.2f }, { 1, 0, 0 } } },

        // Fold body whose folds open out over the hit: bright and buzzy up front, a clean sub tail.
        { "Folded Techno Kick",
          { { length, 480 }, { tune, 50 }, { bodyStyle, bodyFold }, { bodyShape, 0.4f }, { otLevel, -14 },
            { otRatio, 2.6f },
            { modes, 0.7f }, { noiseOn, 0 }, { clickType, clickFelt }, { clickLevel, -12 }, { clickTone, 1200 },
            { clickLength, 8 }, { driveType, 2 }, { driveAmount, 0.2f }, { outHP, 30 }, { outGain, -1.5f } },
          { { 0, 26, -1 }, { 0.45f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.3f }, { 0.3f, 0.1f, -0.4f }, { 0.5f, 0.01f, -0.3f }, { 0.75f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1f, 0.45f, -0.35f }, { 0.6f, 0.05f, -0.2f }, { 1, 0, 0 } } },

        // Tonal body: the harmonics give it a pitched knock that dies into the sub.
        { "Tonal Kick",
          { { tune, 50 }, { bodyStyle, bodyTonal }, { bodyShape, 0.5f }, { otLevel, -4 }, { otRatio, 2 },
            { modes, 0.6f }, { noiseOn, 0 }, { clickType, clickFelt }, { clickLevel, -14 }, { clickTone, 900 },
            { driveAmount, 0.1f }, { outHP, 28 }, { outGain, -3 } },
          { { 0, 49.3992f, -1 }, { 0.0215f, 11.568f, -1 }, { 0.45f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.3f }, { 0.3333f, 0.1f, -0.4f }, { 0.5333f, 0.01f, -0.3f }, { 0.8f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1f, 0.45f, -0.35f }, { 0.6f, 0.05f, -0.2f }, { 1, 0, 0 } } },

        // A miked acoustic kick: a low Membrane head, the Skin click for the beater and the
        // top mic, and a breath of pink noise for the room.
        { "Live Kick",
          { { length, 500 }, { tune, 58 }, { bodyStyle, bodyMembrane }, { bodyShape, 0.25f }, { otLevel, -16 },
            { modes, 0.1f },
            { noiseStyle, noisePink }, { noiseLevel, -24 }, { noiseHP, 300 }, { noiseLP, 6000 }, { noiseColor, 0.6f },
            { noiseCouple, 0.15f }, { noiseWidth, 0.5f }, { clickType, clickSkin }, { clickLevel, -10 }, { clickTone, 4000 },
            { clickLength, 8 }, { driveAmount, 0.1f }, { outHP, 30 }, { outGain, -1 } },
          { { 0, 12, -1 }, { 0.4f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.3f }, { 0.3f, 0.12f, -0.4f }, { 0.55f, 0.01f, -0.3f }, { 0.8f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.6f }, { 0.12f, 0.2f, -0.3f }, { 0.4f, 0, 0 }, { 1, 0, 0 } } },

        /*  DnB snares with the body hit: a saturated Sine body (most of the energy in 120-400 Hz)
            with a big, fast pitch drop into it, the way the genre's snares hit. Each one is matched
            (with `measure-presets`) to a commercial DnB one-shot's measurements: its body pitch, the
            pitch track over the first 20 ms, the body's and the wires' decay, and the energy per band.
            Body share = the 120-400 Hz part of the first 200 ms; the references' are 51-86%, where
            the older DnB presets here have 9-28%. */

        // Neuro: 261 Hz, a short body (-20 dB at 52 ms) with a bright top. Matched to a neuro snare:
        // bands 53 / 11 / 16 / 18 % (ref. 53 / 11 / 18 / 18).
        { "DnB Neuro Snare",
          { { length, 300 }, { tune, 261 }, { bodyStyle, bodySine }, { bodyLevel, -1 }, { bodyShape, 0.5f },
            { otLevel, kMinLevelDb }, { modes, 0 },
            { noiseLevel, -7.5f }, { noiseHP, 800 }, { noiseLP, 19000 }, { noiseColor, 0.1f }, { clickType, clickTick }, { clickLevel, -10 },
            { clickTone, 7000 }, { driveAmount, 0.3f }, { noiseStyle, noiseWires }, { noiseCouple, 0.3f }, { noiseWidth, 0.35f }, { noiseGrit, 0.05f }, { clickLength, 3 }, { driveType, 2 }, { outGain, -4 }, { revMix, 0 } },
          { { 0, 16, -0.6f }, { 0.06f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, 0.15f }, { 0.08f, 0.3f, -0.3f }, { 0.16f, 0.03f, 0 }, { 0.25f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.1f, 0.45f, -0.35f }, { 0.33f, 0.06f, -0.2f }, { 0.45f, 0, 0 }, { 1, 0, 0 } } },

        // Roller: 230 Hz, a 100 ms body under even wires. Matched to a dubplate roller snare:
        // bands 50 / 10 / 26 / 12 % (ref. 51 / 15 / 22 / 12), the same 10 st drop settling in ~17 ms.
        { "DnB Roller Snare",
          { { length, 300 }, { tune, 211 }, { bodyStyle, bodySine }, { bodyLevel, -1 }, { bodyShape, 0.5f },
            { otLevel, kMinLevelDb }, { modes, 0 }, { noiseLevel, -2 }, { noiseHP, 800 }, { noiseLP, 14000 },
            { noiseColor, 0.2f }, { noiseGrit, 0.05f }, { clickLevel, -6 }, { clickLength, 3 }, { driveType, 2 },
            { driveAmount, 0.3f }, { outGain, -4 } },
          { { 0, 15, -0.5f }, { 0.085f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, 0.5437f }, { 0.1127f, 0.2921f, -0.3f }, { 0.33f, 0.04f, 0 }, { 0.45f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 1, -0.5f }, { 0.12f, 0.5f, -0.35f }, { 0.4f, 0.08f, -0.2f }, { 0.55f, 0, 0 }, { 1, 0, 0 } } },

        // Thump: 213 Hz, the heaviest: a 19 st drop, the body swelling in under a bright tick, and long,
        // quiet wires. Matched to a heavy DnB snare: bands 72 / 9 / 10 / 8 % (ref. 71 / 8 / 13 / 8).
        { "DnB Thump Snare",
          { { length, 300 }, { tune, 213 }, { bodyStyle, bodySine }, { bodyLevel, -1 }, { bodyShape, 0.25f },
            { otLevel, kMinLevelDb }, { modes, 0 }, { noiseLevel, -9 }, { noiseHP, 2000 }, { noiseLP, 16000 },
            { noiseColor, 0.1f }, { noiseGrit, 0.05f }, { clickType, clickTick }, { clickLevel, -5 },
            { clickTone, 6000 }, { clickLength, 3 }, { driveType, 2 }, { driveAmount, 0.15f }, { outGain, -4 },
            { revGateTime, 75.77f }, { revGateFade, 16.2f } },
          { { 0, 26, -0.45f }, { 0.07f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 0.3f, -0.4f }, { 0.035f, 1, 0.2f }, { 0.13f, 0.3f, -0.3f }, { 0.23f, 0.05f, 0 }, { 0.33f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 0.8f, -0.5f }, { 0.15f, 0.4f, -0.35f }, { 0.6f, 0.08f, -0.2f }, { 0.8f, 0, 0 }, { 1, 0, 0 } } },

        // Fat: 266 Hz, nearly all body (a long, round one) with a short, airy top. Matched to a layered
        // DnB snare: bands 87 / 5 / 3 / 4 % (ref. 86 / 3 / 5 / 6).
        { "DnB Fat Snare",
          { { length, 300 }, { tune, 266 }, { bodyStyle, bodySine }, { bodyLevel, -1 }, { bodyShape, 0.12f },
            { otLevel, kMinLevelDb }, { modes, 0 },
            { noiseLevel, -5 }, { noiseHP, 2800 }, { noiseLP, 18000 }, { noiseColor, 0.1f }, { clickType, clickTick }, { clickLevel, -6 },
            { clickTone, 6000 }, { driveAmount, 0.15f }, { noiseStyle, noiseWires }, { noiseCouple, 0.3f }, { noiseWidth, 0.35f }, { noiseGrit, 0.05f }, { clickLength, 3 }, { driveType, 2 }, { outGain, -4 }, { revMix, 0 } },
          { { 0, 25, -0.55f }, { 0.05f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 0.2f, -0.5f }, { 0.03f, 1, 0.3f }, { 0.2f, 0.35f, -0.3f }, { 0.32f, 0.06f, 0 }, { 0.45f, 0, 0 }, { 1, 0, 0 } },
          { { 0, 0.8f, -0.5f }, { 0.06f, 0.35f, -0.35f }, { 0.25f, 0.05f, -0.2f }, { 0.6f, 0, 0 }, { 1, 0, 0 } } },
    };

    return presets;
}

} // namespace drum
