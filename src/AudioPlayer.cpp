#include "AudioPlayer.h"
#include "WavCodec.h"

#include <QAudioSink>
#include <QAudioFormat>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QBuffer>
#include <QVector>
#include <QProcess>
#include <QTemporaryFile>
#include <QElapsedTimer>
#include <QStandardPaths>
#include <QDir>
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

namespace {

QAudioFormat makeFormat(int rate, int channels)
{
    QAudioFormat f;
    f.setSampleRate(rate);
    f.setChannelCount(channels);
    f.setSampleFormat(QAudioFormat::Int16);
    return f;
}

// Speech-quality conversion: decode to mono floats, linear-resample, spread
// across the sink's channel count. Good enough for 16-24 kHz speech and it
// removes any dependency on the device accepting the file's own format.
QByteArray convertPcm(const QByteArray &pcm, int srcRate, int srcChannels,
                      int dstRate, int dstChannels)
{
    const int srcFrames = srcChannels > 0 ? pcm.size() / (2 * srcChannels) : 0;
    if (srcFrames <= 0 || srcRate <= 0 || dstRate <= 0 || dstChannels <= 0)
        return {};

    QVector<float> mono(srcFrames);
    for (int i = 0; i < srcFrames; ++i) {
        float sum = 0.0f;
        for (int c = 0; c < srcChannels; ++c) {
            const int offset = (i * srcChannels + c) * 2;
            const qint16 v = qint16(quint16(quint8(pcm.at(offset)))
                                    | (quint16(quint8(pcm.at(offset + 1))) << 8));
            sum += v / 32768.0f;
        }
        mono[i] = sum / float(srcChannels);
    }

    const int dstFrames = int(qint64(srcFrames) * dstRate / srcRate);
    QByteArray out(dstFrames * dstChannels * 2, Qt::Uninitialized);
    for (int i = 0; i < dstFrames; ++i) {
        const double pos = double(i) * srcRate / dstRate;
        const int i0 = qBound(0, int(pos), srcFrames - 1);
        const int i1 = qMin(i0 + 1, srcFrames - 1);
        const float frac = float(pos - i0);
        const float sample = mono[i0] * (1.0f - frac) + mono[i1] * frac;
        const qint16 v = qint16(qBound(-32768.0f, sample * 32767.0f, 32767.0f));
        for (int c = 0; c < dstChannels; ++c) {
            const int offset = (i * dstChannels + c) * 2;
            out[offset] = char(v & 0xff);
            out[offset + 1] = char((v >> 8) & 0xff);
        }
    }
    return out;
}

} // namespace

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

    // Prefer the file's own layout, then the formats every desktop sink
    // accepts. A device's "preferred" format is not trusted: some HDMI
    // endpoints report zero channels and an unusable sample format.
    QList<QAudioFormat> candidates;
    candidates << makeFormat(wav.sampleRate, wav.channels)
               << makeFormat(wav.sampleRate, 1)
               << makeFormat(48000, 2) << makeFormat(48000, 1)
               << makeFormat(44100, 2) << makeFormat(44100, 1)
               << makeFormat(24000, 1) << makeFormat(16000, 1);

    QAudioFormat chosen;
    for (const QAudioFormat &candidate : candidates) {
        if (output.isFormatSupported(candidate)) {
            chosen = candidate;
            break;
        }
    }
    if (!chosen.isValid())
        chosen = makeFormat(48000, 2); // let the sound server convert

    m_pcm = convertPcm(wav.pcm, wav.sampleRate, wav.channels,
                       chosen.sampleRate(), chosen.channelCount());
    if (m_pcm.isEmpty()) {
        emit failed(QStringLiteral("Could not convert the speech audio for playback."));
        return false;
    }
    m_rate = chosen.sampleRate();
    m_channels = chosen.channelCount();
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

    m_sink = new QAudioSink(output, chosen, this);
    m_sink->setBufferSize(qMax(4096, chosen.bytesForDuration(250000)));
    m_sink->start(m_feed);

    if (m_sink->error() != QAudio::NoError) {
        // Some sinks (notably HDMI endpoints reported through PipeWire) expose
        // no usable QAudioFormat at all. Hand the file to the session's player
        // instead and drive the spectrum from the wall clock.
        qWarning() << "[audio] QAudioSink refused the format, falling back to pw-play";
        cleanup();
        if (startExternal(wavBytes, wav.sampleRate, wav.channels)) {
            emit started();
            return true;
        }
        emit failed(QStringLiteral("No audio output is available for the voice."));
        return false;
    }

    m_timer->start();
    emit started();
    return true;
}

bool AudioPlayer::startExternal(const QByteArray &wavBytes, int rate, int channels)
{
    const QString player = QStandardPaths::findExecutable(QStringLiteral("pw-play"));
    if (player.isEmpty())
        return false;

    auto *temp = new QTemporaryFile(QDir::tempPath() + QStringLiteral("/halo-voice-XXXXXX.wav"), this);
    if (!temp->open())
        return false;
    temp->write(wavBytes);
    temp->flush();

    auto *process = new QProcess(this);
    process->setProgram(player);
    process->setArguments({ temp->fileName() });
    process->start();
    if (!process->waitForStarted(1500)) {
        process->deleteLater();
        temp->deleteLater();
        return false;
    }

    m_player = process;
    m_temp = temp;
    m_external = true;
    m_done = false;

    // Play from the file as decoded, so the spectrum window matches the audio.
    const WavData wav = WavCodec::decode(wavBytes);
    m_pcm = wav.pcm;
    m_rate = rate;
    m_channels = channels;
    m_reportedFrame = 0;

    m_clock = new QElapsedTimer;
    m_clock->start();
    m_timer->start();
    return true;
}

void AudioPlayer::tick()
{
    if (m_external) {
        const qint64 totalFrames = m_channels > 0 ? m_pcm.size() / (2 * m_channels) : 0;
        const qint64 frame = m_clock ? (m_clock->elapsed() * qint64(m_rate)) / 1000 : 0;
        const qint64 from = qMin(m_reportedFrame, totalFrames);
        const qint64 to = qMin(frame, totalFrames);
        if (to > from) {
            const int bytesPerFrame = 2 * m_channels;
            const QByteArray chunk = m_pcm.mid(int(from) * bytesPerFrame,
                                               int(to - from) * bytesPerFrame);
            if (!chunk.isEmpty())
                emit pcmAtPosition(chunk, m_rate, m_channels);
            m_reportedFrame = frame;
        }
        if (frame >= totalFrames && totalFrames > 0)
            finish();
        return;
    }

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
    cleanup();
    emit finished();
}

void AudioPlayer::cleanup()
{
    if (m_sink) {
        m_sink->stop();
        m_sink->deleteLater();
        m_sink = nullptr;
    }
    if (m_feed) {
        m_feed->deleteLater();
        m_feed = nullptr;
    }
    if (m_player) {
        m_player->kill();
        m_player->waitForFinished(300);
        m_player->deleteLater();
        m_player = nullptr;
    }
    if (m_temp) {
        m_temp->deleteLater();
        m_temp = nullptr;
    }
    if (m_clock) {
        delete m_clock;
        m_clock = nullptr;
    }
    m_external = false;
}

void AudioPlayer::stop()
{
    if (m_timer)
        m_timer->stop();
    cleanup();
    m_pcm.clear();
    m_done = true;
}
