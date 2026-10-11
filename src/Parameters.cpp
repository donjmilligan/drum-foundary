// Drum Foundry. Copyright (c) 2026 Don Milligan.
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU AGPL v3 with additional terms; see LICENSE and NOTICE.

#include "Parameters.h"
#include "dsp/StyleInfo.h"
#include "dsp/ParametricEq.h"

static_assert (drum::kNumExtraLayers == drum::kNumLayerEnvelopePairs, "each extra layer has its own pitch and level curve");

namespace drum
{

namespace
{
    using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;
    using Float  = juce::AudioParameterFloat;
    using Attrs  = juce::AudioParameterFloatAttributes;

    juce::NormalisableRange<float> range (float lo, float hi, float centre = 0.0f)
    {
        juce::NormalisableRange<float> r (lo, hi);
        if (centre > lo && centre < hi)
            r.setSkewForCentre (centre);
        return r;
    }

    Attrs hzAttrs()
    {
        return Attrs().withLabel ("Hz")
                      .withStringFromValueFunction ([] (float v, int)
                      {
                          return v >= 1000.0f ? juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz"
                                              : juce::String (juce::roundToInt (v)) + " Hz";
                      })
                      .withValueFromStringFunction ([] (const juce::String& s)
                      {
                          auto v = s.getFloatValue();
                          return s.containsIgnoreCase ("k") ? v * 1000.0f : v;
                      });
    }

    Attrs levelAttrs()
    {
        return Attrs().withLabel ("dB")
                      .withStringFromValueFunction ([] (float v, int)
                      {
                          return v <= kMinLevelDb + 0.01f ? juce::String ("-inf")
                                                          : juce::String (v, 1) + " dB";
                      })
                      .withValueFromStringFunction ([] (const juce::String& s)
                      {
                          return s.containsIgnoreCase ("inf") ? kMinLevelDb : s.getFloatValue();
                      });
    }

    Attrs dbAttrs()
    {
        return Attrs().withLabel ("dB")
                      .withStringFromValueFunction ([] (float v, int)
                      {
                          return (v > 0.05f ? "+" : "") + juce::String (v, 1) + " dB";
                      })
                      .withValueFromStringFunction ([] (const juce::String& s) { return s.getFloatValue(); });
    }

    Attrs percentAttrs()
    {
        return Attrs().withLabel ("%")
                      .withStringFromValueFunction ([] (float v, int)
                      {
                          return juce::String (juce::roundToInt (v * 100.0f)) + "%";
                      })
                      .withValueFromStringFunction ([] (const juce::String& s)
                      {
                          return s.getFloatValue() / 100.0f;
                      });
    }

    Attrs msAttrs()
    {
        return Attrs().withLabel ("ms")
                      .withStringFromValueFunction ([] (float v, int)
                      {
                          if (v >= 1000.0f) return juce::String (v / 1000.0f, 2) + " s";
                          if (v < 10.0f)    return juce::String (v, 1) + " ms";
                          return juce::String (juce::roundToInt (v)) + " ms";
                      })
                      .withValueFromStringFunction ([] (const juce::String& s)
                      {
                          auto v = s.getFloatValue();
                          return (s.containsIgnoreCase ("ms") || ! s.containsIgnoreCase ("s")) ? v : v * 1000.0f;
                      });
    }

    Attrs stAttrs()
    {
        return Attrs().withLabel ("st").withStringFromValueFunction ([] (float v, int)
        {
            return (v > 0.05f ? "+" : "") + juce::String (v, 1) + " st";
        });
    }

    Attrs offsetAttrs()
    {
        return Attrs().withLabel ("ms").withStringFromValueFunction ([] (float v, int)
        {
            return (v > 0.05f ? "+" : "") + juce::String (v, std::abs (v) < 10.0f ? 1 : 0) + " ms";
        });
    }

    Attrs panAttrs()
    {
        return Attrs().withStringFromValueFunction ([] (float v, int)
        {
            const int pct = juce::roundToInt (std::abs (v) * 100.0f);
            return pct == 0 ? juce::String ("C") : juce::String (pct) + (v < 0.0f ? "L" : "R");
        });
    }

    void addFloat (Layout& layout, const juce::String& id, const juce::String& name,
                   juce::NormalisableRange<float> r, float def, Attrs attrs)
    {
        layout.add (std::make_unique<Float> (juce::ParameterID { id, 1 }, name, r, def, attrs));
    }

