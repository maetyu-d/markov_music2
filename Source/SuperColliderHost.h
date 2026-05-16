#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_osc/juce_osc.h>

#include "FsmModel.h"

#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

inline constexpr double musicalReleaseSeconds = 1.45;

struct SuperColliderAudioSettings
{
    juce::String outputDevice;
    double sampleRate = 0.0;
    int hardwareBufferSize = 64;
    int outputChannels = 2;

    bool operator== (const SuperColliderAudioSettings& other) const
    {
        return outputDevice == other.outputDevice
            && std::abs (sampleRate - other.sampleRate) < 0.001
            && hardwareBufferSize == other.hardwareBufferSize
            && outputChannels == other.outputChannels;
    }

    bool operator!= (const SuperColliderAudioSettings& other) const
    {
        return ! (*this == other);
    }
};

class SuperColliderHost
{
public:
    std::function<void (const juce::String&)> onLogMessage;
    std::function<void (const juce::String&)> onStatusChanged;

    ~SuperColliderHost();

    void play (Lane& lane, const juce::String& sclangPath);
    bool prepare (Lane& lane, const juce::String& sclangPath);
    int prepareData (const LaneSnapshot& lane, const juce::String& sclangPath);
    bool freezeLane (Lane& lane, const juce::String& sclangPath, double durationSeconds, const juce::File& outputFile);
    bool exportMachine (MachineModel& model,
                        const juce::String& sclangPath,
                        const juce::File& outputFile,
                        double durationSeconds,
                        double rate,
                        int startState,
                        const juce::String& sampleFormat);
    void setLaneVolume (Lane& lane);
    void setLaneMix (Lane& lane);
    void setLaneEffectiveVolume (const Lane& lane, float volume);
    void setLaneEffectiveMix (const Lane& lane, float volume);
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
    void cancelExport();
    void testTone (const juce::String& sclangPath);
    juce::String checkScript (const juce::String& script, const juce::String& sclangPath);
    juce::String readCheckResult (const juce::String& checkId) const;
    void setAudioSettings (const SuperColliderAudioSettings& settings);

private:
    bool ensureBridgeRunning (const juce::String& sclangPath);
    bool ensureBridgeRunningLocked (const juce::String& sclangPath);
    juce::String resolveSclangExecutable (const juce::String& sclangPath) const;
    juce::String makeBridgeScript() const;
    void sendLoadCommand (const juce::String& laneId, const juce::String& scriptPath);
    void sendLoadFrozenCommand (const juce::String& laneId, const juce::String& audioPath);
    void sendFreezeCommand (const juce::String& laneId, const juce::String& audioPath, double durationSeconds);
    void sendExportCommand (const juce::String& audioPath, double durationSeconds, double rate, int startState, const juce::String& sampleFormat);
    void sendCancelExportCommand();
    void sendPlayCommand (const juce::String& laneId);
    void sendTransitionCommand (const juce::StringArray& stopIds,
                                const juce::StringArray& playIds,
                                double releaseSeconds,
                                double delaySeconds);
    void sendVolumeCommand (const juce::String& laneId, float volume);
    void sendMixCommand (const juce::String& laneId, float volume, float pan);
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
    SuperColliderAudioSettings audioSettings;
    std::atomic<bool> logReaderShouldRun { false };
    std::thread logReader;
    juce::String currentStatus { "Audio offline" };
    juce::OwnedArray<juce::File> tempScriptStorage;
    juce::HashMap<juce::String, juce::File*> tempScripts;
};
