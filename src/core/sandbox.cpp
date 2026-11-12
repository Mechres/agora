#include "core/sandbox.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>

namespace agora {

BwrapSandbox::BwrapSandbox(const QString& workspacePath, QObject* parent)
    : QObject(parent), m_workspacePath(workspacePath) {}

void BwrapSandbox::setWorkspacePath(const QString& workspacePath) {
    m_workspacePath = workspacePath;
}

bool BwrapSandbox::isBwrapAvailable() {
    return !QStandardPaths::findExecutable("bwrap").isEmpty();
}

bool BwrapSandbox::cgroupLimitsAvailable() {
    // Probe once: a user systemd instance with cgroup delegation must be reachable
    static const bool available = []() {
        if (QStandardPaths::findExecutable("systemd-run").isEmpty()) return false;
        QProcess probe;
        probe.start("systemd-run", {"--user", "--scope", "--quiet", "--collect",
                                    "-p", "MemoryMax=64M", "-p", "TasksMax=8", "true"});
        return probe.waitForFinished(5000) &&
               probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0;
    }();
    return available;
}

bool BwrapSandbox::ensureWorkspaceInitialized(QString* errorOut) {
    if (m_workspacePath.trimmed().isEmpty()) {
        if (errorOut) *errorOut = "Workspace path is empty";
        return false;
    }

    QDir dir(m_workspacePath);
    if (!dir.exists() && !QDir().mkpath(m_workspacePath)) {
        if (errorOut) *errorOut = "Failed to create workspace directory: " + m_workspacePath;
        return false;
    }

    // Never touch an existing repository's config: the workspace may be a real user project.
    // Sandboxed commit identity comes from GIT_AUTHOR_*/GIT_COMMITTER_* env vars instead.
    const QString gitDir = dir.filePath(".git");
    if (!QFileInfo::exists(gitDir)) {
        QProcess gitInit;
        gitInit.setWorkingDirectory(m_workspacePath);
        gitInit.start("git", {"init", "-q"});
        gitInit.waitForFinished(5000);
    }

    // hooks/ must exist so it can be mounted read-only; otherwise a sandboxed process could
    // create it and plant hooks that run the next time the user invokes git on the host.
    if (QFileInfo(gitDir).isDir()) {
        QDir().mkpath(QDir(gitDir).filePath("hooks"));
    }
    return true;
}

QStringList BwrapSandbox::buildBwrapArgs(const QString& shellCommand, bool applyResourceLimits) const {
    QStringList args;
    args << "--die-with-parent" << "--new-session";
    if (m_allowNetwork) {
        args << "--unshare-user" << "--unshare-ipc" << "--unshare-pid" << "--unshare-uts" << "--share-net";
    } else {
        args << "--unshare-all";
    }

    const QStringList roSystemDirs = {"/usr", "/bin", "/sbin", "/lib", "/lib64", "/opt"};
    for (const QString& d : roSystemDirs) {
        if (QFileInfo::exists(d)) {
            args << "--ro-bind" << d << d;
        }
    }

    // Narrow /etc mounts to only essential runtime/linker/TLS files
    args << "--dir" << "/etc";
    const QStringList roEtcEntries = {
        "/etc/ld.so.cache",
        "/etc/ld.so.conf",
        "/etc/ld.so.conf.d",
        "/etc/alternatives",
        "/etc/ssl",
        "/etc/ca-certificates",
        "/etc/pki",
        "/etc/passwd",
        "/etc/group",
        "/etc/nsswitch.conf",
        "/etc/localtime"
    };
    for (const QString& e : roEtcEntries) {
        if (QFileInfo::exists(e)) {
            args << "--ro-bind" << e << e;
        }
    }
    if (m_allowNetwork) {
        for (const QString& netEtc : {QStringLiteral("/etc/resolv.conf"), QStringLiteral("/etc/hosts")}) {
            if (QFileInfo::exists(netEtc)) {
                args << "--ro-bind" << netEtc << netEtc;
            }
        }
    }

    args << "--proc" << "/proc";
    args << "--dev" << "/dev";
    args << "--tmpfs" << "/tmp";
    args << "--tmpfs" << "/run";

    const QString absWs = QFileInfo(m_workspacePath).absoluteFilePath();
    args << "--bind" << absWs << "/workspace";

    // Git always runs inside the sandbox, so the repo itself stays writable (agents can commit).
    // Only config and hooks are read-only, so nothing planted there can fire if the user later
    // runs git on the host.
    const QString absGit = QDir(absWs).filePath(".git");
    if (QFileInfo(absGit).isDir()) {
        for (const QString& entry : {QStringLiteral("config"), QStringLiteral("hooks")}) {
            const QString hostPath = QDir(absGit).filePath(entry);
            if (QFileInfo::exists(hostPath)) {
                args << "--ro-bind" << hostPath << ("/workspace/.git/" + entry);
            }
        }
    }

    args << "--chdir" << "/workspace";
    args << "--setenv" << "HOME" << "/tmp";
    args << "--setenv" << "PYTHONDONTWRITEBYTECODE" << "1";
    args << "--setenv" << "GIT_CONFIG_NOSYSTEM" << "1";
    args << "--setenv" << "GIT_CONFIG_GLOBAL" << "/dev/null";
    args << "--setenv" << "GIT_AUTHOR_NAME" << "Agora Sandbox";
    args << "--setenv" << "GIT_AUTHOR_EMAIL" << "agora@local";
    args << "--setenv" << "GIT_COMMITTER_NAME" << "Agora Sandbox";
    args << "--setenv" << "GIT_COMMITTER_EMAIL" << "agora@local";
    args << "--setenv" << "PATH" << "/usr/local/bin:/usr/bin:/bin";

    // CPU time is bounded by the wall-clock timeout; memory/tasks by the systemd scope when
    // available, otherwise by an address-space ulimit.
    QString wrappedCmd = shellCommand;
    if (applyResourceLimits) {
        QString prefix = QStringLiteral("ulimit -f %1 2>/dev/null; ")
                             .arg(QString::number(qint64(m_limits.maxFileSizeMiB) * 1024)); // 1 KiB blocks
        if (!cgroupLimitsAvailable()) {
            prefix += QStringLiteral("ulimit -v %1 2>/dev/null; ")
                          .arg(QString::number(qint64(m_limits.memoryMaxMiB) * 1024)); // KiB
        }
        wrappedCmd = prefix + shellCommand;
    }

    args << "/bin/sh" << "-c" << wrappedCmd;
    return args;
}

void BwrapSandbox::cancelRunningCommand() {
    for (const QPointer<QProcess>& proc : std::as_const(m_activeProcesses)) {
        if (proc) proc->kill();
    }
}

SandboxRunResult BwrapSandbox::runCommand(const QString& shellCommand, int timeoutMs) {
    return runInternal(shellCommand, timeoutMs, /*applyResourceLimits=*/true, /*emitUiSignals=*/true);
}

SandboxRunResult BwrapSandbox::runInternal(const QString& shellCommand,
                                           int timeoutMs,
                                           bool applyResourceLimits,
                                           bool emitUiSignals) {
    SandboxRunResult res;
    QString err;
    if (!ensureWorkspaceInitialized(&err)) {
        res.stderrText = err;
        res.exitCode = 1;
        return res;
    }

    const QString bwrapBin = QStandardPaths::findExecutable("bwrap");
    if (bwrapBin.isEmpty()) {
        res.stderrText = "Error: bubblewrap (/usr/bin/bwrap) is required for sandbox execution.";
        res.exitCode = 127;
        return res;
    }

    QString program = bwrapBin;
    QStringList programArgs = buildBwrapArgs(shellCommand, applyResourceLimits);
    if (applyResourceLimits && cgroupLimitsAvailable()) {
        // systemd-run --scope execs bwrap in place, so killing our child still kills bwrap
        program = QStringLiteral("systemd-run");
        programArgs = QStringList{
            "--user", "--scope", "--quiet", "--collect",
            "-p", QStringLiteral("MemoryMax=%1M").arg(QString::number(m_limits.memoryMaxMiB)),
            "-p", "MemorySwapMax=0",
            "-p", QStringLiteral("TasksMax=%1").arg(QString::number(m_limits.tasksMax)),
            "--", bwrapBin} + programArgs;
    }

    res.bwrapUsed = true;
    if (emitUiSignals) {
        emit commandStarted(shellCommand);
    }

    QProcess proc;
    m_activeProcesses.append(&proc);
    proc.setProcessChannelMode(QProcess::SeparateChannels);

    qsizetype keptBytes = 0;
    qsizetype droppedBytes = 0;

    // Past the cap, keep draining the pipes but discard the bytes: killing the command here
    // would abort long builds/test runs halfway and misreport their result.
    auto appendCapped = [&](QString& dest, const QByteArray& raw) {
        if (res.truncated) {
            droppedBytes += raw.size();
            return;
        }
        qsizetype take = qMin(raw.size(), kMaxOutputBytes - keptBytes);
        if (take < raw.size()) {
            // Don't split a UTF-8 sequence at the cut point
            while (take > 0 && (static_cast<unsigned char>(raw[take]) & 0xC0) == 0x80) --take;
            res.truncated = true;
            droppedBytes += raw.size() - take;
        }
        if (take > 0) {
            const QString chunk = QString::fromUtf8(raw.left(take));
            dest += chunk;
            keptBytes += take;
            if (emitUiSignals) emit commandOutputChunk(chunk);
        }
        if (res.truncated && emitUiSignals) {
            emit commandOutputChunk(QStringLiteral("\n...[output limit reached; further output hidden]...\n"));
        }
    };

    QObject::connect(&proc, &QProcess::readyReadStandardOutput, [&]() {
        appendCapped(res.stdoutText, proc.readAllStandardOutput());
    });
    QObject::connect(&proc, &QProcess::readyReadStandardError, [&]() {
        appendCapped(res.stderrText, proc.readAllStandardError());
    });

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);

