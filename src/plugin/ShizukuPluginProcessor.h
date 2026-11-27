#pragma once

#include "AuditionPlayer.h"
#include "CaptureSession.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace shizuku
{

// Audio Unit processor. Bound to ARA, all audio comes from the ARA playback renderer. Otherwise (e.g. Logic Pro
// running natively, which hosts Audio Units out of process without ARA) the capture session records the input and
// the side chain (accompaniment) while the host plays once, and then plays the corrected vocal.
// Previews from the editor (AuditionPlayer) are added to the output in both modes, also while the host is stopped.
class ShizukuPluginProcessor final : public juce::AudioProcessor,
                                     public juce::AudioProcessorARAExtension,
                                     private juce::Timer
{
public:
    ShizukuPluginProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    // Capture mode: the capture id and the settings (the audio is kept in CaptureSession::getCaptureRoot()).
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    CaptureSession& getCaptureSession() noexcept { return captureSession; }
    AuditionPlayer& getAuditionPlayer() noexcept { return auditionPlayer; }

    // Host transport as seen in the last processBlock (for the editor's playhead line).
    bool isHostPlaying() const noexcept { return hostPlaying.load(); }
    double getHostPositionSeconds() const noexcept { return hostPositionSeconds.load(); }

    juce::AudioProcessorARAExtension* getARAClientExtensions() override { return this; }

protected:
    void didBindToARA() noexcept override;

private:
    // Diagnostics (ara.log): whether the host calls processBlock while its transport is stopped.
    void timerCallback() override;

    CaptureSession captureSession;
    AuditionPlayer auditionPlayer;
    std::atomic<bool> hostPlaying { false };
    std::atomic<double> hostPositionSeconds { 0.0 };
    std::atomic<int> blocksPlaying { 0 }, blocksStopped { 0 };
    int lastBlocksPlaying = 0, lastBlocksStopped = 0;
    bool loggedStoppedCalls = false, loggedNoStoppedCalls = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShizukuPluginProcessor)
};

} // namespace shizuku
