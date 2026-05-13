#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_osc/juce_osc.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{
constexpr int maxStateCount = 12;
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
    double serverLatencySeconds = 0.004;
    int hardwareBufferSize = 64;
    double crossfadeSeconds = 0.006;
};

constexpr AudioProfile getAudioProfile()
{
    if constexpr (activeLatencyProfile == LatencyProfile::stable)
        return { 0.015, 64, 0.010 };
    else if constexpr (activeLatencyProfile == LatencyProfile::ultra)
        return { 0.002, 32, 0.003 };
    else
        return { 0.004, 64, 0.006 };
}

juce::Colour backgroundTop() { return juce::Colour (0xff111318); }
juce::Colour backgroundBottom() { return juce::Colour (0xff20242b); }
juce::Colour ink() { return juce::Colour (0xfff2efe7); }
juce::Colour mutedInk() { return juce::Colour (0xffaeb5bd); }
juce::Colour accentA() { return juce::Colour (0xffffc857); }
juce::Colour accentB() { return juce::Colour (0xff52d1dc); }
juce::Colour accentC() { return juce::Colour (0xfff76f8e); }
juce::Colour selectedFill() { return juce::Colour (0xff3a3320); }
juce::Colour inspectedFill() { return juce::Colour (0xff1c3139); }
juce::Colour selectedRow() { return juce::Colour (0xff34313a); }

juce::Colour paletteColour (int index)
{
    static constexpr juce::uint32 colours[] =
    {
        0xff52d1dc, 0xffffc857, 0xfff76f8e, 0xff7bd88f,
        0xffb48cff, 0xffff9f68, 0xff64b5f6, 0xfff06292
    };

    return juce::Colour (colours[static_cast<size_t> (index) % std::size (colours)]);
}

juce::String defaultScriptFor (int stateIndex, int laneIndex)
{
    auto freq = 160 + (stateIndex * 53) + (laneIndex * 37);
    return "(\n"
           "{ |gate=1, fade=0.006|\n"
           "    var pulse = Impulse.kr(" + juce::String (4 + laneIndex) + ");\n"
           "    var env = Decay2.kr(pulse, 0.01, 0.22);\n"
           "    var active = Lag.kr(gate, fade);\n"
           "    var tone = SinOsc.ar(" + juce::String (freq) + " * [1, 1.005]);\n"
           "    var body = LFTri.ar(" + juce::String (freq / 2) + " * [1.002, 1]);\n"
           "    (tone + (body * 0.35)) * env * active * 0.16;\n"
           "}.play;\n"
           ")\n";
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
} // namespace

struct Lane
{
    juce::String id;
    juce::String name;
    juce::String script;
    bool playing = false;
    int preparedBridge = -1;
};

struct LaneSnapshot
{
    juce::String id;
    juce::String name;
    juce::String script;
};

struct State
{
    int index = 0;
    juce::String name;
    std::vector<Lane> lanes;
};

struct Rule
{
    int from = 0;
    int to = 0;
    float weight = 1.0f;
};

class MachineModel
{
public:
    explicit MachineModel (juce::String machineIdToUse = "root", juce::String lanePrefixToUse = "")
        : machineId (std::move (machineIdToUse)), lanePrefix (std::move (lanePrefixToUse))
    {
        setStateCount (5);
        regenerateRingRules();
    }

    ~MachineModel() = default;

    void setStateCount (int newCount)
    {
        newCount = juce::jlimit (1, maxStateCount, newCount);
        const auto oldSize = static_cast<int> (states.size());
        states.resize (static_cast<size_t> (newCount));
        childMachines.resize (static_cast<size_t> (newCount));

        for (int i = oldSize; i < newCount; ++i)
        {
            states[static_cast<size_t> (i)].index = i;
            states[static_cast<size_t> (i)].name = "State " + juce::String (i + 1);
            states[static_cast<size_t> (i)].lanes.push_back (
                { makeLaneId (i, 0), "Lane 1", defaultScriptFor (i, 0), false });
        }

        for (int i = 0; i < newCount; ++i)
            states[static_cast<size_t> (i)].index = i;

        rules.erase (std::remove_if (rules.begin(), rules.end(), [newCount] (const Rule& rule)
        {
            return rule.from >= newCount || rule.to >= newCount;
        }), rules.end());

        selectedState = juce::jlimit (0, newCount - 1, selectedState);
        selectedLane = juce::jlimit (0, getLaneCount (selectedState) - 1, selectedLane);
        entryState = juce::jlimit (0, newCount - 1, entryState);
    }

    int getStateCount() const { return static_cast<int> (states.size()); }
    int getLaneCount (int stateIndex) const { return static_cast<int> (states[static_cast<size_t> (stateIndex)].lanes.size()); }

    State& state (int index) { return states[static_cast<size_t> (index)]; }
    const State& state (int index) const { return states[static_cast<size_t> (index)]; }

    Lane& selectedLaneRef()
    {
        return states[static_cast<size_t> (selectedState)].lanes[static_cast<size_t> (selectedLane)];
    }

    void addLaneToSelectedState()
    {
        auto& s = state (selectedState);
        const auto laneIndex = static_cast<int> (s.lanes.size());
        s.lanes.push_back ({ makeLaneId (selectedState, laneIndex),
                             "Lane " + juce::String (laneIndex + 1),
                             defaultScriptFor (selectedState, laneIndex),
                             false });
        selectedLane = laneIndex;
    }

    void removeSelectedLane()
    {
        auto& s = state (selectedState);
        if (s.lanes.size() <= 1)
            return;

        s.lanes.erase (s.lanes.begin() + selectedLane);
        selectedLane = juce::jlimit (0, static_cast<int> (s.lanes.size()) - 1, selectedLane);
    }

    void regenerateRingRules()
    {
        rules.clear();
        const auto count = getStateCount();
        for (int i = 0; i < count; ++i)
            rules.push_back ({ i, (i + 1) % count, 1.0f });
    }

    juce::String makeLaneId (int stateIndex, int laneIndex) const
    {
        return lanePrefix + "s" + juce::String (stateIndex) + "-l" + juce::String (laneIndex);
    }

    bool hasChildMachine (int stateIndex) const
    {
        return childMachines[static_cast<size_t> (stateIndex)] != nullptr;
    }

    MachineModel* childMachine (int stateIndex)
    {
        return childMachines[static_cast<size_t> (stateIndex)].get();
    }

    const MachineModel* childMachine (int stateIndex) const
    {
        return childMachines[static_cast<size_t> (stateIndex)].get();
    }

    MachineModel& addChildToSelectedState()
    {
        auto childId = machineId + "_state" + juce::String (selectedState) + "_child";
        auto childPrefix = lanePrefix + "n" + juce::String (selectedState) + "-";
        childMachines[static_cast<size_t> (selectedState)] = std::make_unique<MachineModel> (childId, childPrefix);
        return *childMachines[static_cast<size_t> (selectedState)];
    }

    void removeChildFromSelectedState()
    {
        childMachines[static_cast<size_t> (selectedState)] = nullptr;
    }

    std::vector<State> states;
    std::vector<std::unique_ptr<MachineModel>> childMachines;
    std::vector<Rule> rules;
    juce::String machineId;
    juce::String lanePrefix;
    int selectedState = 0;
    int selectedLane = 0;
    int entryState = 0;
    int stepsSinceEntry = 0;
};

class SuperColliderHost
{
public:
    std::function<void (const juce::String&)> onLogMessage;
    std::function<void (const juce::String&)> onStatusChanged;

    ~SuperColliderHost()
    {
        shutdown();
    }

    void play (Lane& lane, const juce::String& sclangPath)
    {
        appendRuntimeLog ("play requested: " + lane.id);
        stop (lane);

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

    bool prepare (Lane& lane, const juce::String& sclangPath)
    {
        if (prepareData ({ lane.id, lane.name, lane.script }, sclangPath) < 0)
            return false;

        lane.preparedBridge = bridgeGeneration;
        return true;
    }

    int prepareData (const LaneSnapshot& lane, const juce::String& sclangPath)
    {
        appendRuntimeLog ("prepare requested: " + lane.id);
        const juce::ScopedLock lock (hostLock);

        if (! ensureBridgeRunningLocked (sclangPath))
            return -1;

        auto scriptFile = makeTempScript (lane.id, lane.script);

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
        addLog ("Loaded " + lane.name);
        setStatus ("Audio ready");
        return bridgeGeneration;
    }

    void stop (Lane& lane)
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
            sendStopCommand (lane.id);

        lane.playing = false;
        setStatus (bridgeProcess != nullptr && bridgeProcess->isRunning() ? "Audio ready" : "Audio offline");
    }

    void stopAll (MachineModel& model)
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
            sendStopAllCommand();

        markAllLanesStopped (model);
        addLog ("Stopped all Markov lanes");
        setStatus (bridgeProcess != nullptr && bridgeProcess->isRunning() ? "Audio ready" : "Audio offline");
    }

    bool isReady() const
    {
        const juce::ScopedLock lock (hostLock);
        return bridgeProcess != nullptr && bridgeProcess->isRunning();
    }

    int getBridgeGeneration() const
    {
        const juce::ScopedLock lock (hostLock);
        return bridgeGeneration;
    }

    void panic (MachineModel& model)
    {
        const juce::ScopedLock lock (hostLock);

        if (bridgeProcess != nullptr && bridgeProcess->isRunning())
        {
            if (oscConnected)
                oscSender.send ("/markov/panic");

            writeCommand ("~markovStopAll.();\n");
        }

        markAllLanesStopped (model);

        addLog ("Panic: freed all active SuperCollider lane objects");
        setStatus (bridgeProcess != nullptr && bridgeProcess->isRunning() ? "Audio ready" : "Audio offline");
    }

    void configureMachine (const MachineModel& model)
    {
        juce::ignoreUnused (model);
        addLog ("FSM prepared");
    }

    void runMachine (int startState, double rateHz)
    {
        juce::ignoreUnused (startState, rateHz);
    }

    void pauseMachine()
    {
    }

    void stepMachine()
    {
    }

    void testTone (const juce::String& sclangPath)
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

