#include "EmbeddedScAudioEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <type_traits>

#if GRIDCOLLIDER_HAS_SC_HEADERS
#include <SC_WorldOptions.h>
#endif

#if JUCE_MAC || JUCE_LINUX
#include <dlfcn.h>
#endif

#if JUCE_WINDOWS
#define NOMINMAX
#include <windows.h>
#endif

namespace gridcollider
{
namespace
{
constexpr int hostRenderQuantum = 64;
constexpr int sourceGroupId = 1;
constexpr int masterBus = 0;
constexpr float outputLimit = 0.92f;
constexpr float limiterCeiling = 0.86f;

struct OscArgument
{
    enum class Type { integer, floating, string, blob };

    static OscArgument integer(const std::int32_t value) { return { Type::integer, value, 0.0f, {}, {} }; }
    static OscArgument floating(const float value) { return { Type::floating, 0, value, {}, {} }; }
    static OscArgument string(juce::String value) { return { Type::string, 0, 0.0f, std::move(value), {} }; }
    static OscArgument blob(std::vector<char> value) { return { Type::blob, 0, 0.0f, {}, std::move(value) }; }

    Type type;
    std::int32_t intValue = 0;
    float floatValue = 0.0f;
    juce::String stringValue;
    std::vector<char> blobValue;
};

void appendPaddedString(std::vector<char>& packet, const juce::String& value)
{
    const auto utf8 = value.toStdString();
    packet.insert(packet.end(), utf8.begin(), utf8.end());
    packet.push_back('\0');

    while ((packet.size() % 4) != 0)
        packet.push_back('\0');
}

void appendInt32(std::vector<char>& packet, const std::int32_t value)
{
    packet.push_back(static_cast<char>((value >> 24) & 0xff));
    packet.push_back(static_cast<char>((value >> 16) & 0xff));
    packet.push_back(static_cast<char>((value >> 8) & 0xff));
    packet.push_back(static_cast<char>(value & 0xff));
}

void appendFloat32(std::vector<char>& packet, const float value)
{
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    appendInt32(packet, static_cast<std::int32_t>(bits));
}

void appendBlob(std::vector<char>& packet, const std::vector<char>& blob)
{
    appendInt32(packet, static_cast<std::int32_t>(blob.size()));
    packet.insert(packet.end(), blob.begin(), blob.end());

    while ((packet.size() % 4) != 0)
        packet.push_back('\0');
}

std::vector<char> buildOscMessage(const juce::String& address, const std::vector<OscArgument>& arguments)
{
    std::vector<char> packet;
    appendPaddedString(packet, address);

    juce::String tags = ",";
    for (const auto& argument : arguments)
    {
        switch (argument.type)
        {
            case OscArgument::Type::integer:  tags << "i"; break;
            case OscArgument::Type::floating: tags << "f"; break;
            case OscArgument::Type::string:   tags << "s"; break;
            case OscArgument::Type::blob:     tags << "b"; break;
        }
    }

    appendPaddedString(packet, tags);

    for (const auto& argument : arguments)
    {
        switch (argument.type)
        {
            case OscArgument::Type::integer:  appendInt32(packet, argument.intValue); break;
            case OscArgument::Type::floating: appendFloat32(packet, argument.floatValue); break;
            case OscArgument::Type::string:   appendPaddedString(packet, argument.stringValue); break;
            case OscArgument::Type::blob:     appendBlob(packet, argument.blobValue); break;
        }
    }

    return packet;
}

juce::String starterSynthSource(const juce::String& name)
{
    if (name == "kick")
        return R"SC(SynthDef(\kick, { |out = 0, pitch = 36, amp = 0.7, sustain = 0.25, pan = 0|
    var env, freq, sig;
    env = EnvGen.kr(Env.perc(0.001, sustain), doneAction: 2);
    freq = EnvGen.kr(Env([pitch.midicps * 2.4, pitch.midicps], [0.05], -4));
    sig = SinOsc.ar(freq) * env * amp;
    Out.ar(out, Pan2.ar((sig * 2.5).tanh, pan));
}))SC";

    if (name == "snare")
        return R"SC(SynthDef(\snare, { |out = 0, pitch = 60, amp = 0.45, sustain = 0.18, pan = 0|
    var env, sig;
    env = EnvGen.kr(Env.perc(0.001, sustain), doneAction: 2);
    sig = HPF.ar(WhiteNoise.ar, 1500) * env * amp;
    Out.ar(out, Pan2.ar(sig.tanh, pan));
}))SC";

    if (name == "hat")
        return R"SC(SynthDef(\hat, { |out = 0, pitch = 80, amp = 0.16, sustain = 0.035, pan = 0|
    var env, sig;
    env = EnvGen.kr(Env.perc(0.0005, sustain.max(0.009), curve: -10), doneAction: 2);
    sig = HPF.ar(WhiteNoise.ar, pitch.midicps.max(8200));
    sig = BPF.ar(sig, pitch.midicps.max(9000), 0.42);
    Out.ar(out, Pan2.ar(sig * env * amp, pan));
}))SC";

    if (name == "bass")
        return R"SC(SynthDef(\bass, { |out = 0, pitch = 36, amp = 0.34, sustain = 0.8, pan = 0|
    var freq, env, sig;
    freq = pitch.midicps;
    env = EnvGen.kr(Env.linen(0.018, sustain.max(0.18), 0.18, curve: -3), doneAction: 2);
    sig = (Saw.ar(freq) * 0.55) + (SinOsc.ar(freq * 0.5) * 0.45);
    sig = LPF.ar(sig, (freq * 5).clip(90, 2400)) * env * amp;
    Out.ar(out, Pan2.ar(sig.softclip, pan));
}))SC";

    if (name == "grain")
        return R"SC(SynthDef(\grain, { |out = 0, pitch = 60, amp = 0.18, sustain = 1.2, pan = 0|
    var freq, env, sig;
    freq = pitch.midicps;
    env = EnvGen.kr(Env.linen(0.03, sustain.max(0.2), 0.55, curve: -2), doneAction: 2);
    sig = SinOsc.ar(freq * LFNoise1.kr(4).range(0.997, 1.006)) * env * amp;
    sig = BPF.ar(sig, (freq * 2.2).clip(180, 7000), 0.22) + (sig * 0.28);
    Out.ar(out, Pan2.ar(sig, pan));
}))SC";

    if (name == "drone")
        return R"SC(SynthDef(\drone, { |out = 0, pitch = 48, amp = 0.20, sustain = 4.0, pan = 0|
    var freq, env, sig;
    freq = pitch.midicps;
    env = EnvGen.kr(Env.linen(0.45, sustain.max(1.0), 1.5, curve: -3), doneAction: 2);
    sig = Mix(Saw.ar(freq * [0.5, 1, 1.002, 1.5])) * 0.18;
    sig = RLPF.ar(sig, (freq * 3).clip(120, 4200), 0.24) * env * amp;
    Out.ar(out, Pan2.ar(sig, pan));
}))SC";

    if (name == "pluck")
        return R"SC(SynthDef(\pluck, { |out = 0, pitch = 60, amp = 0.18, sustain = 0.13, pan = 0|
    var freq, env, snap, body, sig;
    freq = pitch.midicps;
    env = EnvGen.kr(Env.perc(0.0008, sustain.max(0.028), curve: -8), doneAction: 2);
    snap = BPF.ar(WhiteNoise.ar, (freq * 7.5).clip(1600, 12000), 0.035) * 0.85;
    body = SinOsc.ar(freq * 2.0, 0, 0.09);
    sig = HPF.ar(snap + body, 550) * env * amp;
    Out.ar(out, Pan2.ar(sig, pan));
}))SC";

    if (name == "pad")
        return R"SC(SynthDef(\pad, { |out = 0, pitch = 60, amp = 0.18, sustain = 3.2, pan = 0|
    var freq, env, sig;
    freq = pitch.midicps;
    env = EnvGen.kr(Env.linen(0.38, sustain.max(0.5), 1.1, curve: -3), doneAction: 2);
    sig = Mix(Pulse.ar(freq * [0.5, 1, 1.005], [0.38, 0.5, 0.62])) * 0.16;
    sig = RLPF.ar(sig, (freq * 2.6).clip(180, 5200), 0.28) * env * amp;
    Out.ar(out, Pan2.ar(sig, pan));
}))SC";

    if (name == "bell")
        return R"SC(SynthDef(\bell, { |out = 0, pitch = 72, amp = 0.14, sustain = 1.65, pan = 0|
    var freq, env, sig;
    freq = pitch.midicps;
    env = EnvGen.kr(Env.perc(0.004, sustain.max(0.28), curve: -5), doneAction: 2);
    sig = SinOsc.ar(freq * [1, 2.73, 3.91, 5.47], 0, [0.64, 0.30, 0.15, 0.08]).sum;
    sig = HPF.ar(sig * env * amp, 520);
    Out.ar(out, Pan2.ar(sig, pan));
}))SC";

    if (name == "fm")
        return R"SC(SynthDef(\fm, { |out = 0, pitch = 60, amp = 0.14, sustain = 0.42, pan = 0|
    var freq, env, index, mod, sig;
    freq = pitch.midicps;
    env = EnvGen.kr(Env.perc(0.001, sustain.max(0.06), curve: -6), doneAction: 2);
    index = EnvGen.kr(Env([7.2, 0.35], [sustain.max(0.07)], -7));
    mod = SinOsc.ar(freq * 3.005) * freq * index;
    sig = SinOsc.ar((freq * 1.5) + mod) * env * amp;
    sig = BPF.ar(sig, (freq * 6).clip(700, 7800), 0.34) + (HPF.ar(sig, 900) * 0.35);
    Out.ar(out, Pan2.ar(sig, pan));
}))SC";

    if (name == "acid")
        return R"SC(SynthDef(\acid, { |out = 0, pitch = 48, amp = 0.12, sustain = 0.20, pan = 0|
    var freq, env, sweep, sig;
    freq = pitch.midicps;
    env = EnvGen.kr(Env.perc(0.002, sustain.max(0.035), curve: -8), doneAction: 2);
    sweep = EnvGen.kr(Env([freq * 18, freq * 2.6], [sustain.max(0.04)], -7));
    sig = RLPF.ar(Pulse.ar(freq, 0.18), sweep.clip(420, 9800), 0.09);
    sig = HPF.ar(sig * env * amp, 140).softclip;
    Out.ar(out, Pan2.ar(sig, pan));
}))SC";

    if (name == "string")
        return R"SC(SynthDef(\string, { |out = 0, pitch = 60, amp = 0.20, sustain = 2.8, pan = 0|
    var freq, env, sig;
    freq = pitch.midicps;
    env = EnvGen.kr(Env.linen(0.08, sustain.max(0.3), 0.7, curve: -2), doneAction: 2);
    sig = Mix(VarSaw.ar(freq * [1, 1.003, 1.5], 0, 0.38)) * 0.22;
    sig = BLowPass.ar(sig, (freq * 4).clip(240, 4800), 0.55) * env * amp;
    Out.ar(out, Pan2.ar(sig, pan));
}))SC";

    if (name == "noise")
        return R"SC(SynthDef(\noise, { |out = 0, pitch = 72, amp = 0.10, sustain = 0.070, pan = 0|
    var env, centre, sig;
    env = EnvGen.kr(Env.perc(0.002, sustain.max(0.018), curve: -8), doneAction: 2);
    centre = pitch.midicps.clip(5200, 11800);
    sig = BPF.ar(WhiteNoise.ar, centre, 0.16) + (HPF.ar(WhiteNoise.ar, 9000) * 0.18);
    Out.ar(out, Pan2.ar(sig * env * amp, pan));
}))SC";

    if (name == "sub")
        return R"SC(SynthDef(\sub, { |out = 0, pitch = 36, amp = 0.26, sustain = 1.0, pan = 0|
    var freq, env, sig;
    freq = pitch.midicps;
    env = EnvGen.kr(Env.linen(0.012, sustain.max(0.14), 0.25, curve: -3), doneAction: 2);
    sig = SinOsc.ar(freq * 0.5) + (SinOsc.ar(freq) * 0.42);
    sig = LPF.ar(sig, 180) * env * amp;
    Out.ar(out, Pan2.ar(sig.softclip, pan));
}))SC";

    if (name == "lead")
        return R"SC(SynthDef(\lead, { |out = 0, pitch = 60, amp = 0.15, sustain = 0.58, pan = 0|
    var freq, env, sig;
    freq = pitch.midicps;
    env = EnvGen.kr(Env.linen(0.012, sustain.max(0.09), 0.12, curve: -4), doneAction: 2);
    sig = VarSaw.ar(freq * [1, 1.004], 0, [0.20, 0.64]).sum * 0.20;
    sig = RLPF.ar(sig, (freq * 7.2).clip(1300, 9200), 0.20) * env * amp;
    Out.ar(out, Pan2.ar(sig, pan));
}))SC";

    if (name == "perc")
        return R"SC(SynthDef(\perc, { |out = 0, pitch = 60, amp = 0.24, sustain = 0.20, pan = 0|
    var freq, env, sig;
    freq = pitch.midicps;
    env = EnvGen.kr(Env.perc(0.0015, sustain.max(0.03), curve: -6), doneAction: 2);
    sig = (SinOsc.ar(freq * 1.5) * 0.6) + (BPF.ar(WhiteNoise.ar, freq * 6, 0.18) * 0.4);
    Out.ar(out, Pan2.ar(sig * env * amp, pan));
}))SC";

    if (name == "metal")
        return R"SC(SynthDef(\metal, { |out = 0, pitch = 72, amp = 0.15, sustain = 1.4, pan = 0|
    var freq, env, sig;
    freq = pitch.midicps;
    env = EnvGen.kr(Env.perc(0.004, sustain.max(0.18), curve: -4), doneAction: 2);
    sig = Klank.ar(`[
        freq * [1, 1.41, 2.07, 2.83, 3.77],
        [1, 0.5, 0.32, 0.18, 0.11],
        sustain.max(0.2) * [1, 0.8, 0.6, 0.45, 0.35]
    ], PinkNoise.ar(0.012));
    Out.ar(out, Pan2.ar(sig * env * amp, pan));
}))SC";

    if (name == "choir")
        return R"SC(SynthDef(\choir, { |out = 0, pitch = 60, amp = 0.18, sustain = 3.5, pan = 0|
    var freq, env, sig;
    freq = pitch.midicps;
    env = EnvGen.kr(Env.linen(0.25, sustain.max(0.6), 1.2, curve: -3), doneAction: 2);
    sig = Mix(SinOsc.ar(freq * [0.5, 1, 1.5, 2.01], 0, [0.45, 0.38, 0.22, 0.10]));
    sig = RLPF.ar(sig, (freq * 3.4).clip(280, 5200), 0.42) * env * amp;
    Out.ar(out, Pan2.ar(sig, pan));
}))SC";

    return R"SC(SynthDef(\tone, { |out = 0, pitch = 60, amp = 0.26, sustain = 0.7, pan = 0|
    var freq, env, sig;
    freq = pitch.midicps;
    env = EnvGen.kr(Env.linen(0.025, sustain.max(0.12), 0.24, curve: -3), doneAction: 2);
    sig = (SinOsc.ar(freq) * 0.82) + (Pulse.ar(freq * 0.5, 0.42) * 0.18);
    sig = RLPF.ar(sig, (freq * 4).clip(240, 6500), 0.38) * env * amp;
    Out.ar(out, Pan2.ar(sig, pan));
}))SC";
}

void debugLog(const juce::String& message)
{
    std::fprintf(stderr, "[unfolding SC] %s\n", message.toRawUTF8());
    std::fflush(stderr);
}

juce::String instrumentFor(const juce::String& name)
{
    if (name == "mono")
        return "bass";
    if (name == "midi")
        return "tone";
    if (name == "kick" || name == "snare" || name == "hat" || name == "bass" || name == "tone" || name == "grain" || name == "drone"
        || name == "pluck" || name == "pad" || name == "bell" || name == "fm" || name == "acid" || name == "string"
        || name == "noise" || name == "sub" || name == "lead" || name == "perc" || name == "metal" || name == "choir")
        return name;
    return "tone";
}

float defaultLevelFor(const juce::String& instrument)
{
    if (instrument == "kick")  return 0.95f;
    if (instrument == "snare") return 0.78f;
    if (instrument == "hat")   return 0.55f;
    if (instrument == "bass")  return 0.82f;
    if (instrument == "grain") return 0.62f;
    if (instrument == "drone") return 0.50f;
    if (instrument == "sub")   return 0.62f;
    if (instrument == "pad")   return 0.52f;
    if (instrument == "choir") return 0.50f;
    if (instrument == "pluck") return 0.58f;
    if (instrument == "bell")  return 0.46f;
    if (instrument == "fm")    return 0.52f;
    if (instrument == "acid")  return 0.56f;
    if (instrument == "string") return 0.52f;
    if (instrument == "noise") return 0.36f;
    if (instrument == "lead")  return 0.56f;
    if (instrument == "perc")  return 0.42f;
    if (instrument == "metal") return 0.38f;
    return 0.68f;
}

juce::String normalisedControlParameter(const juce::String& name)
{
    const auto lower = name.toLowerCase();

    if (lower == "level" || lower == "volume" || lower == "amp" || lower == "gain" || lower == "cc7")
        return "level";
    if (lower == "pan" || lower == "cc10")
        return "pan";
    if (lower == "drive" || lower == "cc74")
        return "drive";
    if (lower == "master")
        return "master";

    return lower;
}

float panForX(const int x)
{
    return juce::jlimit(-0.9f, 0.9f, (static_cast<float>(x) / 63.0f) * 1.8f - 0.9f);
}

float secondsForTicks(const std::uint64_t ticks, const double bpm)
{
    const auto safeBpm = juce::jlimit(1.0, 999.0, std::isfinite(bpm) ? bpm : 120.0);
    return static_cast<float>(juce::jmax(1.0, static_cast<double>(ticks)) * 60.0 / safeBpm);
}

juce::StringArray libraryCandidates(const char* envName, const juce::String& relativeFromAlchemy, const juce::String& fileName)
{
    juce::StringArray candidates;

    if (const auto* env = std::getenv(envName))
        candidates.addIfNotAlreadyThere(env);

    const juce::File alchemyRoot(GRIDCOLLIDER_ALCHEMY_SC_ROOT);
    candidates.addIfNotAlreadyThere(alchemyRoot.getChildFile(relativeFromAlchemy).getFullPathName());
    candidates.addIfNotAlreadyThere(fileName);
    return candidates;
}

std::array<juce::String, EmbeddedScAudioEngine::channelCount> makeDefaultChannelPalette()
{
    return {
        "tone", "bass", "drone", "grain",
        "pluck", "pad", "bell", "fm",
        "acid", "kick", "snare", "hat",
        "string", "noise", "lead", "choir"
    };
}

juce::StringArray defaultSynthDefNames()
{
    return {
        "kick", "snare", "hat", "bass", "tone", "grain", "drone",
        "pluck", "pad", "bell", "fm", "acid", "string", "noise",
        "sub", "lead", "perc", "metal", "choir"
    };
}
}

struct EmbeddedScAudioEngine::Impl
{
#if GRIDCOLLIDER_HAS_SC_HEADERS
    using WorldNewFn = World* (*) (WorldOptions*);
    using WorldCleanupFn = void (*) (World*, bool);
    using WorldSendPacketFn = bool (*) (World*, int, char*, ReplyFunc);
    using WorldRenderHostAudioFn = bool (*) (World*, const float*, float*, int, int, int);
    using CompileSynthDefFn = bool (*) (const char*, const char*, const char*, char*, int);
    using InitialiseLangFn = bool (*) (const char*, char*, int);

    ~Impl() { release(); }

    bool prepare(const double sampleRate, const int maximumBlockSize, const int outputChannels)
    {
        const juce::ScopedLock lock(engineLock);
        releaseUnlocked();

        if (! loadServerApi() || ! loadLanguageApi())
            return false;

        currentSampleRate = sampleRate;
        maxBlockSize = juce::jmax(maximumBlockSize, hostRenderQuantum);
        numOutputChannels = juce::jlimit(2, 512, outputChannels);
        interleavedOutput.assign(static_cast<std::size_t>(maxBlockSize + hostRenderQuantum) * static_cast<std::size_t>(numOutputChannels), 0.0f);

        worldOptions = WorldOptions();
        worldOptions.mRealTime = true;
        worldOptions.mRendezvous = false;
        worldOptions.mVerbosity = -1;
        worldOptions.mLoadGraphDefs = 0;
        worldOptions.mPreferredSampleRate = static_cast<uint32>(sampleRate);
        worldOptions.mPreferredHardwareBufferFrameSize = hostRenderQuantum;
        worldOptions.mBufLength = hostRenderQuantum;
        worldOptions.mNumInputBusChannels = 0;
        worldOptions.mNumOutputBusChannels = static_cast<uint32>(numOutputChannels);
        worldOptions.mNumAudioBusChannels = 1024;
        pluginPath = findPluginPath();
        worldOptions.mUGensPluginPath = pluginPath.isNotEmpty() ? pluginPath.toRawUTF8() : nullptr;

        world = worldNew(&worldOptions);
        if (world == nullptr)
        {
            lastError = "Embedded SuperCollider world could not start";
            releaseUnlocked();
            return false;
        }

        sendPacket(buildOscMessage("/g_new", { OscArgument::integer(sourceGroupId), OscArgument::integer(0), OscArgument::integer(0) }));
        if (! loadStarterSynthDefs())
            return false;

        ready.store(true, std::memory_order_release);
        status = "EMBEDDED SC READY";
        debugLog("ready at " + juce::String(sampleRate, 1) + " Hz, block " + juce::String(maximumBlockSize));
        lastError.clear();
        return true;
    }

    void release() noexcept
    {
        const juce::ScopedLock lock(engineLock);
        releaseUnlocked();
    }

    void render(juce::AudioBuffer<float>& output)
    {
        const juce::ScopedLock lock(engineLock);
        output.clear();

        if (! ready.load(std::memory_order_acquire) || world == nullptr)
            return;

        renderUnlocked(output, true);
    }

    void renderRaw(juce::AudioBuffer<float>& output)
    {
        const juce::ScopedLock lock(engineLock);
        output.clear();

        if (! ready.load(std::memory_order_acquire) || world == nullptr)
            return;

        renderUnlocked(output, false);
    }

    void renderOffline(juce::AudioBuffer<float>& output)
    {
        const juce::ScopedLock lock(engineLock);
        output.clear();

        if (! ready.load(std::memory_order_acquire) || world == nullptr)
            return;

        renderUnlocked(output, true);
    }

    void enqueue(const std::vector<InternalEvent>& events)
    {
        const juce::ScopedLock lock(queueLock);

        for (const auto& event : events)
        {
            std::visit([this](const auto& typed)
            {
                using Event = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<Event, NoteEvent>)
                {
                    const auto synth = synthForTarget(typed.fields);
                    pendingPackets.push_back(packetFor(typed));
                    const auto queued = queuedEventCount.fetch_add(1, std::memory_order_relaxed);
                    if (queued < 16)
                        debugLog("queued note " + synth + " pitch " + juce::String(typed.fields.pitch));
                }
                else if constexpr (std::is_same_v<Event, TriggerEvent>)
                {
                    pendingPackets.push_back(packetFor(typed));
                    const auto queued = queuedEventCount.fetch_add(1, std::memory_order_relaxed);
                    if (queued < 16)
                        debugLog("queued trigger " + typed.triggerName);
                }
                else if constexpr (std::is_same_v<Event, ControlEvent>)
                {
                    for (auto packet : packetsFor(typed))
                        pendingPackets.push_back(std::move(packet));

                    queuedEventCount.fetch_add(1, std::memory_order_relaxed);
                }
            }, event);
        }
    }

    void setTransport(const double bpmToUse, const std::uint64_t tickToUse, const bool playingToUse)
    {
        bpm.store(bpmToUse, std::memory_order_relaxed);
        tick.store(tickToUse, std::memory_order_relaxed);
        playing.store(playingToUse, std::memory_order_relaxed);
    }

    void setMasterLevel(const float level)
    {
        const auto clamped = juce::jlimit(0.0f, 1.25f, level);
        masterLevel = clamped;
    }

    bool loadSynthDef(const juce::String& name, const juce::String& source)
    {
        if (! ready.load(std::memory_order_acquire) || compileSynthDef == nullptr)
        {
            lastError = "Embedded SuperCollider is not ready";
            return false;
        }

        std::vector<char> bytes;

        {
            const juce::ScopedLock compileGuard(synthCompileLock);

            if (! ready.load(std::memory_order_acquire) || compileSynthDef == nullptr)
            {
                lastError = "Embedded SuperCollider is not ready";
                return false;
            }

            if (! compileSynth(name, source, bytes))
                return false;
        }

        {
            const juce::ScopedLock lock(engineLock);

            if (! ready.load(std::memory_order_acquire) || world == nullptr)
            {
                lastError = "Embedded SuperCollider is not ready";
                return false;
            }

            sendPacket(buildOscMessage("/d_recv", { OscArgument::blob(std::move(bytes)) }));
        }

        registerSynthName(name);
        debugLog("loaded SynthDef " + name);
        return true;
    }

    juce::String getStatusText() const
    {
        if (ready.load(std::memory_order_acquire))
            return status
                + "  Q " + juce::String(static_cast<int>(queuedEventCount.load(std::memory_order_relaxed)))
                + "  S " + juce::String(static_cast<int>(sentEventCount.load(std::memory_order_relaxed)))
                + "  RF " + juce::String(static_cast<int>(renderFailures.load(std::memory_order_relaxed)));
        return lastError.isNotEmpty() ? "EMBEDDED SC OFF" : status;
    }

    bool isReady() const noexcept { return ready.load(std::memory_order_acquire); }

    juce::String getLastError() const
    {
        const juce::ScopedLock lock(engineLock);
        return lastError;
    }

private:
    void renderUnlocked(juce::AudioBuffer<float>& output, const bool applyMasterProcessing)
    {
        const auto frames = output.getNumSamples();
        if (frames <= 0 || frames > maxBlockSize)
            return;

        flushQueuedEvents();

        const auto paddedFrames = ((frames + hostRenderQuantum - 1) / hostRenderQuantum) * hostRenderQuantum;
        std::fill(interleavedOutput.begin(), interleavedOutput.begin() + static_cast<std::ptrdiff_t>(paddedFrames * numOutputChannels), 0.0f);

        if (! worldRenderHostAudio(world, nullptr, interleavedOutput.data(), paddedFrames, 0, numOutputChannels))
        {
            renderFailures.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        float peak = 0.0f;
        for (int frame = 0; frame < frames; ++frame)
        {
            for (int channel = 0; channel < numOutputChannels; ++channel)
                peak = juce::jmax(peak, std::abs(interleavedOutput[static_cast<std::size_t>(frame * numOutputChannels + channel)]));
        }

        auto outputGain = applyMasterProcessing ? juce::jlimit(0.0f, 1.25f, masterLevel) : 1.0f;
        if (applyMasterProcessing && peak * outputGain > limiterCeiling)
            outputGain = limiterCeiling / juce::jmax(peak, 0.000001f);

        for (int channel = 0; channel < output.getNumChannels(); ++channel)
        {
            auto* dst = output.getWritePointer(channel);
            const auto sourceChannel = juce::jmin(channel, numOutputChannels - 1);

            for (int frame = 0; frame < frames; ++frame)
            {
                const auto sample = interleavedOutput[static_cast<std::size_t>(frame * numOutputChannels + sourceChannel)] * outputGain;
                dst[frame] = std::isfinite(sample)
                                 ? (applyMasterProcessing ? juce::jlimit(-outputLimit, outputLimit, sample) : sample)
                                 : 0.0f;
            }
        }
    }

    static void* openLibrary(const juce::String& path)
    {
       #if JUCE_MAC || JUCE_LINUX
        return dlopen(path.toRawUTF8(), RTLD_NOW | RTLD_LOCAL);
       #elif JUCE_WINDOWS
        return LoadLibraryA(path.toRawUTF8());
       #else
        juce::ignoreUnused(path);
        return nullptr;
       #endif
    }

    static void closeLibrary(void* handle)
    {
        if (handle == nullptr)
            return;
       #if JUCE_MAC || JUCE_LINUX
        dlclose(handle);
       #elif JUCE_WINDOWS
        FreeLibrary(static_cast<HMODULE>(handle));
       #endif
    }

    template <typename Function>
    static bool loadSymbol(void* handle, Function& function, const char* name)
    {
       #if JUCE_MAC || JUCE_LINUX
        function = reinterpret_cast<Function>(dlsym(handle, name));
       #elif JUCE_WINDOWS
        function = reinterpret_cast<Function>(GetProcAddress(static_cast<HMODULE>(handle), name));
       #else
        juce::ignoreUnused(handle, name);
        function = nullptr;
       #endif
        return function != nullptr;
    }

    bool loadServerApi()
    {
        for (const auto& candidate : libraryCandidates("GRIDCOLLIDER_SC_LIBRARY",
                                                       "build-supercollider-host/server/scsynth/libscsynth.dylib",
                                                       "libscsynth.dylib"))
        {
            serverLibrary = openLibrary(candidate);
            if (serverLibrary == nullptr)
                continue;

            if (loadSymbol(serverLibrary, worldNew, "World_New")
                && loadSymbol(serverLibrary, worldCleanup, "World_Cleanup")
                && loadSymbol(serverLibrary, worldSendPacket, "World_SendPacket")
                && loadSymbol(serverLibrary, worldRenderHostAudio, "World_RenderHostAudio"))
            {
                return true;
            }

            closeLibrary(serverLibrary);
            serverLibrary = nullptr;
        }

        lastError = "Embedded SuperCollider host-audio library not found";
        status = "EMBEDDED SC MISSING";
        debugLog(lastError);
        return false;
    }

    bool loadLanguageApi()
    {
        for (const auto& candidate : libraryCandidates("GRIDCOLLIDER_SC_LANG_LIBRARY",
                                                       "build-supercollider-host/lang/libweldsclang.dylib",
                                                       "libweldsclang.dylib"))
        {
            languageLibrary = openLibrary(candidate);
            if (languageLibrary == nullptr)
                continue;

            if (loadSymbol(languageLibrary, initialiseLanguage, "WeldSCLang_Initialise")
                && loadSymbol(languageLibrary, compileSynthDef, "WeldSCLang_CompileSynthDef"))
            {
                std::array<char, 4096> error {};
                const auto root = juce::File(GRIDCOLLIDER_ALCHEMY_SC_ROOT).getChildFile("third_party").getChildFile("supercollider");
                if (initialiseLanguage(root.getFullPathName().toRawUTF8(), error.data(), static_cast<int>(error.size())))
                {
                    debugLog("language compiler ready");
                    return true;
                }

                lastError = "Embedded SuperCollider language init failed: " + juce::String(error.data());
                debugLog(lastError);
            }

            closeLibrary(languageLibrary);
            languageLibrary = nullptr;
        }

        if (lastError.isEmpty())
            lastError = "Embedded SuperCollider language compiler not found";

        status = "EMBEDDED SC LANG MISSING";
        debugLog(lastError);
        return false;
    }

    bool loadStarterSynthDefs()
    {
        for (const auto& name : { "kick", "snare", "hat", "bass", "tone", "grain", "drone",
                                  "pluck", "pad", "bell", "fm", "acid", "string",
                                  "noise", "sub", "lead", "perc", "metal", "choir" })
        {
            std::vector<char> bytes;
            if (! compileSynth(name, starterSynthSource(name), bytes))
                return false;

            debugLog("compiled SynthDef " + juce::String(name) + " (" + juce::String(static_cast<int>(bytes.size())) + " bytes)");
            sendPacket(buildOscMessage("/d_recv", { OscArgument::blob(std::move(bytes)) }));
            registerSynthName(name);
        }

        prime();
        return true;
    }

    bool compileSynth(const juce::String& name, const juce::String& source, std::vector<char>& bytes)
    {
        const auto tempFile = juce::File::createTempFile("gridcollider-sc.scsyndef");
        tempFile.deleteFile();

        std::array<char, 8192> error {};
        if (! compileSynthDef(source.toRawUTF8(),
                              tempFile.getFullPathName().toRawUTF8(),
                              name.toRawUTF8(),
                              error.data(),
                              static_cast<int>(error.size())))
        {
            lastError = "SynthDef compile failed: " + name + " " + juce::String(error.data());
            debugLog(lastError);
            tempFile.deleteFile();
            return false;
        }

        juce::MemoryBlock block;
        if (! tempFile.loadFileAsData(block) || block.getSize() == 0)
        {
            lastError = "SynthDef compiler produced no bytes: " + name;
            debugLog(lastError);
            tempFile.deleteFile();
            return false;
        }

        bytes.assign(static_cast<const char*>(block.getData()), static_cast<const char*>(block.getData()) + block.getSize());
        tempFile.deleteFile();
        return true;
    }

    std::vector<char> packetFor(const NoteEvent& event)
    {
        const auto& fields = event.fields;
        const auto synth = synthForTarget(fields);
        const auto nodeId = nextNodeId++;
        const auto duration = secondsForTicks(fields.durationTicks, bpm.load(std::memory_order_relaxed));
        const auto velocity = juce::jlimit(0.0f, 1.0f, fields.velocity);
        const auto level = levelFor(synth);
        const auto out = fields.parameters.count("out") > 0
                             ? juce::jlimit(0, numOutputChannels - 2, fields.parameters.at("out").getIntValue())
                             : masterBus;
        auto pan = panForX(fields.sourceCell.column);

        if (const auto iter = fields.parameters.find("pan"); iter != fields.parameters.end())
            pan = juce::jlimit(-1.0f, 1.0f, iter->second.getFloatValue());

        activeNodeByInstrument[synth] = nodeId;

        std::vector<OscArgument> arguments {
            OscArgument::string(synth),
            OscArgument::integer(nodeId),
            OscArgument::integer(1),
            OscArgument::integer(sourceGroupId),
            OscArgument::string("out"),
            OscArgument::floating(static_cast<float>(out)),
            OscArgument::string("pitch"),
            OscArgument::floating(static_cast<float>(juce::jlimit(0, 127, fields.pitch))),
            OscArgument::string("amp"),
            OscArgument::floating(velocity * 0.45f * level),
            OscArgument::string("sustain"),
            OscArgument::floating(duration),
            OscArgument::string("pan"),
            OscArgument::floating(pan)
        };

        for (const auto& [key, value] : fields.parameters)
        {
            const auto name = juce::String (key).trim();

            if (name.isEmpty() || name == "out" || name == "pan")
                continue;

            arguments.push_back(OscArgument::string(name));
            arguments.push_back(OscArgument::floating(value.getFloatValue()));
        }

        return buildOscMessage("/s_new", arguments);
    }

    std::vector<char> packetFor(const TriggerEvent& event)
    {
        const auto trigger = event.triggerName.toLowerCase();
        const auto synth = synthForName(trigger);
        const auto nodeId = nextNodeId++;
        const auto level = levelFor(synth);
        const auto out = event.fields.parameters.count("out") > 0
                             ? juce::jlimit(0, numOutputChannels - 2, event.fields.parameters.at("out").getIntValue())
                             : masterBus;
        auto pan = panForX(event.fields.sourceCell.column);

        if (const auto iter = event.fields.parameters.find("pan"); iter != event.fields.parameters.end())
            pan = juce::jlimit(-1.0f, 1.0f, iter->second.getFloatValue());

        activeNodeByInstrument[synth] = nodeId;

        return buildOscMessage("/s_new",
                               { OscArgument::string(synth),
                                 OscArgument::integer(nodeId),
                                 OscArgument::integer(1),
                                 OscArgument::integer(sourceGroupId),
                                 OscArgument::string("out"),
                                 OscArgument::floating(static_cast<float>(out)),
                                 OscArgument::string("pitch"),
                                 OscArgument::floating(48.0f),
                                 OscArgument::string("amp"),
                                 OscArgument::floating(0.35f * level),
                                 OscArgument::string("sustain"),
                                 OscArgument::floating(0.18f),
                                 OscArgument::string("pan"),
                                 OscArgument::floating(pan) });
    }

    std::vector<std::vector<char>> packetsFor(const ControlEvent& event)
    {
        std::vector<std::vector<char>> packets;
        const auto parameter = normalisedControlParameter(event.parameterName);
        const auto instrument = synthForTarget(event.fields);
        const auto value = juce::jlimit(0.0f, 1.0f, event.value);

        if (instrument == "master" || parameter == "master")
        {
            masterLevel = value;
            return packets;
        }

        if (parameter == "drive")
        {
            juce::ignoreUnused(value);
            return packets;
        }

        if (parameter == "level")
            instrumentLevels[instrument] = value;

        if (const auto node = activeNodeFor(instrument); node > 0)
        {
            if (parameter == "level")
            {
                packets.push_back(buildOscMessage("/n_set",
                                                  { OscArgument::integer(node),
                                                    OscArgument::string("amp"),
                                                    OscArgument::floating(value * 0.45f) }));
            }
            else if (parameter == "pan")
            {
                packets.push_back(buildOscMessage("/n_set",
                                                  { OscArgument::integer(node),
                                                    OscArgument::string("pan"),
                                                    OscArgument::floating(value * 2.0f - 1.0f) }));
            }
        }

        return packets;
    }

    void registerSynthName(const juce::String& name)
    {
        const auto trimmed = name.trim();

        if (trimmed.isNotEmpty())
            loadedSynthNames[trimmed.toLowerCase()] = trimmed;
    }

    juce::String synthForName(const juce::String& name) const
    {
        const auto target = name.trim().toLowerCase();

        if (const auto iter = loadedSynthNames.find(target); iter != loadedSynthNames.end())
            return iter->second;

        return instrumentFor(target);
    }

    juce::String synthForTarget(const EventFields& fields) const
    {
        const auto target = fields.targetAddress.value_or(fields.instrumentName).trim().toLowerCase();

        if (target == "master" || target == "/master" || target == "gcmaster")
            return "master";

        if (const auto iter = loadedSynthNames.find(target); iter != loadedSynthNames.end())
            return iter->second;

        const auto instrument = fields.instrumentName.trim().toLowerCase();
        if (const auto iter = loadedSynthNames.find(instrument); iter != loadedSynthNames.end())
            return iter->second;

        const auto channelText = target.fromLastOccurrenceOf("/", false, false);
        if (channelText.isNotEmpty())
        {
            bool numeric = true;

            for (auto index = 0; index < channelText.length(); ++index)
            {
                if (! juce::CharacterFunctions::isDigit(channelText[index]))
                {
                    numeric = false;
                    break;
                }
            }

            if (numeric)
            {
                const auto channel = channelText.getIntValue();

                if (channel >= 0 && channel < static_cast<int>(channelPalette.size()))
                {
                    juce::String mappedInstrument;
                    {
                        const juce::ScopedLock lock(channelPaletteLock);
                        mappedInstrument = channelPalette[static_cast<std::size_t>(channel)];
                    }

                    if (mappedInstrument.isNotEmpty())
                        return synthForName(mappedInstrument);
                }
            }
        }

        return synthForName(fields.instrumentName);
    }

public:
    void setChannelInstrument(const int channel, const juce::String& instrumentName)
    {
        if (channel < 0 || channel >= static_cast<int>(channelPalette.size()))
            return;

        const juce::ScopedLock lock(channelPaletteLock);
        channelPalette[static_cast<std::size_t>(channel)] = instrumentName.trim();
    }

    juce::String getChannelInstrument(const int channel) const
    {
        if (channel < 0 || channel >= static_cast<int>(channelPalette.size()))
            return {};

        const juce::ScopedLock lock(channelPaletteLock);
        return channelPalette[static_cast<std::size_t>(channel)];
    }

private:
    float levelFor(const juce::String& instrument)
    {
        if (const auto iter = instrumentLevels.find(instrument); iter != instrumentLevels.end())
            return iter->second;

        const auto level = defaultLevelFor(instrument);
        instrumentLevels[instrument] = level;
        return level;
    }

    std::int32_t activeNodeFor(const juce::String& instrument) const
    {
        if (const auto iter = activeNodeByInstrument.find(instrument); iter != activeNodeByInstrument.end())
            return iter->second;

        return 0;
    }

    void flushQueuedEvents()
    {
        queuedScratch.clear();
        {
            const juce::ScopedLock lock(queueLock);
            queuedScratch.swap(pendingPackets);
        }

        for (auto& packet : queuedScratch)
        {
            sendPacket(packet);
            sentEventCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void sendPacket(std::vector<char> packet)
    {
        if (world != nullptr && worldSendPacket != nullptr && ! packet.empty())
            [[maybe_unused]] const auto sent = worldSendPacket(world, static_cast<int>(packet.size()), packet.data(), nullptr);
    }

    void prime()
    {
        std::vector<float> scratch(static_cast<std::size_t>(hostRenderQuantum * numOutputChannels), 0.0f);
        for (int i = 0; i < 8; ++i)
            [[maybe_unused]] const auto rendered = worldRenderHostAudio(world, nullptr, scratch.data(), hostRenderQuantum, 0, numOutputChannels);
    }

    static juce::String findPluginPath()
    {
        if (const auto* env = std::getenv("GRIDCOLLIDER_SC_PLUGIN_PATH"))
            return env;

        const auto alchemyPlugins = juce::File(GRIDCOLLIDER_ALCHEMY_SC_ROOT)
                                        .getChildFile("build-supercollider-host")
                                        .getChildFile("server")
                                        .getChildFile("plugins");
        return alchemyPlugins.isDirectory() ? alchemyPlugins.getFullPathName() : juce::String {};
    }

    void releaseUnlocked() noexcept
    {
        ready.store(false, std::memory_order_release);

        if (world != nullptr && worldCleanup != nullptr)
            worldCleanup(world, false);

        world = nullptr;
        closeLibrary(serverLibrary);
        serverLibrary = nullptr;
        // The embedded sclang runtime is process-global. Like Alchemy, keep the
        // language dylib resident once loaded rather than unloading it mid-app.
        languageLibrary = nullptr;
        worldNew = nullptr;
        worldCleanup = nullptr;
        worldSendPacket = nullptr;
        worldRenderHostAudio = nullptr;
        compileSynthDef = nullptr;
        initialiseLanguage = nullptr;
        pendingPackets.clear();
        queuedScratch.clear();
        interleavedOutput.clear();
        currentSampleRate = 0.0;
        maxBlockSize = 0;
        numOutputChannels = 0;
        nextNodeId = 10000;
        activeNodeByInstrument.clear();
        instrumentLevels.clear();
        loadedSynthNames.clear();
        masterLevel = 0.9f;
    }

    mutable juce::CriticalSection engineLock;
    mutable juce::CriticalSection synthCompileLock;
    juce::CriticalSection queueLock;
    juce::String lastError;
    juce::String status = "EMBEDDED SC OFF";
    juce::String pluginPath;
    std::vector<float> interleavedOutput;
    std::vector<std::vector<char>> pendingPackets;
    std::vector<std::vector<char>> queuedScratch;
    std::map<juce::String, std::int32_t> activeNodeByInstrument;
    std::map<juce::String, float> instrumentLevels;
    std::map<juce::String, juce::String> loadedSynthNames;
    mutable juce::CriticalSection channelPaletteLock;
    std::array<juce::String, EmbeddedScAudioEngine::channelCount> channelPalette = makeDefaultChannelPalette();
    std::atomic<bool> ready { false };
    std::atomic<double> bpm { 120.0 };
    std::atomic<std::uint64_t> tick { 0 };
    std::atomic<bool> playing { false };
    std::atomic<int> renderFailures { 0 };
    std::atomic<int> queuedEventCount { 0 };
    std::atomic<int> sentEventCount { 0 };
    std::int32_t nextNodeId = 10000;
    float masterLevel = 0.9f;
    double currentSampleRate = 0.0;
    int maxBlockSize = 0;
    int numOutputChannels = 0;
    WorldOptions worldOptions;
    World* world = nullptr;
    void* serverLibrary = nullptr;
    void* languageLibrary = nullptr;
    WorldNewFn worldNew = nullptr;
    WorldCleanupFn worldCleanup = nullptr;
    WorldSendPacketFn worldSendPacket = nullptr;
    WorldRenderHostAudioFn worldRenderHostAudio = nullptr;
    CompileSynthDefFn compileSynthDef = nullptr;
    InitialiseLangFn initialiseLanguage = nullptr;
#else
    bool prepare(double, int, int)
    {
        lastError = "unfolding was built without SuperCollider host-audio headers";
        return false;
    }
    void release() noexcept {}
    void render(juce::AudioBuffer<float>& output) { output.clear(); }
    void renderRaw(juce::AudioBuffer<float>& output) { output.clear(); }
    void renderOffline(juce::AudioBuffer<float>& output) { output.clear(); }
    void enqueue(const std::vector<InternalEvent>&) {}
    void setTransport(double, std::uint64_t, bool) {}
    void setMasterLevel(float) {}
    bool loadSynthDef(const juce::String&, const juce::String&) { return false; }
    void setChannelInstrument(int, const juce::String&) {}
    juce::String getChannelInstrument(int) const { return {}; }
    bool isReady() const noexcept { return false; }
    juce::String getStatusText() const { return "EMBEDDED SC UNBUILT"; }
    juce::String getLastError() const { return lastError; }
    juce::String lastError;
#endif
};

EmbeddedScAudioEngine::EmbeddedScAudioEngine()
    : impl(std::make_unique<Impl>())
{
}

EmbeddedScAudioEngine::~EmbeddedScAudioEngine() = default;

bool EmbeddedScAudioEngine::prepare(const double sampleRate, const int maximumBlockSize, const int outputChannels)
{
    return impl->prepare(sampleRate, maximumBlockSize, outputChannels);
}

void EmbeddedScAudioEngine::release() noexcept
{
    impl->release();
}

void EmbeddedScAudioEngine::render(juce::AudioBuffer<float>& output)
{
    impl->render(output);
}

void EmbeddedScAudioEngine::renderRaw(juce::AudioBuffer<float>& output)
{
    impl->renderRaw(output);
}

void EmbeddedScAudioEngine::renderOffline(juce::AudioBuffer<float>& output)
{
    impl->renderOffline(output);
}

void EmbeddedScAudioEngine::enqueue(const std::vector<InternalEvent>& events)
{
    impl->enqueue(events);
}

void EmbeddedScAudioEngine::setTransport(const double bpm, const std::uint64_t tick, const bool playing)
{
    impl->setTransport(bpm, tick, playing);
}

void EmbeddedScAudioEngine::setMasterLevel(const float level)
{
    impl->setMasterLevel(level);
}

bool EmbeddedScAudioEngine::loadSynthDef(const juce::String& name, const juce::String& source)
{
    return impl->loadSynthDef(name, source);
}

void EmbeddedScAudioEngine::setChannelInstrument(const int channel, const juce::String& instrumentName)
{
    impl->setChannelInstrument(channel, instrumentName);
}

juce::String EmbeddedScAudioEngine::getChannelInstrument(const int channel) const
{
    return impl->getChannelInstrument(channel);
}

juce::StringArray EmbeddedScAudioEngine::getDefaultChannelInstruments()
{
    juce::StringArray names;
    for (const auto& name : makeDefaultChannelPalette())
        names.add(name);

    return names;
}

juce::StringArray EmbeddedScAudioEngine::getDefaultSynthDefNames()
{
    return defaultSynthDefNames();
}

juce::String EmbeddedScAudioEngine::getDefaultSynthDefSource(const juce::String& name)
{
    return starterSynthSource(instrumentFor(name.trim().toLowerCase()));
}

bool EmbeddedScAudioEngine::isReady() const noexcept
{
    return impl->isReady();
}

juce::String EmbeddedScAudioEngine::getStatusText() const
{
    return impl->getStatusText();
}

juce::String EmbeddedScAudioEngine::getLastError() const
{
    return impl->getLastError();
}
}
