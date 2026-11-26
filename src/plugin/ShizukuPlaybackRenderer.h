#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

namespace shizuku
{

class ShizukuDocumentController;

// Plays each ARA playback region from the document controller's in-memory audio: the corrected vocal where
// one has been rendered, otherwise the original source. Nothing is read from the host on the audio thread,
// so real-time playback never has to wait for disk reads. Sources whose sample rate or channel count differ
// from the processing format are converted on the fly (cubic interpolation; channel up/down-mix).
class ShizukuPlaybackRenderer final : public juce::ARAPlaybackRenderer
{
public:
    ShizukuPlaybackRenderer (ARA::PlugIn::DocumentController* documentController, ShizukuDocumentController& owner);

    void prepareToPlay (double sampleRate, int maximumSamplesPerBlock, int numChannels,
                        juce::AudioProcessor::ProcessingPrecision, AlwaysNonRealtime alwaysNonRealtime) override;
    void releaseResources() override;
    bool processBlock (juce::AudioBuffer<float>& buffer, juce::AudioProcessor::Realtime realtime,
                       const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept override;

    using juce::ARAPlaybackRenderer::processBlock;

private:
    ShizukuDocumentController& owner;
    int numChannels = 2;
    double sampleRate = 48000.0;
};

} // namespace shizuku
