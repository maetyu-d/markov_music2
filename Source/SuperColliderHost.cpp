#include "SuperColliderHost.h"

#include <algorithm>
#include <mutex>
#include <thread>

namespace
{
constexpr int superColliderLanguagePort = 57141;

enum class LatencyProfile
{
    stable,
    low,
    ultra
};

constexpr auto activeLatencyProfile = LatencyProfile::low;
constexpr bool enableHiddenCrossfades = true;

struct AudioProfile
{
    double serverLatencySeconds = 0.028;
    int hardwareBufferSize = 64;
    double crossfadeSeconds = 0.006;
};

constexpr AudioProfile getAudioProfile()
{
    if constexpr (activeLatencyProfile == LatencyProfile::stable)
        return { 0.035, 64, 0.010 };
    else if constexpr (activeLatencyProfile == LatencyProfile::ultra)
        return { 0.018, 32, 0.003 };
    else
        return { 0.028, 64, 0.006 };
}

juce::File makeTempScript (const juce::String& laneKey, const juce::String& source)
{
    auto safeKey = laneKey.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_");
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("MarkovFSM")
                   .getChildFile ("runtime")
                   .getChildFile ("lane-scripts");
    dir.createDirectory();
    auto file = dir.getChildFile ("markov-fsm-" + safeKey + ".scd");
    file.replaceWithText (source);
    return file;
}

juce::String scStringLiteral (juce::String value)
{
    value = value.replace ("\\", "\\\\");
    value = value.replace ("\"", "\\\"");
    return "\"" + value + "\"";
}

juce::String scSymbolLiteral (juce::String value)
{
    value = value.replace ("\\", "\\\\");
    value = value.replace ("'", "\\'");
    return "'" + value + "'";
}

juce::String scSymbolArrayLiteral (const juce::StringArray& values)
{
    juce::StringArray symbols;
    for (const auto& value : values)
        symbols.add (scSymbolLiteral (value));

    return "[" + symbols.joinIntoString (", ") + "]";
}

juce::String injectLaneMetering (juce::String source, const juce::String& laneId)
{
    const auto playIndex = source.indexOf ("}.play;");
    if (playIndex < 0)
        return source;

    const auto beforePlay = source.substring (0, playIndex);
    const auto expressionEnd = beforePlay.lastIndexOfChar (';');
    if (expressionEnd < 0)
        return source;

    const auto beforeExpression = source.substring (0, expressionEnd);
    auto expressionStart = beforeExpression.lastIndexOfChar ('\n');
    if (expressionStart < 0)
        expressionStart = 0;
    else
        ++expressionStart;

    const auto expression = source.substring (expressionStart, expressionEnd).trim();
    if (expression.isEmpty())
        return source;

    const auto originalLine = source.substring (expressionStart, expressionEnd);
    const auto trimmedLeft = originalLine.trimStart();
    const auto indent = originalLine.substring (0, originalLine.length() - trimmedLeft.length());
    const auto meteredLine = indent + "~markovMetered.(" + scSymbolLiteral (laneId) + ", " + expression + ");";

    return source.substring (0, expressionStart) + meteredLine + source.substring (expressionEnd + 1);
}

juce::String scFloatLiteral (double value)
{
    return juce::String (juce::jlimit (-1000000.0, 1000000.0, value), 6);
}

juce::String scTimingModeLiteral (NestedTimingMode mode)
{
    switch (mode)
    {
        case NestedTimingMode::followParent: return "\\followParent";
        case NestedTimingMode::freeRun: return "\\freeRun";
        case NestedTimingMode::oneShot: return "\\oneShot";
        case NestedTimingMode::latch: return "\\latch";
    }

    return "\\followParent";
}

bool laneShouldPlayInState (const State& state, const Lane& lane)
{
    juce::ignoreUnused (state);
    return lane.enabled;
}

juce::String machineAsSuperColliderEvent (const MachineModel& model)
{
    juce::String text;
    text << "(id: " << scStringLiteral (model.machineId)
         << ", entry: " << model.entryState
         << ", selected: " << model.entryState
         << ", timing: " << scTimingModeLiteral (model.timingMode)
         << ", division: " << juce::jmax (1, model.parentDivision)
         << ", states: [";

    for (int i = 0; i < model.getStateCount(); ++i)
    {
        const auto& state = model.state (i);
        if (i > 0)
            text << ", ";

        juce::StringArray lanes;
        for (const auto& lane : state.lanes)
            if (laneShouldPlayInState (state, lane))
                lanes.add (lane.id);

        text << "(name: " << scStringLiteral (state.name)
             << ", seconds: " << scFloatLiteral (state.secondsPerBar())
             << ", bpm: " << scFloatLiteral (state.tempoBpm)
             << ", beats: " << scFloatLiteral (static_cast<double> (state.beatsPerBar))
             << ", unit: " << scFloatLiteral (static_cast<double> (state.beatUnit))
             << ", clockBeats: " << scFloatLiteral (static_cast<double> (state.beatsPerBar) * (4.0 / static_cast<double> (state.beatUnit)))
             << ", lanes: " << scSymbolArrayLiteral (lanes)
             << ", rules: [";

        bool hasOutboundRule = false;
        for (const auto& rule : model.rules)
            if (rule.from == i && rule.to != i && rule.weight > 0.0f)
                hasOutboundRule = true;

        bool firstRule = true;
        for (const auto& rule : model.rules)
        {
            if (rule.from != i)
                continue;

            if (hasOutboundRule && rule.to == i)
                continue;

            if (! firstRule)
                text << ", ";

            text << "[" << rule.to << ", " << scFloatLiteral (rule.weight) << "]";
            firstRule = false;
        }

        text << "]";

        if (const auto* child = model.childMachine (i))
            text << ", child: " << machineAsSuperColliderEvent (*child);
        else
            text << ", child: nil";

        text << ")";
    }

    text << "])";
    return text;
}

juce::String shellQuote (juce::String value)
{
    value = value.replace ("'", "'\\''");
    return "'" + value + "'";
}

juce::File runtimeDirectory()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("MarkovFSM")
                   .getChildFile ("runtime");
    dir.createDirectory();
    return dir;
}

