#include "ShizukuDocumentController.h"
#include "AraDiagnostics.h"

#include "ShizukuPlaybackRenderer.h"

namespace shizuku
{

using namespace juce;

namespace
{
// One playback region of a track, copied on the message thread so jobs do not touch ARA model objects.
struct RegionReadPlan
{
    const ARAAudioSource* source = nullptr; // identity only
    double sourceSampleRate = 0.0;
    int64 startInSourceSamples = 0;
    int64 numSourceSamples = 0;
    double startInPlaybackSeconds = 0.0;
};

struct TrackReadPlan
{
    String name;
    std::vector<RegionReadPlan> regions;
};

// A source the job must read from the host (no in-memory copy yet). The reader is created and destroyed on
// the message thread together with the job.
struct SourceToLoad
{
    const ARAAudioSource* source = nullptr;
    std::unique_ptr<ARAAudioSourceReader> reader;
};

// Linear-interpolation resampling to the analysis rate (only used when sources differ in rate).
std::vector<float> resampleLinear (const std::vector<float>& input, double inputRate, double outputRate)
{
    if (juce::exactlyEqual (inputRate, outputRate) || input.empty())
        return input;
    const auto outputLength = (size_t) std::floor ((double) input.size() * outputRate / inputRate);
    std::vector<float> output (outputLength);
    for (size_t i = 0; i < outputLength; ++i)
    {
        const double position = (double) i * inputRate / outputRate;
        const auto index = (size_t) position;
        const auto fraction = (float) (position - (double) index);
        const float a = input[std::min (index, input.size() - 1)];
        const float b = input[std::min (index + 1, input.size() - 1)];
        output[i] = a + (b - a) * fraction;
    }
    return output;
}

using LoadedSources = std::map<const ARAAudioSource*, std::shared_ptr<const SourceAudio>>;

// Reads every source in `toLoad` completely into memory.
bool readSources (std::vector<SourceToLoad>& toLoad, LoadedSources& loaded, const std::function<bool()>& shouldExit, String& error)
{
    constexpr int chunkSize = 1 << 16;
    for (auto& item : toLoad)
    {
        auto& reader = *item.reader;
        auto audio = std::make_shared<SourceAudio>();
        audio->sampleRate = reader.sampleRate;
        const auto numChannels = (int) reader.numChannels;
        const auto length = reader.lengthInSamples;
        audio->channels.assign ((size_t) numChannels, std::vector<float> ((size_t) length));
        AudioBuffer<float> chunk (numChannels, chunkSize);
        for (int64 done = 0; done < length; done += chunkSize)
        {
            if (shouldExit())
                return false;
            const auto count = (int) std::min<int64> (chunkSize, length - done);
            if (! reader.read (&chunk, 0, count, done, true, true))
            {
                error = "Could not read audio from the host (sample access unavailable).";
                return false;
            }
            for (int c = 0; c < numChannels; ++c)
                std::copy (chunk.getReadPointer (c), chunk.getReadPointer (c) + count, audio->channels[(size_t) c].begin() + done);
        }
        loaded[item.source] = std::move (audio);
    }
    return true;
}

std::vector<float> mixRegionsToTimeline (const TrackReadPlan& track, const LoadedSources& sources, double analysisRate,
                                         double timelineStart, double timelineEnd)
{
    if (track.regions.empty())
        return {};
    std::vector<float> timeline ((size_t) std::ceil ((timelineEnd - timelineStart) * analysisRate) + 1, 0.0f);
    for (const auto& region : track.regions)
    {
        const auto it = sources.find (region.source);
        if (it == sources.end())
            continue;
        const auto& audio = *it->second;
        std::vector<float> mono ((size_t) region.numSourceSamples, 0.0f);
        const auto numChannels = audio.channels.size();
        for (const auto& channel : audio.channels)
            for (int64 i = 0; i < region.numSourceSamples; ++i)
            {
                const auto index = region.startInSourceSamples + i;
                if (index >= 0 && index < (int64) channel.size())
                    mono[(size_t) i] += channel[(size_t) index] / (float) numChannels;
            }
        const auto resampled = resampleLinear (mono, region.sourceSampleRate, analysisRate);
        const auto offset = (int64) std::llround ((region.startInPlaybackSeconds - timelineStart) * analysisRate);
        for (size_t i = 0; i < resampled.size(); ++i)
        {
            const auto index = offset + (int64) i;
            if (index >= 0 && (size_t) index < timeline.size())
                timeline[(size_t) index] += resampled[i];
        }
    }
    return timeline;
}
} // namespace

//==============================================================================
// Loads sources that renderers need but that are not in memory yet.
class ShizukuDocumentController::SourceLoadJob final : public ThreadPoolJob
{
public:
    SourceLoadJob (ShizukuDocumentController& ownerIn, std::vector<SourceToLoad> toLoadIn)
        : ThreadPoolJob ("Shizuku Tune source load"), owner (ownerIn), toLoad (std::move (toLoadIn))
    {
    }

