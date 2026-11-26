#include "NoteEdits.h"

#include "Json.h"
#include "MusicTheory.h"
#include "NoteSegmenter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace shizuku
{

double roundToCent (double midi)
{
    return std::round (midi * 100.0) / 100.0;
}

NoteEditDocument parseNoteEdits (const std::string& jsonText)
{
    const auto root = parseJson (jsonText);
    if (root.type != Json::Type::object)
        throw std::runtime_error ("note edits: the document is not an object");
    NoteEditDocument document;
    document.file = root.textOr ("file", "");
    document.baseVersion = root.textOr ("baseVersion", "");
    if (const auto* structure = root.get ("structure"))
    {
        if (structure->type != Json::Type::array)
            throw std::runtime_error ("note edits: \"structure\" is not an array");
        for (const auto& item : structure->items)
        {
            NoteStructureEdit edit;
            const auto op = item.textOr ("op", "");
            if (op == "split")
            {
                edit.op = NoteStructureEdit::Op::split;
                edit.timeSeconds = item.numberOr ("time", -1.0);
                if (edit.timeSeconds < 0.0)
                    throw std::runtime_error ("note edits: a split has no \"time\"");
            }
            else if (op == "merge")
            {
                edit.op = NoteStructureEdit::Op::merge;
                edit.startSeconds = item.numberOr ("start", -1.0);
                edit.endSeconds = item.numberOr ("end", -1.0);
                if (edit.startSeconds < 0.0 || edit.endSeconds < 0.0)
                    throw std::runtime_error ("note edits: a merge has no \"start\" / \"end\"");
                if (edit.endSeconds < edit.startSeconds)
                    std::swap (edit.startSeconds, edit.endSeconds);
            }
            else
                throw std::runtime_error ("note edits: unknown structure op \"" + op + "\"");
            edit.created = item.textOr ("created", "");
            edit.version = item.textOr ("version", "");
            document.structure.push_back (edit);
        }
    }
    const auto* edits = root.get ("edits");
    if (edits == nullptr)
        return document;
    if (edits->type != Json::Type::array)
        throw std::runtime_error ("note edits: \"edits\" is not an array");
    for (const auto& item : edits->items)
    {
        NoteEdit edit;
        edit.startSeconds = item.numberOr ("start", -1.0);
        edit.endSeconds = item.numberOr ("end", edit.startSeconds);
        if (edit.startSeconds < 0.0)
            throw std::runtime_error ("note edits: an edit has no \"start\"");
        if (edit.endSeconds < edit.startSeconds)
            std::swap (edit.startSeconds, edit.endSeconds);
        const auto mode = item.textOr ("mode", "target");
        if (mode == "target")
        {
            edit.mode = NoteEditMode::target;
            const auto* target = item.get ("targetMidi");
            if (target == nullptr || target->type != Json::Type::number)
                throw std::runtime_error ("note edits: a \"target\" edit has no \"targetMidi\"");
            edit.targetMidi = roundToCent (target->number);
        }
        else if (mode == "keep")
            edit.mode = NoteEditMode::keep;
        else if (mode == "auto")
            edit.mode = NoteEditMode::automatic;
        else
            throw std::runtime_error ("note edits: unknown mode \"" + mode + "\"");
        edit.created = item.textOr ("created", "");
        edit.version = item.textOr ("version", "");
        document.edits.push_back (edit);
    }
    return document;
}

NoteEditDocument loadNoteEdits (const std::string& path)
{
    std::ifstream in (path, std::ios::binary);
    if (! in)
        throw std::runtime_error ("cannot open " + path);
    std::stringstream text;
    text << in.rdbuf();
    return parseNoteEdits (text.str());
}

std::string toJson (const NoteEditDocument& document)
{
    const auto number = [] (double value, int decimals)
    {
        char buffer[64];
        std::snprintf (buffer, sizeof (buffer), "%.*f", decimals, value);
        return std::string (buffer);
    };
    std::string out = "{\n  \"format\": \"shizuku-note-edits\",\n  \"formatVersion\": 2,\n";
    out += "  \"file\": " + jsonQuoted (document.file) + ",\n";
    out += "  \"baseVersion\": " + jsonQuoted (document.baseVersion) + ",\n";
    out += "  \"structure\": [";
    for (size_t i = 0; i < document.structure.size(); ++i)
    {
        const auto& edit = document.structure[i];
        out += i ? ",\n    " : "\n    ";
        out += edit.op == NoteStructureEdit::Op::split ? "{\"op\": \"split\", \"time\": " + number (edit.timeSeconds, 3)
                                                       : "{\"op\": \"merge\", \"start\": " + number (edit.startSeconds, 3) + ", \"end\": " + number (edit.endSeconds, 3);
        out += ", \"created\": " + jsonQuoted (edit.created) + ", \"version\": " + jsonQuoted (edit.version) + "}";
    }
    out += document.structure.empty() ? "],\n" : "\n  ],\n";
    out += "  \"edits\": [";
    for (size_t i = 0; i < document.edits.size(); ++i)
    {
        const auto& edit = document.edits[i];
        out += i ? ",\n    " : "\n    ";
        out += "{\"start\": " + number (edit.startSeconds, 3) + ", \"end\": " + number (edit.endSeconds, 3) + ", \"mode\": ";
        out += edit.mode == NoteEditMode::target ? "\"target\", \"targetMidi\": " + number (roundToCent (edit.targetMidi), 2)
               : edit.mode == NoteEditMode::keep ? std::string ("\"keep\"")
                                                 : std::string ("\"auto\"");
        out += ", \"created\": " + jsonQuoted (edit.created) + ", \"version\": " + jsonQuoted (edit.version) + "}";
    }
    out += document.edits.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

std::vector<NoteWithKey> applyNoteStructure (const SongAnalysis& analysis, const std::vector<NoteWithKey>& notes,
                                             const std::vector<NoteStructureEdit>& structure, const NoteStructureSettings& settings,
                                             std::vector<size_t>* unmatched)
{
    auto result = notes;
    const auto& track = analysis.pitch;
    const double hop = track.hopSeconds;
    if (track.frames.empty() || hop <= 0.0)
    {
        if (unmatched != nullptr)
            for (size_t e = 0; e < structure.size(); ++e)
                unmatched->push_back (e);
        return result;
    }
    // Frame range [first, last) of the frames whose centre lies in [from, to).
    const auto frameAt = [&] (double t) { return (size_t) std::clamp ((long) std::ceil ((t - track.frames.front().timeSeconds) / hop - 1.0e-9), 0L, (long) track.frames.size()); };
    const auto made = [&] (double from, double to)
    {
        NoteWithKey entry;
        entry.note = noteFromFrames (track, frameAt (from), frameAt (to));
        entry.keyAtNote = keyAtTime (analysis.key, 0.5 * (entry.note.startSeconds + entry.note.endSeconds));
        entry.nearestScaleMidi = analysis.hasAccompaniment ? nearestScaleMidiNote (entry.keyAtNote, entry.note.centreMidi) : entry.note.nearestMidi;
        entry.alignedReferenceNote = -2;
        return entry;
    };
    for (size_t e = 0; e < structure.size(); ++e)
    {
        const auto& edit = structure[e];
        bool applied = false;
        if (edit.op == NoteStructureEdit::Op::split)
        {
            const double t = edit.timeSeconds;
            for (size_t i = 0; i < result.size(); ++i)
            {
                const auto& note = result[i].note;
                if (std::abs (t - note.startSeconds) <= settings.existingBoundarySeconds || std::abs (t - note.endSeconds) <= settings.existingBoundarySeconds)
                {
                    applied = true; // already a boundary there
                    break;
                }
                if (t <= note.startSeconds || t >= note.endSeconds)
                    continue;
                auto left = made (note.startSeconds, t), right = made (t, note.endSeconds + 0.5 * hop);
                const auto length = [] (const NoteWithKey& n) { return n.note.endSeconds - n.note.startSeconds; };
                if (length (left) >= settings.minPartSeconds && length (right) >= settings.minPartSeconds)
                {
                    result[i] = left;
                    result.insert (result.begin() + (long) i + 1, right);
                    applied = true;
                }
                break;
            }
        }
        else
        {
            size_t first = result.size(), last = 0;
            for (size_t i = 0; i < result.size(); ++i)
            {
                const double middle = 0.5 * (result[i].note.startSeconds + result[i].note.endSeconds);
                if (middle >= edit.startSeconds && middle <= edit.endSeconds)
                {
                    first = std::min (first, i);
                    last = std::max (last, i);
                }
            }
            if (first < result.size())
            {
                applied = true;
                if (last > first)
                {
                    const auto joined = made (result[first].note.startSeconds, result[last].note.endSeconds + 0.5 * hop);
                    result.erase (result.begin() + (long) first + 1, result.begin() + (long) last + 1);
                    result[first] = joined;
                }
            }
        }
        if (! applied && unmatched != nullptr)
            unmatched->push_back (e);
    }
    return result;
}

std::vector<NoteOverride> resolveNoteEdits (const std::vector<NoteWithKey>& notes, const std::vector<NoteEdit>& edits,
                                            const NoteEditMatchSettings& settings, std::vector<size_t>* unmatched)
{
    std::vector<NoteOverride> overrides (notes.size());
    for (size_t e = 0; e < edits.size(); ++e)
    {
        const auto& edit = edits[e];
        long best = -1;
        double bestOverlap = 0.0, bestGap = settings.maxGapSeconds;
        std::vector<size_t> inside;
        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto& note = notes[i].note;
            const double overlap = std::min (note.endSeconds, edit.endSeconds) - std::max (note.startSeconds, edit.startSeconds);
            const double length = note.endSeconds - note.startSeconds;
            if (overlap > bestOverlap)
            {
                bestOverlap = overlap;
                best = (long) i;
            }
            else if (bestOverlap <= 0.0 && overlap <= 0.0)
            {
                const double gap = -overlap;
                if (gap < bestGap)
                {
                    bestGap = gap;
                    best = (long) i;
                }
            }
            if (overlap > 0.0 && length > 0.0 && overlap >= settings.splitNoteInsideFraction * length)
                inside.push_back (i);
        }
        if (best < 0)
        {
            if (unmatched != nullptr)
                unmatched->push_back (e);
            continue;
        }
        if (std::find (inside.begin(), inside.end(), (size_t) best) == inside.end())
            inside.push_back ((size_t) best);
        for (const auto i : inside)
            overrides[i] = { edit.mode, edit.mode == NoteEditMode::target ? roundToCent (edit.targetMidi) : 0.0 };
    }
    return overrides;
}

} // namespace shizuku
