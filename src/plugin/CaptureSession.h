#pragma once

#include "CorrectionModel.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <vector>

namespace shizuku
{

// Vocal (main input) and accompaniment (side chain) recorded against the host timeline, at one sample rate.
struct CapturedAudio
{
    double sampleRate = 0.0;
    juce::int64 originSample = 0;                 // timeline sample of index 0
    std::vector<std::vector<float>> vocal;        // per input channel, all the same length
    std::vector<float> accompaniment;             // mono mix of the side chain; empty: none received
    std::vector<juce::Range<juce::int64>> covered; // timeline ranges actually played (sorted, merged)

    juce::int64 length() const noexcept { return vocal.empty() ? 0 : (juce::int64) vocal[0].size(); }
};

// The corrected vocal played on the audio thread. Immutable once published.
struct CapturePlayback
{
    double sampleRate = 0.0;
    juce::int64 originSample = 0;
    std::vector<std::vector<float>> corrected;      // same layout as CapturedAudio::vocal
    std::vector<juce::Range<juce::int64>> replaced; // timeline ranges where it differs from the input (sorted)
};

// "Play once to capture" mode (hosts that do not bind the plug-in to ARA, such as Logic Pro running natively):
// while armed and the host plays, processBlock records the input and the side chain with the playhead position;
// when the transport stops, the take is analysed and corrected in the background with the same code as the ARA
// mode (analyseSong, ReferenceSet, planCorrection, renderCorrection). Afterwards processBlock outputs the corrected
// vocal at the playhead position where it differs from the input, and the input everywhere else (no latency;
// identical in offline bounces). Captures are kept in <root>/<id>/ (root: ~/Library/Application Support/
// ShizukuTune/captures, or $SHIZUKU_TUNE_CAPTURE_DIR); the plug-in state holds only the id and the settings.
// $SHIZUKU_TUNE_CAPTURE_ARM_ON_START=1 arms a new instance as soon as the host has prepared it (for host tools
// without a GUI).
class CaptureSession final : public CorrectionModel,
                             private juce::Timer
{
public:
    enum class State
    {
        idle,      // nothing captured
        armed,     // waiting for the host to play
        capturing, // recording
        analysing, // analysis / correction rendering in the background (also after loading a saved capture)
        ready      // corrected vocal available
    };

    CaptureSession();
    ~CaptureSession() override;

    // Audio thread. mainIO is processed in place; sideChain may have 0 channels.
    void prepare (double sampleRate, int maximumBlockSize);
    void process (juce::AudioBuffer<float>& mainIO, const juce::AudioBuffer<float>& sideChain,
                  const juce::Optional<juce::AudioPlayHead::PositionInfo>& position, bool nonRealtime) noexcept;

    // Message thread.
    void arm();           // start waiting for playback (discards a capture in progress)
    void finishCapture(); // stop recording now and analyse what was captured
    void cancelArm();
    State getState() const noexcept { return state; }
    double getCapturedSeconds() const noexcept;
    juce::String getRangeText() const; // "取り込み範囲 0:00.0〜3:45.2" etc.
    juce::String getCaptureId() const { return captureId; }
    bool sampleRateMismatch() const noexcept;

    // Plug-in state: id + settings. restoreState loads the saved capture in the background.
    juce::ValueTree saveState() const;
    void restoreState (const juce::ValueTree& tree);

    // Where captures are stored.
    static juce::File getCaptureRoot();

    // Called on the message thread by the timer; also usable by tests to drive the session.
    void update();

    // CorrectionModel
    CorrectionSettings getCorrectionSettings() const override { return correctionSettings; }
    void setCorrectionSettings (const CorrectionSettings& newSettings) override;
    juce::String addReferenceMelody (const juce::File& file) override;
    void clearReferenceMelodies() override;
    juce::String getReferenceDescription() const override;
    int getReferenceChoice() const override { return referenceChoice; }
    void setReferenceChoice (int choice) override;
    bool canChooseReference() const override { return true; }
    std::vector<ReferenceCandidateInfo> getReferenceCandidates() const override;
    juce::String getStatusText() const override;
    std::shared_ptr<const TrackAnalysisResult> getLatestResult() const override { return latestResult; }
    std::shared_ptr<const CorrectionCurve> getLatestCorrection() const override { return latestCorrection; }
    std::shared_ptr<const std::vector<NoteWithKey>> getLatestTargets() const override { return latestTargets; }

private:
    class Recorder;
    class Job;

    void timerCallback() override { update(); }
    void startAnalysis (std::shared_ptr<const CapturedAudio> audio, bool saveAudio);
    void startCorrection();
    void publishPlayback (std::shared_ptr<const CapturePlayback> playback);
    void writeCaptureInfo() const;
    juce::File captureDirectory() const;
    void cancelJobs();

    // Audio thread view.
    std::atomic<State> state { State::idle };
    std::atomic<double> hostSampleRate { 0.0 };
    std::atomic<bool> transportStopped { false }; // capturing and the host stopped
    std::atomic<int> recordersInside { 0 };        // audio threads inside the armed / capturing branch of process
    std::unique_ptr<Recorder> recorder;

    std::shared_ptr<const CapturePlayback> playback; // std::atomic_load / atomic_store
    std::vector<std::shared_ptr<const CapturePlayback>> retiredPlayback; // freed on the message thread
    juce::CriticalSection playbackWriteLock;

    // Message thread.
    juce::ThreadPool jobPool { juce::ThreadPoolOptions{}.withThreadName ("Shizuku Tune capture").withNumberOfThreads (1) };
    std::vector<std::unique_ptr<Job>> jobs;
    int generation = 0; // results of jobs started for an older capture are ignored
    bool correctionRunning = false, correctionPending = false;
    bool restoredRenderValid = false; // the loaded corrected.wav matches the settings (no render needed)
    juce::var renderedWith;
    bool armedFromEnvironment = false;           // settings and references of the published corrected vocal
    juce::String captureId;
    std::shared_ptr<const CapturedAudio> captured;
    CorrectionSettings correctionSettings;
    ReferenceSet references;
    int referenceChoice = referenceAuto;
    std::shared_ptr<const ReferenceTargets> referenceTargets;
    std::shared_ptr<const TrackAnalysisResult> latestResult;
    std::shared_ptr<const CorrectionCurve> latestCorrection;
    std::shared_ptr<const std::vector<NoteWithKey>> latestTargets;
    juce::String detailText; // last analysis / correction message
    juce::uint32 lastCaptureActivityMs = 0;
    juce::int64 lastCapturedSamples = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CaptureSession)
};

} // namespace shizuku
