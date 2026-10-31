#include "AppController.h"

#include "AudioCapture.h"
#include "AudioPlayer.h"
#include "SpectrumModel.h"
#include "GlobalHotkey.h"
#include "LocalAsrClient.h"
#include "OllamaClient.h"
#include "PiperTts.h"
#include "WavCodec.h"

#include <QFile>

#include <QSettings>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QDateTime>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QtMath>
#include <QDebug>

namespace {

const char *kDemoQuestions[] = {
    "What is the weather looking like in Bordeaux this weekend?",
    "Can you explain what a low-opacity glow layer actually does?",
    "Give me one idea for a quiet morning routine.",
};

const char *kDemoAnswers[] = {
    "Saturday stays clear at about twenty-four degrees with a light breeze from the west. "
    "Sunday turns cloudier, and there is a fair chance of showers late in the afternoon.",
    "It paints soft light behind the interface instead of over it, so the panel still reads "
    "clearly. The glow sits at roughly a fifth of full opacity, and only its edges move.",
    "Start with ten minutes of daylight and no screen. Then write down the one thing that would "
    "make the day feel finished, and do that first.",
};

QString sanitiseForSpeech(const QString &text)
{
    QString out = text;
    out.remove(QLatin1Char('*'));
    out.remove(QLatin1Char('`'));
    out.remove(QLatin1Char('#'));
    out.remove(QLatin1Char('_'));
    out.replace(QStringLiteral("  "), QStringLiteral(" "));
    return out.trimmed();
}

// Index just past the first sentence end in `text`, or -1 when there is none.
int sentenceEnd(const QString &text, int minimumLength)
{
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        const bool terminator = c == QLatin1Char('.') || c == QLatin1Char('!')
                             || c == QLatin1Char('?') || c == QChar(0x2026)
                             || c == QLatin1Char('\n');
        if (!terminator)
            continue;
        if (i + 1 < text.size() && !text.at(i + 1).isSpace() && c != QLatin1Char('\n'))
            continue;
        if (i + 1 < minimumLength)
            continue;
        return i + 1;
    }
    return -1;
}

struct HotkeySpec
{
    quint32 keysym = 0x0020;
    quint32 modifiers = 1u << 2; // ControlMask
    QString label = QStringLiteral("Ctrl + Space");
};

bool parseHotkey(const QString &text, HotkeySpec *spec)
{
    HotkeySpec out;
    QStringList parts;
    for (const QString &raw : text.split(QLatin1Char('+'), Qt::SkipEmptyParts))
        parts.append(raw.trimmed().toLower());

    bool haveKey = false;
    bool haveMod = false;
    QStringList labels;
    for (const QString &part : parts) {
        if (part == QLatin1String("ctrl") || part == QLatin1String("control")) {
            out.modifiers |= 1u << 2;
            labels << QStringLiteral("Ctrl");
            haveMod = true;
        } else if (part == QLatin1String("alt") || part == QLatin1String("mod1")) {
            out.modifiers |= 1u << 3;
            labels << QStringLiteral("Alt");
            haveMod = true;
        } else if (part == QLatin1String("shift")) {
            out.modifiers |= 1u << 0;
            labels << QStringLiteral("Shift");
            haveMod = true;
        } else if (part == QLatin1String("super") || part == QLatin1String("meta")
                   || part == QLatin1String("win")) {
            out.modifiers |= 1u << 6;
            labels << QStringLiteral("Super");
            haveMod = true;
        } else if (part == QLatin1String("space")) {
            out.keysym = 0x0020;
            labels << QStringLiteral("Space");
            haveKey = true;
        } else if (part.size() == 1 && part.at(0).isLetter()) {
            out.keysym = quint32(part.at(0).toLatin1());
            labels << part.toUpper();
            haveKey = true;
        } else {
            return false;
        }
    }
    if (!haveKey || !haveMod)
        return false;
    out.label = labels.join(QStringLiteral(" + "));
    *spec = out;
    return true;
}

} // namespace

