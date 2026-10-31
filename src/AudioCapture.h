#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>

class QAudioSource;
class QIODevice;

// 16 kHz mono signed-16 microphone capture, delivering ~100 ms chunks that map
// one-to-one onto the realtime transcription frames.
class AudioCapture : public QObject
{
    Q_OBJECT
public:
    static constexpr int SampleRate = 16000;

    explicit AudioCapture(QObject *parent = nullptr);
    ~AudioCapture() override;

    bool available() const;
    QString deviceName() const;
    bool isActive() const { return m_source != nullptr; }

    void start();
    void stop();

signals:
    void pcm(const QByteArray &chunk);
    void level(float rms01);
    void failed(const QString &message);

private:
    void onReadyRead();
    void flushChunk();

    QAudioSource *m_source = nullptr;
    QIODevice *m_io = nullptr;
    QByteArray m_pending;
    QString m_deviceName;
    bool m_started = false;
    quint64 m_totalBytes = 0;
};