void appendRuntimeLog (const juce::String& message)
{
    runtimeDirectory().getChildFile ("app.log")
        .appendText (juce::Time::getCurrentTime().toString (true, true, true, true)
                     + "  " + message + "\n");
}
}

SuperColliderHost::~SuperColliderHost()
    {
        shutdown();
    }

void SuperColliderHost::play(Lane& lane, const juce::String& sclangPath)
    {
        appendRuntimeLog ("play requested: " + lane.id);
        if (lane.playing)
            stop (lane, 0.08);

        if (! ensureBridgeRunning (sclangPath))
        {
            lane.playing = false;
            setStatus ("Audio offline");
            return;
        }

        if (lane.preparedBridge != bridgeGeneration && ! prepare (lane, sclangPath))
            return;

        sendPlayCommand (lane.id);
        lane.playing = true;
        setStatus ("Playing " + lane.name);
    }

bool SuperColliderHost::prepare(Lane& lane, const juce::String& sclangPath)
    {
        if (prepareData ({ lane.id, lane.name, lane.script, lane.volume }, sclangPath) < 0)
            return false;

        lane.preparedBridge = bridgeGeneration;
        return true;
    }

int SuperColliderHost::prepareData(const LaneSnapshot& lane, const juce::String& sclangPath)
    {
        appendRuntimeLog ("prepare requested: " + lane.id);
        const juce::ScopedLock lock (hostLock);

        if (! ensureBridgeRunningLocked (sclangPath))
            return -1;

        auto script = lane.script;
        script = injectLaneMetering (script, lane.id);
        auto scriptFile = makeTempScript (lane.id, script);

        if (auto* existing = tempScripts[lane.id])
        {
            existing->deleteFile();
            tempScriptStorage.removeObject (existing, true);
            tempScripts.remove (lane.id);
        }

        auto* rawFile = new juce::File (scriptFile);
        tempScriptStorage.add (rawFile);
        tempScripts.set (lane.id, rawFile);

        sendLoadCommand (lane.id, scriptFile.getFullPathName());
        sendVolumeCommand (lane.id, lane.volume);
        addLog ("Loaded " + lane.name);
        setStatus ("Audio ready");
        return bridgeGeneration;
    }

void SuperColliderHost::setLaneVolume(Lane& lane)
    {
        lane.volume = juce::jlimit (0.0f, 1.0f, lane.volume);
        if (! lane.playing)
            lane.preparedBridge = -1;

        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
            sendVolumeCommand (lane.id, lane.volume);
    }

void SuperColliderHost::setLaneEffectiveVolume(const Lane& lane, float volume)
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
            sendVolumeCommand (lane.id, juce::jlimit (0.0f, 1.0f, volume));
    }

void SuperColliderHost::stop(Lane& lane, double releaseSeconds)
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
            sendStopCommand (lane.id, releaseSeconds);

        lane.playing = false;
        setStatus (bridgeProcess != nullptr && bridgeProcess->isRunning() ? "Audio ready" : "Audio offline");
    }

void SuperColliderHost::transition(const std::vector<Lane*>& lanesToStop,
                                   const std::vector<Lane*>& lanesToStart,
                                   const juce::String& sclangPath,
                                   double releaseSeconds,
                                   double delaySeconds)
    {
        if (! ensureBridgeRunning (sclangPath))
        {
            for (auto* lane : lanesToStart)
                if (lane != nullptr)
                    lane->playing = false;

            setStatus ("Audio offline");
            return;
        }

        for (auto* lane : lanesToStart)
        {
            if (lane != nullptr && lane->preparedBridge != bridgeGeneration)
                if (! prepare (*lane, sclangPath))
                    return;
        }

        const juce::ScopedLock lock (hostLock);

        juce::StringArray stopIds;
        juce::StringArray playIds;

        for (auto* lane : lanesToStop)
            if (lane != nullptr)
                stopIds.addIfNotAlreadyThere (lane->id);

        for (auto* lane : lanesToStart)
            if (lane != nullptr)
                playIds.addIfNotAlreadyThere (lane->id);

        for (auto* lane : lanesToStop)
            if (lane != nullptr && ! playIds.contains (lane->id))
                lane->playing = false;

        for (auto* lane : lanesToStart)
            if (lane != nullptr)
                lane->playing = true;

        sendTransitionCommand (stopIds, playIds, releaseSeconds, delaySeconds);
        setStatus (playIds.isEmpty() ? "Audio ready" : "Playing state");
    }

void SuperColliderHost::stopAll(MachineModel& model)
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
            sendStopAllCommand();

        markAllLanesStopped (model);
        addLog ("Stopped all Markov lanes");
        setStatus (bridgeProcess != nullptr && bridgeProcess->isRunning() ? "Audio ready" : "Audio offline");
    }

bool SuperColliderHost::isReady() const
    {
        const juce::ScopedLock lock (hostLock);
        return bridgeProcess != nullptr && bridgeProcess->isRunning();
    }

int SuperColliderHost::getBridgeGeneration() const
    {
        const juce::ScopedLock lock (hostLock);
        return bridgeGeneration;
    }

void SuperColliderHost::panic(MachineModel& model)
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
        {
            if (oscConnected)
                oscSender.send ("/markov/panic");

            if (shouldUseCommandFallback())
                writeCommand ("~markovStopAll.();\n");
        }

        markAllLanesStopped (model);

        addLog ("Panic: freed all active SuperCollider lane objects");
        setStatus (bridgeProcess != nullptr && bridgeProcess->isRunning() ? "Audio ready" : "Audio offline");
    }

void SuperColliderHost::configureMachine(const MachineModel& model)
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess == nullptr || ! bridgeProcess->isRunning())
        {
            setStatus ("Audio offline");
            return;
        }

        writeCommand ("~markovConfigureMachine.(" + machineAsSuperColliderEvent (model) + ");\n");
        addLog ("FSM prepared");
    }

void SuperColliderHost::runMachine(int startState, double rateHz)
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
        {
            const auto rate = juce::jlimit (0.05, 8.0, rateHz);
            writeCommand ("~markovRunMachine.(" + juce::String (juce::jmax (0, startState))
                          + ", " + scFloatLiteral (rate) + ");\n");
        }
    }