AppController::AppController(QObject *parent)
    : QObject(parent)
{
    QSettings settings;
    MistralClient::Config config;
    const QByteArray envKey = qgetenv("MISTRAL_API_KEY");
    config.apiKey = envKey.isEmpty()
        ? settings.value(QStringLiteral("apiKey")).toString()
        : QString::fromUtf8(envKey);
    config.asrModel = settings.value(QStringLiteral("asrModel"),
                                     QStringLiteral("voxtral-mini-transcribe-realtime-3")).toString();
    config.chatModel = settings.value(QStringLiteral("chatModel"),
                                      QStringLiteral("mistral-small-latest")).toString();
    config.ttsModel = settings.value(QStringLiteral("ttsModel"),
                                     QStringLiteral("voxtral-mini-tts-3-0")).toString();
    config.voiceId = settings.value(QStringLiteral("voiceId"), QStringLiteral("voice_478")).toString();
    config.language = settings.value(QStringLiteral("language"), QStringLiteral("en")).toString();
    config.contextBias = settings.value(QStringLiteral("contextBias")).toStringList();
    m_client.setConfig(config);

    m_continuous = settings.value(QStringLiteral("continuous"), true).toBool();
    m_reducedMotion = settings.value(QStringLiteral("reducedMotion"), false).toBool()
        || qEnvironmentVariableIntValue("HALO_REDUCED_MOTION") == 1;

    m_asrMode = settings.value(QStringLiteral("asr"), QStringLiteral("local")).toString();
    m_llmMode = settings.value(QStringLiteral("llm"), QStringLiteral("ollama")).toString();
    m_ttsMode = settings.value(QStringLiteral("tts"), QStringLiteral("piper")).toString();

    m_localAsr = new LocalAsrClient(this);
    QString asrEndpoint = settings.value(QStringLiteral("asrEndpoint"),
        QStringLiteral("ws://127.0.0.1:8765")).toString();
    if (qEnvironmentVariableIsSet("HALO_ASR_URL"))
        asrEndpoint = qEnvironmentVariable("HALO_ASR_URL");
    m_localAsr->setEndpoint(QUrl(asrEndpoint));

    m_llm = new OllamaClient(this);
    m_llm->setEndpoint(QUrl(settings.value(QStringLiteral("ollamaUrl"),
        QStringLiteral("http://127.0.0.1:11434")).toString()));
    m_llm->setModel(settings.value(QStringLiteral("ollamaModel"),
        QStringLiteral("qwen3.8:27b")).toString());

    m_tts = new PiperTts(this);
    QString piperBin = settings.value(QStringLiteral("piperBin")).toString();
    if (piperBin.isEmpty()) {
        // Look next to the app (the bundled venv) before falling back to PATH.
        const QString local = QCoreApplication::applicationDirPath()
            + QStringLiteral("/../.venv/bin/piper");
        piperBin = QFileInfo::exists(local) ? QDir::cleanPath(local)
                                            : QStringLiteral("piper");
    }
    m_tts->setExecutable(piperBin);
    m_tts->setVoice(settings.value(QStringLiteral("piperVoice"),
        QStringLiteral("/home/ayoub/halo/models/piper/en_US-amy-medium.onnx")).toString());

    m_capture = new AudioCapture(this);
    m_player = new AudioPlayer(this);
    m_spectrum = new SpectrumModel(this);
    m_hotkey = new GlobalHotkey(this);

    m_micAvailable = m_capture->available();
    m_demo = qEnvironmentVariableIntValue("HALO_FORCE_DEMO") == 1
        || (m_asrMode == QLatin1String("mistral") && !m_client.hasKey());

    connect(m_capture, &AudioCapture::pcm, this, [this](const QByteArray &chunk) {
        if (m_state != QLatin1String("listening"))
            return;
        if (m_asrMode == QLatin1String("mistral"))
            m_client.pushAudio(chunk);
        else
            m_localAsr->pushAudio(chunk);
        m_spectrum->pushPcm(chunk, AudioCapture::SampleRate, 1);
    });
    connect(m_capture, &AudioCapture::level, this, [this](float level) {
        if (m_state != QLatin1String("listening"))
            return;
        setLevel(level);
        m_listenMs += 100;
        if (level > 0.055f) {
            m_heardVoice = true;
            m_silenceMs = 0;
        } else if (m_heardVoice) {
            m_silenceMs += 100;
            if (m_silenceMs >= 1100)
                endUtterance();
        }
        if (m_listenMs >= 20000 || (!m_heardVoice && m_listenMs >= 11000))
            endUtterance();
    });
    connect(m_capture, &AudioCapture::failed, this, [this](const QString &message) {
        m_micAvailable = false;
        emit statusChanged();
        setError(message + QStringLiteral(" You can type your question instead."));
        if (m_state == QLatin1String("listening"))
            setState(QStringLiteral("idle"));
    });

    connect(m_player, &AudioPlayer::pcmAtPosition, this,
            [this](const QByteArray &chunk, int rate, int channels) {
                m_spectrum->pushPcm(chunk, rate, channels);
                if (chunk.size() >= 2) {
                    double sum = 0.0;
                    const int frames = chunk.size() / (2 * channels);
                    for (int i = 0; i < frames; ++i) {
                        const int offset = i * 2 * channels;
                        const qint16 s = qint16(quint16(quint8(chunk.at(offset)))
                                                | (quint16(quint8(chunk.at(offset + 1))) << 8));
                        const double v = s / 32768.0;
                        sum += v * v;
                    }
                    const double rms = frames > 0 ? std::sqrt(sum / frames) : 0.0;
                    setLevel(std::min(1.0, std::pow(rms * 5.0, 0.7)));
                }
            });
    connect(m_player, &AudioPlayer::finished, this, &AppController::onPlayerFinished);
    connect(m_player, &AudioPlayer::failed, this, [this](const QString &message) {
        setError(message);
        m_audioQueue.clear();
        m_speechQueue.clear();
        m_speechInFlight = false;
        finishTurnIfQuiet();
    });

    connect(&m_client, &MistralClient::streamReady, this, [this]() {
        if (m_state != QLatin1String("listening"))
            return;
        if (!m_feedPcm.isEmpty())
            m_feedTimer->start(100);
        else
            m_capture->start();
    });
    connect(m_localAsr, &LocalAsrClient::streamReady, this, [this]() {
        if (m_state != QLatin1String("listening"))
            return;
        if (!m_feedPcm.isEmpty())
            m_feedTimer->start(100);
        else
            m_capture->start();
    });
    connect(m_localAsr, &LocalAsrClient::status, this, [this](const QString &message) {
        m_statusText = message;
        emit statusChanged();
    });
    connect(m_localAsr, &LocalAsrClient::partialText, this, [this](const QString &text) {
        if (m_state != QLatin1String("listening"))
            return;
        if (m_clearOnSpeech)
            clearForNewSpeech();
        m_transcript = text;
        setQuestion(text.trimmed());
    });
    connect(m_localAsr, &LocalAsrClient::utteranceDone, this, &AppController::onUtteranceDone);
    connect(m_localAsr, &LocalAsrClient::failed, this, [this](const QString &message) {
        setError(message + QStringLiteral(" Run scripts/start-asr.sh, then try again."));
        if (m_state == QLatin1String("listening")) {
            m_capture->stop();
            setState(QStringLiteral("idle"));
        }
    });
    connect(&m_client, &MistralClient::transcriptDelta, this, [this](const QString &delta) {
        if (m_state != QLatin1String("listening"))
            return;
        if (m_clearOnSpeech)
            clearForNewSpeech();
        m_transcript += delta;
        setQuestion(m_transcript.trimmed());
    });
    connect(&m_client, &MistralClient::utteranceDone, this, &AppController::onUtteranceDone);
    connect(m_llm, &OllamaClient::answerDelta, this, [this](const QString &delta) {
        if (m_state == QLatin1String("thinking"))
            setState(QStringLiteral("answering"));
        appendAnswer(delta);
    });
    connect(m_llm, &OllamaClient::answerDone, this, [this](const QString &full) {
        onAnswerDone(full);
    });
    connect(m_llm, &OllamaClient::failed, this, [this](const QString &message) {
        setError(message);
        setState(QStringLiteral("idle"));
    });

    connect(m_tts, &PiperTts::speechReady, this, [this](const QByteArray &wav) {
        m_speechInFlight = false;
        m_audioQueue.enqueue(wav);
        pumpAudio();
        pumpSpeech();
    });
    connect(m_tts, &PiperTts::failed, this, [this](const QString &message) {
        qWarning() << "[halo]" << message;
        setError(message);
        m_speechInFlight = false;
        finishTurnIfQuiet();
    });

    connect(&m_client, &MistralClient::answerDelta, this, [this](const QString &delta) {
        if (m_state == QLatin1String("thinking"))
            setState(QStringLiteral("answering"));
        appendAnswer(delta);
    });
    connect(&m_client, &MistralClient::answerDone, this, [this](const QString &full) {
        onAnswerDone(full);
    });
    connect(&m_client, &MistralClient::speechReady, this, [this](const QByteArray &wav) {
        m_speechInFlight = false;
        m_audioQueue.enqueue(wav);
        pumpAudio();
        pumpSpeech();
    });
    connect(&m_client, &MistralClient::failed, this, [this](const QString &message) {
        qWarning() << "[halo]" << message;
        setError(message);
        m_speechInFlight = false;
        if (m_state == QLatin1String("listening")) {
            m_capture->stop();
            m_client.cancelStreaming();
            setState(QStringLiteral("idle"));
        } else if (m_speechQueue.isEmpty() && m_audioQueue.isEmpty()) {
            finishTurnIfQuiet();
        } else {
            pumpSpeech();
        }
    });

    connect(m_hotkey, &GlobalHotkey::activated, this, &AppController::toggle);

    // Ctrl+Space is often taken by the input-method switcher (IBus uses it by
    // default), so fall back through alternatives and tell the UI which one
    // actually landed.
    QStringList candidates;
    candidates << settings.value(QStringLiteral("hotkey"), QStringLiteral("ctrl+space")).toString();
    candidates << QStringLiteral("ctrl+alt+space") << QStringLiteral("ctrl+shift+space")
               << QStringLiteral("super+space");
    candidates.removeDuplicates();
    bool registered = false;
    QStringList tried;
    for (const QString &candidate : candidates) {
        HotkeySpec spec;
        if (!parseHotkey(candidate, &spec))
            continue;
        if (m_hotkey->start(spec.keysym, spec.modifiers)) {
            m_hotkeyLabel = spec.label;
            registered = true;
            qInfo() << "[halo] global hotkey" << spec.label << "registered";
            break;
        }
        tried << spec.label;
    }
    if (!registered)
        qWarning() << "[halo] no global hotkey could be grabbed (tried" << tried.join(", ")
                   << "):" << m_hotkey->status();
    emit statusChanged();

    m_resumeTimer = new QTimer(this);
    m_resumeTimer->setSingleShot(true);
    connect(m_resumeTimer, &QTimer::timeout, this, &AppController::startListening);

    m_demoTimer = new QTimer(this);
    m_demoTimer->setInterval(33);
    connect(m_demoTimer, &QTimer::timeout, this, &AppController::demoTick);

    m_feedTimer = new QTimer(this);
    m_feedTimer->setInterval(100);
    connect(m_feedTimer, &QTimer::timeout, this, [this]() {
        if (m_feedPcm.isEmpty() || m_state != QLatin1String("listening")) {
            m_feedTimer->stop();
            return;
        }
        const int len = qMin(3200, m_feedPcm.size() - m_feedPos);
        const QByteArray chunk = m_feedPcm.mid(m_feedPos, len);
        m_feedPos += len;

        if (m_asrMode == QLatin1String("local"))
            m_localAsr->pushAudio(chunk);
        else
            m_client.pushAudio(chunk);
        m_spectrum->pushPcm(chunk, AudioCapture::SampleRate, 1);

        double sum = 0.0;
        const int samples = chunk.size() / 2;
        for (int i = 0; i < samples; ++i) {
            const qint16 v = qint16(quint16(quint8(chunk.at(i * 2)))
                                    | (quint16(quint8(chunk.at(i * 2 + 1))) << 8));
            const double f = v / 32768.0;
            sum += f * f;
        }
        const double rms = samples > 0 ? std::sqrt(sum / samples) : 0.0;
        setLevel(std::min(1.0, std::pow(rms * 5.5, 0.72)));

        if (m_feedPos >= m_feedPcm.size()) {
            m_feedTimer->stop();
            QTimer::singleShot(700, this, [this]() {
                if (m_state == QLatin1String("listening"))
                    endUtterance();
            });
        }
    });

    updateStatus();
    setState(QStringLiteral("hidden"));
}

