#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "EmbeddedScAudioEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

namespace
{
constexpr int gridSize = 12;
constexpr int voxelCount = gridSize * gridSize * gridSize;

constexpr uint8_t bitPX = 1u << 0;
constexpr uint8_t bitNX = 1u << 1;
constexpr uint8_t bitPY = 1u << 2;
constexpr uint8_t bitNY = 1u << 3;
constexpr uint8_t bitPZ = 1u << 4;
constexpr uint8_t bitNZ = 1u << 5;

struct IVec3
{
    int x = 0;
    int y = 0;
    int z = 0;

    bool operator== (const IVec3& other) const noexcept
    {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct FVec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

FVec3 toFloatVector (IVec3 value) noexcept
{
    return { (float) value.x, (float) value.y, (float) value.z };
}

struct Direction
{
    uint8_t bit = 0;
    IVec3 vector;
};

constexpr std::array<Direction, 6> directions {{
    { bitPX, {  1,  0,  0 } },
    { bitNX, { -1,  0,  0 } },
    { bitPY, {  0,  1,  0 } },
    { bitNY, {  0, -1,  0 } },
    { bitPZ, {  0,  0,  1 } },
    { bitNZ, {  0,  0, -1 } }
}};

struct FaceDef
{
    const char* name = "";
    IVec3 normal;
    IVec3 u;
    IVec3 v;
};

constexpr std::array<FaceDef, 6> faces {{
    { "Front",  {  0,  0,  1 }, {  1,  0,  0 }, {  0,  1,  0 } },
    { "Right",  {  1,  0,  0 }, {  0,  0, -1 }, {  0,  1,  0 } },
    { "Back",   {  0,  0, -1 }, { -1,  0,  0 }, {  0,  1,  0 } },
    { "Left",   { -1,  0,  0 }, {  0,  0,  1 }, {  0,  1,  0 } },
    { "Top",    {  0,  1,  0 }, {  1,  0,  0 }, {  0,  0, -1 } },
    { "Bottom", {  0, -1,  0 }, {  1,  0,  0 }, {  0,  0,  1 } }
}};

enum class Tool
{
    select,
    pipe,
    tap,
    valve,
    drain,
    erase
};

enum class SoundMode
{
    internal,
    superCollider
};

struct Cell
{
    int col = 0;
    int row = 0;

    bool operator== (const Cell& other) const noexcept
    {
        return col == other.col && row == other.row;
    }
};

struct Flow
{
    IVec3 position;
    FVec3 fallPosition;
    uint8_t direction = 0;
    double progress = 0.0;
    bool falling = false;
    bool dead = false;
};

struct Valve
{
    IVec3 position;
    int midiNote = 60;
};

int signOf (int value) noexcept
{
    return (value > 0 ? 1 : 0) - (value < 0 ? 1 : 0);
}

IVec3 operator+ (IVec3 a, IVec3 b) noexcept
{
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}

IVec3 operator- (IVec3 a, IVec3 b) noexcept
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}

IVec3 operator- (IVec3 value) noexcept
{
    return { -value.x, -value.y, -value.z };
}

int manhattan (IVec3 value) noexcept
{
    return std::abs (value.x) + std::abs (value.y) + std::abs (value.z);
}

bool isInside (IVec3 value) noexcept
{
    return value.x >= 0 && value.x < gridSize
        && value.y >= 0 && value.y < gridSize
        && value.z >= 0 && value.z < gridSize;
}

int indexOf (IVec3 value) noexcept
{
    return value.x + gridSize * (value.y + gridSize * value.z);
}

IVec3 vectorForBit (uint8_t bit) noexcept
{
    for (const auto& direction : directions)
        if (direction.bit == bit)
            return direction.vector;

    return {};
}

uint8_t bitForVector (IVec3 vector) noexcept
{
    for (const auto& direction : directions)
        if (direction.vector == vector)
            return direction.bit;

    return 0;
}

uint8_t oppositeBit (uint8_t bit) noexcept
{
    switch (bit)
    {
        case bitPX: return bitNX;
        case bitNX: return bitPX;
        case bitPY: return bitNY;
        case bitNY: return bitPY;
        case bitPZ: return bitNZ;
        case bitNZ: return bitPZ;
        default:    return 0;
    }
}

int coordinateOnAxis (IVec3 point, IVec3 axis) noexcept
{
    if (axis.x != 0) return point.x;
    if (axis.y != 0) return point.y;
    return point.z;
}

void setCoordinateOnAxis (IVec3& point, IVec3 axis, int value) noexcept
{
    if (axis.x != 0) point.x = value;
    else if (axis.y != 0) point.y = value;
    else point.z = value;
}

int coordinateFromDisplayValue (IVec3 axis, int value, bool screenRow) noexcept
{
    const auto sign = signOf (coordinateOnAxis (axis, axis));

    if (screenRow)
        return sign > 0 ? gridSize - 1 - value : value;

    return sign > 0 ? value : gridSize - 1 - value;
}

int displayValueFromCoordinate (IVec3 axis, int coordinate, bool screenRow) noexcept
{
    const auto sign = signOf (coordinateOnAxis (axis, axis));

    if (screenRow)
        return sign > 0 ? gridSize - 1 - coordinate : coordinate;

    return sign > 0 ? coordinate : gridSize - 1 - coordinate;
}

int layerCoordinate (const FaceDef& face, int depth) noexcept
{
    const auto sign = signOf (coordinateOnAxis (face.normal, face.normal));
    return sign > 0 ? gridSize - 1 - depth : depth;
}

IVec3 cellToVoxel (int faceIndex, int depth, Cell cell) noexcept
{
    const auto& face = faces[(size_t) faceIndex];
    IVec3 voxel;

    setCoordinateOnAxis (voxel, face.normal, layerCoordinate (face, depth));
    setCoordinateOnAxis (voxel, face.u, coordinateFromDisplayValue (face.u, cell.col, false));
    setCoordinateOnAxis (voxel, face.v, coordinateFromDisplayValue (face.v, cell.row, true));

    return voxel;
}

int voxelDepthForFace (int faceIndex, IVec3 voxel) noexcept
{
    const auto& face = faces[(size_t) faceIndex];
    const auto coord = coordinateOnAxis (voxel, face.normal);
    const auto sign = signOf (coordinateOnAxis (face.normal, face.normal));
    return sign > 0 ? gridSize - 1 - coord : coord;
}

std::optional<Cell> voxelToCellOnFaceDepth (int faceIndex, int depth, IVec3 voxel) noexcept
{
    if (! isInside (voxel) || voxelDepthForFace (faceIndex, voxel) != depth)
        return std::nullopt;

    const auto& face = faces[(size_t) faceIndex];
    return Cell {
        displayValueFromCoordinate (face.u, coordinateOnAxis (voxel, face.u), false),
        displayValueFromCoordinate (face.v, coordinateOnAxis (voxel, face.v), true)
    };
}

Cell voxelToCellOnFace (int faceIndex, IVec3 voxel) noexcept
{
    const auto& face = faces[(size_t) faceIndex];
    return {
        displayValueFromCoordinate (face.u, coordinateOnAxis (voxel, face.u), false),
        displayValueFromCoordinate (face.v, coordinateOnAxis (voxel, face.v), true)
    };
}

std::optional<juce::Point<float>> displayDirectionForBit (int faceIndex, uint8_t bit)
{
    const auto vector = vectorForBit (bit);
    const auto& face = faces[(size_t) faceIndex];

    if (vector == face.u)  return juce::Point<float> {  1.0f,  0.0f };
    if (vector == -face.u) return juce::Point<float> { -1.0f,  0.0f };
    if (vector == face.v)  return juce::Point<float> {  0.0f, -1.0f };
    if (vector == -face.v) return juce::Point<float> {  0.0f,  1.0f };

    return std::nullopt;
}

bool isTowardViewer (int faceIndex, uint8_t bit) noexcept
{
    return vectorForBit (bit) == faces[(size_t) faceIndex].normal;
}

bool isAwayFromViewer (int faceIndex, uint8_t bit) noexcept
{
    return vectorForBit (bit) == -faces[(size_t) faceIndex].normal;
}

void erasePoint (std::vector<IVec3>& points, IVec3 point)
{
    points.erase (std::remove (points.begin(), points.end(), point), points.end());
}

bool containsPoint (const std::vector<IVec3>& points, IVec3 point)
{
    return std::find (points.begin(), points.end(), point) != points.end();
}

void eraseValve (std::vector<Valve>& valves, IVec3 point)
{
    valves.erase (std::remove_if (valves.begin(), valves.end(),
                                  [point] (const Valve& valve) { return valve.position == point; }),
                  valves.end());
}

Valve* findValve (std::vector<Valve>& valves, IVec3 point)
{
    const auto iter = std::find_if (valves.begin(), valves.end(),
                                    [point] (const Valve& valve) { return valve.position == point; });
    return iter == valves.end() ? nullptr : &(*iter);
}

const Valve* findValve (const std::vector<Valve>& valves, IVec3 point)
{
    const auto iter = std::find_if (valves.begin(), valves.end(),
                                    [point] (const Valve& valve) { return valve.position == point; });
    return iter == valves.end() ? nullptr : &(*iter);
}

int defaultValveNoteFor (IVec3 voxel)
{
    static constexpr std::array<int, 7> scale { 0, 2, 3, 5, 7, 9, 10 };
    const auto degree = (voxel.x + voxel.z * 2 + voxel.y) % (int) scale.size();
    const auto octave = juce::jlimit (0, 2, voxel.y / 4);
    return 45 + octave * 12 + scale[(size_t) degree];
}

class PipeNetwork
{
public:
    std::array<uint8_t, voxelCount> pipes {};
    std::vector<IVec3> taps;
    std::vector<IVec3> disabledTaps;
    std::vector<Valve> valves;
    std::vector<IVec3> drains;
    std::vector<IVec3> closedDrains;

    void clear()
    {
        pipes.fill (0);
        taps.clear();
        disabledTaps.clear();
        valves.clear();
        drains.clear();
        closedDrains.clear();
    }

    bool hasPipe (IVec3 voxel) const noexcept
    {
        return isInside (voxel) && pipes[(size_t) indexOf (voxel)] != 0;
    }

    bool hasTap (IVec3 voxel) const
    {
        return containsPoint (taps, voxel);
    }

    bool isTapEnabled (IVec3 voxel) const
    {
        return hasTap (voxel) && ! containsPoint (disabledTaps, voxel);
    }

    bool hasValve (IVec3 voxel) const
    {
        return findValve (valves, voxel) != nullptr;
    }

    Valve* valveAt (IVec3 voxel)
    {
        return findValve (valves, voxel);
    }

    const Valve* valveAt (IVec3 voxel) const
    {
        return findValve (valves, voxel);
    }

    bool hasDrain (IVec3 voxel) const
    {
        return containsPoint (drains, voxel);
    }

    bool isDrainOpen (IVec3 voxel) const
    {
        return hasDrain (voxel) && ! containsPoint (closedDrains, voxel);
    }

    void toggleTap (IVec3 voxel)
    {
        if (hasTap (voxel))
        {
            erasePoint (taps, voxel);
            erasePoint (disabledTaps, voxel);
        }
        else
        {
            eraseValve (valves, voxel);
            erasePoint (drains, voxel);
            erasePoint (closedDrains, voxel);
            taps.push_back (voxel);
            erasePoint (disabledTaps, voxel);
        }
    }

    bool toggleTapEnabled (IVec3 voxel)
    {
        if (! hasTap (voxel))
            return false;

        if (containsPoint (disabledTaps, voxel))
            erasePoint (disabledTaps, voxel);
        else
            disabledTaps.push_back (voxel);

        return isTapEnabled (voxel);
    }

    void toggleValve (IVec3 voxel)
    {
        if (hasValve (voxel))
            eraseValve (valves, voxel);
        else
        {
            erasePoint (taps, voxel);
            erasePoint (disabledTaps, voxel);
            erasePoint (drains, voxel);
            erasePoint (closedDrains, voxel);
            valves.push_back ({ voxel, defaultValveNoteFor (voxel) });
        }
    }

    void toggleDrain (IVec3 voxel)
    {
        if (hasDrain (voxel))
        {
            erasePoint (drains, voxel);
            erasePoint (closedDrains, voxel);
        }
        else
        {
            erasePoint (taps, voxel);
            erasePoint (disabledTaps, voxel);
            eraseValve (valves, voxel);
            drains.push_back (voxel);
            erasePoint (closedDrains, voxel);
        }
    }

    bool toggleDrainOpen (IVec3 voxel)
    {
        if (! hasDrain (voxel))
            return false;

        if (containsPoint (closedDrains, voxel))
            erasePoint (closedDrains, voxel);
        else
            closedDrains.push_back (voxel);

        return isDrainOpen (voxel);
    }

    bool connectAdjacent (IVec3 a, IVec3 b)
    {
        if (! isInside (a) || ! isInside (b))
            return false;

        const auto delta = b - a;
        if (manhattan (delta) != 1)
            return false;

        const auto bit = bitForVector (delta);
        const auto opposite = oppositeBit (bit);
        const auto aIndex = (size_t) indexOf (a);
        const auto bIndex = (size_t) indexOf (b);
        const auto beforeA = pipes[aIndex];
        const auto beforeB = pipes[bIndex];

        pipes[aIndex] = (uint8_t) (pipes[aIndex] | bit);
        pipes[bIndex] = (uint8_t) (pipes[bIndex] | opposite);

        return pipes[aIndex] != beforeA || pipes[bIndex] != beforeB;
    }

    void eraseVoxel (IVec3 voxel)
    {
        if (! isInside (voxel))
            return;

        const auto index = (size_t) indexOf (voxel);
        const auto mask = pipes[index];

        for (const auto& direction : directions)
        {
            if ((mask & direction.bit) == 0)
                continue;

            const auto neighbour = voxel + direction.vector;
            if (isInside (neighbour))
                pipes[(size_t) indexOf (neighbour)] = (uint8_t) (pipes[(size_t) indexOf (neighbour)] & ~oppositeBit (direction.bit));
        }

        pipes[index] = 0;
        erasePoint (taps, voxel);
        erasePoint (disabledTaps, voxel);
        eraseValve (valves, voxel);
        erasePoint (drains, voxel);
        erasePoint (closedDrains, voxel);
    }