private:
    bool ensureBridgeRunning (const juce::String& sclangPath)
    {
        const juce::ScopedLock lock (hostLock);
        return ensureBridgeRunningLocked (sclangPath);
    }

    bool ensureBridgeRunningLocked (const juce::String& sclangPath)
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
        return true;
    }

    juce::String resolveSclangExecutable (const juce::String& sclangPath) const
    {
        if (sclangPath.trim().isNotEmpty())
            return sclangPath.trim();

        auto bundledMacPath = juce::File ("/Applications/SuperCollider.app/Contents/MacOS/sclang");
        if (bundledMacPath.existsAsFile())
            return bundledMacPath.getFullPathName();

        return "sclang";
    }

    juce::String makeBridgeScript() const
    {
        const auto commandPath = scStringLiteral (commandDirectory.getFullPathName());
        constexpr auto profile = getAudioProfile();
        const auto latency = juce::String (profile.serverLatencySeconds, 4);
        const auto bufferSize = juce::String (profile.hardwareBufferSize);
        const auto crossfade = juce::String (enableHiddenCrossfades ? profile.crossfadeSeconds : 0.0, 4);

        return "(\n"
               "Server.default.latency = " + latency + ";\n"
               "s.options.hardwareBufferSize = " + bufferSize + ";\n"
               "s.options.numOutputBusChannels = 2;\n"
               "s.options.memSize = 262144;\n"
               "s.boot;\n"
               "~markovFade = " + crossfade + ";\n"
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
               "~markovPrograms = IdentityDictionary.new;\n"
               "~markovLoad = { |key, path|\n"
               "    var file, source;\n"
               "    file = File(path, \"r\");\n"
               "    if (file.isOpen.not) { (\"Markov could not open lane: \" ++ path).warn; ^nil };\n"
               "    source = file.readAllString;\n"
               "    file.close;\n"
               "    ~markovPrograms[key] = (\"{ \" ++ source ++ \" }\").interpret;\n"
               "};\n"
               "~markovStop = { |key|\n"
               "    var obj = ~markovObjects[key];\n"
               "    if (obj.notNil) {\n"
               "        if (obj.respondsTo(\\set)) { obj.set(\\gate, 0, \\fade, ~markovFade) } {\n"
               "            if (obj.respondsTo(\\run)) { obj.run(false) } {\n"
               "                if (obj.respondsTo(\\stop)) { obj.stop };\n"
               "            };\n"
               "        };\n"
               "    };\n"
               "    ~markovObjects.removeAt(key);\n"
               "};\n"
               "~markovStopAll = {\n"
               "    ~markovObjects.keys.copy.do { |key| ~markovStop.(key) };\n"
               "    s.freeAll;\n"
               "    ~markovObjects = IdentityDictionary.new;\n"
               "};\n"
               "~markovPlay = { |key|\n"
               "    ~markovWhenReady.({\n"
               "        var obj = ~markovObjects[key];\n"
               "        var program = ~markovPrograms[key];\n"
               "        if (obj.notNil) {\n"
               "            if (obj.respondsTo(\\set)) { obj.set(\\gate, 1, \\fade, ~markovFade) } {\n"
               "                if (obj.respondsTo(\\run)) { obj.run(true) };\n"
               "            };\n"
               "        } {\n"
               "            if (program.notNil) { ~markovObjects[key] = program.value; };\n"
               "        };\n"
               "    });\n"
               "};\n"
               "OSCdef(\\markovLoad, { |msg| ~markovLoad.(msg[1].asString.asSymbol, msg[2].asString); }, '/markov/load');\n"
               "OSCdef(\\markovPlay, { |msg| ~markovPlay.(msg[1].asString.asSymbol); }, '/markov/play');\n"
               "OSCdef(\\markovStop, { |msg| ~markovStop.(msg[1].asString.asSymbol); }, '/markov/stop');\n"
               "OSCdef(\\markovStopAll, { ~markovStopAll.(); }, '/markov/stopAll');\n"
               "OSCdef(\\markovPanic, { ~markovStopAll.(); }, '/markov/panic');\n"
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

    void sendLoadCommand (const juce::String& laneId, const juce::String& scriptPath)
    {
        // Load is idempotent, so keep a file-backed copy as startup insurance.
        // Early OSC can arrive before sclang has installed OSCdefs.
        writeCommand ("~markovLoad.(" + scSymbolLiteral (laneId) + ", " + scStringLiteral (scriptPath) + ");\n");

        if (oscConnected)
            oscSender.send ("/markov/load", laneId, scriptPath);
    }

    void sendPlayCommand (const juce::String& laneId)
    {
        writeCommand ("~markovPlay.(" + scSymbolLiteral (laneId) + ");\n");

        if (oscConnected)
            oscSender.send ("/markov/play", laneId);
    }

    void sendStopCommand (const juce::String& laneId)
    {
        writeCommand ("~markovStop.(" + scSymbolLiteral (laneId) + ");\n");

        if (oscConnected)
            oscSender.send ("/markov/stop", laneId);
    }

    void sendStopAllCommand()
    {
        writeCommand ("~markovStopAll.();\n");

        if (oscConnected)
            oscSender.send ("/markov/stopAll");
    }

    void sendClearMachineCommand()
    {
        writeCommand ("~markovClearMachine.();\n");

        if (oscConnected)
            oscSender.send ("/markov/clear");
    }

    void writeCommand (const juce::String& command)
    {
        if (! commandDirectory.exists())
            return;

        auto serial = juce::String (++commandSerial).paddedLeft ('0', 8);
        auto file = commandDirectory.getChildFile ("command-"
                    + juce::String::toHexString (juce::Time::currentTimeMillis())
                    + "-" + serial + ".scd");
        file.replaceWithText (command);
    }

    void shutdown()
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
                oscSender.disconnect();
                oscConnected = false;
            }

            if (bridgeProcess->isRunning())
                bridgeProcess->kill();

            bridgeProcess = nullptr;
        }

        tempScriptStorage.clear (true);
        tempScripts.clear();

        // Keep the bridge folder around so sclang.log is available after failures.

        setStatus ("Audio offline");
    }

    static void markAllLanesStopped (MachineModel& model)
    {
        for (auto& state : model.states)
        {
            for (auto& lane : state.lanes)
                lane.playing = false;

            if (auto* child = model.childMachine (state.index))
                markAllLanesStopped (*child);
        }
    }

    void startLogReader()
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

    void addLog (const juce::String& message)
    {
        if (message.trim().isEmpty())
            return;

        if (onLogMessage)
            juce::MessageManager::callAsync ([callback = onLogMessage, message]
            {
                callback (message);
            });
    }

    void setStatus (const juce::String& status)
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

    mutable juce::CriticalSection hostLock;
    std::unique_ptr<juce::ChildProcess> bridgeProcess;
    juce::OSCSender oscSender;
    juce::File bridgeDirectory;
    juce::File commandDirectory;
    juce::File bridgeLogFile;
    juce::String currentExecutable;
    int commandSerial = 0;
    int bridgeGeneration = 0;
    juce::int64 bridgeStartedAtMs = 0;
    bool oscConnected = false;
    std::atomic<bool> logReaderShouldRun { false };
    std::thread logReader;
    juce::String currentStatus { "Audio offline" };
    juce::OwnedArray<juce::File> tempScriptStorage;
    juce::HashMap<juce::String, juce::File*> tempScripts;
};

