#include "OllamaClient.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

namespace {
const char *kSystemPrompt =
    "You are Halo, a calm, precise voice assistant running privately on this computer. "
    "The user is speaking out loud and your reply is read aloud by a speech synthesiser while "
    "it is also shown on screen. Answer in one to three short sentences unless the user asks for "
    "detail. Never use markdown headings, bullet lists, tables, emoji, or code fences. Spell out "
    "symbols and numbers the way a person would say them. If you are unsure, say so briefly "
    "instead of inventing an answer.";
} // namespace

OllamaClient::OllamaClient(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
    , m_endpoint(QStringLiteral("http://127.0.0.1:11434"))
    , m_model(QStringLiteral("ministral-3:3b"))
{
}

void OllamaClient::ask(const QString &prompt, const QJsonArray &history)
{
    abort();
    if (prompt.trimmed().isEmpty())
        return;

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

    QJsonObject options;
    options["temperature"] = 0.3;
    options["num_predict"] = 320;

    QJsonObject body;
    body["model"] = m_model;
    body["messages"] = messages;
    body["stream"] = true;
    body["options"] = options;

    QUrl url = m_endpoint;
    url.setPath(QStringLiteral("/api/chat"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(120000);

    m_buffer.clear();
    m_answer.clear();
    m_reply = m_net->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QNetworkReply *reply = m_reply;

    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        m_buffer += reply->readAll();
        int nl;
        while ((nl = m_buffer.indexOf('\n')) >= 0) {
            const QByteArray line = m_buffer.left(nl).trimmed();
            m_buffer.remove(0, nl + 1);
            if (line.isEmpty())
                continue;
            const QJsonObject event = QJsonDocument::fromJson(line).object();
            if (event.contains(QStringLiteral("error"))) {
                emit failed(event.value(QStringLiteral("error")).toString());
                continue;
            }
            const QJsonObject message = event.value(QStringLiteral("message")).toObject();
            const QString piece = message.value(QStringLiteral("content")).toString();
            if (!piece.isEmpty()) {
                m_answer += piece;
                emit answerDelta(piece);
            }
        }
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply == m_reply)
            m_reply = nullptr;
        const QNetworkReply::NetworkError error = reply->error();
        const QString errorText = reply->errorString();
        reply->deleteLater();

        if (error != QNetworkReply::NoError && error != QNetworkReply::OperationCanceledError) {
            emit failed(QStringLiteral("Ollama: %1").arg(errorText));
            return;
        }
        emit answerDone(m_answer.trimmed());
    });
}

void OllamaClient::abort()
{
    if (!m_reply)
        return;
    m_reply->abort();
    m_reply->deleteLater();
    m_reply = nullptr;
}
