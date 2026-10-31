#include "WsClient.h"

#include <QTcpSocket>
#include <QSslSocket>
#include <QSslError>
#include <QRandomGenerator>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

namespace {
constexpr quint64 kMaxFrame = 32u * 1024u * 1024u; // guard against corrupt length fields

QByteArray frameHeader(quint8 opcode, bool fin, quint64 length, bool masked)
{
    QByteArray h;
    h.reserve(14);
    h.append(char((fin ? 0x80 : 0x00) | (opcode & 0x0f)));
    const quint8 maskBit = masked ? 0x80 : 0x00;
    if (length < 126) {
        h.append(char(maskBit | quint8(length)));
    } else if (length <= 0xffff) {
        h.append(char(maskBit | 126));
        h.append(char((length >> 8) & 0xff));
        h.append(char(length & 0xff));
    } else {
        h.append(char(maskBit | 127));
        for (int i = 7; i >= 0; --i)
            h.append(char((length >> (8 * i)) & 0xff));
    }
    return h;
}
} // namespace

WsClient::WsClient(QObject *parent)
    : QObject(parent)
{
}

WsClient::~WsClient()
{
    dropSocket();
}

void WsClient::open(const QUrl &url, const QMap<QString, QString> &headers)
{
    dropSocket();
    m_url = url;
    m_headers = headers;
    m_error.clear();
    m_buffer.clear();
    m_handshake.clear();
    m_fragment.clear();
    m_open = false;
    m_handshakeDone = false;
    m_closing = false;
    m_failed = false;

    const bool secure = url.scheme() == QLatin1String("wss");
    if (!url.isValid() || (!secure && url.scheme() != QLatin1String("ws"))) {
        fail(QStringLiteral("Unsupported websocket url: %1").arg(url.toString()));
        return;
    }

    quint32 r[4];
    for (int i = 0; i < 4; ++i)
        r[i] = QRandomGenerator::global()->generate();
    m_key = QByteArray(reinterpret_cast<const char *>(r), 16).toBase64();

    QTcpSocket *socket = nullptr;
    if (secure) {
        auto *tls = new QSslSocket(this);
        tls->setPeerVerifyMode(QSslSocket::VerifyPeer);
        connect(tls, &QSslSocket::encrypted, this, &WsClient::onConnected);
        connect(tls, &QSslSocket::sslErrors, this, [this](const QList<QSslError> &errors) {
            if (!errors.isEmpty())
                fail(QStringLiteral("TLS: %1").arg(errors.first().errorString()));
        });
        socket = tls;
    } else {
        socket = new QTcpSocket(this);
    }

    m_socket = socket;
    connect(socket, &QTcpSocket::readyRead, this, &WsClient::onReadyRead);
    connect(socket, &QTcpSocket::disconnected, this, &WsClient::onDisconnected);
    connect(socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (!m_open)
            fail(m_socket ? m_socket->errorString() : QStringLiteral("socket error"));
    });
    if (!secure)
        connect(socket, &QTcpSocket::connected, this, &WsClient::onConnected);

    const quint16 port = quint16(url.port(secure ? 443 : 80));
    if (url.host().isEmpty()) {
        fail(QStringLiteral("Missing websocket host"));
        return;
    }
    if (secure)
        static_cast<QSslSocket *>(socket)->connectToHostEncrypted(url.host(), port, url.host());
    else
        socket->connectToHost(url.host(), port);
}

void WsClient::onConnected()
{
    if (!m_socket)
        return;

    QString path = m_url.path();
    if (path.isEmpty())
        path = QStringLiteral("/");
    if (m_url.hasQuery())
        path += QLatin1Char('?') + m_url.query(QUrl::FullyEncoded);

    QString host = m_url.host();
    const quint16 port = quint16(m_url.port(m_url.scheme() == QLatin1String("wss") ? 443 : 80));
    const quint16 defaultPort = m_url.scheme() == QLatin1String("wss") ? 443 : 80;
    if (port != defaultPort)
        host += QLatin1Char(':') + QString::number(port);

    QByteArray req;
    req += "GET " + path.toUtf8() + " HTTP/1.1\r\n";
    req += "Host: " + host.toUtf8() + "\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + m_key + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    req += "User-Agent: Halo/0.1\r\n";
    for (auto it = m_headers.cbegin(); it != m_headers.cend(); ++it)
        req += it.key().toUtf8() + ": " + it.value().toUtf8() + "\r\n";
    req += "\r\n";
    m_socket->write(req);
    m_socket->flush();
}

void WsClient::onReadyRead()
{
    if (!m_socket)
        return;

    const QByteArray chunk = m_socket->readAll();
    if (chunk.isEmpty())
        return;

    if (!m_handshakeDone) {
        m_handshake += chunk;
        const int end = m_handshake.indexOf("\r\n\r\n");
        if (end < 0) {
            if (m_handshake.size() > 64 * 1024)
                fail(QStringLiteral("Handshake response too large"));
            return;
        }
        const QByteArray head = m_handshake.left(end);
        const QByteArray rest = m_handshake.mid(end + 4);
        m_handshake.clear();

        const QList<QByteArray> lines = head.split('\n');
        if (lines.isEmpty() || !lines.first().contains("101")) {
            fail(QStringLiteral("WebSocket upgrade refused: %1")
                     .arg(QString::fromUtf8(lines.value(0)).trimmed()));
            return;
        }
        QByteArray accept;
        for (const QByteArray &line : lines) {
            const int colon = line.indexOf(':');
            if (colon <= 0)
                continue;
            if (line.left(colon).trimmed().toLower() == "sec-websocket-accept")
                accept = line.mid(colon + 1).trimmed();
        }
        const QByteArray expected =
            QCryptographicHash::hash(m_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
                                     QCryptographicHash::Sha1)
                .toBase64();
        if (!accept.isEmpty() && accept != expected) {
            fail(QStringLiteral("WebSocket accept key mismatch"));
            return;
        }
        m_handshakeDone = true;
        m_open = true;
        m_buffer = rest;
        emit opened();
    } else {
        m_buffer += chunk;
    }
    processFrames();
}

