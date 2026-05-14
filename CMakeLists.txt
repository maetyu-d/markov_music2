cmake_minimum_required(VERSION 3.22)

project(MarkovFSM VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JUCE_PATH "../Granny/JUCE" CACHE PATH "Path to a local JUCE checkout")
add_subdirectory("${JUCE_PATH}" JUCE)

juce_add_gui_app(MarkovFSM
    PRODUCT_NAME "Markov FSM"
    COMPANY_NAME "Markov"
    BUNDLE_ID "com.markov.fsm")

target_sources(MarkovFSM
    PRIVATE
        Source/DemoScripts.cpp
        Source/SuperColliderHost.cpp
        Source/Main.cpp)

target_compile_definitions(MarkovFSM
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)

target_link_libraries(MarkovFSM
    PRIVATE
        juce::juce_gui_extra
        juce::juce_osc
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
