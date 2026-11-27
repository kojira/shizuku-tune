#include "CaptureSession.h"

#include "WavFile.h"

#include <array>
#include <numeric>

namespace shizuku
{

using namespace juce;

namespace
{
constexpr int maxVocalChannels = 2;
constexpr double fifoSeconds = 8.0;            // audio buffered between the audio thread and the drain thread
constexpr double maxCaptureSpanSeconds = 3600.0; // blocks farther than this from the first one are ignored
constexpr int64 replacedMergeGapSamples = 256;  // runs where the corrected vocal differs closer than this are joined
constexpr uint32 hostSilentTimeoutMs = 3000;    // capturing and no audio for this long: the host stopped processing

const Identifier stateType ("ShizukuTuneCapture"), idProperty ("captureId"), strengthProperty ("strength"),
    thresholdProperty ("thresholdCents"), choiceProperty ("referenceChoice"), editsProperty ("noteEdits");

void addRange (std::vector<Range<int64>>& ranges, Range<int64> range)
{
    ranges.push_back (range);
    std::sort (ranges.begin(), ranges.end(), [] (auto a, auto b) { return a.getStart() < b.getStart(); });
    std::vector<Range<int64>> merged;
    for (const auto& r : ranges)
    {
        if (! merged.empty() && r.getStart() <= merged.back().getEnd())
            merged.back() = merged.back().getUnionWith (r);
        else
            merged.push_back (r);
    }
    ranges = std::move (merged);
}

// Timeline ranges (inside `covered`) where `corrected` differs from `vocal`.
std::vector<Range<int64>> differingRanges (const CapturedAudio& audio, const std::vector<std::vector<float>>& corrected)
{
    std::vector<Range<int64>> ranges;
    for (const auto& span : audio.covered)
    {
        int64 runStart = -1, lastDiff = -1;
        for (auto t = span.getStart(); t < span.getEnd(); ++t)
        {
            const auto i = (size_t) (t - audio.originSample);
            bool differs = false;
            for (size_t c = 0; c < audio.vocal.size() && ! differs; ++c)
                differs = i < corrected[c].size() && ! exactlyEqual (corrected[c][i], audio.vocal[c][i]);
            if (! differs)
                continue;
            if (runStart >= 0 && t - lastDiff > replacedMergeGapSamples)
            {
                ranges.push_back ({ runStart, lastDiff + 1 });
                runStart = -1;
            }
            if (runStart < 0)
                runStart = t;
            lastDiff = t;
        }
        if (runStart >= 0)
            ranges.push_back ({ runStart, lastDiff + 1 });
    }
    return ranges;
}

String formatTime (double seconds)
{
    const auto sign = seconds < 0.0 ? String ("-") : String();
    seconds = std::abs (seconds);
    const int minutes = (int) (seconds / 60.0);
    return sign + String (minutes) + ":" + String (seconds - minutes * 60.0, 1).paddedLeft ('0', 4);
}

var rangesToVar (const std::vector<Range<int64>>& ranges)
{
    Array<var> list;
    for (const auto& r : ranges)
        list.add (Array<var> { var (r.getStart()), var (r.getEnd()) });
    return list;
}

std::vector<Range<int64>> rangesFromVar (const var& value)
{
    std::vector<Range<int64>> ranges;
    if (const auto* list = value.getArray())
        for (const auto& item : *list)
            if (item.isArray() && item.size() == 2)
                ranges.push_back ({ (int64) item[0], (int64) item[1] });
    return ranges;
}

// What a corrected render depends on besides the captured audio (compared when a saved render is reused).
var renderKey (const CorrectionSettings& settings, int choice, const StringArray& referencePaths, const NoteEditDocument& edits)
{
    auto object = std::make_unique<DynamicObject>();
    object->setProperty ("strength", settings.strength);
    object->setProperty ("thresholdCents", settings.thresholdCents);
    object->setProperty ("referenceChoice", choice);
    Array<var> paths;
    for (const auto& path : referencePaths)
        paths.add (path);
    object->setProperty ("references", paths);
    if (hasNoteEdits (edits))
    {
        auto plain = edits; // "created" does not change the render
        for (auto& edit : plain.edits)
            edit.created = edit.version = {};
        for (auto& edit : plain.structure)
            edit.created = edit.version = {};
        object->setProperty ("edits", String::fromUTF8 (toJson (plain).c_str()));
    }
    return var (object.release());
}

NoteEditDocument noteEditsFromText (const String& text)
{
    if (text.trim().isEmpty())
        return {};
    try
    {
        return parseNoteEdits (text.toStdString());
    }
    catch (const std::exception&)
    {
        return {};
    }
}
} // namespace

//==============================================================================
// Real-time safe hand-over of the recorded blocks: the audio thread writes each block (timeline position, vocal
// channels, side-chain mono mix) into preallocated lock-free FIFOs; a drain thread copies them into the growing
// timeline buffers (allocation happens only there). A later block at the same position overwrites an earlier one.
class CaptureSession::Recorder final : private Thread
{
public:
    Recorder() : Thread ("Shizuku Tune capture") {}
    ~Recorder() override { stopThread (2000); }