    void addBool (Layout& layout, const juce::String& id, const juce::String& name, bool def)
    {
        layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, def));
    }

    void addChoice (Layout& layout, const juce::String& id, const juce::String& name, const juce::StringArray& items, int def)
    {
        layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, items, def));
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    Layout l;

    addFloat  (l, pid::length,   "Length",      range (50.0f, 2000.0f, 400.0f), 450.0f, msAttrs());
    addFloat  (l, pid::tune,     "Tune",        range (20.0f, 16000.0f, 200.0f), 185.0f, hzAttrs());
    addBool   (l, pid::keytrack, "Key Track",   true);
    addFloat  (l, pid::velSens,  "Velocity",    range (0.0f, 1.0f), 0.6f, percentAttrs());
    addBool   (l, pid::choke,    "Choke",       false);

    addBool   (l, pid::bodyOn,     "Body On",        true);
    addChoice (l, pid::bodyStyle,  "Body Style",     bodyStyleNames, 0);
    addFloat  (l, pid::bodySmpTune, "Body Sample Tune", range (-24.0f, 24.0f), 0.0f, stAttrs());
    addFloat  (l, pid::bodyOffset, "Body Offset",    range (-100.0f, 100.0f), 0.0f, offsetAttrs());
    addBool   (l, pid::bodySolo,   "Body Solo",      false);
    addBool   (l, pid::bodyInvert, "Body Phase Invert", false);
    addFloat  (l, pid::bodyLevel,  "Body Level",     range (kMinLevelDb, 6.0f), 0.0f, levelAttrs());
    addFloat  (l, pid::bodyShape,  "Body Shape",     range (0.0f, 1.0f), 0.1f, percentAttrs());
    addFloat  (l, pid::otLevel,    "Overtone Level", range (kMinLevelDb, 0.0f), -10.0f, levelAttrs());
    addFloat  (l, pid::otRatio,    "Overtone Ratio", range (1.0f, 4.0f, 2.0f), 1.594f,
               Attrs().withStringFromValueFunction ([] (float v, int)
               {
                   return "x" + juce::String (v, 2);
               }));
    addFloat  (l, pid::modes,      "Head Modes",     range (0.0f, 1.0f), 0.25f, percentAttrs());

    addBool   (l, pid::noiseOn,     "Noise On",       true);
    addChoice (l, pid::noiseStyle,  "Noise Style",    noiseStyleNames, 0);
    addFloat  (l, pid::noiseSmpTune, "Noise Sample Tune", range (-24.0f, 24.0f), 0.0f, stAttrs());
    addFloat  (l, pid::noiseOffset, "Noise Offset",   range (-100.0f, 100.0f), 0.0f, offsetAttrs());
    addBool   (l, pid::noiseSolo,   "Noise Solo",     false);
    addBool   (l, pid::noiseInvert, "Noise Phase Invert", false);
    addFloat  (l, pid::noiseLevel,  "Noise Level",    range (kMinLevelDb, 6.0f), -4.0f, levelAttrs());
    addFloat  (l, pid::noiseHP,     "Noise Low Cut",  range (20.0f, 8000.0f, 800.0f), 450.0f, hzAttrs());
    addFloat  (l, pid::noiseLP,     "Noise High Cut", range (1000.0f, 20000.0f, 6000.0f), 9000.0f, hzAttrs());
    addFloat  (l, pid::noiseColor,  "Noise Color",    range (0.0f, 1.0f), 0.4f, percentAttrs());
    addFloat  (l, pid::noiseCouple, "Wire Coupling",  range (0.0f, 1.0f), 0.3f, percentAttrs());
    addFloat  (l, pid::noiseWidth,  "Noise Width",    range (0.0f, 1.0f), 0.35f, percentAttrs());
    addFloat  (l, pid::noiseGrit,   "Noise Grit",     range (0.0f, 1.0f), 0.0f, percentAttrs());

    addBool   (l, pid::clickOn,     "Click On",     true);
    addChoice (l, pid::clickType,   "Click Type",   clickTypeNames, 1);
    addFloat  (l, pid::clickSmpTune, "Click Sample Tune", range (-24.0f, 24.0f), 0.0f, stAttrs());
    addFloat  (l, pid::clickOffset, "Click Offset", range (-100.0f, 100.0f), 0.0f, offsetAttrs());
    addBool   (l, pid::clickSolo,   "Click Solo",   false);
    addBool   (l, pid::clickInvert, "Click Phase Invert", false);
    addFloat  (l, pid::clickLevel,  "Click Level",  range (kMinLevelDb, 6.0f), -10.0f, levelAttrs());
    addFloat  (l, pid::clickTone,   "Click Tone",   range (300.0f, 12000.0f, 2500.0f), 4500.0f, hzAttrs());
    addFloat  (l, pid::clickLength, "Click Length", range (0.5f, 40.0f, 8.0f), 6.0f, msAttrs());

    addBool   (l, pid::driveOn,     "Drive On",     true);
    addChoice (l, pid::driveType,   "Drive Type",   driveTypeNames, 0);
    addFloat  (l, pid::driveAmount, "Drive",        range (0.0f, 1.0f), 0.2f, percentAttrs());
    addFloat  (l, pid::driveMix,    "Drive Mix",    range (0.0f, 1.0f), 1.0f, percentAttrs());

    addBool   (l, pid::eqOn,      "EQ On",       true);
    addChoice (l, pid::eqStyle,   "EQ Style",    eqStyleNames, eqStyleClean);
    {
        // Bands 1-3 start as the old 3-band EQ (DrumParams::eqBands' defaults).
        const DrumParams defaults;
        for (int b = 0; b < kNumEqBands; ++b)
        {
            const auto& d = defaults.eqBands[(size_t) b];
            const auto id = [b] (const char* suffix) { return eqBandParamId (b, suffix); };
            const auto name = [b] (const char* what) { return "EQ " + juce::String (b + 1) + " " + what; };
            // Off bands start as bells, spread over the range.
            static constexpr float spareFreqs[] { 100.0f, 900.0f, 7000.0f, 200.0f, 500.0f, 2500.0f, 5000.0f, 12000.0f };
            const float freq = b < 3 ? d.freq : spareFreqs[b];
            const auto qAttrs = Attrs().withStringFromValueFunction ([] (float v, int) { return juce::String (v, v < 10.0f ? 2 : 1); });

            l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id (pid::eqBandOn), 1 }, name ("On"), d.on));
            l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id (pid::eqBandType), 1 }, name ("Type"), eqTypeNames, d.type));
            l.add (std::make_unique<Float> (juce::ParameterID { id (pid::eqBandFreq), 1 }, name ("Freq"), range (20.0f, 20000.0f, 630.0f), freq, hzAttrs()));
            l.add (std::make_unique<Float> (juce::ParameterID { id (pid::eqBandGain), 1 }, name ("Gain"), range (-24.0f, 24.0f), 0.0f, dbAttrs()));
            l.add (std::make_unique<Float> (juce::ParameterID { id (pid::eqBandQ), 1 }, name ("Q"), range (0.1f, 18.0f, 1.0f), b < 3 ? d.q : 1.0f, qAttrs));
            l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id (pid::eqBandSlope), 1 }, name ("Slope"), eqSlopeNames, 3));
            l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id (pid::eqBandDyn), 1 }, name ("Dynamic"), false));
            l.add (std::make_unique<Float> (juce::ParameterID { id (pid::eqBandThresh), 1 }, name ("Threshold"), range (-60.0f, 0.0f), -24.0f, dbAttrs()));
            l.add (std::make_unique<Float> (juce::ParameterID { id (pid::eqBandAttack), 1 }, name ("Attack"), range (0.1f, 100.0f, 10.0f), 5.0f, msAttrs()));
            l.add (std::make_unique<Float> (juce::ParameterID { id (pid::eqBandRelease), 1 }, name ("Release"), range (5.0f, 1000.0f, 150.0f), 80.0f, msAttrs()));
        }
    }

    addFloat  (l, pid::outHP,      "Output Low Cut",        range (20.0f, 20000.0f, 632.0f), 20.0f, hzAttrs()); // the whole analyzer, either way
    addChoice (l, pid::outHPSlope, "Output Low Cut Slope",  filterSlopeNames, 1);
    addFloat  (l, pid::outLP,      "Output High Cut",       range (20.0f, 20000.0f, 632.0f), 20000.0f, hzAttrs());
    addChoice (l, pid::outLPSlope, "Output High Cut Slope", filterSlopeNames, 1);

    addFloat  (l, pid::outGain, "Output", range (-24.0f, 12.0f), 0.0f, dbAttrs());
    addChoice (l, pid::clip,        "Clip Mode",    clipModeNames, 1);
    addFloat  (l, pid::clipDrive,   "Clip Drive",   range (0.0f, 24.0f), 0.0f, dbAttrs());
    addFloat  (l, pid::clipCeiling, "Clip Ceiling", range (-12.0f, 0.0f), 0.0f, dbAttrs());

    addBool   (l, pid::revOn,       "Reverb On",        true);
    addFloat  (l, pid::revMix,      "Reverb Mix",       range (0.0f, 1.0f), 0.0f, percentAttrs());
    addFloat  (l, pid::revSize,     "Reverb Size",      range (0.0f, 1.0f), 0.6f, percentAttrs());
    addFloat  (l, pid::revDecay,    "Reverb Decay",     range (100.0f, 6000.0f, 1200.0f), 2500.0f, msAttrs());
    addFloat  (l, pid::revPreDelay, "Reverb Pre-Delay", range (0.0f, 80.0f, 20.0f), 10.0f, msAttrs());
    addFloat  (l, pid::revLowCut,   "Reverb Low Cut",   range (20.0f, 1000.0f, 200.0f), 200.0f, hzAttrs());
    addFloat  (l, pid::revHighCut,  "Reverb High Cut",  range (1000.0f, 20000.0f, 6000.0f), 8000.0f, hzAttrs());
    addBool   (l, pid::revGate,     "Reverb Gate",      true);
    addFloat  (l, pid::revGateTime, "Gate Time",        range (10.0f, 2000.0f, 250.0f), 180.0f, msAttrs());
    addFloat  (l, pid::revGateFade, "Gate Fade",        range (1.0f, 200.0f, 20.0f), 15.0f, msAttrs());
    addChoice (l, pid::revEngine,    "Reverb Engine",     revEngineNames, revEngineCloud);
    addFloat  (l, pid::revDiffusion, "Reverb Diffusion",  range (0.0f, 1.0f), 0.7f, percentAttrs());
    addFloat  (l, pid::revModDepth,  "Reverb Mod Depth",  range (0.0f, 1.0f), 0.3f, percentAttrs());
    addFloat  (l, pid::revModRate,   "Reverb Mod Rate",   range (0.0f, 1.0f), 0.3f, percentAttrs());
    addFloat  (l, pid::revWidth,     "Reverb Width",      range (0.0f, 1.0f), 1.0f, percentAttrs());
    addFloat  (l, pid::revEarly,     "Reverb Early",      range (0.0f, 1.0f), 0.2f, percentAttrs());
    addFloat  (l, pid::revDamp,      "Reverb Damping",    range (0.0f, 1.0f), 0.4f, percentAttrs());

    // The chain's second Drive, compressors and clippers, and the output compressor.
    addBool   (l, pid::drive2On,     "Drive 2 On",   true);
    addChoice (l, pid::drive2Type,   "Drive 2 Type", driveTypeNames, 0);
    addFloat  (l, pid::drive2Amount, "Drive 2",      range (0.0f, 1.0f), 0.2f, percentAttrs());
    addFloat  (l, pid::drive2Mix,    "Drive 2 Mix",  range (0.0f, 1.0f), 1.0f, percentAttrs());

    const auto ratioAttrs = [] { return Attrs().withStringFromValueFunction ([] (float v, int) { return juce::String (v, v < 10.0f ? 1 : 0) + ":1"; }); };
    for (int n = -1; n < 2; ++n)
    {
        const auto name = n < 0 ? juce::String ("Output Comp ") : "Comp " + juce::String (n + 1) + " ";
        const auto id = [n] (const char* suffix) { return compParamId (n, suffix); };
        if (n >= 0)
            addBool (l, id (pid::compOn), name + "On", true);
        addFloat (l, id (pid::compThresh),  name + "Threshold", range (-48.0f, 0.0f), n < 0 ? -12.0f : -18.0f, dbAttrs());
        addFloat (l, id (pid::compRatio),   name + "Ratio",     range (1.0f, 20.0f, 4.0f), 4.0f, ratioAttrs());
        addFloat (l, id (pid::compAttack),  name + "Attack",    range (0.1f, 100.0f, 10.0f), 10.0f, msAttrs());
        addFloat (l, id (pid::compRelease), name + "Release",   range (10.0f, 1000.0f, 120.0f), n < 0 ? 100.0f : 120.0f, msAttrs());
        if (n >= 0)
            addFloat (l, id (pid::compKnee), name + "Knee", range (0.0f, 12.0f), 6.0f, dbAttrs());
        addFloat (l, id (pid::compMakeup),  name + "Makeup",    range (0.0f, 24.0f), 0.0f, dbAttrs());
        if (n >= 0)
            addFloat (l, id (pid::compMix), name + "Mix", range (0.0f, 1.0f), 1.0f, percentAttrs());
        addChoice (l, id (pid::compStyle), name + "Style", compStyleNames, 0);
    }
    for (int n = 0; n < 2; ++n)
    {
        const auto name = "Clip " + juce::String (n + 1) + " ";
        const auto id = [n] (const char* suffix) { return chainClipParamId (n, suffix); };
        addBool   (l, id (pid::chainClipOn),      name + "On",      true);
        addChoice (l, id (pid::chainClipShape),   name + "Shape",   chainClipShapeNames, 0);
        addFloat  (l, id (pid::chainClipDrive),   name + "Drive",   range (0.0f, 24.0f), 0.0f, dbAttrs());
        addFloat  (l, id (pid::chainClipCeiling), name + "Ceiling", range (-12.0f, 0.0f), 0.0f, dbAttrs());
    }

    addBool   (l, pid::scTrigger,   "Sidechain Trigger",   false);
    addFloat  (l, pid::scThreshold, "Sidechain Threshold", range (-60.0f, 0.0f), -24.0f, dbAttrs());
    addFloat  (l, pid::scRetrigger, "Sidechain Retrigger", range (10.0f, 250.0f, 60.0f), 50.0f, msAttrs());

    for (int n = 0; n < kNumExtraLayers; ++n)
    {
        const auto id = [n] (const char* suffix) { return layerParamId (n, suffix); };
        const auto name = [n] (const char* what) { return "Layer " + juce::String (n + 1) + " " + what; };
        const auto add = [&l] (const juce::String& pidStr, const juce::String& nm, juce::NormalisableRange<float> r, float def, Attrs a)
        {
            l.add (std::make_unique<Float> (juce::ParameterID { pidStr, 1 }, nm, r, def, a));
        };

        l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id (pid::layOn), 1 }, name ("On"), false));
        l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id (pid::laySource), 1 }, name ("Source"), layerSourceNames, 0));
        add (id (pid::layLevel), name ("Level"), range (kMinLevelDb, 6.0f), -6.0f, levelAttrs());
        add (id (pid::layTune),  name ("Tune"),  range (-24.0f, 24.0f), 0.0f, stAttrs());
        l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id (pid::layEnv), 1 }, name ("Level Curve"), layerEnvNames, layerEnvOwn));
        add (id (pid::layDelay), name ("Offset"), range (-100.0f, 100.0f), 0.0f, offsetAttrs());
        add (id (pid::layHP),    name ("Low Cut"),  range (20.0f, 8000.0f, 800.0f), 20.0f, hzAttrs());
        add (id (pid::layLP),    name ("High Cut"), range (1000.0f, 20000.0f, 6000.0f), 20000.0f, hzAttrs());
        add (id (pid::layPan),   name ("Pan"),   range (-1.0f, 1.0f), 0.0f, panAttrs());
        l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id (pid::layNoiseStyle), 1 }, name ("Noise Style"), layerNoiseStyleNames, 1));
        l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id (pid::layClickType), 1 }, name ("Click Type"), layerClickTypeNames, 1));
        add (id (pid::layColor),  name ("Noise Color"),  range (0.0f, 1.0f), 0.4f, percentAttrs());
        add (id (pid::layTone),   name ("Click Tone"),   range (300.0f, 12000.0f, 2500.0f), 4500.0f, hzAttrs());
        add (id (pid::layLength), name ("Click Length"), range (0.5f, 40.0f, 8.0f), 6.0f, msAttrs());
        l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id (pid::laySolo), 1 }, name ("Solo"), false));
        l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id (pid::layInvert), 1 }, name ("Phase Invert"), false));
        l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id (pid::layBodyStyle), 1 }, name ("Body Style"), layerBodyStyleNames, 0));
        l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id (pid::layPitchEnv), 1 }, name ("Pitch Curve"), layerEnvNames, layerEnvOwn));
    }

    for (int n = 0; n < kNumLfos; ++n)
    {
        const auto name = [n] (const juce::String& what) { return "LFO " + juce::String (n + 1) + " " + what; };
        const auto choice = [&l] (const juce::String& pidStr, const juce::String& nm, const juce::StringArray& items, int def)
        {
            l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { pidStr, 1 }, nm, items, def));
        };
        const auto add = [&l] (const juce::String& pidStr, const juce::String& nm, juce::NormalisableRange<float> r, float def, Attrs a)
        {
            l.add (std::make_unique<Float> (juce::ParameterID { pidStr, 1 }, nm, r, def, a));
        };

        choice (lfoParamId (n, pid::lfoShape), name ("Shape"), lfoShapeNames, 0);
        add (lfoParamId (n, pid::lfoRate), name ("Rate"), range (0.1f, 100.0f, 6.0f), 8.0f, hzAttrs()
                 .withStringFromValueFunction ([] (float v, int)
                 {
                     return v < 10.0f ? juce::String (v, 2) + " Hz" : juce::String (v, 1) + " Hz";
                 }));
        choice (lfoParamId (n, pid::lfoSync), name ("Sync"), lfoSyncNames, 0);
        choice (lfoParamId (n, pid::lfoMode), name ("Mode"), lfoModeNames, 0);
        add (lfoParamId (n, pid::lfoPhase), name ("Phase"), range (0.0f, 1.0f), 0.0f,
             Attrs().withStringFromValueFunction ([] (float v, int)
             {
                 return juce::String (juce::roundToInt (v * 360.0f)) + juce::String (juce::CharPointer_UTF8 ("\xc2\xb0"));
             }));
        for (int k = 0; k < kLfoSlots; ++k)
        {
            const auto slot = " Target " + juce::String (k + 1);
            choice (lfoParamId (n, pid::lfoDest, k), name (slot.trimStart()), modTargetChoiceNames(), 0);
            add (lfoParamId (n, pid::lfoAmt, k), name (slot.trimStart() + " Amount"), range (-1.0f, 1.0f), 0.0f,
                 Attrs().withStringFromValueFunction ([] (float v, int)
                 {
                     const int pct = juce::roundToInt (v * 100.0f);
                     return (pct > 0 ? "+" : "") + juce::String (pct) + "%";
                 }));
        }
    }

    return l;
}

