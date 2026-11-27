#include "PitchLineView.h"

#include "MusicTheory.h"

#include <cmath>

namespace shizuku
{

using namespace juce;

namespace
{
constexpr float dragThresholdPixels = 6.0f;
constexpr double minVisibleSeconds = 0.3;

Colour keyColour (const MusicalKey& key)
{
    // Circle-of-fifths hue so related keys get related colours.
    const int fifths = (key.tonicPitchClass * 7) % 12;
    return Colour::fromHSV ((float) fifths / 12.0f, key.mode == KeyMode::major ? 0.45f : 0.3f, 0.35f, 1.0f);
}

String noteLabel (double midi)
{
    const int note = (int) std::lround (midi);
    const int cents = (int) std::lround ((midi - note) * 100.0);
    return String (midiNoteName (note)) + (cents != 0 ? (cents > 0 ? "+" : "") + String (cents) : String());
}
} // namespace

//==============================================================================
PitchLineView::PitchLineView()
{
    setWantsKeyboardFocus (false);
}

void PitchLineView::setResult (std::shared_ptr<const TrackAnalysisResult> newResult)
{
    const bool changed = newResult != result;
    result = std::move (newResult);
    if (changed && result != nullptr && ! viewChosen)
    {
        const auto total = getTotalRange();
        viewStart = total.getStart();
        viewEnd = total.getEnd();
    }
    updatePitchRange();
    repaint();
}

void PitchLineView::setCorrection (std::shared_ptr<const CorrectionCurve> newCurve, std::shared_ptr<const std::vector<NoteWithKey>> newTargets)
{
    // Keep the selection by time: the notes holding the selected notes' middles (or the click time for one note,
    // so the clicked half of a split stays selected).
    std::vector<double> times;
    if (targets != nullptr)
    {
        if (selection.size() == 1 && clickSeconds >= 0.0 && noteSpan (selection[0]).contains (clickSeconds))
            times.push_back (clickSeconds);
        else
            for (const auto i : selection)
                times.push_back (noteSpan (i).getStart() + 0.5 * noteSpan (i).getLength());
    }
    curve = std::move (newCurve);
    targets = std::move (newTargets);
    std::vector<size_t> kept;
    if (targets != nullptr)
        for (const auto t : times)
            for (size_t i = 0; i < targets->size(); ++i)
            {
                const auto span = noteSpan (i);
                if (t >= span.getStart() - 0.01 && t <= span.getEnd() + 0.01)
                {
                    if (std::find (kept.begin(), kept.end(), i) == kept.end())
                        kept.push_back (i);
                    break;
                }
            }
    const bool selectionChanged = kept != selection;
    selection = std::move (kept);
    if (anchorNote >= (long) numNotes())
        anchorNote = -1;
    updatePitchRange();
    repaint();
    if (selectionChanged && onSelectionChanged)
        onSelectionChanged();
}

void PitchLineView::setReferenceNotes (std::vector<DrawnReferenceNote> notes)
{
    referenceNotes = std::move (notes);
    repaint();
}

void PitchLineView::setShowReference (bool shouldShow)
{
    showReference = shouldShow;
    repaint();
}

void PitchLineView::setPlayhead (double timelineSeconds, bool visible)
{
    if (visible == playheadVisible && (! visible || std::abs (timelineSeconds - playheadSeconds) < 1.0e-4))
        return;
    playheadSeconds = timelineSeconds;
    playheadVisible = visible;
    repaint();
}

void PitchLineView::setBusyText (const String& text)
{
    if (text == busyText)
        return;
    busyText = text;
    repaint();
}

Range<double> PitchLineView::getTotalRange() const
{
    if (result == nullptr)
        return { 0.0, 1.0 };
    return { result->timelineStartSeconds, jmax (result->timelineStartSeconds + minVisibleSeconds, result->timelineEndSeconds) };
}

void PitchLineView::setVisibleRange (double startSeconds, double endSeconds)
{
    const auto total = getTotalRange();
    const double span = jlimit (minVisibleSeconds, jmax (minVisibleSeconds, total.getLength()), endSeconds - startSeconds);
    const double start = jlimit (total.getStart(), jmax (total.getStart(), total.getEnd() - span), startSeconds);
    viewStart = start;
    viewEnd = start + span;
    viewChosen = true;
    repaint();
}

void PitchLineView::zoom (double factor, double aroundSeconds)
{
    const double before = (aroundSeconds - viewStart) * factor, after = (viewEnd - aroundSeconds) * factor;
    setVisibleRange (aroundSeconds - before, aroundSeconds + after);
}

void PitchLineView::showAll()
{
    const auto total = getTotalRange();
    setVisibleRange (total.getStart(), total.getEnd());
}

void PitchLineView::keepVisible (double timelineSeconds)
{
    if (timelineSeconds >= viewStart && timelineSeconds <= viewEnd)
        return;
    const double span = viewEnd - viewStart;
    setVisibleRange (timelineSeconds - 0.1 * span, timelineSeconds + 0.9 * span);
}

void PitchLineView::setSelection (std::vector<size_t> notes)
{
    selection = std::move (notes);
    selectedRange = {};
    repaint();
    if (onSelectionChanged)
        onSelectionChanged();
}

//==============================================================================
double PitchLineView::currentTargetMidi (size_t i) const
{
    if (targets == nullptr || i >= targets->size())
        return 0.0;
    const auto& note = (*targets)[i].note;
    if (curve == nullptr || i >= curve->notes.size())
        return note.centreMidi;
    const auto& decision = curve->notes[i];
    if (decision.edited)
        return decision.corrected ? decision.targetMidi : note.centreMidi;
    return std::round ((note.centreMidi + decision.shiftSemitones) * 100.0) / 100.0;
}

Range<double> PitchLineView::noteSpan (size_t i) const
{
    if (targets == nullptr || i >= targets->size() || result == nullptr)
        return {};
    const auto& note = (*targets)[i].note;
    return { result->timelineStartSeconds + note.startSeconds, result->timelineStartSeconds + note.endSeconds };
}

void PitchLineView::updatePitchRange()
{
    // Pitch range from the notes (fallback: C3..C6).
    lowMidi = 48.0;
    highMidi = 84.0;
    const auto& notes = targets != nullptr ? *targets : result != nullptr ? result->analysis.notes : std::vector<NoteWithKey> {};
    if (notes.empty())
        return;
    lowMidi = 1000.0;
    highMidi = -1000.0;
    for (const auto& entry : notes)
    {
        lowMidi = jmin (lowMidi, entry.note.centreMidi);
        highMidi = jmax (highMidi, entry.note.centreMidi);
    }
    lowMidi = std::floor (lowMidi) - 3.0;
    highMidi = std::ceil (highMidi) + 3.0;
}

float PitchLineView::xForTime (double seconds) const
{
    return (float) ((seconds - viewStart) / jmax (1.0e-6, viewEnd - viewStart) * getWidth());
}

double PitchLineView::timeForX (float x) const
{
    return viewStart + (double) x / jmax (1, getWidth()) * (viewEnd - viewStart);
}

float PitchLineView::yForMidi (double midi) const
{
    return (float) getHeight() - (float) ((midi - lowMidi) / (highMidi - lowMidi)) * (float) getHeight();
}

float PitchLineView::semitonePixels() const
{
    return (float) getHeight() / (float) jmax (1.0, highMidi - lowMidi);
}

std::vector<size_t> PitchLineView::dragGroup() const
{
    if (gesture != Gesture::noteDrag || gestureNote < 0)
        return {};
    if (std::find (selection.begin(), selection.end(), (size_t) gestureNote) != selection.end())
        return selection;
    return { (size_t) gestureNote };
}

PitchLineView::NoteBox PitchLineView::boxFor (size_t i) const
{
    const auto span = noteSpan (i);
    double midi = currentTargetMidi (i);
    if (gesture == Gesture::noteDrag && gestureMoved && dragSemitones != 0)
    {
        const auto group = dragGroup();
        if (std::find (group.begin(), group.end(), i) != group.end())
            midi += dragSemitones;
    }
    const auto x0 = xForTime (span.getStart());
    return { x0, jmax (xForTime (span.getEnd()), x0 + 3.0f), yForMidi (midi), jlimit (4.0f, 10.0f, semitonePixels() * 0.45f), midi };
}

Point<float> PitchLineView::getNoteBoxCentre (size_t note) const
{
    const auto box = boxFor (note);
    return { 0.5f * (box.x0 + box.x1), box.y };
}

long PitchLineView::noteAt (Point<float> position) const
{
    long best = -1;
    float bestDistance = 1.0e9f;
    for (size_t i = 0; i < numNotes(); ++i)
    {
        const auto box = boxFor (i);
        if (position.x < box.x0 - 4.0f || position.x > box.x1 + 4.0f)
            continue;
        const auto distance = std::abs (position.y - box.y);
        if (distance <= box.half + 6.0f && distance < bestDistance)
        {
            bestDistance = distance;
            best = (long) i;
        }
    }
    return best;
}

std::vector<size_t> PitchLineView::notesInRange (Range<double> range) const
{
    std::vector<size_t> notes;
    for (size_t i = 0; i < numNotes(); ++i)
    {
        const auto span = noteSpan (i);
        const double middle = span.getStart() + 0.5 * span.getLength();
        if (middle >= range.getStart() && middle <= range.getEnd())
            notes.push_back (i);
    }
    return notes;
}

//==============================================================================
void PitchLineView::mouseDown (const MouseEvent& event)
{
    gesture = Gesture::none;
    gestureMoved = false;
    dragSemitones = 0;
    if (result == nullptr || ! editable)
        return;
    gestureStartSeconds = timeForX (event.position.x);
    gestureNote = noteAt (event.position);
    gesture = gestureNote >= 0 ? Gesture::noteDrag : Gesture::range;
}

void PitchLineView::mouseDrag (const MouseEvent& event)
{
    if (gesture == Gesture::none)
        return;
    const auto offset = event.position - event.mouseDownPosition;
    if (! gestureMoved && std::abs (gesture == Gesture::noteDrag ? offset.y : offset.x) > dragThresholdPixels)
        gestureMoved = true;
    if (! gestureMoved)
        return;
    if (gesture == Gesture::noteDrag)
    {
        const int semitones = (int) std::lround (-offset.y / semitonePixels());
        if (semitones != dragSemitones)
        {
            dragSemitones = semitones;
            if (onDragStep)
                onDragStep ((size_t) gestureNote, semitones);
        }
        repaint();
    }
    else
    {
        const auto t = timeForX (event.position.x);
        selectedRange = { jmin (gestureStartSeconds, t), jmax (gestureStartSeconds, t) };
        selection = notesInRange (selectedRange);
        repaint();
    }
}

void PitchLineView::mouseUp (const MouseEvent& event)
{
    if (gesture == Gesture::noteDrag && gestureMoved)
        mouseDrag (event); // the semitone offset where the button was released
    const auto finished = gesture;
    gesture = Gesture::none;
    if (finished == Gesture::noteDrag)
    {
        const auto note = (size_t) gestureNote;
        if (gestureMoved)
        {
            gesture = Gesture::noteDrag; // for dragGroup
            const auto group = dragGroup();
            gesture = Gesture::none;
            const int semitones = dragSemitones;
            dragSemitones = 0;
            if (std::find (selection.begin(), selection.end(), note) == selection.end())
            {
                selection = group;
                selectedRange = {};
                if (onSelectionChanged)
                    onSelectionChanged();
            }
            repaint();
            if (semitones != 0 && onNotesDragged)
                onNotesDragged (group, semitones);
            return;
        }
        // A click: select (Cmd / Ctrl: toggle, Shift: range from the anchor).
        const auto mods = event.mods;
        clickSeconds = timeForX (event.position.x);
        if (mods.isCommandDown())
        {
            const auto it = std::find (selection.begin(), selection.end(), note);
            if (it != selection.end())
                selection.erase (it);
            else
                selection.push_back (note);
            std::sort (selection.begin(), selection.end());
        }
        else if (mods.isShiftDown() && anchorNote >= 0)
        {
            selection.clear();
            for (auto i = (size_t) jmin<long> (anchorNote, (long) note); i <= (size_t) jmax<long> (anchorNote, (long) note); ++i)
                selection.push_back (i);
        }
        else
            selection = { note };
        if (! mods.isShiftDown())
            anchorNote = (long) note;
        selectedRange = {};
        repaint();
        if (onSelectionChanged)
            onSelectionChanged();
        if (onNoteClicked)
            onNoteClicked (note);
        return;
    }
    if (finished == Gesture::range)
    {
        if (! gestureMoved)
        {
            // A click on empty space clears the selection.
            selectedRange = {};
            if (! selection.empty())
            {
                selection.clear();
                if (onSelectionChanged)
                    onSelectionChanged();
            }
            repaint();
            return;
        }
        anchorNote = selection.empty() ? -1 : (long) selection.front();
        repaint();
        if (onSelectionChanged)
            onSelectionChanged();
        if (onRangeSelected)
            onRangeSelected (selectedRange);
    }
}

void PitchLineView::mouseWheelMove (const MouseEvent& event, const MouseWheelDetails& wheel)
{
    if (result == nullptr)
        return;
    if (event.mods.isCommandDown() || event.mods.isCtrlDown() || event.mods.isAltDown())
    {
        zoom (std::exp (-wheel.deltaY * 2.0), timeForX (event.position.x));
        return;
    }
    const double delta = (std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? -wheel.deltaX : -wheel.deltaY) * 0.5 * (viewEnd - viewStart);
    setVisibleRange (viewStart + delta, viewEnd + delta);
}

void PitchLineView::mouseMagnify (const MouseEvent& event, float scaleFactor)
{
    if (result != nullptr && scaleFactor > 0.0f)
        zoom (1.0 / scaleFactor, timeForX (event.position.x));
}

//==============================================================================
void PitchLineView::paint (Graphics& g)
{
    g.fillAll (Colour (0xff15181d));
    auto bounds = getLocalBounds().toFloat();

    if (result == nullptr)
    {
        g.setColour (Colours::grey);
        g.drawFittedText (String::fromUTF8 (u8"まだ解析していません。"), getLocalBounds(), Justification::centred, 1);
        return;
    }

    const auto& analysis = result->analysis;
    const double origin = result->timelineStartSeconds; // analysis time 0
    const auto xForAnalysis = [&] (double seconds) { return xForTime (origin + seconds); };

    // Key regions (background bands) with names.
    if (analysis.hasAccompaniment)
    {
        for (const auto& segment : analysis.key.segments)
        {
            const auto x0 = xForAnalysis (segment.startSeconds), x1 = xForAnalysis (segment.endSeconds);
            if (x1 < 0.0f || x0 > bounds.getRight())
                continue;
            g.setColour (keyColour (segment.key).withAlpha (0.35f));
            g.fillRect (x0, bounds.getY(), x1 - x0, bounds.getHeight());
            g.setColour (Colours::white.withAlpha (0.8f));
            g.setFont (FontOptions (12.0f));
            g.drawText (keyName (segment.key), Rectangle<float> (jmax (0.0f, x0) + 4.0f, bounds.getY() + 2.0f, jmax (10.0f, x1 - jmax (0.0f, x0) - 8.0f), 16.0f),
                        Justification::centredLeft, true);
        }
    }

    // Semitone grid; scale notes of the global key slightly brighter, note names on C.
    for (int midi = (int) lowMidi; midi <= (int) highMidi; ++midi)
    {
        const auto y = yForMidi (midi);
        const bool inScale = analysis.hasAccompaniment && isScaleDegree (analysis.key.globalKey, midi % 12);
        g.setColour (Colours::white.withAlpha (inScale ? 0.12f : 0.04f));
        g.drawHorizontalLine ((int) y, bounds.getX(), bounds.getRight());
        if (midi % 12 == 0)
        {
            g.setColour (Colours::white.withAlpha (0.5f));
            g.setFont (FontOptions (11.0f));
            g.drawText (midiNoteName (midi), Rectangle<float> (bounds.getX() + 2.0f, y - 14.0f, 40.0f, 14.0f), Justification::bottomLeft);
        }
    }

    // Selected range.
    if (! selectedRange.isEmpty())
    {
        const auto x0 = xForTime (selectedRange.getStart()), x1 = xForTime (selectedRange.getEnd());
        g.setColour (Colour (0x30ffd24d));
        g.fillRect (x0, bounds.getY(), x1 - x0, bounds.getHeight());
        g.setColour (Colour (0x90ffd24d));
        g.drawVerticalLine ((int) x0, bounds.getY(), bounds.getBottom());
        g.drawVerticalLine ((int) x1, bounds.getY(), bounds.getBottom());
    }

    // Correct melody (reference notes), thin violet bars.
    if (showReference)
    {
        g.setColour (Colour (0x80b07cff));
        for (const auto& note : referenceNotes)
        {
            const auto x0 = xForTime (note.startSeconds), x1 = xForTime (note.endSeconds);
            if (x1 < 0.0f || x0 > bounds.getRight())
                continue;
            const auto y = yForMidi (note.midi);
            g.fillRect (Rectangle<float> (x0, y - 1.5f, jmax (1.0f, x1 - x0), 3.0f));
        }
    }

    // Notes: a box at the pitch the note is corrected to (yellow: edited by hand, dashed outline: kept), with the
    // target marker (orange: key scale, green: reference, cyan: reference farther than 2.5 semitones) and a red
    // outline where the reference was too far / too unsure. Selected notes have a white outline.
    const bool useTargets = targets != nullptr;
    const auto& drawnNotes = useTargets ? *targets : analysis.notes;
    for (size_t i = 0; i < drawnNotes.size(); ++i)
    {
        const auto& entry = drawnNotes[i];
        const auto span = Range<double> (origin + entry.note.startSeconds, origin + entry.note.endSeconds);
        if (span.getEnd() < viewStart || span.getStart() > viewEnd)
            continue;
        const auto box = useTargets ? boxFor (i) : NoteBox { xForTime (span.getStart()), xForTime (span.getEnd()), yForMidi (entry.note.centreMidi), 5.0f, entry.note.centreMidi };
        const bool edited = useTargets && curve != nullptr && i < curve->notes.size() && curve->notes[i].edited;
        const bool kept = edited && ! curve->notes[i].corrected;
        const bool selected = std::find (selection.begin(), selection.end(), i) != selection.end();
        const Rectangle<float> rect (box.x0, box.y - box.half, jmax (2.0f, box.x1 - box.x0), 2.0f * box.half);
        const auto targetColour = entry.largeReferenceShift ? Colour (0xff4de0ff) : entry.hasReferenceTarget ? Colour (0xff6ee07a) : Colour (0xffffc04d);
        const auto fill = edited ? Colour (0xffffe9a8) : Colour (0xff39a0ff);
        g.setColour (fill.withAlpha (edited ? 0.55f : 0.33f));
        g.fillRoundedRectangle (rect, 3.0f);
        if (kept)
        {
            Path outline;
            outline.addRoundedRectangle (rect, 3.0f);
            Path dashed;
            const float dashes[] = { 3.0f, 3.0f };
            PathStrokeType (1.2f).createDashedStroke (dashed, outline, dashes, 2);
            g.setColour (Colour (0xffffe9a8));
            g.fillPath (dashed);
        }
        if (selected)
        {
            g.setColour (Colours::white);
            g.drawRoundedRectangle (rect, 3.0f, 2.0f);
        }
        else if (entry.referenceUnsure)
        {
            g.setColour (Colour (0xffff4d4d));
            g.drawRoundedRectangle (rect, 3.0f, 1.5f);
        }
        if (! edited)
        {
            g.setColour (targetColour.withAlpha (0.8f));
            g.drawHorizontalLine ((int) yForMidi (entry.nearestScaleMidi), box.x0, box.x1);
        }
        if (useTargets && (box.x1 - box.x0 > 34.0f || selected))
        {
            g.setColour (Colours::white.withAlpha (0.9f));
            g.setFont (FontOptions (10.0f));
            g.drawText (noteLabel (box.midi) + (kept ? String::fromUTF8 (u8" 原音") : String()),
                        Rectangle<float> (box.x0 + 2.0f, box.y - box.half - 12.0f, 80.0f, 12.0f), Justification::bottomLeft, false);
        }
    }

    // Phrases whose reference was used shifted by whole beats (call and response): a label above the first note.
    if (useTargets)
    {
        g.setFont (FontOptions (11.0f));
        int lastShift = 0;
        double lastEnd = -1.0e9;
        for (const auto& entry : *targets)
        {
            const int shift = entry.hasReferenceTarget ? entry.referenceShiftBeats : 0;
            if (shift != 0 && (shift != lastShift || entry.note.startSeconds - lastEnd > 0.3))
            {
                const auto x = xForAnalysis (entry.note.startSeconds), y = yForMidi (entry.note.centreMidi) - 24.0f;
                g.setColour (Colour (0xffd9a6ff));
                g.drawText ((shift > 0 ? "+" : "") + String (shift) + String::fromUTF8 (u8" 拍"), Rectangle<float> (x, y, 60.0f, 14.0f),
                            Justification::centredLeft);
            }
            if (shift != 0)
                lastEnd = entry.note.endSeconds;
            lastShift = shift;
        }
    }

    // Pitch lines: sung (blue) and corrected (pink, original pitch + shift curve).
    if (analysis.hasVocal)
    {
        const auto strokeLine = [&] (const std::function<double (size_t)>& midiAt)
        {
            Path line;
            bool drawing = false;
            for (size_t i = 0; i < analysis.pitch.frames.size(); ++i)
            {
                const auto& frame = analysis.pitch.frames[i];
                const double t = origin + frame.timeSeconds;
                const double midi = midiAt (i);
                if (midi <= 0.0 || t < viewStart - 0.05 || t > viewEnd + 0.05)
                {
                    drawing = false;
                    continue;
                }
                const Point<float> point (xForTime (t), yForMidi (midi));
                if (drawing)
                    line.lineTo (point);
                else
                    line.startNewSubPath (point);
                drawing = true;
            }
            return line;
        };
        g.setColour (Colour (0xff7fd6ff).withAlpha (curve != nullptr ? 0.55f : 1.0f));
        g.strokePath (strokeLine ([&] (size_t i)
                                  { const auto& f = analysis.pitch.frames[i]; return f.voiced ? frequencyToMidi (f.frequencyHz) : 0.0; }),
                      PathStrokeType (1.5f));
        if (curve != nullptr && ! curve->points.empty())
        {
            const auto corrected = correctedPitchMidi (analysis.pitch, *curve);
            g.setColour (Colour (0xffff6fb0));
            g.strokePath (strokeLine ([&] (size_t i) { return corrected[i]; }), PathStrokeType (1.5f));
        }
    }

    // The dragged notes' offset.
    if (gesture == Gesture::noteDrag && gestureMoved && dragSemitones != 0 && gestureNote >= 0)
    {
        const auto box = boxFor ((size_t) gestureNote);
        g.setColour (Colour (0xffffd24d));
        g.setFont (FontOptions (13.0f, Font::bold));
        g.drawText ((dragSemitones > 0 ? "+" : "") + String (dragSemitones) + String::fromUTF8 (u8" 半音"),
                    Rectangle<float> (box.x1 + 4.0f, box.y - 8.0f, 80.0f, 16.0f), Justification::centredLeft, false);
    }

    // Host playhead.
    if (playheadVisible && playheadSeconds >= viewStart && playheadSeconds <= viewEnd)
    {
        g.setColour (Colours::white.withAlpha (0.85f));
        const auto x = xForTime (playheadSeconds);
        g.fillRect (Rectangle<float> (x - 0.75f, bounds.getY(), 1.5f, bounds.getHeight()));
    }

    // Legend.
    g.setFont (FontOptions (12.0f));
    float legendWidth = 0.0f;
    const auto widthOf = [] (const char* text) { return 12.0f * (float) String::fromUTF8 (text).length() + 10.0f; };
    const std::pair<Colour, const char*> items[] = { { Colour (0xff7fd6ff), u8"歌った音程" }, { Colour (0xffff6fb0), u8"補正後" },
                                                     { Colour (0xffffc04d), u8"キーの目標" }, { Colour (0xff6ee07a), u8"正解の目標" },
                                                     { Colour (0xffffe9a8), u8"手直し" }, { Colour (0xffb07cff), u8"正解のメロディー" },
                                                     { Colour (0xffff4d4d), u8"正解が不確か" } };
    for (const auto& item : items)
        legendWidth += widthOf (item.second);
    auto legend = Rectangle<float> (bounds.getRight() - legendWidth - 4.0f, bounds.getBottom() - 18.0f, legendWidth, 16.0f);
    for (const auto& [colour, text] : items)
    {
        g.setColour (colour);
        g.drawText (String::fromUTF8 (text), legend.removeFromLeft (widthOf (text)), Justification::centredLeft);
    }

    if (busyText.isNotEmpty())
    {
        const Rectangle<float> badge (bounds.getRight() - 170.0f, bounds.getY() + 4.0f, 164.0f, 20.0f);
        g.setColour (Colour (0xd0302820));
        g.fillRoundedRectangle (badge, 4.0f);
        g.setColour (Colour (0xffffd24d));
        g.drawText (busyText, badge, Justification::centred);
    }
}

} // namespace shizuku