AppController::~AppController() = default;

QString AppController::phaseLabel() const
{
    if (m_state == QLatin1String("listening")) return QStringLiteral("Listening");
    if (m_state == QLatin1String("thinking")) return QStringLiteral("Thinking");
    if (m_state == QLatin1String("answering")) return QStringLiteral("Answering");
    if (m_state == QLatin1String("speaking")) return QStringLiteral("Speaking");
    if (m_state == QLatin1String("error")) return QStringLiteral("Offline");
    if (m_state == QLatin1String("idle")) return QStringLiteral("Ready");
    return QString();
}

QString AppController::hintText() const
{
    if (m_state == QLatin1String("listening"))
        return QStringLiteral("Speak, then pause — I'll answer when you stop.");
    if (m_state == QLatin1String("thinking"))
        return QStringLiteral("Working on it.");
    if (m_state == QLatin1String("answering"))
        return QStringLiteral("Answering in text, voice follows.");
    if (m_state == QLatin1String("speaking"))
        return QStringLiteral("Press Esc to interrupt.");
    if (!m_notice.isEmpty())
        return m_notice;
    return QStringLiteral("%1 to talk again  ·  Esc to close").arg(m_hotkeyLabel);
}

bool AppController::micAvailable() const
{
    return m_micAvailable;
}

