#include "ShizukuPluginEditor.h"

#include "ShizukuPluginProcessor.h"
#include "AraDiagnostics.h"

#include "MusicTheory.h"

#include <mutex>

namespace shizuku
{

using namespace juce;

namespace
{
constexpr int noTrackId = 1;
constexpr int timerIntervalMs = 40;
constexpr double dragPreviewSeconds = 1.2; // a dragged note is previewed for at most this long

String ja (const char* text)
{
    return String::fromUTF8 (text);
}
} // namespace

//==============================================================================
// Renders a dragged note's audio at its new pitch in the background (renderCorrection with a constant shift
// over the note), so the preview is the voice itself; the newest request wins.
class ShizukuPluginEditor::PreviewRenderer final : private Thread
{
public:
    PreviewRenderer() : Thread ("Shizuku Tune preview") { startThread(); }
    ~PreviewRenderer() override
    {
        signalThreadShouldExit();
        notify();
        stopThread (2000);
    }

    // audio: the note's input audio (with some context); shiftSemitones applied between fadeIn / fadeOut seconds.
    void request (AuditionAudio audio, double shiftSemitones, double hintHz)
    {
        {
            const std::scoped_lock lock (mutex);
            pending = Request { std::move (audio), shiftSemitones, hintHz, ++serial };
            hasPending = true;
        }
        notify();
    }

    // Message thread: the newest finished render, if it answers the newest request.
    bool takeResult (AuditionAudio& audio)
    {
        const std::scoped_lock lock (mutex);
        if (! hasResult || resultSerial != serial)
            return false;
        hasResult = false;
        audio = std::move (result);
        return true;
    }

private:
    struct Request
    {
        AuditionAudio audio;
        double shift = 0.0, hint = 0.0;
        int serial = 0;
    };

    void run() override
    {
        while (! threadShouldExit())
        {
            Request job;
            {
                const std::scoped_lock lock (mutex);
                if (hasPending)
                {
                    job = std::move (pending);
                    hasPending = false;
                }
            }
            if (job.serial == 0)
            {
                wait (-1);
                continue;
            }
            const double seconds = job.audio.channels.empty() ? 0.0 : (double) job.audio.channels[0].size() / job.audio.sampleRate;
            CorrectionCurve curve;
            curve.points = { { 0.0, job.shift, 1.0 }, { seconds, job.shift, 1.0 } };
            AuditionAudio rendered;
            rendered.sampleRate = job.audio.sampleRate;
            rendered.channels = renderCorrection (job.audio.channels, job.audio.sampleRate, curve, 0.0, job.hint);
            const std::scoped_lock lock (mutex);
            result = std::move (rendered);
            resultSerial = job.serial;
            hasResult = true;
        }
    }

