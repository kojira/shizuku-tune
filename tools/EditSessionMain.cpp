// shizuku-edit-session: keeps one vocal take's analysis, its automatic correction and the previous edited render
// in memory, and re-renders only around hand-edited notes (see renderEditedSpans), for an interactive editor front end.
// Protocol: one JSON object per line on stdin, one JSON object per line on stdout.
//   {"cmd":"load", "vocal":"vocal.wav", "accompaniment":"inst.wav"|"-", "references":["a.svp@-0.4", ...],
//    "base":"previous/vocal_corrected.wav"}
//      analyses the take (same steps and settings as shizuku-correct), renders the automatic correction and
//      compares it with "base" (the previous automatic result: the edited output equals it outside the edited
//      spans). Reply: {"ok":true, "notes":N, "baseMatches":true|false, "seconds":S}
//   {"cmd":"render", "edits":"edits/x.json", "out":"x_after100.wav", "report":"x_correct_report.txt",
//    "pitch":"x_pitch_after.json"}
//      applies the edits (splits / merges, then per-note targets), writes the corrected WAV (32-bit float), the report (shizuku-correct --report format,
//      edited notes marked) and the corrected pitch line ({"hop":s,"midi":[...]}, original pitch + shift).
//      Reply: {"ok":true, "edits":E, "unmatched":U, "structure":S, "unmatchedStructure":US, "spans":[[s,e],...], "rendered":[[s,e],...], "seconds":S}
//   {"cmd":"quit"}
// Errors: {"ok":false, "error":"..."}.

#include "CorrectionSetup.h"

