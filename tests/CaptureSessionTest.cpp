// Offline host simulation for the capture mode: the plug-in's AudioProcessor is created directly (not bound to
// ARA), a synthetic detuned vocal (main input) and its accompaniment (side chain) are played through processBlock
// with an advancing playhead, the capture is analysed and corrected, and the song is played again. Checks:
// - while capturing, the output is the input;
// - the replayed output equals renderCorrection of the captured vocal with the same analysis / settings as
//   shizuku-correct (sample-exact: no timeline shift), and the input outside the corrected regions;
// - with a reference melody (a guide .wav of the in-tune melody) added in the plug-in, the output equals
//   shizuku-correct --reference guide.wav;
// - a jump back (the second pass over a range overwrites the first), a capture starting at a non-zero position,
//   no side chain, a sample-rate change (no correction played) and getStateInformation / setStateInformation
//   (a new instance plays the same corrected vocal without capturing again).
// - hand edits through the plug-in (a target, keep, a split, a merge, and two notes picked with Cmd + click and
//   dragged up together in the editor's pitch view): the output equals shizuku-correct --edits (automatic render
//   with the edited spans rendered again), is bit-identical to the automatic correction outside the edited spans,
//   one undo takes back the whole drag, and the edits survive getStateInformation / setStateInformation and a new
//   capture (found again by time);
// - previews: with the transport stopped, clicking a note in the editor adds that note's corrected audio to the
//   output; the preview stops when the host starts playing; processBlock does not allocate.
// Optional: --snapshot <file.png> draws the editor (capture mode, after the capture) to a PNG;
// --snapshot-edit <file.png> draws it with the edits, a multi-note selection and the playhead (and <file>-note.png
// with one note selected).
// Optional: --write <dir> writes vocal.wav / accompaniment.wav / plugin_out.wav for a comparison with the
// shizuku-correct command-line tool (see tools/compare_capture.sh).
// Exits non-zero when any check fails.

#include "../src/plugin/CaptureSession.h"
#include "../src/plugin/ShizukuPluginEditor.h"
#include "../src/plugin/ShizukuPluginProcessor.h"
#include "../tools/CorrectionSetup.h"
#include "../tools/SyntheticSignals.h"

#include "PitchCorrection.h"
#include "WavFile.h"

#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#elif JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX // keep std::min / std::max usable
 #endif
 #include <windows.h>
 #include <stdlib.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

// Allocation counter for the audio-thread checks (counts only while countAllocations is set on this thread).
namespace
{
thread_local bool countAllocations = false;
std::atomic<int> allocationsCounted { 0 };
} // namespace