    QObject::connect(&proc, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(&proc, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        res.timedOut = true;
        proc.kill();
        loop.quit();
    });

    timer.start(qMax(500, timeoutMs));
    proc.start(program, programArgs);
    loop.exec();
    m_activeProcesses.removeAll(&proc);

    appendCapped(res.stdoutText, proc.readAllStandardOutput());
    appendCapped(res.stderrText, proc.readAllStandardError());

    if (res.truncated) {
        res.stderrText += QStringLiteral("\n[output truncated: first %1 KiB shown, %2 bytes omitted]")
                              .arg(QString::number(kMaxOutputBytes / 1024), QString::number(droppedBytes));
    }

    if (res.timedOut) {
        res.exitCode = 124;
        res.stderrText += QStringLiteral("\n[Sandbox timeout after %1 ms]").arg(QString::number(timeoutMs));
    } else {
        res.exitCode = proc.exitStatus() == QProcess::NormalExit ? proc.exitCode() : 1;
    }

    if (emitUiSignals) {
        emit commandFinished(res.exitCode, res.combinedOutput());
    }
    return res;
}

QString BwrapSandbox::gitDiff() {
    if (m_workspacePath.isEmpty() || !QDir(m_workspacePath).exists()) {
        return {};
    }
    // Run git inside the bwrap sandbox with disabled fsmonitor/hooks/external diff
    const QString cmd =
        "git -c core.fsmonitor=false -c core.hooksPath=/dev/null -c diff.external= add -N . >/dev/null 2>&1; "
        "git -c core.fsmonitor=false -c core.hooksPath=/dev/null -c diff.external= diff --no-color --no-ext-diff";
    return runInternal(cmd, 5000, /*applyResourceLimits=*/false, /*emitUiSignals=*/false).stdoutText;
}

QString BwrapSandbox::gitStatusShort() {
    if (m_workspacePath.isEmpty() || !QDir(m_workspacePath).exists()) {
        return {};
    }
    const QString cmd =
        "git -c core.fsmonitor=false -c core.hooksPath=/dev/null status --short";
    return runInternal(cmd, 3000, /*applyResourceLimits=*/false, /*emitUiSignals=*/false).stdoutText;
}

} // namespace agora
