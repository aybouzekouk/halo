#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QScreen>
#include <QCursor>
#include <QTimer>
#include <QDir>
#include <QDateTime>
#include <QCommandLineParser>
#include <QPainter>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QSettings>
#include <QDebug>

#include "AppController.h"

#include <QQuickImageProvider>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QAudioFormat>

namespace {

// Soft light painted once into an image: a radial gradient in a Shape item is
// unreliable across backends, an image is not.
class BloomProvider : public QQuickImageProvider
{
public:
    BloomProvider()
        : QQuickImageProvider(QQuickImageProvider::Image)
    {
    }

    QImage requestImage(const QString &id, QSize *size, const QSize &requested) override
    {
        const int w = requested.width() > 0 ? requested.width() : 960;
        const int h = requested.height() > 0 ? requested.height() : 640;
        QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);

        const QPointF c(w / 2.0, h / 2.0);
        if (id == QLatin1String("shadow")) {
            QRadialGradient shadow(c, w * 0.40);
            shadow.setColorAt(0.00, QColor(0, 0, 0, 190));
            shadow.setColorAt(0.45, QColor(0, 0, 0, 110));
            shadow.setColorAt(1.00, QColor(0, 0, 0, 0));
            painter.setBrush(shadow);
            painter.drawEllipse(c, w * 0.40, h * 0.42);
        } else {
            QRadialGradient teal(c, w * 0.47);
            teal.setColorAt(0.00, QColor(45, 212, 191, 110));
            teal.setColorAt(0.30, QColor(56, 189, 172, 66));
            teal.setColorAt(0.60, QColor(96, 132, 208, 28));
            teal.setColorAt(0.82, QColor(139, 124, 240, 12));
            teal.setColorAt(1.00, QColor(167, 139, 250, 0));
            painter.setBrush(teal);
            painter.drawEllipse(c, w * 0.47, h * 0.62);

            QRadialGradient halo(c, w * 0.30);
            halo.setColorAt(0.00, QColor(167, 139, 250, 74));
            halo.setColorAt(0.55, QColor(139, 124, 240, 24));
            halo.setColorAt(1.00, QColor(139, 124, 240, 0));
            painter.setBrush(halo);
            painter.drawEllipse(c, w * 0.30, h * 0.40);
        }

        if (size)
            *size = image.size();
        return image;
    }
};

constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 640;

