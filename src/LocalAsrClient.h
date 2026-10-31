#pragma once

#include <QObject>
#include <QUrl>
#include <QByteArray>

class WsClient;

// Client for the local Voxtral sidecar (python/asr_server.py). It speaks a
// deliberately tiny protocol: audio in as binary frames, text out as events.
class LocalAsrClient : public QObject
{
    Q_OBJECT
public:
    explicit LocalAsrClient(QObject *parent = nullptr);

    void setEndpoint(const QUrl &url) { m_endpoint = url; }
    QUrl endpoint() const { return m_endpoint; }

    void startStreaming();
    void pushAudio(const QByteArray &pcm);
    void endStreaming();
    void cancel();

    bool isStreaming() const;

signals:
    void streamOpening();
    void streamReady();
    void status(const QString &message);
    void partialText(const QString &text);
    void utteranceDone(const QString &text);
    void failed(const QString &message);

private:
    void onMessage(const QString &json);

    WsClient *m_ws = nullptr;
    QUrl m_endpoint;
    bool m_ready = false;
    QByteArray m_pendingAudio;
    bool m_ending = false;
};
