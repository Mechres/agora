#include "ui/workspace_pane.h"

#include <QColor>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QTextCharFormat>
#include <QVBoxLayout>

namespace agora {

CodeDiffHighlighter::CodeDiffHighlighter(QTextDocument* parent, bool diffMode)
    : QSyntaxHighlighter(parent), m_diffMode(diffMode) {}

void CodeDiffHighlighter::highlightBlock(const QString& text) {
    if (m_diffMode) {
        QTextCharFormat fmt;
        if (text.startsWith('+')) {
            fmt.setForeground(QColor("#4ade80"));
            setFormat(0, text.length(), fmt);
        } else if (text.startsWith('-')) {
            fmt.setForeground(QColor("#f87171"));
            setFormat(0, text.length(), fmt);
        } else if (text.startsWith("@@")) {
            fmt.setForeground(QColor("#38bdf8"));
            setFormat(0, text.length(), fmt);
        }
        return;
    }

    QTextCharFormat kwFmt;
    kwFmt.setForeground(QColor("#c084fc"));
    kwFmt.setFontWeight(QFont::Bold);
    static const QRegularExpression kwRe(
        R"(\b(class|struct|enum|void|int|double|float|bool|char|auto|const|constexpr|return|if|else|for|while|switch|case|break|continue|namespace|template|typename|public|private|protected|virtual|override|import|from|def|async|await|fn|let|mut|pub|impl|use|match)\b)");
    auto it = kwRe.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        setFormat(m.capturedStart(), m.capturedLength(), kwFmt);
    }

