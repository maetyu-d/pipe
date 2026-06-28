#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include "InternalEvent.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace gridcollider
{
class EmbeddedScAudioEngine
{
public:
    static constexpr int channelCount = 16;

    EmbeddedScAudioEngine();
    ~EmbeddedScAudioEngine();

    bool prepare(double sampleRate, int maximumBlockSize, int outputChannels);
    void release() noexcept;
    void render(juce::AudioBuffer<float>& output);
    void renderRaw(juce::AudioBuffer<float>& output);
    void renderOffline(juce::AudioBuffer<float>& output);

    void enqueue(const std::vector<InternalEvent>& events);
    void setTransport(double bpm, std::uint64_t tick, bool playing);
    void setMasterLevel(float level);
    bool loadSynthDef(const juce::String& name, const juce::String& source);
    void setChannelInstrument(int channel, const juce::String& instrumentName);
    [[nodiscard]] juce::String getChannelInstrument(int channel) const;
    [[nodiscard]] static juce::StringArray getDefaultChannelInstruments();
    [[nodiscard]] static juce::StringArray getDefaultSynthDefNames();
    [[nodiscard]] static juce::String getDefaultSynthDefSource(const juce::String& name);

    [[nodiscard]] bool isReady() const noexcept;
    [[nodiscard]] juce::String getStatusText() const;
    [[nodiscard]] juce::String getLastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EmbeddedScAudioEngine)
};
}
