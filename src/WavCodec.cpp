#include "WavCodec.h"

#include <QtEndian>
#include <cmath>

namespace {

quint32 rd32(const QByteArray &b, int o)
{
    return quint32(quint8(b.at(o))) | (quint32(quint8(b.at(o + 1))) << 8)
         | (quint32(quint8(b.at(o + 2))) << 16) | (quint32(quint8(b.at(o + 3))) << 24);
}

quint16 rd16(const QByteArray &b, int o)
{
    return quint16(quint16(quint8(b.at(o))) | (quint16(quint8(b.at(o + 1))) << 8));
}

void wr32(QByteArray &b, quint32 v)
{
    b.append(char(v & 0xff));
    b.append(char((v >> 8) & 0xff));
    b.append(char((v >> 16) & 0xff));
    b.append(char((v >> 24) & 0xff));
}

void wr16(QByteArray &b, quint16 v)
{
    b.append(char(v & 0xff));
    b.append(char((v >> 8) & 0xff));
}

} // namespace

namespace WavCodec {

WavData decode(const QByteArray &bytes)
{
    WavData w;
    if (bytes.size() < 44) {
        w.error = QStringLiteral("Audio too small to be a WAV file");
        return w;
    }
    if (bytes.left(4) != "RIFF" || bytes.mid(8, 4) != "WAVE") {
        w.error = QStringLiteral("Not a RIFF/WAVE payload");
        return w;
    }

    int pos = 12;
    quint16 format = 1;
    int blockAlign = 0;
    QByteArray data;

    while (pos + 8 <= bytes.size()) {
        const QByteArray id = bytes.mid(pos, 4);
        const quint32 size = rd32(bytes, pos + 4);
        const int body = pos + 8;
        if (size > quint32(bytes.size() - body))
            break;

        if (id == "fmt ") {
            if (size < 16)
                break;
            format = rd16(bytes, body);
            w.channels = rd16(bytes, body + 2);
            w.sampleRate = int(rd32(bytes, body + 4));
            blockAlign = rd16(bytes, body + 12);
            w.bits = rd16(bytes, body + 14);
            if (format == 0xFFFE && size >= 40)
                format = rd16(bytes, body + 24); // WAVE_FORMAT_EXTENSIBLE sub-format
        } else if (id == "data") {
            data = bytes.mid(body, int(size));
        }

        pos = body + int(size) + int(size & 1);
    }

    if (w.channels <= 0 || w.sampleRate <= 0) {
        w.error = QStringLiteral("WAV header is missing a format chunk");
        return w;
    }
    if (data.isEmpty()) {
        w.error = QStringLiteral("WAV file has no audio data");
        return w;
    }
    if (blockAlign <= 0)
        blockAlign = w.channels * (w.bits / 8);

    if (format == 1 && w.bits == 16) {
        w.pcm = data;
    } else if (format == 1 && w.bits == 8) {
        w.pcm.resize(data.size() * 2);
        for (int i = 0; i < data.size(); ++i) {
            const qint16 s = qint16((int(quint8(data.at(i))) - 128) << 8);
            w.pcm[i * 2] = char(s & 0xff);
            w.pcm[i * 2 + 1] = char((s >> 8) & 0xff);
        }
    } else if (format == 1 && w.bits == 24) {
        const int frames = data.size() / 3;
        w.pcm.resize(frames * 2);
        for (int i = 0; i < frames; ++i) {
            const qint32 s = (qint32(quint8(data.at(i * 3 + 2))) << 24)
                           | (qint32(quint8(data.at(i * 3 + 1))) << 16)
                           | (qint32(quint8(data.at(i * 3))) << 8);
            const qint16 v = qint16(s >> 16);
            w.pcm[i * 2] = char(v & 0xff);
            w.pcm[i * 2 + 1] = char((v >> 8) & 0xff);
        }
    } else if (format == 3 && w.bits == 32) {
        const int n = data.size() / 4;
        w.pcm.resize(n * 2);
        for (int i = 0; i < n; ++i) {
            float f;
            const quint32 bits32 = rd32(data, i * 4);
            memcpy(&f, &bits32, 4);
            f = std::max(-1.0f, std::min(1.0f, f));
            const qint16 v = qint16(std::lround(f * 32767.0f));
            w.pcm[i * 2] = char(v & 0xff);
            w.pcm[i * 2 + 1] = char((v >> 8) & 0xff);
        }
    } else {
        w.error = QStringLiteral("Unsupported WAV encoding (format %1, %2 bit)")
                      .arg(format)
                      .arg(w.bits);
        return w;
    }

    w.bits = 16;
    w.ok = true;
    return w;
}

QByteArray encode(const QByteArray &pcmS16, int sampleRate, int channels)
{
    const quint32 dataSize = quint32(pcmS16.size());
    QByteArray b;
    b.reserve(int(dataSize) + 44);
    b.append("RIFF");
    wr32(b, dataSize + 36);
    b.append("WAVE");
    b.append("fmt ");
    wr32(b, 16);
    wr16(b, 1);
    wr16(b, quint16(channels));
    wr32(b, quint32(sampleRate));
    wr32(b, quint32(sampleRate * channels * 2));
    wr16(b, quint16(channels * 2));
    wr16(b, 16);
    b.append("data");
    wr32(b, dataSize);
    b.append(pcmS16);
    return b;
}

} // namespace WavCodec