    // Message thread, not while recording.
    void allocate (double rate)
    {
        const auto capacity = (int) std::ceil (fifoSeconds * jmax (8000.0, rate));
        if (capacity == sampleCapacity)
            return;
        sampleCapacity = capacity;
        sampleFifo.setTotalSize (capacity);
        for (auto& ring : vocalRings)
            ring.assign ((size_t) capacity, 0.0f);
        sideRing.assign ((size_t) capacity, 0.0f);
        headers.assign (headerCapacity, {});
        headerFifo.setTotalSize (headerCapacity);
    }

    void start (double rate)
    {
        stopThread (2000);
        sampleFifo.reset();
        headerFifo.reset();
        audio = std::make_shared<CapturedAudio>();
        audio->sampleRate = rate;
        sideChainHeard = false;
        capturedSamples = 0;
        droppedBlocks = 0;
        startThread();
    }

    // Audio thread. wait: block until there is room (offline rendering), else drop the block when full.
    void push (int64 position, const AudioBuffer<float>& main, const AudioBuffer<float>& side, bool wait) noexcept
    {
        const int numSamples = main.getNumSamples();
        if (numSamples <= 0 || sampleCapacity == 0 || numSamples >= sampleCapacity)
            return;
        while (sampleFifo.getFreeSpace() < numSamples || headerFifo.getFreeSpace() < 1)
        {
            if (! wait || ! isThreadRunning())
            {
                ++droppedBlocks;
                return;
            }
            Thread::sleep (1);
        }
        const int numChannels = jmin (maxVocalChannels, main.getNumChannels());
        int start1, size1, start2, size2;
        sampleFifo.prepareToWrite (numSamples, start1, size1, start2, size2);
        const auto copy = [&] (int ringStart, int count, int offset)
        {
            for (int c = 0; c < maxVocalChannels; ++c)
                FloatVectorOperations::copy (vocalRings[(size_t) c].data() + ringStart,
                                             main.getReadPointer (jmin (c, numChannels - 1)) + offset, count);
            const int sideChannels = side.getNumChannels();
            auto* sideOut = sideRing.data() + ringStart;
            if (sideChannels == 0)
            {
                FloatVectorOperations::clear (sideOut, count);
                return;
            }
            // Mono mix as WavData::mixToMono (sum, then scale), so the analysis sees what shizuku-correct would.
            FloatVectorOperations::copy (sideOut, side.getReadPointer (0) + offset, count);
            for (int c = 1; c < sideChannels; ++c)
                FloatVectorOperations::add (sideOut, side.getReadPointer (c) + offset, count);
            if (sideChannels > 1)
                FloatVectorOperations::multiply (sideOut, 1.0f / (float) sideChannels, count);
        };
        copy (start1, size1, 0);
        if (size2 > 0)
            copy (start2, size2, size1);
        sampleFifo.finishedWrite (size1 + size2);

        headerFifo.prepareToWrite (1, start1, size1, start2, size2);
        headers[(size_t) (size1 > 0 ? start1 : start2)] = { position, numSamples, numChannels, side.getNumChannels() > 0 };
        headerFifo.finishedWrite (1);
    }

    // Message thread: stops draining, takes everything still in the FIFOs and returns the capture.
    std::shared_ptr<CapturedAudio> stop()
    {
        stopThread (2000);
        drain();
        auto result = std::move (audio);
        if (result != nullptr && ! sideChainHeard)
            result->accompaniment.clear();
        return result;
    }

    void discard()
    {
        stopThread (2000);
        audio.reset();
    }

    int64 getCapturedSamples() const noexcept { return capturedSamples; }
    int getDroppedBlocks() const noexcept { return droppedBlocks; }

private:
    struct Header
    {
        int64 position = 0;
        int numSamples = 0, numChannels = 0;
        bool hasSideChain = false;
    };

    void run() override
    {
        while (! threadShouldExit())
        {
            drain();
            wait (5);
        }
    }

    void drain()
    {
        if (audio == nullptr)
            return;
        while (headerFifo.getNumReady() > 0)
        {
            int start1, size1, start2, size2;
            headerFifo.prepareToRead (1, start1, size1, start2, size2);
            const auto header = headers[(size_t) (size1 > 0 ? start1 : start2)];
            headerFifo.finishedRead (1);

            sampleFifo.prepareToRead (header.numSamples, start1, size1, start2, size2);
            store (header, start1, size1, 0);
            if (size2 > 0)
                store (header, start2, size2, size1);
            sampleFifo.finishedRead (size1 + size2);
            if (! audio->covered.empty() || audio->length() > 0)
                capturedSamples = std::accumulate (audio->covered.begin(), audio->covered.end(), (int64) 0,
                                                   [] (int64 sum, Range<int64> r) { return sum + r.getLength(); });
        }
    }

