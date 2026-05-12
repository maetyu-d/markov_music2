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
    std::function<void (int, int)> onNestedStateCountChanged;

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
            if (machine->childMachine (i) != nullptr
                && getNestedBadgeBounds (statePositions[static_cast<size_t> (i)]).contains (event.position))
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
                && getNestedBadgeBounds (statePositions[static_cast<size_t> (i)]).contains (event.position))
            {
                startNestedStateCountEdit (i, getNestedBadgeBounds (statePositions[static_cast<size_t> (i)]));
                return;
            }
        }
    }

private:
    void layoutStates()
    {
        const auto count = machine->getStateCount();
        statePositions.resize (static_cast<size_t> (count));

        auto area = getLocalBounds().toFloat().reduced (70.0f, 60.0f);
        auto centre = area.getCentre();
        auto rx = area.getWidth() * 0.42f;
        auto ry = area.getHeight() * 0.38f;

        for (int i = 0; i < count; ++i)
        {
            auto angle = (juce::MathConstants<float>::twoPi * static_cast<float> (i) / static_cast<float> (count))
                       - juce::MathConstants<float>::halfPi;
            statePositions[static_cast<size_t> (i)] = { centre.x + std::cos (angle) * rx,
                                                        centre.y + std::sin (angle) * ry };
        }
    }

    void drawRules (juce::Graphics& g)
    {
        for (const auto& rule : machine->rules)
        {
            if (rule.from >= static_cast<int> (statePositions.size()) || rule.to >= static_cast<int> (statePositions.size()))
                continue;

            auto from = statePositions[static_cast<size_t> (rule.from)];
            auto to = statePositions[static_cast<size_t> (rule.to)];
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

            g.setColour (selected ? juce::Colour (0xff2d2b24) : juce::Colour (0xff252a31));
            g.fillEllipse (p.x - stateRadius, p.y - stateRadius, stateRadius * 2.0f, stateRadius * 2.0f);

            g.setColour ((selected ? accentA() : mutedInk()).withAlpha (0.95f));
            g.drawEllipse (p.x - stateRadius, p.y - stateRadius, stateRadius * 2.0f, stateRadius * 2.0f, selected ? 3.0f : 1.5f);

            const auto* child = machine->childMachine (i);
            if (child != nullptr)
            {
                auto nestedRadius = stateRadius + 7.0f;
                g.setColour ((selected ? accentC() : accentB()).withAlpha (selected ? 0.95f : 0.62f));
                g.drawEllipse (p.x - nestedRadius, p.y - nestedRadius, nestedRadius * 2.0f, nestedRadius * 2.0f, 2.0f);
                drawNestedIndicator (g, *child, p, selected);
            }

            g.setColour (ink());
            g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
            g.drawFittedText (machine->state (i).name, juce::Rectangle<int> (static_cast<int> (p.x - 50.0f),
                                                                            static_cast<int> (p.y - 20.0f), 100, 24),
                              juce::Justification::centred, 1);

            g.setColour (mutedInk());
            g.setFont (juce::FontOptions (12.0f));
            g.drawFittedText (juce::String (laneCount) + (laneCount == 1 ? " lane" : " lanes"),
                              juce::Rectangle<int> (static_cast<int> (p.x - 48.0f), static_cast<int> (p.y + 4.0f), 96, 20),
                              juce::Justification::centred, 1);
        }
    }

    void drawNestedIndicator (juce::Graphics& g, const MachineModel& child, juce::Point<float> parentCentre, bool parentSelected)
    {
        const auto childCount = child.getStateCount();
        if (childCount <= 0)
            return;

        const auto orbitRadius = stateRadius + 16.0f;
        const auto nodeRadius = childCount > 7 ? 3.0f : 3.7f;
        std::vector<juce::Point<float>> childPoints;
        childPoints.reserve (static_cast<size_t> (childCount));

        for (int j = 0; j < childCount; ++j)
        {
            const auto angle = (juce::MathConstants<float>::twoPi * static_cast<float> (j) / static_cast<float> (childCount))
                             - juce::MathConstants<float>::halfPi;
            childPoints.push_back ({ parentCentre.x + std::cos (angle) * orbitRadius,
                                     parentCentre.y + std::sin (angle) * orbitRadius });
        }

        g.setColour (juce::Colour (0xff11161d).withAlpha (0.86f));
        g.fillEllipse (parentCentre.x - orbitRadius - 6.0f, parentCentre.y - orbitRadius - 6.0f,
                       (orbitRadius + 6.0f) * 2.0f, (orbitRadius + 6.0f) * 2.0f);

        g.setColour ((parentSelected ? accentA() : accentC()).withAlpha (parentSelected ? 0.88f : 0.48f));
        g.drawEllipse (parentCentre.x - orbitRadius, parentCentre.y - orbitRadius,
                       orbitRadius * 2.0f, orbitRadius * 2.0f, parentSelected ? 2.0f : 1.3f);

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
            g.setColour (selected ? accentA().withAlpha (0.98f) : accentC().withAlpha (0.86f));
            g.fillEllipse (point.x - nodeRadius, point.y - nodeRadius, nodeRadius * 2.0f, nodeRadius * 2.0f);
            g.setColour (juce::Colour (0xff101318).withAlpha (0.92f));
            g.drawEllipse (point.x - nodeRadius, point.y - nodeRadius, nodeRadius * 2.0f, nodeRadius * 2.0f, 1.0f);
        }

        auto badge = getNestedBadgeBounds (parentCentre);
        g.setColour (juce::Colour (0xff101318).withAlpha (0.96f));
        g.fillRoundedRectangle (badge, 6.0f);
        g.setColour ((parentSelected ? accentA() : accentC()).withAlpha (0.88f));
        g.drawRoundedRectangle (badge, 6.0f, 1.1f);
        g.setColour (ink());
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawFittedText (juce::String (childCount), badge.toNearestInt(), juce::Justification::centred, 1);
    }

    juce::Rectangle<float> getNestedBadgeBounds (juce::Point<float> parentCentre) const
    {
        return { parentCentre.x + stateRadius - 9.0f,
                 parentCentre.y + stateRadius - 11.0f,
                 28.0f, 20.0f };
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
        addAndMakeVisible (ringButton);

        weightSlider.setRange (0.1, 5.0, 0.1);
        weightSlider.setValue (1.0);
        weightSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 22);

        addButton.setButtonText ("Add rule");
        ringButton.setButtonText ("Ring rules");

        addButton.onClick = [this]
        {
            machine->rules.push_back ({ fromBox.getSelectedItemIndex(), toBox.getSelectedItemIndex(),
                                       static_cast<float> (weightSlider.getValue()) });
            if (onRulesChanged)
                onRulesChanged();
        };

        ringButton.onClick = [this]
        {
            machine->regenerateRingRules();
            if (onRulesChanged)
                onRulesChanged();
        };

        refreshChoices();
    }

    void setMachine (MachineModel& modelToUse)
    {
        machine = &modelToUse;
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
        fromBox.setSelectedItemIndex (machine->selectedState);
        toBox.setSelectedItemIndex ((machine->selectedState + 1) % machine->getStateCount());
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff181b20));
        g.setColour (ink());
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawText ("Transition rules", getLocalBounds().removeFromTop (28), juce::Justification::centredLeft);

        auto list = getLocalBounds().withTrimmedTop (72).reduced (0, 6);
        g.setFont (juce::FontOptions (12.5f));

        for (int i = 0; i < static_cast<int> (machine->rules.size()); ++i)
        {
            auto row = list.removeFromTop (24);
            const auto& r = machine->rules[static_cast<size_t> (i)];
            g.setColour (i % 2 == 0 ? juce::Colour (0xff20252c) : juce::Colour (0xff1b2026));
            g.fillRoundedRectangle (row.toFloat().reduced (1.0f), 4.0f);
            g.setColour (mutedInk());
            g.drawText (machine->state (r.from).name + "  ->  " + machine->state (r.to).name + "    w " + juce::String (r.weight, 1),
                        row.reduced (8, 0), juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (0, 30).removeFromTop (36);
        fromBox.setBounds (area.removeFromLeft (92).reduced (0, 4));
        toBox.setBounds (area.removeFromLeft (92).reduced (4));
        weightSlider.setBounds (area.removeFromLeft (108).reduced (4));
        addButton.setBounds (area.removeFromLeft (82).reduced (4));
        ringButton.setBounds (area.removeFromLeft (92).reduced (4));
    }

private:
    MachineModel* machine;
    juce::ComboBox fromBox;
    juce::ComboBox toBox;
    juce::Slider weightSlider;
    juce::TextButton addButton;
    juce::TextButton ringButton;

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
                                                         i == selectedIndex ? accentA().darker (0.55f) : juce::Colour (0xff252a31));
            buttons[static_cast<size_t> (i)]->setColour (juce::TextButton::textColourOffId,
                                                         i == selectedIndex ? ink() : mutedInk());
        }
    }

