#pragma once

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>

namespace agora {

struct SandboxRunResult {
    int exitCode = -1;
    QString stdoutText;
    QString stderrText;
    bool timedOut = false;
    bool truncated = false;
    bool bwrapUsed = false;

    QString combinedOutput() const {
        QString out = stdoutText;
        if (!stderrText.isEmpty()) {
            if (!out.isEmpty() && !out.endsWith('\n')) out += '\n';
            out += stderrText;
        }
        return out;
    }
};

class BwrapSandbox : public QObject {
    Q_OBJECT
public:
    static constexpr qsizetype kMaxOutputBytes = 64 * 1024; // 64 KiB cap

    explicit BwrapSandbox(const QString& workspacePath, QObject* parent = nullptr);

    void setWorkspacePath(const QString& workspacePath);
    QString workspacePath() const { return m_workspacePath; }

    void setAllowNetwork(bool allow) { m_allowNetwork = allow; }
    bool allowNetwork() const { return m_allowNetwork; }

    bool ensureWorkspaceInitialized(QString* errorOut = nullptr);
    static bool isBwrapAvailable();

    QStringList buildBwrapArgs(const QString& shellCommand,
                               bool protectGitDir = true,
                               bool applyResourceLimits = true) const;
    SandboxRunResult runCommand(const QString& shellCommand, int timeoutMs = 30000);
    void cancelRunningCommand();

    QString gitDiff() const;
    QString gitStatusShort() const;

signals:
    void commandStarted(const QString& command);
    void commandOutputChunk(const QString& chunk);
    void commandFinished(int exitCode, const QString& combinedOutput);

private:
    SandboxRunResult runInternal(const QString& shellCommand,
                                 int timeoutMs,
                                 bool protectGitDir,
                                 bool applyResourceLimits,
                                 bool emitUiSignals) const;

    QString m_workspacePath;
    bool m_allowNetwork = false;
    mutable QPointer<QProcess> m_activeProcess;
};

} // namespace agora