    std::mutex mutex;
    Request pending;
    bool hasPending = false;
    AuditionAudio result;
    int resultSerial = 0, serial = 0;
    bool hasResult = false;
};

//==============================================================================
ShizukuPluginEditor::ShizukuPluginEditor (ShizukuPluginProcessor& pluginProcessor)
    : AudioProcessorEditor (&pluginProcessor),
      AudioProcessorEditorARAExtension (&pluginProcessor),
      shizukuProcessor (pluginProcessor),
      previewRenderer (std::make_unique<PreviewRenderer>())
{
    for (auto* component : std::initializer_list<Component*> { &vocalLabel, &accompanimentLabel, &vocalChoice, &accompanimentChoice,
                                                                 &analyseButton, &keyLabel, &statusLabel, &pitchLineView, &noteEditPanel,
                                                                 &strengthLabel, &strengthSlider, &thresholdLabel, &thresholdSlider,
                                                                 &referenceButton, &clearReferenceButton, &referenceLabel,
                                                                 &referenceChoiceLabel, &referenceChoice, &captureButton, &finishButton,
                                                                 &captureRangeLabel, &zoomInButton, &zoomOutButton, &showAllButton,
                                                                 &followButton, &showReferenceButton })
        addAndMakeVisible (component);

    vocalLabel.setText (ja (u8"歌のトラック"), dontSendNotification);
    accompanimentLabel.setText (ja (u8"伴奏のトラック"), dontSendNotification);
    analyseButton.setButtonText (ja (u8"解析"));
    strengthLabel.setText (ja (u8"強さ"), dontSendNotification);
    thresholdLabel.setText (ja (u8"しきい値"), dontSendNotification);
    referenceButton.setButtonText (ja (u8"正解を追加…"));
    clearReferenceButton.setButtonText (ja (u8"消す"));
    referenceChoiceLabel.setText (ja (u8"正解のメロディー"), dontSendNotification);

    zoomInButton.setButtonText ("+");
    zoomInButton.setTooltip (ja (u8"拡大"));
    zoomOutButton.setButtonText (String::fromUTF8 (u8"−"));
    zoomOutButton.setTooltip (ja (u8"縮小"));
    showAllButton.setButtonText (ja (u8"全体"));
    followButton.setButtonText (ja (u8"再生に追従"));
    followButton.setClickingTogglesState (true);
    followButton.setToggleState (follow, dontSendNotification);
    followButton.onClick = [this] { follow = followButton.getToggleState(); };
    showReferenceButton.setButtonText (ja (u8"正解を表示"));
    showReferenceButton.setClickingTogglesState (true);
    showReferenceButton.setToggleState (true, dontSendNotification);
    showReferenceButton.onClick = [this] { pitchLineView.setShowReference (showReferenceButton.getToggleState()); };
    zoomInButton.onClick = [this]
    {
        const auto range = pitchLineView.getVisibleRange();
        pitchLineView.zoom (0.5, shizukuProcessor.isHostPlaying() ? shizukuProcessor.getHostPositionSeconds() : range.getStart() + 0.5 * range.getLength());
    };
    zoomOutButton.onClick = [this]
    {
        const auto range = pitchLineView.getVisibleRange();
        pitchLineView.zoom (2.0, range.getStart() + 0.5 * range.getLength());
    };
    showAllButton.onClick = [this] { pitchLineView.showAll(); };

    // Hand editing and previews.
    pitchLineView.onSelectionChanged = [this] { noteEditPanel.refresh(); };
    pitchLineView.onNoteClicked = [this] (size_t)
    {
        const auto& selection = pitchLineView.getSelection();
        if (selection.empty())
            return;
        const auto first = pitchLineView.noteSpan (selection.front()), last = pitchLineView.noteSpan (selection.back());
        previewRange (first.getStart(), last.getEnd());
    };
    pitchLineView.onRangeSelected = [this] (Range<double> range) { previewRange (range.getStart(), range.getEnd()); };
    pitchLineView.onDragStep = [this] (size_t note, int semitones) { previewNoteAt (note, semitones); };
    pitchLineView.onNotesDragged = [this] (std::vector<size_t> notes, int semitones) { noteEditPanel.moveNotesBySemitones (notes, semitones); };
    setWantsKeyboardFocus (true);

    strengthSlider.setRange (0.0, 100.0, 1.0);
    strengthSlider.setTextValueSuffix (" %");
    thresholdSlider.setRange (0.0, 50.0, 1.0);
    thresholdSlider.setTextValueSuffix (ja (u8" セント"));
    for (auto* slider : { &strengthSlider, &thresholdSlider })
    {
        slider->setSliderStyle (Slider::LinearHorizontal);
        slider->setTextBoxStyle (Slider::TextBoxRight, false, 80, 20);
        slider->setChangeNotificationOnlyOnRelease (true); // one re-render per drag
        slider->onValueChange = [this] { applySliderSettings(); };
    }
    referenceLabel.setColour (Label::textColourId, Colours::lightgrey);
    referenceButton.onClick = [this]
    {
        if (model == nullptr)
            return;
        referenceChooser = std::make_unique<FileChooser> (ja (u8"正解のメロディーの候補（Synthesizer V .svp、MIDI .mid、ガイドの .wav）"),
                                                          File(), "*.svp;*.mid;*.midi;*.wav");
        referenceChooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles
                                           | FileBrowserComponent::canSelectMultipleItems,
                                       [this] (const FileChooser& chooser)
                                       {
                                           if (model == nullptr)
                                               return;
                                           for (const auto& file : chooser.getResults())
                                           {
                                               const auto error = model->addReferenceMelody (file);
                                               if (error.isNotEmpty())
                                                   statusLabel.setText (error, dontSendNotification);
                                           }
                                       });
    };
    clearReferenceButton.onClick = [this]
    {
        if (model != nullptr)
            model->clearReferenceMelodies();
    };
    referenceChoice.onChange = [this]
    {
        if (model != nullptr && referenceChoice.getSelectedId() > 0)
            model->setReferenceChoice (referenceChoice.getSelectedId() - 3);
    };

