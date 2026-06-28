#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>

namespace gridcollider
{
struct CellCoordinate
{
    int column = 0;
    int row = 0;
};

using ParameterMap = std::map<std::string, juce::String>;

struct EventFields
{
    double timestampSeconds = 0.0;
    std::uint64_t tick = 0;
    CellCoordinate sourceCell;
    juce::String instrumentName;
    int pitch = -1;
    float velocity = 0.0f;
    std::uint64_t durationTicks = 0;
    ParameterMap parameters;
    std::optional<juce::String> targetAddress;
};

struct NoteEvent
{
    EventFields fields;
};

struct ControlEvent
{
    EventFields fields;
    juce::String parameterName;
    float value = 0.0f;
};

struct TriggerEvent
{
    EventFields fields;
    juce::String triggerName;
};

struct BusRouteEvent
{
    EventFields fields;
    juce::String payload;
};

struct GridMutationEvent
{
    EventFields fields;
    CellCoordinate targetCell;
    char previousGlyph = '.';
    char newGlyph = '.';
};

struct LogEvent
{
    EventFields fields;
    juce::String message;
};

using InternalEvent = std::variant<NoteEvent,
                                   ControlEvent,
                                   TriggerEvent,
                                   BusRouteEvent,
                                   GridMutationEvent,
                                   LogEvent>;
}
