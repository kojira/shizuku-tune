#pragma once

#include "NoteEditPanel.h"
#include "PitchLineView.h"
#include "ShizukuDocumentController.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace shizuku
{

class ShizukuPluginProcessor;

// The plug-in window (ARA and capture mode share everything below the mode-specific top row): strength /
// threshold, reference candidates and choice, the pitch view with hand editing (NoteEditPanel), zoom / follow /
// reference display, the host playhead, and previews ("試聴") of clicked and dragged notes through the
// processor's AuditionPlayer.
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
    bool keyPressed (const juce::KeyPress&) override;

    juce::AudioProcessorEditorARAExtension* getARAClientExtensions() override { return this; }

    // For tests and UI snapshots.
    PitchLineView& getPitchLineView() noexcept { return pitchLineView; }
    NoteEditPanel& getNoteEditPanel() noexcept { return noteEditPanel; }

private:
    class PreviewRenderer;

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
    void refreshReferenceNotes();

    // Previews: the corrected audio of a timeline range (at most AuditionPlayer::maxSeconds), and a note at a
    // pitch offset while it is dragged (the note's audio shifted there; a short tone until that is ready).
    void previewRange (double startSeconds, double endSeconds);
    void previewNoteAt (size_t note, int semitones);

    ShizukuPluginProcessor& shizukuProcessor;
    ShizukuDocumentController* documentController = nullptr;
    CorrectionModel* model = nullptr; // documentController or the processor's capture session
    std::vector<juce::ARARegionSequence*> trackChoices; // ComboBox item id = index + 2 (1 = none)

    juce::Label vocalLabel, accompanimentLabel;
    juce::ComboBox vocalChoice, accompanimentChoice;
    juce::TextButton analyseButton;
    juce::Label keyLabel, statusLabel;
    juce::Label strengthLabel, thresholdLabel, referenceLabel;
    juce::Slider strengthSlider, thresholdSlider; // percent / cents; the correction re-renders when released
    juce::TextButton referenceButton, clearReferenceButton;
    juce::Label referenceChoiceLabel;
    juce::ComboBox referenceChoice; // item id = choice + 3 (1 = automatic, 2 = none, 3.. = candidates)
    std::unique_ptr<juce::FileChooser> referenceChooser;
    PitchLineView pitchLineView;
    NoteEditPanel noteEditPanel { pitchLineView };

    // View controls.
    juce::TextButton zoomInButton, zoomOutButton, showAllButton, followButton, showReferenceButton;

    // Capture mode.
    juce::TextButton captureButton, finishButton;
    juce::Label captureRangeLabel;

    std::unique_ptr<PreviewRenderer> previewRenderer;
    bool follow = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShizukuPluginEditor)
};

} // namespace shizuku
