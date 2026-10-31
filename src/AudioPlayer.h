#pragma once

#include <QObject>
#include <QByteArray>

class QAudioSink;
class QIODevice;
class QTimer;
class QProcess;
class QTemporaryFile;
class QElapsedTimer;

// Plays a decoded WAV (any 16 kHz..48 kHz mono/stereo 16-bit) and reports the
// exact playback position so the UI can render a real, audio-synced spectrum.
class AudioPlayer : public QObject
{
    Q_OBJECT
public:
    explicit AudioPlayer(QObject *parent = nullptr);
    ~AudioPlayer() override;

    bool playWav(const QByteArray &wavBytes);
    void stop();
    bool isPlaying() const { return m_sink != nullptr || m_external; }

signals:
    void started();
    void finished();
    void pcmAtPosition(const QByteArray &chunk, int sampleRate, int channels);
    void failed(const QString &message);

private:
    bool startExternal(const QByteArray &wavBytes, int rate, int channels);
    void cleanup();

private:
    void tick();
    void finish();

    QAudioSink *m_sink = nullptr;
    QIODevice *m_feed = nullptr;
    QTimer *m_timer = nullptr;
    QByteArray m_pcm;
    int m_rate = 0;
    int m_channels = 1;
    qint64 m_reportedFrame = 0;
    qint64 m_lastProcessedUs = 0;
    bool m_done = false;
    bool m_external = false;
    QProcess *m_player = nullptr;
    QTemporaryFile *m_temp = nullptr;
    QElapsedTimer *m_clock = nullptr;
};
