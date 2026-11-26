#pragma once

#include "CorrectionModel.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <map>
#include <memory>
#include <optional>
#include <set>

namespace shizuku
{

// Samples of an audio source (or a corrected excerpt of one), at the source's sample rate.
struct SourceAudio
{
    double sampleRate = 0.0;
    juce::int64 firstSample = 0;              // source sample index of channels[c][0]
    std::vector<std::vector<float>> channels; // all the same length

    juce::int64 numSamples() const noexcept { return channels.empty() ? 0 : (juce::int64) channels[0].size(); }
};

// Everything the playback renderers read on the audio thread. Immutable once published; replaced as a whole.
struct PlaybackData
{
    std::map<const juce::ARAAudioSource*, std::shared_ptr<const SourceAudio>> sources;      // whole source in memory
    std::map<const juce::ARAPlaybackRegion*, std::shared_ptr<const SourceAudio>> corrected; // corrected vocal per region
};

// ARA document controller: tracks which region sequence is the accompaniment / vocal, runs the whole-file
// analysis and the correction on a background thread, and keeps all audio the renderers play in memory, so
// playback never waits for the host's disk reads (the cause of silent/late real-time playback before).
class ShizukuDocumentController final : public juce::ARADocumentControllerSpecialisation,
                                        public CorrectionModel,
                                        private juce::AsyncUpdater,
                                        private juce::Timer
{
public:
    ShizukuDocumentController (const ARA::PlugIn::PlugInEntry* entry,
                               const ARA::ARADocumentControllerHostInstance* instance);
    ~ShizukuDocumentController() override;

    // Region sequences ("tracks") in document order. Message thread only.
    std::vector<juce::ARARegionSequence*> getTracks();
    static juce::String getTrackName (juce::ARARegionSequence& track);
    static bool looksLikeAccompaniment (const juce::String& trackName);

    juce::ARARegionSequence* getAccompanimentTrack() const noexcept { return accompanimentTrack; }
    juce::ARARegionSequence* getVocalTrack() const noexcept { return vocalTrack; }
    void setAccompanimentTrack (juce::ARARegionSequence* track);
    void setVocalTrack (juce::ARARegionSequence* track);

    // Starts analysing the selected tracks. Returns an error message if it cannot start.
    juce::String startAnalysis();
    bool isAnalysing() const noexcept { return analysisJob != nullptr; }

    // CorrectionModel (the reference choice is remembered per vocal track; missing: referenceAuto).
    juce::String getStatusText() const override;
    std::shared_ptr<const TrackAnalysisResult> getLatestResult() const override;
    std::shared_ptr<const CorrectionCurve> getLatestCorrection() const override;
    std::shared_ptr<const std::vector<NoteWithKey>> getLatestTargets() const override;
    CorrectionSettings getCorrectionSettings() const override { return correctionSettings; }
    void setCorrectionSettings (const CorrectionSettings& newSettings) override;
    juce::String addReferenceMelody (const juce::File& file) override;
    void clearReferenceMelodies() override;
    juce::String getReferenceDescription() const override;
    int getReferenceChoice() const override;
    void setReferenceChoice (int choice) override;
    bool canChooseReference() const override { return vocalTrack != nullptr; }
    std::vector<ReferenceCandidateInfo> getReferenceCandidates() const override;

    // Audio thread: current snapshot of in-memory audio. Never blocks for long, never frees memory.
    std::shared_ptr<const PlaybackData> getPlaybackData() const { return std::atomic_load (&playbackData); }

    // Loads the given sources into memory now (blocking). Called from the renderers' prepareToPlay.
    void loadSourcesNow (const std::vector<juce::ARAAudioSource*>& sources);
    // Real-time safe: asks the message thread to load sources that are still missing.
    void requestMissingSources() noexcept;

    juce::ScopedTryReadLock getProcessingLock() { return juce::ScopedTryReadLock { processBlockLock }; }

protected:
    void willBeginEditing (juce::ARADocument*) override;
    void didEndEditing (juce::ARADocument*) override;
    void willDestroyRegionSequence (juce::ARARegionSequence*) override;
    void didEnableAudioSourceSamplesAccess (juce::ARAAudioSource*, bool enable) override;
    void doUpdateAudioSourceContent (juce::ARAAudioSource*, juce::ARAContentUpdateScopes) override;
    void willDestroyAudioSource (juce::ARAAudioSource*) override;
    void willDestroyPlaybackRegion (juce::ARAPlaybackRegion*) override;

    juce::ARAPlaybackRenderer* doCreatePlaybackRenderer() noexcept override;

    bool doRestoreObjectsFromStream (juce::ARAInputStream&, const juce::ARARestoreObjectsFilter*) noexcept override;
    bool doStoreObjectsToStream (juce::ARAOutputStream&, const juce::ARAStoreObjectsFilter*) noexcept override;

private:
    class AnalysisJob;
    class CorrectionJob;
    class SourceLoadJob;

    // One region of the vocal track as it was when analysed: maps analysis time to source samples.
    struct VocalRegion
    {
        juce::ARAPlaybackRegion* region = nullptr; // key only; removed in willDestroyPlaybackRegion
        juce::ARAAudioSource* source = nullptr;
        juce::int64 startInSourceSamples = 0;
        juce::int64 numSourceSamples = 0;
        double sourceSampleRate = 0.0;
        double startInAnalysisSeconds = 0.0; // analysis time of startInSourceSamples
    };

    void handleAsyncUpdate() override;
    void timerCallback() override;
    void chooseDefaultTracks();
    void startCorrection();
    void startLoadingMissingSources();

    // Reads a whole source through `reader` and publishes it in the playback data (any non-real-time thread).
    std::shared_ptr<const SourceAudio> loadSource (const juce::ARAAudioSource* source, juce::ARAAudioSourceReader& reader,
                                                   const std::function<bool()>& shouldExit, juce::String& error);
    // Copy-modify-publish of the playback snapshot (any thread; serialised by playbackDataWriteLock).
    void updatePlaybackData (const std::function<void (PlaybackData&)>& modify);

    juce::ReadWriteLock processBlockLock;
    juce::ARARegionSequence* accompanimentTrack = nullptr;
    juce::ARARegionSequence* vocalTrack = nullptr;
    bool accompanimentChosenByUser = false;
    bool vocalChosenByUser = false;

    juce::ThreadPool jobPool { juce::ThreadPoolOptions{}.withThreadName ("Shizuku Tune analysis").withNumberOfThreads (1) };
    std::unique_ptr<AnalysisJob> analysisJob;     // message thread owns all jobs; finished jobs are released in handleAsyncUpdate
    std::unique_ptr<CorrectionJob> correctionJob;
    std::unique_ptr<SourceLoadJob> sourceLoadJob;
    bool correctionPending = false;
    std::atomic<bool> missingSourcesRequested { false };

    CorrectionSettings correctionSettings;
    std::vector<VocalRegion> vocalRegions; // message thread
    ReferenceSet references;
    std::map<const juce::ARARegionSequence*, int> referenceChoiceByTrack; // missing: referenceAuto
    std::shared_ptr<const ReferenceTargets> latestReferenceTargets;       // message thread

    juce::CriticalSection playbackDataWriteLock;
    std::shared_ptr<const PlaybackData> playbackData { std::make_shared<PlaybackData>() };
    std::vector<std::shared_ptr<const PlaybackData>> retiredPlaybackData; // freed by writers once no renderer holds them

    mutable juce::CriticalSection resultLock;
    std::shared_ptr<const TrackAnalysisResult> latestResult;
    std::shared_ptr<const CorrectionCurve> latestCorrection;
    std::shared_ptr<const std::vector<NoteWithKey>> latestTargets;
    juce::String statusText { "Choose the vocal and accompaniment tracks, then press Analyse." };
};

} // namespace shizuku