class GraphComponent final : public juce::Component,
                             private juce::Timer
{
public:
    std::function<void (int)> onStateChosen;
    std::function<void (int)> onNestedBadgeChosen;
    std::function<void (int, int)> onNestedStateChosen;
    std::function<void (int, int)> onNestedStateCountChanged;

    void setInspectedMachine (MachineModel* inspected)
    {
        inspectedMachine = inspected;
        repaint();
    }

    explicit GraphComponent (MachineModel& machineToUse) : machine (&machineToUse)
    {
        setWantsKeyboardFocus (true);
        startTimerHz (30);
    }

    void setMachine (MachineModel& machineToUse)
    {
        machine = &machineToUse;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        juce::ColourGradient bg (backgroundTop(), bounds.getTopLeft(), backgroundBottom(), bounds.getBottomRight(), false);
        g.setGradientFill (bg);
        g.fillRoundedRectangle (bounds.reduced (2.0f), 8.0f);

        layoutStates();
        drawRules (g);
        drawStates (g);
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        for (int i = 0; i < static_cast<int> (statePositions.size()); ++i)
        {
            if (auto* child = machine->childMachine (i))
            {
                const auto childState = hitTestNestedState (*child, statePositions[static_cast<size_t> (i)], event.position);
                if (childState >= 0)
                {
                    if (onNestedStateChosen)
                        onNestedStateChosen (i, childState);
                    return;
                }
            }
        }

        for (int i = 0; i < static_cast<int> (statePositions.size()); ++i)
        {
            if (machine->childMachine (i) != nullptr
                && getNestedBadgeBounds (*machine->childMachine (i), statePositions[static_cast<size_t> (i)]).contains (event.position))
            {
                if (onNestedBadgeChosen)
                    onNestedBadgeChosen (i);
                return;
            }
        }

        for (int i = 0; i < static_cast<int> (statePositions.size()); ++i)
        {
            if (statePositions[static_cast<size_t> (i)].getDistanceFrom (event.position) < stateRadius)
            {
                machine->selectedState = i;
                machine->selectedLane = 0;
                if (onStateChosen)
                    onStateChosen (i);
                repaint();
                return;
            }
        }
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        for (int i = 0; i < static_cast<int> (statePositions.size()); ++i)
        {
            if (machine->childMachine (i) != nullptr
                && getNestedBadgeBounds (*machine->childMachine (i), statePositions[static_cast<size_t> (i)]).contains (event.position))
            {
                startNestedStateCountEdit (i, getNestedBadgeBounds (*machine->childMachine (i), statePositions[static_cast<size_t> (i)]));
                return;
            }
        }
    }

private:
    void layoutStates()
    {
        const auto count = machine->getStateCount();
        statePositions.resize (static_cast<size_t> (count));

        stateRadius = juce::jmap (static_cast<float> (count), 1.0f, static_cast<float> (maxStateCount), 54.0f, 34.0f);
        stateRadius = juce::jlimit (34.0f, 54.0f, stateRadius);

        const auto outerMargin = getOuterNodeExtent() + 20.0f;
        auto area = getLocalBounds().toFloat().reduced (outerMargin, outerMargin * 0.78f);
        const auto maxLayoutWidth = area.getHeight() * 3.35f;
        if (area.getWidth() > maxLayoutWidth)
            area = area.withSizeKeepingCentre (maxLayoutWidth, area.getHeight());

        auto centre = area.getCentre();
        auto rx = area.getWidth() * 0.48f;
        auto ry = area.getHeight() * 0.44f;

        for (int i = 0; i < count; ++i)
        {
            auto angle = (juce::MathConstants<float>::twoPi * static_cast<float> (i) / static_cast<float> (count))
                       - juce::MathConstants<float>::halfPi;
            statePositions[static_cast<size_t> (i)] = { centre.x + std::cos (angle) * rx,
                                                        centre.y + std::sin (angle) * ry };
        }

        relaxStatePositions (area);
    }

    void relaxStatePositions (juce::Rectangle<float> area)
    {
        const auto count = static_cast<int> (statePositions.size());
        if (count < 2)
            return;

        constexpr int iterations = 90;
        for (int pass = 0; pass < iterations; ++pass)
        {
            for (int a = 0; a < count; ++a)
            {
                for (int b = a + 1; b < count; ++b)
                {
                    auto& pa = statePositions[static_cast<size_t> (a)];
                    auto& pb = statePositions[static_cast<size_t> (b)];
                    auto delta = pb - pa;
                    auto distance = std::sqrt (delta.x * delta.x + delta.y * delta.y);

                    if (distance < 0.001f)
                    {
                        delta = { 1.0f, 0.0f };
                        distance = 1.0f;
                    }

                    const auto minDistance = getNodeClearance (a) + getNodeClearance (b);
                    if (distance >= minDistance)
                        continue;

                    delta.x /= distance;
                    delta.y /= distance;

                    const auto push = (minDistance - distance) * 0.52f;
                    pa -= delta * push;
                    pb += delta * push;
                }
            }

            for (int i = 0; i < count; ++i)
            {
                auto& p = statePositions[static_cast<size_t> (i)];
                const auto extent = getNodeClearance (i);
                p.x = juce::jlimit (area.getX() + extent, area.getRight() - extent, p.x);
                p.y = juce::jlimit (area.getY() + extent, area.getBottom() - extent, p.y);
            }
        }
    }

    void drawRules (juce::Graphics& g)
    {
        for (const auto& rule : machine->rules)
        {
            if (rule.from >= static_cast<int> (statePositions.size()) || rule.to >= static_cast<int> (statePositions.size()))
                continue;

            auto fromCentre = statePositions[static_cast<size_t> (rule.from)];
            auto toCentre = statePositions[static_cast<size_t> (rule.to)];
            auto direction = toCentre - fromCentre;
            const auto length = juce::jmax (1.0f, std::sqrt (direction.x * direction.x + direction.y * direction.y));
            direction.x /= length;
            direction.y /= length;
            auto from = fromCentre + direction * (stateRadius * 1.05f);
            auto to = toCentre - direction * (stateRadius * 1.05f);
            auto mid = (from + to) * 0.5f;
            auto centre = getLocalBounds().toFloat().getCentre();
            auto control = mid + (mid - centre) * 0.18f;

            juce::Path curve;
            curve.startNewSubPath (from);
            curve.quadraticTo (control, to);

            g.setColour (accentB().withAlpha (0.22f + juce::jlimit (0.0f, 0.4f, rule.weight * 0.08f)));
            g.strokePath (curve, juce::PathStrokeType (2.0f + rule.weight));

            auto arrowPoint = from + (to - from) * 0.78f;
            g.setColour (accentB().withAlpha (0.72f));
            g.fillEllipse (arrowPoint.x - 3.0f, arrowPoint.y - 3.0f, 6.0f, 6.0f);
        }
    }

    void drawStates (juce::Graphics& g)
    {
        for (int i = 0; i < machine->getStateCount(); ++i)
        {
            auto p = statePositions[static_cast<size_t> (i)];
            auto selected = i == machine->selectedState;
            auto laneCount = machine->getLaneCount (i);

            juce::ColourGradient glow (selected ? accentA().withAlpha (0.55f) : accentC().withAlpha (0.18f),
                                       p.translated (-stateRadius, -stateRadius),
                                       juce::Colours::transparentBlack,
                                       p.translated (stateRadius, stateRadius),
                                       true);
            g.setGradientFill (glow);
            g.fillEllipse (p.x - stateRadius * 1.55f, p.y - stateRadius * 1.55f, stateRadius * 3.1f, stateRadius * 3.1f);

            g.setColour (selected ? selectedFill() : juce::Colour (0xff252a31));
            g.fillEllipse (p.x - stateRadius, p.y - stateRadius, stateRadius * 2.0f, stateRadius * 2.0f);

            g.setColour ((selected ? accentA() : mutedInk()).withAlpha (0.95f));
            g.drawEllipse (p.x - stateRadius, p.y - stateRadius, stateRadius * 2.0f, stateRadius * 2.0f, selected ? 3.0f : 1.5f);

            const auto* child = machine->childMachine (i);
            if (child != nullptr)
            {
                auto nestedRadius = stateRadius + 7.0f;
                g.setColour ((selected ? accentC() : accentB()).withAlpha (selected ? 0.95f : 0.62f));
                g.drawEllipse (p.x - nestedRadius, p.y - nestedRadius, nestedRadius * 2.0f, nestedRadius * 2.0f, 2.0f);
                drawNestedIndicator (g, *child, p, selected, child == inspectedMachine);
            }

            g.setColour (ink());
            g.setFont (juce::FontOptions (stateRadius < 40.0f ? 13.0f : 16.0f, juce::Font::bold));
            g.drawFittedText (machine->state (i).name, juce::Rectangle<int> (static_cast<int> (p.x - stateRadius * 0.95f),
                                                                            static_cast<int> (p.y - stateRadius * 0.38f),
                                                                            static_cast<int> (stateRadius * 1.9f), 22),
                              juce::Justification::centred, 1);

            g.setColour (mutedInk());
            g.setFont (juce::FontOptions (stateRadius < 40.0f ? 10.0f : 12.0f));
            g.drawFittedText (juce::String (laneCount) + (laneCount == 1 ? " lane" : " lanes"),
                              juce::Rectangle<int> (static_cast<int> (p.x - stateRadius * 0.9f),
                                                    static_cast<int> (p.y + stateRadius * 0.10f),
                                                    static_cast<int> (stateRadius * 1.8f), 18),
                              juce::Justification::centred, 1);
        }
    }

    void drawNestedIndicator (juce::Graphics& g, const MachineModel& child, juce::Point<float> parentCentre, bool parentSelected, bool childInspected)
    {
        const auto childCount = child.getStateCount();
        if (childCount <= 0)
            return;

        const auto orbitRadius = getNestedOrbitRadius();
        const auto nodeRadius = getNestedNodeRadius (childCount);
        std::vector<juce::Point<float>> childPoints;
        childPoints.reserve (static_cast<size_t> (childCount));

        for (int j = 0; j < childCount; ++j)
        {
            const auto angle = (juce::MathConstants<float>::twoPi * static_cast<float> (j) / static_cast<float> (childCount))
                             - juce::MathConstants<float>::halfPi;
            childPoints.push_back ({ parentCentre.x + std::cos (angle) * orbitRadius,
                                     parentCentre.y + std::sin (angle) * orbitRadius });
        }

        g.setColour ((childInspected ? inspectedFill() : juce::Colour (0xff11161d)).withAlpha (childInspected ? 0.88f : 0.78f));
        g.fillEllipse (parentCentre.x - orbitRadius - 4.0f, parentCentre.y - orbitRadius - 4.0f,
                       (orbitRadius + 4.0f) * 2.0f, (orbitRadius + 4.0f) * 2.0f);

        const auto ringColour = childInspected ? accentB() : juce::Colour (0xff6f7b88);
        g.setColour (ringColour.withAlpha (childInspected ? 0.9f : (parentSelected ? 0.58f : 0.38f)));
        g.drawEllipse (parentCentre.x - orbitRadius, parentCentre.y - orbitRadius,
                       orbitRadius * 2.0f, orbitRadius * 2.0f, childInspected ? 2.4f : (parentSelected ? 2.0f : 1.3f));

        for (const auto& rule : child.rules)
        {
            if (rule.from < 0 || rule.to < 0 || rule.from >= childCount || rule.to >= childCount)
                continue;

            auto from = childPoints[static_cast<size_t> (rule.from)];
            auto to = childPoints[static_cast<size_t> (rule.to)];
            auto mid = (from + to) * 0.5f;
            auto control = mid + ((mid - parentCentre) * 0.14f);
            juce::Path path;
            path.startNewSubPath (from);
            path.quadraticTo (control, to);
            g.setColour ((parentSelected ? accentA() : accentB()).withAlpha (0.18f));
            g.strokePath (path, juce::PathStrokeType (1.0f));
        }

        for (int j = 0; j < childCount; ++j)
        {
            const auto point = childPoints[static_cast<size_t> (j)];
            const auto selected = j == child.selectedState;
            const auto stateColour = paletteColour (j);
            g.setColour ((selected && childInspected ? stateColour.brighter (0.35f) : stateColour).withAlpha (selected ? 0.98f : 0.84f));
            g.fillEllipse (point.x - nodeRadius, point.y - nodeRadius, nodeRadius * 2.0f, nodeRadius * 2.0f);
            g.setColour ((selected ? ink() : juce::Colour (0xff101318)).withAlpha (selected ? 0.82f : 0.92f));
            g.drawEllipse (point.x - nodeRadius, point.y - nodeRadius, nodeRadius * 2.0f, nodeRadius * 2.0f, selected ? 1.4f : 1.0f);
        }

        auto badge = getNestedBadgeBounds (child, parentCentre);
        g.setColour (juce::Colour (0xff101318).withAlpha (0.96f));
        g.fillRoundedRectangle (badge, 6.0f);
        g.setColour ((childInspected ? accentB() : (parentSelected ? accentA() : accentC())).withAlpha (0.88f));
        g.drawRoundedRectangle (badge, 6.0f, 1.1f);
        g.setColour (ink());
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawFittedText (juce::String (childCount), badge.toNearestInt(), juce::Justification::centred, 1);
    }

    int hitTestNestedState (const MachineModel& child, juce::Point<float> parentCentre, juce::Point<float> pointer) const
    {
        const auto childCount = child.getStateCount();
        if (childCount <= 0)
            return -1;

        const auto orbitRadius = getNestedOrbitRadius();
        const auto hitRadius = juce::jmax (7.0f, getNestedNodeRadius (childCount) + 5.0f);
        for (int j = 0; j < childCount; ++j)
        {
            const auto angle = (juce::MathConstants<float>::twoPi * static_cast<float> (j) / static_cast<float> (childCount))
                             - juce::MathConstants<float>::halfPi;
            juce::Point<float> point { parentCentre.x + std::cos (angle) * orbitRadius,
                                       parentCentre.y + std::sin (angle) * orbitRadius };

            if (point.getDistanceFrom (pointer) <= hitRadius)
                return j;
        }

        return -1;
    }

    juce::Rectangle<float> getNestedBadgeBounds (const MachineModel& child, juce::Point<float> parentCentre) const
    {
        const auto badgeWidth = stateRadius < 40.0f ? 24.0f : 28.0f;
        const auto badgeHeight = stateRadius < 40.0f ? 18.0f : 20.0f;
        const auto badgeRadius = getNestedOrbitRadius() + badgeWidth * 0.66f + 7.0f;
        juce::Rectangle<float> best;
        auto bestScore = -1.0f;

        for (int candidate = 0; candidate < 16; ++candidate)
        {
            const auto angle = (-juce::MathConstants<float>::halfPi)
                             + (juce::MathConstants<float>::twoPi * (static_cast<float> (candidate) + 0.5f) / 16.0f);
            auto centre = juce::Point<float> { parentCentre.x + std::cos (angle) * badgeRadius,
                                               parentCentre.y + std::sin (angle) * badgeRadius };
            auto badge = juce::Rectangle<float> (0.0f, 0.0f, badgeWidth, badgeHeight).withCentre (centre);
            auto score = getBadgeClearanceScore (child, parentCentre, badge);

            if (score > bestScore)
            {
                bestScore = score;
                best = badge;
            }
        }

        return best;
    }

    float getBadgeClearanceScore (const MachineModel& child, juce::Point<float> parentCentre, juce::Rectangle<float> badge) const
    {
        const auto childCount = child.getStateCount();
        const auto orbitRadius = getNestedOrbitRadius();
        const auto badgeCentre = badge.getCentre();
        auto score = std::numeric_limits<float>::max();

        for (int j = 0; j < childCount; ++j)
        {
            const auto angle = (juce::MathConstants<float>::twoPi * static_cast<float> (j) / static_cast<float> (childCount))
                             - juce::MathConstants<float>::halfPi;
            auto point = juce::Point<float> { parentCentre.x + std::cos (angle) * orbitRadius,
                                              parentCentre.y + std::sin (angle) * orbitRadius };
            score = juce::jmin (score, point.getDistanceFrom (badgeCentre));
        }

        auto graphBounds = getLocalBounds().toFloat().reduced (8.0f);
        if (! graphBounds.contains (badge))
            score -= 200.0f;

        return score;
    }

    float getOuterNodeExtent() const
    {
        for (int i = 0; i < machine->getStateCount(); ++i)
            if (machine->childMachine (i) != nullptr)
                return getNestedOrbitRadius() + 12.0f;

        return stateRadius * 1.55f;
    }

    float getNodeClearance (int stateIndex) const
    {
        auto extent = stateRadius * 1.55f;
        if (machine->childMachine (stateIndex) != nullptr)
            extent = getNestedOrbitRadius() + 11.0f;

        return extent + 8.0f;
    }

    float getNestedOrbitRadius() const
    {
        return stateRadius + juce::jmap (stateRadius, 34.0f, 54.0f, 10.0f, 16.0f);
    }

    float getNestedNodeRadius (int childCount) const
    {
        return childCount > 7 || stateRadius < 40.0f ? 2.5f : 3.7f;
    }

    void startNestedStateCountEdit (int parentStateIndex, juce::Rectangle<float> badgeBounds)
    {
        auto* child = machine->childMachine (parentStateIndex);
        if (child == nullptr)
            return;

        finishNestedStateCountEdit (false);
        editingNestedParentState = parentStateIndex;
        auto safeThis = juce::Component::SafePointer<GraphComponent> (this);

        nestedCountEditor = std::make_unique<juce::TextEditor>();
        nestedCountEditor->setText (juce::String (child->getStateCount()), false);
        nestedCountEditor->setSelectAllWhenFocused (true);
        nestedCountEditor->setInputRestrictions (2, "0123456789");
        nestedCountEditor->setJustification (juce::Justification::centred);
        nestedCountEditor->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff101318));
        nestedCountEditor->setColour (juce::TextEditor::textColourId, ink());
        nestedCountEditor->setColour (juce::TextEditor::highlightColourId, accentA().withAlpha (0.35f));
        nestedCountEditor->setColour (juce::TextEditor::outlineColourId, accentA());
        nestedCountEditor->onReturnKey = [safeThis] { if (safeThis != nullptr) safeThis->finishNestedStateCountEdit (true); };
        nestedCountEditor->onEscapeKey = [safeThis] { if (safeThis != nullptr) safeThis->finishNestedStateCountEdit (false); };
        nestedCountEditor->onFocusLost = [safeThis] { if (safeThis != nullptr) safeThis->finishNestedStateCountEdit (true); };

        addAndMakeVisible (*nestedCountEditor);
        nestedCountEditor->setBounds (badgeBounds.expanded (4.0f, 3.0f).toNearestInt());
        nestedCountEditor->grabKeyboardFocus();
        nestedCountEditor->selectAll();
    }

    void finishNestedStateCountEdit (bool commit)
    {
        if (nestedCountEditor == nullptr)
            return;

        const auto parentStateIndex = editingNestedParentState;
        const auto text = nestedCountEditor->getText();

        auto editor = std::move (nestedCountEditor);
        editingNestedParentState = -1;
        removeChildComponent (editor.get());
        editor.reset();

        if (! commit || parentStateIndex < 0)
            return;

        const auto newCount = juce::jlimit (1, maxStateCount, text.getIntValue());
        if (onNestedStateCountChanged)
            onNestedStateCountChanged (parentStateIndex, newCount);
    }

    void timerCallback() override { repaint(); }

    MachineModel* machine;
    MachineModel* inspectedMachine = nullptr;
    std::vector<juce::Point<float>> statePositions;
    std::unique_ptr<juce::TextEditor> nestedCountEditor;
    float stateRadius = 48.0f;
    int editingNestedParentState = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GraphComponent)
};

