#include "AuditionPlayer.h"

#include "MusicTheory.h"

#include <cmath>

namespace shizuku
{

using namespace juce;

namespace
{
constexpr double fadeSeconds = 0.01;
}

AuditionPlayer::~AuditionPlayer()
{
    pending = nullptr;
}

void AuditionPlayer::publish (std::unique_ptr<Clip> clip)
{
    pending.store (clip.get());
    if (clip != nullptr)
        owned.push_back (std::move (clip));
    collectGarbage();
}

void AuditionPlayer::collectGarbage()
{
    const auto* live = pending.load();
    const auto* used = inUse.load();
    owned.erase (std::remove_if (owned.begin(), owned.end(), [&] (const std::unique_ptr<Clip>& clip) { return clip.get() != live && clip.get() != used; }),
                 owned.end());
}

void AuditionPlayer::play (const AuditionAudio& audio, float gain)
{
    if (audio.channels.empty() || audio.channels[0].empty() || audio.sampleRate <= 0.0)
        return;
    const double rate = outputRate.load();
    const double ratio = audio.sampleRate / rate;
    const auto inputLength = audio.channels[0].size();
    const auto length = (size_t) std::min ((double) inputLength / ratio, maxSeconds * rate);
    auto clip = std::make_unique<Clip>();
    const auto fade = (size_t) (fadeSeconds * rate);
    for (const auto& source : audio.channels)
    {
        std::vector<float> channel (length);
        for (size_t i = 0; i < length; ++i)
        {
            const double at = (double) i * ratio;
            const auto index = (size_t) at;
            const auto fraction = (float) (at - (double) index);
            const float a = source[std::min (index, inputLength - 1)], b = source[std::min (index + 1, inputLength - 1)];
            float value = (a + (b - a) * fraction) * gain;
            if (i < fade)
                value *= (float) i / (float) fade;
            if (length - i < fade)
                value *= (float) (length - i) / (float) fade;
            channel[i] = value;
        }
        clip->channels.push_back (std::move (channel));
    }
    publish (std::move (clip));
}

void AuditionPlayer::playTone (double midi, double seconds)
{
    const double rate = outputRate.load();
    const double frequency = midiToFrequency (midi);
    const auto length = (size_t) (seconds * rate + 0.3 * rate);
    AuditionAudio audio;
    audio.sampleRate = rate;
    std::vector<float> tone (length);
    for (size_t i = 0; i < length; ++i)
    {
        const double t = (double) i / rate;
        const double attack = std::min (1.0, t / 0.015);
        const double release = t < seconds * 0.6 ? 1.0 : std::exp (-(t - seconds * 0.6) / 0.06);
        const double phase = MathConstants<double>::twoPi * frequency * t;
        tone[i] = (float) (0.25 * attack * release * (std::sin (phase) + 0.25 * std::sin (2.0 * phase)));
    }
    audio.channels.push_back (std::move (tone));
    play (audio);
}

void AuditionPlayer::stop()
{
    publish (nullptr);
}

void AuditionPlayer::process (AudioBuffer<float>& buffer, bool hostPlaying) noexcept
{
    if (! hostPlaying)
        ++blocksWhileStopped;
    // Announce the clip before using it, and make sure it is still the published one (else take the newer one).
    const Clip* clip = pending.load();
    for (;;)
    {
        inUse.store (clip);
        const auto* again = pending.load();
        if (again == clip)
            break;
        clip = again;
    }
    if (clip != current)
    {
        current = clip;
        position = 0;
        stoppedByHost = false;
    }
    if (hostPlaying && ! wasHostPlaying)
        stoppedByHost = true; // the host started playing: the preview stops
    wasHostPlaying = hostPlaying;

    if (current == nullptr || stoppedByHost || current->channels.empty() || position >= current->channels[0].size())
    {
        sounding = false;
        return;
    }
    const auto numSamples = (size_t) buffer.getNumSamples();
    const auto count = std::min (numSamples, current->channels[0].size() - position);
    const auto numClipChannels = current->channels.size();
    for (int c = 0; c < buffer.getNumChannels(); ++c)
        FloatVectorOperations::add (buffer.getWritePointer (c), current->channels[(size_t) c % numClipChannels].data() + position, (int) count);
    position += count;
    sounding = true;
}

} // namespace shizuku