    QTextCharFormat strFmt;
    strFmt.setForeground(QColor("#86efac"));
    static const QRegularExpression strRe(R"(("([^"\\]|\\.)*"|'([^'\\]|\\.)*'))");
    auto sit = strRe.globalMatch(text);
    while (sit.hasNext()) {
        const auto m = sit.next();
        setFormat(m.capturedStart(), m.capturedLength(), strFmt);
    }

    QTextCharFormat cmtFmt;
    cmtFmt.setForeground(QColor("#64748b"));
    static const QRegularExpression cmtRe(R"((//.*$|#.*$))");
    auto cit = cmtRe.globalMatch(text);
    while (cit.hasNext()) {
        const auto m = cit.next();
        setFormat(m.capturedStart(), m.capturedLength(), cmtFmt);
    }
}

WorkspacePane::WorkspacePane(BwrapSandbox* sandbox, ToolEngine* toolEngine, QWidget* parent)
    : QWidget(parent), m_sandbox(sandbox), m_toolEngine(toolEngine) {
    auto* rightLayout = new QVBoxLayout(this);
    rightLayout->setContentsMargins(4, 4, 4, 4);

    auto* wsHeaderRow = new QHBoxLayout();
    auto* lblWorkspace = new QLabel("WORKSPACE", this);
    lblWorkspace->setProperty("role", "section");
    wsHeaderRow->addWidget(lblWorkspace);
    m_editWorkspacePath = new QLineEdit(m_sandbox->workspacePath(), this);
    m_editWorkspacePath->setReadOnly(true);
    auto* btnBrowseWs = new QPushButton("Change…", this);
    btnBrowseWs->setToolTip("Use another folder as the sandbox workspace");
    m_chkAllowNetwork = new QCheckBox("Network", this);
    m_chkAllowNetwork->setToolTip("Allow sandboxed commands to use the network (bwrap --share-net). "
                                  "Off = complete network isolation.");
    wsHeaderRow->addWidget(m_editWorkspacePath, 1);
    wsHeaderRow->addWidget(btnBrowseWs);
    wsHeaderRow->addWidget(m_chkAllowNetwork);

    m_tabs = new QTabWidget(this);

    // Tab 1: Files & Code Editor
    auto* tabFiles = new QWidget(m_tabs);
    auto* tabFilesLayout = new QVBoxLayout(tabFiles);
    auto* fileSplitter = new QSplitter(Qt::Vertical, tabFiles);

    m_listFiles = new QListWidget(fileSplitter);
    auto* editorContainer = new QWidget(fileSplitter);
    auto* editorLayout = new QVBoxLayout(editorContainer);
    editorLayout->setContentsMargins(0, 0, 0, 0);

    auto* editorHeader = new QHBoxLayout();
    m_lblEditorFile = new QLabel("Select a file from the workspace above", editorContainer);
    auto* btnSaveFile = new QPushButton("Save", editorContainer);
    editorHeader->addWidget(m_lblEditorFile, 1);
    editorHeader->addWidget(btnSaveFile);

    m_codeEditor = new QPlainTextEdit(editorContainer);
    m_codeEditor->setFont(QFont("Monospace", 10));
    new CodeDiffHighlighter(m_codeEditor->document(), false);

    editorLayout->addLayout(editorHeader);
    editorLayout->addWidget(m_codeEditor, 1);
    fileSplitter->setStretchFactor(0, 1);
    fileSplitter->setStretchFactor(1, 3);
    tabFilesLayout->addWidget(fileSplitter);

    // Tab 2: Live Git Diff
    auto* tabDiff = new QWidget(m_tabs);
    auto* tabDiffLayout = new QVBoxLayout(tabDiff);
    m_diffViewer = new QPlainTextEdit(tabDiff);
    m_diffViewer->setReadOnly(true);
    m_diffViewer->setFont(QFont("Monospace", 10));
    new CodeDiffHighlighter(m_diffViewer->document(), true);
    auto* btnRefreshDiff = new QPushButton("Refresh git diff", tabDiff);
    tabDiffLayout->addWidget(m_diffViewer, 1);
    tabDiffLayout->addWidget(btnRefreshDiff);

    // Tab 3: Sandbox Terminal (bwrap)
    auto* tabTerm = new QWidget(m_tabs);
    auto* tabTermLayout = new QVBoxLayout(tabTerm);
    m_terminalOutput = new QPlainTextEdit(tabTerm);
    m_terminalOutput->setReadOnly(true);
    m_terminalOutput->setFont(QFont("Monospace", 10));
    m_terminalOutput->setPlainText(
        BwrapSandbox::isBwrapAvailable()
            ? "Bubblewrap (/usr/bin/bwrap) sandbox active. Commands run isolated in /workspace.\n"
            : "WARNING: /usr/bin/bwrap not found!\n");

    auto* manualCmdRow = new QHBoxLayout();
    m_editManualCmd = new QLineEdit(tabTerm);
    m_editManualCmd->setPlaceholderText("Run command inside bwrap sandbox (e.g. python3 -m unittest, ls -la)...");
    auto* btnRunCmd = new QPushButton("Run in Sandbox", tabTerm);
    manualCmdRow->addWidget(m_editManualCmd, 1);
    manualCmdRow->addWidget(btnRunCmd);

    tabTermLayout->addWidget(m_terminalOutput, 1);
    tabTermLayout->addLayout(manualCmdRow);

    m_tabs->addTab(tabFiles, "Files && Code");
    m_tabs->addTab(tabDiff, "Git Diff");
    m_tabs->addTab(tabTerm, "Terminal");

    rightLayout->addLayout(wsHeaderRow);
    rightLayout->addWidget(m_tabs, 1);

    connect(btnBrowseWs, &QPushButton::clicked, this, &WorkspacePane::browseWorkspaceRequested);
    connect(m_chkAllowNetwork, &QCheckBox::toggled, this, &WorkspacePane::allowNetworkToggled);
    connect(m_listFiles, &QListWidget::itemClicked, this, &WorkspacePane::onFileSelected);
    connect(btnSaveFile, &QPushButton::clicked, this, &WorkspacePane::onSaveEditorFileClicked);
    connect(btnRefreshDiff, &QPushButton::clicked, this, &WorkspacePane::refreshViews);
    connect(btnRunCmd, &QPushButton::clicked, this, &WorkspacePane::onRunManualSandboxCmd);
    connect(m_editManualCmd, &QLineEdit::returnPressed, this, &WorkspacePane::onRunManualSandboxCmd);

    connect(m_sandbox, &BwrapSandbox::commandStarted, this, [this](const QString& cmd) {
        m_terminalOutput->appendPlainText(QStringLiteral("\n$ %1").arg(cmd));
    });
    connect(m_sandbox, &BwrapSandbox::commandOutputChunk, this, [this](const QString& chunk) {
        m_terminalOutput->moveCursor(QTextCursor::End);
        m_terminalOutput->insertPlainText(chunk);
        m_terminalOutput->moveCursor(QTextCursor::End);
    });
}

void WorkspacePane::setWorkspacePath(const QString& path) {
    m_editWorkspacePath->setText(path);
}

void WorkspacePane::setAllowNetwork(bool allow) {
    m_chkAllowNetwork->setChecked(allow);
}

void WorkspacePane::clearSelectedFile() {
    m_currentSelectedFileRelPath.clear();
    m_codeEditor->clear();
    m_lblEditorFile->setText("Select a file from the workspace above");
}

void WorkspacePane::refreshViews() {
    if (!m_refreshTimer) {
        m_refreshTimer = new QTimer(this);
        m_refreshTimer->setSingleShot(true);
        m_refreshTimer->setInterval(150);
        connect(m_refreshTimer, &QTimer::timeout, this, &WorkspacePane::doRefresh);
    }
    m_refreshTimer->start();
}

void WorkspacePane::doRefresh() {
    const QString ws = m_sandbox->workspacePath();
    m_listFiles->clear();
    if (ws.isEmpty() || !QDir(ws).exists()) return;

    QDir root(ws);
    QStringList files;
    QDirIterator it(ws, QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext() && files.size() < ToolEngine::kMaxListEntries) {
        const QString full = it.next();
        if (QFileInfo(full).isSymLink()) continue;
        const QString rel = root.relativeFilePath(full);
        if (rel == ".git" || rel.startsWith(".git/")) continue;
        files << rel;
    }
    files.sort();
    for (const QString& rel : files) {
        m_listFiles->addItem(rel);
    }

    if (!m_currentSelectedFileRelPath.isEmpty()) {
        QString err;
        if (auto safe = m_toolEngine->resolveSafeWorkspacePath(m_currentSelectedFileRelPath, &err)) {
            QFile f(*safe);
            if (f.open(QIODevice::ReadOnly)) {
                m_codeEditor->setPlainText(QString::fromUtf8(f.readAll()));
            }
        }
    }

    // Only the newest request may update the view; older ones can finish out of order
    const quint64 seq = ++m_diffRequestSeq;
    m_sandbox->gitStatusAndDiffAsync([this, seq](const QString&, const QString& diff) {
        if (seq == m_diffRequestSeq) m_diffViewer->setPlainText(diff);
    });
}

void WorkspacePane::onFileSelected(QListWidgetItem* item) {
    if (!item) return;
    m_currentSelectedFileRelPath = item->text();
    m_lblEditorFile->setText("Editing: " + m_currentSelectedFileRelPath);
    QString err;
    if (auto safe = m_toolEngine->resolveSafeWorkspacePath(m_currentSelectedFileRelPath, &err)) {
        QFile f(*safe);
        if (f.open(QIODevice::ReadOnly)) {
            m_codeEditor->setPlainText(QString::fromUtf8(f.readAll()));
        }
    }
}

void WorkspacePane::onSaveEditorFileClicked() {
    if (m_currentSelectedFileRelPath.isEmpty()) return;
    ToolCall tc;
    tc.id = "manual_save";
    tc.name = "write_file";
    QJsonObject args;
    args["path"] = m_currentSelectedFileRelPath;
    args["content"] = m_codeEditor->toPlainText();
    tc.argumentsJson = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    m_toolEngine->executeToolCallAsync(tc, [this](const ToolResult& res) {
        emit statusMessage(res.output);
    });
}

void WorkspacePane::onRunManualSandboxCmd() {
    const QString cmd = m_editManualCmd->text().trimmed();
    if (cmd.isEmpty()) return;
    m_editManualCmd->clear();
    m_sandbox->startCommand(cmd, 30000, [this](const SandboxRunResult&) {
        refreshViews();
    });
}

} // namespace agora