    keyLabel.setFont (FontOptions (20.0f, Font::bold));
    keyLabel.setJustificationType (Justification::centredRight);
    statusLabel.setColour (Label::textColourId, Colours::lightgrey);

    vocalChoice.onChange = [this]
    {
        if (documentController == nullptr)
            return;
        const int index = vocalChoice.getSelectedId() - 2;
        documentController->setVocalTrack (index >= 0 && index < (int) trackChoices.size() ? trackChoices[(size_t) index] : nullptr);
    };
    accompanimentChoice.onChange = [this]
    {
        if (documentController == nullptr)
            return;
        const int index = accompanimentChoice.getSelectedId() - 2;
        documentController->setAccompanimentTrack (index >= 0 && index < (int) trackChoices.size() ? trackChoices[(size_t) index] : nullptr);
    };
    analyseButton.onClick = [this]
    {
        if (documentController == nullptr)
            return;
        const auto error = documentController->startAnalysis();
        if (error.isNotEmpty())
            statusLabel.setText (error, dontSendNotification);
    };

    captureRangeLabel.setColour (Label::textColourId, Colours::lightgrey);
    captureButton.onClick = [this]
    {
        auto& session = shizukuProcessor.getCaptureSession();
        if (session.getState() == CaptureSession::State::armed)
            session.cancelArm();
        else
            session.arm();
    };
    finishButton.setButtonText (String::fromUTF8 (u8"終了"));
    finishButton.onClick = [this] { shizukuProcessor.getCaptureSession().finishCapture(); };

    if (! attachToARA())
        showCaptureMode (true);
    startTimer (timerIntervalMs);

    // ARA editors must be resizable for host integration.
    setResizable (true, false);
    setResizeLimits (720, 360, 4000, 2000);
    setSize (980, 520);
}

bool ShizukuPluginEditor::attachToARA()
{
    if (documentController != nullptr)
        return true;
    auto* editorView = getARAEditorView();
    if (editorView == nullptr)
        return false;
    documentController = ARADocumentControllerSpecialisation::getSpecialisedDocumentController<ShizukuDocumentController> (
        editorView->getDocumentController());
    if (documentController == nullptr)
        return false;
    showCaptureMode (false);
    setModel (documentController);
    araDiagnostic ("editor attached to the ARA document controller");
    return true;
}

void ShizukuPluginEditor::setModel (CorrectionModel* newModel)
{
    if (model != nullptr)
        model->removeChangeListener (this);
    model = newModel;
    noteEditPanel.setModel (model);
    if (model == nullptr)
        return;
    const auto settings = model->getCorrectionSettings();
    strengthSlider.setValue (settings.strength * 100.0, dontSendNotification);
    thresholdSlider.setValue (settings.thresholdCents, dontSendNotification);
    model->addChangeListener (this);
    changeListenerCallback (model);
}

void ShizukuPluginEditor::showCaptureMode (bool shouldShow)
{
    for (auto* component : std::initializer_list<Component*> { &vocalLabel, &accompanimentLabel, &vocalChoice, &accompanimentChoice, &analyseButton })
        component->setVisible (! shouldShow);
    for (auto* component : std::initializer_list<Component*> { &captureButton, &finishButton, &captureRangeLabel })
        component->setVisible (shouldShow);
    if (shouldShow)
        setModel (&shizukuProcessor.getCaptureSession());
    resized();
}

void ShizukuPluginEditor::refreshCapture()
{
    if (documentController != nullptr)
        return;
    const auto& session = shizukuProcessor.getCaptureSession();
    const auto state = session.getState();
    const bool hasCapture = session.getCaptureId().isNotEmpty();
    captureButton.setButtonText (state == CaptureSession::State::armed ? String::fromUTF8 (u8"待機をやめる")
                                 : hasCapture || state == CaptureSession::State::capturing ? String::fromUTF8 (u8"取り込み直し")
                                                                                            : String::fromUTF8 (u8"取り込み"));
    captureButton.setEnabled (state != CaptureSession::State::capturing);
    finishButton.setEnabled (state == CaptureSession::State::capturing);
    captureRangeLabel.setText (session.getRangeText(), dontSendNotification);
}