    JobStatus runJob() override
    {
        readSources (toLoad, loaded, [this] { return shouldExit(); }, errorText);
        finished = true;
        owner.triggerAsyncUpdate();
        return jobHasFinished;
    }

    std::atomic<bool> finished { false };
    LoadedSources loaded;
    String errorText;

private:
    ShizukuDocumentController& owner;
    std::vector<SourceToLoad> toLoad;
};

//==============================================================================
class ShizukuDocumentController::AnalysisJob final : public ThreadPoolJob
{
public:
    AnalysisJob (ShizukuDocumentController& ownerIn, TrackReadPlan vocalIn, TrackReadPlan accompanimentIn,
                 std::vector<SourceToLoad> toLoadIn, LoadedSources alreadyLoadedIn, double analysisRateIn,
                 double timelineStartIn, double timelineEndIn)
        : ThreadPoolJob ("Shizuku Tune analysis"),
          owner (ownerIn),
          vocal (std::move (vocalIn)),
          accompaniment (std::move (accompanimentIn)),
          toLoad (std::move (toLoadIn)),
          sources (std::move (alreadyLoadedIn)),
          analysisRate (analysisRateIn),
          timelineStart (timelineStartIn),
          timelineEnd (timelineEndIn)
    {
    }

    JobStatus runJob() override
    {
        String error;
        LoadedSources newlyLoaded;
        if (readSources (toLoad, newlyLoaded, [this] { return shouldExit(); }, error))
        {
            loadedSources = newlyLoaded;
            sources.insert (newlyLoaded.begin(), newlyLoaded.end());
            const auto vocalMono = mixRegionsToTimeline (vocal, sources, analysisRate, timelineStart, timelineEnd);
            const auto accompanimentMono = mixRegionsToTimeline (accompaniment, sources, analysisRate, timelineStart, timelineEnd);
            if (! shouldExit())
            {
                auto output = std::make_shared<TrackAnalysisResult>();
                output->vocalTrackName = vocal.name;
                output->accompanimentTrackName = accompaniment.name;
                output->timelineStartSeconds = timelineStart;
                output->timelineEndSeconds = timelineEnd;
                output->analysis = analyseSong (vocalMono, analysisRate, accompanimentMono, analysisRate);
                result = std::move (output);
            }
        }
        errorText = error;
        finished = true;
        owner.triggerAsyncUpdate();
        return jobHasFinished;
    }

    std::atomic<bool> finished { false };
    std::shared_ptr<const TrackAnalysisResult> result; // scale targets only (no reference applied)
    LoadedSources loadedSources;                        // sources read from the host by this job
    String errorText;

private:
    ShizukuDocumentController& owner;
    TrackReadPlan vocal, accompaniment;
    std::vector<SourceToLoad> toLoad;
    LoadedSources sources;
    double analysisRate, timelineStart, timelineEnd;
};

//==============================================================================
// Renders the corrected vocal for every vocal region from the in-memory sources.
class ShizukuDocumentController::CorrectionJob final : public ThreadPoolJob
{
public:
    struct RegionTask
    {
        const ARAPlaybackRegion* region = nullptr; // identity only
        std::shared_ptr<const SourceAudio> source;
        int64 startInSourceSamples = 0, numSourceSamples = 0;
        double startInAnalysisSeconds = 0.0;
    };

