#include "AudioCapture.h"

#include <QAudioSource>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QIODevice>
#include <QtMath>
#include <QDebug>

namespace {
constexpr int kChunkBytes = 3200; // 100 ms of 16 kHz mono s16
}

AudioCapture::AudioCapture(QObject *parent)
    : QObject(parent)
{
    const QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (!device.isNull())
        m_deviceName = device.description();
}

AudioCapture::~AudioCapture()
{
    stop();
}

bool AudioCapture::available() const
{
    const QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (device.isNull())
        return false;

    QAudioFormat fmt;
    fmt.setSampleRate(SampleRate);
    fmt.setChannelCount(1);
    fmt.setSampleFormat(QAudioFormat::Int16);
    return device.isFormatSupported(fmt) || !device.supportedSampleFormats().isEmpty();
}

QString AudioCapture::deviceName() const
{
    return m_deviceName.isEmpty() ? QStringLiteral("no input device") : m_deviceName;
}

void AudioCapture::start()
{
    if (m_source)
        return;

    const QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (device.isNull()) {
        emit failed(QStringLiteral("No microphone was found."));
        return;
    }
    m_deviceName = device.description();

    QAudioFormat fmt;
    fmt.setSampleRate(SampleRate);
    fmt.setChannelCount(1);
    fmt.setSampleFormat(QAudioFormat::Int16);
    if (!device.isFormatSupported(fmt)) {
        fmt = device.preferredFormat();
        fmt.setChannelCount(1);
        if (!device.isFormatSupported(fmt)) {
            emit failed(QStringLiteral("The microphone does not accept a mono PCM format."));
            return;
        }
    }

    m_source = new QAudioSource(device, fmt, this);
    m_source->setBufferSize(kChunkBytes * 6);
    m_io = m_source->start();
    if (!m_io) {
        emit failed(QStringLiteral("Could not start audio capture: %1").arg(m_source->error()));
        m_source->deleteLater();
        m_source = nullptr;
        return;
    }
    m_started = true;
    m_pending.clear();
    connect(m_io, &QIODevice::readyRead, this, &AudioCapture::onReadyRead);
    connect(m_source, &QAudioSource::stateChanged, this, [this](QAudio::State state) {
        if (state == QAudio::StoppedState && m_source && m_source->error() != QAudio::NoError)
            emit failed(QStringLiteral("Microphone stopped unexpectedly."));
    });
    qInfo() << "[audio] capturing from" << m_deviceName << fmt.sampleRate() << "Hz";
}

void AudioCapture::stop()
{
    if (m_source) {
        m_source->stop();
        m_source->deleteLater();
        m_source = nullptr;
    }
    m_io = nullptr;
    m_pending.clear();
    m_started = false;
}

void AudioCapture::onReadyRead()
{
    if (!m_io)
        return;
    m_pending += m_io->readAll();
    while (m_pending.size() >= kChunkBytes) {
        const QByteArray chunk = m_pending.left(kChunkBytes);
        m_pending.remove(0, kChunkBytes);
        m_totalBytes += quint64(chunk.size());

        // RMS over the chunk, mapped perceptually onto 0..1.
        double sum = 0.0;
        const int samples = chunk.size() / 2;
        for (int i = 0; i < samples; ++i) {
            const qint16 s = qint16(quint16(quint8(chunk.at(i * 2)))
                                    | (quint16(quint8(chunk.at(i * 2 + 1))) << 8));
            const double v = s / 32768.0;
            sum += v * v;
        }
        const double rms = samples > 0 ? std::sqrt(sum / samples) : 0.0;
        const float lvl = float(std::min(1.0, std::pow(rms * 5.5, 0.72)));

        emit level(lvl);
        emit pcm(chunk);
    }
}

void AudioCapture::flushChunk()
{
    if (!m_pending.isEmpty()) {
        emit pcm(m_pending);
        m_pending.clear();
    }
}
