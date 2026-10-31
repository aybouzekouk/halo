#pragma once

#include <QObject>
#include <QString>

class QSocketNotifier;

// Grabs Ctrl+Space at the X server level so the overlay opens from any app.
// Falls back gracefully (status() explains why) when X11/xcb is unavailable.
class GlobalHotkey : public QObject
{
    Q_OBJECT
public:
    explicit GlobalHotkey(QObject *parent = nullptr);
    ~GlobalHotkey() override;

    bool start(quint32 keysym = 0x0020 /* space */, quint32 modifiers = 1u << 2 /* control */);
    void stop();

    bool isActive() const { return m_active; }
    QString status() const { return m_status; }

signals:
    void activated();

private:
    void poll();

    void *m_connection = nullptr; // xcb_connection_t*
    QSocketNotifier *m_notifier = nullptr;
    quint32 m_keycode = 0;
    quint32 m_modifiers = 0;
    bool m_active = false;
    QString m_status;
};
