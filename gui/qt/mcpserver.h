#ifndef MCPSERVER_H
#define MCPSERVER_H

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QHash>
#include <QtCore/QQueue>
#include <QtCore/QJsonObject>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>

class MainWindow;

// Local (Unix domain socket / named pipe) server that exposes a subset of
// MainWindow's automation-friendly operations (screenshot, key press,
// program transfer/launch, reset, memory peek...) to an external controller
// (the CEmu MCP Python server) over a small newline-delimited JSON protocol.
//
// Request:  {"id": <number>, "cmd": "<name>", "args": { ... }}\n
// Response: {"id": <number>, "ok": true, ...}\n
//        or {"id": <number>, "ok": false, "error": "<message>"}\n
//
// Requests are processed strictly one at a time, in arrival order, across
// all connected clients: several MainWindow helpers (guiDelay-based waits,
// sendFiles' guiSend spin) re-enter the Qt event loop while they block,
// which would otherwise let a second request be dispatched into the middle
// of the first one's handling.
class McpServer : public QObject {
    Q_OBJECT

public:
    explicit McpServer(MainWindow *mainWindow, QObject *parent = Q_NULLPTR);
    ~McpServer() override;

    bool start(const QString &id);
    void stop();
    bool isListening() const;
    QString socketName() const;
    int clientCount() const;

signals:
    void logMessage(const QString &line);
    void listeningChanged(bool listening);
    void clientCountChanged(int count);

private slots:
    void handleNewConnection();
    void handleReadyRead();
    void handleDisconnected();

private:
    struct PendingRequest {
        QLocalSocket *socket;
        QJsonObject request;
    };

    void queueRequest(QLocalSocket *socket, const QJsonObject &request);
    void processQueue();
    QJsonObject dispatch(const QJsonObject &request, bool &ok, QString &error);
    void sendResponse(QLocalSocket *socket, const QJsonObject &response);

    MainWindow *m_mainWindow;
    QLocalServer *m_server;
    QString m_socketName;
    QList<QLocalSocket *> m_clients;
    QHash<QLocalSocket *, QByteArray> m_buffers;
    QQueue<PendingRequest> m_pending;
    bool m_busy;
};

#endif
