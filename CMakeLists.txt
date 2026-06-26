cmake_minimum_required(VERSION 3.22)

project(PipeGridMusic VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.14
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(PipeGridMusic
    PRODUCT_NAME "Pipe Grid Music"
    COMPANY_NAME "Codex"
    BUNDLE_ID "com.codex.pipegridmusic"
)

target_sources(PipeGridMusic
    PRIVATE
        Source/Main.cpp
)

target_compile_definitions(PipeGridMusic
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:PipeGridMusic,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:PipeGridMusic,JUCE_VERSION>"
)

target_link_libraries(PipeGridMusic
    PRIVATE
        juce::juce_audio_utils
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
