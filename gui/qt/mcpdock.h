#ifndef MCPDOCK_H
#define MCPDOCK_H

#include <QtWidgets/QWidget>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QPlainTextEdit>

// Dock UI for McpServer: lets a human see (and toggle) the automation
// channel that an MCP client (Claude Code, ...) uses to drive this same
// running CEmu window - the socket path, whether it's listening, how many
// clients are attached, and a log of every command received.
class McpDockWidget : public QWidget {
    Q_OBJECT

public:
    explicit McpDockWidget(QWidget *parent = Q_NULLPTR);
    ~McpDockWidget() override = default;

public slots:
    void setListening(bool listening, const QString &socketName);
    void setClientCount(int count);
    void addLogLine(const QString &line);

signals:
    void enabledToggled(bool enabled);

private:
    void updateStatusLabel();

    QCheckBox *m_chkEnabled;
    QLabel *m_lblStatus;
    QLineEdit *m_editSocket;
    QPushButton *m_btnCopySocket;
    QPlainTextEdit *m_log;
    QPushButton *m_btnClearLog;

    bool m_listening = false;
    int m_clientCount = 0;
};

#endif
