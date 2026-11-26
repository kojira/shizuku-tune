#include "NoteSegmenter.h"

#include "MusicTheory.h"

#include <algorithm>
#include <cmath>

namespace shizuku
{

namespace
{
double median (std::vector<double> values)
{
    if (values.empty())
        return 0.0;
    const size_t middle = values.size() / 2;
    std::nth_element (values.begin(), values.begin() + (long) middle, values.end());
    return values[middle];
}

// Pitch change of the least-squares line through the values (first to last), and its R^2.
void linearTrend (const std::vector<double>& values, double& change, double& r2)
{
    change = 0.0;
    r2 = 0.0;
    const auto n = (double) values.size();
    if (values.size() < 4)
        return;
    double sumT = 0.0, sumY = 0.0, sumTT = 0.0, sumTY = 0.0;
    for (size_t i = 0; i < values.size(); ++i)
    {
        sumT += (double) i;
        sumY += values[i];
        sumTT += (double) i * (double) i;
        sumTY += (double) i * values[i];
    }
    const double denominator = n * sumTT - sumT * sumT;
    if (denominator <= 0.0)
        return;
    const double slope = (n * sumTY - sumT * sumY) / denominator;
    const double intercept = (sumY - slope * sumT) / n;
    const double mean = sumY / n;
    double residual = 0.0, total = 0.0;
    for (size_t i = 0; i < values.size(); ++i)
    {
        const double fit = intercept + slope * (double) i;
        residual += (values[i] - fit) * (values[i] - fit);
        total += (values[i] - mean) * (values[i] - mean);
    }
    change = slope * (n - 1.0);
    r2 = total > 0.0 ? 1.0 - residual / total : 0.0;
}

// Longest run of consecutive values whose range (max - min) is <= width: [begin, end).
void longestPlateau (const std::vector<double>& values, double width, size_t& begin, size_t& end)
{
    begin = end = 0;
    for (size_t i = 0; i < values.size(); ++i)
    {
        double low = values[i], high = values[i];
        size_t j = i + 1;
        for (; j < values.size(); ++j)
        {
            low = std::min (low, values[j]);
            high = std::max (high, values[j]);
            if (high - low > width)
                break;
        }
        if (j - i > end - begin)
        {
            begin = i;
            end = j;
        }
    }
}
} // namespace

double noteCentreMidi (const std::vector<double>& midiValues, const NoteSegmenterSettings& settings, bool& glide)
{
    glide = false;
    if (midiValues.empty())
        return 0.0;
    // Use the middle 60% of the note so attack scoops and release falls do not bias the centre.
    const size_t trim = midiValues.size() / 5;
    double centre = median (std::vector<double> (midiValues.begin() + (long) trim, midiValues.end() - (long) trim));

    // A steady rise or fall: the centre is the plateau (the held pitch), not the middle of the movement.
    double change = 0.0, r2 = 0.0;
    linearTrend (midiValues, change, r2);
    if (std::abs (change) >= settings.glideMinSemitones && r2 >= settings.glideMinR2)
    {
        size_t plateauBegin = 0, plateauEnd = 0;
        longestPlateau (midiValues, settings.plateauWidthSemitones, plateauBegin, plateauEnd);
        if ((double) (plateauEnd - plateauBegin) >= settings.minPlateauFraction * (double) midiValues.size())
            centre = median (std::vector<double> (midiValues.begin() + (long) plateauBegin, midiValues.begin() + (long) plateauEnd));
        else
            glide = true;
    }
    return centre;
}

SungNote noteFromFrames (const PitchTrack& track, size_t fromFrame, size_t toFrame, const NoteSegmenterSettings& settings)
{
    SungNote note;
    std::vector<double> values;
    double levelSum = 0.0;
    toFrame = std::min (toFrame, track.frames.size());
    for (size_t f = fromFrame; f < toFrame; ++f)
    {
        const auto& frame = track.frames[f];
        if (! frame.voiced)
            continue;
        if (values.empty())
            note.startSeconds = frame.timeSeconds - 0.5 * track.hopSeconds;
        note.endSeconds = frame.timeSeconds + 0.5 * track.hopSeconds;
        values.push_back (frequencyToMidi (frame.frequencyHz));
        levelSum += frame.rmsDb;
    }
    if (values.empty())
        return note;
    note.centreMidi = noteCentreMidi (values, settings, note.glide);
    note.nearestMidi = (int) std::lround (note.centreMidi);
    note.centsOffset = (note.centreMidi - note.nearestMidi) * 100.0;
    note.meanRmsDb = levelSum / (double) values.size();
    return note;
}

std::vector<double> smoothedPitchMidi (const PitchTrack& track, double smoothSeconds)
{
    const auto& frames = track.frames;
    std::vector<double> out (frames.size(), 0.0);
    if (frames.empty() || track.hopSeconds <= 0.0)
        return out;
    const long radius = std::max (0L, (long) std::lround (0.5 * smoothSeconds / track.hopSeconds));
    for (size_t begin = 0; begin < frames.size();)
    {
        if (! frames[begin].voiced)
        {
            ++begin;
            continue;
        }
        size_t end = begin;
        while (end < frames.size() && frames[end].voiced)
            ++end;
        std::vector<double> values (end - begin);
        for (size_t i = begin; i < end; ++i)
            values[i - begin] = frequencyToMidi (frames[i].frequencyHz);
        std::vector<double> window;
        for (size_t i = begin; i < end; ++i)
        {
            // symmetric window, shrinking towards the ends of the voiced run (so a scoop or fall at the edge is not
            // flattened into a held level)
            const long r = std::min ({ radius, (long) i - (long) begin, (long) end - 1 - (long) i });
            window.assign (values.begin() + ((long) i - r - (long) begin), values.begin() + ((long) i + r + 1 - (long) begin));
            out[i] = median (window);
        }
        begin = end;
    }
    return out;
}

namespace
{
// Held levels of a line: greedy maximal runs (left to right) of at least minFrames values within width, flat
// (first and last quarter within half the width).
struct HeldLevel
{
    size_t begin = 0, end = 0; // [begin, end) positions in the line
    double level = 0.0;        // median
};

std::vector<HeldLevel> heldLevels (const std::vector<double>& line, double width, size_t minFrames)
{
    std::vector<HeldLevel> levels;
    for (size_t i = 0; i < line.size();)
    {
        double low = line[i], high = line[i];
        size_t j = i + 1;
        for (; j < line.size(); ++j)
        {
            if (std::max (high, line[j]) - std::min (low, line[j]) > width)
                break;
            low = std::min (low, line[j]);
            high = std::max (high, line[j]);
        }
        // Held, not moving through: the first and last quarters of the run differ by at most half the width (a
        // scoop moving steadily through the width is not a level). A run ending in the start of a move is shortened.
        const auto meanOf = [&] (size_t from, size_t to)
        {
            double sum = 0.0;
            for (size_t k = from; k < to; ++k)
                sum += line[k];
            return sum / (double) (to - from);
        };
        const auto isFlat = [&] (size_t end)
        {
            const size_t quarter = std::max<size_t> (1, (end - i) / 4);
            return std::abs (meanOf (end - quarter, end) - meanOf (i, i + quarter)) <= 0.5 * width;
        };
        bool flat = false;
        for (; j - i >= minFrames; --j)
            if (isFlat (j))
            {
                flat = true;
                break;
            }
        if (flat)
        {
            levels.push_back ({ i, j, median (std::vector<double> (line.begin() + (long) i, line.begin() + (long) j)) });
            i = j;
        }
        else
            ++i;
    }
    return levels;
}
} // namespace

std::vector<SungNote> NoteSegmenter::segment (const PitchTrack& track) const
{
    std::vector<SungNote> notes;
    const auto& frames = track.frames;
    const double hop = track.hopSeconds;
    if (frames.empty() || hop <= 0.0)
        return notes;

    const int maxGapFrames = std::max (0, (int) std::lround (settings.maxGapSeconds / hop));
    const int confirmFrames = std::max (1, (int) std::lround (settings.jumpConfirmSeconds / hop));
    const int onsetWindowFrames = std::max (1, (int) std::lround (settings.onsetWindowSeconds / hop));
    const int minNoteFrames = std::max (1, (int) std::lround (settings.minNoteSeconds / hop));
    const auto holdFrames = (size_t) std::max (2L, (long) std::lround (settings.holdSeconds / hop));

    std::vector<double> midi (frames.size(), 0.0);
    for (size_t f = 0; f < frames.size(); ++f)
        if (frames[f].voiced)
            midi[f] = frequencyToMidi (frames[f].frequencyHz);
    const auto smooth = smoothedPitchMidi (track, settings.vibratoSmoothSeconds);

    const auto emit = [&] (const std::vector<size_t>& indices)
    {
        if ((int) indices.size() < minNoteFrames)
            return;
        std::vector<double> values;
        double levelSum = 0.0;
        for (size_t index : indices)
        {
            values.push_back (midi[index]);
            levelSum += frames[index].rmsDb;
        }
        SungNote note;
        note.startSeconds = frames[indices.front()].timeSeconds - 0.5 * hop;
        note.endSeconds = frames[indices.back()].timeSeconds + 0.5 * hop;
        note.centreMidi = noteCentreMidi (values, settings, note.glide);
        note.nearestMidi = (int) std::lround (note.centreMidi);
        note.centsOffset = (note.centreMidi - note.nearestMidi) * 100.0;
        note.meanRmsDb = levelSum / (double) indices.size();
        notes.push_back (note);
    };

    // A note candidate (split by gaps, level onsets and leaps) is split further at changes between held levels
    // of the vibrato-free line.
    std::vector<size_t> current; // voiced frame indices of the note being built
    const auto finishNote = [&]
    {
        std::vector<double> line;
        for (size_t index : current)
            line.push_back (smooth[index]);
        const auto levels = heldLevels (line, settings.holdWidthSemitones, holdFrames);
        size_t partBegin = 0;
        double anchor = levels.empty() ? 0.0 : levels.front().level;
        for (size_t k = 1; k < levels.size(); ++k)
        {
            if (std::abs (levels[k].level - anchor) < settings.levelChangeSemitones)
                continue;
            // Split where the line crosses the middle between the two levels (searched from the end of the
            // previous held level; at the latest where the new level begins).
            const double middle = 0.5 * (anchor + levels[k].level);
            const bool rising = levels[k].level > anchor;
            size_t cut = levels[k].begin;
            for (size_t i = levels[k - 1].end; i < levels[k].begin; ++i)
                if (rising ? line[i] >= middle : line[i] <= middle)
                {
                    cut = i;
                    break;
                }
            if (cut > partBegin && cut - partBegin >= (size_t) minNoteFrames && current.size() - cut >= (size_t) minNoteFrames)
            {
                emit (std::vector<size_t> (current.begin() + (long) partBegin, current.begin() + (long) cut));
                partBegin = cut;
            }
            anchor = levels[k].level;
        }
        emit (std::vector<size_t> (current.begin() + (long) partBegin, current.end()));
        current.clear();
    };

    // The note so far ends in a held stretch: its last holdSeconds (or all of it, if shorter) of the vibrato-free
    // line lie within holdWidthSemitones (a scoop moving into the next pitch does not).
    const auto endsHeld = [&]
    {
        const size_t count = std::min (current.size(), holdFrames);
        double low = 1.0e9, high = -1.0e9;
        for (size_t k = current.size() - count; k < current.size(); ++k)
        {
            low = std::min (low, smooth[current[k]]);
            high = std::max (high, smooth[current[k]]);
        }
        return high - low <= settings.holdWidthSemitones;
    };

    const auto noteCentre = [&]
    {
        std::vector<double> values;
        for (size_t index : current)
            values.push_back (midi[index]);
        return median (values);
    };

    int gap = 0;
    for (size_t f = 0; f < frames.size(); ++f)
    {
        if (! frames[f].voiced)
        {
            ++gap;
            if (gap > maxGapFrames && ! current.empty())
                finishNote();
            continue;
        }
        gap = 0;

        if (! current.empty())
        {
            // Level onset: a sudden rise relative to the recent minimum starts a new note. Only frames
            // after the note's own attack (its first minNoteFrames) count, so a fade-in is not an onset.
            const size_t settledFrame = current.front() + (size_t) minNoteFrames;
            const size_t windowStart = std::max (settledFrame, f >= (size_t) onsetWindowFrames ? f - (size_t) onsetWindowFrames : 0);
            double recentMinimum = frames[f].rmsDb;
            for (size_t g = windowStart; g < f; ++g)
                recentMinimum = std::min (recentMinimum, frames[g].rmsDb);
            const bool levelOnset = windowStart < f && frames[f].rmsDb - recentMinimum >= settings.onsetRiseDb;

            // Leap: the next confirmFrames voiced frames all sit leapSemitones away from the note centre, and the
            // vibrato-free line has moved at least half as far by the end of them.
            bool leap = false;
            const double centre = noteCentre();
            if (std::abs (midi[f] - centre) >= settings.leapSemitones && endsHeld())
            {
                leap = true;
                int checked = 0;
                size_t g = f;
                for (; g < frames.size() && checked < confirmFrames; ++g, ++checked)
                {
                    if (! frames[g].voiced || std::abs (midi[g] - centre) < settings.leapSemitones)
                    {
                        leap = false;
                        break;
                    }
                }
                if (leap && (g == 0 || ! frames[g - 1].voiced || std::abs (smooth[g - 1] - centre) < 0.5 * settings.leapSemitones))
                    leap = false;
                // ... and it settles: the vibrato-free line then holds a level (a fall at the end does not).
                if (leap)
                {
                    double low = 1.0e9, high = -1.0e9;
                    size_t k = g;
                    for (; k < frames.size() && k < g + holdFrames && frames[k].voiced; ++k)
                    {
                        low = std::min (low, smooth[k]);
                        high = std::max (high, smooth[k]);
                    }
                    leap = k == g + holdFrames && high - low <= settings.holdWidthSemitones;
                }
            }

            if (levelOnset || leap)
                finishNote();
        }
        current.push_back (f);
    }
    if (! current.empty())
        finishNote();
    return notes;
}

} // namespace shizuku
