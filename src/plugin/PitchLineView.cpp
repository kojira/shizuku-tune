#include "PitchLineView.h"

namespace shizuku
{

using namespace juce;

namespace
{
Colour keyColour (const MusicalKey& key)
{
    // Circle-of-fifths hue so related keys get related colours.
    const int fifths = (key.tonicPitchClass * 7) % 12;
    return Colour::fromHSV ((float) fifths / 12.0f, key.mode == KeyMode::major ? 0.45f : 0.3f, 0.35f, 1.0f);
}
} // namespace

//==============================================================================
void PitchLineView::setResult (std::shared_ptr<const TrackAnalysisResult> newResult)
{
    result = std::move (newResult);
    repaint();
}

void PitchLineView::setCorrection (std::shared_ptr<const CorrectionCurve> newCurve, std::shared_ptr<const std::vector<NoteWithKey>> newTargets)
{
    curve = std::move (newCurve);
    targets = std::move (newTargets);
    repaint();
}

void PitchLineView::paint (Graphics& g)
{
    g.fillAll (Colour (0xff15181d));
    auto bounds = getLocalBounds().toFloat();

    if (result == nullptr)
    {
        g.setColour (Colours::grey);
        g.drawFittedText ("No analysis yet.", getLocalBounds(), Justification::centred, 1);
        return;
    }

    const auto& analysis = result->analysis;
    const double duration = jmax (0.001, result->timelineEndSeconds - result->timelineStartSeconds);

    // Pitch range from the notes (fallback: C3..C6).
    double lowMidi = 48.0, highMidi = 84.0;
    if (! analysis.notes.empty())
    {
        lowMidi = 1000.0;
        highMidi = -1000.0;
        for (const auto& entry : analysis.notes)
        {
            lowMidi = jmin (lowMidi, entry.note.centreMidi);
            highMidi = jmax (highMidi, entry.note.centreMidi);
        }
        lowMidi = std::floor (lowMidi) - 3.0;
        highMidi = std::ceil (highMidi) + 3.0;
    }

    const auto xForTime = [&] (double seconds) { return bounds.getX() + (float) (seconds / duration) * bounds.getWidth(); };
    const auto yForMidi = [&] (double midi)
    { return bounds.getBottom() - (float) ((midi - lowMidi) / (highMidi - lowMidi)) * bounds.getHeight(); };

    // Key regions (background bands) with names.
    if (analysis.hasAccompaniment)
    {
        for (const auto& segment : analysis.key.segments)
        {
            const auto x0 = xForTime (segment.startSeconds), x1 = xForTime (segment.endSeconds);
            g.setColour (keyColour (segment.key).withAlpha (0.35f));
            g.fillRect (x0, bounds.getY(), x1 - x0, bounds.getHeight());
            g.setColour (Colours::white.withAlpha (0.8f));
            g.drawText (keyName (segment.key), Rectangle<float> (x0 + 4.0f, bounds.getY() + 2.0f, jmax (10.0f, x1 - x0 - 8.0f), 16.0f),
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
            g.drawText (midiNoteName (midi), Rectangle<float> (bounds.getX() + 2.0f, y - 14.0f, 40.0f, 14.0f), Justification::bottomLeft);
        }
    }

    // Detected notes: bar at the note centre, thin marker at the target (orange: key scale, green: reference,
    // cyan: reference farther than 2.5 semitones). Notes whose reference was too far / too unsure get a red outline.
    // Phrases that use the reference shifted by whole beats get a violet "+2 拍" label.
    // The targets are the notes as corrected (reference-aligned parts), drawn instead of the analysis' notes.
    const bool useTargets = targets != nullptr;
    const auto& drawnNotes = useTargets ? *targets : analysis.notes;
    for (size_t i = 0; i < drawnNotes.size(); ++i)
    {
        const auto& entry = drawnNotes[i];
        const auto x0 = xForTime (entry.note.startSeconds), x1 = xForTime (entry.note.endSeconds);
        const auto y = yForMidi (entry.note.centreMidi);
        const Rectangle<float> bar (x0, y - 5.0f, jmax (2.0f, x1 - x0), 10.0f);
        g.setColour (Colour (0x5539a0ff));
        g.fillRoundedRectangle (bar, 3.0f);
        if (entry.referenceUnsure)
        {
            g.setColour (Colour (0xffff4d4d));
            g.drawRoundedRectangle (bar, 3.0f, 1.5f);
        }
        const auto targetColour = entry.largeReferenceShift ? Colour (0xff4de0ff) : entry.hasReferenceTarget ? Colour (0xff6ee07a) : Colour (0xffffc04d);
        g.setColour (targetColour.withAlpha (0.8f));
        g.drawHorizontalLine ((int) yForMidi (entry.nearestScaleMidi), x0, x1);
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
                const auto x = xForTime (entry.note.startSeconds), y = yForMidi (entry.note.centreMidi) - 22.0f;
                g.setColour (Colour (0xffd9a6ff));
                g.drawText ((shift > 0 ? "+" : "") + String (shift) + String::fromUTF8 (u8" 拍"), Rectangle<float> (x, y, 60.0f, 14.0f),
                            Justification::centredLeft);
            }
            if (shift != 0)
                lastEnd = entry.note.endSeconds;
            lastShift = shift;
        }
    }