void SuperColliderHost::pauseMachine()
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
            writeCommand ("~markovPauseMachine.();\n");
    }

void SuperColliderHost::stepMachine()
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
            writeCommand ("~markovStepMachine.();\n");
    }

void SuperColliderHost::testTone(const juce::String& sclangPath)
    {
        const juce::ScopedLock lock (hostLock);

        if (! ensureBridgeRunningLocked (sclangPath))
            return;

        appendRuntimeLog ("test tone requested");
        if (oscConnected)
        {
            oscSender.send ("/markov/test");
            juce::Timer::callAfterDelay (650, [this]
            {
                const juce::ScopedLock retryLock (hostLock);
                if (oscConnected)
                    oscSender.send ("/markov/test");
            });
        }
        else
            writeCommand ("~markovTest.();\n");

        addLog ("Test tone requested");
    }

bool SuperColliderHost::ensureBridgeRunning(const juce::String& sclangPath)
    {
        const juce::ScopedLock lock (hostLock);
        return ensureBridgeRunningLocked (sclangPath);
    }

bool SuperColliderHost::ensureBridgeRunningLocked(const juce::String& sclangPath)
    {
        const auto executable = resolveSclangExecutable (sclangPath);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning() && executable == currentExecutable)
            return true;

        shutdown();
        setStatus ("Booting audio");
        addLog ("Starting SuperCollider bridge: " + executable);
        appendRuntimeLog ("starting bridge: " + executable);

        currentExecutable = executable;
        bridgeDirectory = runtimeDirectory().getChildFile ("sc-bridge");
        bridgeDirectory.deleteRecursively();
        commandDirectory = bridgeDirectory.getChildFile ("commands");
        bridgeDirectory.createDirectory();
        commandDirectory.createDirectory();

        auto bridgeScript = bridgeDirectory.getChildFile ("bridge.scd");
        bridgeScript.replaceWithText (makeBridgeScript());
        bridgeLogFile = bridgeDirectory.getChildFile ("sclang.log");

        auto process = std::make_unique<juce::ChildProcess>();
        juce::StringArray args;
        args.add ("/bin/sh");
        args.add ("-c");
        args.add ("exec " + shellQuote (currentExecutable)
                  + " -D -u " + juce::String (superColliderLanguagePort)
                  + " " + shellQuote (bridgeScript.getFullPathName())
                  + " >> " + shellQuote (bridgeLogFile.getFullPathName()) + " 2>&1");

        if (! process->start (args))
        {
            addLog ("Could not start sclang at: " + executable);
            appendRuntimeLog ("could not start bridge");
            setStatus ("SC start failed");
            return false;
        }

        bridgeProcess = std::move (process);
        oscConnected = oscSender.connect ("127.0.0.1", superColliderLanguagePort);
        ++bridgeGeneration;
        bridgeStartedAtMs = juce::Time::currentTimeMillis();
        setStatus (oscConnected ? "Audio bridge online" : "Audio bridge booting");
        addLog ("Bridge log: " + bridgeLogFile.getFullPathName());
        appendRuntimeLog ("bridge started; log: " + bridgeLogFile.getFullPathName());
        bridgeLogReadPosition = 0;
        startLogReader();
        return true;
    }

juce::String SuperColliderHost::resolveSclangExecutable(const juce::String& sclangPath) const
    {
        if (sclangPath.trim().isNotEmpty())
            return sclangPath.trim();

        auto bundledMacPath = juce::File ("/Applications/SuperCollider.app/Contents/MacOS/sclang");
        if (bundledMacPath.existsAsFile())
            return bundledMacPath.getFullPathName();

        return "sclang";
    }