QObject *AppController::spectrum()
{
    return m_spectrum;
}

void AppController::setState(const QString &state)
{
    if (m_state == state)
        return;
    m_state = state;
    if (state != QLatin1String("idle"))
        m_notice.clear();
    if (state != QLatin1String("listening")) {
        m_heardVoice = false;
        m_silenceMs = 0;
        m_listenMs = 0;
    }
    if (!m_state.startsWith(QLatin1String("speaking")) && state != QLatin1String("answering"))
        m_spectrum->setActive(m_state == QLatin1String("listening"));
    emit stateChanged();
}

void AppController::setQuestion(const QString &text)
{
    if (m_question == text)
        return;
    m_question = text;
    emit questionChanged();
}

void AppController::setAnswer(const QString &text)
{
    if (m_answer == text)
        return;
    m_answer = text;
    emit answerChanged();
}

void AppController::appendAnswer(const QString &delta)
{
    setAnswer(m_answer + delta);
    m_answerCarry += delta;

    int cut = sentenceEnd(m_answerCarry, 24);
    while (cut > 0) {
        const QString sentence = m_answerCarry.left(cut).trimmed();
        m_answerCarry.remove(0, cut);
        if (!sentence.isEmpty())
            enqueueSpeech(sentence);
        cut = sentenceEnd(m_answerCarry, 24);
    }
    // Long run-on text still needs to reach the speaker in digestible pieces.
    while (m_answerCarry.size() > 240) {
        int split = m_answerCarry.lastIndexOf(QLatin1Char(' '), 220);
        if (split < 40)
            split = 220;
        enqueueSpeech(m_answerCarry.left(split));
        m_answerCarry.remove(0, split);
    }
}

