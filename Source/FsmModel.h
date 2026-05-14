#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "DemoScripts.h"

#include <algorithm>
#include <memory>
#include <vector>

inline constexpr int maxStateCount = 12;

enum class NestedTimingMode
{
    followParent,
    freeRun,
    oneShot,
    latch
};

inline juce::String nestedTimingModeName (NestedTimingMode mode)
{
    switch (mode)
    {
        case NestedTimingMode::followParent: return "Follow";
        case NestedTimingMode::freeRun: return "Free-run";
        case NestedTimingMode::oneShot: return "One-shot";
        case NestedTimingMode::latch: return "Latch";
    }

    return "Follow";
}

struct Lane
{
    Lane() = default;

    Lane (juce::String idToUse, juce::String nameToUse, juce::String scriptToUse)
        : id (std::move (idToUse)), name (std::move (nameToUse)), script (std::move (scriptToUse))
    {
    }

    juce::String id;
    juce::String name;
    juce::String script;
    float volume = 1.0f;
    bool enabled = true;
    bool muted = false;
    bool solo = false;
    bool frozen = false;
    bool freezeStale = false;
    juce::String frozenAudioPath;
    bool playing = false;
    int preparedBridge = -1;
};

struct LaneSnapshot
{
    juce::String id;
    juce::String name;
    juce::String script;
    float volume = 1.0f;
    bool frozen = false;
    bool freezeStale = false;
    juce::String frozenAudioPath;
};

struct State
{
    int index = 0;
    juce::String name;
    std::vector<Lane> lanes;
    double tempoBpm = 120.0;
    int beatsPerBar = 4;
    int beatUnit = 4;

