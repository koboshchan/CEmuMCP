#include "mcpserver.h"
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "lcdwidget.h"
#include "sendinghandler.h"
#include "utils.h"

// Core headers must be included before autotester.h: autotester.h wraps its
// own copy of these in `namespace cemucore { extern "C" { ... } }`, and once
// the include guards have fired the plain (non-namespaced) declarations used
// below (emu_keypad_event, mem_peek_byte, ...) would otherwise not be visible
// - same ordering MainWindow's own .cpp files rely on.
#include "../../core/emu.h"
#include "../../core/extras.h"
#include "../../core/keypad.h"
#include "../../core/link.h"
#include "../../core/mem.h"
#include "../../tests/autotester/autotester.h"

#include <QtCore/QBuffer>
#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtGui/QImage>

namespace {

// Physical key names accepted by "key"/"keys", kept in sync with
// autotester::valid_keys (tests/autotester/autotester.cpp) and documented in
// tests/autotester/json_file_documentation.txt. That table is file-local to
// autotester.cpp, so it can't be reused directly here; autotester::keyCoordForName()
// still does the actual (authoritative) validation for every keypress below,
// this list only exists to answer "list_keys" for discoverability.
const char *const kKeyNames[] = {
    "graph", "trace", "zoom", "window", "wind", "y=", "yequ", "2nd", "mode", "del",
    "on", "sto", "ln", "log", "^2", "sq", "-1", "inv", "math", "alpha",
    "0", "1", "4", "7", "comma", "sin", "apps", "xton", "xtn",
    "dot", "2", "5", "8", "lpar", "cos", "prgm", "stat",
    "(-)", "neg", "3", "6", "9", "rpar", "tan", "vars",
    "enter", "+", "add", "-", "sub", "*", "mul", "/", "div", "^", "pow", "clear", "clr",
    "down", "left", "right", "up",
};

QJsonObject errorObj(const QString &message) {
    QJsonObject o;
    o["ok"] = false;
    o["error"] = message;
    return o;
}

int locationFromString(const QString &s) {
    if (s.compare(QStringLiteral("ram"), Qt::CaseInsensitive) == 0) {
        return LINK_RAM;
    }
    if (s.compare(QStringLiteral("arch"), Qt::CaseInsensitive) == 0 ||
        s.compare(QStringLiteral("archive"), Qt::CaseInsensitive) == 0) {
        return LINK_ARCH;
    }
    return LINK_FILE;
}

bool parseAddress(const QString &raw, uint32_t &out) {
    const auto tmp = autotester::hash_consts.find(raw.toStdString());
    if (tmp != autotester::hash_consts.end()) {
        out = tmp->second;
        return true;
    }
    bool ok = false;
    if (raw.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        out = raw.mid(2).toUInt(&ok, 16);
    } else {
        out = raw.toUInt(&ok, 10);
    }
    return ok;
}

}

McpServer::McpServer(MainWindow *mainWindow, QObject *parent)
    : QObject(parent), m_mainWindow(mainWindow), m_server(new QLocalServer(this)), m_busy(false) {
    connect(m_server, &QLocalServer::newConnection, this, &McpServer::handleNewConnection);
}

McpServer::~McpServer() {
    stop();
}

bool McpServer::start(const QString &id) {
    stop();

    m_socketName = QStringLiteral("cemu-mcp-") + (id.isEmpty() ? QStringLiteral("default") : id);
    QLocalServer::removeServer(m_socketName);

    if (!m_server->listen(m_socketName)) {
        emit logMessage(tr("[MCP] Failed to listen on '%1': %2").arg(m_socketName, m_server->errorString()));
        return false;
    }

    emit logMessage(tr("[MCP] Listening on '%1'").arg(m_server->fullServerName()));
    emit listeningChanged(true);
    return true;
}

void McpServer::stop() {
    const bool wasListening = m_server->isListening();
    if (wasListening) {
        m_server->close();
    }

    for (QLocalSocket *socket : m_clients) {
        socket->disconnect(this);
        socket->close();
        socket->deleteLater();
    }
    m_clients.clear();
    m_buffers.clear();
    m_pending.clear();
    m_busy = false;

    if (wasListening) {
        emit listeningChanged(false);
        emit clientCountChanged(0);
        emit logMessage(tr("[MCP] Stopped."));
    }
}

bool McpServer::isListening() const {
    return m_server->isListening();
}

QString McpServer::socketName() const {
    return m_server->isListening() ? m_server->fullServerName() : m_socketName;
}

int McpServer::clientCount() const {
    return m_clients.size();
}