void WsClient::processFrames()
{
    while (m_buffer.size() >= 2) {
        const quint8 b0 = quint8(m_buffer.at(0));
        const quint8 b1 = quint8(m_buffer.at(1));
        const bool fin = (b0 & 0x80) != 0;
        const quint8 opcode = b0 & 0x0f;
        const bool masked = (b1 & 0x80) != 0;
        quint64 length = quint64(b1 & 0x7f);
        int offset = 2;

        if (length == 126) {
            if (m_buffer.size() < 4)
                return;
            length = quint64((quint8(m_buffer.at(2)) << 8) | quint8(m_buffer.at(3)));
            offset = 4;
        } else if (length == 127) {
            if (m_buffer.size() < 10)
                return;
            length = 0;
            for (int i = 0; i < 8; ++i)
                length = (length << 8) | quint8(m_buffer.at(2 + i));
            offset = 10;
        }
        if (length > kMaxFrame) {
            fail(QStringLiteral("WebSocket frame too large"));
            return;
        }

        QByteArray mask;
        if (masked) {
            if (m_buffer.size() < offset + 4)
                return;
            mask = m_buffer.mid(offset, 4);
            offset += 4;
        }
        if (quint64(m_buffer.size()) < quint64(offset) + length)
            return;

        QByteArray payload = m_buffer.mid(offset, int(length));
        m_buffer.remove(0, offset + int(length));
        if (masked) {
            for (int i = 0; i < payload.size(); ++i)
                payload[i] = char(payload.at(i) ^ mask.at(i % 4));
        }

        switch (opcode) {
        case 0x0: // continuation
            if (!m_fragmentOpcode) {
                fail(QStringLiteral("Unexpected websocket continuation frame"));
                return;
            }
            m_fragment += payload;
            if (fin) {
                const QByteArray whole = m_fragment;
                const quint8 op = m_fragmentOpcode;
                m_fragment.clear();
                m_fragmentOpcode = 0;
                if (op == 0x1)
                    emit textMessage(QString::fromUtf8(whole));
                else
                    emit binaryMessage(whole);
            }
            break;
        case 0x1:
        case 0x2:
            if (fin) {
                if (opcode == 0x1)
                    emit textMessage(QString::fromUtf8(payload));
                else
                    emit binaryMessage(payload);
            } else {
                m_fragmentOpcode = opcode;
                m_fragment = payload;
            }
            break;
        case 0x8: // close
            m_closing = true;
            sendFrame(0x8, QByteArray());
            if (m_socket)
                m_socket->disconnectFromHost();
            break;
        case 0x9: // ping
            sendFrame(0xA, payload);
            break;
        case 0xA: // pong
            break;
        default:
            break;
        }
    }
}

void WsClient::sendText(const QString &text)
{
    if (!m_open)
        return;
    sendFrame(0x1, text.toUtf8());
}

void WsClient::sendBinary(const QByteArray &data)
{
    if (!m_open)
        return;
    sendFrame(0x2, data);
}

void WsClient::sendFrame(quint8 opcode, const QByteArray &payload)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return;

    QByteArray mask(4, Qt::Uninitialized);
    const quint32 r = QRandomGenerator::global()->generate();
    mask[0] = char(r & 0xff);
    mask[1] = char((r >> 8) & 0xff);
    mask[2] = char((r >> 16) & 0xff);
    mask[3] = char((r >> 24) & 0xff);

    QByteArray frame = frameHeader(opcode, true, quint64(payload.size()), true);
    frame.append(mask);
    QByteArray body = payload;
    for (int i = 0; i < body.size(); ++i)
        body[i] = char(body.at(i) ^ mask.at(i % 4));
    frame.append(body);
    m_socket->write(frame);
    m_socket->flush();
}

void WsClient::close()
{
    if (!m_socket)
        return;
    m_closing = true;
    if (m_open) {
        sendFrame(0x8, QByteArray());
        m_open = false;
    }
    m_socket->disconnectFromHost();
}

void WsClient::dropSocket()
{
    m_open = false;
    m_handshakeDone = false;
    if (!m_socket)
        return;
    m_socket->disconnect(this);
    m_socket->abort();
    m_socket->deleteLater();
    m_socket = nullptr;
}

void WsClient::onDisconnected()
{
    const bool wasOpen = m_open;
    m_open = false;
    m_handshakeDone = false;
    if (wasOpen || !m_failed)
        emit closed();
    if (m_socket) {
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

void WsClient::fail(const QString &message)
{
    if (m_failed)
        return;
    m_failed = true;
    m_error = message;
    m_open = false;
    m_handshakeDone = false;
    qWarning() << "[ws]" << message;
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    emit failed(message);
}