void ShizukuPluginEditor::timerCallback()
{
    if (documentController == nullptr)
        attachToARA();

    // Host playhead (and follow while playing).
    const bool playing = shizukuProcessor.isHostPlaying();
    const auto position = shizukuProcessor.getHostPositionSeconds();
    pitchLineView.setPlayhead (position, playing || position > 0.0);
    if (playing && follow)
        pitchLineView.keepVisible (position);

    // A dragged note's voice at its new pitch, when it is ready.
    AuditionAudio rendered;
    if (previewRenderer->takeResult (rendered))
        shizukuProcessor.getAuditionPlayer().play (rendered);

    pitchLineView.setBusyText (model != nullptr && model->isRendering() ? ja (u8"作り直し中…") : String());
}

bool ShizukuPluginEditor::keyPressed (const KeyPress& key)
{
    if (model == nullptr)
        return false;
    if (key.getModifiers().isCommandDown() && (key.getKeyCode() == 'Z' || key.getKeyCode() == 'z'))
    {
        if (key.getModifiers().isShiftDown())
            model->redoNoteEdit();
        else
            model->undoNoteEdit();
        return true;
    }
    if (key.getModifiers().isCommandDown() && (key.getKeyCode() == 'Y' || key.getKeyCode() == 'y'))
    {
        model->redoNoteEdit();
        return true;
    }
    if (key == KeyPress::upKey || key == KeyPress::downKey)
    {
        noteEditPanel.moveSelectedBySemitones (key == KeyPress::upKey ? 1 : -1);
        return true;
    }
    return false;
}

void ShizukuPluginEditor::previewRange (double startSeconds, double endSeconds)
{
    if (model == nullptr || shizukuProcessor.isHostPlaying())
        return; // while the host plays, the playback itself is what is heard
    const double length = jlimit (0.25, AuditionPlayer::maxSeconds, endSeconds - startSeconds);
    const double from = endSeconds - startSeconds < 0.25 ? startSeconds - 0.1 : startSeconds;
    AuditionAudio audio;
    if (model->getAuditionAudio (from, from + length, true, audio))
        shizukuProcessor.getAuditionPlayer().play (audio);
}

void ShizukuPluginEditor::previewNoteAt (size_t note, int semitones)
{
    if (model == nullptr)
        return;
    const double targetMidi = pitchLineView.currentTargetMidi (note) + semitones;
    auto& player = shizukuProcessor.getAuditionPlayer();
    // A short tone right away; replaced by the voice at that pitch when it has been rendered.
    player.playTone (targetMidi);
    const auto targets = model->getLatestTargets();
    const auto result = model->getLatestResult();
    if (targets == nullptr || result == nullptr || note >= targets->size())
        return;
    const auto span = pitchLineView.noteSpan (note);
    AuditionAudio audio;
    if (! model->getAuditionAudio (span.getStart(), span.getStart() + jmin (dragPreviewSeconds, span.getLength()), false, audio))
        return;
    previewRenderer->request (std::move (audio), targetMidi - (*targets)[note].note.centreMidi, medianVoicedFrequency (result->analysis.pitch));
}

ShizukuPluginEditor::~ShizukuPluginEditor()
{
    stopTimer();
    if (model != nullptr)
        model->removeChangeListener (this);
    shizukuProcessor.getAuditionPlayer().stop();
}

void ShizukuPluginEditor::paint (Graphics& g)
{
    g.fillAll (Colour (0xff20242b));
}