void McpServer::handleNewConnection() {
    while (QLocalSocket *socket = m_server->nextPendingConnection()) {
        m_clients.append(socket);
        connect(socket, &QLocalSocket::readyRead, this, &McpServer::handleReadyRead);
        connect(socket, &QLocalSocket::disconnected, this, &McpServer::handleDisconnected);
        emit clientCountChanged(m_clients.size());
        emit logMessage(tr("[MCP] Client connected (%1 total)").arg(m_clients.size()));
    }
}

void McpServer::handleReadyRead() {
    QLocalSocket *socket = qobject_cast<QLocalSocket *>(sender());
    if (!socket) {
        return;
    }

    QByteArray &buffer = m_buffers[socket];
    buffer += socket->readAll();

    int newlineIndex;
    while ((newlineIndex = buffer.indexOf('\n')) != -1) {
        QByteArray line = buffer.left(newlineIndex).trimmed();
        buffer.remove(0, newlineIndex + 1);
        if (line.isEmpty()) {
            continue;
        }

        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            sendResponse(socket, errorObj(tr("Invalid JSON request: %1").arg(parseError.errorString())));
            continue;
        }

        queueRequest(socket, doc.object());
    }

    processQueue();
}

void McpServer::handleDisconnected() {
    QLocalSocket *socket = qobject_cast<QLocalSocket *>(sender());
    if (!socket) {
        return;
    }

    m_clients.removeAll(socket);
    m_buffers.remove(socket);
    emit clientCountChanged(m_clients.size());
    emit logMessage(tr("[MCP] Client disconnected (%1 total)").arg(m_clients.size()));
    socket->deleteLater();
}

void McpServer::queueRequest(QLocalSocket *socket, const QJsonObject &request) {
    m_pending.enqueue({socket, request});
}

// Drains m_pending one request at a time. dispatch() below can re-enter the
// Qt event loop (guiDelay spins processEvents()), which can deliver more
// socket data and call this function again reentrantly; the m_busy guard
// makes the nested call a no-op and the newly queued request(s) simply get
// picked up by this (the outer, still-running) loop once the current
// request finishes.
void McpServer::processQueue() {
    if (m_busy) {
        return;
    }
    m_busy = true;

    while (!m_pending.isEmpty()) {
        const PendingRequest pr = m_pending.dequeue();
        if (!m_clients.contains(pr.socket)) {
            continue; // client disconnected before we got to it
        }

        bool ok = true;
        QString error;
        const QJsonObject result = dispatch(pr.request, ok, error);

        QJsonObject response;
        if (pr.request.contains(QStringLiteral("id"))) {
            response[QStringLiteral("id")] = pr.request[QStringLiteral("id")];
        }
        response[QStringLiteral("ok")] = ok;
        if (ok) {
            for (auto it = result.constBegin(); it != result.constEnd(); ++it) {
                response[it.key()] = it.value();
            }
        } else {
            response[QStringLiteral("error")] = error;
        }
        sendResponse(pr.socket, response);
    }

    m_busy = false;
}

void McpServer::sendResponse(QLocalSocket *socket, const QJsonObject &response) {
    if (!socket || socket->state() != QLocalSocket::ConnectedState) {
        return;
    }
    QByteArray payload = QJsonDocument(response).toJson(QJsonDocument::Compact);
    payload += '\n';
    socket->write(payload);
    socket->flush();
}