juce::String SuperColliderHost::makeBridgeScript() const
    {
        const auto commandPath = scStringLiteral (commandDirectory.getFullPathName());
        constexpr auto profile = getAudioProfile();
        const auto latency = juce::String (profile.serverLatencySeconds, 4);
        const auto bufferSize = juce::String (profile.hardwareBufferSize);
        const auto crossfade = juce::String (enableHiddenCrossfades ? profile.crossfadeSeconds : 0.0, 4);
        const auto defaultRelease = juce::String (musicalReleaseSeconds, 3);
        const auto attack = juce::String (0.120, 3);

        return "(\n"
               "Server.default.latency = " + latency + ";\n"
               "s.options.hardwareBufferSize = " + bufferSize + ";\n"
               "s.options.numOutputBusChannels = 2;\n"
               "s.options.memSize = 262144;\n"
               "s.boot;\n"
               "~markovFade = " + crossfade + ";\n"
               "~markovAttack = " + attack + ";\n"
               "~markovRelease = " + defaultRelease + ";\n"
               "~markovServerReady = false;\n"
               "~markovPending = List.new;\n"
               "~markovWhenReady = { |func|\n"
               "    if (~markovServerReady) {\n"
               "        func.value;\n"
               "    } {\n"
               "        ~markovPending.add(func);\n"
               "        s.waitForBoot {\n"
               "            ~markovServerReady = true;\n"
               "            ~markovPending.do { |pending| pending.value };\n"
               "            ~markovPending.clear;\n"
               "        };\n"
               "    };\n"
               "};\n"
               "~markovObjects = IdentityDictionary.new;\n"
               "~markovStopTokens = IdentityDictionary.new;\n"
               "~markovVolumes = IdentityDictionary.new;\n"
               "~markovPrograms = IdentityDictionary.new;\n"
               "~markovLaneBuses = IdentityDictionary.new;\n"
               "~markovLaneRouters = IdentityDictionary.new;\n"
               "~markovMeterIds = IdentityDictionary.new;\n"
               "~markovMeterKeys = IdentityDictionary.new;\n"
               "~markovNextMeterId = 1;\n"
               "~markovJuce = NetAddr(\"127.0.0.1\", 57142);\n"
               "~markovLoad = { |key, path|\n"
               "    var file, source;\n"
               "    file = File(path, \"r\");\n"
               "    if (file.isOpen.not) { (\"Markov could not open lane: \" ++ path).warn; ^nil };\n"
               "    source = file.readAllString;\n"
               "    file.close;\n"
               "    ~markovPrograms[key] = (\"{ \" ++ source ++ \" }\").interpret;\n"
               "};\n"
               "~markovWriteCheckResult = { |resultPath, text|\n"
               "    var out = File(resultPath, \"w\");\n"
               "    if (out.isOpen) { out.write(text); out.close };\n"
               "};\n"
               "~markovCheck = { |checkId, path, resultPath|\n"
               "    var file, source;\n"
               "    (\"MARKOV_CHECK_BEGIN \" ++ checkId).postln;\n"
               "    file = File(path, \"r\");\n"
               "    if (file.isOpen.not) {\n"
               "        ~markovWriteCheckResult.(resultPath, \"ERROR could not open script file\");\n"
               "        (\"MARKOV_CHECK_ERROR \" ++ checkId ++ \" could not open script file\").postln;\n"
               "        ^nil;\n"
               "    };\n"
               "    source = file.readAllString;\n"
               "    file.close;\n"
               "    try {\n"
               "        var compiled = (\"{ \" ++ source ++ \" }\").compile;\n"
               "        if (compiled.isNil) {\n"
               "            ~markovWriteCheckResult.(resultPath, \"ERROR compile failed\");\n"
               "            (\"MARKOV_CHECK_ERROR \" ++ checkId ++ \" compile failed\").postln;\n"
               "        } {\n"
               "            ~markovWriteCheckResult.(resultPath, \"OK\");\n"
               "            (\"MARKOV_CHECK_OK \" ++ checkId).postln;\n"
               "        };\n"
               "    } { |error|\n"
               "        ~markovWriteCheckResult.(resultPath, \"ERROR \" ++ error.errorString);\n"
               "        (\"MARKOV_CHECK_ERROR \" ++ checkId ++ \" \" ++ error.errorString).postln;\n"
               "    };\n"
               "};\n"
               "SynthDef(\\markovLaneRouter, { |bus = 0, replyId = 0|\n"
               "    var sig = In.ar(bus, 2);\n"
               "    var mono = Mix(sig) * 0.5;\n"
               "    var meterTrig = Impulse.kr(30, 0) + Trig1.kr(1, ControlDur.ir);\n"
               "    var rms = Amplitude.kr(mono, 0.004, 0.055).clip(0, 1);\n"
               "    var peak = Peak.kr(mono.abs, meterTrig).clip(0, 1);\n"
               "    SendReply.kr(meterTrig, '/markov/laneMeter', [rms, peak], replyId);\n"
               "    Out.ar(0, sig);\n"
               "}).add;\n"
               "OSCdef(\\markovLaneMeter, { |msg|\n"
               "    var key = ~markovMeterKeys[msg[2].asInteger];\n"
               "    if (key.notNil) { ~markovJuce.sendMsg('/markov/meter', key.asString, msg[3].asFloat, msg[4].asFloat) };\n"
               "}, '/markov/laneMeter');\n"
               "~markovMeterIdFor = { |key|\n"
               "    var id = ~markovMeterIds[key];\n"
               "    if (id.isNil) {\n"
               "        id = ~markovNextMeterId;\n"
               "        ~markovNextMeterId = ~markovNextMeterId + 1;\n"
               "        ~markovMeterIds[key] = id;\n"
               "        ~markovMeterKeys[id] = key;\n"
               "    };\n"
               "    id;\n"
               "};\n"
               "~markovMetered = { |key, sig|\n"
               "    var controlled = sig * Lag.kr(\\markovVol.kr(~markovVolumes[key] ? 1), 0.02);\n"
               "    var mono = Mix(controlled) * 0.5;\n"
               "    var meterTrig = Impulse.kr(30, 0) + Trig1.kr(1, ControlDur.ir);\n"
               "    var rms = Amplitude.kr(mono, 0.004, 0.055).clip(0, 1);\n"
               "    var peak = Peak.kr(mono.abs, meterTrig).clip(0, 1);\n"
               "    SendReply.kr(meterTrig, '/markov/laneMeter', [rms, peak], ~markovMeterIdFor.(key));\n"
               "    controlled;\n"
               "};\n"
               "~markovStartLaneRouter = { |key, bus|\n"
               "    var router = ~markovLaneRouters[key];\n"
               "    var target = ~markovMaster ? s;\n"
               "    if (router.notNil) { router.free };\n"
               "    ~markovLaneRouters[key] = Synth(\\markovLaneRouter,\n"
               "        [\\bus, bus, \\replyId, ~markovMeterIdFor.(key)],\n"
               "        target: target,\n"
               "        addAction: if (~markovMaster.notNil, { \\addBefore }, { \\addToTail }));\n"
               "};\n"
               "~markovLaneBusFor = { |key|\n"
               "    var bus = ~markovLaneBuses[key];\n"
               "    if (bus.isNil) {\n"
               "        bus = Bus.audio(s, 2);\n"
               "        ~markovLaneBuses[key] = bus;\n"
               "        ~markovStartLaneRouter.(key, bus);\n"
               "    };\n"
               "    bus;\n"
               "};\n"
               "~markovStartMaster = {\n"
               "    if (~markovMaster.notNil) { ~markovMaster.free };\n"
               "    ~markovMaster = {\n"
               "        var in = In.ar(0, 2);\n"
               "        var low = HPF.ar(LeakDC.ar(in), 30);\n"
               "        var controlled = Compander.ar(low * 0.95, low, 0.24, 1, 0.30, 0.004, 0.20);\n"
               "        ReplaceOut.ar(0, Limiter.ar(controlled.tanh * 0.78, 0.58, 0.018));\n"
               "    }.play(s, addAction: \\addToTail);\n"
               "};\n"
               "~markovSetVolume = { |key, volume|\n"
               "    var obj;\n"
               "    volume = volume.clip(0, 1);\n"
               "    ~markovVolumes[key] = volume;\n"
               "    obj = ~markovObjects[key];\n"
               "    if (obj.notNil and: { obj.respondsTo(\\set) }) { obj.set(\\markovVol, volume) };\n"
               "};\n"
               "~markovStop = { |key, release|\n"
               "    var obj = ~markovObjects[key];\n"
               "    var token;\n"
               "    release = release ? ~markovRelease;\n"
               "    if (obj.notNil) {\n"
               "        if (obj.respondsTo(\\set)) {\n"
               "            token = (~markovStopTokens[key] ? 0) + 1;\n"
               "            ~markovStopTokens[key] = token;\n"
               "            obj.set(\\gate, 0, \\fade, release);\n"
               "            SystemClock.sched(release + 0.12, {\n"
               "                if ((~markovObjects[key] === obj) and: { ~markovStopTokens[key] == token }) {\n"
               "                    ~markovObjects.removeAt(key);\n"
               "                    ~markovStopTokens.removeAt(key);\n"
               "                    if (obj.respondsTo(\\free)) { obj.free };\n"
               "                };\n"
               "                nil;\n"
               "            });\n"
               "        } {\n"
               "            if (obj.respondsTo(\\run)) { obj.run(false) } {\n"
               "                if (obj.respondsTo(\\stop)) { obj.stop };\n"
               "            };\n"
               "            ~markovObjects.removeAt(key);\n"
               "        };\n"
               "    };\n"
               "};\n"
               "~markovStopAll = {\n"
               "    if (~markovPauseMachine.notNil) { ~markovPauseMachine.() };\n"
               "    ~markovObjects.keys.copy.do { |key| ~markovStop.(key, 0.025) };\n"
               "};\n"
               "~markovPanic = {\n"
               "    s.freeAll;\n"
               "    ~markovObjects = IdentityDictionary.new;\n"
               "    ~markovStopTokens = IdentityDictionary.new;\n"
               "    ~markovVolumes = IdentityDictionary.new;\n"
               "    ~markovLaneBuses = IdentityDictionary.new;\n"
               "    ~markovLaneRouters = IdentityDictionary.new;\n"
               "    ~markovMeterIds = IdentityDictionary.new;\n"
               "    ~markovMeterKeys = IdentityDictionary.new;\n"
               "    ~markovNextMeterId = 1;\n"
               "    SystemClock.sched(0.05, { ~markovStartMaster.(); nil });\n"
               "};\n"
               "~markovWhenReady.({ ~markovStartMaster.(); });\n"
               "~markovPlay = { |key|\n"
               "    ~markovWhenReady.({\n"
               "        var obj = ~markovObjects[key];\n"
               "        var program = ~markovPrograms[key];\n"
               "        if (obj.notNil) {\n"
               "            ~markovStopTokens.removeAt(key);\n"
               "            if (obj.respondsTo(\\set)) { obj.set(\\gate, 1, \\fade, ~markovAttack, \\markovVol, ~markovVolumes[key] ? 1) };\n"
               "        } {\n"
               "            if (program.notNil) {\n"
               "                ~markovStopTokens.removeAt(key);\n"
               "                obj = program.value;\n"
               "                ~markovObjects[key] = obj;\n"
               "            };\n"
               "        };\n"
               "    });\n"
               "};\n"
               "~markovTransition = { |stopKeys, playKeys, release, delay = 0|\n"
               "    ~markovWhenReady.({\n"
               "        var requestedAt = Main.elapsedTime;\n"
               "        var action;\n"
               "        (\"MARKOV_TRANSITION_REQUEST delayMs=\" ++ (delay.max(0) * 1000).round(0.001) ++ \" stop=\" ++ stopKeys.size ++ \" play=\" ++ playKeys.size).postln;\n"
               "        action = {\n"
               "            (\"MARKOV_TRANSITION_EXEC actualMs=\" ++ ((Main.elapsedTime - requestedAt) * 1000).round(0.001)).postln;\n"
               "            s.bind {\n"
               "                stopKeys.do { |key| if (playKeys.includes(key).not) { ~markovStop.(key, release) } };\n"
               "                playKeys.do { |key| ~markovPlay.(key) };\n"
               "            };\n"
               "        };\n"
               "        if (delay <= 0) { action.value } { SystemClock.sched(delay.max(0), { action.value; nil }) };\n"
               "    });\n"
               "};\n"
               "~markovSetMachineTiming = { |machine|\n"
               "    var selected = machine[\\selected] ? machine[\\entry] ? 0;\n"
               "    var state = machine[\\states][selected];\n"
               "    var bpm = (state[\\bpm] ? 104).clip(20, 320);\n"
               "    ~markovTempoHz = bpm / 60;\n"
               "    ~markovMachineClock.tempo = ~markovTempoHz.max(0.05);\n"
               "    (\"MARKOV_TIMING bpm=\" ++ bpm.round(0.001) ++ \" rate=\" ++ (~markovRate ? 1.0).round(0.001) ++ \" hz=\" ++ ~markovTempoHz.round(0.001)).postln;\n"
               "    nil;\n"
               "};\n"
               "~markovConfiguredMachine = nil;\n"
               "~markovRootTask = nil;\n"
               "~markovMachineTokens = IdentityDictionary.new;\n"
               "~markovRate = 1.0;\n"
               "~markovMachineClock = TempoClock.new(1.0);\n"
               "~markovRulesForState = { |machine, index|\n"
               "    var state = machine[\\states][index];\n"
               "    var rules = state[\\rules] ? [];\n"
               "    if (rules.isEmpty) { [[(index + 1) % machine[\\states].size, 1.0]] } { rules };\n"
               "};\n"
               "~markovChooseNextState = { |machine|\n"
               "    var rules = ~markovRulesForState.(machine, machine[\\selected] ? machine[\\entry] ? 0);\n"
               "    var total = rules.inject(0.0, { |sum, rule| sum + rule[1].max(0) });\n"
               "    var pick;\n"
               "    var chosen = rules[0][0];\n"
               "    if (total > 0) {\n"
               "        pick = total.rand;\n"
               "        rules.do { |rule|\n"
               "            if (pick > 0) {\n"
               "                pick = pick - rule[1].max(0);\n"
               "                if (pick <= 0) { chosen = rule[0] };\n"
               "            };\n"
               "        };\n"
               "    };\n"
               "    chosen;\n"
               "};\n"
               "~markovStateLanes = { |machine, index| machine[\\states][index][\\lanes] ? [] };\n"
               "~markovActiveMachineLanes = { |machine|\n"
               "    var keys = ~markovStateLanes.(machine, machine[\\selected] ? machine[\\entry] ? 0);\n"
               "    machine[\\states].do { |state|\n"
               "        if (state[\\child].notNil) { keys = keys ++ ~markovActiveMachineLanes.(state[\\child]) };\n"
               "    };\n"
               "    keys;\n"
               "};\n"
               "~markovInvalidateMachineRecursive = { |machine|\n"
               "    ~markovMachineTokens[machine[\\id]] = (~markovMachineTokens[machine[\\id]] ? 0) + 1;\n"
               "    machine[\\states].do { |state| if (state[\\child].notNil) { ~markovInvalidateMachineRecursive.(state[\\child]) } };\n"
               "};\n"
               "~markovStopMachineRecursive = { |machine|\n"
               "    ~markovInvalidateMachineRecursive.(machine);\n"
               "    ~markovActiveMachineLanes.(machine).do { |key| ~markovStop.(key, ~markovRelease) };\n"
               "};\n"
               "~markovArmChildMachine = { |machine|\n"
               "    machine[\\selected] = machine[\\entry] ? 0;\n"
               "    (\"MARKOV_STATE \" ++ machine[\\id] ++ \" \" ++ machine[\\selected]).postln;\n"
               "    SystemClock.sched(Server.default.latency ? 0, { ~markovJuce.sendMsg('/markov/state', machine[\\id], machine[\\selected]); nil });\n"
               "    ~markovStateLanes.(machine, machine[\\selected]);\n"
               "};\n"
               "~markovEnterMachineState = { |machine, next, force = false|\n"
               "    var previous = machine[\\selected] ? machine[\\entry] ? 0;\n"
               "    var changing = previous != next;\n"
               "    var stopKeys = [];\n"
               "    var playKeys = [];\n"
               "    var previousChild;\n"
               "    var nextChild;\n"
               "    if (changing.not and: { force.not }) { ^nil };\n"
               "    if (changing) {\n"
               "        stopKeys = stopKeys ++ ~markovStateLanes.(machine, previous);\n"
               "        previousChild = machine[\\states][previous][\\child];\n"
               "        if (previousChild.notNil and: { previousChild[\\timing] != \\latch }) {\n"
               "            stopKeys = stopKeys ++ ~markovActiveMachineLanes.(previousChild);\n"
               "            ~markovInvalidateMachineRecursive.(previousChild);\n"
               "        };\n"
               "    };\n"
               "    machine[\\selected] = next;\n"
               "    ~markovSetMachineTiming.(machine);\n"
               "    (\"MARKOV_STATE \" ++ machine[\\id] ++ \" \" ++ next).postln;\n"
               "    SystemClock.sched(Server.default.latency ? 0, { ~markovJuce.sendMsg('/markov/state', machine[\\id], next); nil });\n"
               "    playKeys = ~markovStateLanes.(machine, next);\n"
               "    nextChild = machine[\\states][next][\\child];\n"
               "    if (nextChild.notNil) { playKeys = playKeys ++ ~markovArmChildMachine.(nextChild) };\n"
               "    ~markovTransition.(stopKeys, playKeys, ~markovRelease, 0);\n"
               "    nil;\n"
               "};\n"
               "~markovAdvanceMachine = { |machine|\n"
               "    var next = ~markovChooseNextState.(machine);\n"
               "    ~markovEnterMachineState.(machine, next, false);\n"
               "    nil;\n"
               "};\n"
               "~markovMachineDuration = { |machine|\n"
               "    var selected = machine[\\selected] ? machine[\\entry] ? 0;\n"
               "    var state = machine[\\states][selected];\n"
               "    ((state[\\clockBeats] ? 4) / (~markovRate ? 1.0).max(0.05)).max(0.25);\n"
               "};\n"
               "~markovStartMachineTask = { |machine|\n"
               "    var token = (~markovMachineTokens[machine[\\id]] ? 0) + 1;\n"
               "    var scheduleNext;\n"
               "    ~markovMachineTokens[machine[\\id]] = token;\n"
               "    scheduleNext = {\n"
               "        ~markovSetMachineTiming.(machine);\n"
               "        ~markovMachineClock.sched(~markovMachineDuration.(machine), {\n"
               "            if (~markovMachineTokens[machine[\\id]] == token) {\n"
               "                ~markovAdvanceMachine.(machine);\n"
               "                scheduleNext.value;\n"
               "            };\n"
               "            nil;\n"
               "        });\n"
               "    };\n"
               "    scheduleNext.value;\n"
               "};\n"
               "~markovStartChildMachine = { |machine|\n"
               "    ~markovArmChildMachine.(machine);\n"
               "    // Child machines are pre-armed here; independent child clocks stay disabled until\n"
               "    // the top-level SC scheduler is fully stable.\n"
               "};\n"
               "~markovConfigureMachine = { |machine|\n"
               "    ~markovPauseMachine.();\n"
               "    ~markovConfiguredMachine = machine;\n"
               "    (\"MARKOV_MACHINE_CONFIGURED states=\" ++ machine[\\states].size).postln;\n"
               "};\n"
               "~markovRunMachine = { |startState = 0, rate = 1|\n"
               "    if (~markovConfiguredMachine.isNil) { \"MARKOV_MACHINE_MISSING\".warn; ^nil };\n"
               "    ~markovPauseMachine.();\n"
               "    ~markovRate = rate.max(0.05);\n"
               "    ~markovConfiguredMachine[\\entry] = startState.clip(0, ~markovConfiguredMachine[\\states].size - 1);\n"
               "    ~markovConfiguredMachine[\\selected] = ~markovConfiguredMachine[\\entry];\n"
               "    ~markovEnterMachineState.(~markovConfiguredMachine, ~markovConfiguredMachine[\\entry], true);\n"
               "    ~markovStartMachineTask.(~markovConfiguredMachine);\n"
               "    (\"MARKOV_MACHINE_RUNNING rate=\" ++ ~markovRate).postln;\n"
               "};\n"
               "~markovPauseMachine = {\n"
               "    ~markovMachineTokens.keysValuesDo { |key, token| ~markovMachineTokens[key] = token + 1 };\n"
               "    if (~markovConfiguredMachine.notNil) { ~markovStopMachineRecursive.(~markovConfiguredMachine) };\n"
               "};\n"
               "~markovStepMachine = { if (~markovConfiguredMachine.notNil) { ~markovAdvanceMachine.(~markovConfiguredMachine) } };\n"
               "OSCdef(\\markovLoad, { |msg| ~markovLoad.(msg[1].asString.asSymbol, msg[2].asString); }, '/markov/load');\n"
               "OSCdef(\\markovCheck, { |msg| ~markovCheck.(msg[1].asString, msg[2].asString, msg[3].asString); }, '/markov/check');\n"
               "OSCdef(\\markovPlay, { |msg| ~markovPlay.(msg[1].asString.asSymbol); }, '/markov/play');\n"
               "OSCdef(\\markovRunMachine, { |msg| ~markovRunMachine.(msg[1].asInteger, msg[2].asFloat); }, '/markov/runMachine');\n"
               "OSCdef(\\markovPauseMachine, { ~markovPauseMachine.(); }, '/markov/pauseMachine');\n"
               "OSCdef(\\markovTransition, { |msg|\n"
               "    var release = msg[1].asFloat;\n"
               "    var delay = msg[2].asFloat;\n"
               "    var stopCount = msg[3].asInteger;\n"
               "    var playOffset = 4 + stopCount;\n"
               "    var playCount = msg[playOffset].asInteger;\n"
               "    var stops = Array.fill(stopCount, { |i| msg[4 + i].asString.asSymbol });\n"
               "    var plays = Array.fill(playCount, { |i| msg[playOffset + 1 + i].asString.asSymbol });\n"
               "    ~markovTransition.(stops, plays, release, delay);\n"
               "}, '/markov/transition');\n"
               "OSCdef(\\markovVolume, { |msg| ~markovSetVolume.(msg[1].asString.asSymbol, msg[2].asFloat); }, '/markov/volume');\n"
               "OSCdef(\\markovStop, { |msg| ~markovStop.(msg[1].asString.asSymbol, msg[2].asFloat); }, '/markov/stop');\n"
               "OSCdef(\\markovStopAll, { ~markovStopAll.(); }, '/markov/stopAll');\n"
               "OSCdef(\\markovPanic, { ~markovPanic.(); }, '/markov/panic');\n"
               "OSCdef(\\markovQuit, { ~markovPanic.(); s.quit; SystemClock.sched(0.18, { 0.exit; nil }); }, '/markov/quit');\n"
               "~markovTest = { ~markovWhenReady.({ { SinOsc.ar(660 ! 2) * EnvGen.kr(Env.perc(0.01, 1.8), doneAction: 2) * 0.18 }.play; }); };\n"
               "OSCdef(\\markovTest, { ~markovTest.(); }, '/markov/test');\n"
               "~markovPollCommands = {\n"
               "    var dir = PathName(" + commandPath + ");\n"
               "    dir.files.sort({ |a, b| a.fileName < b.fileName }).do { |file|\n"
               "        if (file.extension == \"scd\") {\n"
               "            var commandFile = File(file.fullPath, \"r\");\n"
               "            var command = commandFile.readAllString;\n"
               "            commandFile.close;\n"
               "            command.interpret;\n"
               "            File.delete(file.fullPath);\n"
               "        };\n"
               "    };\n"
               "    0.012;\n"
               "};\n"
               "SystemClock.sched(0.012, ~markovPollCommands);\n"
               ")\n";
    }