private:
    std::vector<std::unique_ptr<juce::TextButton>> buttons;
    int selectedIndex = 0;
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
        addAndMakeVisible (stateCount);
        addAndMakeVisible (runButton);
        addAndMakeVisible (stepButton);
        addAndMakeVisible (stopAllButton);
        addAndMakeVisible (rateSlider);
        addAndMakeVisible (graph);
        addAndMakeVisible (rules);
        addAndMakeVisible (stateTabs);
        addAndMakeVisible (laneTabs);
        addAndMakeVisible (scriptEditor);
        addAndMakeVisible (addLaneButton);
        addAndMakeVisible (removeLaneButton);
        addAndMakeVisible (addChildMachineButton);
        addAndMakeVisible (enterChildMachineButton);
        addAndMakeVisible (exitChildMachineButton);
        addAndMakeVisible (bootAudioButton);
        addAndMakeVisible (playButton);
        addAndMakeVisible (stopButton);
        addAndMakeVisible (sclangPath);

        title.setText ("Markov Finite-State Machine", juce::dontSendNotification);
        title.setFont (juce::FontOptions (25.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, ink());

        statusLabel.setText ("Audio offline", juce::dontSendNotification);
        statusLabel.setFont (juce::FontOptions (13.0f));
        statusLabel.setColour (juce::Label::textColourId, mutedInk());
        statusLabel.setJustificationType (juce::Justification::centredRight);

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

        stateCount.setRange (1, maxStateCount, 1);
        stateCount.setValue (currentMachine().getStateCount());
        stateCount.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 22);
        stateCount.onValueChange = [this]
        {
            stopMachineRecursive (machine);
            currentMachine().setStateCount (static_cast<int> (stateCount.getValue()));
            markMachineDirty();
            refreshControls();
        };

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

        laneTabs.onIndexSelected = [this] (int newIndex)
        {
            currentMachine().selectedLane = newIndex;
            refreshControls();
        };

        scriptEditor.setMultiLine (true);
        scriptEditor.setReturnKeyStartsNewLine (true);
        scriptEditor.setFont (juce::FontOptions (15.0f, juce::Font::plain));
        scriptEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff111318));
        scriptEditor.setColour (juce::TextEditor::textColourId, ink());
        scriptEditor.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        scriptEditor.onTextChange = [this]
        {
            currentMachine().selectedLaneRef().script = scriptEditor.getText();
            currentMachine().selectedLaneRef().preparedBridge = -1;
            markMachineDirty();
        };

        addLaneButton.setButtonText ("+ Lane");
        removeLaneButton.setButtonText ("- Lane");
        addChildMachineButton.setButtonText ("+ FSM");
        enterChildMachineButton.setButtonText ("Enter");
        exitChildMachineButton.setButtonText ("Back");
        bootAudioButton.setButtonText ("Boot audio");
        playButton.setButtonText ("Play");
        stopButton.setButtonText ("Stop");
        sclangPath.setTextToShowWhenEmpty ("sclang path (optional)", mutedInk());

        addLaneButton.onClick = [this]
        {
            currentMachine().addLaneToSelectedState();
            markMachineDirty();
            refreshControls();
        };

        removeLaneButton.onClick = [this]
        {
            host.stop (currentMachine().selectedLaneRef());
            currentMachine().removeSelectedLane();
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

        bootAudioButton.onClick = [this]
        {
            startPrepareJob (false);
        };

        playButton.onClick = [this]
        {
            host.play (currentMachine().selectedLaneRef(), sclangPath.getText());
            refreshControls();
        };

        stopButton.onClick = [this]
        {
            host.stop (currentMachine().selectedLaneRef());
            refreshControls();
        };

        graph.onStateChosen = [this] (int)
        {
            rules.setMachine (currentMachine());
            refreshControls();
        };

        graph.onNestedBadgeChosen = [this] (int parentStateIndex)
        {
            if (auto* child = currentMachine().childMachine (parentStateIndex))
            {
                currentMachine().selectedState = parentStateIndex;
                rules.setMachine (*child);
                rules.refreshChoices();
                graph.repaint();
                rules.repaint();
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
        stateCount.setBounds (header.removeFromRight (160).reduced (0, 8));
        rateSlider.setBounds (header.removeFromRight (150).reduced (6, 8));
        logButton.setBounds (header.removeFromRight (56).reduced (4, 8));
        panicButton.setBounds (header.removeFromRight (74).reduced (4, 8));
        stopAllButton.setBounds (header.removeFromRight (86).reduced (4, 8));
        stepButton.setBounds (header.removeFromRight (66).reduced (4, 8));
        runButton.setBounds (header.removeFromRight (88).reduced (4, 8));
        statusLabel.setBounds (header.reduced (8, 8));

        auto lower = area.removeFromBottom (230);
        rules.setBounds (lower.removeFromLeft (520).reduced (0, 8));

        auto laneArea = lower.reduced (14, 8);
        auto laneHeader = laneArea.removeFromTop (34);
        addLaneButton.setBounds (laneHeader.removeFromLeft (76).reduced (3));
        removeLaneButton.setBounds (laneHeader.removeFromLeft (76).reduced (3));
        addChildMachineButton.setBounds (laneHeader.removeFromLeft (70).reduced (3));
        enterChildMachineButton.setBounds (laneHeader.removeFromLeft (66).reduced (3));
        exitChildMachineButton.setBounds (laneHeader.removeFromLeft (62).reduced (3));
        bootAudioButton.setBounds (laneHeader.removeFromLeft (96).reduced (3));
        playButton.setBounds (laneHeader.removeFromLeft (72).reduced (3));
        stopButton.setBounds (laneHeader.removeFromLeft (72).reduced (3));
        sclangPath.setBounds (laneHeader.reduced (8, 3));
        laneTabs.setBounds (laneArea.removeFromTop (32));
        scriptEditor.setBounds (laneArea.reduced (0, 6));

        stateTabs.setBounds (area.removeFromTop (36));
        auto graphArea = area.reduced (0, 10);
        if (logVisible)
            logView.setBounds (graphArea.removeFromBottom (142).reduced (0, 8));

        graph.setBounds (graphArea);
    }

private:
    MachineModel& currentMachine() const
    {
        return *activeMachine;
    }

    void setActiveMachine (MachineModel& newMachine)
    {
        activeMachine = &newMachine;
        graph.setMachine (newMachine);
        rules.setMachine (newMachine);
        stateCount.setValue (newMachine.getStateCount(), juce::dontSendNotification);
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
        bootAudioButton.setButtonText ("Booting...");

        auto lanes = makeLaneSnapshots();
        auto path = sclangPath.getText();
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
                safeThis->bootAudioButton.setButtonText ("Boot audio");

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
            host.play (lane, sclangPath.getText());
    }

    void prepareAllLanes()
    {
        for (auto& state : currentMachine().states)
            for (auto& lane : state.lanes)
                host.prepare (lane, sclangPath.getText());
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
            host.play (lane, sclangPath.getText());

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
        refreshLaneTabs();
        rules.refreshChoices();

        scriptEditor.setText (currentMachine().selectedLaneRef().script, juce::dontSendNotification);
        playButton.setColour (juce::TextButton::buttonColourId, currentMachine().selectedLaneRef().playing ? accentA().darker (0.2f) : accentB().darker (0.35f));
        const auto hasChild = currentMachine().hasChildMachine (currentMachine().selectedState);
        addChildMachineButton.setEnabled (! hasChild);
        enterChildMachineButton.setEnabled (hasChild);
        exitChildMachineButton.setEnabled (! machineStack.empty());
        graph.repaint();
        rules.repaint();
    }

    void refreshStateTabs()
    {
        juce::StringArray names;
        for (int i = 0; i < currentMachine().getStateCount(); ++i)
            names.add (currentMachine().state (i).name);

        stateTabs.setItems (names, currentMachine().selectedState);
    }

    void refreshLaneTabs()
    {
        juce::StringArray names;
        auto& s = currentMachine().state (currentMachine().selectedState);
        for (int i = 0; i < static_cast<int> (s.lanes.size()); ++i)
        {
            auto& lane = s.lanes[static_cast<size_t> (i)];
            names.add (lane.name + (lane.playing ? " *" : ""));
        }

        laneTabs.setItems (names, currentMachine().selectedLane);
    }

    MachineModel machine;
    MachineModel* activeMachine = &machine;
    std::vector<MachineModel*> machineStack;
    SuperColliderHost host;
    GraphComponent graph;
    RuleListComponent rules;

    juce::Label title;
    juce::Label statusLabel;
    juce::TextButton logButton;
    juce::TextButton panicButton;
    juce::Slider stateCount;
    juce::TextButton runButton;
    juce::TextButton stepButton;
    juce::TextButton stopAllButton;
    juce::Slider rateSlider;
    PillBar stateTabs;
    PillBar laneTabs;
    juce::TextEditor scriptEditor;
    juce::TextButton addLaneButton;
    juce::TextButton removeLaneButton;
    juce::TextButton addChildMachineButton;
    juce::TextButton enterChildMachineButton;
    juce::TextButton exitChildMachineButton;
    juce::TextButton bootAudioButton;
    juce::TextButton playButton;
    juce::TextButton stopButton;
    juce::TextEditor sclangPath;
    juce::TextEditor logView;
    juce::String scLog;
    bool logVisible = false;
    bool fsmRunning = false;
    bool machinePrepared = false;
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
            centreWithSize (getWidth(), getHeight());
            setResizable (true, true);
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