#include "../src/analysis/Json.h"
#include "../src/correction/NoteEdits.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace
{
using namespace shizuku;

struct Session
{
    std::optional<tools::PreparedCorrection> prepared;
    CorrectionSettings settings;
    double fundamentalHintHz = 0.0;
    CorrectionCurve automaticCurve;
    std::vector<std::vector<float>> baseOutput;
    bool baseMatches = false;
    std::unique_ptr<EditedRender> previous;
    CorrectionCurve previousCurve;
};

std::string number (double value, int decimals)
{
    char buffer[64];
    std::snprintf (buffer, sizeof (buffer), "%.*f", decimals, value);
    return buffer;
}

std::string spansJson (const std::vector<TimeSpan>& spans)
{
    std::string out = "[";
    for (size_t i = 0; i < spans.size(); ++i)
        out += (i ? ",[" : "[") + number (spans[i].startSeconds, 3) + "," + number (spans[i].endSeconds, 3) + "]";
    return out + "]";
}

void writeText (const std::string& path, const std::string& text)
{
    const auto temporary = path + ".tmp";
    {
        std::ofstream out (temporary, std::ios::binary);
        if (! out)
            throw std::runtime_error ("cannot write " + path);
        out << text;
    }
    if (std::rename (temporary.c_str(), path.c_str()) != 0)
        throw std::runtime_error ("cannot write " + path);
}

double secondsSince (std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
}

std::string load (Session& session, const Json& command)
{
    const auto start = std::chrono::steady_clock::now();
    std::vector<tools::ReferenceArgument> references;
    if (const auto* list = command.get ("references"))
        for (const auto& item : list->items)
            references.push_back (tools::parseReferenceArgument (item.text));
    session.previous.reset();
    session.prepared.reset();
    session.prepared = tools::prepareCorrection (command.textOr ("vocal", ""), command.textOr ("accompaniment", "-"), references, -2, -1.0, true);
    const auto& prepared = *session.prepared;
    session.fundamentalHintHz = medianVoicedFrequency (prepared.analysis.pitch);
    session.automaticCurve = planCorrection (prepared.analysis, session.settings);
    const auto automatic = renderCorrection (prepared.vocal.channels, prepared.vocal.sampleRate, session.automaticCurve, 0.0,
                                             session.fundamentalHintHz);
    const auto basePath = command.textOr ("base", "");
    if (basePath.empty())
    {
        session.baseOutput = automatic;
        session.baseMatches = true;
    }
    else
    {
        const auto base = readWav (basePath);
        if (base.channels.size() != automatic.size() || base.channels[0].size() != automatic[0].size())
            throw std::runtime_error ("base " + basePath + " does not match the vocal's length / channels");
        session.baseMatches = base.channels == automatic;
        session.baseOutput = base.channels;
    }
    return "{\"ok\":true,\"notes\":" + std::to_string (prepared.analysis.notes.size()) + ",\"baseMatches\":"
           + (session.baseMatches ? "true" : "false") + ",\"seconds\":" + number (secondsSince (start), 3) + "}";
}

std::string render (Session& session, const Json& command)
{
    if (! session.prepared)
        throw std::runtime_error ("nothing loaded");
    const auto start = std::chrono::steady_clock::now();
    const auto& prepared = *session.prepared;
    const auto& analysis = prepared.analysis;
    NoteEditDocument edits;
    const auto editsPath = command.textOr ("edits", "");
    if (! editsPath.empty())
    {
        std::ifstream probe (editsPath);
        if (probe)
            edits = loadNoteEdits (editsPath);
    }
    const auto edited = tools::applyEdits (prepared, edits);
    const auto curve = planCorrection (edited.analysis, session.settings, edited.overrides);
    auto result = std::make_unique<EditedRender> (renderEditedSpans (prepared.vocal.channels, prepared.vocal.sampleRate, session.automaticCurve,
                                                                      session.baseOutput, curve, 0.0, session.fundamentalHintHz,
                                                                      session.previous.get(), session.previous ? &session.previousCurve : nullptr));
    const double renderSeconds = secondsSince (start);

    const auto outPath = command.textOr ("out", "");
    if (outPath.empty())
        throw std::runtime_error ("render: no \"out\"");
    const auto temporary = outPath + ".tmp.wav";
    writeWavFloat32 (temporary, result->output, prepared.vocal.sampleRate);
    if (std::rename (temporary.c_str(), outPath.c_str()) != 0)
        throw std::runtime_error ("cannot write " + outPath);

    const auto reportPath = command.textOr ("report", "");
    if (! reportPath.empty())
        writeText (reportPath, prepared.log + tools::describeSummary (edited.analysis, curve, session.settings, outPath)
                                   + tools::describeNotes (edited.analysis, curve, &session.automaticCurve, &analysis.notes));
    const auto pitchPath = command.textOr ("pitch", "");
    if (! pitchPath.empty())
    {
        const auto midi = correctedPitchMidi (edited.analysis.pitch, curve);
        std::string text = "{\"hop\":" + number (analysis.pitch.hopSeconds, 6) + ",\"midi\":[";
        for (size_t i = 0; i < midi.size(); ++i)
            text += (i ? "," : "") + (midi[i] > 0.0 ? number (midi[i], 2) : std::string ("null"));
        writeText (pitchPath, text + "]}");
    }

    std::string reply = "{\"ok\":true,\"edits\":" + std::to_string (edits.edits.size()) + ",\"unmatched\":" + std::to_string (edited.unmatchedEdits.size())
                        + ",\"structure\":" + std::to_string (edits.structure.size()) + ",\"unmatchedStructure\":" + std::to_string (edited.unmatchedStructure.size())
                        + ",\"spans\":" + spansJson (result->spans) + ",\"rendered\":" + spansJson (result->renderedSpans)
                        + ",\"renderSeconds\":" + number (renderSeconds, 3) + ",\"seconds\":" + number (secondsSince (start), 3) + "}";
    session.previousCurve = curve;
    session.previous = std::move (result);
    return reply;
}
} // namespace

int main()
{
    Session session;
    std::string line;
    while (std::getline (std::cin, line))
    {
        if (line.find_first_not_of (" \t\r") == std::string::npos)
            continue;
        std::string reply;
        try
        {
            const auto command = parseJson (line);
            const auto name = command.textOr ("cmd", "");
            if (name == "quit")
                break;
            if (name == "load")
                reply = load (session, command);
            else if (name == "render")
                reply = render (session, command);
            else
                throw std::runtime_error ("unknown cmd \"" + name + "\"");
        }
        catch (const std::exception& error)
        {
            reply = "{\"ok\":false,\"error\":" + jsonQuoted (error.what()) + "}";
        }
        std::cout << reply << std::endl;
    }
    return 0;
}
