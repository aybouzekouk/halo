#include "MistralClient.h"
#include "WsClient.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrlQuery>
#include <QDebug>

namespace {
constexpr char kApiBase[] = "https://api.mistral.ai";
constexpr char kRealtimePath[] = "/v1/audio/transcriptions/realtime";

const char *kSystemPrompt =
    "You are Halo, a calm, precise voice assistant. The user is speaking out loud and your "
    "reply will be read aloud by a speech synthesiser while it is also shown on screen. "
    "Answer in one to three short sentences unless the user asks for detail. Never use markdown "
    "headings, bullet lists, tables, emoji, or code fences. Spell out symbols and numbers the way "
    "a person would say them. If you are unsure, say so briefly instead of inventing an answer.";
} // namespace

MistralClient::MistralClient(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
    m_ws = new WsClient(this);
    connect(m_ws, &WsClient::opened, this, [this]() {
        m_sessionUpdated = false;
        m_pendingAudio.clear();
        sendSessionUpdate();
    });
    connect(m_ws, &WsClient::textMessage, this, &MistralClient::onWsText);
    connect(m_ws, &WsClient::closed, this, [this]() {
        m_sessionUpdated = false;
        emit streamClosed();
    });
    connect(m_ws, &WsClient::failed, this, [this](const QString &message) {
        m_sessionUpdated = false;
        emit failed(QStringLiteral("Realtime transcription: %1").arg(message));
    });
}

void MistralClient::setConfig(const Config &config)
{
    m_config = config;
}