void ShizukuPluginEditor::resized()
{
    auto area = getLocalBounds().reduced (8);
    auto top = area.removeFromTop (28);
    vocalLabel.setBounds (top.removeFromLeft (80));
    vocalChoice.setBounds (top.removeFromLeft (180));
    top.removeFromLeft (12);
    accompanimentLabel.setBounds (top.removeFromLeft (140));
    accompanimentChoice.setBounds (top.removeFromLeft (180));
    top.removeFromLeft (12);
    analyseButton.setBounds (top.removeFromLeft (90));
    if (documentController == nullptr)
    {
        top = top.withX (area.getX()).withRight (area.getRight());
        captureButton.setBounds (top.removeFromLeft (120));
        top.removeFromLeft (6);
        finishButton.setBounds (top.removeFromLeft (70));
        top.removeFromLeft (12);
        captureRangeLabel.setBounds (top.removeFromLeft (jmax (0, top.getWidth() - 220)));
    }
    keyLabel.setBounds (top);
    area.removeFromTop (4);
    auto controls = area.removeFromTop (26);
    strengthLabel.setBounds (controls.removeFromLeft (64));
    strengthSlider.setBounds (controls.removeFromLeft (220));
    controls.removeFromLeft (12);
    thresholdLabel.setBounds (controls.removeFromLeft (70));
    thresholdSlider.setBounds (controls.removeFromLeft (220));
    controls.removeFromLeft (12);
    referenceButton.setBounds (controls.removeFromLeft (120));
    controls.removeFromLeft (4);
    clearReferenceButton.setBounds (controls.removeFromLeft (50));
    area.removeFromTop (4);
    auto referenceRow = area.removeFromTop (24);
    referenceChoiceLabel.setBounds (referenceRow.removeFromLeft (110));
    referenceChoice.setBounds (referenceRow.removeFromLeft (420));
    referenceRow.removeFromLeft (8);
    referenceLabel.setBounds (referenceRow);
    area.removeFromTop (4);
    auto viewRow = area.removeFromTop (24);
    zoomOutButton.setBounds (viewRow.removeFromLeft (28));
    viewRow.removeFromLeft (2);
    zoomInButton.setBounds (viewRow.removeFromLeft (28));
    viewRow.removeFromLeft (2);
    showAllButton.setBounds (viewRow.removeFromLeft (52));
    viewRow.removeFromLeft (8);
    followButton.setBounds (viewRow.removeFromLeft (96));
    viewRow.removeFromLeft (4);
    showReferenceButton.setBounds (viewRow.removeFromLeft (96));
    area.removeFromTop (4);
    statusLabel.setBounds (area.removeFromBottom (22));
    area.removeFromBottom (4);
    noteEditPanel.setBounds (area.removeFromBottom (92));
    area.removeFromBottom (4);
    pitchLineView.setBounds (area);
}

void ShizukuPluginEditor::changeListenerCallback (ChangeBroadcaster*)
{
    if (documentController != nullptr)
        refreshTrackChoices();
    else
        refreshCapture();
    refreshResult();
}

void ShizukuPluginEditor::refreshTrackChoices()
{
    trackChoices = documentController->getTracks();
    for (auto* box : { &vocalChoice, &accompanimentChoice })
    {
        box->clear (dontSendNotification);
        box->addItem (ja (u8"（なし）"), noTrackId);
        for (size_t i = 0; i < trackChoices.size(); ++i)
        {
            auto name = ShizukuDocumentController::getTrackName (*trackChoices[i]);
            if (box == &accompanimentChoice && ShizukuDocumentController::looksLikeAccompaniment (name))
                name += String::fromUTF8 (u8"  (伴奏候補)");
            box->addItem (name, (int) i + 2);
        }
    }

    const auto idFor = [this] (ARARegionSequence* track)
    {
        const auto it = std::find (trackChoices.begin(), trackChoices.end(), track);
        return it == trackChoices.end() ? noTrackId : (int) (it - trackChoices.begin()) + 2;
    };
    vocalChoice.setSelectedId (idFor (documentController->getVocalTrack()), dontSendNotification);
    accompanimentChoice.setSelectedId (idFor (documentController->getAccompanimentTrack()), dontSendNotification);
    analyseButton.setEnabled (! documentController->isAnalysing());
}

void ShizukuPluginEditor::applySliderSettings()
{
    if (model == nullptr)
        return;
    auto settings = model->getCorrectionSettings();
    settings.strength = strengthSlider.getValue() / 100.0;
    settings.thresholdCents = thresholdSlider.getValue();
    model->setCorrectionSettings (settings);
}