juce::String eqBandParamId (int band, const char* suffix)
{
    // The old 3-band EQ's knobs keep their ids (sessions, automation, LFO routes).
    const juce::String s (suffix);
    if (band == 0 && s == pid::eqBandGain) return pid::eqLow;
    if (band == 1 && s == pid::eqBandFreq) return pid::eqMidFreq;
    if (band == 1 && s == pid::eqBandGain) return pid::eqMidGain;
    if (band == 2 && s == pid::eqBandGain) return pid::eqHigh;
    return "eq" + juce::String (band + 1) + s;
}

juce::String compParamId (int n, const char* suffix)
{
    return (n < 0 ? juce::String (pid::outComp) : "comp" + juce::String (n + 1)) + suffix;
}

juce::String chainClipParamId (int n, const char* suffix)
{
    return "fxClip" + juce::String (n + 1) + suffix;
}

juce::String layerParamId (int layer, const char* suffix)
{
    return "lay" + juce::String (layer + 1) + suffix;
}

juce::String lfoParamId (int lfo, const char* suffix, int slot)
{
    auto id = "lfo" + juce::String (lfo + 1) + suffix;
    if (slot >= 0)
        id << (slot + 1);
    return id;
}

float lfoSyncBeats (int sync) noexcept
{
    // lfoSyncNames: Free, 4 / 2 / 1 bars, 1/2 .. 1/64, triplets, dotted.
    static constexpr float beats[] { 0.0f, 16.0f, 8.0f, 4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 0.125f, 0.0625f,
                                     2.0f / 3.0f, 1.0f / 3.0f, 1.0f / 6.0f, 1.0f / 12.0f,
                                     1.5f, 0.75f, 0.375f };
    static_assert (std::size (beats) == 17);
    return juce::isPositiveAndBelow (sync, (int) std::size (beats)) ? beats[sync] : 0.0f;
}