    double secondsPerBar() const
    {
        const auto bpm = juce::jlimit (20.0, 320.0, tempoBpm);
        const auto beats = juce::jlimit (1, 32, beatsPerBar);
        const auto unit = juce::jlimit (1, 32, beatUnit);
        return (60.0 / bpm) * static_cast<double> (beats) * (4.0 / static_cast<double> (unit));
    }
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
        if (machineId == "root" && lanePrefix.isEmpty())
            configureRootDemo();
        else
            configureDefaultChildDemo();
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
                { makeLaneId (i, 0), "Lane 1", MarkovDemo::defaultScriptFor (i, 0) });
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
                             MarkovDemo::defaultScriptFor (selectedState, laneIndex) });
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

    void moveSelectedLane (int offset)
    {
        auto& s = state (selectedState);
        const auto count = static_cast<int> (s.lanes.size());
        const auto target = juce::jlimit (0, count - 1, selectedLane + offset);
        if (target == selectedLane)
            return;

        std::swap (s.lanes[static_cast<size_t> (selectedLane)], s.lanes[static_cast<size_t> (target)]);
        selectedLane = target;
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

    void configureRootDemo()
    {
        setStateCount (6);
        childMachines.clear();
        childMachines.resize (states.size());

        setStateDemo (0, "Autobahn", { { "Robot hook", "lead" }, { "Motor arp", "arp" }, { "Electric bass", "bassline" }, { "Drum machine", "softdrums" }, { "Vocoder chords", "chords" } });
        setStateDemo (1, "Downtown", { { "Neon lead", "lead" }, { "Answer pulse", "counter" }, { "Electric bass", "bassline" }, { "Drum machine", "softdrums" }, { "Vocoder chords", "chords" } });
        setStateDemo (2, "Lift", { { "Lift hook", "lead" }, { "Glass line", "shimmer" }, { "Counter pulse", "counter" }, { "Electric bass", "bassline" }, { "Drum machine", "softdrums" }, { "Lift chords", "chords" } });
        setStateDemo (3, "Chorus", { { "Main hook", "lead" }, { "Octave arp", "arp" }, { "Round bass", "bassline" }, { "Four on floor", "softdrums" }, { "Wide chords", "chords" } });
        setStateDemo (4, "Bridge", { { "Bridge hook", "lead" }, { "Answer pulse", "counter" }, { "Electric bass", "bassline" }, { "Drum machine", "softdrums" }, { "Bridge chords", "chords" }, { "Soft air", "texture" } });
        setStateDemo (5, "Reprise", { { "Final hook", "lead" }, { "Glass return", "shimmer" }, { "Low bass", "bassline" }, { "Final chords", "chords" } });
        setStateTiming (0, 104.0, 4, 4);
        setStateTiming (1, 106.0, 4, 4);
        setStateTiming (2, 108.0, 4, 4);
        setStateTiming (3, 110.0, 4, 4);
        setStateTiming (4, 102.0, 4, 4);
        setStateTiming (5, 104.0, 4, 4);

        rules = {
            { 0, 0, 6.0f }, { 0, 1, 1.0f },
            { 1, 1, 12.0f }, { 1, 2, 1.0f },
            { 2, 2, 6.0f }, { 2, 3, 1.0f },
            { 3, 3, 14.0f }, { 3, 4, 1.0f },
            { 4, 4, 8.0f }, { 4, 5, 1.0f },
            { 5, 5, 6.0f }, { 5, 0, 1.0f }
        };

        childMachines[1] = std::make_unique<MachineModel> (machineId + "_groove_child", lanePrefix + "groove-");
        configureGrooveChild (*childMachines[1]);

        childMachines[2] = std::make_unique<MachineModel> (machineId + "_bloom_child", lanePrefix + "bloom-");
        configureBloomChild (*childMachines[2]);

        childMachines[3] = std::make_unique<MachineModel> (machineId + "_fracture_child", lanePrefix + "fracture-");
        configureFractureChild (*childMachines[3]);

        selectedState = 0;
        selectedLane = 0;
    }

    void configureDefaultChildDemo()
    {
        setStateCount (4);
        childMachines.clear();
        childMachines.resize (states.size());
        setStateDemo (0, "Cell A", { { "Accent", "arp" } });
        setStateDemo (1, "Cell B", { { "Colour", "shimmer" } });
        setStateDemo (2, "Cell C", { { "Answer", "phrase" } });
        setStateDemo (3, "Cell D", { { "Bed", "texture" } });
        rules = { { 0, 1, 1.0f }, { 1, 2, 0.8f }, { 1, 3, 0.35f }, { 2, 0, 1.0f }, { 3, 0, 1.0f } };
    }

    void configureGrooveChild (MachineModel& child)
    {
        child.setStateCount (4);
        child.timingMode = NestedTimingMode::freeRun;
        child.parentDivision = 2;
        child.setStateDemo (0, "Motif A", { { "Tiny lead", "lead" }, { "Air", "texture" } });
        child.setStateDemo (1, "Motif B", { { "Tiny answer", "counter" }, { "Small chords", "chords" } });
        child.setStateDemo (2, "Motif C", { { "Bell lead", "lead" }, { "Upper reply", "shimmer" } });
        child.setStateDemo (3, "Motif D", { { "Turn line", "lead" } });
        child.rules = { { 0, 0, 4.0f }, { 0, 1, 1.0f }, { 1, 1, 4.0f }, { 1, 2, 1.0f }, { 2, 2, 4.0f }, { 2, 3, 1.0f }, { 3, 0, 1.0f } };

        child.childMachines[1] = std::make_unique<MachineModel> (child.machineId + "_figure_b_child", child.lanePrefix + "figB-");
        configureMicroArpChild (*child.childMachines[1], "Figure B Cells", NestedTimingMode::followParent, 2);

        child.childMachines[2] = std::make_unique<MachineModel> (child.machineId + "_figure_c_child", child.lanePrefix + "figC-");
        configureMicroAnswerChild (*child.childMachines[2], "Figure C Cells", NestedTimingMode::freeRun, 3);
    }

    void configureBloomChild (MachineModel& child)
    {
        child.setStateCount (3);
        child.timingMode = NestedTimingMode::followParent;
        child.parentDivision = 4;
        child.setStateDemo (0, "Hook fifth", { { "Fifth lead", "lead" } });
        child.setStateDemo (1, "High answer", { { "Glass answer", "shimmer" }, { "Air", "texture" } });
        child.setStateDemo (2, "Fold down", { { "Fold melody", "phrase" } });
        child.rules = { { 0, 1, 1.0f }, { 1, 1, 0.4f }, { 1, 2, 1.0f }, { 2, 0, 1.0f } };

        child.childMachines[1] = std::make_unique<MachineModel> (child.machineId + "_motes_child", child.lanePrefix + "motes-");
        configureMicroArpChild (*child.childMachines[1], "Mote Cells", NestedTimingMode::freeRun, 4);
    }

    void configureFractureChild (MachineModel& child)
    {
        child.setStateCount (5);
        child.timingMode = NestedTimingMode::followParent;
        child.parentDivision = 4;
        child.setStateDemo (0, "Question", { { "Question lead", "lead" } });
        child.setStateDemo (1, "Lift", { { "Lift lead", "lead" }, { "Edge shimmer", "shimmer" } });
        child.setStateDemo (2, "Drop", { { "Drop melody", "phrase" }, { "Drop answer", "counter" } });
        child.setStateDemo (3, "Suspension", { { "Suspended lead", "lead" }, { "Chord shade", "chords" } });
        child.setStateDemo (4, "Exit", { { "Exit lead", "lead" } });
        child.rules = { { 0, 1, 1.0f }, { 1, 2, 0.75f }, { 1, 3, 0.8f }, { 2, 4, 1.0f }, { 3, 4, 1.0f }, { 4, 0, 1.0f } };

        child.childMachines[1] = std::make_unique<MachineModel> (child.machineId + "_lift_child", child.lanePrefix + "lift-");
        configureMicroArpChild (*child.childMachines[1], "Lift Cells", NestedTimingMode::oneShot, 1);

        child.childMachines[3] = std::make_unique<MachineModel> (child.machineId + "_suspension_child", child.lanePrefix + "susp-");
        configureMicroAnswerChild (*child.childMachines[3], "Suspension Cells", NestedTimingMode::followParent, 2);

        child.entryState = 0;
        child.selectedState = 0;
        child.selectedLane = 0;
    }

    void configureMicroArpChild (MachineModel& child, const juce::String& name, NestedTimingMode mode, int division)
    {
        juce::ignoreUnused (name);
        child.setStateCount (3);
        child.timingMode = mode;
        child.parentDivision = division;
        child.entryState = 0;
        child.setStateDemo (0, "Spark", { { "Tiny hook", "lead" } });
        child.setStateDemo (1, "Fold", { { "Glass hook", "shimmer" } });
        child.setStateDemo (2, "Return", { { "Hook return", "phrase" } });
        child.rules = { { 0, 0, 2.5f }, { 0, 1, 1.0f }, { 1, 1, 1.8f }, { 1, 2, 1.0f }, { 2, 0, 1.0f } };
        child.setAllLaneVolumes (0.42f);
    }

    void configureMicroAnswerChild (MachineModel& child, const juce::String& name, NestedTimingMode mode, int division)
    {
        juce::ignoreUnused (name);
        child.setStateCount (4);
        child.timingMode = mode;
        child.parentDivision = division;
        child.entryState = 0;
        child.setStateDemo (0, "Answer A", { { "Answer hook", "lead" } });
        child.setStateDemo (1, "Answer B", { { "Small melody", "phrase" } });
        child.setStateDemo (2, "Answer C", { { "Glass reply", "shimmer" } });
        child.setStateDemo (3, "Rest", { { "Thin air", "texture" } });
        child.rules = { { 0, 1, 1.0f }, { 1, 2, 0.8f }, { 1, 3, 0.35f }, { 2, 0, 1.0f }, { 3, 0, 1.0f } };
        child.setAllLaneVolumes (0.36f);
    }

    void setAllLaneVolumes (float volume)
    {
        const auto clipped = juce::jlimit (0.0f, 1.0f, volume);
        for (auto& stateToScale : states)
            for (auto& lane : stateToScale.lanes)
                lane.volume = clipped;
    }

    void setStateDemo (int stateIndex, std::initializer_list<std::pair<const char*, const char*>> laneDefs)
    {
        auto& s = state (stateIndex);
        s.name = s.name.isEmpty() ? "State " + juce::String (stateIndex + 1) : s.name;
        s.lanes.clear();
        int laneIndex = 0;
        for (const auto& lane : laneDefs)
        {
            auto role = juce::String (lane.second);
            Lane demoLane { makeLaneId (stateIndex, laneIndex),
                            lane.first,
                            MarkovDemo::scriptForRole (role, stateIndex, laneIndex) };
            demoLane.volume = MarkovDemo::volumeForRole (role);
            s.lanes.push_back (std::move (demoLane));
            ++laneIndex;
        }

        if (s.lanes.empty())
            s.lanes.push_back ({ makeLaneId (stateIndex, 0), "Lane 1", MarkovDemo::defaultScriptFor (stateIndex, 0) });
    }

    void setStateDemo (int stateIndex, const juce::String& name, std::initializer_list<std::pair<const char*, const char*>> laneDefs)
    {
        state (stateIndex).name = name;
        setStateDemo (stateIndex, laneDefs);
    }

    void setStateTiming (int stateIndex, double bpm, int beats, int unit)
    {
        auto& s = state (stateIndex);
        s.tempoBpm = juce::jlimit (20.0, 320.0, bpm);
        s.beatsPerBar = juce::jlimit (1, 32, beats);
        s.beatUnit = juce::jlimit (1, 32, unit);
    }

    std::vector<State> states;
    std::vector<std::unique_ptr<MachineModel>> childMachines;
    std::vector<Rule> rules;
    juce::String machineId;
    juce::String lanePrefix;
    NestedTimingMode timingMode = NestedTimingMode::followParent;
    int parentDivision = 1;
    int parentTickCounter = 0;
    bool oneShotComplete = false;
    bool latchedActive = false;
    int selectedState = 0;
    int selectedLane = 0;
    int entryState = 0;
    int stepsSinceEntry = 0;
};