void SuperColliderHost::sendLoadCommand(const juce::String& laneId, const juce::String& scriptPath)
    {
        if (shouldUseCommandFallback())
            writeCommand ("~markovLoad.(" + scSymbolLiteral (laneId) + ", " + scStringLiteral (scriptPath) + ");\n");

        if (oscConnected)
            oscSender.send ("/markov/load", laneId, scriptPath);
    }

void SuperColliderHost::sendPlayCommand(const juce::String& laneId)
    {
        if (shouldUseCommandFallback())
            writeCommand ("~markovPlay.(" + scSymbolLiteral (laneId) + ");\n");

        if (oscConnected)
            oscSender.send ("/markov/play", laneId);
    }

void SuperColliderHost::sendTransitionCommand(const juce::StringArray& stopIds,
                                              const juce::StringArray& playIds,
                                              double releaseSeconds,
                                              double delaySeconds)
    {
        const auto clippedDelay = juce::jlimit (0.0, 1.0, delaySeconds);
        appendRuntimeLog ("transition requested: stop=" + juce::String (stopIds.size())
                          + " play=" + juce::String (playIds.size())
                          + " delayMs=" + juce::String (clippedDelay * 1000.0, 2));

        if (shouldUseCommandFallback())
            writeCommand ("~markovTransition.(" + scSymbolArrayLiteral (stopIds) + ", "
                          + scSymbolArrayLiteral (playIds) + ", "
                          + juce::String (releaseSeconds, 3) + ", "
                          + juce::String (clippedDelay, 4) + ");\n");

        if (oscConnected)
        {
            juce::OSCMessage message ("/markov/transition");
            message.addFloat32 (static_cast<float> (releaseSeconds));
            message.addFloat32 (static_cast<float> (clippedDelay));
            message.addInt32 (stopIds.size());
            for (const auto& id : stopIds)
                message.addString (id);

            message.addInt32 (playIds.size());
            for (const auto& id : playIds)
                message.addString (id);

            oscSender.send (message);
        }
    }