void AppController::setError(const QString &text)
{
    if (m_errorText == text)
        return;
    m_errorText = text;
    emit errorChanged();
}

void AppController::setLevel(qreal level)
{
    const qreal clamped = qBound(0.0, level, 1.0);
    if (qAbs(clamped - m_level) < 0.004)
        return;
    m_level = clamped;
    emit levelChanged();
}

void AppController::updateStatus()
{
    if (m_demo) {
        m_statusText = QStringLiteral("Demo mode  ·  scripted");
    } else {
        const QString asr = m_asrMode == QLatin1String("local")
            ? QStringLiteral("Voxtral 4B")
            : QStringLiteral("Voxtral realtime");
        const QString llm = m_llmMode == QLatin1String("ollama")
            ? m_llm->model().section(QLatin1Char(':'), 0, 0)
            : QStringLiteral("Mistral");
        m_statusText = QStringLiteral("%1  ·  %2").arg(asr, llm);
    }
    emit statusChanged();
}

void AppController::activate()
{
    m_notice.clear();
    if (!m_shown) {
        m_shown = true;
        emit shownChanged();
        emit requestShow();
    }
    startListening();
}

void AppController::toggle()
{
    if (m_shown && (m_state == QLatin1String("listening") || m_state == QLatin1String("idle")
                    || m_state == QLatin1String("speaking") || m_state == QLatin1String("error"))) {
        dismiss();
        return;
    }
    if (!m_shown)
        activate();
    else
        startListening();
}

