#pragma once

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <functional>

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

struct SandboxLimits {
    int memoryMaxMiB = 4096;   // per command, includes all children
    int tasksMax = 512;        // processes + threads per command
    int maxFileSizeMiB = 64;   // largest single file a command may write
};

struct SandboxRunOptions {
    bool applyResourceLimits = true;
    bool emitUiSignals = true;   // mirror output to the terminal pane
};

class BwrapSandbox : public QObject {
    Q_OBJECT
public:
    using CommandCallback = std::function<void(const SandboxRunResult&)>;

    static constexpr qsizetype kMaxOutputBytes = 64 * 1024; // 64 KiB cap

    explicit BwrapSandbox(const QString& workspacePath, QObject* parent = nullptr);
    ~BwrapSandbox() override;

    void setWorkspacePath(const QString& workspacePath);
    QString workspacePath() const { return m_workspacePath; }

    void setAllowNetwork(bool allow) { m_allowNetwork = allow; }
    bool allowNetwork() const { return m_allowNetwork; }

    void setLimits(const SandboxLimits& limits) { m_limits = limits; }
    SandboxLimits limits() const { return m_limits; }

    bool ensureWorkspaceInitialized(QString* errorOut = nullptr);
    static bool isBwrapAvailable();
    // True when memory/task limits are enforced by a systemd user scope (cgroup v2).
    // Otherwise only an address-space ulimit is applied and task count is unlimited.
    static bool cgroupLimitsAvailable();

    QStringList buildBwrapArgs(const QString& shellCommand, bool applyResourceLimits = true) const;

    // Starts the command and returns immediately; `done` is always invoked later from the
    // event loop (never synchronously), unless the sandbox is destroyed first.
    void startCommand(const QString& shellCommand,
                      int timeoutMs,
                      CommandCallback done,
                      SandboxRunOptions options = {});
    // Blocking wrapper around startCommand (spins a local event loop). For tests and
    // non-GUI callers only; GUI code must use startCommand.
    SandboxRunResult runCommand(const QString& shellCommand, int timeoutMs = 30000);
    void cancelRunningCommand();

    // Runs git inside the sandbox with hooks, fsmonitor and external diff disabled
    void gitStatusAndDiffAsync(std::function<void(const QString& status, const QString& diff)> done);
    QString gitDiff(); // blocking wrapper, tests only

signals:
    void commandStarted(const QString& command);
    void commandOutputChunk(const QString& chunk);
    void commandFinished(int exitCode, const QString& combinedOutput);

private:
    QString m_workspacePath;
    bool m_allowNetwork = false;
    SandboxLimits m_limits;
    // Several commands can be in flight at once (agent tool + manual terminal + diff refresh)
    QList<QPointer<QProcess>> m_activeProcesses;
};

} // namespace agora
