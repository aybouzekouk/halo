#pragma once

#include <QObject>
#include <QString>
#include <QJsonArray>
#include <QQueue>

#include "MistralClient.h"

class AudioCapture;
class AudioPlayer;
class SpectrumModel;
class GlobalHotkey;
class LocalAsrClient;
class OllamaClient;
class PiperTts;
class QTimer;

// Orchestrates the whole voice turn: hotkey -> capture -> Voxtral realtime
// transcript -> chat answer -> Voxtral speech, plus a self-contained demo mode
// so the interface can be used (and reviewed) without a key or a microphone.
class AppController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString phaseLabel READ phaseLabel NOTIFY stateChanged)
    Q_PROPERTY(QString question READ question NOTIFY questionChanged)
    Q_PROPERTY(QString answer READ answer NOTIFY answerChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString hintText READ hintText NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorChanged)
    Q_PROPERTY(bool demo READ demo NOTIFY statusChanged)
    Q_PROPERTY(bool shown READ shown NOTIFY shownChanged)
    Q_PROPERTY(bool micAvailable READ micAvailable NOTIFY statusChanged)
    Q_PROPERTY(bool reducedMotion READ reducedMotion CONSTANT)
    Q_PROPERTY(qreal level READ level NOTIFY levelChanged)
    Q_PROPERTY(QObject *spectrum READ spectrum CONSTANT)
    Q_PROPERTY(bool speaking READ speaking NOTIFY stateChanged)

public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    QString state() const { return m_state; }
    QString phaseLabel() const;
    QString question() const { return m_question; }
    QString answer() const { return m_answer; }
    QString statusText() const { return m_statusText; }
    QString hintText() const;
    QString errorText() const { return m_errorText; }
    bool demo() const { return m_demo; }
    bool shown() const { return m_shown; }
    bool micAvailable() const;
    bool reducedMotion() const { return m_reducedMotion; }
    qreal level() const { return m_level; }
    QObject *spectrum();
    bool speaking() const { return m_state == QLatin1String("speaking"); }

    Q_INVOKABLE void activate();
    Q_INVOKABLE void toggle();
    Q_INVOKABLE void dismiss();
    Q_INVOKABLE void interrupt();
    Q_INVOKABLE void submitText(const QString &text);
    Q_INVOKABLE void restartListening();
    Q_INVOKABLE void feedWav(const QString &path);

signals:
    void stateChanged();
    void questionChanged();
    void answerChanged();
    void statusChanged();
    void errorChanged();
    void shownChanged();
    void levelChanged();
    void requestShow();
    void requestHide();

private:
    void startListening();
    void stopListening();
    void endUtterance();
    void onUtteranceDone(const QString &text);
    void onAnswerDone(const QString &full);
    void askQuestion(const QString &text);
    void enqueueSpeech(const QString &sentence);
    void pumpSpeech();
    void pumpAudio();
    void onPlayerFinished();
    void endTurn();
    void finishTurnIfQuiet();
    void setState(const QString &state);
    void setQuestion(const QString &text);
    void setAnswer(const QString &text);
    void appendAnswer(const QString &delta);
    void setError(const QString &text);
    void setLevel(qreal level);
    void pushHistory(const QString &question, const QString &answer);
    void resetTurnBuffers();
    void clearForNewSpeech();
    void updateStatus();

    void demoStart();
    void demoTick();

    MistralClient m_client;
    LocalAsrClient *m_localAsr = nullptr;
    OllamaClient *m_llm = nullptr;
    PiperTts *m_tts = nullptr;
    AudioCapture *m_capture = nullptr;
    AudioPlayer *m_player = nullptr;
    SpectrumModel *m_spectrum = nullptr;
    GlobalHotkey *m_hotkey = nullptr;

    QString m_asrMode;
    QString m_llmMode;
    QString m_ttsMode;

    QString m_state = QStringLiteral("hidden");
    QString m_question;
    QString m_answer;
    QString m_statusText;
    QString m_errorText;
    QString m_notice;
    QString m_sentenceCarry;
    QString m_answerCarry;
    QString m_transcript;
    qreal m_level = 0.0;
    bool m_shown = false;
    bool m_demo = false;
    bool m_reducedMotion = false;
    bool m_micAvailable = false;
    bool m_continuous = true;

    QQueue<QString> m_speechQueue;
    QQueue<QByteArray> m_audioQueue;
    bool m_speechInFlight = false;
    bool m_answerComplete = false;
    bool m_voiceWarned = false;
    bool m_clearOnSpeech = false;

    // utterance detection
    bool m_heardVoice = false;
    int m_silenceMs = 0;
    int m_listenMs = 0;

    QJsonArray m_history;
    QString m_lastQuestion;
    QString m_lastAnswer;

    // demo timeline
    QTimer *m_demoTimer = nullptr;
    int m_demoMs = 0;
    int m_demoPhase = 0;
    int m_demoChars = 0;
    int m_demoCycle = 0;
    QString m_demoQuestion;
    QString m_demoAnswer;

    QTimer *m_resumeTimer = nullptr;
    QTimer *m_feedTimer = nullptr;
    QByteArray m_feedPcm;
    int m_feedPos = 0;
};