void MistralClient::sendSessionUpdate()
{
    QJsonObject audioFormat;
    audioFormat["encoding"] = QStringLiteral("pcm_s16le");
    audioFormat["sample_rate"] = 16000;

    QJsonObject turnDetection;
    turnDetection["start_threshold"] = 0.5;
    turnDetection["eager_end_threshold"] = 0.45;
    turnDetection["end_threshold"] = 0.33;
    turnDetection["min_silence_ms"] = 700;
    turnDetection["end_timeout_ms"] = 2000;

    QJsonObject session;
    session["audio_format"] = audioFormat;
    session["target_streaming_delay_ms"] = 480;
    session["turn_detection"] = turnDetection;
    if (!m_config.language.trimmed().isEmpty())
        session["language"] = m_config.language.trimmed();
    if (!m_config.contextBias.isEmpty()) {
        QJsonArray bias;
        for (const QString &term : m_config.contextBias) {
            if (!term.trimmed().isEmpty())
                bias.append(term.trimmed());
        }
        session["context_bias"] = bias;
    }

    QJsonObject message;
    message["type"] = QStringLiteral("session.update");
    message["session"] = session;

    m_ws->sendText(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

void MistralClient::startStreaming()
{
    if (!hasKey()) {
        emit failed(QStringLiteral("No Mistral API key is configured."));
        return;
    }
    cancelStreaming();

    QUrl url(QString::fromLatin1(kApiBase));
    url.setScheme(QStringLiteral("wss"));
    url.setPath(QString::fromLatin1(kRealtimePath));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("model"), m_config.asrModel);
    url.setQuery(query);

    QMap<QString, QString> headers;
    headers[QStringLiteral("Authorization")] = QStringLiteral("Bearer ") + m_config.apiKey;
    m_sessionUpdated = false;
    emit streamOpening();
    m_ws->open(url, headers);
}

bool MistralClient::isStreaming() const
{
    return m_ws->isOpen();
}

void MistralClient::pushAudio(const QByteArray &pcm)
{
    if (pcm.isEmpty())
        return;
    if (!m_sessionUpdated) {
        m_pendingAudio += pcm;
        // Never hoard more than a few seconds while the session handshakes.
        const int maxBytes = 16000 * 2 * 4;
        if (m_pendingAudio.size() > maxBytes)
            m_pendingAudio = m_pendingAudio.right(maxBytes);
        return;
    }
    QJsonObject message;
    message["type"] = QStringLiteral("input_audio.append");
    message["audio"] = QString::fromLatin1(pcm.toBase64());
    m_ws->sendText(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

void MistralClient::endStreaming()
{
    if (!m_ws->isOpen())
        return;
    if (!m_sessionUpdated) {
        // Nothing was sent yet: closing is cheaper than an empty session.
        cancelStreaming();
        return;
    }
    QJsonObject message;
    message["type"] = QStringLiteral("input_audio.end");
    m_ws->sendText(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

void MistralClient::cancelStreaming()
{
    m_pendingAudio.clear();
    m_sessionUpdated = false;
    if (m_ws->isOpen())
        m_ws->close();
}

void MistralClient::onWsText(const QString &message)
{
    const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject())
        return;
    const QJsonObject obj = doc.object();
    const QString type = obj.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("session.created")) {
        sendSessionUpdate();
    } else if (type == QLatin1String("session.updated")) {
        m_sessionUpdated = true;
        emit streamReady();
        if (!m_pendingAudio.isEmpty()) {
            const QByteArray pending = m_pendingAudio;
            m_pendingAudio.clear();
            pushAudio(pending);
        }
    } else if (type == QLatin1String("transcription.text.delta")) {
        emit transcriptDelta(obj.value(QStringLiteral("text")).toString());
    } else if (type == QLatin1String("transcription.done")) {
        const QString text = obj.value(QStringLiteral("text")).toString().trimmed();
        emit utteranceDone(text);
    } else if (type == QLatin1String("error")) {
        const QJsonObject error = obj.value(QStringLiteral("error")).toObject();
        const QString detail = error.value(QStringLiteral("message")).toString();
        qWarning() << "[mistral] realtime error:" << detail;
        emit failed(detail.isEmpty() ? QStringLiteral("Realtime session error") : detail);
    }
}

void MistralClient::ask(const QString &prompt, const QJsonArray &history)
{
    if (!hasKey()) {
        emit failed(QStringLiteral("No Mistral API key is configured."));
        return;
    }

    if (m_chatReply) {
        m_chatReply->abort();
        m_chatReply->deleteLater();
        m_chatReply = nullptr;
    }

    QJsonArray messages;
    QJsonObject system;
    system["role"] = QStringLiteral("system");
    system["content"] = QString::fromLatin1(kSystemPrompt);
    messages.append(system);
    for (const QJsonValue &value : history)
        messages.append(value);

    QJsonObject user;
    user["role"] = QStringLiteral("user");
    user["content"] = prompt;
    messages.append(user);

    QJsonObject body;
    body["model"] = m_config.chatModel;
    body["messages"] = messages;
    body["stream"] = true;
    body["temperature"] = 0.3;
    body["max_tokens"] = 320;

    QNetworkRequest request(QUrl(QString::fromLatin1(kApiBase) + QStringLiteral("/v1/chat/completions")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", "Bearer " + m_config.apiKey.toUtf8());
    request.setRawHeader("Accept", "text/event-stream");
    request.setTransferTimeout(60000);

    m_sseBuffer.clear();
    m_answer.clear();
    m_chatReply = m_net->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));

    QNetworkReply *reply = m_chatReply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        m_sseBuffer += reply->readAll();
        while (true) {
            const int nl = m_sseBuffer.indexOf('\n');
            if (nl < 0)
                break;
            QByteArray line = m_sseBuffer.left(nl).trimmed();
            m_sseBuffer.remove(0, nl + 1);
            if (!line.startsWith("data:"))
                continue;
            line = line.mid(5).trimmed();
            if (line.isEmpty() || line == "[DONE]")
                continue;
            const QJsonDocument eventDoc = QJsonDocument::fromJson(line);
            const QJsonObject event = eventDoc.object();
            if (event.contains(QStringLiteral("error"))) {
                emit failed(event.value(QStringLiteral("error")).toObject()
                                .value(QStringLiteral("message"))
                                .toString());
                continue;
            }
            const QJsonArray choices = event.value(QStringLiteral("choices")).toArray();
            if (choices.isEmpty())
                continue;
            const QJsonObject delta = choices.first().toObject()
                                          .value(QStringLiteral("delta")).toObject();
            const QString piece = delta.value(QStringLiteral("content")).toString();
            if (!piece.isEmpty()) {
                m_answer += piece;
                emit answerDelta(piece);
            }
        }
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray rest = reply->readAll();
        if (!rest.isEmpty() && m_sseBuffer.size() + rest.size() < 4 * 1024 * 1024) {
            m_sseBuffer += rest;
            // Flush any trailing data line without a newline.
            const int nl = m_sseBuffer.indexOf('\n');
            if (nl > 0) {
                QByteArray line = m_sseBuffer.left(nl).trimmed();
                if (line.startsWith("data:")) {
                    line = line.mid(5).trimmed();
                    const QJsonObject event = QJsonDocument::fromJson(line).object();
                    const QJsonArray choices = event.value(QStringLiteral("choices")).toArray();
                    if (!choices.isEmpty()) {
                        const QString piece = choices.first().toObject()
                                                  .value(QStringLiteral("delta")).toObject()
                                                  .value(QStringLiteral("content")).toString();
                        if (!piece.isEmpty()) {
                            m_answer += piece;
                            emit answerDelta(piece);
                        }
                    }
                }
            }
        }

        if (reply == m_chatReply)
            m_chatReply = nullptr;
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError && reply->error() != QNetworkReply::OperationCanceledError) {
            emit failed(QStringLiteral("Answer request failed: %1").arg(reply->errorString()));
            return;
        }
        emit answerDone(m_answer.trimmed());
    });
}