    CorrectionJob (ShizukuDocumentController& ownerIn, std::shared_ptr<const CorrectionCurve> curveIn, double fundamentalHintHzIn,
                   std::vector<RegionTask> tasksIn)
        : ThreadPoolJob ("Shizuku Tune correction"),
          curve (std::move (curveIn)),
          owner (ownerIn),
          fundamentalHintHz (fundamentalHintHzIn),
          tasks (std::move (tasksIn))
    {
    }

    JobStatus runJob() override
    {
        for (const auto& task : tasks)
        {
            if (shouldExit())
                break;
            const auto& source = *task.source;
            const double rate = source.sampleRate;
            // Pad so that ramps at the region edges have context.
            const auto pad = (int64) std::lround (0.25 * rate);
            const auto first = std::max<int64> (0, task.startInSourceSamples - pad);
            const auto last = std::min<int64> (source.numSamples(), task.startInSourceSamples + task.numSourceSamples + pad);
            if (last <= first)
                continue;
            std::vector<std::vector<float>> excerpt;
            for (const auto& channel : source.channels)
                excerpt.emplace_back (channel.begin() + first, channel.begin() + last);
            const double sampleZeroSeconds = task.startInAnalysisSeconds + (double) (first - task.startInSourceSamples) / rate;

            auto corrected = std::make_shared<SourceAudio>();
            corrected->sampleRate = rate;
            corrected->firstSample = first;
            corrected->channels = renderCorrection (excerpt, rate, *curve, sampleZeroSeconds, fundamentalHintHz);
            results.emplace_back (task.region, std::move (corrected));
        }
        finished = true;
        owner.triggerAsyncUpdate();
        return jobHasFinished;
    }