void SuperColliderHost::sendVolumeCommand(const juce::String& laneId, float volume)
    {
        const auto clipped = juce::jlimit (0.0f, 1.0f, volume);

        if (shouldUseCommandFallback())
            writeCommand ("~markovSetVolume.(" + scSymbolLiteral (laneId) + ", " + juce::String (clipped, 3) + ");\n");

        if (oscConnected)
            oscSender.send ("/markov/volume", laneId, clipped);
    }

void SuperColliderHost::sendStopCommand(const juce::String& laneId, double releaseSeconds)
    {
        if (shouldUseCommandFallback())
            writeCommand ("~markovStop.(" + scSymbolLiteral (laneId) + ", " + juce::String (releaseSeconds, 3) + ");\n");

        if (oscConnected)
            oscSender.send ("/markov/stop", laneId, static_cast<float> (releaseSeconds));
    }

void SuperColliderHost::sendStopAllCommand()
    {
        if (shouldUseCommandFallback())
            writeCommand ("~markovStopAll.();\n");

        if (oscConnected)
            oscSender.send ("/markov/stopAll");
    }

void SuperColliderHost::sendClearMachineCommand()
    {
        if (shouldUseCommandFallback())
            writeCommand ("~markovClearMachine.();\n");

        if (oscConnected)
            oscSender.send ("/markov/clear");
    }