void MistralClient::speak(const QString &text)
{
    if (!hasKey() || text.trimmed().isEmpty()) {
        emit failed(QStringLiteral("Cannot synthesise speech."));
        return;
    }
    if (m_ttsReply) {
        m_ttsReply->abort();
        m_ttsReply->deleteLater();
        m_ttsReply = nullptr;
    }

    QJsonObject body;
    body["model"] = m_config.ttsModel;
    body["input"] = text;
    body["voice_id"] = m_config.voiceId;
    body["response_format"] = QStringLiteral("wav");
    body["stream"] = false;

    QNetworkRequest request(QUrl(QString::fromLatin1(kApiBase) + QStringLiteral("/v1/audio/speech")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", "Bearer " + m_config.apiKey.toUtf8());
    request.setTransferTimeout(45000);

    m_ttsReply = m_net->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QNetworkReply *reply = m_ttsReply;

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray payload = reply->readAll();
        if (reply == m_ttsReply)
            m_ttsReply = nullptr;
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit failed(QStringLiteral("Speech request failed: %1").arg(reply->errorString()));
            return;
        }

        QByteArray wav = payload;
        const QByteArray contentType = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
        if (contentType.contains("json") || payload.startsWith('{')) {
            const QJsonObject obj = QJsonDocument::fromJson(payload).object();
            const QByteArray data = obj.value(QStringLiteral("audio_data")).toString().toLatin1();
            wav = QByteArray::fromBase64(data);
        }
        if (wav.isEmpty()) {
            emit failed(QStringLiteral("Speech response contained no audio."));
            return;
        }
        emit speechReady(wav);
    });
}

void MistralClient::abortRequests()
{
    if (m_chatReply) {
        m_chatReply->abort();
        m_chatReply->deleteLater();
        m_chatReply = nullptr;
    }
    if (m_ttsReply) {
        m_ttsReply->abort();
        m_ttsReply->deleteLater();
        m_ttsReply = nullptr;
    }
}

void MistralClient::fail(const QString &message)
{
    emit failed(message);
}
