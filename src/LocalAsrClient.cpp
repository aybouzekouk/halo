#include "LocalAsrClient.h"
#include "WsClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

LocalAsrClient::LocalAsrClient(QObject *parent)
    : QObject(parent)
{
    m_endpoint = QUrl(QStringLiteral("ws://127.0.0.1:8765"));
    m_ws = new WsClient(this);
    connect(m_ws, &WsClient::opened, this, [this]() {
        m_ready = false;
        QJsonObject hello;
        hello["type"] = QStringLiteral("hello");
        hello["client"] = QStringLiteral("halo/0.1");
        m_ws->sendText(QString::fromUtf8(QJsonDocument(hello).toJson(QJsonDocument::Compact)));
        emit streamOpening();
    });
    connect(m_ws, &WsClient::textMessage, this, &LocalAsrClient::onMessage);
    connect(m_ws, &WsClient::closed, this, [this]() {
        m_ready = false;
        if (m_ending) {
            m_ending = false;
            emit utteranceDone(QString());
        }
    });
    connect(m_ws, &WsClient::failed, this, [this](const QString &message) {
        m_ready = false;
        m_ending = false;
        emit failed(QStringLiteral("Local voice model: %1").arg(message));
    });
}

void LocalAsrClient::startStreaming()
{
    cancel();
    m_pendingAudio.clear();
    m_ready = false;
    m_ending = false;
    m_ws->open(m_endpoint);
}

void LocalAsrClient::onMessage(const QString &payload)
{
    const QJsonObject event = QJsonDocument::fromJson(payload.toUtf8()).object();
    const QString type = event.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("ready")) {
        m_ready = true;
        emit streamReady();
        if (!m_pendingAudio.isEmpty()) {
            const QByteArray pending = m_pendingAudio;
            m_pendingAudio.clear();
            m_ws->sendBinary(pending);
        }
    } else if (type == QLatin1String("status")) {
        emit status(event.value(QStringLiteral("message")).toString());
    } else if (type == QLatin1String("partial")) {
        emit partialText(event.value(QStringLiteral("text")).toString());
    } else if (type == QLatin1String("final")) {
        m_ready = false;
        m_ending = false;
        const QString text = event.value(QStringLiteral("text")).toString().trimmed();
        emit utteranceDone(text);
        m_ws->close();
    } else if (type == QLatin1String("error")) {
        emit failed(event.value(QStringLiteral("message")).toString());
    }
}

void LocalAsrClient::pushAudio(const QByteArray &pcm)
{
    if (pcm.isEmpty())
        return;
    if (!m_ready || !m_ws->isOpen()) {
        m_pendingAudio += pcm;
        if (m_pendingAudio.size() > 16000 * 2 * 4)
            m_pendingAudio = m_pendingAudio.right(16000 * 2 * 4);
        return;
    }
    m_ws->sendBinary(pcm);
}

void LocalAsrClient::endStreaming()
{
    if (!m_ws->isOpen()) {
        emit utteranceDone(QString());
        return;
    }
    m_ending = true;
    QJsonObject message;
    message["type"] = QStringLiteral("stop");
    m_ws->sendText(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

void LocalAsrClient::cancel()
{
    m_ready = false;
    m_ending = false;
    m_pendingAudio.clear();
    if (m_ws->isOpen())
        m_ws->close();
}

bool LocalAsrClient::isStreaming() const
{
    return m_ws->isOpen();
}
