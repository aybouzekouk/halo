#include "GlobalHotkey.h"

#include <QSocketNotifier>
#include <QGuiApplication>
#include <QDebug>

#ifdef HALO_HAVE_XCB
#include <xcb/xcb.h>
#include <memory>
#endif

GlobalHotkey::GlobalHotkey(QObject *parent)
    : QObject(parent)
{
}

GlobalHotkey::~GlobalHotkey()
{
    stop();
}

#ifdef HALO_HAVE_XCB
namespace {
struct ConnectionDeleter
{
    void operator()(xcb_connection_t *c) const
    {
        if (c)
            xcb_disconnect(c);
    }
};

quint32 keycodeForKeysym(xcb_connection_t *c, quint32 keysym)
{
    const xcb_setup_t *setup = xcb_get_setup(c);
    const xcb_get_keyboard_mapping_cookie_t cookie =
        xcb_get_keyboard_mapping(c, setup->min_keycode,
                                 quint8(setup->max_keycode - setup->min_keycode + 1));
    xcb_get_keyboard_mapping_reply_t *reply =
        xcb_get_keyboard_mapping_reply(c, cookie, nullptr);
    if (!reply)
        return 0;

    quint32 found = 0;
    const quint32 perKey = quint32(reply->keysyms_per_keycode);
    const xcb_keysym_t *syms = xcb_get_keyboard_mapping_keysyms(reply);
    const int count = xcb_get_keyboard_mapping_keysyms_length(reply);
    for (int i = 0; i < count; ++i) {
        if (syms[i] != keysym)
            continue;
        const int keycode = setup->min_keycode + (i / int(perKey));
        // Prefer a keycode whose first level is this keysym (the unshifted key).
        if (i % int(perKey) == 0) {
            found = quint32(keycode);
            break;
        }
        if (!found)
            found = quint32(keycode);
    }
    free(reply);
    return found;
}

constexpr quint32 kControlMask = 1u << 2;
constexpr quint32 kLockMask = 1u << 1;
constexpr quint32 kMod2Mask = 1u << 4; // NumLock on most layouts

quint32 kGrabs[4] = { 0, 0, 0, 0 };
} // namespace
#endif

bool GlobalHotkey::start(quint32 keysym, quint32 modifiers)
{
#ifndef HALO_HAVE_XCB
    Q_UNUSED(keysym);
    Q_UNUSED(modifiers);
    m_status = QStringLiteral("Global hotkey needs libxcb (built without it).");
    return false;
#else
    if (m_active)
        return true;

    if (!QGuiApplication::platformName().contains(QStringLiteral("xcb"))) {
        m_status = QStringLiteral("Global hotkey needs an X11 session (running on %1).")
                       .arg(QGuiApplication::platformName());
        return false;
    }

    int screenNumber = 0;
    std::unique_ptr<xcb_connection_t, ConnectionDeleter> connection(xcb_connect(nullptr, &screenNumber));
    if (!connection || xcb_connection_has_error(connection.get())) {
        m_status = QStringLiteral("Could not open a second X11 connection.");
        return false;
    }

    const quint32 keycode = keycodeForKeysym(connection.get(), keysym);
    if (keycode == 0) {
        m_status = QStringLiteral("Could not resolve the Space keycode.");
        return false;
    }

    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(connection.get()));
    for (int i = 0; i < screenNumber && it.rem; ++i)
        xcb_screen_next(&it);
    if (!it.rem) {
        m_status = QStringLiteral("No X11 screen found.");
        return false;
    }
    const xcb_window_t root = it.data->root;

    // NumLock and CapsLock change the modifier mask, so grab every combination.
    kGrabs[0] = modifiers;
    kGrabs[1] = modifiers | kLockMask;
    kGrabs[2] = modifiers | kMod2Mask;
    kGrabs[3] = modifiers | kLockMask | kMod2Mask;

    for (quint32 mods : kGrabs) {
        const xcb_void_cookie_t cookie =
            xcb_grab_key_checked(connection.get(), 1, root, quint16(mods), quint8(keycode),
                                 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        xcb_generic_error_t *error = xcb_request_check(connection.get(), cookie);
        if (error) {
            m_status = QStringLiteral("The shortcut is already grabbed (X error %1).")
                           .arg(error->error_code);
            free(error);
            for (quint32 m : kGrabs)
                xcb_ungrab_key(connection.get(), quint8(keycode), root, quint16(m));
            xcb_flush(connection.get());
            return false;
        }
    }
    xcb_flush(connection.get());

    m_connection = connection.release();
    m_keycode = keycode;
    m_modifiers = modifiers;
    m_active = true;
    m_status = QStringLiteral("Ctrl+Space is registered globally.");

    auto *c = static_cast<xcb_connection_t *>(m_connection);
    m_notifier = new QSocketNotifier(xcb_get_file_descriptor(c), QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &GlobalHotkey::poll);
    return true;
#endif
}

void GlobalHotkey::poll()
{
#ifdef HALO_HAVE_XCB
    auto *c = static_cast<xcb_connection_t *>(m_connection);
    if (!c)
        return;
    while (xcb_generic_event_t *event = xcb_poll_for_event(c)) {
        const quint8 type = event->response_type & ~0x80;
        if (type == XCB_KEY_PRESS) {
            auto *press = reinterpret_cast<xcb_key_press_event_t *>(event);
            const bool ctrl = (press->state & kControlMask) != 0;
            if (press->detail == m_keycode && ctrl)
                emit activated();
        }
        free(event);
    }
#endif
}

void GlobalHotkey::stop()
{
#ifdef HALO_HAVE_XCB
    if (m_connection) {
        auto *c = static_cast<xcb_connection_t *>(m_connection);
        xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(c));
        if (it.rem) {
            for (quint32 mods : kGrabs)
                xcb_ungrab_key(c, quint8(m_keycode), it.data->root, quint16(mods));
            xcb_flush(c);
        }
        xcb_disconnect(c);
        m_connection = nullptr;
    }
#else
    Q_UNUSED(m_keycode);
#endif
    if (m_notifier) {
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    m_active = false;
}