class RuleListComponent final : public juce::Component
{
public:
    explicit RuleListComponent (MachineModel& modelToUse) : machine (&modelToUse)
    {
        addAndMakeVisible (fromBox);
        addAndMakeVisible (toBox);
        addAndMakeVisible (weightSlider);
        addAndMakeVisible (addButton);
        addAndMakeVisible (updateButton);
        addAndMakeVisible (removeButton);
        addAndMakeVisible (ringButton);

        weightSlider.setRange (0.1, 5.0, 0.1);
        weightSlider.setValue (1.0);
        weightSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 22);

        addButton.setButtonText ("Add");
        updateButton.setButtonText ("Save");
        removeButton.setButtonText ("Delete");
        ringButton.setButtonText ("Rules");

        addButton.onClick = [this]
        {
            addRuleFromControls();
        };

        updateButton.onClick = [this]
        {
            updateSelectedRule();
        };

        removeButton.onClick = [this]
        {
            removeSelectedRule();
        };

        ringButton.onClick = [this]
        {
            machine->regenerateRingRules();
            selectedRuleIndex = -1;
            refreshChoices();
            if (onRulesChanged)
                onRulesChanged();
        };

        refreshChoices();
    }

    void setMachine (MachineModel& modelToUse)
    {
        machine = &modelToUse;
        selectedRuleIndex = -1;
        refreshChoices();
        repaint();
    }

    std::function<void()> onRulesChanged;

    void refreshChoices()
    {
        fromBox.clear();
        toBox.clear();
        for (int i = 0; i < machine->getStateCount(); ++i)
        {
            fromBox.addItem (machine->state (i).name, i + 1);
            toBox.addItem (machine->state (i).name, i + 1);
        }
        selectedRuleIndex = selectedRuleIndex >= static_cast<int> (machine->rules.size()) ? -1 : selectedRuleIndex;

        if (selectedRuleIndex >= 0)
            loadRuleIntoControls (selectedRuleIndex);
        else
        {
            fromBox.setSelectedItemIndex (machine->selectedState);
            toBox.setSelectedItemIndex ((machine->selectedState + 1) % machine->getStateCount());
            weightSlider.setValue (1.0, juce::dontSendNotification);
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff181b20));
        g.setColour (ink());
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawText ("Transition rules", getLocalBounds().removeFromTop (28), juce::Justification::centredLeft);

        auto list = getRuleListBounds();
        g.setFont (juce::FontOptions (12.5f));

        for (int i = 0; i < static_cast<int> (machine->rules.size()); ++i)
        {
            auto row = list.removeFromTop (26);
            const auto& r = machine->rules[static_cast<size_t> (i)];
            const auto selected = i == selectedRuleIndex;
            g.setColour (selected ? selectedRow() : (i % 2 == 0 ? juce::Colour (0xff20252c) : juce::Colour (0xff1b2026)));
            g.fillRoundedRectangle (row.toFloat().reduced (1.0f), 4.0f);
            if (selected)
            {
                g.setColour (accentC().withAlpha (0.88f));
                g.fillRoundedRectangle (row.removeFromLeft (4).toFloat().reduced (0.0f, 4.0f), 2.0f);
            }

            g.setColour (selected ? ink() : mutedInk());

            auto rowArea = row.reduced (8, 0);
            g.drawText (machine->state (r.from).name, rowArea.removeFromLeft (96), juce::Justification::centredLeft);
            g.drawText ("->", rowArea.removeFromLeft (24), juce::Justification::centred);
            g.drawText (machine->state (r.to).name, rowArea.removeFromLeft (96), juce::Justification::centredLeft);
            g.drawText ("w " + juce::String (r.weight, 1), rowArea.removeFromRight (52), juce::Justification::centredRight);
        }
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        auto list = getRuleListBounds();
        for (int i = 0; i < static_cast<int> (machine->rules.size()); ++i)
        {
            auto row = list.removeFromTop (26);
            if (row.contains (event.getPosition()))
            {
                selectedRuleIndex = i;
                loadRuleIntoControls (i);
                repaint();
                return;
            }
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (0, 30).removeFromTop (36);
        fromBox.setBounds (area.removeFromLeft (82).reduced (0, 4));
        toBox.setBounds (area.removeFromLeft (82).reduced (4));
        weightSlider.setBounds (area.removeFromLeft (96).reduced (4));
        addButton.setBounds (area.removeFromLeft (54).reduced (4));
        updateButton.setBounds (area.removeFromLeft (58).reduced (4));
        removeButton.setBounds (area.removeFromLeft (62).reduced (4));
        ringButton.setBounds (area.removeFromLeft (64).reduced (4));
    }

