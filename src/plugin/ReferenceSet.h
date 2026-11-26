#pragma once

#include "PitchLineView.h"
#include "ReferenceMelody.h"
#include "ReferenceSelection.h"

#include <juce_core/juce_core.h>

#include <memory>
#include <vector>

namespace shizuku
{

// Which candidate is the correct melody: referenceAuto = chosen per phrase by matching, referenceNone = key
// scale only, >= 0 = that candidate.
constexpr int referenceAuto = -2, referenceNone = -1;

// A reference candidate with how well it matches the analysed vocal (from the last applyTo).
struct ReferenceCandidateInfo
{
    juce::String name;
    double offsetSeconds = 0.0;
    double presence = 0.0;   // fraction of sung time where the candidate has notes
    double looseMatch = 0.0; // fraction of that time sung within ~1 semitone of it (any octave)
    double usedSeconds = 0.0; // sung time of the phrases that use it (automatic choice)
    double shiftedSeconds = 0.0; // ... of which used shifted by whole beats (call and response)
    juce::StringArray shiftedPhrases; // "79.9 s +2 beats" per shifted phrase
};

// The analysis with the targets set from the key scale and the chosen reference candidates.
struct ReferenceTargets
{
    SongAnalysis analysis;
    std::vector<ReferenceCandidateInfo> candidates; // parallel to the reference set
    ReferenceApplyStats stats;
    double beatSeconds = 0.0; // beat length used for shifted matching (from the accompaniment's tempo)
    double tempoBpm = 0.0;
};

// Reference melody candidates (.svp / .mid / guide .wav) shared by the ARA document controller and the capture
// session: loading, the time offset per candidate and the targets for an analysis (same steps as shizuku-correct).
class ReferenceSet
{
public:
    // Loads a candidate. Returns an error message on failure.
    juce::String add (const juce::File& file);
    void clear();
    size_t size() const noexcept { return melodies.size(); }
    juce::StringArray getPaths() const { return paths; }

    // Targets for `base`: scale targets, then (with candidates and choice != referenceNone) the chosen references,
    // the reference-aligned parts and their reach. Each candidate's offset is the alignment with the most matching
    // notes over every analysis applied so far (all tracks share the song's timeline; short or repetitive parts
    // otherwise align to a wrong repetition).
    ReferenceTargets applyTo (const TrackAnalysisResult& base, int choice);

    // One line for the editor: candidates, how many notes use them, tempo and beat-shifted phrases (in Japanese
    // for the capture mode's editor).
    juce::String describe (const ReferenceTargets* targets, int choice, bool japanese = false) const;

private:
    std::vector<std::shared_ptr<const ReferenceMelody>> melodies;
    std::vector<ReferenceAlignment> alignments; // parallel to melodies
    juce::StringArray paths;                    // parallel to melodies
};

} // namespace shizuku