//==============================================================================
namespace
{
    /** One parameter an LFO can modulate: its id, the name its LFO slot shows, and how
        its plain value goes into a DrumParams (as ParamReader::read() does). */
    struct ModTarget
    {
        juce::String id, name;
        void (*set) (DrumParams&, int layer, float) = nullptr;
        int layer = 0;
    };

    const std::vector<ModTarget>& modTargets()
    {
        // Only ever append: a slot's Dest stores the index (+ 1) in sessions and presets.
        static const std::vector<ModTarget> targets = []
        {
            using P = DrumParams;
            std::vector<ModTarget> t {
                { pid::length,      "Length",            [] (P& p, int, float v) { p.lengthMs = v; } },
                { pid::tune,        "Tune",              [] (P& p, int, float v) { p.tuneHz = v; } },
                { pid::velSens,     "Velocity",          [] (P& p, int, float v) { p.velSens = v; } },
                { pid::bodyLevel,   "Body Level",        [] (P& p, int, float v) { p.bodyGain = levelDbToGain (v); } },
                { pid::bodyShape,   "Body Shape",        [] (P& p, int, float v) { p.bodyShape = v; } },
                { pid::otLevel,     "Body Overtone",     [] (P& p, int, float v) { p.otGain = levelDbToGain (v); } },
                { pid::otRatio,     "Body OT Ratio",     [] (P& p, int, float v) { p.otRatio = v; } },
                { pid::modes,       "Body Head Modes",   [] (P& p, int, float v) { p.modes = v; } },
                { pid::bodySmpTune, "Body Sample Tune",  [] (P& p, int, float v) { p.bodySmpTune = v; } },
                { pid::bodyOffset,  "Body Offset",       [] (P& p, int, float v) { p.bodyOffsetMs = v; } },
                { pid::noiseLevel,  "Noise Level",       [] (P& p, int, float v) { p.noiseGain = levelDbToGain (v); } },
                { pid::noiseHP,     "Noise Low Cut",     [] (P& p, int, float v) { p.noiseHP = v; } },
                { pid::noiseLP,     "Noise High Cut",    [] (P& p, int, float v) { p.noiseLP = v; } },
                { pid::noiseColor,  "Noise Color",       [] (P& p, int, float v) { p.noiseColor = v; } },
                { pid::noiseCouple, "Noise Coupling",    [] (P& p, int, float v) { p.noiseCouple = v; } },
                { pid::noiseWidth,  "Noise Width",       [] (P& p, int, float v) { p.noiseWidth = v; } },
                { pid::noiseGrit,   "Noise Grit",        [] (P& p, int, float v) { p.noiseGrit = v; } },
                { pid::noiseSmpTune, "Noise Sample Tune", [] (P& p, int, float v) { p.noiseSmpTune = v; } },
                { pid::noiseOffset, "Noise Offset",      [] (P& p, int, float v) { p.noiseOffsetMs = v; } },
                { pid::clickLevel,  "Click Level",       [] (P& p, int, float v) { p.clickGain = levelDbToGain (v); } },
                { pid::clickTone,   "Click Tone",        [] (P& p, int, float v) { p.clickTone = v; } },
                { pid::clickLength, "Click Length",      [] (P& p, int, float v) { p.clickLengthMs = v; } },
                { pid::clickSmpTune, "Click Sample Tune", [] (P& p, int, float v) { p.clickSmpTune = v; } },
                { pid::clickOffset, "Click Offset",      [] (P& p, int, float v) { p.clickOffsetMs = v; } },
                { pid::driveAmount, "Drive",             [] (P& p, int, float v) { p.driveAmount = v; } },
                { pid::driveMix,    "Drive Mix",         [] (P& p, int, float v) { p.driveMix = v; } },
                { pid::eqLow,       "EQ 1 Gain",         [] (P& p, int, float v) { p.eqBands[0].gainDb = v; } },
                { pid::eqMidFreq,   "EQ 2 Freq",         [] (P& p, int, float v) { p.eqBands[1].freq = v; } },
                { pid::eqMidGain,   "EQ 2 Gain",         [] (P& p, int, float v) { p.eqBands[1].gainDb = v; } },
                { pid::eqHigh,      "EQ 3 Gain",         [] (P& p, int, float v) { p.eqBands[2].gainDb = v; } },
                { pid::revMix,      "Reverb Mix",        [] (P& p, int, float v) { p.revMix = v; } },
                { pid::revSize,     "Reverb Size",       [] (P& p, int, float v) { p.revSize = v; } },
                { pid::revDecay,    "Reverb Decay",      [] (P& p, int, float v) { p.revDecayMs = v; } },
                { pid::revPreDelay, "Reverb Pre-Delay",  [] (P& p, int, float v) { p.revPreDelayMs = v; } },
                { pid::revLowCut,   "Reverb Low Cut",    [] (P& p, int, float v) { p.revLowCut = v; } },
                { pid::revHighCut,  "Reverb High Cut",   [] (P& p, int, float v) { p.revHighCut = v; } },
                { pid::revGateTime, "Gate Time",         [] (P& p, int, float v) { p.revGateMs = v; } },
                { pid::revGateFade, "Gate Fade",         [] (P& p, int, float v) { p.revFadeMs = v; } },
                { pid::outHP,       "Output Low Cut",    [] (P& p, int, float v) { p.outHP = v; } },
                { pid::outLP,       "Output High Cut",   [] (P& p, int, float v) { p.outLP = v; } },
                { pid::outGain,     "Output",            [] (P& p, int, float v) { p.outGain = juce::Decibels::decibelsToGain (v); } },
                { pid::clipDrive,   "Clip Drive",        [] (P& p, int, float v) { p.clipDriveDb = v; } },
                { pid::clipCeiling, "Clip Ceiling",      [] (P& p, int, float v) { p.clipCeilingDb = v; } },
            };

            for (int n = 0; n < kNumExtraLayers; ++n)
            {
                const auto add = [&t, n] (const char* suffix, const char* what, void (*set) (P&, int, float))
                {
                    t.push_back ({ layerParamId (n, suffix), "Layer " + juce::String (n + 1) + " " + what, set, n });
                };
                add (pid::layLevel,  "Level",        [] (P& p, int k, float v) { p.layers[(size_t) k].gain = levelDbToGain (v); });
                add (pid::layTune,   "Tune",         [] (P& p, int k, float v) { p.layers[(size_t) k].tuneSt = v; });
                add (pid::layHP,     "Low Cut",      [] (P& p, int k, float v) { p.layers[(size_t) k].hp = v; });
                add (pid::layLP,     "High Cut",     [] (P& p, int k, float v) { p.layers[(size_t) k].lp = v; });
                add (pid::layPan,    "Pan",          [] (P& p, int k, float v) { p.layers[(size_t) k].pan = v; });
                add (pid::layDelay,  "Offset",       [] (P& p, int k, float v) { p.layers[(size_t) k].delayMs = v; });
                add (pid::layColor,  "Color",        [] (P& p, int k, float v) { p.layers[(size_t) k].color = v; });
                add (pid::layTone,   "Click Tone",   [] (P& p, int k, float v) { p.layers[(size_t) k].clickTone = v; });
                add (pid::layLength, "Click Length", [] (P& p, int k, float v) { p.layers[(size_t) k].clickLengthMs = v; });
            }

            // Every EQ band's Freq, Gain and Q (the ones already listed above keep their place).
            for (int b = 0; b < kNumEqBands; ++b)
            {
                const auto add = [&t, b] (const char* suffix, const char* what, void (*set) (P&, int, float))
                {
                    const auto id = eqBandParamId (b, suffix);
                    for (const auto& existing : t)
                        if (existing.id == id)
                            return;
                    t.push_back ({ id, "EQ " + juce::String (b + 1) + " " + what, set, b });
                };
                add (pid::eqBandFreq, "Freq", [] (P& p, int k, float v) { p.eqBands[(size_t) k].freq = v; });
                add (pid::eqBandGain, "Gain", [] (P& p, int k, float v) { p.eqBands[(size_t) k].gainDb = v; });
                add (pid::eqBandQ,    "Q",    [] (P& p, int k, float v) { p.eqBands[(size_t) k].q = v; });
            }

            // The chain's second Drive, compressors and clippers, and the output compressor.
            t.push_back ({ pid::drive2Amount, "Drive 2",     [] (P& p, int, float v) { p.drive2.amount = v; } });
            t.push_back ({ pid::drive2Mix,    "Drive 2 Mix", [] (P& p, int, float v) { p.drive2.mix = v; } });
            for (int n = 0; n < 2; ++n)
            {
                const auto name = "Comp " + juce::String (n + 1) + " ";
                t.push_back ({ compParamId (n, pid::compThresh), name + "Threshold", [] (P& p, int k, float v) { p.comp[(size_t) k].thresholdDb = v; }, n });
                t.push_back ({ compParamId (n, pid::compMakeup), name + "Makeup",    [] (P& p, int k, float v) { p.comp[(size_t) k].makeupDb = v; }, n });
                t.push_back ({ compParamId (n, pid::compMix),    name + "Mix",       [] (P& p, int k, float v) { p.comp[(size_t) k].mix = v; }, n });
            }
            for (int n = 0; n < 2; ++n)
            {
                const auto name = "Clip " + juce::String (n + 1) + " ";
                t.push_back ({ chainClipParamId (n, pid::chainClipDrive),   name + "Drive",   [] (P& p, int k, float v) { p.chainClip[(size_t) k].driveDb = v; }, n });
                t.push_back ({ chainClipParamId (n, pid::chainClipCeiling), name + "Ceiling", [] (P& p, int k, float v) { p.chainClip[(size_t) k].ceilingDb = v; }, n });
            }
            t.push_back ({ compParamId (-1, pid::compThresh), "Output Comp Threshold", [] (P& p, int, float v) { p.outComp.thresholdDb = v; } });
            t.push_back ({ compParamId (-1, pid::compMakeup), "Output Comp Makeup",    [] (P& p, int, float v) { p.outComp.makeupDb = v; } });
            t.push_back ({ pid::revDiffusion, "Reverb Diffusion", [] (P& p, int, float v) { p.revDiffusion = v; } });
            t.push_back ({ pid::revModDepth,  "Reverb Mod Depth", [] (P& p, int, float v) { p.revModDepth = v; } });
            t.push_back ({ pid::revWidth,     "Reverb Width",     [] (P& p, int, float v) { p.revWidth = v; } });
            t.push_back ({ pid::revEarly,     "Reverb Early",     [] (P& p, int, float v) { p.revEarly = v; } });
            t.push_back ({ pid::revDamp,      "Reverb Damping",   [] (P& p, int, float v) { p.revDamp = v; } });
            return t;
        }();
        return targets;
    }

