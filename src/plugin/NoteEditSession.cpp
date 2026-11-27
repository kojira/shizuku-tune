#include "NoteEditSession.h"

#include "MusicTheory.h"

#include <cmath>
#include <cstring>

namespace shizuku
{

using namespace juce;

namespace
{
// Times are kept in whole milliseconds, the precision of the JSON file, so a saved document gives exactly the
// same notes as the one in memory.
double roundToMillisecond (double seconds)
{
    return std::round (seconds * 1000.0) / 1000.0;
}

std::string nowText()
{
    return Time::getCurrentTime().toISO8601 (true).toStdString();
}

// Edits of a note whose middle lies inside one of notes are dropped (the new edit replaces them).
void dropEditsOf (NoteEditDocument& document, const std::vector<NoteSpan>& notes)
{
    auto& edits = document.edits;
    edits.erase (std::remove_if (edits.begin(), edits.end(),
                                 [&] (const NoteEdit& edit)
                                 {
                                     const double middle = 0.5 * (edit.startSeconds + edit.endSeconds);
                                     return std::any_of (notes.begin(), notes.end(), [middle] (const NoteSpan& note)
                                                         { return middle >= note.startSeconds && middle <= note.endSeconds; });
                                 }),
                 edits.end());
}
} // namespace

//==============================================================================
bool NoteEditHistory::apply (const NoteEditDocument& next)
{
    if (sameNoteEdits (next, document))
        return false;
    undoStack.push_back (document);
    redoStack.clear();
    document = next;
    return true;
}

bool NoteEditHistory::undo()
{
    if (undoStack.empty())
        return false;
    redoStack.push_back (document);
    document = undoStack.back();
    undoStack.pop_back();
    return true;
}

bool NoteEditHistory::redo()
{
    if (redoStack.empty())
        return false;
    undoStack.push_back (document);
    document = redoStack.back();
    redoStack.pop_back();
    return true;
}

void NoteEditHistory::reset (const NoteEditDocument& next)
{
    document = next;
    undoStack.clear();
    redoStack.clear();
}

bool sameNoteEdits (const NoteEditDocument& a, const NoteEditDocument& b)
{
    const auto strip = [] (NoteEditDocument d)
    {
        d.file = d.baseVersion = {};
        for (auto& e : d.edits)
            e.created = e.version = {};
        for (auto& e : d.structure)
            e.created = e.version = {};
        return toJson (d);
    };
    return strip (a) == strip (b);
}

bool hasNoteEdits (const NoteEditDocument& document)
{
    return ! document.edits.empty() || ! document.structure.empty();
}

//==============================================================================
EditedCorrection planEditedCorrection (const ReferenceTargets& targets, const CorrectionSettings& settings, const NoteEditDocument& timelineEdits,
                                       double timelineStartSeconds, std::shared_ptr<const CorrectionCurve> automaticCurve,
                                       std::shared_ptr<const std::vector<NoteWithKey>> automaticNotes)
{
    EditedCorrection result;
    if (! hasNoteEdits (timelineEdits))
    {
        result.curve = std::move (automaticCurve);
        result.notes = std::move (automaticNotes);
        return result;
    }
    static const std::vector<ReferenceCandidate> noCandidates;
    const auto edits = shiftNoteEdits (timelineEdits, -timelineStartSeconds);
    auto edited = applyNoteEdits (targets.analysis, edits, targets.referencesApplied ? targets.referenceCandidates : noCandidates,
                                  targets.selection, targets.selectionSettings);
    auto analysis = targets.analysis;
    analysis.notes = edited.notes;
    result.curve = std::make_shared<CorrectionCurve> (planCorrection (analysis, settings, edited.overrides));
    result.notes = std::make_shared<std::vector<NoteWithKey>> (std::move (edited.notes));
    result.unmatchedEdits = edited.unmatchedEdits.size();
    result.unmatchedStructure = edited.unmatchedStructure.size();
    return result;
}

LayeredRender renderLayered (const std::vector<std::vector<float>>& input, double sampleRate, double sampleZeroSeconds, double fundamentalHintHz,
                             std::shared_ptr<const CorrectionCurve> automaticCurve, std::shared_ptr<const CorrectionCurve> editedCurve,
                             const LayeredRender* previous)
{
    LayeredRender result;
    result.automaticCurve = automaticCurve;
    result.editedCurve = editedCurve != nullptr ? editedCurve : automaticCurve;
    const bool sameBase = previous != nullptr && previous->automaticCurve == automaticCurve && previous->automatic != nullptr;
    result.automatic = sameBase ? previous->automatic
                                : std::make_shared<const std::vector<std::vector<float>>> (
                                      renderCorrection (input, sampleRate, *automaticCurve, sampleZeroSeconds, fundamentalHintHz));
    if (result.editedCurve == automaticCurve)
        return result;
    const bool reuse = sameBase && previous->edited != nullptr;
    result.edited = std::make_shared<const EditedRender> (renderEditedSpans (input, sampleRate, *automaticCurve, *result.automatic, *result.editedCurve,
                                                                             sampleZeroSeconds, fundamentalHintHz,
                                                                             reuse ? previous->edited.get() : nullptr,
                                                                             reuse ? previous->editedCurve.get() : nullptr));
    return result;
}

//==============================================================================
NoteEditDocument withNoteTargets (const NoteEditDocument& document, const std::vector<NoteSpan>& notes, const std::vector<double>& targetMidi)
{
    auto next = document;
    dropEditsOf (next, notes);
    const auto created = nowText();
    for (size_t i = 0; i < notes.size() && i < targetMidi.size(); ++i)
    {
        NoteEdit edit;
        edit.startSeconds = roundToMillisecond (notes[i].startSeconds);
        edit.endSeconds = roundToMillisecond (notes[i].endSeconds);
        edit.mode = NoteEditMode::target;
        edit.targetMidi = roundToCent (targetMidi[i]);
        edit.created = created;
        next.edits.push_back (edit);
    }
    return next;
}

NoteEditDocument withNoteMode (const NoteEditDocument& document, const std::vector<NoteSpan>& notes, NoteEditMode mode)
{
    auto next = document;
    dropEditsOf (next, notes);
    if (mode == NoteEditMode::automatic)
        return next; // "auto" is the absence of an edit
    const auto created = nowText();
    for (const auto& note : notes)
    {
        NoteEdit edit;
        edit.startSeconds = roundToMillisecond (note.startSeconds);
        edit.endSeconds = roundToMillisecond (note.endSeconds);
        edit.mode = mode;
        edit.created = created;
        next.edits.push_back (edit);
    }
    return next;
}

NoteEditDocument withSplit (const NoteEditDocument& document, double timeSeconds)
{
    auto next = document;
    NoteStructureEdit edit;
    edit.op = NoteStructureEdit::Op::split;
    edit.timeSeconds = roundToMillisecond (timeSeconds);
    edit.created = nowText();
    next.structure.push_back (edit);
    return next;
}

NoteEditDocument withMerge (const NoteEditDocument& document, const NoteSpan& first, const NoteSpan& last)
{
    auto next = document;
    NoteStructureEdit edit;
    edit.op = NoteStructureEdit::Op::merge;
    edit.startSeconds = roundToMillisecond (std::min (first.startSeconds, last.startSeconds));
    edit.endSeconds = roundToMillisecond (std::max (first.endSeconds, last.endSeconds));
    edit.created = nowText();
    next.structure.push_back (edit);
    return next;
}

//==============================================================================
bool parsePitchText (const String& text, double& midi)
{
    // Japanese "セント", a trailing "c" for cents and the minus sign U+2212 are accepted.
    auto s = text.trim().replace (String::fromUTF8 (u8"セント"), "").replace (String::fromUTF8 (u8"−"), "-").trim();
    if (s.endsWithIgnoreCase ("c") && s.length() > 2)
        s = s.dropLastCharacters (1).trim();
    if (s.isEmpty())
        return false;
    static const char* names = "CDEFGAB";
    static const int classes[] = { 0, 2, 4, 5, 7, 9, 11 };
    const auto letter = CharacterFunctions::toUpperCase (s[0]);
    const char* at = std::strchr (names, (char) letter);
    if (letter == 0 || at == nullptr)
        return false;
    int pitchClass = classes[at - names];
    int i = 1;
    if (s[i] == '#' || s[i] == 0x266f)
    {
        ++pitchClass;
        ++i;
    }
    else if (s[i] == 'b' || s[i] == 0x266d)
    {
        --pitchClass;
        ++i;
    }
    // Octave: optional minus, digits.
    int j = i;
    if (s[j] == '-' && CharacterFunctions::isDigit (s[j + 1]))
        ++j;
    while (CharacterFunctions::isDigit (s[j]))
        ++j;
    if (j == i)
        return false;
    const int octave = s.substring (i, j).getIntValue();
    auto rest = s.substring (j).trim();
    double cents = 0.0;
    if (rest.isNotEmpty())
    {
        if (! rest.containsOnly ("+-0123456789."))
            return false;
        cents = rest.getDoubleValue();
        if (std::abs (cents) > 100.0)
            return false;
    }
    midi = roundToCent (12.0 * (octave + 1) + pitchClass + cents / 100.0);
    return true;
}

String pitchText (double midi)
{
    const int note = (int) std::lround (midi);
    const int cents = (int) std::lround ((midi - note) * 100.0);
    return String (midiNoteName (note)) + (cents >= 0 ? " +" : " -") + String (std::abs (cents));
}

} // namespace shizuku
