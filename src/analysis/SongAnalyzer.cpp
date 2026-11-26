#include "SongAnalyzer.h"

#include <cmath>
#include <cstdio>
#include <sstream>

namespace shizuku
{

MusicalKey keyAtTime (const KeyAnalysis& analysis, double timeSeconds)
{
    for (const auto& segment : analysis.segments)
        if (timeSeconds >= segment.startSeconds && timeSeconds < segment.endSeconds)
            return segment.key;
    if (! analysis.segments.empty() && timeSeconds >= analysis.segments.back().startSeconds)
        return analysis.segments.back().key;
    return analysis.globalKey;
}

SongAnalysis analyseSong (const std::vector<float>& vocalMono, double vocalSampleRate,
                          const std::vector<float>& accompanimentMono, double accompanimentSampleRate)
{
    SongAnalysis result;
    if (! accompanimentMono.empty() && accompanimentSampleRate > 0.0)
    {
        result.hasAccompaniment = true;
        result.key = KeyDetector().analyse (accompanimentMono, accompanimentSampleRate);
        result.tempo = estimateTempo (accompanimentMono, accompanimentSampleRate);
    }
    if (! vocalMono.empty() && vocalSampleRate > 0.0)
    {
        result.hasVocal = true;
        result.pitch = PitchTracker().analyse (vocalMono, vocalSampleRate);
        for (const auto& note : NoteSegmenter().segment (result.pitch))
        {
            NoteWithKey entry;
            entry.note = note;
            entry.keyAtNote = keyAtTime (result.key, 0.5 * (note.startSeconds + note.endSeconds));
            entry.nearestScaleMidi = result.hasAccompaniment ? nearestScaleMidiNote (entry.keyAtNote, note.centreMidi)
                                                             : note.nearestMidi;
            result.notes.push_back (entry);
        }
    }
    return result;
}

namespace
{
std::string number (double value, int decimals)
{
    char buffer[64];
    std::snprintf (buffer, sizeof (buffer), "%.*f", decimals, value);
    return buffer;
}

std::string keyJson (const MusicalKey& key)
{
    return std::string ("{\"name\":\"") + keyName (key) + "\",\"tonic\":\"" + pitchClassName (key.tonicPitchClass)
           + "\",\"mode\":\"" + (key.mode == KeyMode::major ? "major" : "minor") + "\"}";
}
} // namespace

std::string toJson (const SongAnalysis& analysis, bool includePitchFrames)
{
    std::ostringstream out;
    out << "{\n";
    if (analysis.hasAccompaniment)
    {
        out << "  \"key\": {\n";
        out << "    \"global\": " << keyJson (analysis.key.globalKey) << ",\n";
        out << "    \"globalConfidence\": " << number (analysis.key.globalConfidence, 3) << ",\n";
        out << "    \"candidates\": [";
        for (size_t i = 0; i < analysis.key.rankedGlobalKeys.size() && i < 3; ++i)
            out << (i ? ", " : "") << "{\"name\":\"" << keyName (analysis.key.rankedGlobalKeys[i].key)
                << "\",\"correlation\":" << number (analysis.key.rankedGlobalKeys[i].correlation, 3) << "}";
        out << "],\n";
        out << "    \"chroma\": [";
        for (size_t i = 0; i < 12; ++i)
            out << (i ? ", " : "") << number (analysis.key.globalChroma[i], 3);
        out << "],\n";
        out << "    \"segments\": [\n";
        for (size_t i = 0; i < analysis.key.segments.size(); ++i)
        {
            const auto& segment = analysis.key.segments[i];
            out << "      {\"start\":" << number (segment.startSeconds, 3) << ",\"end\":" << number (segment.endSeconds, 3)
                << ",\"key\":" << keyJson (segment.key) << ",\"confidence\":" << number (segment.confidence, 3) << "}"
                << (i + 1 < analysis.key.segments.size() ? "," : "") << "\n";
        }
        out << "    ]\n  },\n";
        out << "  \"tempo\": {\"bpm\":" << number (analysis.tempo.bpm, 2) << ",\"confidence\":" << number (analysis.tempo.confidence, 3) << "}";
    }
    else
    {
        out << "  \"key\": null";
    }
    out << ",\n";

    out << "  \"notes\": [\n";
    for (size_t i = 0; i < analysis.notes.size(); ++i)
    {
        const auto& entry = analysis.notes[i];
        const auto& note = entry.note;
        out << "    {\"start\":" << number (note.startSeconds, 3) << ",\"end\":" << number (note.endSeconds, 3)
            << ",\"midi\":" << number (note.centreMidi, 3) << ",\"nearest\":\"" << midiNoteName (note.nearestMidi)
            << "\",\"cents\":" << number (note.centsOffset, 1) << ",\"scaleTarget\":\"" << midiNoteName (entry.nearestScaleMidi)
            << "\",\"scaleTargetMidi\":" << entry.nearestScaleMidi << ",\"rmsDb\":" << number (note.meanRmsDb, 1) << "}"
            << (i + 1 < analysis.notes.size() ? "," : "") << "\n";
    }
    out << "  ]";

    if (includePitchFrames && analysis.hasVocal)
    {
        out << ",\n  \"pitch\": {\"hop\":" << number (analysis.pitch.hopSeconds, 6) << ",\"hz\":[";
        for (size_t i = 0; i < analysis.pitch.frames.size(); ++i)
            out << (i ? "," : "") << number (analysis.pitch.frames[i].frequencyHz, 2);
        out << "]}";
    }
    out << "\n}\n";
    return out.str();
}

} // namespace shizuku