    // Pitch line.
    if (analysis.hasVocal)
    {
        Path line;
        bool drawing = false;
        for (const auto& frame : analysis.pitch.frames)
        {
            if (! frame.voiced)
            {
                drawing = false;
                continue;
            }
            const Point<float> point (xForTime (frame.timeSeconds), yForMidi (frequencyToMidi (frame.frequencyHz)));
            if (drawing)
                line.lineTo (point);
            else
                line.startNewSubPath (point);
            drawing = true;
        }
        g.setColour (Colour (0xff7fd6ff).withAlpha (curve != nullptr ? 0.55f : 1.0f));
        g.strokePath (line, PathStrokeType (1.5f));

        // Corrected pitch line (original pitch + shift curve).
        if (curve != nullptr && ! curve->points.empty())
        {
            const auto corrected = correctedPitchMidi (analysis.pitch, *curve);
            Path correctedLine;
            bool drawingCorrected = false;
            for (size_t i = 0; i < analysis.pitch.frames.size(); ++i)
            {
                if (corrected[i] <= 0.0)
                {
                    drawingCorrected = false;
                    continue;
                }
                const Point<float> point (xForTime (analysis.pitch.frames[i].timeSeconds), yForMidi (corrected[i]));
                if (drawingCorrected)
                    correctedLine.lineTo (point);
                else
                    correctedLine.startNewSubPath (point);
                drawingCorrected = true;
            }
            g.setColour (Colour (0xffff6fb0));
            g.strokePath (correctedLine, PathStrokeType (1.5f));
        }

        // Legend.
        g.setFont (FontOptions (12.0f));
        auto legend = Rectangle<float> (bounds.getRight() - 470.0f, bounds.getBottom() - 18.0f, 466.0f, 16.0f);
        g.setColour (Colour (0xff7fd6ff));
        g.drawText ("sung", legend.removeFromLeft (40.0f), Justification::centredLeft);
        g.setColour (Colour (0xffff6fb0));
        g.drawText ("corrected", legend.removeFromLeft (70.0f), Justification::centredLeft);
        g.setColour (Colour (0xffffc04d));
        g.drawText ("key target", legend.removeFromLeft (80.0f), Justification::centredLeft);
        g.setColour (Colour (0xff6ee07a));
        g.drawText ("reference target", legend.removeFromLeft (110.0f), Justification::centredLeft);
        g.setColour (Colour (0xff4de0ff));
        g.drawText ("far reference", legend.removeFromLeft (90.0f), Justification::centredLeft);
        g.setColour (Colour (0xffff4d4d));
        g.drawText ("unsure", legend, Justification::centredLeft);
    }
}

} // namespace shizuku