    std::vector<uint8_t> validConnectedBits (IVec3 voxel, uint8_t excludedBit = 0) const
    {
        std::vector<uint8_t> result;

        if (! isInside (voxel))
            return result;

        const auto mask = pipes[(size_t) indexOf (voxel)];

        for (const auto& direction : directions)
        {
            if (direction.bit == excludedBit || (mask & direction.bit) == 0)
                continue;

            const auto neighbour = voxel + direction.vector;
            if (! isInside (neighbour))
                continue;

            const auto neighbourMask = pipes[(size_t) indexOf (neighbour)];
            if ((neighbourMask & oppositeBit (direction.bit)) != 0)
                result.push_back (direction.bit);
        }

        return result;
    }

    bool isMatchedPort (IVec3 voxel, uint8_t bit) const
    {
        if (! isInside (voxel) || (pipes[(size_t) indexOf (voxel)] & bit) == 0)
            return false;

        const auto neighbour = voxel + vectorForBit (bit);
        return isInside (neighbour)
            && (pipes[(size_t) indexOf (neighbour)] & oppositeBit (bit)) != 0;
    }

    int occupiedCount() const
    {
        return (int) std::count_if (pipes.begin(), pipes.end(), [] (uint8_t mask) { return mask != 0; });
    }

    int armCount() const
    {
        int total = 0;

        for (const auto mask : pipes)
            for (const auto& direction : directions)
                if ((mask & direction.bit) != 0)
                    ++total;

        return total;
    }

    void connectLine (IVec3 a, IVec3 b)
    {
        auto cursor = a;

        while (cursor.x != b.x)
        {
            auto next = cursor;
            next.x += signOf (b.x - cursor.x);
            connectAdjacent (cursor, next);
            cursor = next;
        }

        while (cursor.y != b.y)
        {
            auto next = cursor;
            next.y += signOf (b.y - cursor.y);
            connectAdjacent (cursor, next);
            cursor = next;
        }

        while (cursor.z != b.z)
        {
            auto next = cursor;
            next.z += signOf (b.z - cursor.z);
            connectAdjacent (cursor, next);
            cursor = next;
        }
    }

    void loadDemo()
    {
        clear();

        connectLine ({ 1, 9, 11 }, { 10, 9, 11 });
        connectLine ({ 10, 9, 11 }, { 10, 3, 11 });
        connectLine ({ 10, 3, 11 }, { 3, 3, 11 });
        connectLine ({ 3, 3, 11 }, { 3, 7, 11 });

        connectLine ({ 6, 9, 11 }, { 6, 9, 5 });
        connectLine ({ 6, 9, 5 }, { 2, 9, 5 });
        connectLine ({ 6, 9, 5 }, { 9, 9, 5 });
        connectLine ({ 9, 9, 5 }, { 9, 5, 5 });

        connectLine ({ 3, 7, 11 }, { 3, 7, 8 });
        connectLine ({ 3, 7, 8 }, { 8, 7, 8 });

        taps = { { 1, 9, 11 }, { 10, 3, 11 }, { 2, 9, 5 } };
        disabledTaps.clear();
        valves = {
            { { 4, 9, 11 }, defaultValveNoteFor ({ 4, 9, 11 }) },
            { { 8, 9, 11 }, defaultValveNoteFor ({ 8, 9, 11 }) },
            { { 10, 6, 11 }, defaultValveNoteFor ({ 10, 6, 11 }) },
            { { 6, 3, 11 }, defaultValveNoteFor ({ 6, 3, 11 }) },
            { { 6, 9, 8 }, defaultValveNoteFor ({ 6, 9, 8 }) },
            { { 9, 7, 5 }, defaultValveNoteFor ({ 9, 7, 5 }) },
            { { 8, 7, 8 }, defaultValveNoteFor ({ 8, 7, 8 }) }
        };
        drains = { { 10, 9, 11 }, { 6, 9, 5 } };
        closedDrains.clear();
    }
};

struct Voice
{
    double frequency = 440.0;
    double phase = 0.0;
    double age = 0.0;
    double level = 0.075;
    double brightness = 0.12;
    double pan = 0.0;
};

struct NoteEvent
{
    double frequency = 440.0;
    double level = 0.075;
    double brightness = 0.12;
    double pan = 0.0;
};

class PipeLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    PipeLookAndFeel()
    {
        setColour (juce::Slider::thumbColourId, lemon);
        setColour (juce::Slider::trackColourId, mint.withAlpha (0.72f));
        setColour (juce::Slider::backgroundColourId, lineDim);
        setColour (juce::ComboBox::backgroundColourId, panel2);
        setColour (juce::ComboBox::textColourId, ink);
        setColour (juce::ComboBox::outlineColourId, lavender.withAlpha (0.55f));
        setColour (juce::PopupMenu::backgroundColourId, panel2);
        setColour (juce::PopupMenu::textColourId, ink);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, pink.withAlpha (0.20f));
    }

    void drawButtonBackground (juce::Graphics& g,
                               juce::Button& button,
                               const juce::Colour&,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override
    {
        const auto bounds = button.getLocalBounds().toFloat().reduced (1.0f);
        const auto active = button.getToggleState();
        const auto rainbow = colourForButton (button.getButtonText());
        auto fill = active ? rainbow.withAlpha (0.22f) : panel2.withAlpha (0.78f);

        if (shouldDrawButtonAsDown)
            fill = fill.brighter (0.12f);
        else if (shouldDrawButtonAsHighlighted)
            fill = fill.brighter (0.08f);

        g.setColour (fill);
        g.fillRoundedRectangle (bounds, 14.0f);
        g.setColour (active ? rainbow.withAlpha (0.92f) : rainbow.withAlpha (0.26f));
        g.drawRoundedRectangle (bounds, 14.0f, active ? 2.0f : 1.1f);

        if (active)
        {
            g.setColour (rainbow.withAlpha (0.16f));
            g.fillEllipse (juce::Rectangle<float> (bounds.getX() + 8.0f,
                                                   bounds.getCentreY() - 5.0f,
                                                   10.0f,
                                                   10.0f));
        }
    }

    void drawButtonText (juce::Graphics& g,
                         juce::TextButton& button,
                         bool,
                         bool) override
    {
        g.setFont (juce::FontOptions (12.0f).withStyle ("Bold"));
        g.setColour (button.getToggleState() ? colourForButton (button.getButtonText()).brighter (0.22f) : muted);
        g.drawFittedText (button.getButtonText(),
                          button.getLocalBounds().reduced (4, 2),
                          juce::Justification::centred,
                          1);
    }

    static juce::Colour colourForButton (const juce::String& text)
    {
        if (text == "SELECT") return mint;
        if (text == "PIPE")  return lemon;
        if (text == "TAP")   return aqua;
        if (text == "VALVE") return pink;
        if (text == "DRAIN") return coral;
        if (text == "ERASE") return lavender;
        if (text == "PLAY")  return mint;
        if (text == "STOP")  return coral;
        if (text == "DEMO")  return sky;
        if (text == "CLEAR") return lavender;
        if (text == "Front") return lemon;
        if (text == "Right") return aqua;
        if (text == "Back") return pink;
        if (text == "Left") return mint;
        if (text == "Top") return sky;
        if (text == "Bottom") return lavender;
        return accent;
    }

    static juce::Colour rainbowForIndex (int index)
    {
        static constexpr std::array<uint32_t, 7> colours {{
            0xffff7aa8, 0xffff9d72, 0xffffdd6d, 0xff8ef6a3,
            0xff58d8ff, 0xff9aa7ff, 0xffd58cff
        }};

        return juce::Colour (colours[(size_t) (std::abs (index) % (int) colours.size())]);
    }

    static inline const juce::Colour bg       { 0xff11111f };
    static inline const juce::Colour panel    { 0xff171729 };
    static inline const juce::Colour panel2   { 0xff202038 };
    static inline const juce::Colour line     { 0xff353553 };
    static inline const juce::Colour lineDim  { 0xff262642 };
    static inline const juce::Colour ink      { 0xfffff9f0 };
    static inline const juce::Colour muted    { 0xffaaa6c8 };
    static inline const juce::Colour accent   { 0xffffdd6d };
    static inline const juce::Colour water    { 0xff58d8ff };
    static inline const juce::Colour danger   { 0xffff7f8a };
    static inline const juce::Colour mint     { 0xff8ef6a3 };
    static inline const juce::Colour aqua     { 0xff58d8ff };
    static inline const juce::Colour sky      { 0xff9aa7ff };
    static inline const juce::Colour lavender { 0xffd58cff };
    static inline const juce::Colour pink     { 0xffff7aa8 };
    static inline const juce::Colour coral    { 0xffff9d72 };
    static inline const juce::Colour lemon    { 0xffffdd6d };
};

class MainComponent final : public juce::AudioAppComponent,
                            public juce::MenuBarModel,
                            private juce::Timer,
                            private juce::Button::Listener,
                            private juce::Slider::Listener,
                            private juce::ComboBox::Listener
{
public:
    MainComponent()
    {
        setLookAndFeel (&lookAndFeel);
        setOpaque (true);
        setWantsKeyboardFocus (true);
        setSize (1180, 760);

        faceBox.addItem ("Front", 1);
        faceBox.addItem ("Right", 2);
        faceBox.addItem ("Back", 3);
        faceBox.addItem ("Left", 4);
        faceBox.addItem ("Top", 5);
        faceBox.addItem ("Bottom", 6);
        faceBox.setSelectedId (1, juce::dontSendNotification);
        faceBox.addListener (this);
        addAndMakeVisible (faceBox);
        faceBox.setVisible (false);

        static constexpr std::array<const char*, 12> noteNames {{
            "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"
        }};
        for (int i = 0; i < (int) noteNames.size(); ++i)
            keyBox.addItem (noteNames[(size_t) i], i + 1);
        keyBox.setSelectedId (1, juce::dontSendNotification);
        keyBox.addListener (this);
        addAndMakeVisible (keyBox);

        scaleBox.addItem ("Major", 1);
        scaleBox.addItem ("Minor", 2);
        scaleBox.addItem ("Pentatonic", 3);
        scaleBox.addItem ("Dorian", 4);
        scaleBox.addItem ("Whole", 5);
        scaleBox.setSelectedId (3, juce::dontSendNotification);
        scaleBox.addListener (this);
        addAndMakeVisible (scaleBox);

        scProgramEditor.setMultiLine (true);
        scProgramEditor.setReturnKeyStartsNewLine (true);
        scProgramEditor.setScrollbarsShown (true);
        scProgramEditor.setFont (juce::FontOptions (11.0f));
        scProgramEditor.setColour (juce::TextEditor::backgroundColourId, PipeLookAndFeel::panel2.withAlpha (0.70f));
        scProgramEditor.setColour (juce::TextEditor::textColourId, PipeLookAndFeel::ink);
        scProgramEditor.setColour (juce::TextEditor::outlineColourId, PipeLookAndFeel::line.withAlpha (0.55f));
        scProgramEditor.setText (defaultScProgram(), juce::dontSendNotification);
        scProgramEditor.setVisible (false);
        addAndMakeVisible (scProgramEditor);

        for (int i = 0; i < (int) faceButtons.size(); ++i)
            setupButton (faceButtons[(size_t) i], faces[(size_t) i].name, "Show this cube face in the 2D editor");

        setupSlider (layerSlider, 1.0, (double) gridSize, 1.0, 1.0);
        setupSlider (tempoSlider, 40.0, 220.0, 1.0, 120.0);
        setupSlider (noteSlider, 24.0, 96.0, 1.0, 60.0);

        setupButton (selectButton, "SELECT", "Inspect cells without changing the pipework");
        setupButton (pipeButton, "PIPE", "Draw connected pipe paths");
        setupButton (tapButton, "TAP", "Add or remove a water source");
        setupButton (valveButton, "VALVE", "Add or remove a sounding point");
        setupButton (drainButton, "DRAIN", "Add or remove a drain outlet");
        setupButton (eraseButton, "ERASE", "Erase a pipe cell and its markers");
        setupButton (playButton, "PLAY", "Start water flow");
        setupButton (stopButton, "STOP", "Stop water flow");
        setupButton (demoButton, "DEMO", "Reload the starter patch");
        setupButton (clearButton, "CLEAR", "Clear every pipe, tap, valve and drain");
        setupButton (noteDownButton, "NOTE -", "Lower the selected valve note");
        setupButton (noteUpButton, "NOTE +", "Raise the selected valve note");
        setupButton (compileScButton, "APPLY SC", "Compile the SuperCollider program for SC mode");
        noteSlider.setTooltip ("Tune the selected valve");
        noteDownButton.setVisible (false);
        noteUpButton.setVisible (false);
        noteSlider.setVisible (false);
        compileScButton.setVisible (false);

        network.loadDemo();
        updateButtonStates();
        juce::MenuBarModel::setMacMainMenu (this);
        startTimerHz (60);
        setAudioChannels (0, 2);
    }

    ~MainComponent() override
    {
        juce::MenuBarModel::setMacMainMenu (nullptr);
        shutdownAudio();
        setLookAndFeel (nullptr);
    }

    juce::StringArray getMenuBarNames() override
    {
        return { "File", "Mode", "View" };
    }

    juce::PopupMenu getMenuForIndex (int, const juce::String& menuName) override
    {
        juce::PopupMenu menu;

        if (menuName == "File")
        {
            menu.addItem (savePatchMenuId, "Save Patch...");
            menu.addItem (loadPatchMenuId, "Load Patch...");
        }
        else if (menuName == "Mode")
        {
            menu.addItem (internalSoundModeMenuId, "JUCE Sound", true, soundMode == SoundMode::internal);
            menu.addItem (superColliderSoundModeMenuId, "SuperCollider Sound", true, soundMode == SoundMode::superCollider);
        }
        else if (menuName == "View")
        {
            menu.addItem (mainViewMenuId, "Main View", true, ! large3DView);
            menu.addItem (large3DViewMenuId, "Large 3D View", true, large3DView);
        }

        return menu;
    }

    void menuItemSelected (int menuItemID, int) override
    {
        if (menuItemID == savePatchMenuId)
            savePatchAs();
        else if (menuItemID == loadPatchMenuId)
            loadPatch();
        else if (menuItemID == internalSoundModeMenuId)
            setSoundMode (SoundMode::internal);
        else if (menuItemID == superColliderSoundModeMenuId)
            setSoundMode (SoundMode::superCollider);
        else if (menuItemID == mainViewMenuId)
            setLarge3DView (false);
        else if (menuItemID == large3DViewMenuId)
            setLarge3DView (true);
    }

    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override
    {
        currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
        voices.clear();
        scScratch.setSize (2, juce::jmax (64, samplesPerBlockExpected));

        if (scEngine.prepare (currentSampleRate, scScratch.getNumSamples(), 2))
        {
            scEngine.setMasterLevel (0.82f);
            compileScProgram();
        }
    }

    void releaseResources() override
    {
        voices.clear();
        scEngine.release();
        scScratch.setSize (0, 0);
    }

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill) override
    {
        bufferToFill.clearActiveBufferRegion();

        if (soundMode == SoundMode::superCollider && scEngine.isReady())
        {
            scEngine.setTransport (bpm,
                                   (std::uint64_t) juce::jmax (0.0, animationPhase * bpm / 60.0),
                                   playing);
            scScratch.setSize (bufferToFill.buffer->getNumChannels(), bufferToFill.numSamples, false, false, true);
            scEngine.render (scScratch);

            for (int channel = 0; channel < bufferToFill.buffer->getNumChannels(); ++channel)
                bufferToFill.buffer->addFrom (channel, bufferToFill.startSample, scScratch, channel, 0, bufferToFill.numSamples);

            return;
        }

        std::vector<NoteEvent> pending;
        {
            const juce::ScopedLock lock (noteLock);
            pending.swap (pendingNotes);
        }

        constexpr size_t maxVoices = 40;

        for (const auto& event : pending)
        {
            if (voices.size() >= maxVoices)
                voices.erase (voices.begin());

            voices.push_back ({ event.frequency, 0.0, 0.0, event.level, event.brightness, event.pan });
        }

        if (voices.empty())
            return;

        auto* left = bufferToFill.buffer->getWritePointer (0, bufferToFill.startSample);
        auto* right = bufferToFill.buffer->getNumChannels() > 1
                        ? bufferToFill.buffer->getWritePointer (1, bufferToFill.startSample)
                        : nullptr;

        const auto twoPi = juce::MathConstants<double>::twoPi;

        for (int sample = 0; sample < bufferToFill.numSamples; ++sample)
        {
            double leftValue = 0.0;
            double rightValue = 0.0;

            for (auto& voice : voices)
            {
                const auto attack = 1.0 - std::exp (-voice.age * 90.0);
                const auto decay = std::exp (-voice.age * 5.2);
                const auto envelope = attack * decay;
                auto value = std::sin (voice.phase) * envelope * voice.level;
                value += std::sin (voice.phase * 2.01) * envelope * voice.level * voice.brightness;

                const auto pan = juce::jlimit (-1.0, 1.0, voice.pan);
                leftValue += value * (0.72 - pan * 0.28);
                rightValue += value * (0.72 + pan * 0.28);

                voice.phase += twoPi * voice.frequency / currentSampleRate;
                if (voice.phase > twoPi)
                    voice.phase -= twoPi;

                voice.age += 1.0 / currentSampleRate;
            }

            left[sample] += (float) std::tanh (leftValue * 0.72);
            if (right != nullptr)
                right[sample] += (float) std::tanh (rightValue * 0.72);
        }

        voices.erase (std::remove_if (voices.begin(), voices.end(),
                                      [] (const Voice& voice) { return voice.age > 1.6; }),
                      voices.end());
    }

