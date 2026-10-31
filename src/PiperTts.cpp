#include "PiperTts.h"
#include "WavCodec.h"

#include <QProcess>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

PiperTts::PiperTts(QObject *parent)
    : QObject(parent)
{
}

void PiperTts::setVoice(const QString &voicePath)
{
    m_voice = voicePath;
    m_sampleRate = 22050;

    QString configPath = voicePath;
    configPath.replace(QStringLiteral(".onnx"), QStringLiteral(".json"));
    QFile config(configPath);
    if (config.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(config.readAll()).object();
        const int rate = root.value(QStringLiteral("audio")).toObject()
                             .value(QStringLiteral("sample_rate")).toInt();
        if (rate > 0)
            m_sampleRate = rate;
    }
}

bool PiperTts::isConfigured() const
{
    return !m_executable.isEmpty() && QFileInfo::exists(m_executable)
        && !m_voice.isEmpty() && QFileInfo::exists(m_voice);
}

void PiperTts::speak(const QString &text)
{
    abort();
    if (!isConfigured()) {
        emit failed(QStringLiteral("Piper is not configured."));
        return;
    }
    if (text.trimmed().isEmpty())
        return;

    m_pcm.clear();
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    QStringList args;
    args << QStringLiteral("--model") << m_voice
         << QStringLiteral("--output-raw")
         << QStringLiteral("--length_scale") << QStringLiteral("1.0");
    m_process->start(m_executable, args);
    if (!m_process->waitForStarted(4000)) {
        emit failed(QStringLiteral("Could not start Piper: %1").arg(m_process->errorString()));
        m_process->deleteLater();
        m_process = nullptr;
        return;
    }

    connect(m_process, &QProcess::readyReadStandardOutput, this, [this]() {
        if (m_process)
            m_pcm += m_process->readAllStandardOutput();
    });
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        QProcess *process = m_process;
        m_process = nullptr;
        if (!process)
            return;
        m_pcm += process->readAllStandardOutput();
        const QByteArray errors = process->readAllStandardError();
        process->deleteLater();

        if (status != QProcess::NormalExit || code != 0 || m_pcm.isEmpty()) {
            emit failed(QStringLiteral("Piper failed: %1")
                            .arg(QString::fromUtf8(errors).trimmed().left(200)));
            return;
        }
        const QByteArray wav = WavCodec::encode(m_pcm, m_sampleRate, 1);
        m_pcm.clear();
        emit speechReady(wav);
    });

    m_process->write(text.toUtf8());
    m_process->closeWriteChannel();
}

void PiperTts::abort()
{
    if (!m_process)
        return;
    m_process->kill();
    m_process->deleteLater();
    m_process = nullptr;
    m_pcm.clear();
}
