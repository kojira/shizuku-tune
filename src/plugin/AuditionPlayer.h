#pragma once

#include "CorrectionModel.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <memory>
#include <vector>

namespace shizuku
{

// Short previews from the editor ("試聴": a clicked note or range, a dragged note at its new pitch), added to the
// plug-in's output whether the host is playing or stopped. A preview stops when the host starts playing.
// Message thread: play / playTone / stop. Audio thread: prepare, process (no allocation, no locks: the clip is
// handed over through an atomic pointer; the message thread frees a clip only when the audio thread has
// announced (hazard pointer) that it no longer uses it).
class AuditionPlayer
{
public:
    AuditionPlayer() = default;
    ~AuditionPlayer();

    static constexpr double maxSeconds = 4.0;

    // Message thread. The audio is resampled to the output rate, limited to maxSeconds and faded in / out.
    void play (const AuditionAudio& audio, float gain = 1.0f);
    // A short tone at a MIDI pitch (sine + a little 2nd harmonic), when no voice preview is ready in time.
    void playTone (double midi, double seconds = 0.35);
    void stop();
    // Frees clips the audio thread no longer uses (called by play / stop and the editor's timer).
    void collectGarbage();

    // Audio thread.
    void prepare (double sampleRate) noexcept { outputRate = sampleRate; }
    // Adds the current clip to buffer. hostPlaying: the host transport plays (a start stops the preview).
    void process (juce::AudioBuffer<float>& buffer, bool hostPlaying) noexcept;

    // For tests / diagnostics.
    bool isSounding() const noexcept { return sounding.load(); }
    int getBlocksWhileStopped() const noexcept { return blocksWhileStopped.load(); }

private:
    struct Clip
    {
        std::vector<std::vector<float>> channels; // at outputRate
    };

    void publish (std::unique_ptr<Clip> clip);

    std::atomic<double> outputRate { 48000.0 };
    std::atomic<const Clip*> pending { nullptr }; // written by the message thread
    std::atomic<const Clip*> inUse { nullptr };   // hazard pointer, written by the audio thread
    std::vector<std::unique_ptr<Clip>> owned;     // message thread

    // Audio thread only.
    const Clip* current = nullptr;
    size_t position = 0;
    bool wasHostPlaying = false;
    bool stoppedByHost = false;

    std::atomic<bool> sounding { false };
    std::atomic<int> blocksWhileStopped { 0 };
};

} // namespace shizuku
