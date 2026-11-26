// shizuku-correct: analyses the vocal (and the accompaniment for the key), corrects the vocal and writes it.
// Usage: shizuku-correct <vocal.wav> <accompaniment.wav|-> <out.wav> [--strength 0..100] [--threshold cents]
//                       [--reference melody.svp|.mid|.wav[@offset]]... [--reference-offset seconds]
//                       [--choose auto|N] [--bpm N] [--edits edits.json] [--report]
// --reference (repeatable): candidate "correct melodies". Per sung phrase the best matching candidate is chosen
//              (any octave) and its notes become the targets; see ReferenceSelection.h. "@offset" or
//              --reference-offset (applies to the last --reference) sets the time offset (vocal time = reference
//              time + offset); otherwise it is found automatically.
// --choose N:  use candidate N (0-based) wherever it has notes instead of choosing per phrase.
//              Phrases may use a candidate shifted by whole beats (call and response); the beat comes from the
//              accompaniment's tempo (--bpm overrides it, --bpm 0 turns shifting off).
// --edits:     hand edits (NoteEdits.h): splits / merges of notes and per-note targets; they take precedence over the
//              automatic decisions. Only the spans around edited notes differ from the automatic correction (see
//              renderEditedSpans).
// --report prints the chosen candidate per phrase and one line per note (deviation, applied shift) to stdout.

#include "CorrectionSetup.h"

#include "../src/correction/NoteEdits.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

int main (int argc, char** argv)
{
    if (argc < 4)
    {
        std::cerr << "usage: " << argv[0]
                  << " <vocal.wav> <accompaniment.wav|-> <out.wav> [--strength 0..100] [--threshold cents]"
                     " [--reference melody[@offset]]... [--reference-offset seconds] [--choose auto|N] [--bpm N] [--edits edits.json] [--report]\n";
        return 2;
    }
    shizuku::CorrectionSettings settings;
    bool report = false;
    std::vector<shizuku::tools::ReferenceArgument> references;
    int choose = -2;
    double bpmOverride = -1.0;
    std::string editsPath;
    for (int i = 4; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--strength") == 0 && i + 1 < argc)
            settings.strength = std::atof (argv[++i]) / 100.0;
        else if (std::strcmp (argv[i], "--threshold") == 0 && i + 1 < argc)
            settings.thresholdCents = std::atof (argv[++i]);
        else if (std::strcmp (argv[i], "--reference") == 0 && i + 1 < argc)
            references.push_back (shizuku::tools::parseReferenceArgument (argv[++i]));
        else if (std::strcmp (argv[i], "--reference-offset") == 0 && i + 1 < argc && ! references.empty())
        {
            references.back().offset = std::atof (argv[++i]);
            references.back().offsetGiven = true;
        }
        else if (std::strcmp (argv[i], "--choose") == 0 && i + 1 < argc)
        {
            ++i;
            choose = std::strcmp (argv[i], "auto") == 0 ? -2 : std::atoi (argv[i]);
        }
        else if (std::strcmp (argv[i], "--bpm") == 0 && i + 1 < argc)
            bpmOverride = std::atof (argv[++i]);
        else if (std::strcmp (argv[i], "--edits") == 0 && i + 1 < argc)
            editsPath = argv[++i];
        else if (std::strcmp (argv[i], "--report") == 0)
            report = true;
        else
        {
            std::cerr << "unknown option " << argv[i] << "\n";
            return 2;
        }
    }

    try
    {
        const auto prepared = shizuku::tools::prepareCorrection (argv[1], argv[2], references, choose, bpmOverride, report);
        const auto& analysis = prepared.analysis;
        std::printf ("%s", prepared.log.c_str());
        shizuku::NoteEditDocument edits;
        if (! editsPath.empty())
            edits = shizuku::loadNoteEdits (editsPath);
        const auto edited = shizuku::tools::applyEdits (prepared, edits);
        const bool anyEdits = ! edits.edits.empty() || ! edits.structure.empty();
        if (! editsPath.empty())
        {
            std::printf ("edits %s: %zu edits, %zu without a note; %zu splits / merges, %zu without a note\n", editsPath.c_str(), edits.edits.size(),
                         edited.unmatchedEdits.size(), edits.structure.size(), edited.unmatchedStructure.size());
            for (const auto e : edited.unmatchedEdits)
                std::printf ("  edit %.3f-%.3fs found no note\n", edits.edits[e].startSeconds, edits.edits[e].endSeconds);
            for (const auto e : edited.unmatchedStructure)
                std::printf ("  %s at %.3fs found no note\n", edits.structure[e].op == shizuku::NoteStructureEdit::Op::split ? "split" : "merge",
                             edits.structure[e].op == shizuku::NoteStructureEdit::Op::split ? edits.structure[e].timeSeconds : edits.structure[e].startSeconds);
        }
        // With edits: the automatic render, with only the spans around edited notes rendered again (as the check
        // page's edit session does), so every other note is bit-identical to the automatic correction.
        const double fundamentalHintHz = shizuku::medianVoicedFrequency (analysis.pitch);
        const auto automaticCurve = shizuku::planCorrection (analysis, settings);
        auto corrected = shizuku::renderCorrection (prepared.vocal.channels, prepared.vocal.sampleRate, automaticCurve, 0.0, fundamentalHintHz);
        const auto curve = anyEdits ? shizuku::planCorrection (edited.analysis, settings, edited.overrides) : automaticCurve;
        if (anyEdits)
            corrected = shizuku::renderEditedSpans (prepared.vocal.channels, prepared.vocal.sampleRate, automaticCurve, corrected, curve, 0.0,
                                                    fundamentalHintHz)
                            .output;
        shizuku::writeWavFloat32 (argv[3], corrected, prepared.vocal.sampleRate);
        std::printf ("%s", shizuku::tools::describeSummary (edited.analysis, curve, settings, argv[3]).c_str());
        if (report)
            std::printf ("%s", shizuku::tools::describeNotes (edited.analysis, curve, anyEdits ? &automaticCurve : nullptr,
                                                              anyEdits ? &analysis.notes : nullptr)
                                   .c_str());
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