bool SuperColliderHost::shouldUseCommandFallback() const
    {
        return ! oscConnected || juce::Time::currentTimeMillis() - bridgeStartedAtMs < 1800;
    }

void SuperColliderHost::writeCommand(const juce::String& command)
    {
        if (! commandDirectory.exists())
            return;

        auto serial = juce::String (++commandSerial).paddedLeft ('0', 8);
        auto file = commandDirectory.getChildFile ("command-"
                    + juce::String::toHexString (juce::Time::currentTimeMillis())
                    + "-" + serial + ".scd");
        file.replaceWithText (command);
    }

void SuperColliderHost::shutdown()
    {
        logReaderShouldRun = false;
        if (logReader.joinable())
        {
            if (logReader.get_id() != std::this_thread::get_id())
                logReader.join();
            else
                logReader.detach();
        }

        if (bridgeProcess != nullptr)
        {
            if (oscConnected)
            {
                oscSender.send ("/markov/panic");
                oscSender.send ("/markov/quit");
                oscSender.disconnect();
                oscConnected = false;
            }

            writeCommand ("~markovPanic.(); s.quit; SystemClock.sched(0.18, { 0.exit; nil });\n");
            juce::Thread::sleep (260);

            if (bridgeProcess->isRunning())
                bridgeProcess->kill();

            bridgeProcess = nullptr;
        }

        tempScriptStorage.clear (true);
        tempScripts.clear();

        // Keep the bridge folder around so sclang.log is available after failures.

        setStatus ("Audio offline");
    }