    // Writes ring samples [ringStart, ringStart + count) to the timeline at header.position + offset.
    void store (const Header& header, int ringStart, int count, int offset)
    {
        auto& a = *audio;
        const auto position = header.position + offset;
        if (a.vocal.empty())
        {
            a.vocal.assign ((size_t) header.numChannels, {});
            a.originSample = position;
        }
        const auto maxSpan = (int64) (maxCaptureSpanSeconds * a.sampleRate);
        if (position < a.originSample)
        {
            const auto grow = a.originSample - position;
            if (a.length() + grow > maxSpan)
                return;
            for (auto& channel : a.vocal)
                channel.insert (channel.begin(), (size_t) grow, 0.0f);
            a.accompaniment.insert (a.accompaniment.begin(), (size_t) grow, 0.0f);
            a.originSample = position;
        }
        const auto end = position + count - a.originSample;
        if (end > maxSpan)
            return;
        if (end > a.length())
        {
            for (auto& channel : a.vocal)
                channel.resize ((size_t) end, 0.0f);
            a.accompaniment.resize ((size_t) end, 0.0f);
        }
        const auto index = (size_t) (position - a.originSample);
        for (size_t c = 0; c < a.vocal.size(); ++c)
            std::copy_n (vocalRings[c].data() + ringStart, count, a.vocal[c].data() + index);
        std::copy_n (sideRing.data() + ringStart, count, a.accompaniment.data() + index);
        if (header.hasSideChain && ! sideChainHeard)
            sideChainHeard = FloatVectorOperations::findMaximum (sideRing.data() + ringStart, count) > 1.0e-6f
                             || FloatVectorOperations::findMinimum (sideRing.data() + ringStart, count) < -1.0e-6f;
        addRange (a.covered, { position, position + count });
    }

    static constexpr int headerCapacity = 1 << 15;
    int sampleCapacity = 0;
    AbstractFifo sampleFifo { 1 }, headerFifo { 1 };
    std::array<std::vector<float>, maxVocalChannels> vocalRings;
    std::vector<float> sideRing;
    std::vector<Header> headers;

    std::shared_ptr<CapturedAudio> audio; // drain thread while running, message thread otherwise
    bool sideChainHeard = false;
    std::atomic<int64> capturedSamples { 0 };
    std::atomic<int> droppedBlocks { 0 };
};

//==============================================================================
// Background work; onFinished runs on the message thread (in update()) unless the capture changed since.
class CaptureSession::Job final : public ThreadPoolJob
{
public:
    Job (int generationIn, std::function<void()> workIn, std::function<void()> onFinishedIn)
        : ThreadPoolJob ("Shizuku Tune capture job"), generation (generationIn), work (std::move (workIn)), onFinished (std::move (onFinishedIn))
    {
    }

    JobStatus runJob() override
    {
        work();
        finished = true;
        return jobHasFinished;
    }