    // NormalisableRange's mapping for a plain skewed range (no interval, no symmetric skew).
    float toNormalised (float v, float lo, float hi, float skew) noexcept
    {
        const float proportion = juce::jlimit (0.0f, 1.0f, (v - lo) / (hi - lo));
        return juce::exactlyEqual (skew, 1.0f) ? proportion : std::pow (proportion, skew);
    }

    float fromNormalised (float proportion, float lo, float hi, float skew) noexcept
    {
        proportion = juce::jlimit (0.0f, 1.0f, proportion);
        if (! juce::exactlyEqual (skew, 1.0f) && proportion > 0.0f)
            proportion = std::exp (std::log (proportion) / skew);
        return lo + (hi - lo) * proportion;
    }
}

const juce::StringArray& modTargetIds()
{
    static const juce::StringArray ids = []
    {
        juce::StringArray a;
        for (const auto& t : modTargets())
            a.add (t.id);
        return a;
    }();
    return ids;
}

const juce::StringArray& modTargetChoiceNames()
{
    static const juce::StringArray names = []
    {
        juce::StringArray a { "None" };
        for (const auto& t : modTargets())
            a.add (t.name);
        return a;
    }();
    return names;
}

int modTargetIndex (const juce::String& paramId)
{
    return modTargetIds().indexOf (paramId);
}

