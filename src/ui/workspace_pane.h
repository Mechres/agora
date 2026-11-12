#pragma once

#include "core/sandbox.h"
#include "core/tools.h"

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QSyntaxHighlighter>
#include <QTabWidget>
#include <QTimer>
#include <QWidget>

namespace agora {

class CodeDiffHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit CodeDiffHighlighter(QTextDocument* parent, bool diffMode = false);

protected:
    void highlightBlock(const QString& text) override;

private:
    bool m_diffMode = false;
};

class WorkspacePane : public QWidget {
    Q_OBJECT
public:
    explicit WorkspacePane(BwrapSandbox* sandbox, ToolEngine* toolEngine, QWidget* parent = nullptr);

    void setWorkspacePath(const QString& path);
    void setAllowNetwork(bool allow);
    void clearSelectedFile();
    // Debounced: bursts of file events collapse into one refresh
    void refreshViews();

signals:
    void browseWorkspaceRequested();
    void allowNetworkToggled(bool allow);
    void statusMessage(const QString& msg);

private slots:
    void onFileSelected(QListWidgetItem* item);
    void onSaveEditorFileClicked();
    void onRunManualSandboxCmd();

private:
    void doRefresh();

    BwrapSandbox* m_sandbox = nullptr;
    QTimer* m_refreshTimer = nullptr;
    quint64 m_diffRequestSeq = 0;
    ToolEngine* m_toolEngine = nullptr;
    QString m_currentSelectedFileRelPath;

    QLineEdit* m_editWorkspacePath = nullptr;
    QCheckBox* m_chkAllowNetwork = nullptr;
    QTabWidget* m_tabs = nullptr;
    QListWidget* m_listFiles = nullptr;
    QLabel* m_lblEditorFile = nullptr;
    QPlainTextEdit* m_codeEditor = nullptr;
    QPlainTextEdit* m_diffViewer = nullptr;
    QPlainTextEdit* m_terminalOutput = nullptr;
    QLineEdit* m_editManualCmd = nullptr;
};

} // namespace agora
