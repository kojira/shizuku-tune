#pragma once

#include "PitchLineView.h"
#include "ShizukuDocumentController.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace shizuku
{

class ShizukuPluginProcessor;

class ShizukuPluginEditor final : public juce::AudioProcessorEditor,
                                  public juce::AudioProcessorEditorARAExtension,
                                  private juce::ChangeListener,
                                  private juce::Timer
{
public:
    explicit ShizukuPluginEditor (ShizukuPluginProcessor&);
    ~ShizukuPluginEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    juce::AudioProcessorEditorARAExtension* getARAClientExtensions() override { return this; }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    // Logic may bind the plug-in to ARA after the editor was created: attach when the ARA editor view appears.
    // Until then (and in hosts without ARA) the editor shows the capture mode.
    bool attachToARA();
    void setModel (CorrectionModel* newModel);
    void showCaptureMode (bool shouldShow);
    void refreshCapture();
    void refreshTrackChoices();
    void refreshResult();
    void applySliderSettings();
    void refreshReferenceChoices();

    ShizukuPluginProcessor& shizukuProcessor;
    ShizukuDocumentController* documentController = nullptr;
    CorrectionModel* model = nullptr; // documentController or the processor's capture session
    std::vector<juce::ARARegionSequence*> trackChoices; // ComboBox item id = index + 2 (1 = none)

    juce::Label vocalLabel { {}, "Vocal track" }, accompanimentLabel { {}, "Accompaniment track" };
    juce::ComboBox vocalChoice, accompanimentChoice;
    juce::TextButton analyseButton { "Analyse" };
    juce::Label keyLabel, statusLabel;
    juce::Label strengthLabel { {}, "Strength" }, thresholdLabel { {}, "Threshold" }, referenceLabel;
    juce::Slider strengthSlider, thresholdSlider; // percent / cents; the correction re-renders when released
    juce::TextButton referenceButton { "Add reference..." }, clearReferenceButton { "Clear" };
    juce::Label referenceChoiceLabel { {}, "Correct melody" };
    juce::ComboBox referenceChoice; // item id = choice + 3 (1 = automatic, 2 = none, 3.. = candidates)
    std::unique_ptr<juce::FileChooser> referenceChooser;
    PitchLineView pitchLineView;

    // Capture mode.
    juce::TextButton captureButton, finishButton;
    juce::Label captureRangeLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShizukuPluginEditor)
};

} // namespace shizuku
