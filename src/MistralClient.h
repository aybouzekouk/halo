#pragma once

#include <QObject>
#include <QByteArray>
#include <QJsonArray>
#include <QStringList>

class WsClient;
class QNetworkAccessManager;
class QNetworkReply;

// Talks to Mistral: Voxtral realtime transcription over a websocket, chat
// completions over SSE, and Voxtral speech generation.
class MistralClient : public QObject
{
    Q_OBJECT
public:
    struct Config
    {
        QString apiKey;
        QString asrModel = QStringLiteral("voxtral-mini-transcribe-realtime-3");
        QString chatModel = QStringLiteral("mistral-small-latest");
        QString ttsModel = QStringLiteral("voxtral-mini-tts-3-0");
        QString voiceId = QStringLiteral("voice_478");
        QString language = QStringLiteral("en");
        QStringList contextBias;
    };

    explicit MistralClient(QObject *parent = nullptr);

    void setConfig(const Config &config);
    const Config &config() const { return m_config; }
    bool hasKey() const { return !m_config.apiKey.trimmed().isEmpty(); }

    void startStreaming();
    void pushAudio(const QByteArray &pcm);
    void endStreaming();
    void cancelStreaming();
    bool isStreaming() const;

    void ask(const QString &prompt, const QJsonArray &history);
    void speak(const QString &text);
    void abortRequests();

signals:
    void streamOpening();
    void streamReady();
    void streamClosed();
    void transcriptDelta(const QString &delta);
    void utteranceDone(const QString &text);

    void answerDelta(const QString &delta);
    void answerDone(const QString &fullText);

    void speechReady(const QByteArray &wav);
    void failed(const QString &message);

private:
    void onWsText(const QString &message);
    void sendSessionUpdate();
    void fail(const QString &message);

    Config m_config;
    WsClient *m_ws = nullptr;
    bool m_sessionUpdated = false;
    QByteArray m_pendingAudio;

    QNetworkAccessManager *m_net = nullptr;
    QNetworkReply *m_chatReply = nullptr;
    QNetworkReply *m_ttsReply = nullptr;
    QByteArray m_sseBuffer;
    QString m_answer;
};