void applyModulation (DrumParams& p, const DrumParams& base, const float* lfoValues) noexcept
{
    const auto& targets = modTargets();
    const int num = juce::jlimit (0, kNumModRoutes, base.numRoutes);

    // Every target once, with the sum of its routes (several LFOs, or slots, can share one).
    for (int i = 0; i < num; ++i)
    {
        const auto& r = base.routes[(size_t) i];
        if (! juce::isPositiveAndBelow (r.target, (int) targets.size()))
            continue;

        bool seen = false;
        for (int j = 0; j < i && ! seen; ++j)
            seen = base.routes[(size_t) j].target == r.target;
        if (seen)
            continue;

        float offset = 0.0f;
        for (int j = i; j < num; ++j)
            if (const auto& o = base.routes[(size_t) j]; o.target == r.target)
                offset += o.amount * lfoValues[juce::jlimit (0, kNumLfos - 1, o.lfo)];

        const float norm = toNormalised (r.base, r.lo, r.hi, r.skew) + offset;
        const auto& t = targets[(size_t) r.target];
        t.set (p, t.layer, fromNormalised (norm, r.lo, r.hi, r.skew));
    }
}

const juce::StringArray& allParameterIds()
{
    static const juce::StringArray ids = []
    {
        juce::StringArray a {
        pid::length, pid::tune, pid::keytrack, pid::velSens, pid::choke,
        pid::bodyOn, pid::bodyStyle, pid::bodySmpTune,
        pid::noiseOn, pid::noiseStyle, pid::noiseSmpTune,
        pid::clickOn, pid::clickSmpTune,
        pid::outHP, pid::outHPSlope, pid::outLP, pid::outLPSlope,
        pid::bodyOffset, pid::noiseOffset, pid::clickOffset, pid::bodySolo, pid::noiseSolo, pid::clickSolo,
        pid::clipDrive, pid::clipCeiling, pid::bodyInvert, pid::noiseInvert, pid::clickInvert,
        pid::bodyLevel, pid::bodyShape, pid::otLevel, pid::otRatio, pid::modes,
        pid::noiseLevel, pid::noiseHP, pid::noiseLP, pid::noiseColor, pid::noiseCouple, pid::noiseWidth, pid::noiseGrit,
        pid::clickType, pid::clickLevel, pid::clickTone, pid::clickLength,
        pid::driveOn, pid::driveType, pid::driveAmount, pid::driveMix,
        pid::eqOn, pid::eqStyle,
        pid::outGain, pid::clip,
        pid::revOn, pid::revMix, pid::revSize, pid::revDecay, pid::revPreDelay, pid::revLowCut, pid::revHighCut,
        pid::revGate, pid::revGateTime, pid::revGateFade,
        pid::scTrigger, pid::scThreshold, pid::scRetrigger,
        pid::drive2On, pid::drive2Type, pid::drive2Amount, pid::drive2Mix,
        pid::revEngine, pid::revDiffusion, pid::revModDepth, pid::revModRate, pid::revWidth, pid::revEarly, pid::revDamp
        };
        for (int n = 0; n < 2; ++n)
            for (auto* suffix : { pid::compOn, pid::compThresh, pid::compRatio, pid::compAttack, pid::compRelease,
                                  pid::compKnee, pid::compMakeup, pid::compMix, pid::compStyle })
                a.add (compParamId (n, suffix));
        for (auto* suffix : { pid::compThresh, pid::compRatio, pid::compAttack, pid::compRelease, pid::compMakeup, pid::compStyle })
            a.add (compParamId (-1, suffix));
        for (int n = 0; n < 2; ++n)
            for (auto* suffix : { pid::chainClipOn, pid::chainClipShape, pid::chainClipDrive, pid::chainClipCeiling })
                a.add (chainClipParamId (n, suffix));
        for (int n = 0; n < kNumExtraLayers; ++n)
            for (auto* suffix : { pid::layOn, pid::laySource, pid::layLevel, pid::layTune, pid::layEnv,
                                  pid::layDelay, pid::layHP, pid::layLP, pid::layPan,
                                  pid::layNoiseStyle, pid::layClickType, pid::layColor, pid::layTone, pid::layLength, pid::laySolo,
                                  pid::layInvert, pid::layBodyStyle, pid::layPitchEnv })
                a.add (layerParamId (n, suffix));
        for (int b = 0; b < kNumEqBands; ++b)
            for (auto* suffix : { pid::eqBandOn, pid::eqBandType, pid::eqBandFreq, pid::eqBandGain, pid::eqBandQ, pid::eqBandSlope,
                                  pid::eqBandDyn, pid::eqBandThresh, pid::eqBandAttack, pid::eqBandRelease })
                a.add (eqBandParamId (b, suffix));
        for (int n = 0; n < kNumLfos; ++n)
        {
            for (auto* suffix : { pid::lfoShape, pid::lfoRate, pid::lfoSync, pid::lfoMode, pid::lfoPhase })
                a.add (lfoParamId (n, suffix));
            for (int k = 0; k < kLfoSlots; ++k)
            {
                a.add (lfoParamId (n, pid::lfoDest, k));
                a.add (lfoParamId (n, pid::lfoAmt, k));
            }
        }
        return a;
    }();
    return ids;
}

const juce::StringArray& sessionOnlyParameterIds()
{
    static const juce::StringArray ids = []
    {
        // Solo is a mixing aid, like the sidechain settings: part of the session, not the patch.
        juce::StringArray a { pid::scTrigger, pid::scThreshold, pid::scRetrigger, pid::bodySolo, pid::noiseSolo, pid::clickSolo };
        for (int n = 0; n < kNumExtraLayers; ++n)
            a.add (layerParamId (n, pid::laySolo));
        return a;
    }();
    return ids;
}

