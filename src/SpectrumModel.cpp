#include "SpectrumModel.h"

#include <QtMath>
#include <algorithm>

namespace {

constexpr int kFftSize = 512;

// In-place iterative radix-2 Cooley-Tukey FFT over real input packed as
// complex pairs (imaginary parts zero).
void fft(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * float(M_PI) / float(len);
        const float wr = std::cos(ang);
        const float wi = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                const float ur = re[i + k];
                const float ui = im[i + k];
                const float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                const float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr;
                im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr;
                im[i + k + len / 2] = ui - vi;
                const float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

} // namespace

SpectrumModel::SpectrumModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_value(BandCount, 0.0f)
    , m_peak(BandCount, 0.0f)
{
}

int SpectrumModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : BandCount;
}

QVariant SpectrumModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= BandCount)
        return {};
    switch (role) {
    case LevelRole:
        return double(m_value.at(index.row()));
    case PeakRole:
        return double(m_peak.at(index.row()));
    default:
        return {};
    }
}

QHash<int, QByteArray> SpectrumModel::roleNames() const
{
    return { { LevelRole, QByteArrayLiteral("level") },
             { PeakRole, QByteArrayLiteral("peak") } };
}

void SpectrumModel::pushPcm(const QByteArray &pcm, int sampleRate, int channels)
{
    if (pcm.size() < 4 || sampleRate <= 0)
        return;
    channels = std::max(1, channels);

    const int availableFrames = pcm.size() / (2 * channels);
    const int count = std::min(kFftSize, availableFrames);
    if (count < 64)
        return;

    // Take the most recent window so the visual follows what is heard now.
    const int startFrame = availableFrames - count;
    for (int i = 0; i < kFftSize; ++i)
        m_sampleWindow[i] = 0.0f;

    float mean = 0.0f;
    for (int i = 0; i < count; ++i) {
        float sum = 0.0f;
        for (int c = 0; c < channels; ++c) {
            const int offset = ((startFrame + i) * channels + c) * 2;
            const qint16 s = qint16(quint16(quint8(pcm.at(offset)))
                                    | (quint16(quint8(pcm.at(offset + 1))) << 8));
            sum += float(s);
        }
        m_sampleWindow[i] = sum / (32768.0f * channels);
        mean += m_sampleWindow[i];
    }
    mean /= count;

    const int n = kFftSize;
    QVector<float> re(n), im(n, 0.0f);
    for (int i = 0; i < n; ++i) {
        // Hann window plus DC removal.
        const float w = 0.5f - 0.5f * std::cos(2.0f * float(M_PI) * i / (n - 1));
        re[i] = (m_sampleWindow[i] - mean) * w;
    }
    fft(re.data(), im.data(), n);

    const float binHz = float(sampleRate) / float(n);
    const float nyquist = sampleRate / 2.0f;
    const float lowHz = 70.0f;
    const float highHz = std::min(7600.0f, nyquist * 0.96f);

    QVector<float> target(BandCount, 0.0f);
    for (int b = 0; b < BandCount; ++b) {
        const float f0 = lowHz * std::pow(highHz / lowHz, float(b) / BandCount);
        const float f1 = lowHz * std::pow(highHz / lowHz, float(b + 1) / BandCount);
        int k0 = std::max(1, int(f0 / binHz));
        int k1 = std::min(n / 2 - 1, std::max(k0, int(f1 / binHz)));
        float energy = 0.0f;
        int bins = 0;
        for (int k = k0; k <= k1; ++k) {
            energy += re[k] * re[k] + im[k] * im[k];
            ++bins;
        }
        if (bins == 0)
            continue;
        const float mag = std::sqrt(energy / bins) / (n * 0.25f);
        // Perceptual curve: bass always carries more energy, so tilt the bands
        // toward the top and compress the dynamic range for a lively shape.
        const float tilt = 0.55f + 0.95f * (float(b) / BandCount);
        const float v = std::pow(std::clamp(mag * tilt * 2.4f, 0.0f, 1.0f), 0.62f);
        target[b] = v;
    }
    commit(target, 0.62f, 0.14f);
}

void SpectrumModel::pushLevel(float level)
{
    // Used when only an envelope is available (demo mode): a slow travelling
    // curve keeps the bar alive without pretending to be a real spectrum.
    QVector<float> target(BandCount, 0.0f);
    const float v = std::clamp(level, 0.0f, 1.0f);
    for (int b = 0; b < BandCount; ++b) {
        const float x = float(b) / (BandCount - 1);
        const float shape = 0.35f + 0.65f * std::sin(float(M_PI) * std::pow(x, 0.85f));
        target[b] = std::clamp(v * shape, 0.0f, 1.0f);
    }
    commit(target, 0.6f, 0.16f);
}

void SpectrumModel::commit(const QVector<float> &target, float attack, float release)
{
    for (int i = 0; i < BandCount; ++i) {
        const float t = target.at(i);
        const float k = t > m_value[i] ? attack : release;
        m_value[i] += (t - m_value[i]) * k;
        m_peak[i] = std::max(m_peak[i] * 0.94f, m_value[i]);
    }
    m_active = true;
    emit dataChanged(index(0), index(BandCount - 1), { LevelRole, PeakRole });
}

void SpectrumModel::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    if (!active)
        reset();
}

void SpectrumModel::reset()
{
    for (int i = 0; i < BandCount; ++i) {
        m_value[i] = 0.0f;
        m_peak[i] = 0.0f;
    }
    emit dataChanged(index(0), index(BandCount - 1), { LevelRole, PeakRole });
}
