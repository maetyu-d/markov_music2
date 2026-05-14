#include "DemoScripts.h"

namespace MarkovDemo
{
juce::String defaultScriptFor (int stateIndex, int laneIndex)
{
    auto freq = 160 + (stateIndex * 53) + (laneIndex * 37);
    return "(\n"
           "{ |gate=1, fade=0.12, vol=1|\n"
           "    var pulse = Impulse.kr((~markovTempoHz ? 1) * " + juce::String (4 + laneIndex) + ", 0.0);\n"
           "    var env = Decay2.kr(pulse, 0.01, 0.22);\n"
           "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
           "    var tone = SinOsc.ar(" + juce::String (freq) + " * [1, 1.005]);\n"
           "    var body = LFTri.ar(" + juce::String (freq / 2) + " * [1.002, 1]);\n"
           "    (tone + (body * 0.35)) * env * active * vol * 0.055;\n"
           "}.play;\n"
           ")\n";
}

juce::String scriptForRole (const juce::String& role, int stateIndex, int laneIndex)
{
    auto baseRole = role;
    const auto sevenFour = baseRole.endsWithChar ('7');

    if (sevenFour)
        baseRole = baseRole.dropLastCharacters (1);

    const auto melodicRoot = 50 + (stateIndex * 3) + (laneIndex * 5);
    const auto phraseRoot = 57 + (stateIndex * 2) + (laneIndex * 4);
    const auto textureHz = 150 + stateIndex * 34 + laneIndex * 21;

    if (baseRole == "pulse")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~markovTempoHz ? 1) * 2, 0.0);\n"
               "    var env = Decay2.kr(trig, 0.002, 0.18);\n"
               "    var sweep = EnvGen.kr(Env.perc(0.001, 0.06, 42, -5), trig);\n"
               "    var kick = SinOsc.ar(42 + sweep) * env * 0.62;\n"
               "    var click = BPF.ar(WhiteNoise.ar(0.08), 1700, 0.42) * Decay2.kr(trig, 0.001, 0.014);\n"
               "    Limiter.ar(LPF.ar((kick + click) ! 2, 2600), 0.18) * active * vol * 0.032;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "softdrums")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var clock = Impulse.kr((~markovTempoHz ? 1) * 4.0, 0.0);\n"
               "    var kpat = Dseq([1,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,0,0], inf);\n"
               "    var spat = Dseq([0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,0,0], inf);\n"
               "    var hpat = Dseq([0,0,1,0, 0,0,1,0, 0,0,1,0, 0,0,1,0], inf);\n"
               "    var cpat = Dseq([0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1], inf);\n"
               "    var ktrig = clock * Demand.kr(clock, 0, kpat);\n"
               "    var strig = clock * Demand.kr(clock, 0, spat);\n"
               "    var htrig = clock * Demand.kr(clock, 0, hpat);\n"
               "    var ctrig = clock * Demand.kr(clock, 0, cpat);\n"
               "    var kickEnv = EnvGen.kr(Env.perc(0.004, 0.18, curve: -5), ktrig);\n"
               "    var kick = SinOsc.ar(48 + EnvGen.kr(Env.perc(0.001, 0.05, 24, -6), ktrig)) * kickEnv * 0.58;\n"
               "    var punch = BPF.ar(WhiteNoise.ar(0.18), 1900, 0.52) * EnvGen.kr(Env.perc(0.001, 0.030), ktrig);\n"
               "    var snare = (BPF.ar(PinkNoise.ar(0.50), 1400, 0.66) + SinOsc.ar(205, 0, 0.08)) * EnvGen.kr(Env.perc(0.004, 0.16), strig);\n"
               "    var hat = HPF.ar(WhiteNoise.ar(0.20), 4800) * EnvGen.kr(Env.perc(0.001, 0.050), htrig);\n"
               "    var clap = BPF.ar(WhiteNoise.ar(0.22), 2400, 0.40) * EnvGen.kr(Env.perc(0.002, 0.060), ctrig);\n"
               "    var sig = LeakDC.ar(HPF.ar(kick + (punch * 0.12) + (snare * 0.34) + (hat * 0.10) + (clap * 0.10), 36));\n"
               "    sig = Limiter.ar(sig.tanh, 0.36, 0.010);\n"
               "    Pan2.ar(sig, 0) * active * vol * 0.36;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "shimmer")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~markovTempoHz ? 1) * 1.0, 0.0);\n"
               "    var notes = Demand.kr(trig, 0, Dseq([" + juce::String (melodicRoot + 12) + ", "
                    + juce::String (melodicRoot + 19) + ", " + juce::String (melodicRoot + 24) + ", "
                    + juce::String (melodicRoot + 31) + "].midicps, inf));\n"
               "    var env = Decay2.kr(trig, 0.01, 0.42);\n"
               "    var sig = VarSaw.ar(notes * [0.5, 0.5015], 0, 0.32) * env;\n"
               "    sig = CombC.ar(sig, 0.32, 0.21, 0.55) + (sig * 0.72);\n"
               "    Limiter.ar(LeakDC.ar(LPF.ar(HPF.ar(sig, 180), 2600)), 0.24) * active * vol * 0.046;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "lead")
    {
        const auto leadRoot = 60 + (stateIndex * 2) + (laneIndex * 3);
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~markovTempoHz ? 1) * 1.0, 0.0);\n"
               "    var seq = Dseq([" + juce::String (leadRoot) + ", " + juce::String (leadRoot + 4) + ", "
                    + juce::String (leadRoot + 7) + ", " + juce::String (leadRoot + 12) + ", "
                    + juce::String (leadRoot + 11) + ", " + juce::String (leadRoot + 7) + ", "
                    + juce::String (leadRoot + 4) + ", " + juce::String (leadRoot + 7) + ", "
                    + juce::String (leadRoot + 12) + ", " + juce::String (leadRoot + 16) + ", "
                    + juce::String (leadRoot + 14) + ", " + juce::String (leadRoot + 12) + ", "
                    + juce::String (leadRoot + 7) + ", " + juce::String (leadRoot + 4)
                    + (sevenFour ? "" : ", " + juce::String (leadRoot + 7) + ", " + juce::String (leadRoot + 12)) + "].midicps, inf);\n"
               "    var freq = Demand.kr(trig, 0, seq);\n"
               "    var env = EnvGen.kr(Env.perc(0.006, 0.34, curve: -4), trig);\n"
               "    var bend = EnvGen.kr(Env.perc(0.001, 0.045, 0.018, -4), trig);\n"
               "    var tone = SinOsc.ar(freq * (1 + bend) * [1, 1.002]) + (Pulse.ar(freq * 2, 0.40, 0.16));\n"
               "    var sig = RLPF.ar(tone * env, Decay2.kr(trig, 0.012, 0.22).range(900, 3200), 0.20);\n"
               "    sig = CombC.ar(sig, 0.38, [0.19, 0.255], 0.72) * 0.12 + sig;\n"
               "    LeakDC.ar(Limiter.ar(HPF.ar(sig, 90), 0.30)) * active * vol * 0.128;\n"
               "}.play;\n"
               ")\n";
    }

    if (baseRole == "arp")
    {
        const auto arpRoot = 48 + (stateIndex * 3) + (laneIndex * 5);
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~markovTempoHz ? 1) * 2.0, 0.0);\n"
               "    var seq = Dseq([" + juce::String (arpRoot) + ", " + juce::String (arpRoot + 7) + ", "
                    + juce::String (arpRoot + 12) + ", " + juce::String (arpRoot + 16) + ", "
                    + juce::String (arpRoot + 19) + ", " + juce::String (arpRoot + 16) + ", "
                    + juce::String (arpRoot + 12) + ", " + juce::String (arpRoot + 7) + ", "
                    + juce::String (arpRoot) + ", " + juce::String (arpRoot + 7) + ", "
                    + juce::String (arpRoot + 12) + ", " + juce::String (arpRoot + 16) + ", "
                    + juce::String (arpRoot + 19) + ", " + juce::String (arpRoot + 12)
                    + (sevenFour ? "" : ", " + juce::String (arpRoot + 12) + ", " + juce::String (arpRoot + 7)) + "].midicps, inf);\n"
               "    var freq = Demand.kr(trig, 0, seq);\n"
               "    var env = Decay2.kr(trig, 0.018, 0.92);\n"
               "    var sweep = Decay2.kr(trig, 0.035, 0.62).range(520, 2600);\n"
               "    var sub = VarSaw.ar(freq * 0.5, 0, 0.46, 0.30);\n"
               "    var main = VarSaw.ar(freq, 0, 0.38, 0.38);\n"
               "    var detune = VarSaw.ar(freq * [0.997, 1.004], 0, 0.42, 0.24).sum;\n"
               "    var octave = Pulse.ar(freq * 2.0, 0.42, 0.16);\n"
               "    var dry, echo;\n"
               "    var osc = (sub + main + detune + octave) * env;\n"
               "    var sig = RLPF.ar(osc, sweep, 0.22);\n"
               "    sig = LeakDC.ar(sig.tanh);\n"
               "    dry = Splay.ar([sub * env * 0.45, sig, octave * env * 0.28], 0.48);\n"
               "    echo = CombC.ar(dry, 0.72, [0.31, 0.47], 0.95);\n"
               "    Limiter.ar(LeakDC.ar(HPF.ar(dry + (echo * 0.12), 55)), 0.32) * active * vol * 0.102;\n"
               "}.play;\n"
               ")\n";
    }

    if (baseRole == "counter")
    {
        const auto counterRoot = 55 + (stateIndex * 4) + (laneIndex * 3);
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~markovTempoHz ? 1) * 1.0, 0.0);\n"
               "    var seq = Dseq([" + juce::String (counterRoot + 19) + ", " + juce::String (counterRoot + 16) + ", "
                    + juce::String (counterRoot + 12) + ", " + juce::String (counterRoot + 7) + ", "
                    + juce::String (counterRoot + 11) + ", " + juce::String (counterRoot + 14) + ", "
                    + juce::String (counterRoot + 12) + ", " + juce::String (counterRoot + 7) + "].midicps, inf);\n"
               "    var freq = Demand.kr(trig, 0, seq);\n"
               "    var env = Decay2.kr(trig, 0.035, 0.95);\n"
               "    var sweep = Decay2.kr(trig, 0.07, 0.9).range(420, 1800);\n"
               "    var sig = VarSaw.ar(freq * [0.5, 1, 1.003], 0, [0.44, 0.35, 0.31]).sum * env;\n"
               "    sig = RLPF.ar(sig, sweep, 0.28);\n"
               "    sig = Splay.ar([sig * 0.78, CombC.ar(sig, 0.64, 0.39, 0.85) * 0.10], 0.42);\n"
               "    LeakDC.ar(Limiter.ar(HPF.ar(sig, 120), 0.24)) * active * vol * 0.058;\n"
               "}.play;\n"
               ")\n";
    }

    if (baseRole == "chords")
    {
        const auto chordRoot = 48 + (stateIndex * 2) + laneIndex;
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~markovTempoHz ? 1) * 0.5, 0.0);\n"
               "    var roots = Demand.kr(trig, 0, Dseq([" + juce::String (chordRoot) + ", "
                    + juce::String (chordRoot + 5) + ", " + juce::String (chordRoot + 9) + ", "
                    + juce::String (chordRoot + 7)
                    + (sevenFour ? ", " + juce::String (chordRoot + 12) + ", " + juce::String (chordRoot + 9) + ", " + juce::String (chordRoot + 5) : "") + "].midicps, inf));\n"
               "    var env = EnvGen.kr(Env.perc(0.018, 0.72, curve: -4), trig);\n"
               "    var chord = roots * [1, 1.25, 1.5, 2.0];\n"
               "    var sig = Mix(VarSaw.ar(chord * LFNoise1.kr(0.15 ! 4).range(0.997, 1.004), 0, 0.38, 0.16)) * env;\n"
               "    sig = RLPF.ar(sig, Decay2.kr(trig, 0.05, 0.42).range(650, 2300), 0.26);\n"
               "    sig = Splay.ar([sig, CombC.ar(sig, 0.5, 0.33, 0.80) * 0.10], 0.44);\n"
               "    Limiter.ar(LeakDC.ar(HPF.ar(sig, 90)), 0.26) * active * vol * 0.074;\n"
               "}.play;\n"
               ")\n";
    }

    if (baseRole == "phrase")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~markovTempoHz ? 1) * 1.0, 0.0);\n"
               "    var freq = Demand.kr(trig, 0, Dseq([" + juce::String (phraseRoot + 7) + ", "
                    + juce::String (phraseRoot + 12) + ", " + juce::String (phraseRoot + 14) + ", "
                    + juce::String (phraseRoot + 12) + ", " + juce::String (phraseRoot + 5) + ", "
                    + juce::String (phraseRoot + 7) + "].midicps, inf));\n"
               "    var env = EnvGen.kr(Env.perc(0.08, 1.55, curve: -3), trig);\n"
               "    var sig = (SinOsc.ar(freq * [1, 1.004]) + (LFTri.ar(freq * 0.5, 0, 0.25))) * env;\n"
               "    sig = RLPF.ar(sig, Decay2.kr(trig, 0.04, 0.70).range(520, 1700), 0.30);\n"
               "    LeakDC.ar(Limiter.ar(HPF.ar(sig, 80), 0.24)) * active * vol * 0.104;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "bass")
        return scriptForRole (sevenFour ? "bassline7" : "bassline", stateIndex, laneIndex);

    if (baseRole == "bassline")
    {
        const auto lineRoot = 36 + (stateIndex % 4) * 2 + laneIndex * 2;
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~markovTempoHz ? 1) * 1.0, 0.0);\n"
               "    var seq = Dseq([" + juce::String (lineRoot) + ", " + juce::String (lineRoot) + ", "
                    + juce::String (lineRoot + 7) + ", " + juce::String (lineRoot) + ", "
                    + juce::String (lineRoot + 12) + ", " + juce::String (lineRoot + 12) + ", "
                    + juce::String (lineRoot + 7) + ", " + juce::String (lineRoot) + ", "
                    + juce::String (lineRoot + 10) + ", " + juce::String (lineRoot + 10) + ", "
                    + juce::String (lineRoot + 14) + ", " + juce::String (lineRoot + 10) + ", "
                    + juce::String (lineRoot + 7)
                    + (sevenFour ? ", " + juce::String (lineRoot) : ", " + juce::String (lineRoot + 7) + ", " + juce::String (lineRoot) + ", " + juce::String (lineRoot)) + "].midicps, inf);\n"
               "    var freq = Demand.kr(trig, 0, seq);\n"
               "    var env = Decay2.kr(trig, 0.012, 0.62);\n"
               "    var slide = Lag.kr(freq, 0.035);\n"
               "    var cutoff = Decay2.kr(trig, 0.025, 0.42).range(220, 1180);\n"
               "    var sig = (Pulse.ar(slide * [0.5, 1.0], [0.42, 0.57], [0.30, 0.20]).sum + VarSaw.ar(slide, 0, 0.35, 0.20)) * env;\n"
               "    sig = RLPF.ar(sig.tanh, cutoff, 0.21);\n"
               "    sig = LeakDC.ar(HPF.ar(sig, 38));\n"
               "    Pan2.ar(Limiter.ar(sig, 0.28), -0.06) * active * vol * 0.225;\n"
               "}.play;\n"
               ")\n";
    }

    if (baseRole == "texture")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var slow = LFNoise1.kr(0.18).range(180, 920);\n"
               "    var sig = RLPF.ar(PinkNoise.ar(0.11 ! 2), slow, 0.22);\n"
               "    sig = sig + (SinOsc.ar([" + juce::String (textureHz) + ", "
                    + juce::String (textureHz + 3) + "] * LFNoise1.kr(0.07).range(0.995, 1.01)) * 0.022);\n"
               "    Limiter.ar(LeakDC.ar(HPF.ar(LPF.ar(sig, 1800), 90)), 0.16) * active * vol * 0.026;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "fill")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var trig = Impulse.kr((~markovTempoHz ? 1) * (3 + LFNoise0.kr(0.7).range(0, 2)), 0.0);\n"
               "    var env = Decay2.kr(trig, 0.004, TRand.kr(0.05, 0.16, trig));\n"
               "    var freq = TRand.kr(140, 740, trig);\n"
               "    var sig = SinOsc.ar(freq * [1, 1.012]) * env;\n"
               "    sig = sig + (LFTri.ar(freq * 0.5, 0, 0.22) * env);\n"
               "    Pan2.ar(Limiter.ar(HPF.ar(LPF.ar(sig.sum, 900), 120), 0.12), LFNoise1.kr(1.5)) * active * vol * 0.010;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "break")
        return "(\n"
               "{ |gate=1, fade=0.12, vol=1|\n"
               "    var active = EnvGen.kr(Env.asr(fade, 1, fade), gate);\n"
               "    var clock = Impulse.kr((~markovTempoHz ? 1) * 6, 0.0);\n"
               "    var step = PulseCount.kr(clock) % 32;\n"
               "    var kpat = Dseq([1,0,0,0, 0,0,1,0, 1,0,0,1, 0,0,1,0, 1,0,0,0, 0,1,0,0, 1,0,0,0, 0,0,0,1], inf);\n"
               "    var spat = Dseq([0,0,1,0, 0,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,1,0, 0,0,0,0, 0,0,1,0, 0,1,0,0], inf);\n"
               "    var hpat = Dseq([1,0,0,0, 0,1,0,0, 1,0,0,0, 0,0,0,0], inf);\n"
               "    var ktrig = clock * Demand.kr(clock, 0, kpat);\n"
               "    var strig = clock * Demand.kr(clock, 0, spat);\n"
               "    var htrig = clock * Demand.kr(clock, 0, hpat) * ToggleFF.kr(Impulse.kr((~markovTempoHz ? 1) * 0.75, 0.0));\n"
               "    var glitch = clock * (step > 29) * (TRand.kr(0, 1, clock) > 0.72);\n"
               "    var kick = SinOsc.ar(44 + EnvGen.kr(Env.perc(0.001, 0.045, 38, -6), ktrig)) * Decay2.kr(ktrig, 0.003, 0.095);\n"
               "    var snare = (SinOsc.ar(176) + LFTri.ar(118, 0, 0.4)) * Decay2.kr(strig, 0.004, 0.06);\n"
               "    var hats = SinOsc.ar(820 + Decay2.kr(htrig, 0.001, 0.018, 180)) * Decay2.kr(htrig, 0.002, 0.018);\n"
               "    var cuts = SinOsc.ar(TRand.kr(190, 680, glitch) * [1, 1.018]) * Decay2.kr(glitch, 0.003, 0.025);\n"
               "    var sig = (kick * 0.58) + (snare * 0.14) + (hats * 0.035) + (cuts.sum * 0.045);\n"
               "    sig = LeakDC.ar(Compander.ar(sig, sig, 0.2, 1, 0.55, 0.004, 0.05));\n"
               "    sig = Limiter.ar(HPF.ar(LPF.ar(sig.tanh, 760), 42), 0.10);\n"
               "    Pan2.ar(sig, LFNoise1.kr(1.5).range(-0.1, 0.1)) * active * vol * 0.006;\n"
               "}.play;\n"
               ")\n";

    if (baseRole == "drone")
        return scriptForRole (sevenFour ? "counter7" : "counter", stateIndex, laneIndex);

    return defaultScriptFor (stateIndex, laneIndex);
}

float volumeForRole (const juce::String& role)
{
    auto baseRole = role;

    if (baseRole.endsWithChar ('7'))
        baseRole = baseRole.dropLastCharacters (1);

    if (baseRole == "softdrums") return 0.78f;
    if (baseRole == "bassline")  return 0.76f;
    if (baseRole == "lead")      return 0.56f;
    if (baseRole == "arp")       return 0.50f;
    if (baseRole == "counter")   return 0.46f;
    if (baseRole == "chords")    return 0.56f;
    if (baseRole == "phrase")    return 0.48f;
    if (baseRole == "shimmer")   return 0.34f;
    if (baseRole == "texture")   return 0.30f;
    if (baseRole == "fill")      return 0.22f;
    if (baseRole == "break")     return 0.20f;
    if (baseRole == "pulse")     return 0.35f;
    if (baseRole == "bass")      return 0.55f;
    return 0.48f;
}
}
