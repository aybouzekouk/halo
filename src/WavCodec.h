#pragma once

#include <QByteArray>
#include <QString>

// Small WAV reader: enough for the 16-bit PCM (and 8-bit / float32) files the
// speech endpoints return. Decoded audio is always exposed as mono-aware
// 16-bit signed PCM at the file's own sample rate.
struct WavData
{
    QByteArray pcm;   // interleaved s16le
    int sampleRate = 0;
    int channels = 0;
    int bits = 0;
    bool ok = false;
    QString error;

    int bytesPerFrame() const { return channels * 2; }
    qint64 frameCount() const { return bytesPerFrame() > 0 ? pcm.size() / bytesPerFrame() : 0; }
    qint64 durationMs() const { return sampleRate > 0 ? (frameCount() * 1000) / sampleRate : 0; }
};

namespace WavCodec {
WavData decode(const QByteArray &bytes);
QByteArray encode(const QByteArray &pcmS16, int sampleRate, int channels);
} // namespace WavCodec
