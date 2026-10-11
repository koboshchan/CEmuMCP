#include "mcpdock.h"

#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QApplication>
#include <QtGui/QClipboard>
#include <QtGui/QFontDatabase>
#include <QtGui/QTextCursor>

McpDockWidget::McpDockWidget(QWidget *parent) : QWidget{parent} {
    m_chkEnabled = new QCheckBox(tr("Enable MCP server"));
    m_lblStatus = new QLabel(tr("Not listening"));
    m_editSocket = new QLineEdit();
    m_editSocket->setReadOnly(true);
    m_editSocket->setPlaceholderText(tr("(not started)"));
    m_btnCopySocket = new QPushButton(tr("Copy"));
    m_log = new QPlainTextEdit();
    m_log->setReadOnly(true);
    m_log->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QFont monospace = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monospace.setStyleHint(QFont::Monospace);
    m_log->setFont(monospace);
    m_btnClearLog = new QPushButton(tr("Clear Log"));

    QHBoxLayout *topLayout = new QHBoxLayout();
    topLayout->addWidget(m_chkEnabled);
    topLayout->addWidget(m_lblStatus);
    topLayout->addStretch();

    QHBoxLayout *socketLayout = new QHBoxLayout();
    socketLayout->addWidget(new QLabel(tr("Socket:")));
    socketLayout->addWidget(m_editSocket);
    socketLayout->addWidget(m_btnCopySocket);

    QHBoxLayout *bottomLayout = new QHBoxLayout();
    bottomLayout->addWidget(m_btnClearLog);
    bottomLayout->addStretch();

    QVBoxLayout *layout = new QVBoxLayout();
    layout->addLayout(topLayout);
    layout->addLayout(socketLayout);
    layout->addWidget(m_log);
    layout->addLayout(bottomLayout);
    setLayout(layout);

    connect(m_chkEnabled, &QCheckBox::toggled, this, &McpDockWidget::enabledToggled);
    connect(m_btnClearLog, &QPushButton::clicked, m_log, &QPlainTextEdit::clear);
    connect(m_btnCopySocket, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_editSocket->text());
    });
}

void McpDockWidget::setListening(bool listening, const QString &socketName) {
    m_listening = listening;
    m_editSocket->setText(socketName);
    m_chkEnabled->blockSignals(true);
    m_chkEnabled->setChecked(listening);
    m_chkEnabled->blockSignals(false);
    updateStatusLabel();
}

void McpDockWidget::setClientCount(int count) {
    m_clientCount = count;
    updateStatusLabel();
}

void McpDockWidget::updateStatusLabel() {
    if (!m_listening) {
        m_lblStatus->setText(tr("Not listening"));
    } else {
        m_lblStatus->setText(tr("Listening - %1 client(s) connected").arg(m_clientCount));
    }
}

void McpDockWidget::addLogLine(const QString &line) {
    m_log->moveCursor(QTextCursor::End);
    m_log->insertPlainText(line + QStringLiteral("\n"));
    m_log->moveCursor(QTextCursor::End);
}