void* operator new (std::size_t size)
{
    if (countAllocations)
        ++allocationsCounted;
    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t size)
{
    if (countAllocations)
        ++allocationsCounted;
    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

using namespace shizuku;
using namespace juce;

namespace
{
constexpr double sampleRate = 44100.0;
constexpr int blockSize = 512;
int failures = 0;

// Dispatches the JUCE message queue for about the given time (the plug-in builds JUCE without modal loops, so
// MessageManager::runDispatchLoopUntil is unavailable).
void runMessageLoop (double seconds)
{
   #if JUCE_MAC
    CFRunLoopRunInMode (kCFRunLoopDefaultMode, seconds, true);
   #elif JUCE_WINDOWS
    const auto end = Time::getMillisecondCounterHiRes() + seconds * 1000.0;
    do
    {
        MSG message;
        while (PeekMessage (&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage (&message);
            DispatchMessage (&message);
        }
        Thread::sleep (1);
    } while (Time::getMillisecondCounterHiRes() < end);
   #else
    #error "runMessageLoop: unsupported platform"
   #endif
}

void setEnvironmentVariable (const char* name, const String& value)
{
   #if JUCE_WINDOWS
    _putenv_s (name, value.toRawUTF8());
   #else
    setenv (name, value.toRawUTF8(), 1);
   #endif
}

void check (bool condition, const char* what)
{
    std::printf ("  %s: %s\n", condition ? "ok  " : "FAIL", what);
    if (! condition)
        ++failures;
}

class TestPlayHead final : public AudioPlayHead
{
public:
    Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (playing);
        info.setTimeInSamples (timeInSamples);
        info.setTimeInSeconds ((double) timeInSamples / sampleRate);
        return info;
    }
    bool playing = false;
    int64 timeInSamples = 0;
};

struct Song
{
    std::vector<float> vocal, accompaniment; // timeline from sample 0
};

Song makeSong()
{
    const MusicalKey key { 7, KeyMode::major };
    Song song;
    song.accompaniment = synth::renderAccompaniment ({ synth::diatonicSection (key), synth::diatonicSection (key) }, 2.0, sampleRate);
    song.vocal = synth::renderVocal (synth::detunedMelody (key, 60, 0.5), synth::VoiceTimbre::voiceLike, sampleRate,
                                     (double) song.accompaniment.size() / sampleRate);
    return song;
}

// The plug-in with a playhead; stereo main bus (vocal on both channels), optional stereo side chain.
struct Host
{
    explicit Host (bool withSideChain)
    {
        processor = std::make_unique<ShizukuPluginProcessor>();
        auto layout = processor->getBusesLayout();
        if (layout.inputBuses.size() > 1)
            layout.inputBuses.getReference (1) = withSideChain ? AudioChannelSet::stereo() : AudioChannelSet::disabled();
        processor->setBusesLayout (layout);
        processor->setPlayHead (&playHead);
        processor->setNonRealtime (true);
        processor->prepareToPlay (sampleRate, blockSize);
    }

    ~Host() { processor->releaseResources(); }

    // Plays timeline samples [from, to) and returns the main output (channel 0) for that range.
    std::vector<float> play (const Song& song, int64 from, int64 to, bool playing = true)
    {
        std::vector<float> output;
        const int numChannels = processor->getTotalNumInputChannels();
        AudioBuffer<float> buffer (jmax (numChannels, processor->getTotalNumOutputChannels()), blockSize);
        MidiBuffer midi;
        playHead.playing = playing;
        for (auto position = from; position < to; position += blockSize)
        {
            const int n = (int) jmin<int64> (blockSize, to - position);
            buffer.setSize (buffer.getNumChannels(), n, false, false, true);
            buffer.clear();
            const auto sample = [position] (const std::vector<float>& signal, int i)
            { return position + i < (int64) signal.size() ? signal[(size_t) (position + i)] : 0.0f; };
            for (int i = 0; i < n; ++i)
            {
                buffer.setSample (0, i, sample (song.vocal, i));
                buffer.setSample (1, i, sample (song.vocal, i));
                if (numChannels >= 4)
                {
                    buffer.setSample (2, i, sample (song.accompaniment, i));
                    buffer.setSample (3, i, sample (song.accompaniment, i));
                }
            }
            playHead.timeInSamples = position;
            countAllocations = true;
            processor->processBlock (buffer, midi);
            countAllocations = false;
            output.insert (output.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + n);
        }
        return output;
    }

    // The host stops: one block with the transport stopped.
    void stop()
    {
        playHead.playing = false;
        AudioBuffer<float> buffer (jmax (processor->getTotalNumInputChannels(), processor->getTotalNumOutputChannels()), blockSize);
        buffer.clear();
        MidiBuffer midi;
        processor->processBlock (buffer, midi);
    }

    // Runs the message loop until the capture session is ready (or the timeout).
    bool waitUntilReady (int timeoutMs = 120000)
    {
        auto& session = processor->getCaptureSession();
        const auto start = Time::getMillisecondCounter();
        while (Time::getMillisecondCounter() - start < (uint32) timeoutMs)
        {
            session.update();
            runMessageLoop (0.02);
            if (session.getState() == CaptureSession::State::ready)
                return true;
        }
        return false;
    }

    TestPlayHead playHead;
    std::unique_ptr<ShizukuPluginProcessor> processor;
};

// What shizuku-correct does for this vocal / accompaniment (no references, default settings).
std::vector<float> expectedCorrection (const std::vector<float>& vocal, const std::vector<float>& accompaniment)
{
    const auto analysis = analyseSong (vocal, sampleRate, accompaniment, accompaniment.empty() ? 0.0 : sampleRate);
    const auto curve = planCorrection (analysis, CorrectionSettings {});
    return renderCorrection ({ vocal, vocal }, sampleRate, curve, 0.0, medianVoicedFrequency (analysis.pitch))[0];
}

double maxDifference (const std::vector<float>& a, const std::vector<float>& b, size_t offsetInB = 0)
{
    double worst = 0.0;
    for (size_t i = 0; i < a.size() && i + offsetInB < b.size(); ++i)
        worst = std::max (worst, (double) std::abs (a[i] - b[i + offsetInB]));
    return worst;
}

double maxDifferenceToInput (const std::vector<float>& a, const std::vector<float>& input, int64 from)
{
    return maxDifference (a, input, (size_t) from);
}

// Shift the playhead reports for the output that matches the expected render best (0 = sample-aligned).
int bestLag (const std::vector<float>& output, const std::vector<float>& expected, int64 from)
{
    int best = 0;
    double bestError = 1.0e9;
    for (int lag = -64; lag <= 64; ++lag)
    {
        double error = 0.0;
        for (size_t i = 0; i < output.size(); i += 7)
        {
            const auto j = (int64) i + from + lag;
            if (j >= 0 && j < (int64) expected.size())
                error += std::abs (output[i] - expected[(size_t) j]);
        }
        if (error < bestError)
        {
            bestError = error;
            best = lag;
        }
    }
    return best;
}
} // namespace

// What shizuku-correct --edits does: the automatic render, with the spans around the edited notes rendered again.
struct EditedExpectation
{
    std::vector<float> output;
    std::vector<TimeSpan> spans;
};

EditedExpectation expectedEditedCorrection (const Song& song, const NoteEditDocument& edits)
{
    const auto directory = File::getSpecialLocation (File::tempDirectory).getChildFile ("shizuku-capture-test-edits");
    directory.createDirectory();
    const auto path = [&] (const char* name) { return directory.getChildFile (name).getFullPathName().toStdString(); };
    writeWavFloat32 (path ("vocal.wav"), { song.vocal, song.vocal }, sampleRate);
    writeWavFloat32 (path ("accompaniment.wav"), { song.accompaniment }, sampleRate);
    directory.getChildFile ("edits.json").replaceWithText (String::fromUTF8 (toJson (edits).c_str()));
    const auto prepared = tools::prepareCorrection (path ("vocal.wav"), path ("accompaniment.wav"), {}, -2, -1.0, false);
    const auto document = loadNoteEdits (path ("edits.json")); // through the file, as shizuku-correct --edits reads it
    const auto edited = tools::applyEdits (prepared, document);
    const CorrectionSettings settings;
    const double hint = medianVoicedFrequency (prepared.analysis.pitch);
    const auto automaticCurve = planCorrection (prepared.analysis, settings);
    const auto automatic = renderCorrection (prepared.vocal.channels, sampleRate, automaticCurve, 0.0, hint);
    const auto curve = planCorrection (edited.analysis, settings, edited.overrides);
    auto render = renderEditedSpans (prepared.vocal.channels, sampleRate, automaticCurve, automatic, curve, 0.0, hint);
    directory.deleteRecursively();
    return { render.output[0], render.spans };
}

// Largest difference outside the given spans (timeline = analysis time).
double maxDifferenceOutside (const std::vector<float>& a, const std::vector<float>& b, const std::vector<TimeSpan>& spans)
{
    double worst = 0.0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
    {
        const double t = (double) i / sampleRate;
        if (std::any_of (spans.begin(), spans.end(), [t] (const TimeSpan& span) { return t >= span.startSeconds - 0.001 && t <= span.endSeconds + 0.001; }))
            continue;
        worst = std::max (worst, (double) std::abs (a[i] - b[i]));
    }
    return worst;
}

MouseEvent mouseEventAt (Component& component, juce::Point<float> position, juce::Point<float> downPosition, ModifierKeys mods, bool dragged)
{
    const auto now = Time::getCurrentTime();
    return MouseEvent (Desktop::getInstance().getMainMouseSource(), position, mods, MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f,
                       &component, &component, now, downPosition, now, 1, dragged);
}

void clickNote (PitchLineView& view, size_t note, ModifierKeys mods = {})
{
    const auto at = view.getNoteBoxCentre (note);
    view.mouseDown (mouseEventAt (view, at, at, mods, false));
    view.mouseUp (mouseEventAt (view, at, at, mods, false));
}

void dragNote (PitchLineView& view, size_t note, int semitones)
{
    const auto at = view.getNoteBoxCentre (note);
    view.mouseDown (mouseEventAt (view, at, at, {}, false));
    for (int step = 1; step <= 4; ++step)
    {
        const auto to = at.translated (0.0f, -view.getSemitoneHeight() * (float) semitones * (float) step / 4.0f);
        view.mouseDrag (mouseEventAt (view, to, at, ModifierKeys (ModifierKeys::leftButtonModifier), true));
    }
    const auto end = at.translated (0.0f, -view.getSemitoneHeight() * (float) semitones);
    view.mouseUp (mouseEventAt (view, end, at, {}, true));
}

double peak (const std::vector<float>& signal)
{
    double worst = 0.0;
    for (const auto value : signal)
        worst = std::max (worst, (double) std::abs (value));
    return worst;
}

int main (int argc, char** argv)
{
    ScopedJuceInitialiser_GUI juce;
    String writeDirectory, snapshotPath, editSnapshotPath;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--write") == 0 && i + 1 < argc)
            writeDirectory = argv[++i];
        else if (std::strcmp (argv[i], "--snapshot") == 0 && i + 1 < argc)
            snapshotPath = argv[++i];
        else if (std::strcmp (argv[i], "--snapshot-edit") == 0 && i + 1 < argc)
            editSnapshotPath = argv[++i];
    }

    const auto captureRoot = File::getSpecialLocation (File::tempDirectory).getChildFile ("shizuku-capture-test");
    captureRoot.deleteRecursively();
    setEnvironmentVariable ("SHIZUKU_TUNE_CAPTURE_DIR", captureRoot.getFullPathName());

    const auto song = makeSong();
    const auto length = (int64) song.vocal.size();
    const auto expected = expectedCorrection (song.vocal, song.accompaniment);
    MemoryBlock savedState;

    std::printf ("capture with side chain, from 0, with a jump back\n");
    {
        Host host (true);
        check (host.processor->getTotalNumInputChannels() == 4, "side-chain bus enabled (2 + 2 input channels)");
        auto& session = host.processor->getCaptureSession();
        session.arm();
        check (session.getState() == CaptureSession::State::armed, "armed after arm()");
        // Play the first 3 s, jump back to 1 s (the second pass overwrites), then play to the end.
        const auto jumpAt = (int64) (3.0 * sampleRate) / blockSize * blockSize;
        const auto jumpTo = (int64) (1.0 * sampleRate) / blockSize * blockSize;
        const auto firstPass = host.play (song, 0, jumpAt);
        check (session.getState() == CaptureSession::State::capturing, "capturing while the host plays");
        const auto secondPass = host.play (song, jumpTo, length);
        check (maxDifferenceToInput (firstPass, song.vocal, 0) == 0.0 && maxDifferenceToInput (secondPass, song.vocal, jumpTo) == 0.0,
               "input passes through unchanged while capturing");
        host.stop();
        host.waitUntilReady();
        check (session.getState() == CaptureSession::State::ready, "analysed and corrected after the transport stopped");
        std::printf ("  status: %s\n  %s\n", session.getStatusText().toRawUTF8(), session.getRangeText().toRawUTF8());
        check (std::abs (session.getCapturedSeconds() - (double) length / sampleRate) < 0.02, "captured length = song length");
        const auto result = session.getLatestResult();
        check (result != nullptr && result->analysis.hasAccompaniment && result->analysis.key.globalKey.tonicPitchClass == 7,
               "key from the side chain (G major)");

        const auto output = host.play (song, 0, length);
        const auto lag = bestLag (output, expected, 0);
        std::printf ("  output vs. shizuku-correct render: lag %d samples, max difference %.3g\n", lag, maxDifference (output, expected));
        check (lag == 0, "no timeline shift");
        check (maxDifference (output, expected) < 1.0e-6, "output = shizuku-correct's correction of the same vocal");
        check (maxDifference (output, song.vocal) > 0.01, "the output is corrected (differs from the input)");

        // Playing from the middle (any block alignment) gives the same samples.
        const auto from = (int64) (2.3 * sampleRate) + 17;
        const auto middle = host.play (song, from, from + (int64) sampleRate);
        check (maxDifference (middle, expected, (size_t) from) < 1.0e-6, "same output when playback starts mid-song");

        if (writeDirectory.isNotEmpty())
        {
            File (writeDirectory).createDirectory();
            const auto path = [&] (const char* name) { return File (writeDirectory).getChildFile (name).getFullPathName().toStdString(); };
            writeWavFloat32 (path ("vocal.wav"), { song.vocal }, sampleRate);
            writeWavFloat32 (path ("accompaniment.wav"), { song.accompaniment }, sampleRate);
            writeWavFloat32 (path ("plugin_out.wav"), { output }, sampleRate);
            std::printf ("  wrote vocal.wav, accompaniment.wav, plugin_out.wav to %s\n", writeDirectory.toRawUTF8());
        }

        if (snapshotPath.isNotEmpty())
        {
            std::unique_ptr<AudioProcessorEditor> editor (host.processor->createEditor());
            runMessageLoop (0.1);
            const auto image = editor->createComponentSnapshot (editor->getLocalBounds());
            FileOutputStream stream { File (snapshotPath) };
            stream.setPosition (0);
            stream.truncate();
            PNGImageFormat().writeImageToStream (image, stream);
            std::printf ("  wrote the editor to %s\n", snapshotPath.toRawUTF8());
        }

        // Settings change: re-rendered with the new strength.
        auto settings = session.getCorrectionSettings();
        settings.strength = 0.5;
        session.setCorrectionSettings (settings);
        host.waitUntilReady();
        const auto half = host.play (song, 0, length);
        const auto analysis = analyseSong (song.vocal, sampleRate, song.accompaniment, sampleRate);
        const auto halfExpected = renderCorrection ({ song.vocal, song.vocal }, sampleRate, planCorrection (analysis, settings), 0.0,
                                                    medianVoicedFrequency (analysis.pitch))[0];
        check (maxDifference (half, halfExpected) < 1.0e-6, "strength 50% re-rendered like shizuku-correct --strength 50");
        settings.strength = 1.0;
        session.setCorrectionSettings (settings);
        host.waitUntilReady();
        host.processor->getStateInformation (savedState);

        // A different sample rate: no correction is played, the editor asks for a new capture.
        host.processor->prepareToPlay (48000.0, blockSize);
        check (session.sampleRateMismatch(), "sample-rate change detected");
        const auto other = host.play (song, 0, (int64) sampleRate);
        check (maxDifferenceToInput (other, song.vocal, 0) == 0.0, "input passes through at another sample rate");
        host.processor->prepareToPlay (sampleRate, blockSize);
    }

    std::printf ("restore from the plug-in state in a new instance\n");
    {
        Host host (true);
        host.processor->setStateInformation (savedState.getData(), (int) savedState.getSize());
        auto& session = host.processor->getCaptureSession();
        check (session.getCaptureId().isNotEmpty(), "capture id restored");
        // The saved corrected vocal is played right away (before the analysis has been run again).
        const auto early = host.play (song, 0, length);
        check (maxDifference (early, expected) < 1.0e-6, "saved corrected vocal played immediately after loading");
        host.waitUntilReady();
        check (session.getLatestResult() != nullptr, "analysis rebuilt for the view");
        const auto output = host.play (song, 0, length);
        check (maxDifference (output, expected) < 1.0e-6, "same output after restoring (no new capture)");
        check (savedState.getSize() < 1024, "plug-in state holds only the id and settings");
    }

    std::printf ("reference melody (guide .wav), as shizuku-correct --reference\n");
    {
        const auto directory = File::getSpecialLocation (File::tempDirectory).getChildFile ("shizuku-capture-test-ref");
        directory.createDirectory();
        const auto path = [&] (const char* name) { return directory.getChildFile (name).getFullPathName().toStdString(); };
        // In tune: the true melody rounded to semitones, sine timbre.
        auto guideNotes = synth::detunedMelody (MusicalKey { 7, KeyMode::major }, 60, 0.5);
        for (auto& note : guideNotes)
            note.midi = std::round (note.midi);
        writeWavFloat32 (path ("guide.wav"), { synth::renderVocal (guideNotes, synth::VoiceTimbre::sine, sampleRate, (double) length / sampleRate) }, sampleRate);
        writeWavFloat32 (path ("vocal.wav"), { song.vocal, song.vocal }, sampleRate);
        writeWavFloat32 (path ("accompaniment.wav"), { song.accompaniment }, sampleRate);
        const auto prepared = tools::prepareCorrection (path ("vocal.wav"), path ("accompaniment.wav"), { tools::parseReferenceArgument (path ("guide.wav")) },
                                                        -2, -1.0, false);
        const auto curve = planCorrection (prepared.analysis, CorrectionSettings {});
        const auto cli = renderCorrection (prepared.vocal.channels, sampleRate, curve, 0.0, medianVoicedFrequency (prepared.analysis.pitch))[0];
        size_t referenceNotes = 0;
        for (const auto& entry : prepared.analysis.notes)
            referenceNotes += entry.hasReferenceTarget ? 1 : 0;

        Host host (true);
        auto& session = host.processor->getCaptureSession();
        session.arm();
        host.play (song, 0, length);
        host.stop();
        host.waitUntilReady();
        check (session.addReferenceMelody (File (path ("guide.wav"))).isEmpty(), "guide added");
        host.waitUntilReady();
        size_t sessionReferenceNotes = 0;
        if (const auto targets = session.getLatestTargets())
            for (const auto& entry : *targets)
                sessionReferenceNotes += entry.hasReferenceTarget ? 1 : 0;
        std::printf ("  notes with a reference target: plug-in %zu, shizuku-correct %zu\n  %s\n", sessionReferenceNotes, referenceNotes,
                     session.getReferenceDescription().toRawUTF8());
        check (referenceNotes > 0 && sessionReferenceNotes == referenceNotes, "same reference targets as shizuku-correct");
        const auto output = host.play (song, 0, length);
        check (maxDifference (output, cli) < 1.0e-6, "output = shizuku-correct --reference guide.wav");
        directory.deleteRecursively();
    }

    std::printf ("capture without side chain, starting at 4 s on the timeline\n");
    {
        Host host (false);
        check (host.processor->getTotalNumInputChannels() == 2, "side-chain bus disabled");
        auto& session = host.processor->getCaptureSession();
        const auto offset = (int64) (4.0 * sampleRate) + 123;
        Song shifted; // the same vocal placed at 4 s on the timeline
        shifted.vocal.assign ((size_t) offset, 0.0f);
        shifted.vocal.insert (shifted.vocal.end(), song.vocal.begin(), song.vocal.end());
        session.arm();
        host.play (shifted, offset, offset + length);
        session.finishCapture(); // the "終了" button
        host.waitUntilReady();
        check (session.getState() == CaptureSession::State::ready, "ready after finishing by hand");
        const auto result = session.getLatestResult();
        check (result != nullptr && ! result->analysis.hasAccompaniment, "no accompaniment: key not detected, scale = nearest semitone");
        check (result != nullptr && std::abs (result->timelineStartSeconds - (double) offset / sampleRate) < 1.0e-9, "timeline start from the playhead");
        const auto expectedNoAccompaniment = expectedCorrection (song.vocal, {});
        const auto output = host.play (shifted, offset, offset + length);
        check (bestLag (output, expectedNoAccompaniment, 0) == 0, "no timeline shift at a non-zero start");
        check (maxDifference (output, expectedNoAccompaniment) < 1.0e-6, "output = shizuku-correct (accompaniment '-')");
        const auto before = host.play (shifted, 0, offset);
        check (maxDifferenceToInput (before, shifted.vocal, 0) == 0.0, "input unchanged outside the captured range");
    }

    std::printf ("pitch entry in the edit panel\n");
    {
        double midi = 0.0;
        check (parsePitchText ("D5 +3", midi) && std::abs (midi - 74.03) < 1.0e-9, "\"D5 +3\" = 74.03");
        check (parsePitchText (String::fromUTF8 (u8"C#4 −12 セント"), midi) && std::abs (midi - 60.88) < 1.0e-9, "\"C#4 -12 セント\" = 60.88");
        check (parsePitchText ("Eb4", midi) && std::abs (midi - 63.0) < 1.0e-9, "\"Eb4\" = 63");
        check (! parsePitchText ("H2", midi) && ! parsePitchText ("D5 x", midi), "unreadable entries rejected");
        check (pitchText (74.03) == "D5 +3" && pitchText (60.88) == "C#4 -12", "pitch text round trip");
    }

    std::printf ("hand edits through the plug-in, as shizuku-correct --edits\n");
    {
        MemoryBlock editedState;
        NoteEditDocument edits;
        EditedExpectation expectedEdited;
        {
            Host host (true);
            auto& session = host.processor->getCaptureSession();
            session.arm();
            host.play (song, 0, length);
            host.stop();
            host.waitUntilReady();
            const auto automaticOutput = host.play (song, 0, length);
            host.stop(); // edited while the host is stopped (clicks and drags preview)
            const auto notes = session.getLatestTargets();
            check (notes != nullptr && notes->size() >= 10, "notes to edit");
            if (notes == nullptr || notes->size() < 10)
                return 1;
            const auto span = [&] (size_t i) { return NoteSpan { (*notes)[i].note.startSeconds, (*notes)[i].note.endSeconds }; };

            // Panel operations as the edit panel does them: a target, keep, a split, a merge (one undo step each).
            session.applyNoteEdits (withNoteTargets (session.getNoteEdits(), { span (2) }, { (*notes)[2].nearestScaleMidi + 1.03 }));
            session.applyNoteEdits (withNoteMode (session.getNoteEdits(), { span (4) }, NoteEditMode::keep));
            const auto& longNote = (*notes)[9].note; // 0.9 s
            session.applyNoteEdits (withSplit (session.getNoteEdits(), 0.5 * (longNote.startSeconds + longNote.endSeconds)));
            session.applyNoteEdits (withMerge (session.getNoteEdits(), span (6), span (7)));
            host.waitUntilReady();

            // Two notes picked with Cmd + click in the editor and dragged up 2 semitones together.
            std::unique_ptr<AudioProcessorEditor> editor (host.processor->createEditor());
            auto& pluginEditor = dynamic_cast<ShizukuPluginEditor&> (*editor);
            runMessageLoop (0.1);
            auto& view = pluginEditor.getPitchLineView();
            const auto before = session.getNoteEdits();
            const auto noteA = (size_t) 0, noteB = (size_t) 1;
            const double targetA = view.currentTargetMidi (noteA), targetB = view.currentTargetMidi (noteB);
            clickNote (view, noteA);
            clickNote (view, noteB, ModifierKeys (ModifierKeys::commandModifier));
            check (view.getSelection() == std::vector<size_t> ({ noteA, noteB }), "Cmd + click adds a note to the selection");
            dragNote (view, noteA, 2);
            host.waitUntilReady();
            runMessageLoop (0.1);
            check (std::abs (view.currentTargetMidi (noteA) - (targetA + 2.0)) < 0.006 && std::abs (view.currentTargetMidi (noteB) - (targetB + 2.0)) < 0.006,
                   "dragging one selected note moves every selected note by the same semitones");
            const auto dragged = session.getNoteEdits();
            session.undoNoteEdit();
            check (sameNoteEdits (session.getNoteEdits(), before), "one undo takes back the whole drag");
            session.redoNoteEdit();
            check (sameNoteEdits (session.getNoteEdits(), dragged), "redo applies it again");
            host.waitUntilReady();
            runMessageLoop (0.1);

            // Shift + click: every note from the previously clicked one.
            clickNote (view, 3);
            clickNote (view, 5, ModifierKeys (ModifierKeys::shiftModifier));
            check (view.getSelection() == std::vector<size_t> ({ 3, 4, 5 }), "Shift + click selects the notes in between");

            edits = session.getNoteEdits();
            std::printf ("  %zu edits, %zu splits / merges\n", edits.edits.size(), edits.structure.size());
            expectedEdited = expectedEditedCorrection (song, edits);
            const auto output = host.play (song, 0, length);
            std::printf ("  output vs. shizuku-correct --edits: max difference %.3g; edited spans:", maxDifference (output, expectedEdited.output));
            for (const auto& s : expectedEdited.spans)
                std::printf (" %.2f-%.2f", s.startSeconds, s.endSeconds);
            std::printf ("\n");
            check (maxDifference (output, expectedEdited.output) < 1.0e-6, "output = shizuku-correct --edits");
            check (maxDifference (output, automaticOutput) > 0.01, "the edits change the output");
            check (! expectedEdited.spans.empty() && maxDifferenceOutside (output, automaticOutput, expectedEdited.spans) == 0.0,
                   "outside the edited spans the output is the automatic correction (bit-identical)");
            const auto editsFile = CaptureSession::getCaptureRoot().getChildFile (session.getCaptureId()).getChildFile ("edits.json");
            check (editsFile.existsAsFile() && sameNoteEdits (parseNoteEdits (editsFile.loadFileAsString().toStdString()), edits),
                   "edits saved to captures/<id>/edits.json");

            if (editSnapshotPath.isNotEmpty())
            {
                clickNote (view, 0);
                clickNote (view, 1, ModifierKeys (ModifierKeys::commandModifier));
                view.zoom (0.5, 1.5);
                host.play (song, (int64) (2.0 * sampleRate), (int64) (2.0 * sampleRate) + blockSize); // playhead near 2 s
                runMessageLoop (0.2);
                const auto image = editor->createComponentSnapshot (editor->getLocalBounds());
                File (editSnapshotPath).deleteFile();
                FileOutputStream stream { File (editSnapshotPath) };
                PNGImageFormat().writeImageToStream (image, stream);
                std::printf ("  wrote the editor with edits to %s\n", editSnapshotPath.toRawUTF8());
                // One note picked: the panel with its pitches.
                clickNote (view, 2);
                runMessageLoop (0.1);
                const auto single = File (editSnapshotPath).getSiblingFile (File (editSnapshotPath).getFileNameWithoutExtension() + "-note.png");
                single.deleteFile();
                FileOutputStream singleStream { single };
                PNGImageFormat().writeImageToStream (editor->createComponentSnapshot (editor->getLocalBounds()), singleStream);
            }

            // Previews: with the transport stopped, a click on a note sounds that note (corrected) once.
            {
                Song silence;
                const auto stoppedBlocks = host.processor->getAuditionPlayer().getBlocksWhileStopped();
                allocationsCounted = 0;
                const auto quiet = host.play (silence, 0, (int64) (0.2 * sampleRate), false);
                check (peak (quiet) == 0.0, "stopped, no preview: silence");
                clickNote (view, 3);
                const auto preview = host.play (silence, 0, (int64) (0.5 * sampleRate), false);
                check (peak (preview) > 0.01, "stopped: clicking a note plays it (preview added to the output)");
                check (host.processor->getAuditionPlayer().getBlocksWhileStopped() > stoppedBlocks, "processBlock is called while stopped (counted)");
                // The clicked note's corrected audio from its start.
                const auto noteStart = (int64) std::floor (view.noteSpan (3).getStart() * sampleRate);
                bool matches = true;
                for (size_t i = (size_t) (0.02 * sampleRate); i < (size_t) (0.2 * sampleRate) && matches; ++i)
                    matches = std::abs (preview[i] - expectedEdited.output[(size_t) noteStart + i]) < 1.0e-5;
                check (matches, "the preview is the note's corrected audio");
                clickNote (view, 3);
                host.play (silence, 0, (int64) (0.1 * sampleRate), false);
                const auto started = host.play (silence, 0, (int64) (0.3 * sampleRate), true);
                check (peak (started) == 0.0, "the preview stops when the host starts playing");
                // Dragging: a tone right away, then the voice at the new pitch.
                const auto at = view.getNoteBoxCentre (3);
                view.mouseDown (mouseEventAt (view, at, at, {}, false));
                view.mouseDrag (mouseEventAt (view, at.translated (0.0f, -view.getSemitoneHeight()), at, ModifierKeys (ModifierKeys::leftButtonModifier), true));
                const auto tone = host.play (silence, 0, (int64) (0.1 * sampleRate), false);
                check (peak (tone) > 0.01, "dragging a note sounds its new pitch while stopped");
                view.mouseUp (mouseEventAt (view, at, at, {}, true)); // back where it started: no edit
                check (sameNoteEdits (session.getNoteEdits(), edits), "a drag back to the start makes no edit");
                check (allocationsCounted.load() == 0, "processBlock (capture playback and previews) does not allocate");
                std::printf ("  allocations in processBlock: %d\n", allocationsCounted.load());
            }
            editor.reset();
            host.processor->getStateInformation (editedState);
        }

        // A new instance from the plug-in state: the edits and the edited output are back without capturing again.
        Host host (true);
        host.processor->setStateInformation (editedState.getData(), (int) editedState.getSize());
        auto& session = host.processor->getCaptureSession();
        check (sameNoteEdits (session.getNoteEdits(), edits), "edits restored from the plug-in state");
        const auto early = host.play (song, 0, length);
        check (maxDifference (early, expectedEdited.output) < 1.0e-6, "saved edited render played right after loading");
        host.waitUntilReady();
        const auto restored = host.play (song, 0, length);
        check (maxDifference (restored, expectedEdited.output) < 1.0e-6, "same edited output after restoring");

        // Capture again (取り込み直し): the edits stay and find their notes by time.
        session.arm();
        host.play (song, 0, length);
        host.stop();
        host.waitUntilReady();
        check (sameNoteEdits (session.getNoteEdits(), edits), "edits kept after capturing again");
        const auto recaptured = host.play (song, 0, length);
        check (maxDifference (recaptured, expectedEdited.output) < 1.0e-6, "same edited output after capturing again");
    }

    captureRoot.deleteRecursively();
    std::printf (failures == 0 ? "all checks passed\n" : "%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
