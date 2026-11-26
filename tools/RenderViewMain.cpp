// shizuku-render-view: analyses <vocal.wav> <accompaniment.wav> and draws the plug-in's
// PitchLineView into a PNG, so the on-screen result can be checked without a DAW.
// Usage: shizuku-render-view <vocal.wav> <accompaniment.wav> <out.png> [width height] [--reference melody.svp|.mid|.wav[@offset]]...
// --reference (repeatable) adds candidates chosen per phrase as in the plug-in's automatic choice.
// The default correction (100 %, 25 cents) is drawn as the corrected pitch line.

#include "PitchLineView.h"
#include "ReferenceMelody.h"
#include "ReferenceSelection.h"
#include "WavFile.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>
#include <string>

int main (int argc, char** argv)
{
    if (argc < 4)
    {
        std::cerr << "usage: " << argv[0] << " <vocal.wav> <accompaniment.wav> <out.png> [width height]\n";
        return 2;
    }
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;
    std::vector<std::string> referencePaths;
    std::vector<std::string> positional;
    for (int i = 4; i < argc; ++i)
    {
        if (std::string (argv[i]) == "--reference" && i + 1 < argc)
            referencePaths.push_back (argv[++i]);
        else
            positional.push_back (argv[i]);
    }
    const int width = positional.size() >= 2 ? std::atoi (positional[0].c_str()) : 960;
    const int height = positional.size() >= 2 ? std::atoi (positional[1].c_str()) : 420;

    try
    {
        const auto vocal = shizuku::readWav (argv[1]);
        const auto accompaniment = shizuku::readWav (argv[2]);
        auto result = std::make_shared<shizuku::TrackAnalysisResult>();
        result->vocalTrackName = "vocal";
        result->accompanimentTrackName = "accompaniment";
        result->analysis = shizuku::analyseSong (vocal.mixToMono(), vocal.sampleRate, accompaniment.mixToMono(), accompaniment.sampleRate);
        result->timelineEndSeconds = std::max (vocal.channels.empty() ? 0.0 : (double) vocal.channels[0].size() / vocal.sampleRate,
                                               accompaniment.channels.empty() ? 0.0 : (double) accompaniment.channels[0].size() / accompaniment.sampleRate);

        auto targets = std::make_shared<std::vector<shizuku::NoteWithKey>> (result->analysis.notes);
        if (! referencePaths.empty())
        {
            std::vector<shizuku::ReferenceCandidate> candidates;
            for (const auto& path : referencePaths)
            {
                shizuku::ReferenceCandidate candidate;
                auto argument = path;
                const auto at = argument.find_last_of ('@');
                const bool offsetGiven = at != std::string::npos;
                const double offset = offsetGiven ? std::atof (argument.c_str() + at + 1) : 0.0;
                if (offsetGiven)
                    argument.resize (at);
                candidate.melody = shizuku::loadReference (argument);
                candidate.name = candidate.melody.sourceName;
                candidate.offsetSeconds = offsetGiven ? offset : shizuku::alignReference (*targets, candidate.melody).offsetSeconds;
                candidates.push_back (std::move (candidate));
            }
            shizuku::ReferenceSelectionSettings selectionSettings;
            selectionSettings.beatSeconds = shizuku::beatSecondsForTempo (result->analysis.tempo.bpm, result->analysis.tempo.confidence);
            auto aligned = result->analysis;
            auto selection = shizuku::selectReferences (aligned.notes, candidates, selectionSettings);
            shizuku::alignNotesToReferences (aligned, candidates, selection);
            shizuku::applySelectedReferences (aligned.notes, candidates, selection, selectionSettings);
            shizuku::measureReach (aligned.notes, aligned.pitch);
            *targets = aligned.notes;
        }
        auto withTargets = result->analysis;
        withTargets.notes = *targets;
        auto curve = std::make_shared<shizuku::CorrectionCurve> (shizuku::planCorrection (withTargets, {}));

        shizuku::PitchLineView view;
        view.setSize (width, height);
        view.setResult (result);
        view.setCorrection (curve, targets);
        const auto image = view.createComponentSnapshot (view.getLocalBounds(), true, 1.0f);

        juce::File output (juce::File::getCurrentWorkingDirectory().getChildFile (argv[3]));
        output.deleteFile();
        juce::FileOutputStream stream (output);
        juce::PNGImageFormat png;
        if (! stream.openedOk() || ! png.writeImageToStream (image, stream))
        {
            std::cerr << "error: cannot write " << argv[3] << "\n";
            return 1;
        }
        std::cout << "wrote " << output.getFullPathName() << " (" << result->analysis.notes.size() << " notes, key "
                  << shizuku::keyName (result->analysis.key.globalKey) << ")\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