void AppController::dismiss()
{
    stopListening();
    m_client.abortRequests();
    m_client.cancelStreaming();
    m_localAsr->cancel();
    m_llm->abort();
    m_tts->abort();
    if (m_player)
        m_player->stop();
    m_speechQueue.clear();
    m_audioQueue.clear();
    m_speechInFlight = false;
    if (m_demoTimer)
        m_demoTimer->stop();
    m_spectrum->reset();
    resetTurnBuffers();
    setQuestion(QString());
    setAnswer(QString());
    setError(QString());
    setLevel(0.0);

    if (m_shown) {
        m_shown = false;
        emit shownChanged();
        emit requestHide();
    }
    setState(QStringLiteral("hidden"));
}

void AppController::startListening()
{
    if (!m_shown) {
        activate();
        return;
    }
    m_resumeTimer->stop();
    resetTurnBuffers();
    setError(QString());
    setLevel(0.0);

    // Microphones come and go (USB, Bluetooth): re-check before every turn.
    const bool micNow = m_capture->available();
    if (micNow != m_micAvailable) {
        m_micAvailable = micNow;
        updateStatus();
    }
    m_voiceWarned = false;
    m_clearOnSpeech = true;

    if (m_demo) {
        demoStart();
        return;
    }

    setState(QStringLiteral("listening"));
    m_listenMs = 0;
    m_silenceMs = 0;
    m_heardVoice = false;

    if (!m_micAvailable && m_feedPcm.isEmpty()) {
        m_notice = QStringLiteral("No microphone — type your question and press Enter.");
        setState(QStringLiteral("idle"));
        emit stateChanged();
        return;
    }
    if (m_asrMode == QLatin1String("local"))
        m_localAsr->startStreaming();
    else
        m_client.startStreaming();
}

void AppController::stopListening()
{
    if (m_capture)
        m_capture->stop();
    if (m_asrMode == QLatin1String("local"))
        m_localAsr->endStreaming();
    else
        m_client.endStreaming();
}

void AppController::feedWav(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(QStringLiteral("Could not open %1").arg(path));
        return;
    }
    const WavData wav = WavCodec::decode(file.readAll());
    if (!wav.ok) {
        setError(QStringLiteral("Could not read %1: %2").arg(path, wav.error));
        return;
    }
    if (wav.sampleRate != AudioCapture::SampleRate || wav.channels != 1) {
        setError(QStringLiteral("Feed audio must be 16 kHz mono; got %1 Hz, %2 channels.")
                     .arg(wav.sampleRate).arg(wav.channels));
        return;
    }
    m_feedPcm = wav.pcm;
    m_feedPos = 0;
    if (m_state == QLatin1String("listening"))
        m_feedTimer->start(100);
}

void AppController::restartListening()
{
    if (m_state == QLatin1String("speaking") || m_state == QLatin1String("answering")) {
        interrupt();
        return;
    }
    startListening();
}

void AppController::endUtterance()
{
    if (m_state != QLatin1String("listening"))
        return;
    m_capture->stop();
    if (m_asrMode == QLatin1String("local"))
        m_localAsr->endStreaming();
    else
        m_client.endStreaming();
    setState(QStringLiteral("thinking"));

    // If the realtime session never produces a transcript, do not hang forever.
    QTimer::singleShot(6000, this, [this]() {
        if (m_state == QLatin1String("thinking") && m_question.trimmed().isEmpty()) {
            m_client.cancelStreaming();
            m_localAsr->cancel();
            setState(QStringLiteral("idle"));
            m_notice = QStringLiteral("The transcription session timed out. Try again.");
            emit stateChanged();
        }
    });
}

void AppController::onUtteranceDone(const QString &text)
{
    if (m_state != QLatin1String("thinking") && m_state != QLatin1String("listening"))
        return;
    m_client.cancelStreaming();
    m_localAsr->cancel();
    m_capture->stop();

    const QString finalText = text.trimmed().isEmpty() ? m_transcript.trimmed() : text.trimmed();
    if (finalText.isEmpty()) {
        setState(QStringLiteral("idle"));
        m_notice = QStringLiteral("I didn't catch that — press Ctrl+Space and try again.");
        emit stateChanged();
        return;
    }
    qInfo().noquote() << "[halo] heard:" << finalText;
    setQuestion(finalText);
    askQuestion(finalText);
}