    std::atomic<bool> finished { false };
    std::shared_ptr<const CorrectionCurve> curve;
    std::vector<std::pair<const ARAPlaybackRegion*, std::shared_ptr<const SourceAudio>>> results;

private:
    ShizukuDocumentController& owner;
    double fundamentalHintHz;
    std::vector<RegionTask> tasks;
};

//==============================================================================
ShizukuDocumentController::ShizukuDocumentController (const ARA::PlugIn::PlugInEntry* entry,
                                                      const ARA::ARADocumentControllerHostInstance* instance)
    : ARADocumentControllerSpecialisation (entry, instance)
{
    araNote ("documentControllerCreated");
    startTimer (100);
}

ShizukuDocumentController::~ShizukuDocumentController()
{
    stopTimer();
    cancelPendingUpdate();
    jobPool.removeAllJobs (true, -1);
    analysisJob.reset();
    correctionJob.reset();
    sourceLoadJob.reset();
}

std::vector<ARARegionSequence*> ShizukuDocumentController::getTracks()
{
    std::vector<ARARegionSequence*> tracks;
    if (auto* document = getDocument())
        for (auto* sequence : document->getRegionSequences<ARARegionSequence>())
            tracks.push_back (sequence);
    return tracks;
}

String ShizukuDocumentController::getTrackName (ARARegionSequence& track)
{
    if (const auto& name = track.getName())
        return String::fromUTF8 (name);
    return "Track " + String (track.getOrderIndex() + 1);
}

bool ShizukuDocumentController::looksLikeAccompaniment (const String& trackName)
{
    for (const auto* keyword : { u8"伴奏", "inst", "karaoke", "backing", u8"オケ" })
        if (trackName.containsIgnoreCase (String::fromUTF8 (keyword)))
            return true;
    return false;
}

void ShizukuDocumentController::setAccompanimentTrack (ARARegionSequence* track)
{
    accompanimentTrack = track;
    accompanimentChosenByUser = true;
    sendChangeMessage();
}

void ShizukuDocumentController::setVocalTrack (ARARegionSequence* track)
{
    vocalTrack = track;
    vocalChosenByUser = true;
    sendChangeMessage();
}

void ShizukuDocumentController::chooseDefaultTracks()
{
    const auto tracks = getTracks();
    const auto contains = [&tracks] (ARARegionSequence* track)
    { return std::find (tracks.begin(), tracks.end(), track) != tracks.end(); };

    if (! contains (accompanimentTrack))
        accompanimentTrack = nullptr;
    if (! contains (vocalTrack))
        vocalTrack = nullptr;

    if (accompanimentTrack == nullptr && ! accompanimentChosenByUser)
        for (auto* track : tracks)
            if (looksLikeAccompaniment (getTrackName (*track)))
            {
                accompanimentTrack = track;
                break;
            }

    if (vocalTrack == nullptr && ! vocalChosenByUser)
        for (auto* track : tracks)
            if (track != accompanimentTrack && ! looksLikeAccompaniment (getTrackName (*track)))
            {
                vocalTrack = track;
                break;
            }
}

//==============================================================================
String ShizukuDocumentController::startAnalysis()
{
    if (analysisJob != nullptr)
        return "Analysis is already running.";
    if (vocalTrack == nullptr && accompanimentTrack == nullptr)
        return "Choose a vocal track and/or an accompaniment track first.";

    const auto snapshot = getPlaybackData();
    std::vector<SourceToLoad> toLoad;
    LoadedSources alreadyLoaded;
    double analysisRate = 0.0;
    double timelineStart = std::numeric_limits<double>::max();
    double timelineEnd = std::numeric_limits<double>::lowest();
    std::vector<VocalRegion> newVocalRegions;

    const auto planTrack = [&] (ARARegionSequence* sequence, TrackReadPlan& plan, bool isVocal) -> String
    {
        if (sequence == nullptr)
            return {};
        plan.name = getTrackName (*sequence);
        for (auto* region : sequence->getPlaybackRegions<ARAPlaybackRegion>())
        {
            auto* source = region->getAudioModification()->getAudioSource();
            const auto cached = snapshot->sources.find (source);
            if (cached != snapshot->sources.end())
            {
                alreadyLoaded[source] = cached->second;
            }
            else if (alreadyLoaded.count (source) == 0
                     && std::none_of (toLoad.begin(), toLoad.end(), [source] (const SourceToLoad& item) { return item.source == source; }))
            {
                if (! source->isSampleAccessEnabled())
                    return "The host has not enabled sample access for track \"" + plan.name + "\".";
                toLoad.push_back ({ source, std::make_unique<ARAAudioSourceReader> (source) });
            }

            RegionReadPlan regionPlan;
            regionPlan.source = source;
            regionPlan.sourceSampleRate = source->getSampleRate();
            regionPlan.startInSourceSamples = region->getStartInAudioModificationSamples();
            regionPlan.numSourceSamples = region->getEndInAudioModificationSamples() - regionPlan.startInSourceSamples;
            regionPlan.startInPlaybackSeconds = region->getStartInPlaybackTime();
            plan.regions.push_back (regionPlan);
            if (isVocal)
                newVocalRegions.push_back ({ region, source, regionPlan.startInSourceSamples, regionPlan.numSourceSamples,
                                             regionPlan.sourceSampleRate, regionPlan.startInPlaybackSeconds });

            if (analysisRate <= 0.0)
                analysisRate = source->getSampleRate();
            timelineStart = std::min (timelineStart, region->getStartInPlaybackTime());
            timelineEnd = std::max (timelineEnd, region->getEndInPlaybackTime());
        }
        return {};
    };

    TrackReadPlan vocalPlan, accompanimentPlan;
    for (const auto& error : { planTrack (vocalTrack, vocalPlan, true), planTrack (accompanimentTrack, accompanimentPlan, false) })
        if (error.isNotEmpty())
            return error;
    if (vocalPlan.regions.empty() && accompanimentPlan.regions.empty())
        return "The selected tracks have no regions.";

    for (auto& region : newVocalRegions)
        region.startInAnalysisSeconds -= timelineStart; // playback time -> analysis time
    vocalRegions = std::move (newVocalRegions);

    analysisJob = std::make_unique<AnalysisJob> (*this, std::move (vocalPlan), std::move (accompanimentPlan), std::move (toLoad),
                                                 std::move (alreadyLoaded), analysisRate, timelineStart, timelineEnd);
    jobPool.addJob (analysisJob.get(), false);
    {
        const ScopedLock lock (resultLock);
        statusText = "Analysing...";
    }
    sendChangeMessage();
    return {};
}

void ShizukuDocumentController::setCorrectionSettings (const CorrectionSettings& newSettings)
{
    correctionSettings = newSettings;
    startCorrection();
}

String ShizukuDocumentController::addReferenceMelody (const File& file)
{
    const auto error = references.add (file);
    if (error.isEmpty())
        startCorrection();
    return error;
}

void ShizukuDocumentController::clearReferenceMelodies()
{
    references.clear();
    latestReferenceTargets.reset();
    startCorrection();
}

int ShizukuDocumentController::getReferenceChoice() const
{
    const auto it = referenceChoiceByTrack.find (vocalTrack);
    return it == referenceChoiceByTrack.end() ? referenceAuto : it->second;
}

void ShizukuDocumentController::setReferenceChoice (int choice)
{
    if (vocalTrack == nullptr)
        return;
    referenceChoiceByTrack[vocalTrack] = choice;
    startCorrection();
}

std::vector<ReferenceCandidateInfo> ShizukuDocumentController::getReferenceCandidates() const
{
    return latestReferenceTargets != nullptr ? latestReferenceTargets->candidates : std::vector<ReferenceCandidateInfo> {};
}

String ShizukuDocumentController::getReferenceDescription() const
{
    return references.describe (latestReferenceTargets.get(), getReferenceChoice());
}

// Applies the reference (if any) to the analysed notes, plans the correction and starts rendering it.
void ShizukuDocumentController::startCorrection()
{
    std::shared_ptr<const TrackAnalysisResult> base;
    {
        const ScopedLock lock (resultLock);
        base = latestResult;
    }
    if (base == nullptr || ! base->analysis.hasVocal)
    {
        sendChangeMessage();
        return;
    }
    if (correctionJob != nullptr)
    {
        correctionJob->signalJobShouldExit();
        correctionPending = true; // restarted when the running job finishes
        return;
    }
    correctionPending = false;

    // Reference targets on a copy of the analysis (the stored result keeps the key-scale targets).
    auto targets = std::make_shared<ReferenceTargets> (references.applyTo (*base, getReferenceChoice()));
    latestReferenceTargets = targets;
    const auto& analysis = targets->analysis;
    auto curve = std::make_shared<CorrectionCurve> (planCorrection (analysis, correctionSettings));
    {
        const ScopedLock lock (resultLock);
        latestCorrection = curve;
        latestTargets = std::make_shared<std::vector<NoteWithKey>> (analysis.notes);
    }

    const auto snapshot = getPlaybackData();
    std::vector<CorrectionJob::RegionTask> tasks;
    for (const auto& region : vocalRegions)
    {
        const auto it = snapshot->sources.find (region.source);
        if (it == snapshot->sources.end())
            continue;
        tasks.push_back ({ region.region, it->second, region.startInSourceSamples, region.numSourceSamples, region.startInAnalysisSeconds });
    }
    correctionJob = std::make_unique<CorrectionJob> (*this, curve, medianVoicedFrequency (analysis.pitch), std::move (tasks));
    jobPool.addJob (correctionJob.get(), false);
    {
        const ScopedLock lock (resultLock);
        statusText = "Rendering corrected vocal...";
    }
    sendChangeMessage();
}

//==============================================================================
void ShizukuDocumentController::updatePlaybackData (const std::function<void (PlaybackData&)>& modify)
{
    const ScopedLock lock (playbackDataWriteLock);
    auto next = std::make_shared<PlaybackData> (*playbackData);
    modify (*next);
    retiredPlaybackData.push_back (playbackData);
    std::atomic_store (&playbackData, std::shared_ptr<const PlaybackData> (std::move (next)));
}

void ShizukuDocumentController::loadSourcesNow (const std::vector<ARAAudioSource*>& sources)
{
    const auto snapshot = getPlaybackData();
    std::vector<SourceToLoad> toLoad;
    for (auto* source : sources)
        if (snapshot->sources.count (source) == 0 && source->isSampleAccessEnabled()
            && std::none_of (toLoad.begin(), toLoad.end(), [source] (const SourceToLoad& item) { return item.source == source; }))
            toLoad.push_back ({ source, std::make_unique<ARAAudioSourceReader> (source) });
    if (toLoad.empty())
        return;

    LoadedSources loaded;
    String error;
    readSources (toLoad, loaded, [] { return false; }, error);
    if (! loaded.empty())
        updatePlaybackData ([&loaded] (PlaybackData& data) { data.sources.insert (loaded.begin(), loaded.end()); });
    if (loaded.size() < toLoad.size())
        missingSourcesRequested = true; // retry in the background
}

void ShizukuDocumentController::requestMissingSources() noexcept
{
    missingSourcesRequested = true;
}

void ShizukuDocumentController::startLoadingMissingSources()
{
    if (sourceLoadJob != nullptr || getDocument() == nullptr)
        return;
    const auto snapshot = getPlaybackData();
    std::vector<SourceToLoad> toLoad;
    for (auto* source : getDocument()->getAudioSources<ARAAudioSource>())
        if (snapshot->sources.count (source) == 0 && source->isSampleAccessEnabled() && ! source->getAudioModifications().empty())
            toLoad.push_back ({ source, std::make_unique<ARAAudioSourceReader> (source) });
    if (toLoad.empty())
        return;
    sourceLoadJob = std::make_unique<SourceLoadJob> (*this, std::move (toLoad));
    jobPool.addJob (sourceLoadJob.get(), false);
}

void ShizukuDocumentController::timerCallback()
{
    if (missingSourcesRequested.exchange (false))
        startLoadingMissingSources();

    // Free retired snapshots no renderer holds any more (so the audio thread never frees memory).
    const ScopedLock lock (playbackDataWriteLock);
    retiredPlaybackData.erase (std::remove_if (retiredPlaybackData.begin(), retiredPlaybackData.end(),
                                               [] (const std::shared_ptr<const PlaybackData>& data) { return data.use_count() == 1; }),
                               retiredPlaybackData.end());
}

void ShizukuDocumentController::handleAsyncUpdate()
{
    const auto liveSources = [this]
    {
        std::set<const ARAAudioSource*> live;
        if (auto* document = getDocument())
            for (auto* source : document->getAudioSources<ARAAudioSource>())
                live.insert (source);
        return live;
    };
    const auto publishSources = [&] (const LoadedSources& loaded)
    {
        const auto live = liveSources();
        updatePlaybackData ([&] (PlaybackData& data)
                            {
                                for (const auto& [source, audio] : loaded)
                                    if (live.count (source) != 0)
                                        data.sources[source] = audio;
                            });
    };

    if (sourceLoadJob != nullptr && sourceLoadJob->finished)
    {
        jobPool.waitForJobToFinish (sourceLoadJob.get(), -1);
        if (! sourceLoadJob->loaded.empty())
            publishSources (sourceLoadJob->loaded);
        sourceLoadJob.reset();
    }

    if (analysisJob != nullptr && analysisJob->finished)
    {
        jobPool.waitForJobToFinish (analysisJob.get(), -1);
        if (! analysisJob->loadedSources.empty())
            publishSources (analysisJob->loadedSources);
        bool analysed = false;
        {
            const ScopedLock lock (resultLock);
            if (analysisJob->result != nullptr)
            {
                latestResult = analysisJob->result;
                const auto& analysis = latestResult->analysis;
                statusText = "Analysed: " + String ((int) analysis.notes.size()) + " notes"
                             + (analysis.hasAccompaniment ? ", key " + String (keyName (analysis.key.globalKey)) : String());
                analysed = true;
            }
            else
            {
                statusText = analysisJob->errorText.isNotEmpty() ? analysisJob->errorText : String ("Analysis cancelled.");
            }
        }
        analysisJob.reset();
        if (analysed)
            startCorrection();
    }

    if (correctionJob != nullptr && correctionJob->finished)
    {
        jobPool.waitForJobToFinish (correctionJob.get(), -1);
        if (! correctionPending)
        {
            std::set<const ARAPlaybackRegion*> currentRegions;
            for (const auto& region : vocalRegions)
                currentRegions.insert (region.region);
            const auto results = correctionJob->results;
            updatePlaybackData ([&] (PlaybackData& data)
                                {
                                    data.corrected.clear();
                                    for (const auto& [region, audio] : results)
                                        if (currentRegions.count (region) != 0)
                                            data.corrected[region] = audio;
                                });
            size_t numCorrected = 0;
            for (const auto& note : correctionJob->curve->notes)
                numCorrected += note.corrected ? 1 : 0;
            const ScopedLock lock (resultLock);
            statusText = "Corrected " + String ((int) numCorrected) + " of " + String ((int) correctionJob->curve->notes.size())
                         + " notes (strength " + String (roundToInt (correctionSettings.strength * 100.0)) + "%, threshold "
                         + String (roundToInt (correctionSettings.thresholdCents)) + " cents)";
            if (latestResult != nullptr && latestResult->analysis.hasAccompaniment)
                statusText << ", key " << keyName (latestResult->analysis.key.globalKey);
        }
        correctionJob.reset();
        if (correctionPending)
            startCorrection();
    }
    sendChangeMessage();
}

String ShizukuDocumentController::getStatusText() const
{
    const ScopedLock lock (resultLock);
    return statusText;
}

std::shared_ptr<const TrackAnalysisResult> ShizukuDocumentController::getLatestResult() const
{
    const ScopedLock lock (resultLock);
    return latestResult;
}

std::shared_ptr<const CorrectionCurve> ShizukuDocumentController::getLatestCorrection() const
{
    const ScopedLock lock (resultLock);
    return latestCorrection;
}

std::shared_ptr<const std::vector<NoteWithKey>> ShizukuDocumentController::getLatestTargets() const
{
    const ScopedLock lock (resultLock);
    return latestTargets;
}

//==============================================================================
void ShizukuDocumentController::willBeginEditing (ARADocument*)
{
    processBlockLock.enterWrite();
}

void ShizukuDocumentController::didEndEditing (ARADocument*)
{
    processBlockLock.exitWrite();
    chooseDefaultTracks();
    missingSourcesRequested = true;
    sendChangeMessage();
}

void ShizukuDocumentController::willDestroyRegionSequence (ARARegionSequence* sequence)
{
    if (accompanimentTrack == sequence)
        accompanimentTrack = nullptr;
    if (vocalTrack == sequence)
        vocalTrack = nullptr;
    referenceChoiceByTrack.erase (sequence);
}

void ShizukuDocumentController::didEnableAudioSourceSamplesAccess (ARAAudioSource*, bool enable)
{
    if (enable)
        missingSourcesRequested = true;
}

void ShizukuDocumentController::doUpdateAudioSourceContent (ARAAudioSource* source, ARAContentUpdateScopes scopeFlags)
{
    if (scopeFlags.affectSamples())
    {
        updatePlaybackData ([source] (PlaybackData& data) { data.sources.erase (source); });
        missingSourcesRequested = true;
    }
}

void ShizukuDocumentController::willDestroyAudioSource (ARAAudioSource* source)
{
    updatePlaybackData ([source] (PlaybackData& data) { data.sources.erase (source); });
    vocalRegions.erase (std::remove_if (vocalRegions.begin(), vocalRegions.end(),
                                        [source] (const VocalRegion& region) { return region.source == source; }),
                        vocalRegions.end());
}

void ShizukuDocumentController::willDestroyPlaybackRegion (ARAPlaybackRegion* region)
{
    updatePlaybackData ([region] (PlaybackData& data) { data.corrected.erase (region); });
    vocalRegions.erase (std::remove_if (vocalRegions.begin(), vocalRegions.end(),
                                        [region] (const VocalRegion& entry) { return entry.region == region; }),
                        vocalRegions.end());
}

ARAPlaybackRenderer* ShizukuDocumentController::doCreatePlaybackRenderer() noexcept
{
    return new ShizukuPlaybackRenderer (getDocumentController(), *this);
}

// Stage 3 keeps no persistent state yet; analysis is re-run on demand (stage 4: ARA archiving).
bool ShizukuDocumentController::doRestoreObjectsFromStream (ARAInputStream& input, const ARARestoreObjectsFilter*) noexcept
{
    return ! input.failed();
}

bool ShizukuDocumentController::doStoreObjectsToStream (ARAOutputStream&, const ARAStoreObjectsFilter*) noexcept
{
    return true;
}

} // namespace shizuku