//==============================================================================
ParamReader::ParamReader (juce::AudioProcessorValueTreeState& s)
    : length (get (s, pid::length)), tune (get (s, pid::tune)), keytrack (get (s, pid::keytrack)),
      velSens (get (s, pid::velSens)), choke (get (s, pid::choke)),
      bodyLevel (get (s, pid::bodyLevel)), bodyShape (get (s, pid::bodyShape)),
      otLevel (get (s, pid::otLevel)), otRatio (get (s, pid::otRatio)), modes (get (s, pid::modes)),
      noiseLevel (get (s, pid::noiseLevel)), noiseHP (get (s, pid::noiseHP)), noiseLP (get (s, pid::noiseLP)),
      noiseColor (get (s, pid::noiseColor)), noiseCouple (get (s, pid::noiseCouple)),
      noiseWidth (get (s, pid::noiseWidth)), noiseGrit (get (s, pid::noiseGrit)),
      clickType (get (s, pid::clickType)), clickLevel (get (s, pid::clickLevel)),
      clickTone (get (s, pid::clickTone)), clickLength (get (s, pid::clickLength)),
      driveOn (get (s, pid::driveOn)), driveType (get (s, pid::driveType)), driveAmount (get (s, pid::driveAmount)), driveMix (get (s, pid::driveMix)),
      eqOn (get (s, pid::eqOn)), eqStyle (get (s, pid::eqStyle)),
      outGain (get (s, pid::outGain)), clip (get (s, pid::clip)),
      revOn (get (s, pid::revOn)), revMix (get (s, pid::revMix)), revSize (get (s, pid::revSize)), revDecay (get (s, pid::revDecay)),
      revPreDelay (get (s, pid::revPreDelay)), revLowCut (get (s, pid::revLowCut)),
      revHighCut (get (s, pid::revHighCut)), revGate (get (s, pid::revGate)),
      revGateTime (get (s, pid::revGateTime)), revGateFade (get (s, pid::revGateFade)),
      revEngine (get (s, pid::revEngine)), revDiffusion (get (s, pid::revDiffusion)), revModDepth (get (s, pid::revModDepth)),
      revModRate (get (s, pid::revModRate)), revWidth (get (s, pid::revWidth)), revEarly (get (s, pid::revEarly)),
      revDamp (get (s, pid::revDamp)),
      scTrigger (get (s, pid::scTrigger)), scThreshold (get (s, pid::scThreshold)),
      scRetrigger (get (s, pid::scRetrigger)),
      bodyOn (get (s, pid::bodyOn)), bodyStyle (get (s, pid::bodyStyle)), bodySmpTune (get (s, pid::bodySmpTune)),
      noiseOn (get (s, pid::noiseOn)), noiseStyle (get (s, pid::noiseStyle)), noiseSmpTune (get (s, pid::noiseSmpTune)),
      clickOn (get (s, pid::clickOn)), clickSmpTune (get (s, pid::clickSmpTune)),
      outHP (get (s, pid::outHP)), outHPSlope (get (s, pid::outHPSlope)),
      outLP (get (s, pid::outLP)), outLPSlope (get (s, pid::outLPSlope)),
      bodyOffset (get (s, pid::bodyOffset)), noiseOffset (get (s, pid::noiseOffset)), clickOffset (get (s, pid::clickOffset)),
      bodySolo (get (s, pid::bodySolo)), noiseSolo (get (s, pid::noiseSolo)), clickSolo (get (s, pid::clickSolo)),
      clipDrive (get (s, pid::clipDrive)), clipCeiling (get (s, pid::clipCeiling)),
      bodyInvert (get (s, pid::bodyInvert)), noiseInvert (get (s, pid::noiseInvert)), clickInvert (get (s, pid::clickInvert))
{
    drive2 = { get (s, pid::drive2On), get (s, pid::drive2Type), get (s, pid::drive2Amount), get (s, pid::drive2Mix) };
    for (int n = 0; n < 2; ++n)
    {
        const auto g = [&] (const char* suffix) { return get (s, compParamId (n, suffix)); };
        comps[(size_t) n] = { g (pid::compOn), g (pid::compThresh), g (pid::compRatio), g (pid::compAttack), g (pid::compRelease),
                              g (pid::compKnee), g (pid::compMakeup), g (pid::compMix), g (pid::compStyle) };
        const auto c = [&] (const char* suffix) { return get (s, chainClipParamId (n, suffix)); };
        chainClips[(size_t) n] = { c (pid::chainClipOn), c (pid::chainClipShape), c (pid::chainClipDrive), c (pid::chainClipCeiling) };
    }
    {
        const auto g = [&] (const char* suffix) { return get (s, compParamId (-1, suffix)); };
        outComp = { g (pid::compThresh), g (pid::compRatio), g (pid::compAttack), g (pid::compRelease), g (pid::compMakeup), g (pid::compStyle) };
    }

    for (int b = 0; b < kNumEqBands; ++b)
    {
        const auto g = [&] (const char* suffix) { return get (s, eqBandParamId (b, suffix)); };
        eqBands[(size_t) b] = { g (pid::eqBandOn), g (pid::eqBandType), g (pid::eqBandFreq), g (pid::eqBandGain), g (pid::eqBandQ),
                                g (pid::eqBandSlope), g (pid::eqBandDyn), g (pid::eqBandThresh), g (pid::eqBandAttack), g (pid::eqBandRelease) };
    }

    for (int n = 0; n < kNumExtraLayers; ++n)
    {
        const auto g = [&] (const char* suffix) { return get (s, layerParamId (n, suffix)); };
        layers[(size_t) n] = { g (pid::layOn), g (pid::laySource), g (pid::layLevel), g (pid::layTune), g (pid::layEnv),
                               g (pid::layDelay), g (pid::layHP), g (pid::layLP), g (pid::layPan),
                               g (pid::layNoiseStyle), g (pid::layClickType), g (pid::layColor), g (pid::layTone), g (pid::layLength),
                               g (pid::laySolo), g (pid::layInvert), g (pid::layBodyStyle), g (pid::layPitchEnv) };
    }

    for (int n = 0; n < kNumLfos; ++n)
    {
        auto& l = lfos[(size_t) n];
        l.shape = get (s, lfoParamId (n, pid::lfoShape));
        l.rate  = get (s, lfoParamId (n, pid::lfoRate));
        l.sync  = get (s, lfoParamId (n, pid::lfoSync));
        l.mode  = get (s, lfoParamId (n, pid::lfoMode));
        l.phase = get (s, lfoParamId (n, pid::lfoPhase));
        for (int k = 0; k < kLfoSlots; ++k)
        {
            l.dest[(size_t) k]   = get (s, lfoParamId (n, pid::lfoDest, k));
            l.amount[(size_t) k] = get (s, lfoParamId (n, pid::lfoAmt, k));
        }
    }

    for (const auto& id : modTargetIds())
        targets.push_back (get (s, id));
}

ParamReader::Param ParamReader::get (juce::AudioProcessorValueTreeState& state, const juce::String& id)
{
    Param p;
    p.value = state.getRawParameterValue (id);
    jassert (p.value != nullptr);

    if (auto* param = state.getParameter (id))
    {
        const auto& r = param->getNormalisableRange();
        p.lo   = r.start;
        p.hi   = r.end;
        p.skew = r.skew;
        p.def  = r.convertFrom0to1 (param->getDefaultValue());
        // applyModulation() assumes a plain skewed range for every float parameter.
        jassert (dynamic_cast<juce::AudioParameterFloat*> (param) == nullptr || (! r.symmetricSkew && juce::exactlyEqual (r.interval, 0.0f)));
    }
    return p;
}