private:
    juce::Rectangle<int> getRuleListBounds() const
    {
        return getLocalBounds().withTrimmedTop (76).reduced (0, 6);
    }

    void addRuleFromControls()
    {
        auto from = fromBox.getSelectedItemIndex();
        auto to = toBox.getSelectedItemIndex();
        if (from < 0 || to < 0)
            return;

        machine->rules.push_back ({ from, to, static_cast<float> (weightSlider.getValue()) });
        selectedRuleIndex = static_cast<int> (machine->rules.size()) - 1;
        if (onRulesChanged)
            onRulesChanged();
        repaint();
    }

    void updateSelectedRule()
    {
        if (selectedRuleIndex < 0 || selectedRuleIndex >= static_cast<int> (machine->rules.size()))
            return;

        auto from = fromBox.getSelectedItemIndex();
        auto to = toBox.getSelectedItemIndex();
        if (from < 0 || to < 0)
            return;

        machine->rules[static_cast<size_t> (selectedRuleIndex)] = { from, to, static_cast<float> (weightSlider.getValue()) };
        if (onRulesChanged)
            onRulesChanged();
        repaint();
    }

    void removeSelectedRule()
    {
        if (selectedRuleIndex < 0 || selectedRuleIndex >= static_cast<int> (machine->rules.size()))
            return;

        machine->rules.erase (machine->rules.begin() + selectedRuleIndex);
        selectedRuleIndex = juce::jmin (selectedRuleIndex, static_cast<int> (machine->rules.size()) - 1);
        refreshChoices();
        if (onRulesChanged)
            onRulesChanged();
        repaint();
    }

    void loadRuleIntoControls (int index)
    {
        if (index < 0 || index >= static_cast<int> (machine->rules.size()))
            return;

        const auto& rule = machine->rules[static_cast<size_t> (index)];
        fromBox.setSelectedItemIndex (rule.from, juce::dontSendNotification);
        toBox.setSelectedItemIndex (rule.to, juce::dontSendNotification);
        weightSlider.setValue (rule.weight, juce::dontSendNotification);
    }

    MachineModel* machine;
    juce::ComboBox fromBox;
    juce::ComboBox toBox;
    juce::Slider weightSlider;
    juce::TextButton addButton;
    juce::TextButton updateButton;
    juce::TextButton removeButton;
    juce::TextButton ringButton;
    int selectedRuleIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RuleListComponent)
};

class PillBar final : public juce::Component
{
public:
    std::function<void (int)> onIndexSelected;

    void setItems (const juce::StringArray& names, int selected)
    {
        buttons.clear();
        selectedIndex = selected;

        for (int i = 0; i < names.size(); ++i)
        {
            auto button = std::make_unique<juce::TextButton> (names[i]);
            button->setClickingTogglesState (false);
            button->onClick = [this, i]
            {
                selectedIndex = i;
                if (onIndexSelected)
                    onIndexSelected (i);
                repaint();
            };
            addAndMakeVisible (*button);
            buttons.push_back (std::move (button));
        }

        resized();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff181b20));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 7.0f);
    }

    void resized() override
    {
        if (buttons.empty())
            return;

        auto area = getLocalBounds().reduced (3);
        auto width = area.getWidth() / static_cast<int> (buttons.size());

        for (int i = 0; i < static_cast<int> (buttons.size()); ++i)
        {
            auto cell = area.removeFromLeft (i == static_cast<int> (buttons.size()) - 1 ? area.getWidth() : width);
            buttons[static_cast<size_t> (i)]->setBounds (cell.reduced (2, 1));
            buttons[static_cast<size_t> (i)]->setColour (juce::TextButton::buttonColourId,
                                                         i == selectedIndex ? selectedFill() : juce::Colour (0xff252a31));
            buttons[static_cast<size_t> (i)]->setColour (juce::TextButton::buttonOnColourId, selectedFill());
            buttons[static_cast<size_t> (i)]->setColour (juce::TextButton::textColourOffId,
                                                         i == selectedIndex ? accentA().brighter (0.2f) : mutedInk());
        }
    }

private:
    std::vector<std::unique_ptr<juce::TextButton>> buttons;
    int selectedIndex = 0;
};

class ClickableLabel final : public juce::Label
{
public:
    std::function<void()> onClick;

    void mouseUp (const juce::MouseEvent&) override
    {
        if (onClick)
            onClick();
    }
};

class TrackListComponent final : public juce::Component
{
public:
    std::function<void (int)> onTrackSelected;

    void setState (State& stateToShow, int selectedLane)
    {
        state = &stateToShow;
        selectedIndex = selectedLane;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        g.setColour (juce::Colour (0xff181b20));
        g.fillRoundedRectangle (bounds.toFloat(), 7.0f);

        if (state == nullptr)
            return;

        auto list = getTrackListBounds();
        for (int i = 0; i < static_cast<int> (state->lanes.size()); ++i)
        {
            auto row = list.removeFromTop (34).reduced (0, 3);
            const auto& lane = state->lanes[static_cast<size_t> (i)];
            const auto selected = i == selectedIndex;

            const auto laneColour = getTrackColour (i);
            g.setColour (selected ? selectedRow() : juce::Colour (0xff20252c));
            g.fillRoundedRectangle (row.toFloat(), 5.0f);
            if (selected)
            {
                g.setColour (laneColour.withAlpha (0.45f));
                g.fillRoundedRectangle (row.removeFromLeft (5).toFloat(), 3.0f);
            }

            g.setColour (selected ? laneColour.withAlpha (0.95f) : juce::Colour (0xff414a55));
            g.drawRoundedRectangle (row.toFloat(), 5.0f, selected ? 1.4f : 0.8f);

            auto rowText = row.reduced (10, 0);
            auto dotArea = rowText.removeFromLeft (12).withSizeKeepingCentre (8, 8).toFloat();
            g.setColour (laneColour.withAlpha (lane.playing ? 0.96f : 0.72f));
            g.fillEllipse (dotArea);
            g.setColour (lane.playing ? ink().withAlpha (0.85f) : juce::Colour (0xff101318).withAlpha (0.8f));
            g.drawEllipse (dotArea.expanded (1.0f), lane.playing ? 1.4f : 0.8f);

            g.setColour (selected ? ink() : mutedInk());
            g.setFont (juce::FontOptions (12.5f, selected ? juce::Font::bold : juce::Font::plain));
            g.drawFittedText (lane.name, rowText.removeFromLeft (84), juce::Justification::centredLeft, 1);

            g.setColour (lane.playing ? accentA() : mutedInk().withAlpha (0.68f));
            g.setFont (juce::FontOptions (10.5f));
            g.drawFittedText (lane.playing ? "live" : "script", rowText, juce::Justification::centredRight, 1);
        }
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (state == nullptr)
            return;

        auto list = getTrackListBounds();
        for (int i = 0; i < static_cast<int> (state->lanes.size()); ++i)
        {
            auto row = list.removeFromTop (34).reduced (0, 3);
            if (row.contains (event.getPosition()))
            {
                selectedIndex = i;
                if (onTrackSelected)
                    onTrackSelected (i);
                repaint();
                return;
            }
        }
    }

private:
    juce::Colour getTrackColour (int index) const
    {
        return paletteColour (index);
    }

    juce::Rectangle<int> getTrackListBounds() const
    {
        return getLocalBounds().reduced (8, 8);
    }

    State* state = nullptr;
    int selectedIndex = 0;
};

class PaneDivider final : public juce::Component
{
public:
    enum class Orientation
    {
        vertical,
        horizontal
    };

    std::function<void()> onDragStarted;
    std::function<void (int)> onDragged;

    explicit PaneDivider (Orientation orientationToUse = Orientation::vertical) : orientation (orientationToUse)
    {
        setMouseCursor (orientation == Orientation::vertical ? juce::MouseCursor::LeftRightResizeCursor
                                                             : juce::MouseCursor::UpDownResizeCursor);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff0f1116).withAlpha (0.72f));
        g.fillRoundedRectangle (orientation == Orientation::vertical ? bounds.reduced (2.0f, 0.0f)
                                                                     : bounds.reduced (0.0f, 2.0f), 3.0f);
        g.setColour (accentB().withAlpha (isMouseOverOrDragging() ? 0.62f : 0.18f));
        g.fillRoundedRectangle (orientation == Orientation::vertical
                                    ? bounds.withSizeKeepingCentre (1.5f, bounds.getHeight() - 20.0f)
                                    : bounds.withSizeKeepingCentre (bounds.getWidth() - 28.0f, 1.5f), 1.0f);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (onDragStarted)
            onDragStarted();
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (onDragged)
            onDragged (orientation == Orientation::vertical ? event.getDistanceFromDragStartX()
                                                            : event.getDistanceFromDragStartY());
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        repaint();
    }

