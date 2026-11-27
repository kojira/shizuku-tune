#include "NoteEditPanel.h"

#include "MusicTheory.h"

#include <cmath>

namespace shizuku
{

using namespace juce;

namespace
{
constexpr int centNudgeDelayMs = 700;
constexpr double maxJoinGapSeconds = 0.3;

String formatSeconds (double seconds)
{
    const int minutes = (int) (seconds / 60.0);
    return String (minutes) + ":" + String (seconds - minutes * 60.0, 2).paddedLeft ('0', 5);
}
} // namespace

NoteEditPanel::NoteEditPanel (PitchLineView& viewIn) : view (viewIn)
{
    for (auto* component : std::initializer_list<Component*> { &infoLabel, &pitchLabel, &pitchEntry, &applyPitchButton, &centDownButton, &centUpButton,
                                                                 &semitoneDownButton, &semitoneUpButton, &keepButton, &autoButton, &splitButton,
                                                                 &joinPreviousButton, &joinNextButton, &undoButton, &redoButton })
        addAndMakeVisible (component);

    infoLabel.setJustificationType (Justification::topLeft);
    infoLabel.setColour (Label::textColourId, Colours::white.withAlpha (0.9f));
    pitchLabel.setText (String::fromUTF8 (u8"高さ"), dontSendNotification);
    pitchEntry.setTextToShowWhenEmpty ("D5 +3", Colours::grey);
    pitchEntry.setSelectAllWhenFocused (true);
    pitchEntry.onReturnKey = [this]
    {
        double midi = 0.0;
        if (parsePitchText (pitchEntry.getText(), midi))
            setSelectedTarget (midi);
        else
            refresh(); // unreadable: back to the current value
    };
    applyPitchButton.setButtonText (String::fromUTF8 (u8"この高さにする"));
    applyPitchButton.onClick = pitchEntry.onReturnKey;
    centDownButton.setButtonText (String::fromUTF8 (u8"−1 セント"));
    centUpButton.setButtonText (String::fromUTF8 (u8"+1 セント"));
    centDownButton.onClick = [this] { nudgeSelectedCents (-1); };
    centUpButton.onClick = [this] { nudgeSelectedCents (1); };
    semitoneDownButton.setButtonText (String::fromUTF8 (u8"半音下"));
    semitoneUpButton.setButtonText (String::fromUTF8 (u8"半音上"));
    semitoneDownButton.onClick = [this] { moveSelectedBySemitones (-1); };
    semitoneUpButton.onClick = [this] { moveSelectedBySemitones (1); };
    keepButton.setButtonText (String::fromUTF8 (u8"補正しない"));
    keepButton.onClick = [this] { setSelectedMode (NoteEditMode::keep); };
    autoButton.setButtonText (String::fromUTF8 (u8"自動に戻す"));
    autoButton.onClick = [this] { setSelectedMode (NoteEditMode::automatic); };
    splitButton.setButtonText (String::fromUTF8 (u8"ここで分ける"));
    splitButton.onClick = [this] { splitAtClick(); };
    joinPreviousButton.setButtonText (String::fromUTF8 (u8"前とつなげる"));
    joinPreviousButton.onClick = [this] { joinWithNeighbour (true); };
    joinNextButton.setButtonText (String::fromUTF8 (u8"後ろとつなげる"));
    joinNextButton.onClick = [this] { joinWithNeighbour (false); };
    undoButton.setButtonText (String::fromUTF8 (u8"取り消し"));
    redoButton.setButtonText (String::fromUTF8 (u8"やり直し"));
    undoButton.onClick = [this]
    {
        if (model != nullptr)
            model->undoNoteEdit();
    };
    redoButton.onClick = [this]
    {
        if (model != nullptr)
            model->redoNoteEdit();
    };
    refresh();
}

void NoteEditPanel::setModel (CorrectionModel* newModel)
{
    commitPendingCents();
    model = newModel;
    refresh();
}

std::vector<NoteSpan> NoteEditPanel::spansOf (const std::vector<size_t>& notes) const
{
    std::vector<NoteSpan> spans;
    for (const auto i : notes)
    {
        const auto span = view.noteSpan (i);
        spans.push_back ({ span.getStart(), span.getEnd() });
    }
    return spans;
}

String NoteEditPanel::automaticText (size_t note) const
{
    // The automatic decision for this note: the automatic note overlapping it most.
    const auto curve = model != nullptr ? model->getAutomaticCorrection() : nullptr;
    const auto notes = model != nullptr ? model->getAutomaticTargets() : nullptr;
    const auto result = model != nullptr ? model->getLatestResult() : nullptr;
    if (curve == nullptr || notes == nullptr || result == nullptr)
        return "?";
    const auto span = view.noteSpan (note);
    long best = -1;
    double bestOverlap = 0.0;
    for (size_t k = 0; k < notes->size() && k < curve->notes.size(); ++k)
    {
        const auto& other = (*notes)[k].note;
        const double overlap = jmin (span.getEnd(), result->timelineStartSeconds + other.endSeconds)
                               - jmax (span.getStart(), result->timelineStartSeconds + other.startSeconds);
        if (overlap > bestOverlap)
        {
            bestOverlap = overlap;
            best = (long) k;
        }
    }
    if (best < 0)
        return "?";
    const auto& decision = curve->notes[(size_t) best];
    const auto& sung = (*notes)[(size_t) best].note;
    if (! decision.corrected || std::abs (decision.shiftSemitones) < 0.0005)
        return String::fromUTF8 (u8"補正しない（") + String (midiNoteName ((int) std::lround (decision.targetMidi))) + String::fromUTF8 (u8" から ")
               + String (std::abs (decision.deviationCents), 0) + String::fromUTF8 (u8" セント）");
    return pitchText (sung.centreMidi + decision.shiftSemitones) + String::fromUTF8 (u8"（") + String (midiNoteName ((int) std::lround (decision.targetMidi)))
           + String::fromUTF8 (u8"、") + (decision.shiftSemitones > 0 ? "+" : "") + String (decision.shiftSemitones * 100.0, 0) + String::fromUTF8 (u8" セント動かす）");
}

void NoteEditPanel::refresh()
{
    const auto& selection = view.getSelection();
    const bool any = model != nullptr && ! selection.empty();
    const bool single = any && selection.size() == 1;
    const auto curve = model != nullptr ? model->getLatestCorrection() : nullptr;
    const auto targets = model != nullptr ? model->getLatestTargets() : nullptr;

    if (! any)
    {
        infoLabel.setText (String::fromUTF8 (u8"音をクリックすると手直しできます（上下にドラッグで半音ずつ、Cmd/Ctrl+クリックで追加、Shift+クリックで範囲、"
                                             u8"空いている所をドラッグでまとめて選択）。"),
                           dontSendNotification);
        if (! pitchEntry.hasKeyboardFocus (false))
            pitchEntry.clear();
    }
    else if (single && targets != nullptr && selection[0] < targets->size())
    {
        const auto i = selection[0];
        const auto& note = (*targets)[i].note;
        const auto span = view.noteSpan (i);
        const bool edited = curve != nullptr && i < curve->notes.size() && curve->notes[i].edited;
        const bool kept = edited && ! curve->notes[i].corrected;
        const auto now = kept ? String::fromUTF8 (u8"補正しない（手直し）")
                              : pitchText (view.currentTargetMidi (i)) + (edited ? String::fromUTF8 (u8"（手直し）") : String::fromUTF8 (u8"（自動）"));
        infoLabel.setText (String::fromUTF8 (u8"この音 ") + formatSeconds (span.getStart()) + String::fromUTF8 (u8"〜") + formatSeconds (span.getEnd())
                               + String::fromUTF8 (u8"\n元の高さ: ") + pitchText (note.centreMidi) + String::fromUTF8 (u8"\n自動の目標: ") + automaticText (i)
                               + String::fromUTF8 (u8"\n今の目標: ") + now,
                           dontSendNotification);
        if (! pitchEntry.hasKeyboardFocus (false))
            pitchEntry.setText (pitchText (view.currentTargetMidi (i) + pendingCents / 100.0), false);
    }
    else
    {
        size_t edited = 0;
        for (const auto i : selection)
            edited += curve != nullptr && i < curve->notes.size() && curve->notes[i].edited ? 1 : 0;
        const auto first = view.noteSpan (selection.front()), last = view.noteSpan (selection.back());
        infoLabel.setText (String ((int) selection.size()) + String::fromUTF8 (u8" 音を選択（") + formatSeconds (first.getStart())
                               + String::fromUTF8 (u8"〜") + formatSeconds (last.getEnd()) + String::fromUTF8 (u8"、手直し済み ") + String ((int) edited)
                               + String::fromUTF8 (u8"）。どれかを上下にドラッグすると全部が動きます。"),
                           dontSendNotification);
        if (! pitchEntry.hasKeyboardFocus (false))
            pitchEntry.clear();
    }

    for (auto* button : { &applyPitchButton, &centDownButton, &centUpButton, &semitoneDownButton, &semitoneUpButton, &keepButton })
        button->setEnabled (any);
    pitchEntry.setEnabled (any);
    bool anyEdited = false;
    if (any && curve != nullptr)
        for (const auto i : selection)
            anyEdited = anyEdited || (i < curve->notes.size() && curve->notes[i].edited);
    autoButton.setEnabled (anyEdited);

    const auto click = view.getClickSeconds();
    const auto span = single ? view.noteSpan (selection[0]) : Range<double>();
    const bool canSplit = single && click > span.getStart() + 0.03 && click < span.getEnd() - 0.03;
    splitButton.setEnabled (canSplit);
    splitButton.setButtonText (canSplit ? String::fromUTF8 (u8"ここで分ける（") + String (click, 2) + " s)" : String::fromUTF8 (u8"ここで分ける"));
    const auto i = single ? (long) selection[0] : -1L;
    joinPreviousButton.setEnabled (single && i > 0 && span.getStart() - view.noteSpan ((size_t) i - 1).getEnd() < maxJoinGapSeconds);
    joinNextButton.setEnabled (single && i + 1 < (long) view.numNotes() && view.noteSpan ((size_t) i + 1).getStart() - span.getEnd() < maxJoinGapSeconds);
    undoButton.setEnabled (model != nullptr && model->canUndoNoteEdit());
    redoButton.setEnabled (model != nullptr && model->canRedoNoteEdit());
}

//==============================================================================
void NoteEditPanel::moveNotesBySemitones (const std::vector<size_t>& notes, int semitones)
{
    if (model == nullptr || notes.empty() || semitones == 0)
        return;
    commitPendingCents();
    std::vector<double> midi;
    for (const auto i : notes)
        midi.push_back (view.currentTargetMidi (i) + semitones);
    model->applyNoteEdits (withNoteTargets (model->getNoteEdits(), spansOf (notes), midi));
}

void NoteEditPanel::moveSelectedBySemitones (int semitones)
{
    moveNotesBySemitones (view.getSelection(), semitones);
}

void NoteEditPanel::nudgeSelectedCents (int cents)
{
    if (model == nullptr || view.getSelection().empty())
        return;
    pendingCents += cents;
    if (view.getSelection().size() == 1)
        pitchEntry.setText (pitchText (view.currentTargetMidi (view.getSelection()[0]) + pendingCents / 100.0), false);
    startTimer (centNudgeDelayMs);
}

void NoteEditPanel::timerCallback()
{
    commitPendingCents();
}

void NoteEditPanel::commitPendingCents()
{
    stopTimer();
    if (pendingCents == 0 || model == nullptr)
    {
        pendingCents = 0;
        return;
    }
    const auto cents = pendingCents;
    pendingCents = 0;
    const auto& notes = view.getSelection();
    std::vector<double> midi;
    for (const auto i : notes)
        midi.push_back (view.currentTargetMidi (i) + cents / 100.0);
    model->applyNoteEdits (withNoteTargets (model->getNoteEdits(), spansOf (notes), midi));
}

void NoteEditPanel::setSelectedTarget (double midi)
{
    if (model == nullptr || view.getSelection().empty())
        return;
    stopTimer();
    pendingCents = 0;
    const auto& notes = view.getSelection();
    model->applyNoteEdits (withNoteTargets (model->getNoteEdits(), spansOf (notes), std::vector<double> (notes.size(), midi)));
}

void NoteEditPanel::setSelectedMode (NoteEditMode mode)
{
    if (model == nullptr || view.getSelection().empty())
        return;
    commitPendingCents();
    model->applyNoteEdits (withNoteMode (model->getNoteEdits(), spansOf (view.getSelection()), mode));
}

void NoteEditPanel::splitAtClick()
{
    if (model == nullptr || view.getSelection().size() != 1)
        return;
    commitPendingCents();
    model->applyNoteEdits (withSplit (model->getNoteEdits(), view.getClickSeconds()));
}

void NoteEditPanel::joinWithNeighbour (bool previous)
{
    if (model == nullptr || view.getSelection().size() != 1)
        return;
    commitPendingCents();
    const auto i = view.getSelection()[0];
    if ((previous && i == 0) || (! previous && i + 1 >= view.numNotes()))
        return;
    const auto other = previous ? i - 1 : i + 1;
    const auto a = view.noteSpan (jmin (i, other)), b = view.noteSpan (jmax (i, other));
    model->applyNoteEdits (withMerge (model->getNoteEdits(), { a.getStart(), a.getEnd() }, { b.getStart(), b.getEnd() }));
}

//==============================================================================
void NoteEditPanel::paint (Graphics& g)
{
    g.setColour (Colour (0xff262a32));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
    g.setColour (Colour (0xffffd24d).withAlpha (0.7f));
    g.drawHorizontalLine (0, 0.0f, (float) getWidth());
}

void NoteEditPanel::resized()
{
    auto area = getLocalBounds().reduced (6, 4);
    auto right = area.removeFromRight (jmin (640, area.getWidth() * 2 / 3));
    infoLabel.setBounds (area);
    const int rowHeight = 24;
    auto row1 = right.removeFromTop (rowHeight);
    pitchLabel.setBounds (row1.removeFromLeft (36));
    pitchEntry.setBounds (row1.removeFromLeft (90));
    row1.removeFromLeft (4);
    applyPitchButton.setBounds (row1.removeFromLeft (110));
    row1.removeFromLeft (4);
    centDownButton.setBounds (row1.removeFromLeft (76));
    row1.removeFromLeft (2);
    centUpButton.setBounds (row1.removeFromLeft (76));
    row1.removeFromLeft (8);
    semitoneDownButton.setBounds (row1.removeFromLeft (64));
    row1.removeFromLeft (2);
    semitoneUpButton.setBounds (row1.removeFromLeft (64));
    right.removeFromTop (4);
    auto row2 = right.removeFromTop (rowHeight);
    keepButton.setBounds (row2.removeFromLeft (84));
    row2.removeFromLeft (2);
    autoButton.setBounds (row2.removeFromLeft (84));
    row2.removeFromLeft (8);
    splitButton.setBounds (row2.removeFromLeft (150));
    row2.removeFromLeft (2);
    joinPreviousButton.setBounds (row2.removeFromLeft (92));
    row2.removeFromLeft (2);
    joinNextButton.setBounds (row2.removeFromLeft (100));
    right.removeFromTop (4);
    auto row3 = right.removeFromTop (rowHeight);
    undoButton.setBounds (row3.removeFromLeft (84));
    row3.removeFromLeft (2);
    redoButton.setBounds (row3.removeFromLeft (84));
}

} // namespace shizuku
