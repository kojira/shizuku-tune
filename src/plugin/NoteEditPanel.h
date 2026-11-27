#pragma once

#include "CorrectionModel.h"
#include "PitchLineView.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace shizuku
{

// The hand-edit panel under the pitch view: for the selected note(s) the sung pitch, the automatic target and
// the current target, a pitch entry ("D5 +3") with +-1 cent, a semitone up / down, keep (no correction), back to
// automatic, split at the clicked time, join with the previous / next note, undo / redo. Every button acts on all
// selected notes (one undo step). Edits go to the model, which re-renders only around the edited notes.
class NoteEditPanel final : public juce::Component,
                            private juce::Timer
{
public:
    explicit NoteEditPanel (PitchLineView& view);

    void setModel (CorrectionModel* newModel);
    // Shows the selection of the view (call when it or the model's notes changed).
    void refresh();

    // Edit operations (also used by the editor's keyboard shortcuts and drag handling).
    void moveSelectedBySemitones (int semitones);
    void moveNotesBySemitones (const std::vector<size_t>& notes, int semitones);
    void nudgeSelectedCents (int cents);       // several quick nudges become one edit (after a short pause)
    void setSelectedTarget (double midi);
    void setSelectedMode (NoteEditMode mode);
    void splitAtClick();
    void joinWithNeighbour (bool previous);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    std::vector<NoteSpan> spansOf (const std::vector<size_t>& notes) const;
    juce::String automaticText (size_t note) const;
    void commitPendingCents();

    PitchLineView& view;
    CorrectionModel* model = nullptr;
    int pendingCents = 0; // +-1 cent nudges not yet applied

    juce::Label infoLabel, pitchLabel;
    juce::TextEditor pitchEntry;
    juce::TextButton applyPitchButton, centDownButton, centUpButton, semitoneDownButton, semitoneUpButton, keepButton, autoButton,
        splitButton, joinPreviousButton, joinNextButton, undoButton, redoButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteEditPanel)
};

} // namespace shizuku