private:
    Orientation orientation = Orientation::vertical;
};

class MainComponent final : public juce::Component
{
public:
    MainComponent() : graph (machine), rules (machine)
    {
        setSize (1180, 760);

        addAndMakeVisible (title);
        addAndMakeVisible (statusLabel);
        addAndMakeVisible (logButton);
        addAndMakeVisible (panicButton);
        addAndMakeVisible (topStateCountLabel);
        addAndMakeVisible (topStateCountMinus);
        addAndMakeVisible (topStateCountEditor);
        addAndMakeVisible (topStateCountPlus);
        addAndMakeVisible (runButton);
        addAndMakeVisible (stepButton);
        addAndMakeVisible (stopAllButton);
        addAndMakeVisible (rateSlider);
        addAndMakeVisible (graph);
        addAndMakeVisible (rules);
        addAndMakeVisible (graphBottomDivider);
        addAndMakeVisible (rulesTracksDivider);
        addAndMakeVisible (tracksCodeDivider);
        addAndMakeVisible (stateTabs);
        addAndMakeVisible (trackPaneTitle);
        addAndMakeVisible (trackList);
        addAndMakeVisible (codePaneTitle);
        addAndMakeVisible (scriptEditor);
        addAndMakeVisible (addLaneButton);
        addAndMakeVisible (removeLaneButton);
        addAndMakeVisible (addChildMachineButton);
        addAndMakeVisible (enterChildMachineButton);
        addAndMakeVisible (exitChildMachineButton);
        addAndMakeVisible (playButton);
        addAndMakeVisible (stopButton);

        title.setText ("Markov FSM", juce::dontSendNotification);
        title.setFont (juce::FontOptions (24.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, ink());

        codePaneTitle.setText ("SC Code", juce::dontSendNotification);
        codePaneTitle.setFont (juce::FontOptions (13.5f, juce::Font::bold));
        codePaneTitle.setColour (juce::Label::textColourId, ink());
        codePaneTitle.setJustificationType (juce::Justification::centredLeft);

        trackPaneTitle.setText ("Tracks", juce::dontSendNotification);
        trackPaneTitle.setFont (juce::FontOptions (13.5f, juce::Font::bold));
        trackPaneTitle.setColour (juce::Label::textColourId, ink());
        trackPaneTitle.setJustificationType (juce::Justification::centredLeft);

        statusLabel.setText ("Audio offline", juce::dontSendNotification);
        statusLabel.setFont (juce::FontOptions (13.0f));
        statusLabel.setColour (juce::Label::textColourId, mutedInk());
        statusLabel.setJustificationType (juce::Justification::centredRight);
        statusLabel.setMouseCursor (juce::MouseCursor::PointingHandCursor);
        statusLabel.onClick = [this]
        {
            startPrepareJob (false);
        };

        rulesTracksDivider.onDragged = [this] (int delta)
        {
            rulesPaneWidth = juce::jlimit (300, 860, dividerDragStartRulesWidth + delta);
            resized();
        };
        rulesTracksDivider.onDragStarted = [this]
        {
            rulesPaneUserSized = true;
            dividerDragStartRulesWidth = rulesPaneWidth;
        };

        tracksCodeDivider.onDragged = [this] (int delta)
        {
            tracksPaneWidth = juce::jlimit (260, 460, dividerDragStartTracksWidth - delta);
            resized();
        };
        tracksCodeDivider.onDragStarted = [this]
        {
            tracksPaneUserSized = true;
            dividerDragStartTracksWidth = tracksPaneWidth;
        };

        graphBottomDivider.onDragged = [this] (int delta)
        {
            bottomPaneHeight = dividerDragStartBottomHeight - delta;
            resized();
        };
        graphBottomDivider.onDragStarted = [this]
        {
            bottomPaneUserSized = true;
            dividerDragStartBottomHeight = bottomPaneHeight;
        };

        logButton.setButtonText ("Log");
        panicButton.setButtonText ("Panic");
        panicButton.setColour (juce::TextButton::buttonColourId, accentC().darker (0.45f));

        logView.setMultiLine (true);
        logView.setReadOnly (true);
        logView.setScrollbarsShown (true);
        logView.setCaretVisible (false);
        logView.setFont (juce::FontOptions (12.5f));
        logView.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xee111318));
        logView.setColour (juce::TextEditor::textColourId, mutedInk());
        logView.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff333a44));
        addChildComponent (logView);

        host.onStatusChanged = [this] (const juce::String& status)
        {
            statusLabel.setText (status, juce::dontSendNotification);
        };

        host.onLogMessage = [this] (const juce::String& message)
        {
            appendLog (message);
        };

        topStateCountLabel.setText ("Top states", juce::dontSendNotification);
        topStateCountLabel.setFont (juce::FontOptions (12.5f, juce::Font::bold));
        topStateCountLabel.setColour (juce::Label::textColourId, mutedInk());
        topStateCountLabel.setJustificationType (juce::Justification::centredRight);

        topStateCountMinus.setButtonText ("-");
        topStateCountPlus.setButtonText ("+");
        topStateCountMinus.onClick = [this] { setTopLevelStateCount (machine.getStateCount() - 1); };
        topStateCountPlus.onClick = [this] { setTopLevelStateCount (machine.getStateCount() + 1); };

        topStateCountEditor.setText (juce::String (machine.getStateCount()), false);
        topStateCountEditor.setInputRestrictions (2, "0123456789");
        topStateCountEditor.setJustification (juce::Justification::centred);
        topStateCountEditor.setSelectAllWhenFocused (true);
        topStateCountEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff181b20));
        topStateCountEditor.setColour (juce::TextEditor::textColourId, ink());
        topStateCountEditor.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff333a44));
        topStateCountEditor.setColour (juce::TextEditor::focusedOutlineColourId, accentA());
        topStateCountEditor.onReturnKey = [this] { commitTopLevelStateCountEditor(); };
        topStateCountEditor.onFocusLost = [this] { commitTopLevelStateCountEditor(); };

        runButton.setButtonText ("Run FSM");
        stepButton.setButtonText ("Step");
        stopAllButton.setButtonText ("Stop all");
        rateSlider.setRange (0.2, 4.0, 0.1);
        rateSlider.setValue (1.0);
        rateSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 22);

        runButton.onClick = [this]
        {
            if (! fsmRunning)
            {
                fsmRunning = true;
                if (machinePrepared && ! audioJobRunning)
                    startPreparedRun();
                else
                    startPrepareJob (true);
            }
            else
            {
                fsmRunning = false;
                stopTransport();
                stopMachineRecursive (machine);
                runButton.setButtonText ("Run FSM");
            }
        };

        stepButton.onClick = [this]
        {
            advanceStateVisualOnly();
        };

        stopAllButton.onClick = [this]
        {
            fsmRunning = false;
            stopTransport();
            runButton.setButtonText ("Run FSM");
            host.stopAll (machine);
            refreshControls();
        };

        panicButton.onClick = [this]
        {
            fsmRunning = false;
            stopTransport();
            stopMachineRecursive (machine);
            runButton.setButtonText ("Run FSM");
            host.panic (machine);
            refreshControls();
        };

        logButton.onClick = [this]
        {
            logVisible = ! logVisible;
            logView.setVisible (logVisible);
            resized();
        };

        rateSlider.onValueChange = [this]
        {
            if (fsmRunning)
            {
                restartTransport();
            }
        };

        stateTabs.onIndexSelected = [this] (int newIndex)
        {
            currentMachine().selectedState = newIndex;
            currentMachine().selectedLane = 0;
            refreshControls();
        };

        trackList.onTrackSelected = [this] (int newIndex)
        {
            currentInspectorMachine().selectedLane = newIndex;
            refreshControls();
        };

        scriptEditor.setMultiLine (true);
        scriptEditor.setReturnKeyStartsNewLine (true);
        scriptEditor.setFont (juce::FontOptions (15.0f, juce::Font::plain));
        scriptEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff0f1116));
        scriptEditor.setColour (juce::TextEditor::textColourId, ink());
        scriptEditor.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff242a32));
        scriptEditor.onTextChange = [this]
        {
            currentInspectorMachine().selectedLaneRef().script = scriptEditor.getText();
            currentInspectorMachine().selectedLaneRef().preparedBridge = -1;
            markMachineDirty();
        };

        addLaneButton.setButtonText ("+ Lane");
        removeLaneButton.setButtonText ("- Lane");
        addChildMachineButton.setButtonText ("+ FSM");
        enterChildMachineButton.setButtonText ("Enter");
        exitChildMachineButton.setButtonText ("Back");
        playButton.setButtonText ("Play");
        stopButton.setButtonText ("Stop");

        addLaneButton.onClick = [this]
        {
            currentInspectorMachine().addLaneToSelectedState();
            markMachineDirty();
            refreshControls();
        };

        removeLaneButton.onClick = [this]
        {
            host.stop (currentInspectorMachine().selectedLaneRef());
            currentInspectorMachine().removeSelectedLane();
            markMachineDirty();
            refreshControls();
        };

        addChildMachineButton.onClick = [this]
        {
            currentMachine().addChildToSelectedState();
            markMachineDirty();
            refreshControls();
        };

        enterChildMachineButton.onClick = [this]
        {
            if (auto* child = currentMachine().childMachine (currentMachine().selectedState))
            {
                machineStack.push_back (activeMachine);
                setActiveMachine (*child);
            }
        };

        exitChildMachineButton.onClick = [this]
        {
            if (! machineStack.empty())
            {
                auto* parent = machineStack.back();
                machineStack.pop_back();
                setActiveMachine (*parent);
            }
        };

        playButton.onClick = [this]
        {
            host.play (currentInspectorMachine().selectedLaneRef(), getSclangPathOverride());
            refreshControls();
        };

        stopButton.onClick = [this]
        {
            host.stop (currentInspectorMachine().selectedLaneRef());
            refreshControls();
        };

        graph.onStateChosen = [this] (int)
        {
            inspectedMachine = &currentMachine();
            refreshControls();
        };

        graph.onNestedBadgeChosen = [this] (int parentStateIndex)
        {
            if (auto* child = currentMachine().childMachine (parentStateIndex))
            {
                currentMachine().selectedState = parentStateIndex;
                child->selectedLane = 0;
                inspectedMachine = child;
                refreshControls();
            }
        };

        graph.onNestedStateChosen = [this] (int parentStateIndex, int childStateIndex)
        {
            if (auto* child = currentMachine().childMachine (parentStateIndex))
            {
                currentMachine().selectedState = parentStateIndex;
                child->selectedState = childStateIndex;
                child->selectedLane = 0;
                inspectedMachine = child;
                refreshControls();
            }
        };

        graph.onNestedStateCountChanged = [this] (int parentStateIndex, int newCount)
        {
            if (auto* child = currentMachine().childMachine (parentStateIndex))
            {
                fsmRunning = false;
                stopTransport();
                host.stopAll (machine);
                runButton.setButtonText ("Run FSM");

                child->setStateCount (newCount);
                child->regenerateRingRules();
                markMachineDirty();
                refreshControls();
            }
        };

        rules.onRulesChanged = [this]
        {
            markMachineDirty();
            graph.repaint();
            rules.repaint();
        };

        refreshControls();
        juce::Timer::callAfterDelay (350, [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (safeThis != nullptr)
                safeThis->startPrepareJob (false);
        });
    }

    ~MainComponent() override
    {
        stopTransport();
        stopMachineRecursive (machine);
        host.onLogMessage = nullptr;
        host.onStatusChanged = nullptr;
    }

    void paint (juce::Graphics& g) override
    {
        juce::ColourGradient bg (juce::Colour (0xff0f1013), getLocalBounds().getTopLeft().toFloat(),
                                 juce::Colour (0xff242830), getLocalBounds().getBottomRight().toFloat(), false);
        g.setGradientFill (bg);
        g.fillAll();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (18);
        auto header = area.removeFromTop (46);
        title.setBounds (header.removeFromLeft (360));
        auto topCountArea = header.removeFromRight (188).reduced (0, 8);
        topStateCountLabel.setBounds (topCountArea.removeFromLeft (76));
        topStateCountMinus.setBounds (topCountArea.removeFromLeft (28).reduced (2, 0));
        topStateCountEditor.setBounds (topCountArea.removeFromLeft (42).reduced (2, 0));
        topStateCountPlus.setBounds (topCountArea.removeFromLeft (28).reduced (2, 0));
        rateSlider.setBounds (header.removeFromRight (150).reduced (6, 8));
        logButton.setBounds (header.removeFromRight (56).reduced (4, 8));
        panicButton.setBounds (header.removeFromRight (74).reduced (4, 8));
        stopAllButton.setBounds (header.removeFromRight (86).reduced (4, 8));
        stepButton.setBounds (header.removeFromRight (66).reduced (4, 8));
        runButton.setBounds (header.removeFromRight (88).reduced (4, 8));
        statusLabel.setBounds (header.reduced (8, 8));

        const auto horizontalDividerHeight = 8;
        const auto minGraphHeight = 230;
        const auto minBottomHeight = 170;
        const auto maxBottomHeight = juce::jmax (minBottomHeight, area.getHeight() - 36 - minGraphHeight - horizontalDividerHeight);
        if (! bottomPaneUserSized)
            bottomPaneHeight = juce::jlimit (240, 330, juce::roundToInt (static_cast<float> (area.getHeight()) * 0.30f));
        bottomPaneHeight = juce::jlimit (minBottomHeight, maxBottomHeight, bottomPaneHeight);

        const auto dividerWidth = 8;
        const auto minWorkspace = 760;
        const auto minTracks = 260;
        const auto maxTracks = juce::jmax (minTracks, area.getWidth() - minWorkspace - dividerWidth);
        if (! tracksPaneUserSized)
            tracksPaneWidth = juce::jlimit (300, 360, juce::roundToInt (static_cast<float> (area.getWidth()) * 0.17f));
        tracksPaneWidth = juce::jlimit (minTracks, juce::jmin (460, maxTracks), tracksPaneWidth);

        auto tracksPane = area.removeFromRight (tracksPaneWidth);
        tracksCodeDivider.setBounds (area.removeFromRight (dividerWidth).reduced (0, 8));
        auto workspace = area;

        auto lower = workspace.removeFromBottom (bottomPaneHeight).reduced (0, 8);
        graphBottomDivider.setBounds (workspace.removeFromBottom (horizontalDividerHeight).reduced (0, 1));
        const auto minRules = 300;
        const auto minCode = 440;
        const auto maxRules = juce::jmax (minRules, lower.getWidth() - minCode - dividerWidth);
        if (! rulesPaneUserSized)
            rulesPaneWidth = juce::roundToInt (static_cast<float> (lower.getWidth()) * 0.43f);
        rulesPaneWidth = juce::jlimit (minRules, maxRules, rulesPaneWidth);

        auto bottom = lower;
        auto rulesPane = bottom.removeFromLeft (rulesPaneWidth);
        auto dividerA = bottom.removeFromLeft (dividerWidth);
        auto codePane = bottom;

        rules.setBounds (rulesPane);
        rulesTracksDivider.setBounds (dividerA);

        auto trackPaneInner = tracksPane.reduced (10, 8);
        trackPaneTitle.setBounds (trackPaneInner.removeFromTop (28).reduced (2, 0));
        auto trackHeader = trackPaneInner.removeFromTop (38);
        addLaneButton.setBounds (trackHeader.removeFromLeft (92).reduced (0, 4));
        removeLaneButton.setBounds (trackHeader.removeFromLeft (92).reduced (6, 4));
        trackList.setBounds (trackPaneInner.reduced (0, 4));

        auto codePaneInner = codePane.reduced (8, 0);
        auto codeHeader = codePaneInner.removeFromTop (34);
        codePaneTitle.setBounds (codeHeader.removeFromLeft (74).reduced (3));
        addChildMachineButton.setBounds (codeHeader.removeFromLeft (64).reduced (3));
        enterChildMachineButton.setBounds (codeHeader.removeFromLeft (58).reduced (3));
        exitChildMachineButton.setBounds (codeHeader.removeFromLeft (54).reduced (3));
        playButton.setBounds (codeHeader.removeFromLeft (64).reduced (3));
        stopButton.setBounds (codeHeader.removeFromLeft (64).reduced (3));
        scriptEditor.setBounds (codePaneInner.reduced (0, 6));

        stateTabs.setBounds (area.removeFromTop (36));
        auto graphArea = workspace.reduced (0, 10);
        if (logVisible)
            logView.setBounds (graphArea.removeFromBottom (142).reduced (0, 8));

        graph.setBounds (graphArea);
    }