void SuperColliderHost::markAllLanesStopped(MachineModel& model)
    {
        for (auto& state : model.states)
        {
            for (auto& lane : state.lanes)
                lane.playing = false;

            if (auto* child = model.childMachine (state.index))
                markAllLanesStopped (*child);
        }
    }

void SuperColliderHost::startLogReader()
    {
        logReaderShouldRun = false;
        if (logReader.joinable())
            logReader.join();

        logReaderShouldRun = true;
        logReader = std::thread ([this]
        {
            while (logReaderShouldRun)
            {
                {
                    const juce::ScopedLock lock (hostLock);

                    if (bridgeProcess == nullptr)
                        break;

                    char buffer[4096] {};
                    auto bytesRead = bridgeProcess->readProcessOutput (buffer, static_cast<int> (sizeof (buffer) - 1));
                    if (bytesRead > 0)
                    {
                        buffer[bytesRead] = 0;
                        addLog (juce::String::fromUTF8 (buffer, bytesRead).trimEnd());
                    }

                    if (bridgeLogFile.existsAsFile())
                    {
                        const auto logSize = bridgeLogFile.getSize();
                        if (logSize < bridgeLogReadPosition)
                            bridgeLogReadPosition = 0;

                        if (logSize > bridgeLogReadPosition)
                        {
                            if (auto stream = bridgeLogFile.createInputStream())
                            {
                                stream->setPosition (bridgeLogReadPosition);
                                auto text = stream->readString();
                                bridgeLogReadPosition = stream->getPosition();

                                juce::StringArray lines;
                                lines.addLines (text);
                                for (const auto& line : lines)
                                    addLog (line);
                            }
                        }
                    }

                    if (bridgeProcess != nullptr && ! bridgeProcess->isRunning())
                    {
                        addLog ("SuperCollider bridge exited");
                        bridgeProcess = nullptr;
                        oscConnected = false;
                        setStatus ("Audio offline");
                        break;
                    }
                }

                juce::Thread::sleep (40);
            }
        });
    }

void SuperColliderHost::addLog(const juce::String& message)
    {
        if (message.trim().isEmpty())
            return;

        if (onLogMessage)
            juce::MessageManager::callAsync ([callback = onLogMessage, message]
            {
                callback (message);
            });
    }

juce::String SuperColliderHost::checkScript (const juce::String& script, const juce::String& sclangPath)
    {
        const juce::ScopedLock lock (hostLock);

        if (! ensureBridgeRunningLocked (sclangPath))
            return {};

        const auto checkId = juce::String::toHexString (juce::Time::currentTimeMillis())
                           + "-" + juce::String (++commandSerial);
        const auto scriptFile = makeTempScript ("check-" + checkId, script);
        auto resultDir = bridgeDirectory.getChildFile ("check-results");
        resultDir.createDirectory();
        auto resultFile = resultDir.getChildFile (checkId + ".txt");
        resultFile.deleteFile();
        writeCommand ("~markovCheck.(" + scStringLiteral (checkId) + ", "
                    + scStringLiteral (scriptFile.getFullPathName()) + ", "
                    + scStringLiteral (resultFile.getFullPathName()) + ");\n");
        appendRuntimeLog ("check requested: " + checkId);
        return checkId;
    }

juce::String SuperColliderHost::readCheckResult (const juce::String& checkId) const
    {
        if (checkId.isEmpty())
            return {};

        auto resultFile = bridgeDirectory.getChildFile ("check-results").getChildFile (checkId + ".txt");
        if (! resultFile.existsAsFile())
            return {};

        return resultFile.loadFileAsString().trim();
    }

void SuperColliderHost::setStatus(const juce::String& status)
    {
        if (currentStatus == status)
            return;

        currentStatus = status;
        if (onStatusChanged)
            juce::MessageManager::callAsync ([callback = onStatusChanged, status]
            {
                callback (status);
            });
    }