void centreOnScreen(QQuickWindow *window)
{
    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen || !window)
        return;
    const QRect available = screen->availableGeometry();
    window->setPosition(available.center().x() - window->width() / 2,
                        available.center().y() - window->height() / 2);
}

} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Halo"));
    QCoreApplication::setApplicationName(QStringLiteral("Halo"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Halo"));

    // The overlay is a transparent, always-on-top surface: ask for an alpha
    // buffer before any window is created.
    QQuickWindow::setDefaultAlphaBuffer(true);

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Halo — a quiet voice assistant overlay."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption demoOption(QStringLiteral("demo"),
        QStringLiteral("Run the scripted demo instead of calling the API."));
    const QCommandLineOption showOption(QStringLiteral("show"),
        QStringLiteral("Open the overlay immediately."));
    const QCommandLineOption captureOption(QStringLiteral("capture"),
        QStringLiteral("Write a PNG for every state change into <dir>."),
        QStringLiteral("dir"));
    const QCommandLineOption hotkeyOption(QStringLiteral("no-hotkey"),
        QStringLiteral("Do not grab Ctrl+Space globally."));
    const QCommandLineOption probeOption(QStringLiteral("audio-devices"),
        QStringLiteral("List audio devices and exit."));
    const QCommandLineOption feedOption(QStringLiteral("feed"),
        QStringLiteral("Stream this 16 kHz mono WAV instead of the microphone."),
        QStringLiteral("wav"));
    const QCommandLineOption askOption(QStringLiteral("ask"),
        QStringLiteral("Ask this question as text (smoke test for the answer + voice path)."),
        QStringLiteral("text"));
    parser.addOption(demoOption);
    parser.addOption(showOption);
    parser.addOption(captureOption);
    parser.addOption(hotkeyOption);
    parser.addOption(askOption);
    parser.addOption(feedOption);
    parser.addOption(probeOption);
    parser.process(app);

    if (parser.isSet(probeOption)) {
        auto describe = [](const QAudioDevice &device) {
            QAudioFormat preferred = device.preferredFormat();
            qInfo().noquote() << " \"" << device.description() << "\"";
            qInfo().noquote() << "    default:" << device.isDefault()
                              << " preferred:" << preferred.sampleRate() << "Hz"
                              << preferred.channelCount() << "ch"
                              << int(preferred.sampleFormat());
            for (int rate : { 16000, 22050, 24000, 44100, 48000 }) {
                for (int ch : { 1, 2 }) {
                    QAudioFormat f;
                    f.setSampleRate(rate);
                    f.setChannelCount(ch);
                    f.setSampleFormat(QAudioFormat::Int16);
                    if (device.isFormatSupported(f))
                        qInfo().noquote() << "    ok:" << rate << "Hz" << ch << "ch int16";
                }
            }
        };
        qInfo().noquote() << "== inputs ==";
        for (const QAudioDevice &d : QMediaDevices::audioInputs())
            describe(d);
        qInfo().noquote() << "== outputs ==";
        for (const QAudioDevice &d : QMediaDevices::audioOutputs())
            describe(d);
        return 0;
    }

    if (parser.isSet(demoOption))
        qputenv("HALO_FORCE_DEMO", "1");
    if (parser.isSet(hotkeyOption))
        qputenv("HALO_NO_HOTKEY", "1");

    const QString captureDir = parser.value(captureOption);
    if (!captureDir.isEmpty())
        QDir().mkpath(captureDir);

    AppController controller;

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("bloom"), new BloomProvider);
    engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
    engine.loadFromModule("Halo", "Main");
    if (engine.rootObjects().isEmpty()) {
        qCritical() << "Halo: the QML interface failed to load.";
        return 1;
    }

    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (!window) {
        qCritical() << "Halo: the root object is not a window.";
        return 1;
    }

    window->setFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool
                     | Qt::NoDropShadowWindowHint);
    window->setColor(Qt::transparent);
    window->resize(kWindowWidth, kWindowHeight);
    centreOnScreen(window);

    QObject::connect(&controller, &AppController::requestShow, window, [window]() {
        centreOnScreen(window);
        window->show();
        window->raise();
        window->requestActivate();
    });
    QObject::connect(&controller, &AppController::requestHide, window, [window]() {
        QTimer::singleShot(260, window, [window]() { window->hide(); });
    });

    if (!captureDir.isEmpty()) {
        auto *counter = new int(0);
        QObject::connect(&controller, &AppController::stateChanged, window,
                         [&controller, window, captureDir, counter]() {
            const QString state = controller.state();
            const int n = ++(*counter);
            auto shoot = [window, captureDir, state, n]() {
                if (!window->isVisible())
                    return;
                const QImage windowImage = window->grabWindow();
                if (!windowImage.isNull()) {
                    windowImage.save(QStringLiteral("%1/%2-w%3.png")
                                         .arg(captureDir)
                                         .arg(state)
                                         .arg(n, 2, 10, QLatin1Char('0')));
                }
                if (QScreen *screen = window->screen()) {
                    const QPixmap desktop = screen->grabWindow(0);
                    if (!desktop.isNull()) {
                        desktop.save(QStringLiteral("%1/%2-screen%3.png")
                                         .arg(captureDir)
                                         .arg(state)
                                         .arg(n, 2, 10, QLatin1Char('0')));
                    }
                }

                // A review preview: the transparent window composed over a
                // neutral desktop so the glow and glass can be judged.
                const QImage framed = [&windowImage]() {
                    QImage canvas(1280, 800, QImage::Format_ARGB32_Premultiplied);
                    canvas.fill(Qt::transparent);
                    QPainter p(&canvas);
                    p.setRenderHint(QPainter::Antialiasing, true);
                    QLinearGradient bg(0, 0, 0, 800);
                    bg.setColorAt(0.0, QColor(0x16, 0x1A, 0x21));
                    bg.setColorAt(0.55, QColor(0x0D, 0x10, 0x15));
                    bg.setColorAt(1.0, QColor(0x07, 0x08, 0x0B));
                    p.fillRect(canvas.rect(), bg);
                    QRadialGradient glow(canvas.width() / 2.0, canvas.height() / 2.0, 520);
                    glow.setColorAt(0.0, QColor(46, 120, 105, 46));
                    glow.setColorAt(0.6, QColor(60, 52, 120, 22));
                    glow.setColorAt(1.0, QColor(0, 0, 0, 0));
                    p.fillRect(canvas.rect(), glow);
                    const int x = (canvas.width() - windowImage.width()) / 2;
                    const int y = (canvas.height() - windowImage.height()) / 2;
                    p.drawImage(x, y, windowImage);
                    return canvas;
                }();
                framed.save(QStringLiteral("%1/%2-view%3.png")
                                .arg(captureDir)
                                .arg(state)
                                .arg(n, 2, 10, QLatin1Char('0')));
            };
            // Give each state time to say something worth photographing.
            int delay = 700;
            if (state == QLatin1String("listening")) delay = 2200;
            else if (state == QLatin1String("thinking")) delay = 420;
            else if (state == QLatin1String("answering")) delay = 1500;
            else if (state == QLatin1String("speaking")) delay = 900;
            QTimer::singleShot(delay, window, shoot);
        });
    }

    if (parser.isSet(showOption))
        QTimer::singleShot(120, &controller, &AppController::activate);

    if (parser.isSet(feedOption)) {
        const QString wav = parser.value(feedOption);
        QTimer::singleShot(150, &controller, [&controller, wav]() {
            controller.feedWav(wav);
            controller.activate();
        });
    }

    if (parser.isSet(askOption)) {
        const QString question = parser.value(askOption);
        QTimer::singleShot(120, &controller, &AppController::activate);
        QTimer::singleShot(600, &controller, [&controller, question]() {
            controller.submitText(question);
        });
    }

    return app.exec();
}