DrumParams ParamReader::read() const noexcept
{
    // Param::load() swaps a non-finite value for the parameter's default and
    // clamps the rest to range, so nothing a corrupt session stored reaches the DSP.
    DrumParams p;
    p.lengthMs    = length.load();
    p.tuneHz      = tune.load();
    p.keytrack    = keytrack.load() > 0.5f;
    p.velSens     = velSens.load();
    p.choke       = choke.load() > 0.5f;

    const auto choiceIndex = [] (const Param& param, const juce::StringArray& names)
    {
        return juce::jlimit (0, names.size() - 1, juce::roundToInt (param.load()));
    };

    p.bodyOn      = bodyOn.load() > 0.5f;
    p.bodyStyle   = choiceIndex (bodyStyle, bodyStyleNames);
    p.bodySmpTune = bodySmpTune.load();
    p.bodyGain    = levelDbToGain (bodyLevel.load());
    p.bodyShape   = bodyShape.load();
    p.bodyOffsetMs = bodyOffset.load();
    p.bodySolo    = bodySolo.load() > 0.5f;
    p.bodyInvert  = bodyInvert.load() > 0.5f;
    p.otGain      = levelDbToGain (otLevel.load());
    p.otRatio     = otRatio.load();
    p.modes       = modes.load();

    p.noiseOn      = noiseOn.load() > 0.5f;
    p.noiseStyle   = choiceIndex (noiseStyle, noiseStyleNames);
    p.noiseSmpTune = noiseSmpTune.load();
    p.noiseOffsetMs = noiseOffset.load();
    p.noiseSolo    = noiseSolo.load() > 0.5f;
    p.noiseInvert  = noiseInvert.load() > 0.5f;
    p.noiseGain   = levelDbToGain (noiseLevel.load());
    p.noiseHP     = noiseHP.load();
    p.noiseLP     = noiseLP.load();
    p.noiseColor  = noiseColor.load();
    p.noiseCouple = noiseCouple.load();
    p.noiseWidth  = noiseWidth.load();
    p.noiseGrit   = noiseGrit.load();

    p.clickOn       = clickOn.load() > 0.5f;
    p.clickType     = choiceIndex (clickType, clickTypeNames);
    p.clickSmpTune  = clickSmpTune.load();
    p.clickOffsetMs = clickOffset.load();
    p.clickSolo     = clickSolo.load() > 0.5f;
    p.clickInvert   = clickInvert.load() > 0.5f;
    p.clickGain     = levelDbToGain (clickLevel.load());
    p.clickTone     = clickTone.load();
    p.clickLengthMs = clickLength.load();

    p.driveOn     = driveOn.load() > 0.5f;
    p.driveType   = juce::jlimit (0, driveTypeNames.size() - 1, juce::roundToInt (driveType.load()));
    p.driveAmount = driveAmount.load();
    p.driveMix    = driveMix.load();

    p.eqOn      = eqOn.load() > 0.5f;
    p.eqStyle   = choiceIndex (eqStyle, eqStyleNames);
    for (size_t b = 0; b < eqBands.size(); ++b)
    {
        const auto& src = eqBands[b];
        auto& dst = p.eqBands[b];
        dst.on          = src.on.load() > 0.5f;
        dst.type        = choiceIndex (src.type, eqTypeNames);
        dst.freq        = src.freq.load();
        dst.gainDb      = src.gain.load();
        dst.q           = src.q.load();
        dst.slope       = choiceIndex (src.slope, eqSlopeNames);
        dst.dynamic     = src.dyn.load() > 0.5f;
        dst.thresholdDb = src.thresh.load();
        dst.attackMs    = src.attack.load();
        dst.releaseMs   = src.release.load();
    }

    p.outHP      = outHP.load();
    p.outHPSlope = choiceIndex (outHPSlope, filterSlopeNames);
    p.outLP      = outLP.load();
    p.outLPSlope = choiceIndex (outLPSlope, filterSlopeNames);

    p.outGain = juce::Decibels::decibelsToGain (outGain.load());
    p.clipMode      = choiceIndex (clip, clipModeNames);
    p.clipDriveDb   = clipDrive.load();
    p.clipCeilingDb = clipCeiling.load();

    p.revOn         = revOn.load() > 0.5f;
    p.revMix        = revMix.load();
    p.revSize       = revSize.load();
    p.revDecayMs    = revDecay.load();
    p.revPreDelayMs = revPreDelay.load();
    p.revLowCut     = revLowCut.load();
    p.revHighCut    = revHighCut.load();
    p.revGate       = revGate.load() > 0.5f;
    p.revGateMs     = revGateTime.load();
    p.revFadeMs     = revGateFade.load();
    p.revEngine     = choiceIndex (revEngine, revEngineNames);
    p.revDiffusion  = revDiffusion.load();
    p.revModDepth   = revModDepth.load();
    p.revModRate    = revModRate.load();
    p.revWidth      = revWidth.load();
    p.revEarly      = revEarly.load();
    p.revDamp       = revDamp.load();

    p.drive2.on     = drive2.on.load() > 0.5f;
    p.drive2.type   = choiceIndex (drive2.type, driveTypeNames);
    p.drive2.amount = drive2.amount.load();
    p.drive2.mix    = drive2.mix.load();
    for (size_t n = 0; n < 2; ++n)
    {
        const auto& c = comps[n];
        p.comp[n] = { c.on.load() > 0.5f, c.thresh.load(), c.ratio.load(), c.attack.load(), c.release.load(),
                      c.knee.load(), c.makeup.load(), c.mix.load(), choiceIndex (c.style, compStyleNames) };
        const auto& k = chainClips[n];
        p.chainClip[n] = { k.on.load() > 0.5f, choiceIndex (k.shape, chainClipShapeNames), k.drive.load(), k.ceiling.load() };
    }
    p.outComp = { true, outComp.thresh.load(), outComp.ratio.load(), outComp.attack.load(), outComp.release.load(),
                  6.0f, outComp.makeup.load(), 1.0f, choiceIndex (outComp.style, compStyleNames) };
    // (p.chain is the processor's, not a parameter: DrumFoundryProcessor::readParams() fills it in.)

    for (size_t n = 0; n < layers.size(); ++n)
    {
        const auto& src = layers[n];
        auto& dst = p.layers[n];
        dst.on      = src.on.load() > 0.5f;
        dst.source  = choiceIndex (src.source, layerSourceNames);
        dst.gain    = levelDbToGain (src.level.load());
        dst.tuneSt  = src.tune.load();
        dst.env     = choiceIndex (src.env, layerEnvNames);
        dst.pitchEnv = choiceIndex (src.pitchEnv, layerEnvNames);
        dst.delayMs = src.delay.load();
        dst.hp      = src.hp.load();
        dst.lp      = src.lp.load();
        dst.pan     = src.pan.load();
        dst.noiseStyle    = choiceIndex (src.noiseStyle, layerNoiseStyleNames);
        dst.clickType     = choiceIndex (src.clickType, layerClickTypeNames);
        dst.color         = src.color.load();
        dst.clickTone     = src.tone.load();
        dst.clickLengthMs = src.length.load();
        dst.solo          = src.solo.load() > 0.5f;
        dst.invert        = src.invert.load() > 0.5f;
        dst.bodyStyle     = choiceIndex (src.bodyStyle, layerBodyStyleNames);
    }

    p.numRoutes = 0;
    for (size_t n = 0; n < lfos.size(); ++n)
    {
        const auto& src = lfos[n];
        auto& dst = p.lfos[n];
        dst.shape  = choiceIndex (src.shape, lfoShapeNames);
        dst.rateHz = src.rate.load();
        dst.sync   = choiceIndex (src.sync, lfoSyncNames);
        dst.mode   = choiceIndex (src.mode, lfoModeNames);
        dst.phase  = src.phase.load();

        for (size_t k = 0; k < (size_t) kLfoSlots; ++k)
        {
            const int target = juce::roundToInt (src.dest[k].load()) - 1;
            const float amount = src.amount[k].load();
            if (! juce::isPositiveAndBelow (target, (int) targets.size()) || juce::exactlyEqual (amount, 0.0f))
                continue;

            const auto& t = targets[(size_t) target];
            auto& r = p.routes[(size_t) p.numRoutes++];
            r.target = target;
            r.lfo    = (int) n;
            r.amount = amount;
            r.base   = t.load();
            r.lo     = t.lo;
            r.hi     = t.hi;
            r.skew   = t.skew;
        }
    }
    return p;
}

SidechainParams ParamReader::readSidechain() const noexcept
{
    SidechainParams p;
    p.enabled     = scTrigger.load() > 0.5f;
    p.thresholdDb = scThreshold.load();
    p.retriggerMs = scRetrigger.load();
    return p;
}

//==============================================================================
juce::String noteName (float midiNote)
{
    return juce::MidiMessage::getMidiNoteName (juce::roundToInt (midiNote), true, true, 3);
}

float frequencyToMidi (float hz) noexcept
{
    return 69.0f + 12.0f * std::log2 (juce::jmax (1.0f, hz) / 440.0f);
}

} // namespace drum