    const int generation;
    std::atomic<bool> finished { false };
    std::function<void()> work, onFinished;
};

//==============================================================================
CaptureSession::CaptureSession() : recorder (std::make_unique<Recorder>())
{
    startTimer (100);
}

CaptureSession::~CaptureSession()
{
    stopTimer();
    recorder->discard();
    cancelJobs();
}

void CaptureSession::cancelJobs()
{
    jobPool.removeAllJobs (true, -1);
    jobs.clear();
}

File CaptureSession::getCaptureRoot()
{
    const auto overridden = SystemStats::getEnvironmentVariable ("SHIZUKU_TUNE_CAPTURE_DIR", {});
    if (overridden.isNotEmpty())
        return File (overridden);
   #if JUCE_MAC
    return File::getSpecialLocation (File::userApplicationDataDirectory).getChildFile ("Application Support/ShizukuTune/captures");
   #else
    return File::getSpecialLocation (File::userApplicationDataDirectory).getChildFile ("ShizukuTune/captures");
   #endif
}

File CaptureSession::captureDirectory() const
{
    return getCaptureRoot().getChildFile (captureId);
}

void CaptureSession::prepare (double sampleRate, int)
{
    const bool changed = ! exactlyEqual (hostSampleRate.exchange (sampleRate), sampleRate);
    if (state != State::capturing && state != State::armed)
        recorder->allocate (sampleRate);
    if (changed)
        sendChangeMessage(); // the editor shows whether the capture's sample rate still matches
}

void CaptureSession::process (AudioBuffer<float>& mainIO, const AudioBuffer<float>& sideChain,
                              const Optional<AudioPlayHead::PositionInfo>& position, bool nonRealtime) noexcept
{
    const auto timeInSamples = position.hasValue() ? position->getTimeInSamples() : Optional<int64>();
    const bool playing = position.hasValue() && position->getIsPlaying() && timeInSamples.hasValue();

    auto current = state.load();
    if (current == State::armed || current == State::capturing)
    {
        ++recordersInside;
        current = state.load();
        if (current == State::armed && playing)
        {
            auto expected = State::armed;
            state.compare_exchange_strong (expected, State::capturing);
            current = state.load();
        }
        if (current == State::capturing)
        {
            if (playing)
                recorder->push (*timeInSamples, mainIO, sideChain, nonRealtime);
            else if (position.hasValue())
                transportStopped = true;
        }
        --recordersInside;
        return; // input passes through while waiting / capturing
    }

    if (! playing)
        return;
    const auto data = std::atomic_load (&playback);
    if (data == nullptr || data->corrected.empty() || ! exactlyEqual (data->sampleRate, hostSampleRate.load()))
        return;

    const auto blockStart = *timeInSamples;
    const auto block = Range<int64>::withStartAndLength (blockStart, mainIO.getNumSamples());
    const auto& ranges = data->replaced;
    auto it = std::upper_bound (ranges.begin(), ranges.end(), blockStart, [] (int64 t, const Range<int64>& r) { return t < r.getEnd(); });
    const auto numCorrected = data->corrected.size();
    for (; it != ranges.end() && it->getStart() < block.getEnd(); ++it)
    {
        const auto overlap = block.getIntersectionWith (*it);
        if (overlap.isEmpty())
            continue;
        const auto from = (size_t) (overlap.getStart() - data->originSample);
        const auto to = (int) (overlap.getStart() - blockStart);
        for (int c = 0; c < mainIO.getNumChannels(); ++c)
        {
            const auto& source = data->corrected[(size_t) c % numCorrected];
            if (from + (size_t) overlap.getLength() <= source.size())
                FloatVectorOperations::copy (mainIO.getWritePointer (c) + to, source.data() + from, (int) overlap.getLength());
        }
    }
}

//==============================================================================
void CaptureSession::arm()
{
    // Make sure no audio thread is in the recorder before it is (re)started.
    state = State::idle;
    while (recordersInside.load() > 0)
        Thread::yield();
    ++generation;
    correctionRunning = correctionPending = false;
    restoredRenderValid = false;
    publishPlayback (nullptr);
    if (hostSampleRate.load() <= 0.0)
        hostSampleRate = 48000.0;
    recorder->allocate (hostSampleRate);
    recorder->start (hostSampleRate);
    transportStopped = false;
    lastCapturedSamples = 0;
    lastCaptureActivityMs = Time::getMillisecondCounter();
    detailText = {};
    state = State::armed;
    sendChangeMessage();
}

void CaptureSession::cancelArm()
{
    if (state != State::armed)
        return;
    state = State::idle;
    while (recordersInside.load() > 0)
        Thread::yield();
    recorder->discard();
    state = captured != nullptr ? State::analysing : State::idle;
    if (captured != nullptr)
        startAnalysis (captured, false);
    sendChangeMessage();
}

void CaptureSession::finishCapture()
{
    const auto previous = state.load();
    if (previous != State::capturing && previous != State::armed)
        return;
    state = State::analysing;
    while (recordersInside.load() > 0)
        Thread::yield();
    auto audio = recorder->stop();
    const int dropped = recorder->getDroppedBlocks();
    if (audio == nullptr || audio->length() == 0)
    {
        state = captured != nullptr ? State::analysing : State::idle;
        if (captured != nullptr)
            startAnalysis (captured, false);
        detailText = String::fromUTF8 (u8"何も取り込まれませんでした。");
        sendChangeMessage();
        return;
    }
    captureId = Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S-") + String::toHexString (Random::getSystemRandom().nextInt()).paddedLeft ('0', 8);
    captured = std::move (audio);
    latestRender.reset(); // a new take: everything is rendered again
    detailText = dropped > 0 ? String::fromUTF8 (u8"取り込み中に処理が追いつかず ") + String (dropped) + String::fromUTF8 (u8" ブロック欠けました。取り込み直してください。")
                             : String();
    writeCaptureInfo();
    writeNoteEdits(); // the edits stay (found again by time in the new capture)
    startAnalysis (captured, true);
}

double CaptureSession::getCapturedSeconds() const noexcept
{
    const auto rate = state == State::capturing || state == State::armed ? hostSampleRate.load() : (captured != nullptr ? captured->sampleRate : 0.0);
    const auto samples = state == State::capturing || state == State::armed ? recorder->getCapturedSamples()
                                                                            : (captured != nullptr ? std::accumulate (captured->covered.begin(), captured->covered.end(), (int64) 0, [] (int64 s, Range<int64> r) { return s + r.getLength(); }) : 0);
    return rate > 0.0 ? (double) samples / rate : 0.0;
}

String CaptureSession::getRangeText() const
{
    if (captured == nullptr || captured->covered.empty())
        return {};
    const auto rate = captured->sampleRate;
    String text = String::fromUTF8 (u8"取り込み範囲 ") + formatTime ((double) captured->covered.front().getStart() / rate)
                  + String::fromUTF8 (u8"〜") + formatTime ((double) captured->covered.back().getEnd() / rate);
    if (captured->covered.size() > 1)
        text << String::fromUTF8 (u8"（") << (int) captured->covered.size() << String::fromUTF8 (u8" か所に分かれています）");
    text << (captured->accompaniment.empty() ? String::fromUTF8 (u8"、伴奏なし") : String::fromUTF8 (u8"、伴奏あり"));
    return text;
}

bool CaptureSession::sampleRateMismatch() const noexcept
{
    return captured != nullptr && hostSampleRate.load() > 0.0 && ! exactlyEqual (captured->sampleRate, hostSampleRate.load());
}

//==============================================================================
void CaptureSession::startAnalysis (std::shared_ptr<const CapturedAudio> audio, bool saveAudio)
{
    state = State::analysing;
    const auto directory = captureDirectory();
    auto result = std::make_shared<std::shared_ptr<TrackAnalysisResult>>();
    auto error = std::make_shared<String>();
    jobs.push_back (std::make_unique<Job> (
        generation,
        [audio, saveAudio, directory, result, error]
        {
            try
            {
                if (saveAudio)
                {
                    directory.createDirectory();
                    writeWavFloat32 (directory.getChildFile ("vocal.wav").getFullPathName().toStdString(), audio->vocal, audio->sampleRate);
                    if (! audio->accompaniment.empty())
                        writeWavFloat32 (directory.getChildFile ("accompaniment.wav").getFullPathName().toStdString(), { audio->accompaniment },
                                         audio->sampleRate);
                }
                WavData vocal;
                vocal.sampleRate = audio->sampleRate;
                vocal.numChannels = (int) audio->vocal.size();
                vocal.channels = audio->vocal;
                auto output = std::make_shared<TrackAnalysisResult>();
                output->vocalTrackName = "Input";
                output->accompanimentTrackName = audio->accompaniment.empty() ? String() : String ("Side chain");
                output->timelineStartSeconds = (double) audio->originSample / audio->sampleRate;
                output->timelineEndSeconds = (double) (audio->originSample + audio->length()) / audio->sampleRate;
                output->analysis = analyseSong (vocal.mixToMono(), audio->sampleRate, audio->accompaniment,
                                                audio->accompaniment.empty() ? 0.0 : audio->sampleRate);
                if (directory.isDirectory())
                    directory.getChildFile ("analysis.json").replaceWithText (String::fromUTF8 (toJson (output->analysis, true).c_str()));
                *result = std::move (output);
            }
            catch (const std::exception& e)
            {
                *error = e.what();
            }
        },
        [this, result, error]
        {
            if (*result == nullptr)
            {
                detailText = String::fromUTF8 (u8"解析できませんでした: ") + *error;
                state = std::atomic_load (&playback) != nullptr ? State::ready : State::idle;
                return;
            }
            latestResult = *result;
            startCorrection();
        }));
    jobPool.addJob (jobs.back().get(), false);
    sendChangeMessage();
}

void CaptureSession::startCorrection()
{
    if (latestResult == nullptr || captured == nullptr)
    {
        sendChangeMessage();
        return;
    }
    auto targets = std::make_shared<ReferenceTargets> (references.applyTo (*latestResult, referenceChoice));
    referenceTargets = targets;
    automaticCorrection = std::make_shared<CorrectionCurve> (planCorrection (targets->analysis, correctionSettings));
    automaticTargets = std::make_shared<std::vector<NoteWithKey>> (targets->analysis.notes);
    startRender();
}

void CaptureSession::startRender()
{
    if (referenceTargets == nullptr || automaticCorrection == nullptr || captured == nullptr || latestResult == nullptr)
    {
        sendChangeMessage();
        return;
    }
    const auto edited = planEditedCorrection (*referenceTargets, correctionSettings, getNoteEdits(), latestResult->timelineStartSeconds,
                                              automaticCorrection, automaticTargets);
    latestCorrection = edited.curve;
    latestTargets = edited.notes;
    if (correctionRunning)
    {
        correctionPending = true; // rendered when the running job finishes
        sendChangeMessage();
        return;
    }
    correctionPending = false;

    size_t numCorrected = 0, numEdited = 0;
    for (const auto& note : edited.curve->notes)
    {
        numCorrected += note.corrected ? 1 : 0;
        numEdited += note.edited ? 1 : 0;
    }
    auto summary = String (numCorrected) + " / " + String ((int) edited.curve->notes.size()) + String::fromUTF8 (u8" 音を補正（強さ ")
                   + String (roundToInt (correctionSettings.strength * 100.0)) + String::fromUTF8 (u8"%、しきい値 ")
                   + String (roundToInt (correctionSettings.thresholdCents)) + String::fromUTF8 (u8" セント）");
    if (numEdited > 0)
        summary << String::fromUTF8 (u8"、手直し ") << (int) numEdited << String::fromUTF8 (u8" 音");
    if (edited.unmatchedEdits + edited.unmatchedStructure > 0)
        summary << String::fromUTF8 (u8"、音が見つからない手直し ") << (int) (edited.unmatchedEdits + edited.unmatchedStructure);

    if (restoredRenderValid)
    {
        // The saved corrected vocal was made with these settings, references and edits: keep playing it.
        restoredRenderValid = false;
        detailText = summary;
        state = State::ready;
        sendChangeMessage();
        return;
    }

    correctionRunning = true;
    state = State::analysing; // the previous corrected vocal keeps playing until the new one is published
    const auto audio = captured;
    const auto directory = captureDirectory();
    const auto hint = medianVoicedFrequency (referenceTargets->analysis.pitch);
    const auto key = renderKey (correctionSettings, referenceChoice, references.getPaths(), getNoteEdits());
    const auto automaticCurve = automaticCorrection;
    const auto editedCurve = edited.curve;
    const auto previous = latestRender;
    auto rendered = std::make_shared<std::shared_ptr<CapturePlayback>>();
    auto layered = std::make_shared<std::shared_ptr<const LayeredRender>>();
    jobs.push_back (std::make_unique<Job> (
        generation,
        [audio, automaticCurve, editedCurve, previous, hint, directory, rendered, layered]
        {
            auto render = std::make_shared<const LayeredRender> (renderLayered (audio->vocal, audio->sampleRate, 0.0, hint, automaticCurve, editedCurve,
                                                                                previous.get()));
            auto data = std::make_shared<CapturePlayback>();
            data->sampleRate = audio->sampleRate;
            data->originSample = audio->originSample;
            data->corrected = render->output();
            data->replaced = differingRanges (*audio, data->corrected);
            if (directory.isDirectory())
            {
                try
                {
                    writeWavFloat32 (directory.getChildFile ("corrected.wav").getFullPathName().toStdString(), data->corrected, data->sampleRate);
                }
                catch (const std::exception&)
                {
                }
            }
            *rendered = std::move (data);
            *layered = std::move (render);
        },
        [this, rendered, layered, summary, key]
        {
            correctionRunning = false;
            publishPlayback (*rendered);
            latestRender = *layered;
            renderedWith = key;
            writeCaptureInfo();
            detailText = summary;
            state = State::ready;
            if (correctionPending)
                startRender();
        }));
    jobPool.addJob (jobs.back().get(), false);
    sendChangeMessage();
}

void CaptureSession::noteEditsChanged()
{
    writeNoteEdits();
    startRender();
}

void CaptureSession::writeNoteEdits() const
{
    if (captureId.isEmpty())
        return;
    const auto directory = captureDirectory();
    if (! directory.createDirectory())
        return;
    auto document = getNoteEdits();
    document.file = "vocal.wav";
    directory.getChildFile ("edits.json").replaceWithText (String::fromUTF8 (toJson (document).c_str()));
}

bool CaptureSession::getAuditionAudio (double startSeconds, double endSeconds, bool corrected, AuditionAudio& audio) const
{
    if (captured == nullptr || captured->length() == 0)
        return false;
    const auto data = std::atomic_load (&playback);
    const bool useCorrected = corrected && data != nullptr && ! data->corrected.empty();
    const double rate = captured->sampleRate;
    const auto first = jlimit<int64> (0, captured->length(), (int64) std::floor (startSeconds * rate) - captured->originSample);
    const auto last = jlimit<int64> (first, captured->length(), (int64) std::ceil (endSeconds * rate) - captured->originSample);
    if (last <= first)
        return false;
    audio.sampleRate = rate;
    audio.channels.clear();
    for (const auto& channel : useCorrected ? data->corrected : captured->vocal)
        audio.channels.emplace_back (channel.begin() + first, channel.begin() + last);
    return true;
}

void CaptureSession::publishPlayback (std::shared_ptr<const CapturePlayback> next)
{
    const ScopedLock lock (playbackWriteLock);
    retiredPlayback.push_back (std::atomic_load (&playback));
    std::atomic_store (&playback, std::move (next));
}

void CaptureSession::update()
{
    {
        // Free retired playback data no audio thread holds any more (the audio thread never frees memory).
        const ScopedLock lock (playbackWriteLock);
        retiredPlayback.erase (std::remove_if (retiredPlayback.begin(), retiredPlayback.end(),
                                               [] (const auto& data) { return data == nullptr || data.use_count() == 1; }),
                               retiredPlayback.end());
    }

    // Test hook for host tools without a GUI (tools/AuCaptureCheckMain.mm): arm once the host has prepared us.
    if (! armedFromEnvironment && state == State::idle && captureId.isEmpty() && hostSampleRate.load() > 0.0
        && SystemStats::getEnvironmentVariable ("SHIZUKU_TUNE_CAPTURE_ARM_ON_START", {}) == "1")
    {
        armedFromEnvironment = true;
        arm();
    }

    if (state == State::capturing)
    {
        const auto samples = recorder->getCapturedSamples();
        const auto now = Time::getMillisecondCounter();
        if (samples != lastCapturedSamples)
        {
            lastCapturedSamples = samples;
            lastCaptureActivityMs = now;
        }
        if (transportStopped || now - lastCaptureActivityMs > hostSilentTimeoutMs)
            finishCapture();
        else
            sendChangeMessage(); // captured seconds
    }

    for (size_t i = 0; i < jobs.size();)
    {
        auto& job = jobs[i];
        if (! job->finished)
        {
            ++i;
            continue;
        }
        jobPool.waitForJobToFinish (job.get(), -1);
        auto finishedJob = std::move (job);
        jobs.erase (jobs.begin() + (std::ptrdiff_t) i);
        if (finishedJob->generation == generation)
            finishedJob->onFinished();
        sendChangeMessage();
    }
}

//==============================================================================
void CaptureSession::setCorrectionSettings (const CorrectionSettings& newSettings)
{
    correctionSettings = newSettings;
    writeCaptureInfo();
    startCorrection();
}

String CaptureSession::addReferenceMelody (const File& file)
{
    const auto error = references.add (file);
    if (error.isEmpty())
    {
        writeCaptureInfo();
        startCorrection();
    }
    return error;
}

void CaptureSession::clearReferenceMelodies()
{
    references.clear();
    referenceTargets.reset();
    writeCaptureInfo();
    startCorrection();
}

String CaptureSession::getReferenceDescription() const
{
    return references.describe (referenceTargets.get(), referenceChoice, true);
}

void CaptureSession::setReferenceChoice (int choice)
{
    referenceChoice = choice;
    writeCaptureInfo();
    startCorrection();
}

std::vector<ReferenceCandidateInfo> CaptureSession::getReferenceCandidates() const
{
    return referenceTargets != nullptr ? referenceTargets->candidates : std::vector<ReferenceCandidateInfo> {};
}

String CaptureSession::getStatusText() const
{
    String text;
    switch (state.load())
    {
        case State::idle:
            text = String::fromUTF8 (u8"「取り込み」を押してから、ホストで曲を再生してください（伴奏はサイドチェーンで渡せます）。");
            break;
        case State::armed:
            text = String::fromUTF8 (u8"待機中: ホストで曲を再生すると取り込みを始めます。");
            break;
        case State::capturing:
            text = String::fromUTF8 (u8"取り込み中 ") + String (getCapturedSeconds(), 1) + String::fromUTF8 (u8" 秒（止めると解析を始めます）");
            break;
        case State::analysing:
            text = correctionRunning ? String::fromUTF8 (u8"補正した歌を作っています…") : String::fromUTF8 (u8"解析中…");
            break;
        case State::ready:
            text = String::fromUTF8 (u8"準備完了: 再生すると補正した歌が鳴ります。");
            break;
    }
    if (sampleRateMismatch())
        text = String::fromUTF8 (u8"サンプルレートが取り込んだ時（") + String (captured->sampleRate, 0)
               + String::fromUTF8 (u8" Hz）と違うため補正していません。取り込み直してください。");
    if (detailText.isNotEmpty())
        text << "  " << detailText;
    return text;
}

//==============================================================================
void CaptureSession::writeCaptureInfo() const
{
    if (captureId.isEmpty())
        return;
    const auto directory = captureDirectory();
    if (! directory.createDirectory())
        return;
    auto object = std::make_unique<DynamicObject>();
    object->setProperty ("formatVersion", 1);
    object->setProperty ("strength", correctionSettings.strength);
    object->setProperty ("thresholdCents", correctionSettings.thresholdCents);
    object->setProperty ("referenceChoice", referenceChoice);
    Array<var> paths;
    for (const auto& path : references.getPaths())
        paths.add (path);
    object->setProperty ("references", paths);
    if (captured != nullptr)
    {
        object->setProperty ("sampleRate", captured->sampleRate);
        object->setProperty ("originSample", captured->originSample);
        object->setProperty ("covered", rangesToVar (captured->covered));
        object->setProperty ("hasAccompaniment", ! captured->accompaniment.empty());
    }
    if (! renderedWith.isVoid())
        object->setProperty ("correctedWith", renderedWith);
    directory.getChildFile ("capture.json").replaceWithText (JSON::toString (var (object.release())));
}

ValueTree CaptureSession::saveState() const
{
    ValueTree tree (stateType);
    tree.setProperty (idProperty, captureId, nullptr);
    tree.setProperty (strengthProperty, correctionSettings.strength, nullptr);
    tree.setProperty (thresholdProperty, correctionSettings.thresholdCents, nullptr);
    tree.setProperty (choiceProperty, referenceChoice, nullptr);
    if (hasNoteEdits (getNoteEdits()))
        tree.setProperty (editsProperty, String::fromUTF8 (toJson (getNoteEdits()).c_str()), nullptr);
    return tree;
}

void CaptureSession::restoreState (const ValueTree& tree)
{
    if (! tree.hasType (stateType))
        return;
    state = State::idle;
    while (recordersInside.load() > 0)
        Thread::yield();
    recorder->discard();
    ++generation;
    correctionRunning = correctionPending = false;
    publishPlayback (nullptr);
    captured.reset();
    latestResult.reset();
    latestCorrection.reset();
    latestTargets.reset();
    automaticCorrection.reset();
    automaticTargets.reset();
    latestRender.reset();
    referenceTargets.reset();
    references.clear();
    renderedWith = var();
    restoredRenderValid = false;
    state = State::idle;

    correctionSettings.strength = tree.getProperty (strengthProperty, correctionSettings.strength);
    correctionSettings.thresholdCents = tree.getProperty (thresholdProperty, correctionSettings.thresholdCents);
    referenceChoice = tree.getProperty (choiceProperty, referenceAuto);
    captureId = tree.getProperty (idProperty).toString();
    detailText = {};
    // Hand edits: from the plug-in state, else from the capture's edits.json.
    const auto directory = captureDirectory();
    if (tree.hasProperty (editsProperty))
        noteEditHistory.reset (noteEditsFromText (tree.getProperty (editsProperty).toString()));
    else if (captureId.isNotEmpty())
        noteEditHistory.reset (noteEditsFromText (directory.getChildFile ("edits.json").loadFileAsString()));
    else
        noteEditHistory.reset ({});
    if (captureId.isEmpty())
    {
        sendChangeMessage();
        return;
    }

    const auto info = JSON::parse (directory.getChildFile ("capture.json"));
    if (! info.isObject())
    {
        detailText = String::fromUTF8 (u8"保存した取り込みが見つかりません。取り込み直してください。");
        sendChangeMessage();
        return;
    }
    StringArray missing;
    if (const auto* paths = info["references"].getArray())
        for (const auto& path : *paths)
            if (references.add (File (path.toString())).isNotEmpty())
                missing.add (File (path.toString()).getFileName());
    if (! missing.isEmpty())
        detailText = String::fromUTF8 (u8"正解を読み込めませんでした: ") + missing.joinIntoString (", ");

    const auto vocalFile = directory.getChildFile ("vocal.wav");
    if (! vocalFile.existsAsFile())
    {
        sendChangeMessage();
        return;
    }
    try
    {
        auto audio = std::make_shared<CapturedAudio>();
        const auto vocal = readWav (vocalFile.getFullPathName().toStdString());
        audio->sampleRate = vocal.sampleRate;
        audio->vocal = vocal.channels;
        audio->originSample = (int64) info["originSample"];
        audio->covered = rangesFromVar (info["covered"]);
        if (audio->covered.empty())
            audio->covered.push_back ({ audio->originSample, audio->originSample + audio->length() });
        const auto accompanimentFile = directory.getChildFile ("accompaniment.wav");
        if (accompanimentFile.existsAsFile())
            audio->accompaniment = readWav (accompanimentFile.getFullPathName().toStdString()).mixToMono();
        audio->accompaniment.resize (audio->accompaniment.empty() ? 0 : (size_t) audio->length(), 0.0f);
        captured = audio;

        // Play the saved corrected vocal right away when it was made with the current settings and references
        // (an offline bounce may start before the analysis has run again for the view).
        const auto correctedFile = directory.getChildFile ("corrected.wav");
        const auto savedKey = info["correctedWith"];
        if (correctedFile.existsAsFile() && ! savedKey.isVoid()
            && JSON::toString (savedKey) == JSON::toString (renderKey (correctionSettings, referenceChoice, references.getPaths(), getNoteEdits())))
        {
            auto data = std::make_shared<CapturePlayback>();
            const auto corrected = readWav (correctedFile.getFullPathName().toStdString());
            if (corrected.channels.size() == audio->vocal.size() && corrected.channels[0].size() == (size_t) audio->length())
            {
                data->sampleRate = audio->sampleRate;
                data->originSample = audio->originSample;
                data->corrected = corrected.channels;
                data->replaced = differingRanges (*audio, data->corrected);
                publishPlayback (data);
                renderedWith = savedKey;
                restoredRenderValid = true;
            }
        }
    }
    catch (const std::exception& e)
    {
        captured.reset();
        detailText = String::fromUTF8 (u8"保存した取り込みを読めませんでした: ") + e.what();
        sendChangeMessage();
        return;
    }
    startAnalysis (captured, false);
}

} // namespace shizuku