private:
    MachineModel& currentMachine() const
    {
        return *activeMachine;
    }

    MachineModel& currentInspectorMachine() const
    {
        return inspectedMachine != nullptr ? *inspectedMachine : *activeMachine;
    }

    juce::String getSclangPathOverride() const
    {
        return {};
    }

    void setActiveMachine (MachineModel& newMachine)
    {
        activeMachine = &newMachine;
        inspectedMachine = &newMachine;
        graph.setMachine (newMachine);
        graph.setInspectedMachine (&newMachine);
        rules.setMachine (newMachine);
        topStateCountEditor.setText (juce::String (machine.getStateCount()), false);
        refreshControls();
    }

    void commitTopLevelStateCountEditor()
    {
        setTopLevelStateCount (topStateCountEditor.getText().getIntValue());
    }

    void setTopLevelStateCount (int newCount)
    {
        newCount = juce::jlimit (1, maxStateCount, newCount);
        topStateCountEditor.setText (juce::String (newCount), false);

        if (newCount == machine.getStateCount())
            return;

        fsmRunning = false;
        stopTransport();
        host.stopAll (machine);
        runButton.setButtonText ("Run FSM");

        machineStack.clear();
        activeMachine = &machine;
        machine.setStateCount (newCount);
        graph.setMachine (machine);
        graph.setInspectedMachine (&machine);
        rules.setMachine (machine);
        markMachineDirty();
        refreshControls();
    }

    std::vector<LaneSnapshot> makeLaneSnapshots() const
    {
        std::vector<LaneSnapshot> lanes;
        collectLaneSnapshots (machine, lanes);

        return lanes;
    }

    void collectLaneSnapshots (const MachineModel& model, std::vector<LaneSnapshot>& lanes) const
    {
        for (const auto& state : model.states)
        {
            for (const auto& lane : state.lanes)
                lanes.push_back ({ lane.id, lane.name, lane.script });

            if (auto* child = model.childMachine (state.index))
                collectLaneSnapshots (*child, lanes);
        }
    }

    void markPreparedLanes (const std::vector<LaneSnapshot>& lanes, int bridge)
    {
        markPreparedLanesInMachine (machine, lanes, bridge);
    }

    void markPreparedLanesInMachine (MachineModel& model, const std::vector<LaneSnapshot>& lanes, int bridge)
    {
        for (const auto& snapshot : lanes)
        {
            for (auto& state : model.states)
            {
                for (auto& lane : state.lanes)
                    if (lane.id == snapshot.id)
                        lane.preparedBridge = bridge;

                if (auto* child = model.childMachine (state.index))
                    markPreparedLanesInMachine (*child, lanes, bridge);
            }
        }
    }

    void markMachineDirty()
    {
        machinePrepared = false;
    }

    void startPreparedRun()
    {
        runButton.setButtonText ("Pause");
        startMachine (machine, machine.selectedState);
        startTransport();
        refreshControls();
    }

    void startPrepareJob (bool startAfterPrepare)
    {
        if (audioJobRunning.exchange (true))
            return;

        runButton.setButtonText (startAfterPrepare ? "Starting..." : "Run FSM");
        statusLabel.setText ("Booting audio", juce::dontSendNotification);

        auto lanes = makeLaneSnapshots();
        auto path = getSclangPathOverride();
        auto safeThis = juce::Component::SafePointer<MainComponent> (this);

        juce::Thread::launch ([safeThis, lanes, path, startAfterPrepare]
        {
            if (safeThis == nullptr)
                return;

            int preparedBridge = -1;
            for (const auto& lane : lanes)
            {
                if (safeThis == nullptr)
                    return;

                preparedBridge = safeThis->host.prepareData (lane, path);
                if (preparedBridge < 0)
                    break;
            }

            juce::MessageManager::callAsync ([safeThis, lanes, preparedBridge, startAfterPrepare]
            {
                if (safeThis == nullptr)
                    return;

                safeThis->audioJobRunning = false;

                if (preparedBridge >= 0)
                {
                    safeThis->markPreparedLanes (lanes, preparedBridge);
                    safeThis->host.configureMachine (safeThis->machine);
                    safeThis->machinePrepared = true;

                    if (startAfterPrepare && safeThis->fsmRunning)
                    {
                        safeThis->startPreparedRun();
                    }
                    else
                    {
                        safeThis->runButton.setButtonText ("Run FSM");
                    }
                }
                else
                {
                    safeThis->fsmRunning = false;
                    safeThis->runButton.setButtonText ("Run FSM");
                }

                safeThis->refreshControls();
            });
        });
    }

    void appendLog (const juce::String& message)
    {
        scLog << juce::Time::getCurrentTime().toString (true, true, true, true) << "  " << message << "\n";

        constexpr int maxLogChars = 24000;
        if (scLog.length() > maxLogChars)
            scLog = scLog.substring (scLog.length() - maxLogChars);

        logView.setText (scLog, juce::dontSendNotification);
        logView.moveCaretToEnd();
    }

    int getTransportIntervalMs() const
    {
        return juce::jlimit (80, 5000, static_cast<int> (1000.0 / getTransportRateHz()));
    }

    double getTransportRateHz() const
    {
        return juce::jmax (0.1, rateSlider.getValue() * 2.0);
    }

    void startTransport()
    {
        stopTransport();
        transportShouldRun = true;

        auto safeThis = juce::Component::SafePointer<MainComponent> (this);
        auto intervalMs = getTransportIntervalMs();
        transportThread = std::thread ([safeThis, intervalMs]
        {
            auto nextTick = std::chrono::steady_clock::now() + std::chrono::milliseconds (intervalMs);

            while (safeThis != nullptr && safeThis->transportShouldRun)
            {
                {
                    std::unique_lock<std::mutex> lock (safeThis->transportMutex);
                    if (safeThis->transportCv.wait_until (lock, nextTick, [safeThis]
                    {
                        return safeThis == nullptr || ! safeThis->transportShouldRun.load();
                    }))
                    {
                        break;
                    }
                }

                nextTick += std::chrono::milliseconds (intervalMs);

                if (safeThis == nullptr || ! safeThis->transportShouldRun)
                    break;

                juce::MessageManager::callAsync ([safeThis]
                {
                    if (safeThis != nullptr && safeThis->transportShouldRun)
                        safeThis->advanceStateVisualOnly();
                });
            }
        });
    }

    void stopTransport()
    {
        transportShouldRun = false;
        transportCv.notify_all();
        if (transportThread.joinable())
        {
            if (transportThread.get_id() != std::this_thread::get_id())
                transportThread.join();
            else
                transportThread.detach();
        }
    }

    void restartTransport()
    {
        if (! fsmRunning)
            return;

        startTransport();
    }

    void playState (int stateIndex)
    {
        for (auto& lane : currentMachine().state (stateIndex).lanes)
            host.play (lane, getSclangPathOverride());
    }

    void prepareAllLanes()
    {
        for (auto& state : currentMachine().states)
            for (auto& lane : state.lanes)
                host.prepare (lane, getSclangPathOverride());
    }

    void stopState (int stateIndex)
    {
        for (auto& lane : currentMachine().state (stateIndex).lanes)
            host.stop (lane);
    }

    void advanceStateVisualOnly()
    {
        advanceMachineTree (machine);
        refreshControls();
    }

    int chooseNextState (const MachineModel& model) const
    {
        std::vector<Rule> candidates;
        float total = 0.0f;

        for (const auto& rule : model.rules)
        {
            if (rule.from == model.selectedState)
            {
                candidates.push_back (rule);
                total += juce::jmax (0.0f, rule.weight);
            }
        }

        if (candidates.empty() || total <= 0.0f)
            return (model.selectedState + 1) % model.getStateCount();

        auto pick = juce::Random::getSystemRandom().nextFloat() * total;
        for (const auto& rule : candidates)
        {
            pick -= juce::jmax (0.0f, rule.weight);
            if (pick <= 0.0f)
                return rule.to;
        }

        return candidates.back().to;
    }

    void startMachine (MachineModel& model, int stateIndex)
    {
        model.entryState = juce::jlimit (0, model.getStateCount() - 1, stateIndex);
        model.stepsSinceEntry = 0;
        enterState (model, model.entryState);
    }

    bool advanceMachineTree (MachineModel& model)
    {
        if (auto* child = model.childMachine (model.selectedState))
        {
            if (! advanceMachineTree (*child))
                return false;
        }

        const auto nextState = chooseNextState (model);
        enterState (model, nextState);
        ++model.stepsSinceEntry;

        return model.stepsSinceEntry > 0 && model.selectedState == model.entryState;
    }

    void enterState (MachineModel& model, int stateIndex)
    {
        if (model.selectedState >= 0 && model.selectedState < model.getStateCount())
        {
            for (auto& lane : model.state (model.selectedState).lanes)
                host.stop (lane);

            if (auto* child = model.childMachine (model.selectedState))
                stopMachineRecursive (*child);
        }

        model.selectedState = stateIndex;
        model.selectedLane = 0;

        for (auto& lane : model.state (stateIndex).lanes)
            host.play (lane, getSclangPathOverride());

        if (auto* child = model.childMachine (stateIndex))
            startMachine (*child, child->selectedState);
    }

    void stopMachineRecursive (MachineModel& model)
    {
        for (auto& state : model.states)
        {
            for (auto& lane : state.lanes)
                host.stop (lane);

            if (auto* child = model.childMachine (state.index))
                stopMachineRecursive (*child);
        }
    }

    void refreshControls()
    {
        refreshStateTabs();
        refreshTrackList();
        rules.setMachine (currentInspectorMachine());

        scriptEditor.setText (currentInspectorMachine().selectedLaneRef().script, juce::dontSendNotification);
        topStateCountEditor.setText (juce::String (machine.getStateCount()), false);
        playButton.setColour (juce::TextButton::buttonColourId, currentInspectorMachine().selectedLaneRef().playing ? accentA().darker (0.2f) : accentB().darker (0.35f));
        const auto hasChild = currentMachine().hasChildMachine (currentMachine().selectedState);
        addChildMachineButton.setEnabled (! hasChild);
        enterChildMachineButton.setEnabled (hasChild);
        exitChildMachineButton.setEnabled (! machineStack.empty());
        graph.repaint();
        rules.repaint();
        graph.setInspectedMachine (&currentInspectorMachine());
    }

    void refreshStateTabs()
    {
        juce::StringArray names;
        for (int i = 0; i < currentMachine().getStateCount(); ++i)
            names.add (currentMachine().state (i).name);

        stateTabs.setItems (names, currentMachine().selectedState);
    }

    void refreshTrackList()
    {
        auto& inspected = currentInspectorMachine();
        auto& s = inspected.state (inspected.selectedState);
        trackList.setState (s, inspected.selectedLane);
    }

    MachineModel machine;
    MachineModel* activeMachine = &machine;
    MachineModel* inspectedMachine = &machine;
    std::vector<MachineModel*> machineStack;
    SuperColliderHost host;
    GraphComponent graph;
    RuleListComponent rules;
    PaneDivider graphBottomDivider { PaneDivider::Orientation::horizontal };
    PaneDivider rulesTracksDivider;
    PaneDivider tracksCodeDivider;

    juce::Label title;
    ClickableLabel statusLabel;
    juce::TextButton logButton;
    juce::TextButton panicButton;
    juce::Label topStateCountLabel;
    juce::TextButton topStateCountMinus;
    juce::TextEditor topStateCountEditor;
    juce::TextButton topStateCountPlus;
    juce::TextButton runButton;
    juce::TextButton stepButton;
    juce::TextButton stopAllButton;
    juce::Slider rateSlider;
    PillBar stateTabs;
    juce::Label trackPaneTitle;
    TrackListComponent trackList;
    juce::Label codePaneTitle;
    juce::TextEditor scriptEditor;
    juce::TextButton addLaneButton;
    juce::TextButton removeLaneButton;
    juce::TextButton addChildMachineButton;
    juce::TextButton enterChildMachineButton;
    juce::TextButton exitChildMachineButton;
    juce::TextButton playButton;
    juce::TextButton stopButton;
    juce::TextEditor logView;
    juce::String scLog;
    bool logVisible = false;
    bool fsmRunning = false;
    bool machinePrepared = false;
    int rulesPaneWidth = 500;
    int tracksPaneWidth = 210;
    int bottomPaneHeight = 250;
    int dividerDragStartRulesWidth = 500;
    int dividerDragStartTracksWidth = 210;
    int dividerDragStartBottomHeight = 250;
    bool rulesPaneUserSized = false;
    bool tracksPaneUserSized = false;
    bool bottomPaneUserSized = false;
    std::atomic<bool> audioJobRunning { false };
    std::atomic<bool> transportShouldRun { false };
    std::mutex transportMutex;
    std::condition_variable transportCv;
    std::thread transportThread;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

class MarkovApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Markov FSM"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override
    {
        mainWindow = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (juce::String name)
            : DocumentWindow (std::move (name), backgroundTop(), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
                setBounds (display->userArea);
            else
                centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (MarkovApplication)
