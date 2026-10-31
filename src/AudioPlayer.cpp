#include "AudioPlayer.h"
#include "WavCodec.h"

#include <QAudioSink>
#include <QAudioFormat>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QBuffer>
#include <QTimer>
#include <QtMath>
#include <QDebug>

AudioPlayer::AudioPlayer(QObject *parent)
    : QObject(parent)
{
    m_timer = new QTimer(this);
    m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, &AudioPlayer::tick);
}

AudioPlayer::~AudioPlayer()
{
    stop();
}

bool AudioPlayer::playWav(const QByteArray &wavBytes)
{
    stop();

    const WavData wav = WavCodec::decode(wavBytes);
    if (!wav.ok) {
        emit failed(wav.error);
        return false;
    }

    const QAudioDevice output = QMediaDevices::defaultAudioOutput();
    if (output.isNull()) {
        emit failed(QStringLiteral("No audio output device is available."));
        return false;
    }

    QAudioFormat fmt;
    fmt.setSampleRate(wav.sampleRate);
    fmt.setChannelCount(wav.channels);
    fmt.setSampleFormat(QAudioFormat::Int16);
    if (!output.isFormatSupported(fmt)) {
        fmt = output.preferredFormat();
        fmt.setChannelCount(1);
        fmt.setSampleFormat(QAudioFormat::Int16);
    }

    m_pcm = wav.pcm;
    m_rate = fmt.sampleRate();
    m_channels = fmt.channelCount();
    m_reportedFrame = 0;
    m_lastProcessedUs = 0;
    m_done = false;

    auto *buffer = new QBuffer(this);
    buffer->setData(m_pcm);
    if (!buffer->open(QIODevice::ReadOnly)) {
        emit failed(QStringLiteral("Could not buffer the speech audio."));
        delete buffer;
        return false;
    }
    m_feed = buffer;

    m_sink = new QAudioSink(output, fmt, this);
    m_sink->setBufferSize(qMax(4096, fmt.bytesForDuration(250000)));
    m_sink->start(m_feed);

    if (m_sink->error() != QAudio::NoError) {
        emit failed(QStringLiteral("Could not start audio playback."));
        stop();
        return false;
    }

    m_timer->start();
    emit started();
    return true;
}

void AudioPlayer::tick()
{
    if (!m_sink)
        return;

    const qint64 processed = m_sink->processedUSecs();
    const qint64 frame = (processed * qint64(m_rate)) / 1000000;
    const qint64 totalFrames = m_channels > 0 ? m_pcm.size() / (2 * m_channels) : 0;

    if (frame > m_reportedFrame) {
        // Emit the window that just became audible, so the visualisation sits
        // exactly on the sound rather than ahead of it.
        const qint64 from = qMin(m_reportedFrame, totalFrames);
        const qint64 to = qMin(frame, totalFrames);
        const int bytesPerFrame = 2 * m_channels;
        if (to > from) {
            const QByteArray chunk =
                m_pcm.mid(int(from) * bytesPerFrame, int(to - from) * bytesPerFrame);
            if (!chunk.isEmpty())
                emit pcmAtPosition(chunk, m_rate, m_channels);
        }
        m_reportedFrame = frame;
    }
    m_lastProcessedUs = processed;

    const bool feedFinished = !m_feed || m_feed->atEnd();
    if (feedFinished && frame >= totalFrames - 1 && m_sink->state() == QAudio::IdleState)
        finish();
}

void AudioPlayer::finish()
{
    if (m_done)
        return;
    m_done = true;
    m_timer->stop();
    if (m_sink) {
        m_sink->stop();
        m_sink->deleteLater();
        m_sink = nullptr;
    }
    if (m_feed) {
        m_feed->deleteLater();
        m_feed = nullptr;
    }
    emit finished();
}

void AudioPlayer::stop()
{
    if (m_timer)
        m_timer->stop();
    if (m_sink) {
        m_sink->stop();
        m_sink->deleteLater();
        m_sink = nullptr;
    }
    if (m_feed) {
        m_feed->deleteLater();
        m_feed = nullptr;
    }
    m_pcm.clear();
    m_done = true;
}