    void paint (juce::Graphics& g) override
    {
        const auto layout = getLayout();

        g.fillAll (PipeLookAndFeel::bg);
        drawTopBar (g, layout);
        drawToolRail (g, layout);
        drawEditor (g, layout);
        drawPreview (g, layout);
        drawFooter (g, layout);
    }

    void resized() override
    {
        const auto layout = getLayout();

        auto top = layout.top.reduced (14, 10);
        top.removeFromLeft (154);
        tempoSlider.setBounds (top.removeFromLeft (168));
        top.removeFromLeft (12);
        keyBox.setBounds (top.removeFromLeft (62));
        top.removeFromLeft (6);
        scaleBox.setBounds (top.removeFromLeft (116));
        top.removeFromLeft (12);
        playButton.setBounds (top.removeFromLeft (72));
        top.removeFromLeft (6);
        stopButton.setBounds (top.removeFromLeft (72));
        top.removeFromLeft (6);
        demoButton.setBounds (top.removeFromLeft (72));
        top.removeFromLeft (6);
        clearButton.setBounds (top.removeFromLeft (78));

        auto tools = layout.tools.reduced (10, 16);
        selectButton.setBounds (tools.removeFromTop (48));
        tools.removeFromTop (8);
        pipeButton.setBounds (tools.removeFromTop (48));
        tools.removeFromTop (8);
        tapButton.setBounds (tools.removeFromTop (48));
        tools.removeFromTop (8);
        valveButton.setBounds (tools.removeFromTop (48));
        tools.removeFromTop (8);
        drainButton.setBounds (tools.removeFromTop (48));
        tools.removeFromTop (8);
        eraseButton.setBounds (tools.removeFromTop (48));

        auto nav = layout.preview.reduced (18, 20).toFloat();
        nav.removeFromTop (28.0f);
        nav.removeFromTop (18.0f);
        const auto reservedForControls = large3DView ? 210.0f : 220.0f;
        const auto cardSide = juce::jmin (nav.getWidth(),
                                          juce::jmax (220.0f, nav.getHeight() - reservedForControls),
                                          large3DView ? 560.0f : 360.0f);
        const auto card = juce::Rectangle<float> (cardSide, cardSide)
                            .withCentre ({ nav.getCentreX(), nav.getY() + cardSide * 0.5f });

        auto faceArea = juce::Rectangle<int> ((int) card.getX(),
                                              (int) card.getBottom() + 28,
                                              (int) card.getWidth(),
                                              82);
        const auto gap = 7;
        const auto faceW = (faceArea.getWidth() - gap * 2) / 3;
        const auto faceH = (faceArea.getHeight() - gap) / 2;

        for (int i = 0; i < (int) faceButtons.size(); ++i)
        {
            const auto col = i % 3;
            const auto row = i / 3;
            faceButtons[(size_t) i].setBounds (faceArea.getX() + col * (faceW + gap),
                                                faceArea.getY() + row * (faceH + gap),
                                                faceW,
                                                faceH);
        }

        auto layerArea = faceArea.withY (faceArea.getBottom() + 28).withHeight (42);
        layerSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        layerSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 42, 24);
        layerSlider.setBounds (layerArea);

        auto noteArea = layerArea.withY (layerArea.getBottom() + 60).withHeight (34);
        const auto noteGap = 8;
        const auto noteW = (noteArea.getWidth() - noteGap) / 2;
        noteDownButton.setBounds (noteArea.removeFromLeft (noteW));
        noteArea.removeFromLeft (noteGap);
        noteUpButton.setBounds (noteArea);

        auto pitchArea = noteArea.withX ((int) card.getX()).withY (noteUpButton.getBottom() + 8).withWidth ((int) card.getWidth()).withHeight (42);
        noteSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        noteSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 58, 24);
        noteSlider.setBounds (pitchArea);

        auto scArea = pitchArea.withY (pitchArea.getBottom() + 18).withHeight (large3DView ? 130 : 92);
        compileScButton.setBounds (scArea.removeFromTop (34));
        scArea.removeFromTop (8);
        scProgramEditor.setBounds (scArea);
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        grabKeyboardFocus();

        const auto cell = cellFromPosition (event.position);
        if (! cell.has_value())
            return;

        hoverCell = *cell;
        selectedCell = *cell;
        updateInspectorControls();

        if (playing)
        {
            const auto voxel = cellToVoxel (selectedFace, selectedDepth, *cell);

            if (network.hasTap (voxel))
            {
                const auto enabled = network.toggleTapEnabled (voxel);
                setStatus (enabled ? "Tap on" : "Tap off");
                repaint();
                return;
            }

            if (network.hasDrain (voxel))
            {
                const auto open = network.toggleDrainOpen (voxel);
                setStatus (open ? "Drain open" : "Drain closed");
                repaint();
                return;
            }
        }

        if (selectedTool == Tool::select)
        {
            setStatus ("Cell selected");
            repaint();
            return;
        }

        if (selectedTool == Tool::pipe)
        {
            drawingPipe = true;
            pipeStartCell = *cell;
            lastPipeCell = *cell;
            previewPipeCells = { *cell };
            drewPipeSegment = false;
            setStatus ("Preview pipe");
            repaint();
            return;
        }

        applyToolToCell (*cell);
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        const auto cell = cellFromPosition (event.position);
        if (! cell.has_value())
            return;

        hoverCell = *cell;

        if (drawingPipe && pipeStartCell.has_value())
        {
            previewPipeCells = pipePath (*pipeStartCell, *cell);
            lastPipeCell = *cell;
        }

        repaint();
    }

    void mouseMove (const juce::MouseEvent& event) override
    {
        hoverCell = cellFromPosition (event.position);
        repaint();
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hoverCell.reset();
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (drawingPipe)
        {
            if (previewPipeCells.size() > 1)
            {
                beginChange();
                for (size_t i = 1; i < previewPipeCells.size(); ++i)
                    connectCells (previewPipeCells[i - 1], previewPipeCells[i]);
            }

            setStatus (drewPipeSegment ? "Pipe changed" : "Drag across cells to create a pipe");
        }

        drawingPipe = false;
        pipeStartCell.reset();
        lastPipeCell.reset();
        previewPipeCells.clear();
        repaint();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        const auto text = key.getTextCharacter();

        if (text == '1' || text == 's' || text == 'S') { setTool (Tool::select); return true; }
        if (text == '2' || text == 'p' || text == 'P') { setTool (Tool::pipe); return true; }
        if (text == '3' || text == 't' || text == 'T') { setTool (Tool::tap); return true; }
        if (text == '4' || text == 'v' || text == 'V') { setTool (Tool::valve); return true; }
        if (text == '5' || text == 'd' || text == 'D') { setTool (Tool::drain); return true; }
        if (text == 'e' || text == 'E') { setTool (Tool::erase); return true; }

        if (key == juce::KeyPress::spaceKey)
        {
            setPlaying (! playing);
            return true;
        }

        if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0))
            return undo();

        if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)
            || key == juce::KeyPress ('y', juce::ModifierKeys::commandModifier, 0))
            return redo();

        if (key == juce::KeyPress::backspaceKey || key == juce::KeyPress::deleteKey)
            return deleteSelectedCell();

        if (text == '[')
        {
            selectedDepth = juce::jmax (0, selectedDepth - 1);
            layerSlider.setValue (selectedDepth + 1, juce::dontSendNotification);
            updateInspectorControls();
            repaint();
            return true;
        }

        if (text == ']')
        {
            selectedDepth = juce::jmin (gridSize - 1, selectedDepth + 1);
            layerSlider.setValue (selectedDepth + 1, juce::dontSendNotification);
            updateInspectorControls();
            repaint();
            return true;
        }

        return false;
    }