void AppController::onAnswerDone(const QString &full)
{
    m_answerComplete = true;
    if (!m_answer.trimmed().isEmpty() && !full.trimmed().isEmpty()
        && m_answer.trimmed() != full.trimmed())
        setAnswer(full);
    if (!m_answerCarry.trimmed().isEmpty()) {
        enqueueSpeech(m_answerCarry);
        m_answerCarry.clear();
    }
    qInfo().noquote() << "[halo] answered:" << m_answer.trimmed().left(160);
    pushHistory(m_question, m_answer);
    if (m_answer.trimmed().isEmpty()) {
        setError(QStringLiteral("The model returned an empty answer."));
        setState(QStringLiteral("idle"));
        return;
    }
    finishTurnIfQuiet();
}

void AppController::askQuestion(const QString &text)
{
    m_answerComplete = false;
    m_lastQuestion = text;
    setAnswer(QString());
    setState(QStringLiteral("thinking"));
    if (m_llmMode == QLatin1String("ollama"))
        m_llm->ask(text, m_history);
    else
        m_client.ask(text, m_history);
}

void AppController::enqueueSpeech(const QString &sentence)
{
    if (m_demo)
        return;
    const QString clean = sanitiseForSpeech(sentence);
    if (clean.isEmpty())
        return;
    m_speechQueue.enqueue(clean);
    pumpSpeech();
}

void AppController::pumpSpeech()
{
    if (m_demo || m_speechInFlight || m_speechQueue.isEmpty())
        return;

    const bool noVoice = m_ttsMode == QLatin1String("none")
        || (m_ttsMode == QLatin1String("piper") && !m_tts->isConfigured())
        || (m_ttsMode == QLatin1String("mistral") && !m_client.hasKey());
    if (noVoice) {
        if (!m_voiceWarned) {
            m_voiceWarned = true;
            setError(QStringLiteral("No local voice is configured — the answer stays on screen."));
        }
        m_speechQueue.clear();
        m_speechInFlight = false;
        finishTurnIfQuiet();
        return;
    }

    const QString next = m_speechQueue.dequeue();
    m_speechInFlight = true;
    if (m_ttsMode == QLatin1String("piper"))
        m_tts->speak(next);
    else
        m_client.speak(next);
}

void AppController::pumpAudio()
{
    if (!m_player || m_player->isPlaying())
        return;
    if (m_audioQueue.isEmpty()) {
        finishTurnIfQuiet();
        return;
    }
    const QByteArray wav = m_audioQueue.dequeue();
    if (!m_player->playWav(wav)) {
        QTimer::singleShot(0, this, &AppController::pumpAudio);
        return;
    }
    setState(QStringLiteral("speaking"));
}

void AppController::onPlayerFinished()
{
    pumpAudio();
    if (!m_player->isPlaying() && m_audioQueue.isEmpty())
        finishTurnIfQuiet();
}

void AppController::finishTurnIfQuiet()
{
    if (!m_answerComplete)
        return;
    if (m_player && m_player->isPlaying())
        return;
    if (!m_audioQueue.isEmpty() || !m_speechQueue.isEmpty() || m_speechInFlight)
        return;
    if (m_state == QLatin1String("thinking") || m_state == QLatin1String("answering"))
        setState(QStringLiteral("speaking"));

    m_resumeTimer->stop();
    QTimer::singleShot(650, this, [this]() {
        if (!m_shown)
            return;
        if (m_player && m_player->isPlaying())
            return;
        endTurn();
    });
}

void AppController::endTurn()
{
    if (!m_shown)
        return;
    if (m_continuous && !m_demo) {
        m_notice.clear();
        startListening();
    } else {
        setState(QStringLiteral("idle"));
        setLevel(0.0);
    }
}

void AppController::interrupt()
{
    m_player->stop();
    m_client.abortRequests();
    m_client.cancelStreaming();
    m_localAsr->cancel();
    m_llm->abort();
    m_tts->abort();
    m_capture->stop();
    m_speechQueue.clear();
    m_audioQueue.clear();
    m_speechInFlight = false;
    m_spectrum->reset();
    if (!m_shown)
        return;
    if (m_demo) {
        demoStart();
        return;
    }
    startListening();
}

