#pragma once

#include "PitchCorrection.h"
#include "SongAnalyzer.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace shizuku
{

// Result of analysing the chosen vocal and accompaniment tracks, in host playback time.
struct TrackAnalysisResult
{
    juce::String vocalTrackName;
    juce::String accompanimentTrackName;
    double timelineStartSeconds = 0.0; // playback time of analysis time 0
    double timelineEndSeconds = 0.0;
    SongAnalysis analysis;
};

// A note of the correct melody (reference) as drawn, in timeline seconds.
struct DrawnReferenceNote
{
    double startSeconds = 0.0, endSeconds = 0.0;
    int midi = 0;
};

// Draws the vocal pitch line, the notes (as boxes at the pitch they are corrected to), key regions, the corrected
// pitch line, the correct melody and the host playhead; and lets the user pick and drag notes ("手直し"):
// - drag a note up / down: semitone steps (every selected note moves when the dragged one is selected);
// - click a note: select it; Cmd (Ctrl on Windows) + click: add / remove; Shift + click: everything from the
//   previously clicked note to this one;
// - drag on empty space: select the notes whose middle lies in the dragged range;
// - Cmd / Ctrl + wheel or pinch: zoom at the mouse; wheel: scroll in time.
// The view only reports what the user did (callbacks); the editor turns it into edits and previews.
class PitchLineView final : public juce::Component
{
public:
    PitchLineView();

    void setResult (std::shared_ptr<const TrackAnalysisResult> newResult);
    // Correction to draw on top: corrected pitch line, and per note the target actually used (targets is
    // parallel to curve->notes; reference targets are drawn in a different colour). The selection is kept by time.
    void setCorrection (std::shared_ptr<const CorrectionCurve> newCurve, std::shared_ptr<const std::vector<NoteWithKey>> newTargets);
    void setReferenceNotes (std::vector<DrawnReferenceNote> notes);
    void setShowReference (bool shouldShow);
    bool isShowingReference() const noexcept { return showReference; }
    void setPlayhead (double timelineSeconds, bool visible);
    void setBusyText (const juce::String& text); // e.g. "作り直し中…" (empty: none)
    void setEditable (bool shouldBeEditable) { editable = shouldBeEditable; }

    // Visible time range (timeline seconds).
    void setVisibleRange (double startSeconds, double endSeconds);
    juce::Range<double> getVisibleRange() const noexcept { return { viewStart, viewEnd }; }
    void zoom (double factor, double aroundSeconds); // factor < 1 zooms in
    void showAll();
    juce::Range<double> getTotalRange() const;
    // Scrolls so the playhead stays visible while the host plays (follow mode).
    void keepVisible (double timelineSeconds);

    // Selection (indices into the targets / curve notes).
    const std::vector<size_t>& getSelection() const noexcept { return selection; }
    void setSelection (std::vector<size_t> notes);
    juce::Range<double> getSelectedRange() const noexcept { return selectedRange; } // empty unless chosen by a range drag
    double getClickSeconds() const noexcept { return clickSeconds; } // timeline time of the last click on a note

    // The pitch a note is corrected to (fractional MIDI): its edited target, its sung centre when kept, else
    // the sung centre + the applied shift.
    double currentTargetMidi (size_t note) const;
    // Timeline span of a note.
    juce::Range<double> noteSpan (size_t note) const;
    size_t numNotes() const noexcept { return targets != nullptr ? targets->size() : 0; }

    std::function<void()> onSelectionChanged;
    std::function<void (size_t note)> onNoteClicked;                 // a click on a note (after the selection changed)
    std::function<void (juce::Range<double> range)> onRangeSelected; // a range drag on empty space
    std::function<void (size_t note, int semitones)> onDragStep;     // the dragged note's offset changed (preview)
    std::function<void (std::vector<size_t> notes, int semitones)> onNotesDragged; // drag finished

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scaleFactor) override;

private:
    struct NoteBox
    {
        float x0, x1, y, half;
        double midi;
    };

    double timeForX (float x) const;
    float xForTime (double seconds) const;
    float yForMidi (double midi) const;
    float semitonePixels() const;
    NoteBox boxFor (size_t note) const;
    long noteAt (juce::Point<float> position) const;
    std::vector<size_t> notesInRange (juce::Range<double> range) const;
    void updatePitchRange();
    std::vector<size_t> dragGroup() const;

    std::shared_ptr<const TrackAnalysisResult> result;
    std::shared_ptr<const CorrectionCurve> curve;
    std::shared_ptr<const std::vector<NoteWithKey>> targets;
    std::vector<DrawnReferenceNote> referenceNotes;
    bool showReference = true;
    bool editable = true;
    double playheadSeconds = 0.0;
    bool playheadVisible = false;
    juce::String busyText;

    double viewStart = 0.0, viewEnd = 1.0;
    bool viewChosen = false; // false: show everything when a result arrives
    double lowMidi = 48.0, highMidi = 84.0;

    std::vector<size_t> selection;
    long anchorNote = -1;
    juce::Range<double> selectedRange;
    double clickSeconds = -1.0;

    enum class Gesture
    {
        none,
        noteDrag,
        range
    };
    Gesture gesture = Gesture::none;
    long gestureNote = -1;
    bool gestureMoved = false;
    int dragSemitones = 0;
    double gestureStartSeconds = 0.0;
};

} // namespace shizuku
