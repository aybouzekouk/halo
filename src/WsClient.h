#pragma once

#include <QObject>
#include <QUrl>
#include <QMap>
#include <QByteArray>

class QTcpSocket;

// Minimal RFC 6455 WebSocket client (text + binary + ping/pong + fragmentation)
// built on QSslSocket so the app needs no extra Qt module.
class WsClient : public QObject
{
    Q_OBJECT
public:
    explicit WsClient(QObject *parent = nullptr);
    ~WsClient() override;

    void open(const QUrl &url, const QMap<QString, QString> &headers = {});
    void close();
    void sendText(const QString &text);
    void sendBinary(const QByteArray &data);

    bool isOpen() const { return m_open; }
    QString errorString() const { return m_error; }

signals:
    void opened();
    void closed();
    void textMessage(const QString &text);
    void binaryMessage(const QByteArray &data);
    void failed(const QString &message);

private:
    void onConnected();
    void onReadyRead();
    void onDisconnected();
    void sendFrame(quint8 opcode, const QByteArray &payload);
    void processFrames();
    void fail(const QString &message);
    void dropSocket();

    QTcpSocket *m_socket = nullptr;
    QByteArray m_buffer;      // frame bytes (after the handshake)
    QByteArray m_handshake;   // bytes until the end of the HTTP response
    QByteArray m_fragment;    // reassembly buffer
    quint8 m_fragmentOpcode = 0;
    QUrl m_url;
    QByteArray m_key;
    QMap<QString, QString> m_headers;
    QString m_error;
    bool m_open = false;
    bool m_handshakeDone = false;
    bool m_closing = false;
    bool m_failed = false;
};