void AppController::submitText(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || !m_shown)
        return;
    setError(QString());
    m_clearOnSpeech = false;
    setQuestion(trimmed);
    m_answerComplete = false;
    setAnswer(QString());

    if (m_demo) {
        setState(QStringLiteral("thinking"));
        m_demoTimer->stop();
        m_demoPhase = 2;
        m_demoMs = 0;
        m_demoAnswer = QStringLiteral(
            "This is the offline demo, so I can only show the interface. With a Mistral API key, "
            "this question would go to %1 and the answer would be spoken back to you.")
            .arg(QStringLiteral("Voxtral"));
        m_demoChars = 0;
        m_demoTimer->start();
        return;
    }
    askQuestion(trimmed);
}

void AppController::clearForNewSpeech()
{
    m_clearOnSpeech = false;
    m_transcript.clear();
    setQuestion(QString());
    setAnswer(QString());
    m_answerComplete = false;
    m_answerCarry.clear();
}

void AppController::resetTurnBuffers()
{
    // The previous question and answer stay on screen until the next utterance
    // actually produces words (see clearForNewSpeech()).
    m_answerCarry.clear();
    m_sentenceCarry.clear();
    m_transcript.clear();
    m_answerComplete = false;
}

void AppController::pushHistory(const QString &question, const QString &answer)
{
    if (question.trimmed().isEmpty() || answer.trimmed().isEmpty())
        return;
    QJsonObject user;
    user["role"] = QStringLiteral("user");
    user["content"] = question.trimmed();
    QJsonObject assistant;
    assistant["role"] = QStringLiteral("assistant");
    assistant["content"] = answer.trimmed();
    m_history.append(user);
    m_history.append(assistant);
    while (m_history.size() > 12)
        m_history.removeFirst();
}

// ---------------------------------------------------------------- demo mode

void AppController::demoStart()
{
    m_demoCycle = (m_demoCycle + 1) % 3;
    m_demoPhase = 0;
    m_demoMs = 0;
    m_demoChars = 0;
    m_demoQuestion = QString::fromUtf8(kDemoQuestions[m_demoCycle]);
    m_demoAnswer = QString::fromUtf8(kDemoAnswers[m_demoCycle]);
    setQuestion(QString());
    setAnswer(QString());
    setState(QStringLiteral("listening"));
    setError(QString());
    m_demoTimer->start();
}

void AppController::demoTick()
{
    m_demoMs += 33;
    const qreal t = m_demoMs / 1000.0;

    auto speechLevel = [t]() {
        const qreal a = qAbs(qSin(t * 6.4));
        const qreal b = 0.55 + 0.45 * qSin(t * 1.7 + 1.2);
        return qBound(0.05, 0.22 + 0.62 * a * b, 1.0);
    };

    switch (m_demoPhase) {
    case 0: { // listening
        const int chars = int(qMax(0.0, (m_demoMs - 520) / 18.0));
        setQuestion(m_demoQuestion.left(chars));
        const qreal lvl = m_demoMs > 420 && m_demoMs < 2350 ? speechLevel() : 0.03;
        m_spectrum->pushLevel(lvl);
        setLevel(lvl);
        if (m_demoMs >= 2750) {
            m_demoPhase = 1;
            m_demoMs = 0;
            setState(QStringLiteral("thinking"));
            setLevel(0.0);
            m_spectrum->setActive(false);
        }
        break;
    }
    case 1: // thinking
        if (m_demoMs >= 850) {
            m_demoPhase = 2;
            m_demoMs = 0;
            m_demoChars = 0;
            setState(QStringLiteral("answering"));
        }
        break;
    case 2: { // answer typing
        m_demoChars = qMin(m_demoAnswer.size(), int(t * 52.0) + 1);
        setAnswer(m_demoAnswer.left(m_demoChars));
        if (m_demoChars >= m_demoAnswer.size()) {
            m_demoPhase = 3;
            m_demoMs = 0;
            setState(QStringLiteral("speaking"));
        }
        break;
    }
    case 3: { // speaking
        const qreal lvl = speechLevel();
        m_spectrum->pushLevel(lvl);
        setLevel(lvl);
        if (m_demoMs >= 4200) {
            m_demoPhase = 4;
            m_demoMs = 0;
            m_spectrum->setActive(false);
            setLevel(0.0);
            setState(QStringLiteral("idle"));
        }
        break;
    }
    default: // rest, then the next question
        if (m_demoMs >= 1500)
            demoStart();
        break;
    }
}
