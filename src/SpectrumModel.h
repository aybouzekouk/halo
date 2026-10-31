#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QVector>

// Thirty-two smoothed energy bands, driven either by real PCM (a small FFT) or
// by a single level value. The QML bar visualisation binds straight to this.
class SpectrumModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum { BandCount = 32 };
    enum Roles { LevelRole = Qt::UserRole + 1, PeakRole };

    explicit SpectrumModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void pushPcm(const QByteArray &pcmS16, int sampleRate, int channels);
    void pushLevel(float level);
    void setActive(bool active);
    void reset();

private:
    void commit(const QVector<float> &target, float attack, float release);

    QVector<float> m_value;
    QVector<float> m_peak;
    float m_sampleWindow[512] = {};
    bool m_active = false;
};
