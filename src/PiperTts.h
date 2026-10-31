#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>

class QProcess;

// Local speech synthesis through Piper: text in, 16-bit mono PCM out, wrapped
// in a WAV container so the existing player and spectrum code can use it.
class PiperTts : public QObject
{
    Q_OBJECT
public:
    explicit PiperTts(QObject *parent = nullptr);

    void setExecutable(const QString &path) { m_executable = path; }
    void setVoice(const QString &voicePath);
    QString executable() const { return m_executable; }
    QString voice() const { return m_voice; }

    bool isConfigured() const;
    bool busy() const { return m_process != nullptr; }

    void speak(const QString &text);
    void abort();

signals:
    void speechReady(const QByteArray &wav);
    void failed(const QString &message);

private:
    QString m_executable;
    QString m_voice;
    int m_sampleRate = 22050;
    QProcess *m_process = nullptr;
    QByteArray m_pcm;
};
