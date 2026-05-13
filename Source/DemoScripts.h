#pragma once

#include <juce_core/juce_core.h>

namespace MarkovDemo
{
juce::String defaultScriptFor (int stateIndex, int laneIndex);
juce::String scriptForRole (const juce::String& role, int stateIndex, int laneIndex);
float volumeForRole (const juce::String& role);
}