private:
    struct Layout
    {
        juce::Rectangle<int> top;
        juce::Rectangle<int> content;
        juce::Rectangle<int> tools;
        juce::Rectangle<int> editor;
        juce::Rectangle<int> grid;
        juce::Rectangle<int> preview;
        juce::Rectangle<int> footer;
    };

    PipeLookAndFeel lookAndFeel;
    PipeNetwork network;

    juce::ComboBox faceBox;
    juce::ComboBox keyBox;
    juce::ComboBox scaleBox;
    std::array<juce::TextButton, 6> faceButtons;
    juce::Slider layerSlider;
    juce::Slider tempoSlider;
    juce::Slider noteSlider;
    juce::TextButton selectButton;
    juce::TextButton pipeButton;
    juce::TextButton tapButton;
    juce::TextButton valveButton;
    juce::TextButton drainButton;
    juce::TextButton eraseButton;
    juce::TextButton playButton;
    juce::TextButton stopButton;
    juce::TextButton demoButton;
    juce::TextButton clearButton;
    juce::TextButton noteDownButton;
    juce::TextButton noteUpButton;
    juce::TextButton compileScButton;
    juce::TextEditor scProgramEditor;

    Tool selectedTool = Tool::select;
    int selectedFace = 0;
    int selectedDepth = 0;
    int rootNote = 0;
    int scaleIndex = 2;
    SoundMode soundMode = SoundMode::internal;
    bool drawingPipe = false;
    bool drewPipeSegment = false;
    std::optional<Cell> pipeStartCell;
    std::optional<Cell> lastPipeCell;
    std::optional<Cell> hoverCell;
    std::optional<Cell> selectedCell;
    std::vector<Cell> previewPipeCells;

    bool playing = false;
    double bpm = 120.0;
    double emitAccumulator = 0.0;
    double lastTimerMs = 0.0;
    double animationPhase = 0.0;
    IVec3 gravityVector { 0, -1, 0 };
    std::vector<Flow> flows;

    double currentSampleRate = 44100.0;
    juce::CriticalSection noteLock;
    std::vector<NoteEvent> pendingNotes;
    std::vector<Voice> voices;
    gridcollider::EmbeddedScAudioEngine scEngine;
    juce::AudioBuffer<float> scScratch;
    juce::String scSynthName = "pipe_sc";
    bool scProgramLoaded = false;

    juce::String status = "Ready";
    bool large3DView = false;
    std::unique_ptr<juce::FileChooser> fileChooser;
    std::vector<PipeNetwork> undoStack;
    std::vector<PipeNetwork> redoStack;
    bool updatingNoteSlider = false;

    static constexpr int savePatchMenuId = 9001;
    static constexpr int loadPatchMenuId = 9002;
    static constexpr int internalSoundModeMenuId = 9501;
    static constexpr int superColliderSoundModeMenuId = 9502;
    static constexpr int mainViewMenuId = 1001;
    static constexpr int large3DViewMenuId = 1002;

    static juce::var pointToVar (IVec3 point)
    {
        auto* object = new juce::DynamicObject();
        object->setProperty ("x", point.x);
        object->setProperty ("y", point.y);
        object->setProperty ("z", point.z);
        return object;
    }

    static std::optional<IVec3> pointFromVar (const juce::var& value)
    {
        if (! value.isObject())
            return std::nullopt;

        const IVec3 point {
            (int) value.getProperty ("x", -1),
            (int) value.getProperty ("y", -1),
            (int) value.getProperty ("z", -1)
        };

        return isInside (point) ? std::optional<IVec3> (point) : std::nullopt;
    }

    juce::var makePointArray (const std::vector<IVec3>& points) const
    {
        juce::Array<juce::var> array;
        for (const auto point : points)
            array.add (pointToVar (point));
        return array;
    }

    std::vector<IVec3> loadPointArray (const juce::var& value) const
    {
        std::vector<IVec3> points;

        if (const auto* array = value.getArray())
            for (const auto& item : *array)
                if (const auto point = pointFromVar (item))
                    points.push_back (*point);

        return points;
    }

    juce::var createPatch() const
    {
        auto* root = new juce::DynamicObject();
        root->setProperty ("version", 1);
        root->setProperty ("size", gridSize);

        juce::Array<juce::var> pipeArray;
        for (const auto pipe : network.pipes)
            pipeArray.add ((int) pipe);
        root->setProperty ("pipes", pipeArray);

        root->setProperty ("taps", makePointArray (network.taps));
        root->setProperty ("disabledTaps", makePointArray (network.disabledTaps));
        root->setProperty ("drains", makePointArray (network.drains));
        root->setProperty ("closedDrains", makePointArray (network.closedDrains));

        juce::Array<juce::var> valveArray;
        for (const auto& valve : network.valves)
        {
            auto* object = new juce::DynamicObject();
            object->setProperty ("x", valve.position.x);
            object->setProperty ("y", valve.position.y);
            object->setProperty ("z", valve.position.z);
            object->setProperty ("note", valve.midiNote);
            valveArray.add (object);
        }
        root->setProperty ("valves", valveArray);
        root->setProperty ("face", selectedFace);
        root->setProperty ("layer", selectedDepth);
        root->setProperty ("tempo", bpm);
        root->setProperty ("rootNote", rootNote);
        root->setProperty ("scale", scaleIndex);
        root->setProperty ("soundMode", soundMode == SoundMode::superCollider ? "sc" : "internal");
        root->setProperty ("scProgram", scProgramEditor.getText());

        return root;
    }

    bool loadPatchFromVar (const juce::var& patch)
    {
        if (! patch.isObject() || (int) patch.getProperty ("size", 0) != gridSize)
            return false;

        PipeNetwork loaded;

        if (const auto* pipeArray = patch.getProperty ("pipes", {}).getArray())
        {
            for (int i = 0; i < juce::jmin ((int) pipeArray->size(), voxelCount); ++i)
                loaded.pipes[(size_t) i] = (uint8_t) juce::jlimit (0, 63, (int) (*pipeArray)[i]);
        }

        loaded.taps = loadPointArray (patch.getProperty ("taps", {}));
        loaded.disabledTaps = loadPointArray (patch.getProperty ("disabledTaps", {}));
        loaded.drains = loadPointArray (patch.getProperty ("drains", {}));
        loaded.closedDrains = loadPointArray (patch.getProperty ("closedDrains", {}));

        if (const auto* valves = patch.getProperty ("valves", {}).getArray())
        {
            for (const auto& item : *valves)
            {
                if (const auto point = pointFromVar (item))
                    loaded.valves.push_back ({ *point, juce::jlimit (24, 96, (int) item.getProperty ("note", defaultValveNoteFor (*point))) });
            }
        }

        beginChange();
        network = std::move (loaded);
        selectedDepth = juce::jlimit (0, gridSize - 1, (int) patch.getProperty ("layer", selectedDepth));
        selectedCell.reset();
        bpm = juce::jlimit (40.0, 220.0, (double) patch.getProperty ("tempo", bpm));
        rootNote = juce::jlimit (0, 11, (int) patch.getProperty ("rootNote", rootNote));
        scaleIndex = juce::jlimit (0, 4, (int) patch.getProperty ("scale", scaleIndex));
        soundMode = patch.getProperty ("soundMode", "internal").toString() == "sc" ? SoundMode::superCollider
                                                                                   : SoundMode::internal;
        scProgramEditor.setText (patch.getProperty ("scProgram", defaultScProgram()).toString(), juce::dontSendNotification);
        tempoSlider.setValue (bpm, juce::dontSendNotification);
        keyBox.setSelectedItemIndex (rootNote, juce::dontSendNotification);
        scaleBox.setSelectedItemIndex (scaleIndex, juce::dontSendNotification);
        layerSlider.setValue (selectedDepth + 1, juce::dontSendNotification);
        setSelectedFace ((int) patch.getProperty ("face", selectedFace));
        setPlaying (false);
        updateSoundModeControls();
        if (soundMode == SoundMode::superCollider)
            compileScProgram();
        updateInspectorControls();
        setStatus ("Patch loaded");
        return true;
    }

    void savePatchAs()
    {
        fileChooser = std::make_unique<juce::FileChooser> ("Save patch",
                                                           juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("pipe-patch.json"),
                                                           "*.json");
        fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& chooser)
                                  {
                                      const auto file = chooser.getResult();
                                      if (file == juce::File())
                                          return;

                                      if (file.replaceWithText (juce::JSON::toString (createPatch(), true)))
                                          setStatus ("Patch saved");
                                      else
                                          setStatus ("Could not save patch");
                                  });
    }

    void loadPatch()
    {
        fileChooser = std::make_unique<juce::FileChooser> ("Load patch",
                                                           juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                           "*.json");
        fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& chooser)
                                  {
                                      const auto file = chooser.getResult();
                                      if (file == juce::File())
                                          return;

                                      const auto parsed = juce::JSON::parse (file);
                                      if (! loadPatchFromVar (parsed))
                                          setStatus ("Could not load patch");
                                  });
    }

    void setupButton (juce::TextButton& button, const juce::String& text, const juce::String& tooltip)
    {
        button.setButtonText (text);
        button.setTooltip (tooltip);
        button.addListener (this);
        addAndMakeVisible (button);
    }

    void setupSlider (juce::Slider& slider, double min, double max, double step, double value)
    {
        slider.setRange (min, max, step);
        slider.setValue (value, juce::dontSendNotification);
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 46, 24);
        slider.addListener (this);
        addAndMakeVisible (slider);
    }

    void beginChange()
    {
        undoStack.push_back (network);
        if (undoStack.size() > 80)
            undoStack.erase (undoStack.begin());

        redoStack.clear();
    }

    bool undo()
    {
        if (undoStack.empty())
            return false;

        redoStack.push_back (network);
        network = undoStack.back();
        undoStack.pop_back();
        flows.clear();
        updateInspectorControls();
        setStatus ("Undo");
        repaint();
        return true;
    }

    bool redo()
    {
        if (redoStack.empty())
            return false;

        undoStack.push_back (network);
        network = redoStack.back();
        redoStack.pop_back();
        flows.clear();
        updateInspectorControls();
        setStatus ("Redo");
        repaint();
        return true;
    }

    Layout getLayout() const
    {
        Layout layout;
        auto bounds = getLocalBounds();
        layout.top = bounds.removeFromTop (64);
        layout.footer = bounds.removeFromBottom (30);
        layout.content = bounds;

        layout.tools = bounds.removeFromLeft (88);

        const auto previewWidth = large3DView
                                    ? juce::jlimit (520, 780, (int) std::round ((double) bounds.getWidth() * 0.56))
                                    : juce::jlimit (300, 420, bounds.getWidth() / 4);
        layout.preview = bounds.removeFromRight (previewWidth);
        layout.editor = bounds;

        auto gridArea = layout.editor.reduced (38, 40);
        gridArea.removeFromTop (8);
        const auto side = juce::jmax (160, juce::jmin (gridArea.getWidth(), gridArea.getHeight()));
        layout.grid = juce::Rectangle<int> (side, side).withCentre (gridArea.getCentre());

        return layout;
    }

    void setLarge3DView (bool shouldUseLarge3DView)
    {
        if (large3DView == shouldUseLarge3DView)
            return;

        large3DView = shouldUseLarge3DView;
        setStatus (large3DView ? "Large 3D view" : "Main view");
        resized();
        repaint();
    }

    void setTool (Tool tool)
    {
        selectedTool = tool;
        updateButtonStates();
        setStatus (tool == Tool::select ? "Select tool" :
                   tool == Tool::pipe ? "Pipe tool" :
                   tool == Tool::tap ? "Tap tool" :
                   tool == Tool::valve ? "Valve tool" :
                   tool == Tool::drain ? "Drain tool" : "Erase tool");
        repaint();
    }

    IVec3 gravityForFace (int faceIndex) const noexcept
    {
        return -faces[(size_t) faceIndex].v;
    }

    void updateGravityForSelectedFace()
    {
        gravityVector = gravityForFace (selectedFace);
        if (playing)
            setStatus ("Gravity follows " + juce::String (faces[(size_t) selectedFace].name));
    }

    void setSelectedFace (int faceIndex)
    {
        selectedFace = juce::jlimit (0, 5, faceIndex);
        faceBox.setSelectedItemIndex (selectedFace, juce::dontSendNotification);

        if (playing)
            updateGravityForSelectedFace();

        updateButtonStates();
        updateInspectorControls();
        repaint();
    }

    void updateButtonStates()
    {
        selectButton.setToggleState (selectedTool == Tool::select, juce::dontSendNotification);
        pipeButton.setToggleState (selectedTool == Tool::pipe, juce::dontSendNotification);
        tapButton.setToggleState (selectedTool == Tool::tap, juce::dontSendNotification);
        valveButton.setToggleState (selectedTool == Tool::valve, juce::dontSendNotification);
        drainButton.setToggleState (selectedTool == Tool::drain, juce::dontSendNotification);
        eraseButton.setToggleState (selectedTool == Tool::erase, juce::dontSendNotification);
        playButton.setToggleState (playing, juce::dontSendNotification);
        stopButton.setToggleState (! playing && flows.empty(), juce::dontSendNotification);

        for (int i = 0; i < (int) faceButtons.size(); ++i)
            faceButtons[(size_t) i].setToggleState (selectedFace == i, juce::dontSendNotification);
    }

    void setStatus (const juce::String& newStatus)
    {
        status = newStatus;
    }

    std::optional<IVec3> selectedVoxel() const
    {
        if (! selectedCell.has_value())
            return std::nullopt;

        return cellToVoxel (selectedFace, selectedDepth, *selectedCell);
    }

    Valve* selectedValve()
    {
        const auto voxel = selectedVoxel();
        return voxel.has_value() ? network.valveAt (*voxel) : nullptr;
    }

    const Valve* selectedValve() const
    {
        const auto voxel = selectedVoxel();
        return voxel.has_value() ? network.valveAt (*voxel) : nullptr;
    }

    static juce::String noteName (int midiNote)
    {
        return juce::MidiMessage::getMidiNoteName (midiNote, true, true, 3);
    }

    static juce::String scaleNameForIndex (int index)
    {
        static constexpr std::array<const char*, 5> names {{
            "Major", "Minor", "Pentatonic", "Dorian", "Whole"
        }};
        return names[(size_t) juce::jlimit (0, (int) names.size() - 1, index)];
    }

    static juce::String defaultScProgram()
    {
        return R"SC(|out = 0, pitch = 60, amp = 0.18, sustain = 0.35, pan = 0, brightness = 0.2, water = 0.5, height = 0.5, falling = 0|
    var freq = pitch.midicps;
    var env = EnvGen.kr(Env.perc(0.004, sustain.max(0.04), curve: -5), doneAction: 2);
    var source = SinOsc.ar(freq) + (Pulse.ar(freq * 0.5, 0.42) * 0.22);
    var shimmer = BPF.ar(WhiteNoise.ar, (freq * (5 + brightness * 20)).clip(900, 12000), 0.18) * water;
    var sig = RLPF.ar(source, (freq * (2.5 + brightness * 12)).clip(180, 9000), 0.24);
    sig = (sig + shimmer + (SinOsc.ar(freq * 2.01) * falling * 0.18)) * env * amp * (0.75 + height * 0.5);
    Out.ar(out, Pan2.ar(sig.tanh, pan));
)SC";
    }

    static juce::String wrappedScProgram (const juce::String& synthName, juce::String program)
    {
        const auto trimmed = program.trim();

        if (trimmed.containsIgnoreCase ("SynthDef("))
            return trimmed.replace ("__name__", synthName, true);

        return "SynthDef(\\"
            + synthName
            + ", {\n"
            + trimmed
            + "\n})";
    }

    bool compileScProgram()
    {
        scProgramLoaded = false;

        if (! scEngine.isReady())
        {
            setStatus ("SC unavailable");
            return false;
        }

        scSynthName = "pipe_sc_" + juce::String (juce::Time::getMillisecondCounter());
        const auto source = wrappedScProgram (scSynthName, scProgramEditor.getText());

        if (! scEngine.loadSynthDef (scSynthName, source))
        {
            setStatus ("SC compile failed");
            return false;
        }

        scProgramLoaded = true;
        setStatus ("SC program ready");
        return true;
    }

    void updateSoundModeControls()
    {
        const auto isSc = soundMode == SoundMode::superCollider;
        scProgramEditor.setVisible (isSc);
        compileScButton.setVisible (isSc);

        if (isSc && ! scProgramLoaded && scEngine.isReady())
            compileScProgram();

        repaint();
    }

    void setSoundMode (SoundMode mode)
    {
        soundMode = mode;

        if (soundMode == SoundMode::superCollider)
        {
            if (scEngine.isReady())
                compileScProgram();
            else
                setStatus ("SC unavailable");
        }
        else
        {
            setStatus ("JUCE sound mode");
        }

        updateSoundModeControls();
    }

    const std::vector<int>& currentScale() const
    {
        static const std::vector<int> major { 0, 2, 4, 5, 7, 9, 11 };
        static const std::vector<int> minor { 0, 2, 3, 5, 7, 8, 10 };
        static const std::vector<int> pentatonic { 0, 2, 4, 7, 9 };
        static const std::vector<int> dorian { 0, 2, 3, 5, 7, 9, 10 };
        static const std::vector<int> whole { 0, 2, 4, 6, 8, 10 };

        switch (scaleIndex)
        {
            case 0: return major;
            case 1: return minor;
            case 2: return pentatonic;
            case 3: return dorian;
            default: return whole;
        }
    }

    int noteInCurrentScale (int midiNote) const
    {
        const auto& scale = currentScale();
        const auto octave = juce::jlimit (1, 8, midiNote / 12);
        const auto noteClass = (midiNote % 12 + 12) % 12;

        auto bestNote = rootNote + 12 * octave;
        auto bestDistance = 128;

        for (int octaveOffset = -1; octaveOffset <= 1; ++octaveOffset)
        {
            for (const auto degree : scale)
            {
                const auto candidate = rootNote + degree + 12 * (octave + octaveOffset);
                const auto distance = std::abs (candidate - midiNote);
                if (distance < bestDistance || (distance == bestDistance && ((candidate % 12 + 12) % 12) == noteClass))
                {
                    bestDistance = distance;
                    bestNote = candidate;
                }
            }
        }

        return juce::jlimit (24, 96, bestNote);
    }

    int defaultMusicalNoteFor (IVec3 voxel) const
    {
        return noteInCurrentScale (defaultValveNoteFor (voxel));
    }

    void updateInspectorControls()
    {
        const auto hasValve = selectedValve() != nullptr;
        noteDownButton.setVisible (hasValve);
        noteUpButton.setVisible (hasValve);
        noteSlider.setVisible (hasValve);

        updatingNoteSlider = true;
        if (const auto* valve = selectedValve())
            noteSlider.setValue (valve->midiNote, juce::dontSendNotification);
        updatingNoteSlider = false;
    }

    void adjustSelectedValveNote (int semitones)
    {
        if (auto* valve = selectedValve())
        {
            beginChange();
            valve->midiNote = juce::jlimit (24, 96, valve->midiNote + semitones);
            updateInspectorControls();
            setStatus ("Valve note " + noteName (valve->midiNote));
            repaint();
        }
    }

    void setSelectedValveNote (int midiNote)
    {
        if (updatingNoteSlider)
            return;

        if (auto* valve = selectedValve())
        {
            beginChange();
            valve->midiNote = juce::jlimit (24, 96, midiNote);
            setStatus ("Valve note " + noteName (valve->midiNote));
            repaint();
        }
    }

    bool deleteSelectedCell()
    {
        if (selectedTool != Tool::select || ! selectedCell.has_value())
            return false;

        beginChange();
        network.eraseVoxel (cellToVoxel (selectedFace, selectedDepth, *selectedCell));
        updateInspectorControls();
        setStatus ("Selection deleted");
        repaint();
        return true;
    }

    std::optional<Cell> cellFromPosition (juce::Point<float> position) const
    {
        const auto grid = getLayout().grid.toFloat();
        if (! grid.contains (position))
            return std::nullopt;

        const auto cellSize = grid.getWidth() / (float) gridSize;
        const auto col = juce::jlimit (0, gridSize - 1, (int) ((position.x - grid.getX()) / cellSize));
        const auto row = juce::jlimit (0, gridSize - 1, (int) ((position.y - grid.getY()) / cellSize));
        return Cell { col, row };
    }

    void applyToolToCell (Cell cell)
    {
        const auto voxel = cellToVoxel (selectedFace, selectedDepth, cell);

        if (selectedTool == Tool::tap)
        {
            beginChange();
            network.toggleTap (voxel);
            setStatus (network.hasTap (voxel) ? "Tap added" : "Tap removed");
        }
        else if (selectedTool == Tool::valve)
        {
            beginChange();
            const auto hadValve = network.hasValve (voxel);
            network.toggleValve (voxel);
            if (! hadValve)
                if (auto* valve = network.valveAt (voxel))
                    valve->midiNote = defaultMusicalNoteFor (voxel);
            setStatus (network.hasValve (voxel) ? "Valve added" : "Valve removed");
        }
        else if (selectedTool == Tool::drain)
        {
            beginChange();
            network.toggleDrain (voxel);
            setStatus (network.hasDrain (voxel) ? "Drain added" : "Drain removed");
        }
        else if (selectedTool == Tool::erase)
        {
            beginChange();
            network.eraseVoxel (voxel);
            setStatus ("Cell erased");
        }

        updateInspectorControls();
        repaint();
    }

    std::vector<Cell> pipePath (Cell from, Cell to) const
    {
        std::vector<Cell> path { from };

        if (from == to)
            return path;

        auto cursor = from;
        const auto horizontalFirst = std::abs (to.col - from.col) >= std::abs (to.row - from.row);

        auto stepHorizontal = [&]
        {
            while (cursor.col != to.col)
            {
                auto next = cursor;
                next.col += signOf (to.col - cursor.col);
                cursor = next;
                path.push_back (cursor);
            }
        };

        auto stepVertical = [&]
        {
            while (cursor.row != to.row)
            {
                auto next = cursor;
                next.row += signOf (to.row - cursor.row);
                cursor = next;
                path.push_back (cursor);
            }
        };

        if (horizontalFirst)
        {
            stepHorizontal();
            stepVertical();
        }
        else
        {
            stepVertical();
            stepHorizontal();
        }

        return path;
    }

    void connectCells (Cell a, Cell b)
    {
        const auto va = cellToVoxel (selectedFace, selectedDepth, a);
        const auto vb = cellToVoxel (selectedFace, selectedDepth, b);

        if (network.connectAdjacent (va, vb))
        {
            drewPipeSegment = true;
            setStatus ("Pipe changed");
        }
    }

    void setPlaying (bool shouldPlay)
    {
        playing = shouldPlay;
        emitAccumulator = shouldPlay ? secondsPerBeat() : 0.0;
        lastTimerMs = juce::Time::getMillisecondCounterHiRes();

        if (shouldPlay)
            updateGravityForSelectedFace();

        if (! shouldPlay)
            flows.clear();

        setStatus (playing ? "Water running" : "Stopped");
        updateButtonStates();
        repaint();
    }

    double secondsPerBeat() const
    {
        return 60.0 / juce::jlimit (30.0, 240.0, bpm);
    }

    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto dt = lastTimerMs > 0.0 ? juce::jlimit (0.0, 0.1, (now - lastTimerMs) / 1000.0) : 0.0;
        lastTimerMs = now;

        if (playing)
        {
            animationPhase += dt;
            updateFlow (dt);
        }

        updateButtonStates();
        repaint();
    }

    void updateFlow (double dt)
    {
        const auto beat = secondsPerBeat();
        emitAccumulator += dt;

        while (emitAccumulator >= beat)
        {
            emitAccumulator -= beat;
            emitFromTaps();
        }

        for (auto& flow : flows)
        {
            if (flow.falling)
            {
                advanceFallingFlow (flow, dt / beat);
                continue;
            }

            flow.progress += dt / beat;

            while (flow.progress >= 1.0 && ! flow.dead && ! flow.falling)
            {
                flow.progress -= 1.0;
                advanceFlow (flow);
            }
        }

        flows.erase (std::remove_if (flows.begin(), flows.end(), [] (const Flow& flow) { return flow.dead; }),
                     flows.end());
    }

    void emitFromTaps()
    {
        constexpr size_t maxFlows = 128;

        for (const auto tap : network.taps)
        {
            if (flows.size() >= maxFlows)
                break;

            if (! network.isTapEnabled (tap))
                continue;

            const auto exits = network.validConnectedBits (tap);
            if (exits.empty())
                continue;

            const auto choice = exits[(size_t) juce::Random::getSystemRandom().nextInt ((int) exits.size())];
            flows.push_back ({ tap, { (float) tap.x, (float) tap.y, (float) tap.z }, choice, 0.0, false, false });
        }
    }

    void advanceFlow (Flow& flow)
    {
        if (! isInside (flow.position) || ! network.hasPipe (flow.position))
        {
            flow.dead = true;
            return;
        }

        const auto next = flow.position + vectorForBit (flow.direction);

        if (! isInside (next)
            || (network.pipes[(size_t) indexOf (flow.position)] & flow.direction) == 0
            || (network.pipes[(size_t) indexOf (next)] & oppositeBit (flow.direction)) == 0)
        {
            flow.dead = true;
            return;
        }

        flow.position = next;

        if (network.hasValve (flow.position))
            triggerValve (flow.position, flow);

        if (network.isDrainOpen (flow.position))
        {
            flow.falling = true;
            flow.fallPosition = { (float) flow.position.x, (float) flow.position.y, (float) flow.position.z };
            flow.progress = 0.0;
            setStatus ("Drain released water");
            return;
        }

        const auto arrivedFrom = oppositeBit (flow.direction);
        auto exits = network.validConnectedBits (flow.position, arrivedFrom);

        if (! exits.empty())
        {
            flow.direction = exits[(size_t) juce::Random::getSystemRandom().nextInt ((int) exits.size())];
            return;
        }

        const auto allExits = network.validConnectedBits (flow.position);
        if (std::find (allExits.begin(), allExits.end(), arrivedFrom) != allExits.end())
            flow.direction = arrivedFrom;
        else
            flow.dead = true;
    }

    void advanceFallingFlow (Flow& flow, double beatFraction)
    {
        const auto previous = flow.fallPosition;
        const auto gravity = toFloatVector (gravityVector);
        const auto distance = (float) (beatFraction * 2.0);
        const FVec3 next {
            previous.x + gravity.x * distance,
            previous.y + gravity.y * distance,
            previous.z + gravity.z * distance
        };

        const auto axisValue = [] (FVec3 value, IVec3 axis)
        {
            if (axis.x != 0) return value.x;
            if (axis.y != 0) return value.y;
            return value.z;
        };

        const auto setAxisValue = [] (IVec3& value, IVec3 axis, int coordinate)
        {
            if (axis.x != 0) value.x = coordinate;
            else if (axis.y != 0) value.y = coordinate;
            else value.z = coordinate;
        };

        const auto startAxis = axisValue (previous, gravityVector);
        const auto endAxis = axisValue (next, gravityVector);
        const auto gravitySign = coordinateOnAxis (gravityVector, gravityVector);
        const auto startStep = gravitySign > 0
                                ? (int) std::ceil (startAxis + 0.001f)
                                : (int) std::floor (startAxis - 0.001f);
        const auto endStep = gravitySign > 0
                                ? (int) std::ceil (endAxis)
                                : (int) std::floor (endAxis);

        for (int step = startStep;
             gravitySign > 0 ? step <= endStep : step >= endStep;
             step += gravitySign)
        {
            IVec3 candidate {
                juce::jlimit (0, gridSize - 1, (int) std::round (next.x)),
                juce::jlimit (0, gridSize - 1, (int) std::round (next.y)),
                juce::jlimit (0, gridSize - 1, (int) std::round (next.z))
            };
            setAxisValue (candidate, gravityVector, step);

            if (! isInside (candidate) || ! network.hasPipe (candidate))
                continue;

            flow.position = candidate;
            flow.fallPosition = { (float) candidate.x, (float) candidate.y, (float) candidate.z };
            flow.falling = false;
            flow.progress = 0.0;

            if (network.hasValve (candidate))
                triggerValve (candidate, flow);

            if (network.isDrainOpen (candidate))
            {
                flow.falling = true;
                return;
            }

            const auto exits = network.validConnectedBits (candidate, oppositeBit (bitForVector (gravityVector)));
            if (! exits.empty())
            {
                flow.direction = exits[(size_t) juce::Random::getSystemRandom().nextInt ((int) exits.size())];
                setStatus ("Water rejoined pipe");
            }
            else
            {
                const auto allExits = network.validConnectedBits (candidate);
                flow.direction = allExits.empty() ? 0 : allExits[(size_t) juce::Random::getSystemRandom().nextInt ((int) allExits.size())];
                flow.dead = flow.direction == 0;
            }

            return;
        }

        flow.fallPosition = next;

        const auto floorValue = gravitySign > 0 ? (float) (gridSize - 1) : 0.0f;
        const auto hitFloor = gravitySign > 0
                                ? axisValue (flow.fallPosition, gravityVector) >= floorValue
                                : axisValue (flow.fallPosition, gravityVector) <= floorValue;

        if (hitFloor)
        {
            flow.dead = true;
            setStatus ("Water hit the floor");
        }
    }

    void triggerValve (IVec3 voxel, const Flow& flow)
    {
        const auto* valve = network.valveAt (voxel);
        const auto rawMidiNote = valve != nullptr ? valve->midiNote : defaultMusicalNoteFor (voxel);
        const auto midiNote = noteInCurrentScale (rawMidiNote);
        const auto exits = (double) network.validConnectedBits (voxel).size();
        const auto height = (double) voxel.y / (double) (gridSize - 1);
        const auto travelEnergy = juce::jlimit (0.0, 1.0, flow.progress);

        NoteEvent event;
        event.frequency = juce::MidiMessage::getMidiNoteInHertz (midiNote);
        event.level = juce::jlimit (0.035, 0.12, 0.052 + exits * 0.008 + (flow.falling ? 0.026 : 0.0));
        event.brightness = juce::jlimit (0.06, 0.42, 0.10 + height * 0.16 + travelEnergy * 0.08 + (flow.falling ? 0.10 : 0.0));
        event.pan = juce::jlimit (-0.85, 0.85, ((double) voxel.x / (double) (gridSize - 1)) * 1.7 - 0.85);

        if (soundMode == SoundMode::superCollider && scEngine.isReady() && scProgramLoaded)
        {
            gridcollider::EventFields fields;
            fields.timestampSeconds = animationPhase;
            fields.tick = (std::uint64_t) juce::jmax (0.0, animationPhase * bpm / 60.0);
            fields.sourceCell = { juce::jlimit (0, 63, juce::roundToInt ((float) voxel.x / (float) (gridSize - 1) * 63.0f)),
                                  juce::jlimit (0, 63, juce::roundToInt ((float) voxel.y / (float) (gridSize - 1) * 63.0f)) };
            fields.instrumentName = scSynthName;
            fields.pitch = midiNote;
            fields.velocity = juce::jlimit (0.0f, 1.0f, (float) (event.level * 8.0));
            fields.durationTicks = flow.falling ? 2 : 1;
            fields.parameters["pan"] = juce::String ((float) event.pan, 3);
            fields.parameters["brightness"] = juce::String ((float) event.brightness, 3);
            fields.parameters["water"] = juce::String ((float) travelEnergy, 3);
            fields.parameters["height"] = juce::String ((float) height, 3);
            fields.parameters["falling"] = flow.falling ? "1" : "0";
            fields.parameters["exits"] = juce::String ((float) exits, 3);
            fields.parameters["tempo"] = juce::String ((float) bpm, 3);

            scEngine.setTransport (bpm, fields.tick, playing);
            scEngine.enqueue ({ gridcollider::InternalEvent { gridcollider::NoteEvent { fields } } });
            return;
        }

        const juce::ScopedLock lock (noteLock);
        pendingNotes.push_back (event);
    }

    FVec3 currentFlowPosition (const Flow& flow) const
    {
        const auto direction = vectorForBit (flow.direction);
        if (flow.falling)
            return flow.fallPosition;

        return {
            (float) flow.position.x + (float) direction.x * (float) flow.progress,
            (float) flow.position.y + (float) direction.y * (float) flow.progress,
            (float) flow.position.z + (float) direction.z * (float) flow.progress
        };
    }

    void drawTopBar (juce::Graphics& g, const Layout& layout)
    {
        g.setColour (PipeLookAndFeel::panel);
        g.fillRect (layout.top);

        juce::ColourGradient rainbow (PipeLookAndFeel::pink, 0.0f, 0.0f,
                                      PipeLookAndFeel::sky, (float) getWidth(), 0.0f,
                                      false);
        rainbow.addColour (0.20, PipeLookAndFeel::coral);
        rainbow.addColour (0.38, PipeLookAndFeel::lemon);
        rainbow.addColour (0.58, PipeLookAndFeel::mint);
        rainbow.addColour (0.76, PipeLookAndFeel::aqua);
        g.setGradientFill (rainbow);
        g.fillRect (layout.top.withHeight (3));
        g.setColour (PipeLookAndFeel::line.withAlpha (0.45f));
        g.drawHorizontalLine (layout.top.getBottom() - 1, 0.0f, (float) getWidth());

        auto title = layout.top.reduced (16, 10).removeFromLeft (142);
        g.setColour (PipeLookAndFeel::ink);
        g.setFont (juce::FontOptions (17.0f).withStyle ("Bold"));
        g.drawFittedText ("pipe", title.removeFromTop (24), juce::Justification::centredLeft, 1);
        g.setColour (PipeLookAndFeel::pink);
        g.setFont (juce::FontOptions (10.0f).withStyle ("Bold"));
        g.drawFittedText ("matd.space", title, juce::Justification::centredLeft, 1);

        g.setFont (juce::FontOptions (9.0f));
        g.setColour (PipeLookAndFeel::muted.withAlpha (0.80f));
        g.drawFittedText ("Tempo", tempoSlider.getBounds().translated (0, -18), juce::Justification::centredLeft, 1);
        g.drawFittedText ("Key", keyBox.getBounds().translated (0, -18), juce::Justification::centredLeft, 1);
        g.drawFittedText ("Scale", scaleBox.getBounds().translated (0, -18), juce::Justification::centredLeft, 1);
    }

    void drawToolRail (juce::Graphics& g, const Layout& layout)
    {
        g.setColour (PipeLookAndFeel::panel);
        g.fillRect (layout.tools);
        g.setColour (PipeLookAndFeel::line.withAlpha (0.42f));
        g.drawVerticalLine (layout.tools.getRight() - 1, (float) layout.tools.getY(), (float) layout.tools.getBottom());
    }

    void drawEditor (juce::Graphics& g, const Layout& layout)
    {
        g.setColour (PipeLookAndFeel::bg);
        g.fillRect (layout.editor);

        const auto grid = layout.grid.toFloat();
        const auto cell = grid.getWidth() / (float) gridSize;

        auto caption = juce::Rectangle<int> (layout.editor.getX() + 28,
                                             layout.grid.getY() - 28,
                                             juce::jmax (120, layout.grid.getX() - layout.editor.getX() - 44),
                                             20);
        g.setFont (juce::FontOptions (12.0f).withStyle ("Bold"));
        g.setColour (PipeLookAndFeel::lemon);
        g.drawFittedText (juce::String (faces[(size_t) selectedFace].name).toUpperCase()
                            + " / LAYER " + juce::String (selectedDepth + 1).paddedLeft ('0', 2),
                          caption,
                          juce::Justification::bottomLeft,
                          1);

        drawRulers (g, layout, cell);

        g.setColour (PipeLookAndFeel::panel.withAlpha (0.82f));
        g.fillRoundedRectangle (grid.expanded (10.0f), 18.0f);
        g.setColour (juce::Colour (0xff17172b));
        g.fillRoundedRectangle (grid, 12.0f);

        drawLayerGhosts (g, grid, cell);

        for (int row = 0; row < gridSize; ++row)
            for (int col = 0; col < gridSize; ++col)
                drawCellContents (g, Cell { col, row }, selectedDepth, grid, cell, 1.0f);

        drawPipePreview (g, grid, cell);
        drawMarkers2D (g, grid, cell);
        drawFlows2D (g, grid, cell);

        g.setColour (PipeLookAndFeel::line.withAlpha (0.58f));
        for (int i = 0; i <= gridSize; ++i)
        {
            const auto p = grid.getX() + (float) i * cell;
            const auto q = grid.getY() + (float) i * cell;
            g.drawLine (p, grid.getY(), p, grid.getBottom(), i == 0 || i == gridSize ? 1.2f : 0.55f);
            g.drawLine (grid.getX(), q, grid.getRight(), q, i == 0 || i == gridSize ? 1.2f : 0.55f);
        }

        if (hoverCell.has_value())
        {
            const auto hover = cellBounds (grid, cell, *hoverCell).reduced (2.0f);
            g.setColour (PipeLookAndFeel::aqua.withAlpha (0.16f));
            g.fillRoundedRectangle (hover, 8.0f);
            g.setColour (PipeLookAndFeel::aqua.withAlpha (0.85f));
            g.drawRoundedRectangle (hover, 8.0f, 1.4f);
        }

        if (selectedCell.has_value())
        {
            const auto selected = cellBounds (grid, cell, *selectedCell).reduced (3.5f);
            g.setColour (PipeLookAndFeel::pink.withAlpha (0.12f));
            g.fillRoundedRectangle (selected, 7.0f);
            g.setColour (PipeLookAndFeel::pink.withAlpha (0.90f));
            g.drawRoundedRectangle (selected, 7.0f, 1.7f);
        }
    }

    void drawRulers (juce::Graphics& g, const Layout& layout, float cell)
    {
        const auto grid = layout.grid.toFloat();
        g.setFont (juce::FontOptions (9.0f));
        g.setColour (PipeLookAndFeel::muted);

        for (int i = 0; i < gridSize; ++i)
        {
            const auto label = juce::String (i + 1).paddedLeft ('0', 2);
            g.drawFittedText (label,
                              juce::Rectangle<float> (grid.getX() + (float) i * cell, grid.getY() - 18.0f, cell, 14.0f).toNearestInt(),
                              juce::Justification::centred,
                              1);
            g.drawFittedText (label,
                              juce::Rectangle<float> (grid.getX() - 28.0f, grid.getY() + (float) i * cell, 22.0f, cell).toNearestInt(),
                              juce::Justification::centredRight,
                              1);
        }
    }

    juce::Rectangle<float> cellBounds (juce::Rectangle<float> grid, float cell, Cell displayCell) const
    {
        return { grid.getX() + (float) displayCell.col * cell,
                 grid.getY() + (float) displayCell.row * cell,
                 cell,
                 cell };
    }

    juce::Point<float> cellCentre (juce::Rectangle<float> grid, float cell, Cell displayCell) const
    {
        return { grid.getX() + ((float) displayCell.col + 0.5f) * cell,
                 grid.getY() + ((float) displayCell.row + 0.5f) * cell };
    }

    void drawLayerGhosts (juce::Graphics& g, juce::Rectangle<float> grid, float cell)
    {
        for (int distance = gridSize - 1; distance >= 1; --distance)
        {
            const auto behindDepth = selectedDepth + distance;
            const auto aheadDepth = selectedDepth - distance;

            if (behindDepth < gridSize)
                drawLayerGhost (g, behindDepth, grid, cell, distance, false);

            if (aheadDepth >= 0)
                drawLayerGhost (g, aheadDepth, grid, cell, distance, true);
        }
    }

    void drawLayerGhost (juce::Graphics& g, int depth, juce::Rectangle<float> grid, float cell, int distance, bool towardViewer)
    {
        const auto alpha = juce::jlimit (0.045f, 0.32f, 0.34f / ((float) distance + 0.45f));

        for (int row = 0; row < gridSize; ++row)
            for (int col = 0; col < gridSize; ++col)
                drawGhostCellContents (g, Cell { col, row }, depth, grid, cell, alpha, towardViewer);
    }

    void drawGhostCellContents (juce::Graphics& g,
                                Cell displayCell,
                                int depth,
                                juce::Rectangle<float> grid,
                                float cell,
                                float alpha,
                                bool towardViewer)
    {
        const auto voxel = cellToVoxel (selectedFace, depth, displayCell);
        const auto mask = network.pipes[(size_t) indexOf (voxel)];

        if (mask == 0)
            return;

        const auto offsetAmount = juce::jlimit (1.2f, 5.0f, cell * (towardViewer ? -0.055f : 0.055f));
        const auto centre = cellCentre (grid, cell, displayCell) + juce::Point<float> (offsetAmount, offsetAmount);
        const auto pipeWidth = juce::jmax (1.4f, cell * 0.055f);
        const auto ghostColour = (towardViewer ? PipeLookAndFeel::aqua : PipeLookAndFeel::lavender).withAlpha (alpha);

        g.setColour (ghostColour);

        for (const auto& direction : directions)
        {
            if ((mask & direction.bit) == 0)
                continue;

            if (auto displayDirection = displayDirectionForBit (selectedFace, direction.bit))
            {
                const auto end = centre + (*displayDirection) * (cell * 0.43f);
                g.drawDashedLine ({ centre, end }, std::array<float, 2> { 4.0f, 4.0f }.data(), 2, pipeWidth);
            }
            else
            {
                const auto r = cell * 0.105f;
                g.drawEllipse ({ centre.x - r, centre.y - r, r * 2.0f, r * 2.0f }, pipeWidth);
            }
        }

        g.fillEllipse ({ centre.x - pipeWidth * 0.7f, centre.y - pipeWidth * 0.7f, pipeWidth * 1.4f, pipeWidth * 1.4f });
    }

    void drawCellContents (juce::Graphics& g, Cell displayCell, int depth, juce::Rectangle<float> grid, float cell, float alpha)
    {
        const auto voxel = cellToVoxel (selectedFace, depth, displayCell);
        const auto mask = network.pipes[(size_t) indexOf (voxel)];

        if (mask == 0)
            return;

        const auto centre = cellCentre (grid, cell, displayCell);
        const auto pipeWidth = juce::jmax (3.0f, cell * 0.13f);
        const auto rainbow = PipeLookAndFeel::rainbowForIndex (voxel.x + voxel.y * 2 + voxel.z * 3);
        const auto pipeColour = depth == selectedDepth
                                    ? rainbow.withAlpha (0.96f * alpha)
                                    : rainbow.withAlpha (0.30f * alpha);

        g.setColour (pipeColour);

        for (const auto& direction : directions)
        {
            if ((mask & direction.bit) == 0)
                continue;

            if (auto displayDirection = displayDirectionForBit (selectedFace, direction.bit))
            {
                const auto end = centre + (*displayDirection) * (cell * 0.46f);
                g.drawLine ({ centre, end }, pipeWidth);

                if (depth == selectedDepth && ! network.isMatchedPort (voxel, direction.bit))
                {
                    g.setColour (PipeLookAndFeel::danger.withAlpha (0.88f));
                    g.fillEllipse ({ end.x - 3.0f, end.y - 3.0f, 6.0f, 6.0f });
                    g.setColour (pipeColour);
                }
            }
            else if (depth == selectedDepth && isTowardViewer (selectedFace, direction.bit))
            {
                g.fillEllipse ({ centre.x - cell * 0.13f, centre.y - cell * 0.13f, cell * 0.26f, cell * 0.26f });
            }
            else if (depth == selectedDepth && isAwayFromViewer (selectedFace, direction.bit))
            {
                juce::Path diamond;
                diamond.startNewSubPath (centre.x, centre.y - cell * 0.16f);
                diamond.lineTo (centre.x + cell * 0.16f, centre.y);
                diamond.lineTo (centre.x, centre.y + cell * 0.16f);
                diamond.lineTo (centre.x - cell * 0.16f, centre.y);
                diamond.closeSubPath();
                g.fillPath (diamond);
            }
        }

        g.setColour (pipeColour.brighter (0.18f));
        g.fillEllipse ({ centre.x - pipeWidth * 0.55f, centre.y - pipeWidth * 0.55f, pipeWidth * 1.1f, pipeWidth * 1.1f });
    }

    void drawPipePreview (juce::Graphics& g, juce::Rectangle<float> grid, float cell)
    {
        if (previewPipeCells.size() < 2)
            return;

        const auto width = juce::jmax (3.0f, cell * 0.11f);
        g.setColour (PipeLookAndFeel::lemon.withAlpha (0.34f));

        for (size_t i = 1; i < previewPipeCells.size(); ++i)
        {
            const auto a = cellCentre (grid, cell, previewPipeCells[i - 1]);
            const auto b = cellCentre (grid, cell, previewPipeCells[i]);
            g.drawDashedLine ({ a, b }, std::array<float, 2> { 7.0f, 5.0f }.data(), 2, width);
        }

        g.setColour (PipeLookAndFeel::lemon.withAlpha (0.70f));
        for (const auto previewCell : previewPipeCells)
        {
            const auto centre = cellCentre (grid, cell, previewCell);
            const auto r = cell * 0.10f;
            g.fillEllipse ({ centre.x - r, centre.y - r, r * 2.0f, r * 2.0f });
        }
    }

    void drawMarkers2D (juce::Graphics& g, juce::Rectangle<float> grid, float cell)
    {
        const auto pulse = playing ? 0.5f + 0.5f * std::sin ((float) animationPhase * 7.0f) : 0.0f;

        for (const auto tap : network.taps)
        {
            if (const auto display = voxelToCellOnFaceDepth (selectedFace, selectedDepth, tap))
            {
                const auto centre = cellCentre (grid, cell, *display);
                const auto radius = cell * 0.22f;
                const auto enabled = network.isTapEnabled (tap);
                const auto tapColour = enabled ? PipeLookAndFeel::water : PipeLookAndFeel::muted;

                g.setColour (tapColour.withAlpha (enabled ? 0.18f : 0.10f));
                g.fillEllipse ({ centre.x - radius * 1.7f, centre.y - radius * 1.7f, radius * 3.4f, radius * 3.4f });

                if (playing && enabled)
                {
                    const auto pulseRadius = radius * (1.85f + pulse * 0.95f);
                    g.setColour (tapColour.withAlpha (0.18f * (1.0f - pulse)));
                    g.drawEllipse ({ centre.x - pulseRadius, centre.y - pulseRadius, pulseRadius * 2.0f, pulseRadius * 2.0f }, 2.0f);
                }

                g.setColour (tapColour.withAlpha (enabled ? 1.0f : 0.62f));
                g.drawEllipse ({ centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f }, 2.0f);
                g.drawLine (centre.x, centre.y - radius * 1.25f, centre.x, centre.y + radius * 1.25f, 1.8f);
                g.drawLine (centre.x - radius * 1.25f, centre.y, centre.x + radius * 1.25f, centre.y, 1.8f);

                if (! enabled)
                    g.drawLine (centre.x - radius * 1.28f, centre.y + radius * 1.28f,
                                centre.x + radius * 1.28f, centre.y - radius * 1.28f, 2.0f);
            }
        }

        for (const auto valve : network.valves)
        {
            if (const auto display = voxelToCellOnFaceDepth (selectedFace, selectedDepth, valve.position))
            {
                const auto centre = cellCentre (grid, cell, *display);
                const auto radius = cell * 0.20f;
                g.setColour (PipeLookAndFeel::pink.withAlpha (0.18f));
                g.fillRoundedRectangle ({ centre.x - radius * 1.45f, centre.y - radius * 1.45f, radius * 2.9f, radius * 2.9f }, 3.0f);
                g.setColour (PipeLookAndFeel::pink);
                g.drawRoundedRectangle ({ centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f }, 3.0f, 2.0f);
                g.drawLine (centre.x - radius, centre.y - radius, centre.x + radius, centre.y + radius, 1.6f);
                g.drawLine (centre.x - radius, centre.y + radius, centre.x + radius, centre.y - radius, 1.6f);
            }
        }

        for (const auto drain : network.drains)
        {
            if (const auto display = voxelToCellOnFaceDepth (selectedFace, selectedDepth, drain))
            {
                const auto centre = cellCentre (grid, cell, *display);
                const auto radius = cell * 0.22f;
                const auto open = network.isDrainOpen (drain);
                const auto drainColour = open ? PipeLookAndFeel::coral : PipeLookAndFeel::muted;

                g.setColour (drainColour.withAlpha (open ? 0.16f : 0.10f));
                g.fillEllipse ({ centre.x - radius * 1.45f, centre.y - radius * 1.45f, radius * 2.9f, radius * 2.9f });
                if (playing && open)
                {
                    g.setColour (drainColour.withAlpha (0.14f + pulse * 0.16f));
                    g.drawLine (centre.x, centre.y + radius * 1.0f,
                                centre.x, centre.y + radius * (1.8f + pulse * 0.8f),
                                2.0f);
                }
                g.setColour (drainColour.withAlpha (open ? 1.0f : 0.62f));
                g.drawEllipse ({ centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f }, 2.0f);

                juce::Path funnel;
                funnel.startNewSubPath (centre.x - radius * 0.82f, centre.y - radius * 0.36f);
                funnel.lineTo (centre.x + radius * 0.82f, centre.y - radius * 0.36f);
                funnel.lineTo (centre.x, centre.y + radius * 0.86f);
                funnel.closeSubPath();
                g.fillPath (funnel);

                if (! open)
                    g.drawLine (centre.x - radius * 1.25f, centre.y + radius * 1.25f,
                                centre.x + radius * 1.25f, centre.y - radius * 1.25f, 2.0f);
            }
        }
    }

    void drawFlows2D (juce::Graphics& g, juce::Rectangle<float> grid, float cell)
    {
        for (const auto& flow : flows)
        {
            if (flow.falling)
            {
                const IVec3 fallingVoxel {
                    juce::jlimit (0, gridSize - 1, (int) std::round (flow.fallPosition.x)),
                    juce::jlimit (0, gridSize - 1, (int) std::round (flow.fallPosition.y)),
                    juce::jlimit (0, gridSize - 1, (int) std::round (flow.fallPosition.z))
                };

                if (voxelDepthForFace (selectedFace, fallingVoxel) != selectedDepth)
                    continue;

                const auto display = voxelToCellOnFace (selectedFace, fallingVoxel);
                const auto point = cellCentre (grid, cell, display);
                const auto radius = cell * 0.14f;

                g.setColour (PipeLookAndFeel::water.withAlpha (0.16f));
                g.fillEllipse ({ point.x - radius * 1.8f, point.y - radius * 1.8f, radius * 3.6f, radius * 3.6f });
                g.setColour (PipeLookAndFeel::water);
                g.fillEllipse ({ point.x - radius, point.y - radius, radius * 2.0f, radius * 2.0f });
                continue;
            }

            const auto next = flow.position + vectorForBit (flow.direction);
            const auto startDepth = voxelDepthForFace (selectedFace, flow.position);
            const auto endDepth = isInside (next) ? voxelDepthForFace (selectedFace, next) : startDepth;

            if (startDepth != selectedDepth && endDepth != selectedDepth)
                continue;

            const auto a = voxelToCellOnFace (selectedFace, flow.position);
            const auto b = isInside (next) ? voxelToCellOnFace (selectedFace, next) : a;
            const auto ca = cellCentre (grid, cell, a);
            const auto cb = cellCentre (grid, cell, b);
            const auto amount = startDepth == selectedDepth && endDepth == selectedDepth ? (float) flow.progress : 0.0f;
            const auto point = ca + (cb - ca) * amount;
            const auto radius = cell * 0.17f;

            g.setColour (PipeLookAndFeel::water.withAlpha (0.22f));
            g.fillEllipse ({ point.x - radius * 2.0f, point.y - radius * 2.0f, radius * 4.0f, radius * 4.0f });
            g.setColour (PipeLookAndFeel::water);
            g.fillEllipse ({ point.x - radius, point.y - radius, radius * 2.0f, radius * 2.0f });
        }
    }

    juce::Point<float> isoPoint (FVec3 point, juce::Rectangle<float> area) const
    {
        const auto s = juce::jmin (area.getWidth() / 17.4f, area.getHeight() / 16.0f);
        const auto ox = point.x - (float) (gridSize - 1) * 0.5f;
        const auto oy = point.y - (float) (gridSize - 1) * 0.5f;
        const auto oz = point.z - (float) (gridSize - 1) * 0.5f;

        return {
            area.getCentreX() + (ox - oz) * s * 0.92f,
            area.getCentreY() + (ox + oz) * s * 0.42f - oy * s * 0.82f + s * 1.12f
        };
    }

    void drawSelectedFaceInPreview (juce::Graphics& g, juce::Rectangle<float> area)
    {
        const auto max = (float) (gridSize - 1);
        std::array<FVec3, 4> corners;

        switch (selectedFace)
        {
            case 0: corners = {{{ 0.0f, 0.0f, max }, { max, 0.0f, max }, { max, max, max }, { 0.0f, max, max }}}; break;
            case 1: corners = {{{ max, 0.0f, max }, { max, 0.0f, 0.0f }, { max, max, 0.0f }, { max, max, max }}}; break;
            case 2: corners = {{{ max, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, max, 0.0f }, { max, max, 0.0f }}}; break;
            case 3: corners = {{{ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, max }, { 0.0f, max, max }, { 0.0f, max, 0.0f }}}; break;
            case 4: corners = {{{ 0.0f, max, max }, { max, max, max }, { max, max, 0.0f }, { 0.0f, max, 0.0f }}}; break;
            case 5: corners = {{{ 0.0f, 0.0f, 0.0f }, { max, 0.0f, 0.0f }, { max, 0.0f, max }, { 0.0f, 0.0f, max }}}; break;
            default: return;
        }

        juce::Path facePath;
        const auto p0 = isoPoint (corners[0], area);
        facePath.startNewSubPath (p0);

        for (size_t i = 1; i < corners.size(); ++i)
            facePath.lineTo (isoPoint (corners[i], area));

        facePath.closeSubPath();

        const auto faceColour = PipeLookAndFeel::rainbowForIndex (selectedFace);
        g.setColour (faceColour.withAlpha (0.11f));
        g.fillPath (facePath);
        g.setColour (faceColour.withAlpha (0.78f));
        g.strokePath (facePath, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const auto centre = isoPoint ({ (corners[0].x + corners[2].x) * 0.5f,
                                        (corners[0].y + corners[2].y) * 0.5f,
                                        (corners[0].z + corners[2].z) * 0.5f },
                                      area);
        g.setColour (faceColour);
        g.fillEllipse ({ centre.x - 3.0f, centre.y - 3.0f, 6.0f, 6.0f });
    }

    void drawPreviewGrid (juce::Graphics& g, juce::Rectangle<float> area)
    {
        const auto max = (float) (gridSize - 1);

        auto line = [&] (FVec3 a, FVec3 b, juce::Colour colour, float thickness)
        {
            g.setColour (colour);
            g.drawLine ({ isoPoint (a, area), isoPoint (b, area) }, thickness);
        };

        for (int i = 0; i < gridSize; ++i)
        {
            const auto p = (float) i;
            const auto major = i == 0 || i == gridSize - 1 || i == selectedDepth;
            const auto alpha = major ? 0.28f : 0.13f;
            const auto thickness = major ? 1.0f : 0.58f;

            line ({ 0.0f, p, 0.0f }, { max, p, 0.0f }, PipeLookAndFeel::aqua.withAlpha (alpha), thickness);
            line ({ 0.0f, p, max }, { max, p, max }, PipeLookAndFeel::aqua.withAlpha (alpha), thickness);
            line ({ 0.0f, 0.0f, p }, { max, 0.0f, p }, PipeLookAndFeel::lavender.withAlpha (alpha), thickness);
            line ({ 0.0f, max, p }, { max, max, p }, PipeLookAndFeel::lavender.withAlpha (alpha), thickness);
            line ({ p, 0.0f, 0.0f }, { p, 0.0f, max }, PipeLookAndFeel::pink.withAlpha (alpha), thickness);
            line ({ p, max, 0.0f }, { p, max, max }, PipeLookAndFeel::pink.withAlpha (alpha), thickness);
        }

        const auto edge = PipeLookAndFeel::ink.withAlpha (0.42f);
        constexpr float edgeThickness = 1.35f;
        line ({ 0.0f, 0.0f, 0.0f }, { max, 0.0f, 0.0f }, edge, edgeThickness);
        line ({ max, 0.0f, 0.0f }, { max, 0.0f, max }, edge, edgeThickness);
        line ({ max, 0.0f, max }, { 0.0f, 0.0f, max }, edge, edgeThickness);
        line ({ 0.0f, 0.0f, max }, { 0.0f, 0.0f, 0.0f }, edge, edgeThickness);
        line ({ 0.0f, max, 0.0f }, { max, max, 0.0f }, edge, edgeThickness);
        line ({ max, max, 0.0f }, { max, max, max }, edge, edgeThickness);
        line ({ max, max, max }, { 0.0f, max, max }, edge, edgeThickness);
        line ({ 0.0f, max, max }, { 0.0f, max, 0.0f }, edge, edgeThickness);
        line ({ 0.0f, 0.0f, 0.0f }, { 0.0f, max, 0.0f }, edge, edgeThickness);
        line ({ max, 0.0f, 0.0f }, { max, max, 0.0f }, edge, edgeThickness);
        line ({ max, 0.0f, max }, { max, max, max }, edge, edgeThickness);
        line ({ 0.0f, 0.0f, max }, { 0.0f, max, max }, edge, edgeThickness);
    }

    void drawPreview (juce::Graphics& g, const Layout& layout)
    {
        g.setColour (PipeLookAndFeel::panel);
        g.fillRect (layout.preview);
        g.setColour (PipeLookAndFeel::line.withAlpha (0.42f));
        g.drawVerticalLine (layout.preview.getX(), (float) layout.preview.getY(), (float) layout.preview.getBottom());

        auto panel = layout.preview.reduced (18, 20).toFloat();
        auto titleArea = panel.removeFromTop (28.0f);
        panel.removeFromTop (18.0f);

        const auto reservedForControls = large3DView ? 210.0f : 220.0f;
        const auto cardSide = juce::jmin (panel.getWidth(),
                                          juce::jmax (220.0f, panel.getHeight() - reservedForControls),
                                          large3DView ? 560.0f : 360.0f);
        const auto card = juce::Rectangle<float> (cardSide, cardSide)
                            .withCentre ({ panel.getCentreX(), panel.getY() + cardSide * 0.5f });
        const auto area = card.reduced (30.0f, 28.0f);

        g.setColour (PipeLookAndFeel::panel2.withAlpha (0.54f));
        g.fillRoundedRectangle (card, 20.0f);
        g.setColour (PipeLookAndFeel::line.withAlpha (0.32f));
        g.drawRoundedRectangle (card, 20.0f, 1.0f);

        g.saveState();
        g.reduceClipRegion (card.toNearestInt());

        drawSelectedFaceInPreview (g, area);
        drawPreviewGrid (g, area);

        std::vector<IVec3> occupied;
        occupied.reserve ((size_t) network.occupiedCount());

        for (int z = 0; z < gridSize; ++z)
            for (int y = 0; y < gridSize; ++y)
                for (int x = 0; x < gridSize; ++x)
                    if (network.pipes[(size_t) indexOf ({ x, y, z })] != 0)
                        occupied.push_back ({ x, y, z });

        std::sort (occupied.begin(), occupied.end(), [] (IVec3 a, IVec3 b)
        {
            return a.x + a.z + a.y < b.x + b.z + b.y;
        });

        for (const auto voxel : occupied)
        {
            const auto mask = network.pipes[(size_t) indexOf (voxel)];
            const auto centre = isoPoint ({ (float) voxel.x, (float) voxel.y, (float) voxel.z }, area);
            const auto inSlice = voxelDepthForFace (selectedFace, voxel) == selectedDepth;
            const auto rainbow = PipeLookAndFeel::rainbowForIndex (voxel.x + voxel.y * 2 + voxel.z * 3);
            const auto colour = inSlice ? rainbow : rainbow.withAlpha (0.40f);

            g.setColour (colour);

            for (const auto& direction : directions)
            {
                if ((mask & direction.bit) == 0)
                    continue;

                const auto end = isoPoint ({ (float) voxel.x + (float) direction.vector.x * 0.47f,
                                             (float) voxel.y + (float) direction.vector.y * 0.47f,
                                             (float) voxel.z + (float) direction.vector.z * 0.47f },
                                           area);
                g.drawLine ({ centre, end }, inSlice ? 2.25f : 1.25f);
            }

            const auto radius = inSlice ? 3.0f : 2.0f;
            g.fillEllipse ({ centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f });
        }

        drawPreviewMarkers (g, area);
        drawPreviewFlows (g, area);

        g.restoreState();

        g.setFont (juce::FontOptions (11.0f).withStyle ("Bold"));
        g.setColour (PipeLookAndFeel::mint);
        g.drawFittedText ("3D - " + juce::String (faces[(size_t) selectedFace].name),
                          titleArea.toNearestInt(),
                          juce::Justification::centredLeft,
                          1);

        const auto faceLabel = juce::Rectangle<float> (card.getX(),
                                                       card.getBottom() + 8.0f,
                                                       card.getWidth(),
                                                       14.0f);
        g.setColour (PipeLookAndFeel::muted.withAlpha (0.82f));
        g.setFont (juce::FontOptions (9.0f).withStyle ("Bold"));
        g.drawFittedText ("FACE", faceLabel.toNearestInt(), juce::Justification::centredLeft, 1);

        const auto layerLabel = juce::Rectangle<float> (card.getX(),
                                                        card.getBottom() + 122.0f,
                                                        card.getWidth(),
                                                        14.0f);
        g.setColour (PipeLookAndFeel::lemon);
        g.drawFittedText ("LAYER " + juce::String (selectedDepth + 1).paddedLeft ('0', 2),
                          layerLabel.toNearestInt(),
                          juce::Justification::centredLeft,
                          1);

        drawInspector (g, card);
    }

    void drawInspector (juce::Graphics& g, juce::Rectangle<float> card) const
    {
        auto area = juce::Rectangle<float> (card.getX(),
                                            card.getBottom() + 186.0f,
                                            card.getWidth(),
                                            48.0f);

        g.setFont (juce::FontOptions (9.0f).withStyle ("Bold"));
        g.setColour (PipeLookAndFeel::muted.withAlpha (0.82f));
        g.drawFittedText ("SELECTED", area.removeFromTop (14.0f).toNearestInt(), juce::Justification::centredLeft, 1);

        g.setFont (juce::FontOptions (11.0f).withStyle ("Bold"));
        g.setColour (PipeLookAndFeel::ink.withAlpha (0.92f));

        const auto voxel = selectedVoxel();
        if (! voxel.has_value())
        {
            g.drawFittedText ("No cell", area.removeFromTop (18.0f).toNearestInt(), juce::Justification::centredLeft, 1);
            return;
        }

        const auto& v = *voxel;
        auto firstLine = "X" + juce::String (v.x + 1).paddedLeft ('0', 2)
                       + "  Y" + juce::String (v.y + 1).paddedLeft ('0', 2)
                       + "  Z" + juce::String (v.z + 1).paddedLeft ('0', 2);

        if (network.hasTap (v)) firstLine += network.isTapEnabled (v) ? "  tap on" : "  tap off";
        else if (network.hasDrain (v)) firstLine += network.isDrainOpen (v) ? "  drain open" : "  drain shut";
        else if (network.hasValve (v)) firstLine += "  valve";
        else if (network.hasPipe (v)) firstLine += "  pipe";
        else firstLine += "  empty";

        g.drawFittedText (firstLine, area.removeFromTop (20.0f).toNearestInt(), juce::Justification::centredLeft, 1);

        if (const auto* valve = selectedValve())
        {
            g.setColour (PipeLookAndFeel::pink.brighter (0.16f));
            g.drawFittedText ("Note " + noteName (noteInCurrentScale (valve->midiNote)),
                              area.removeFromTop (20.0f).toNearestInt(),
                              juce::Justification::centredLeft,
                              1);
        }
    }

    void drawPreviewMarkers (juce::Graphics& g, juce::Rectangle<float> area)
    {
        const auto pulse = playing ? 0.5f + 0.5f * std::sin ((float) animationPhase * 7.0f) : 0.0f;

        for (const auto tap : network.taps)
        {
            const auto point = isoPoint ({ (float) tap.x, (float) tap.y, (float) tap.z }, area);
            const auto enabled = network.isTapEnabled (tap);
            const auto tapColour = enabled ? PipeLookAndFeel::water : PipeLookAndFeel::muted;

            g.setColour (tapColour.withAlpha (enabled ? 0.20f : 0.08f));
            g.fillEllipse ({ point.x - 8.0f, point.y - 8.0f, 16.0f, 16.0f });
            if (playing && enabled)
            {
                const auto r = 8.0f + pulse * 5.0f;
                g.setColour (tapColour.withAlpha (0.18f * (1.0f - pulse)));
                g.drawEllipse ({ point.x - r, point.y - r, r * 2.0f, r * 2.0f }, 1.4f);
            }
            g.setColour (tapColour.withAlpha (enabled ? 1.0f : 0.58f));
            g.drawEllipse ({ point.x - 5.5f, point.y - 5.5f, 11.0f, 11.0f }, 2.0f);

            if (! enabled)
                g.drawLine (point.x - 5.0f, point.y + 5.0f, point.x + 5.0f, point.y - 5.0f, 1.6f);
        }

        for (const auto valve : network.valves)
        {
            const auto point = isoPoint ({ (float) valve.position.x, (float) valve.position.y, (float) valve.position.z }, area);
            g.setColour (PipeLookAndFeel::pink.withAlpha (0.24f));
            g.fillRoundedRectangle ({ point.x - 7.0f, point.y - 7.0f, 14.0f, 14.0f }, 3.0f);
            g.setColour (PipeLookAndFeel::pink);
            g.drawRoundedRectangle ({ point.x - 5.0f, point.y - 5.0f, 10.0f, 10.0f }, 3.0f, 1.6f);
        }

        for (const auto drain : network.drains)
        {
            const auto point = isoPoint ({ (float) drain.x, (float) drain.y, (float) drain.z }, area);
            const auto open = network.isDrainOpen (drain);
            const auto drainColour = open ? PipeLookAndFeel::coral : PipeLookAndFeel::muted;

            g.setColour (drainColour.withAlpha (open ? 0.18f : 0.08f));
            g.fillEllipse ({ point.x - 7.0f, point.y - 7.0f, 14.0f, 14.0f });
            if (playing && open)
            {
                g.setColour (drainColour.withAlpha (0.18f + pulse * 0.16f));
                g.drawLine (point.x, point.y + 6.0f, point.x, point.y + 11.0f + pulse * 5.0f, 1.6f);
            }
            g.setColour (drainColour.withAlpha (open ? 1.0f : 0.58f));
            juce::Path funnel;
            funnel.startNewSubPath (point.x - 5.5f, point.y - 3.0f);
            funnel.lineTo (point.x + 5.5f, point.y - 3.0f);
            funnel.lineTo (point.x, point.y + 5.5f);
            funnel.closeSubPath();
            g.fillPath (funnel);

            if (! open)
                g.drawLine (point.x - 5.0f, point.y + 5.0f, point.x + 5.0f, point.y - 5.0f, 1.6f);
        }
    }

    void drawPreviewFlows (juce::Graphics& g, juce::Rectangle<float> area)
    {
        for (const auto& flow : flows)
        {
            const auto point3 = currentFlowPosition (flow);
            const auto point = isoPoint (point3, area);
            g.setColour (PipeLookAndFeel::water.withAlpha (0.18f));
            g.fillEllipse ({ point.x - 5.8f, point.y - 5.8f, 11.6f, 11.6f });
            g.setColour (PipeLookAndFeel::water.brighter (0.22f));
            g.fillEllipse ({ point.x - 2.9f, point.y - 2.9f, 5.8f, 5.8f });
        }
    }

    void drawFooter (juce::Graphics& g, const Layout& layout)
    {
        g.setColour (PipeLookAndFeel::panel);
        g.fillRect (layout.footer);
        g.setColour (PipeLookAndFeel::line.withAlpha (0.42f));
        g.drawHorizontalLine (layout.footer.getY(), 0.0f, (float) getWidth());

        g.setFont (juce::FontOptions (10.0f));
        g.setColour (PipeLookAndFeel::mint);
        g.drawFittedText (status, layout.footer.reduced (14, 0), juce::Justification::centredLeft, 1);

        const auto stats = juce::String (network.occupiedCount()) + " cells / "
                         + juce::String (network.armCount()) + " arms / "
                         + juce::String ((int) network.taps.size()) + " taps / "
                         + juce::String ((int) network.valves.size()) + " valves / "
                         + juce::String ((int) network.drains.size()) + " drains / "
                         + juce::String ((int) flows.size()) + " water / "
                         + (soundMode == SoundMode::superCollider
                                ? (scEngine.isReady() ? "SC" : "SC missing")
                                : "JUCE");
        g.setColour (PipeLookAndFeel::muted.withAlpha (0.82f));
        g.drawFittedText (stats, layout.footer.reduced (14, 0), juce::Justification::centredRight, 1);
    }

    void buttonClicked (juce::Button* button) override
    {
        if (button == &selectButton)     setTool (Tool::select);
        else if (button == &pipeButton)  setTool (Tool::pipe);
        else if (button == &tapButton)   setTool (Tool::tap);
        else if (button == &valveButton) setTool (Tool::valve);
        else if (button == &drainButton) setTool (Tool::drain);
        else if (button == &eraseButton) setTool (Tool::erase);
        else if (button == &playButton)  setPlaying (! playing);
        else if (button == &stopButton)  setPlaying (false);
        else if (button == &noteDownButton) adjustSelectedValveNote (-1);
        else if (button == &noteUpButton)   adjustSelectedValveNote (1);
        else if (button == &compileScButton) compileScProgram();
        else if (button == &demoButton)
        {
            setPlaying (false);
            beginChange();
            network.loadDemo();
            setSelectedFace (0);
            selectedDepth = 0;
            selectedCell.reset();
            layerSlider.setValue (1.0, juce::dontSendNotification);
            updateInspectorControls();
            setStatus ("Demo loaded");
            repaint();
        }
        else if (button == &clearButton)
        {
            setPlaying (false);
            beginChange();
            network.clear();
            selectedCell.reset();
            updateInspectorControls();
            setStatus ("Cleared");
            repaint();
        }
        else
        {
            for (int i = 0; i < (int) faceButtons.size(); ++i)
            {
                if (button == &faceButtons[(size_t) i])
                {
                    setSelectedFace (i);
                    return;
                }
            }
        }
    }

    void sliderValueChanged (juce::Slider* slider) override
    {
        if (slider == &layerSlider)
        {
            selectedDepth = juce::jlimit (0, gridSize - 1, (int) std::round (layerSlider.getValue()) - 1);
            layerSlider.setValue (selectedDepth + 1, juce::dontSendNotification);
            updateInspectorControls();
            repaint();
        }
        else if (slider == &tempoSlider)
        {
            bpm = tempoSlider.getValue();
            setStatus ("Tempo " + juce::String ((int) std::round (bpm)) + " BPM");
        }
        else if (slider == &noteSlider)
        {
            setSelectedValveNote ((int) std::round (noteSlider.getValue()));
        }
    }

    void comboBoxChanged (juce::ComboBox* comboBoxThatHasChanged) override
    {
        if (comboBoxThatHasChanged == &faceBox)
        {
            setSelectedFace (faceBox.getSelectedItemIndex());
        }
        else if (comboBoxThatHasChanged == &keyBox)
        {
            rootNote = juce::jlimit (0, 11, keyBox.getSelectedItemIndex());
            updateInspectorControls();
            setStatus ("Key " + keyBox.getText());
            repaint();
        }
        else if (comboBoxThatHasChanged == &scaleBox)
        {
            scaleIndex = juce::jlimit (0, 4, scaleBox.getSelectedItemIndex());
            updateInspectorControls();
            setStatus ("Scale " + scaleNameForIndex (scaleIndex));
            repaint();
        }
    }
};

class PipeApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override             { return true; }

    void initialise (const juce::String&) override
    {
        mainWindow.reset (new MainWindow (getApplicationName()));
    }

    void shutdown() override
    {
        mainWindow = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted (const juce::String&) override {}

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (juce::String name)
            : DocumentWindow (std::move (name),
                              PipeLookAndFeel::bg,
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setResizable (true, true);
            setContentOwned (new MainComponent(), true);
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
}

START_JUCE_APPLICATION (PipeApplication)
