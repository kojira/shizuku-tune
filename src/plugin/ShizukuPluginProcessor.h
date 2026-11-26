#pragma once

#include "CaptureSession.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace shizuku
{

// Audio Unit processor. Bound to ARA, all audio comes from the ARA playback renderer. Otherwise (e.g. Logic Pro
// running natively, which hosts Audio Units out of process without ARA) the capture session records the input and
// the side chain (accompaniment) while the host plays once, and then plays the corrected vocal.
class ShizukuPluginProcessor final : public juce::AudioProcessor,
                                     public juce::AudioProcessorARAExtension
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

    juce::AudioProcessorARAExtension* getARAClientExtensions() override { return this; }

protected:
    void didBindToARA() noexcept override;

private:
    CaptureSession captureSession;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShizukuPluginProcessor)
};

} // namespace shizuku
