#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_osc/juce_osc.h>

#include "FsmModel.h"

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

inline constexpr double musicalReleaseSeconds = 1.45;

class SuperColliderHost
{
public:
    std::function<void (const juce::String&)> onLogMessage;
    std::function<void (const juce::String&)> onStatusChanged;

    ~SuperColliderHost();

    void play (Lane& lane, const juce::String& sclangPath);
    bool prepare (Lane& lane, const juce::String& sclangPath);
    int prepareData (const LaneSnapshot& lane, const juce::String& sclangPath);
    void setLaneVolume (Lane& lane);
    void setLaneEffectiveVolume (const Lane& lane, float volume);
    void stop (Lane& lane, double releaseSeconds = musicalReleaseSeconds);
    void transition (const std::vector<Lane*>& lanesToStop,
                     const std::vector<Lane*>& lanesToStart,
                     const juce::String& sclangPath,
                     double releaseSeconds = musicalReleaseSeconds,
                     double delaySeconds = 0.0);
    void stopAll (MachineModel& model);
    bool isReady() const;
    int getBridgeGeneration() const;
    void panic (MachineModel& model);
    void configureMachine (const MachineModel& model);
    void runMachine (int startState, double rateHz);
    void pauseMachine();
    void stepMachine();
    void testTone (const juce::String& sclangPath);
    juce::String checkScript (const juce::String& script, const juce::String& sclangPath);
    juce::String readCheckResult (const juce::String& checkId) const;

private:
    bool ensureBridgeRunning (const juce::String& sclangPath);
    bool ensureBridgeRunningLocked (const juce::String& sclangPath);
    juce::String resolveSclangExecutable (const juce::String& sclangPath) const;
    juce::String makeBridgeScript() const;
    void sendLoadCommand (const juce::String& laneId, const juce::String& scriptPath);
    void sendPlayCommand (const juce::String& laneId);
    void sendTransitionCommand (const juce::StringArray& stopIds,
                                const juce::StringArray& playIds,
                                double releaseSeconds,
                                double delaySeconds);
    void sendVolumeCommand (const juce::String& laneId, float volume);
    void sendStopCommand (const juce::String& laneId, double releaseSeconds);
    void sendStopAllCommand();
    void sendClearMachineCommand();
    bool shouldUseCommandFallback() const;
    void writeCommand (const juce::String& command);
    void shutdown();
    static void markAllLanesStopped (MachineModel& model);
    void startLogReader();
    void addLog (const juce::String& message);
    void setStatus (const juce::String& status);

    mutable juce::CriticalSection hostLock;
    std::unique_ptr<juce::ChildProcess> bridgeProcess;
    juce::OSCSender oscSender;
    juce::File bridgeDirectory;
    juce::File commandDirectory;
    juce::File bridgeLogFile;
    juce::int64 bridgeLogReadPosition = 0;
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