QJsonObject McpServer::dispatch(const QJsonObject &request, bool &ok, QString &error) {
    const QString cmd = request.value(QStringLiteral("cmd")).toString();
    const QJsonObject args = request.value(QStringLiteral("args")).toObject();

    emit logMessage(tr("[MCP] -> %1").arg(cmd));

    if (cmd == QStringLiteral("status")) {
        QJsonObject result;
        result[QStringLiteral("listening")] = isListening();
        result[QStringLiteral("clients")] = clientCount();
        return result;
    }

    if (cmd == QStringLiteral("list_keys")) {
        QJsonArray names;
        for (const char *name : kKeyNames) {
            names.append(QString::fromLatin1(name));
        }
        QJsonObject result;
        result[QStringLiteral("keys")] = names;
        return result;
    }

    if (cmd == QStringLiteral("screenshot")) {
        const QImage image = m_mainWindow->ui->lcd->getImage();
        QByteArray pngBytes;
        QBuffer buffer(&pngBytes);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG")) {
            ok = false;
            error = QStringLiteral("Failed to encode screenshot as PNG");
            return {};
        }
        QJsonObject result;
        result[QStringLiteral("width")] = image.width();
        result[QStringLiteral("height")] = image.height();
        result[QStringLiteral("png_base64")] = QString::fromLatin1(pngBytes.toBase64());
        return result;
    }

    if (cmd == QStringLiteral("key")) {
        const QString name = args.value(QStringLiteral("name")).toString();
        const QString action = args.value(QStringLiteral("action")).toString(QStringLiteral("tap"));
        const int durationMs = args.value(QStringLiteral("duration_ms")).toInt(80);

        autotester::key_coord_t coord{};
        if (!autotester::keyCoordForName(name.toStdString(), coord)) {
            ok = false;
            error = QStringLiteral("Unknown key name: '%1'").arg(name);
            return {};
        }

        if (action == QStringLiteral("down")) {
            emu_keypad_event(coord.y, coord.x, true);
        } else if (action == QStringLiteral("up")) {
            emu_keypad_event(coord.y, coord.x, false);
        } else {
            emu_keypad_event(coord.y, coord.x, true);
            guiDelay(qMax(1, durationMs));
            emu_keypad_event(coord.y, coord.x, false);
            guiDelay(50);
        }
        return {};
    }

    if (cmd == QStringLiteral("keys")) {
        const QString sequence = args.value(QStringLiteral("sequence")).toString();

        QStringList errors;
        autotester::key_sequence_handlers_t handlers;
        handlers.keyEvent = [](uint8_t row, uint8_t col, bool pressed) {
            emu_keypad_event(row, col, pressed);
        };
        handlers.delay = [](unsigned int ms) {
            guiDelay(static_cast<int>(ms));
        };
        handlers.error = [&errors](const std::string &err) {
            errors.append(QString::fromStdString(err));
        };

        const bool sequenceOk = autotester::runKeySequence(sequence.toStdString(), handlers);
        if (!sequenceOk || !errors.isEmpty()) {
            ok = false;
            error = errors.isEmpty() ? QStringLiteral("Invalid key sequence") : errors.join(QStringLiteral("; "));
            return {};
        }
        return {};
    }

    if (cmd == QStringLiteral("text")) {
        const QString text = args.value(QStringLiteral("text")).toString();
        QStringList skipped;
        for (const QChar &c : text) {
            const char latin1 = c.toLatin1();
            const bool supported = (latin1 >= '0' && latin1 <= '9') ||
                                    (latin1 >= 'A' && latin1 <= 'Z') ||
                                    (latin1 >= 'a' && latin1 <= 'z') ||
                                    latin1 == '@';
            if (!supported) {
                skipped.append(c);
                continue;
            }
            m_mainWindow->sendEmuLetterKey(latin1 >= 'a' && latin1 <= 'z' ? latin1 - 'a' + 'A' : latin1);
        }
        QJsonObject result;
        if (!skipped.isEmpty()) {
            result[QStringLiteral("skipped_chars")] = skipped.join(QString());
        }
        return result;
    }

    if (cmd == QStringLiteral("os_key")) {
        const int code = args.value(QStringLiteral("code")).toInt(-1);
        if (code < 0 || code > 0xFFFF) {
            ok = false;
            error = QStringLiteral("os_key 'code' must be a 16-bit unsigned integer");
            return {};
        }
        m_mainWindow->sendEmuKey(static_cast<uint16_t>(code));
        return {};
    }

    if (cmd == QStringLiteral("send")) {
        QStringList files;
        for (const QJsonValue &v : args.value(QStringLiteral("files")).toArray()) {
            files.append(v.toString());
        }
        if (files.isEmpty()) {
            ok = false;
            error = QStringLiteral("send requires a non-empty 'files' array");
            return {};
        }
        for (const QString &f : files) {
            if (!fileExists(f)) {
                ok = false;
                error = QStringLiteral("File does not exist: %1").arg(f);
                return {};
            }
        }

        const int location = locationFromString(args.value(QStringLiteral("location")).toString(QStringLiteral("file")));
        sendingHandler->sendFiles(files, location);

        // sendFiles() is asynchronous (it hands off to the emu thread via a
        // signal), so wait for it to finish. Unlike MainWindow's own
        // guiSend spin-waits elsewhere (e.g. varClipboardListToL1), which
        // busy-poll guiDelay() (processEvents() back-to-back, no sleep) and
        // so pin this thread at 100% CPU for the whole transfer, use
        // WaitForMoreEvents here: it blocks (no busy-spin) until an event -
        // e.g. the emu thread's linkProgress signal - actually arrives.
        // CEmu's own doSend() already disables throttling to run transfers
        // as fast as possible, so a busy GUI thread stealing CPU from the
        // emu thread is what visibly slows large (e.g. 90-variable bundle)
        // transfers down, not anything in the transfer path itself.
        // A large multi-variable transfer (e.g. a many-file .b84 bundle) can
        // trigger real TI-OS flash defragmentation mid-transfer, which alone
        // can take well over a minute; give this plenty of room.
        QElapsedTimer sendTimer;
        sendTimer.start();
        while (guiSend && sendTimer.elapsed() < 180000) {
            qApp->processEvents(QEventLoop::WaitForMoreEvents, 25);
        }
        if (guiSend) {
            ok = false;
            error = QStringLiteral("Timed out waiting for file transfer to finish");
            return {};
        }
        return {};
    }

    if (cmd == QStringLiteral("launch")) {
        const QString name = args.value(QStringLiteral("name")).toString().toUpper();
        const bool isAsm = args.value(QStringLiteral("asm")).toBool(false);
        if (name.isEmpty()) {
            ok = false;
            error = QStringLiteral("launch requires a program 'name'");
            return {};
        }

        // Each key below goes through MainWindow's enqueueKeys() queue, which
        // the emu thread drains one at a time (waiting for the OS to consume
        // each one before sending the next), so this is safe to fire in a
        // tight loop; the extra guiDelay()s here are for the *OS's* catalog
        // helper (PRGM inserts a "prgm" token and then auto-completes/matches
        // against typed letters), which needs a redraw between keystrokes to
        // track the right state - without it, keys can land while the OS is
        // still mid-update and get misinterpreted.
        guiDelay(50);
        m_mainWindow->sendEmuKey(CE_KEY_CLEAR);
        guiDelay(80);
        if (isAsm) {
            m_mainWindow->sendEmuKey(CE_KEY_ASM);
            guiDelay(80);
        }
        m_mainWindow->sendEmuKey(CE_KEY_PRGM);
        guiDelay(80);
        for (const QChar &c : name) {
            m_mainWindow->sendEmuLetterKey(c.toLatin1());
            guiDelay(80);
        }
        m_mainWindow->sendEmuKey(CE_KEY_ENTER);
        return {};
    }

    if (cmd == QStringLiteral("reset")) {
        m_mainWindow->resetEmu();
        // Give the OS boot sequence time to settle before any follow-up
        // command (matches the autotester DSL's own action|reset, which
        // waits 1000ms after triggering a reset for the same reason).
        guiDelay(1000);
        return {};
    }

    if (cmd == QStringLiteral("reload_rom")) {
        const QString path = args.value(QStringLiteral("path")).toString();
        if (!path.isEmpty()) {
            if (!fileExists(path)) {
                ok = false;
                error = QStringLiteral("ROM file does not exist: %1").arg(path);
                return {};
            }
            m_mainWindow->m_pathRom = QDir::cleanPath(path);
        } else if (m_mainWindow->m_pathRom.isEmpty() || !fileExists(m_mainWindow->m_pathRom)) {
            ok = false;
            error = QStringLiteral("No ROM currently loaded and no 'path' given");
            return {};
        }

        // Fresh boot from the ROM file, discarding all current RAM/emulation
        // state (same as MainWindow's "Reload ROM" menu action/button).
        m_mainWindow->emuLoad(EMU_DATA_ROM);
        // Let the OS boot sequence settle before any follow-up command,
        // matching the "reset" command above.
        guiDelay(1000);

        QJsonObject result;
        result[QStringLiteral("rom")] = m_mainWindow->m_pathRom;
        return result;
    }

    if (cmd == QStringLiteral("run_ms")) {
        const int ms = args.value(QStringLiteral("ms")).toInt(0);
        if (ms <= 0 || ms > 120000) {
            ok = false;
            error = QStringLiteral("run_ms 'ms' must be between 1 and 120000");
            return {};
        }
        guiDelay(ms);
        return {};
    }

    if (cmd == QStringLiteral("read_memory")) {
        uint32_t address = 0;
        if (!parseAddress(args.value(QStringLiteral("address")).toString(), address)) {
            ok = false;
            error = QStringLiteral("Invalid 'address'");
            return {};
        }
        const int size = args.value(QStringLiteral("size")).toInt(0);
        if (size <= 0 || size > 4096) {
            ok = false;
            error = QStringLiteral("'size' must be between 1 and 4096");
            return {};
        }

        QByteArray data;
        data.reserve(size);
        for (int i = 0; i < size; i++) {
            data.append(static_cast<char>(mem_peek_byte(address + static_cast<uint32_t>(i))));
        }
        QJsonObject result;
        result[QStringLiteral("data_hex")] = QString::fromLatin1(data.toHex());
        return result;
    }

    if (cmd == QStringLiteral("set_speed")) {
        const int percent = args.value(QStringLiteral("percent")).toInt(-1);
        if (percent < 0 || percent > 500) {
            ok = false;
            error = QStringLiteral("'percent' must be between 0 and 500");
            return {};
        }
        m_mainWindow->setEmuSpeed(percent);
        return {};
    }

    if (cmd == QStringLiteral("throttle")) {
        const bool enabled = args.value(QStringLiteral("enabled")).toBool(true);
        m_mainWindow->setThrottle(enabled ? Qt::Checked : Qt::Unchecked);
        return {};
    }

    ok = false;
    error = QStringLiteral("Unknown command: '%1'").arg(cmd);
    return {};
}
