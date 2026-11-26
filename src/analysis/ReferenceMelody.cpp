#include "ReferenceMelody.h"

#include "Json.h"
#include "WavFile.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>

namespace shizuku
{

namespace
{
std::vector<unsigned char> readFileBytes (const std::string& path)
{
    std::ifstream in (path, std::ios::binary);
    if (! in)
        throw std::runtime_error ("cannot open " + path);
    return { std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>() };
}

std::string baseName (const std::string& path)
{
    const auto slash = path.find_last_of ('/');
    return slash == std::string::npos ? path : path.substr (slash + 1);
}

std::string lowerExtension (const std::string& path)
{
    const auto dot = path.find_last_of ('.');
    std::string extension = dot == std::string::npos ? std::string() : path.substr (dot + 1);
    for (auto& c : extension)
        c = (char) std::tolower ((unsigned char) c);
    return extension;
}

// Tempo map: piecewise-constant tempo from positions given in beats.
struct TempoMap
{
    struct Change
    {
        double beat;
        double bpm;
    };
    std::vector<Change> changes; // sorted by beat, first at beat 0

    double secondsAt (double beat) const
    {
        double seconds = 0.0;
        for (size_t i = 0; i < changes.size(); ++i)
        {
            const double segmentEnd = i + 1 < changes.size() ? changes[i + 1].beat : beat;
            const double until = std::min (beat, segmentEnd);
            if (until > changes[i].beat)
                seconds += (until - changes[i].beat) * 60.0 / changes[i].bpm;
            if (beat <= segmentEnd)
                break;
        }
        return seconds;
    }
};

void finish (ReferenceMelody& melody)
{
    if (melody.partNames.empty())
        melody.partNames.push_back ("all");
    std::sort (melody.notes.begin(), melody.notes.end(),
               [] (const ReferenceNote& a, const ReferenceNote& b) { return a.startSeconds < b.startSeconds; });
}

//==============================================================================
uint32_t readVariableLength (const std::vector<unsigned char>& data, size_t& position, size_t end)
{
    uint32_t value = 0;
    for (int i = 0; i < 4 && position < end; ++i)
    {
        const unsigned char byte = data[position++];
        value = (value << 7) | (byte & 0x7f);
        if ((byte & 0x80) == 0)
            break;
    }
    return value;
}

uint32_t readBigEndian (const std::vector<unsigned char>& data, size_t position, int bytes)
{
    uint32_t value = 0;
    for (int i = 0; i < bytes; ++i)
        value = (value << 8) | data[position + (size_t) i];
    return value;
}
} // namespace

//==============================================================================
std::string ReferenceMelody::partName (int part) const
{
    return part >= 0 && part < (int) partNames.size() ? partNames[(size_t) part] : std::string ("?");
}

ReferenceMelody loadSvp (const std::string& path)
{
    const auto bytes = readFileBytes (path);
    std::string text (bytes.begin(), bytes.end());
    while (! text.empty() && (text.back() == '\0' || std::isspace ((unsigned char) text.back())))
        text.pop_back();
    const auto document = parseJson (text);

    constexpr double blicksPerBeat = 705600000.0;
    TempoMap tempo;
    if (const auto* time = document.get ("time"))
        if (const auto* changes = time->get ("tempo"))
            for (const auto& change : changes->items)
                tempo.changes.push_back ({ change.numberOr ("position", 0.0) / blicksPerBeat, change.numberOr ("bpm", 120.0) });
    if (tempo.changes.empty() || tempo.changes.front().beat > 0.0)
        tempo.changes.insert (tempo.changes.begin(), { 0.0, tempo.changes.empty() ? 120.0 : tempo.changes.front().bpm });

    ReferenceMelody melody;
    melody.sourceName = baseName (path);
    const auto* tracks = document.get ("tracks");
    if (tracks == nullptr)
        throw std::runtime_error ("no tracks in " + path);
    for (const auto& track : tracks->items)
    {
        const auto* group = track.get ("mainGroup");
        const auto* notes = group != nullptr ? group->get ("notes") : nullptr;
        if (notes == nullptr)
            continue;
        double pitchOffset = 0.0, blickOffset = 0.0;
        if (const auto* ref = track.get ("mainRef"))
        {
            pitchOffset = ref->numberOr ("pitchOffset", 0.0);
            blickOffset = ref->numberOr ("blickOffset", 0.0);
        }
        for (const auto& note : notes->items)
        {
            const double onset = (note.numberOr ("onset", 0.0) + blickOffset) / blicksPerBeat;
            const double duration = note.numberOr ("duration", 0.0) / blicksPerBeat;
            ReferenceNote entry;
            entry.startSeconds = tempo.secondsAt (onset);
            entry.endSeconds = tempo.secondsAt (onset + duration);
            entry.midi = (int) std::lround (note.numberOr ("pitch", 60.0) + pitchOffset);
            melody.notes.push_back (entry);
        }
    }
    finish (melody);
    return melody;
}

ReferenceMelody loadMidi (const std::string& path)
{
    const auto data = readFileBytes (path);
    if (data.size() < 14 || std::string (data.begin(), data.begin() + 4) != "MThd")
        throw std::runtime_error ("not a MIDI file: " + path);
    const auto headerLength = readBigEndian (data, 4, 4);
    const auto numTracks = readBigEndian (data, 10, 2);
    const auto division = readBigEndian (data, 12, 2);
    if ((division & 0x8000) != 0)
        throw std::runtime_error ("SMPTE time division is not supported: " + path);
    const double ticksPerBeat = division;

    struct RawNote
    {
        uint64_t startTick, endTick;
        int midi, velocity, part;
    };
    std::vector<RawNote> rawNotes;
    std::vector<std::pair<uint64_t, double>> tempoChanges; // tick, microseconds per beat
    std::vector<std::string> trackNames;
    std::vector<bool> velocityIsConfidence;

    size_t position = 8 + headerLength;
    for (uint32_t track = 0; track < numTracks && position + 8 <= data.size(); ++track)
    {
        if (std::string (data.begin() + (long) position, data.begin() + (long) position + 4) != "MTrk")
            throw std::runtime_error ("bad track chunk in " + path);
        const auto length = readBigEndian (data, position + 4, 4);
        size_t p = position + 8;
        const size_t end = std::min (data.size(), p + length);
        position = end;

        uint64_t tick = 0;
        unsigned char status = 0;
        std::map<int, std::vector<std::pair<uint64_t, int>>> open; // (channel << 8 | key) -> (start tick, velocity)
        trackNames.emplace_back();
        velocityIsConfidence.push_back (false);
        while (p < end)
        {
            tick += readVariableLength (data, p, end);
            if (p >= end)
                break;
            if ((data[p] & 0x80) != 0)
                status = data[p++];
            if (status == 0xff)
            {
                if (p >= end)
                    break;
                const unsigned char type = data[p++];
                const auto metaLength = readVariableLength (data, p, end);
                if (type == 0x51 && metaLength == 3 && p + 3 <= end)
                    tempoChanges.push_back ({ tick, (double) readBigEndian (data, p, 3) });
                if ((type == 0x01 || type == 0x03) && p + metaLength <= end)
                {
                    const std::string text (data.begin() + (long) p, data.begin() + (long) (p + metaLength));
                    if (type == 0x03)
                        trackNames.back() = text;
                    else if (text == "shizuku:velocity=confidence")
                        velocityIsConfidence.back() = true;
                }
                p += metaLength;
                continue;
            }
            if (status == 0xf0 || status == 0xf7)
            {
                p += readVariableLength (data, p, end);
                continue;
            }
            const int kind = status & 0xf0, channel = status & 0x0f;
            const int dataBytes = (kind == 0xc0 || kind == 0xd0) ? 1 : 2;
            if (p + (size_t) dataBytes > end)
                break;
            const int key = data[p], velocity = dataBytes > 1 ? data[p + 1] : 0;
            p += (size_t) dataBytes;
            const int id = channel << 8 | key;
            if (kind == 0x90 && velocity > 0)
                open[id].push_back ({ tick, velocity });
            else if (kind == 0x80 || (kind == 0x90 && velocity == 0))
            {
                auto& starts = open[id];
                if (! starts.empty())
                {
                    rawNotes.push_back ({ starts.front().first, tick, key, starts.front().second, (int) track });
                    starts.erase (starts.begin());
                }
            }
        }
    }

    std::sort (tempoChanges.begin(), tempoChanges.end());
    TempoMap tempo;
    tempo.changes.push_back ({ 0.0, 120.0 });
    for (const auto& [tick, microsecondsPerBeat] : tempoChanges)
    {
        const double beat = (double) tick / ticksPerBeat;
        const double bpm = 60.0e6 / microsecondsPerBeat;
        if (beat <= tempo.changes.back().beat)
            tempo.changes.back().bpm = bpm;
        else
            tempo.changes.push_back ({ beat, bpm });
    }

    // Parts: tracks that contain notes, in file order.
    ReferenceMelody melody;
    melody.sourceName = baseName (path);
    std::map<int, int> partOfTrack;
    for (const auto& note : rawNotes)
        if (partOfTrack.count (note.part) == 0)
        {
            partOfTrack[note.part] = (int) melody.partNames.size();
            const auto& name = trackNames[(size_t) note.part];
            melody.partNames.push_back (name.empty() ? "track " + std::to_string (note.part + 1) : name);
        }
    for (const auto& note : rawNotes)
    {
        ReferenceNote entry;
        entry.startSeconds = tempo.secondsAt ((double) note.startTick / ticksPerBeat);
        entry.endSeconds = tempo.secondsAt ((double) note.endTick / ticksPerBeat);
        entry.midi = note.midi;
        entry.confidence = velocityIsConfidence[(size_t) note.part] ? note.velocity / 127.0 : 1.0;
        entry.part = partOfTrack[note.part];
        melody.notes.push_back (entry);
    }
    finish (melody);
    return melody;
}

ReferenceMelody referenceFromAudio (const std::vector<float>& monoSamples, double sampleRate, const std::string& name)
{
    ReferenceMelody melody;
    melody.sourceName = name;
    const auto track = PitchTracker().analyse (monoSamples, sampleRate);
    for (const auto& note : NoteSegmenter().segment (track))
        melody.notes.push_back ({ note.startSeconds, note.endSeconds, note.nearestMidi });
    finish (melody);
    return melody;
}

ReferenceMelody loadReference (const std::string& path)
{
    const auto extension = lowerExtension (path);
    if (extension == "svp")
        return loadSvp (path);
    if (extension == "mid" || extension == "midi")
        return loadMidi (path);
    if (extension == "wav")
    {
        const auto wav = readWav (path);
        return referenceFromAudio (wav.mixToMono(), wav.sampleRate, baseName (path));
    }
    throw std::runtime_error ("unsupported reference melody format (use .svp, .mid or .wav): " + path);
}

//==============================================================================
namespace
{
const ReferenceNote* referenceNoteAt (const ReferenceMelody& reference, double seconds)
{
    // Notes are sorted by start; find the last one starting at or before `seconds` that is still sounding.
    auto it = std::upper_bound (reference.notes.begin(), reference.notes.end(), seconds,
                                [] (double t, const ReferenceNote& n) { return t < n.startSeconds; });
    while (it != reference.notes.begin())
    {
        --it;
        if (it->endSeconds > seconds)
            return &*it;
        if (seconds - it->startSeconds > 8.0)
            break;
    }
    return nullptr;
}

// Signed distance (semitones) from `sung` to the nearest octave of `referenceMidi`, in [-6, 6).
double octaveFoldedDistance (double sung, int referenceMidi)
{
    double distance = std::fmod (sung - referenceMidi, 12.0);
    if (distance < -6.0)
        distance += 12.0;
    if (distance >= 6.0)
        distance -= 12.0;
    return distance;
}
} // namespace

ReferenceAlignment alignReference (const std::vector<NoteWithKey>& notes, const ReferenceMelody& reference, double maxOffsetSeconds)
{
    ReferenceAlignment best;
    if (notes.empty() || reference.notes.empty())
        return best;

    double totalTime = 0.0;
    for (const auto& entry : notes)
        totalTime += entry.note.endSeconds - entry.note.startSeconds;

    const auto evaluate = [&] (double offset)
    {
        ReferenceAlignment result;
        result.offsetSeconds = offset;
        double matchedTime = 0.0;
        for (const auto& entry : notes)
        {
            const auto& note = entry.note;
            const auto* target = referenceNoteAt (reference, 0.5 * (note.startSeconds + note.endSeconds) - offset);
            if (target == nullptr)
                continue;
            ++result.consideredNotes;
            if (std::abs (octaveFoldedDistance (note.centreMidi, target->midi)) <= 0.5)
            {
                ++result.matchedNotes;
                matchedTime += note.endSeconds - note.startSeconds;
            }
        }
        result.matchedFraction = totalTime > 0.0 ? matchedTime / totalTime : 0.0;
        return result;
    };

    // Coarse search every 20 ms, then refine around the best within +-20 ms in 2 ms steps.
    for (double offset = -maxOffsetSeconds; offset <= maxOffsetSeconds + 1.0e-9; offset += 0.02)
    {
        const auto candidate = evaluate (offset);
        if (candidate.matchedFraction > best.matchedFraction + 1.0e-9
            || (std::abs (candidate.matchedFraction - best.matchedFraction) <= 1.0e-9 && std::abs (offset) < std::abs (best.offsetSeconds)))
            best = candidate;
    }
    const double coarse = best.offsetSeconds;
    for (double offset = coarse - 0.02; offset <= coarse + 0.02 + 1.0e-9; offset += 0.002)
    {
        const auto candidate = evaluate (offset);
        if (candidate.matchedFraction > best.matchedFraction + 1.0e-9)
            best = candidate;
    }
    return best;
}

size_t applyReferenceTargets (std::vector<NoteWithKey>& notes, const ReferenceMelody& reference, const ReferenceTargetSettings& settings)
{
    size_t applied = 0;
    for (auto& entry : notes)
    {
        const auto& note = entry.note;
        const auto* target = referenceNoteAt (reference, 0.5 * (note.startSeconds + note.endSeconds) - settings.offsetSeconds);
        if (target == nullptr)
            continue;
        const double distance = octaveFoldedDistance (note.centreMidi, target->midi);
        if (std::abs (distance) > settings.maxDistanceSemitones)
            continue;
        entry.nearestScaleMidi = (int) std::lround (note.centreMidi - distance);
        entry.hasReferenceTarget = true;
        ++applied;
    }
    return applied;
}

} // namespace shizuku
