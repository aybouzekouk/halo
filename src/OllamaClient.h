#pragma once

#include <QObject>
#include <QJsonArray>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

// Local answers through Ollama's HTTP API (streaming NDJSON), so the whole
// conversation can run on this machine.
class OllamaClient : public QObject
{
    Q_OBJECT
public:
    explicit OllamaClient(QObject *parent = nullptr);

    void setEndpoint(const QUrl &url) { m_endpoint = url; }
    void setModel(const QString &model) { m_model = model; }
    QString model() const { return m_model; }

    void ask(const QString &prompt, const QJsonArray &history);
    void abort();
    bool busy() const { return m_reply != nullptr; }

signals:
    void answerDelta(const QString &delta);
    void answerDone(const QString &fullText);
    void failed(const QString &message);

private:
    QNetworkAccessManager *m_net = nullptr;
    QNetworkReply *m_reply = nullptr;
    QUrl m_endpoint;
    QString m_model;
    QByteArray m_buffer;
    QString m_answer;
};