void ShizukuPluginEditor::refreshReferenceChoices()
{
    referenceChoice.clear (dontSendNotification);
    const auto candidates = model->getReferenceCandidates();
    const int choice = model->getReferenceChoice();
    String autoText = String::fromUTF8 (u8"自動（区間ごとに照合）");
    if (choice == referenceAuto && ! candidates.empty())
    {
        // Name the candidates the automatic choice actually used, most sung time first.
        std::vector<std::pair<double, String>> used;
        for (const auto& info : candidates)
            if (info.usedSeconds > 0.0)
                used.push_back ({ info.usedSeconds, info.name + (info.shiftedSeconds > 0.0 ? String::fromUTF8 (u8" (拍ずらし ") + String (info.shiftedSeconds, 1) + " s)" : String()) });
        std::sort (used.begin(), used.end(), [] (const auto& a, const auto& b) { return a.first > b.first; });
        StringArray names;
        for (const auto& [seconds, name] : used)
            names.add (name + " " + String (seconds, 1) + " s");
        if (! names.isEmpty())
            autoText << ": " << names.joinIntoString (", ");
    }
    referenceChoice.addItem (autoText, 1);
    referenceChoice.addItem (String::fromUTF8 (u8"なし（キーの音階）"), 2);
    for (size_t c = 0; c < candidates.size(); ++c)
    {
        const auto& info = candidates[c];
        referenceChoice.addItem (info.name + ja (u8"（一致 ") + String (roundToInt (info.looseMatch * 100.0)) + ja (u8"%、ある所 ")
                                     + String (roundToInt (info.presence * 100.0)) + ja (u8"%、ずれ ") + String (info.offsetSeconds, 2) + ja (u8" 秒）"),
                                 (int) c + 3);
    }
    referenceChoice.setSelectedId (choice + 3, dontSendNotification);
    referenceChoice.setEnabled (model->canChooseReference());
}

void ShizukuPluginEditor::refreshResult()
{
    refreshReferenceChoices();
    statusLabel.setText (model->getStatusText(), dontSendNotification);
    referenceLabel.setText (model->getReferenceDescription(), dontSendNotification);
    const auto result = model->getLatestResult();
    pitchLineView.setResult (result);
    pitchLineView.setCorrection (model->getLatestCorrection(), model->getLatestTargets());
    refreshReferenceNotes();
    noteEditPanel.refresh();
    if (result != nullptr && result->analysis.hasAccompaniment)
    {
        String text = ja (u8"キー: ") + String (keyName (result->analysis.key.globalKey));
        if (result->analysis.key.segments.size() > 1)
            text << " (" << (int) result->analysis.key.segments.size() << ja (u8" 区間)");
        keyLabel.setText (text, dontSendNotification);
    }
    else
    {
        keyLabel.setText (ja (u8"キー: -"), dontSendNotification);
    }
}

void ShizukuPluginEditor::refreshReferenceNotes()
{
    // The correct melody as used: per phrase the chosen candidate's notes (with its offset and beat shift).
    std::vector<DrawnReferenceNote> notes;
    const auto targets = model->getReferenceTargets();
    const auto result = model->getLatestResult();
    if (targets != nullptr && result != nullptr && targets->referencesApplied)
    {
        const auto& selection = targets->selection;
        for (const auto& segment : selection.segments)
        {
            if (segment.candidate < 0 || (size_t) segment.candidate >= targets->referenceCandidates.size())
                continue;
            const auto& candidate = targets->referenceCandidates[(size_t) segment.candidate];
            const double offset = candidate.offsetSeconds + segment.chosenShiftBeats() * selection.beatSeconds;
            for (const auto& note : candidate.melody.notes)
            {
                const double start = note.startSeconds + offset, end = note.endSeconds + offset;
                if (end <= segment.startSeconds || start >= segment.endSeconds)
                    continue;
                if (segment.part >= 0 && note.part != segment.part)
                    continue;
                notes.push_back ({ result->timelineStartSeconds + jmax (start, segment.startSeconds),
                                   result->timelineStartSeconds + jmin (end, segment.endSeconds), note.midi + 12 * segment.octave });
            }
        }
    }
    pitchLineView.setReferenceNotes (std::move (notes));
}

} // namespace shizuku
