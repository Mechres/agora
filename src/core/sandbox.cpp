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

    const QString gitDir = dir.filePath(".git");
    if (!QDir(gitDir).exists()) {
        QProcess gitInit;
        gitInit.setWorkingDirectory(m_workspacePath);
        gitInit.start("git", {"-c", "core.fsmonitor=false", "-c", "core.hooksPath=/dev/null", "init", "-q"});
        gitInit.waitForFinished(5000);
    }

    // Always sanitize .git/config before mounting it read-only into the sandbox
    const QString gitConfigPath = QDir(gitDir).filePath("config");
    if (QFileInfo::exists(gitConfigPath)) {
        QFile cfg(gitConfigPath);
        if (cfg.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            const QByteArray safeCfg =
                "[core]\n"
                "\trepositoryformatversion = 0\n"
                "\tfilemode = true\n"
                "\tbare = false\n"
                "\tlogallrefupdates = true\n"
                "\tfsmonitor = false\n"
                "\thooksPath = /dev/null\n"
                "[user]\n"
                "\tname = Agora Sandbox\n"
                "\temail = agora@local\n";
            cfg.write(safeCfg);
            cfg.close();
        }
    }
    return true;
}

QStringList BwrapSandbox::buildBwrapArgs(const QString& shellCommand,
                                         bool protectGitDir,
                                         bool applyResourceLimits) const {
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

    // Prevent sandboxed processes from tampering with .git/config or .git/hooks
    const QString absGit = QDir(absWs).filePath(".git");
    if (QFileInfo::exists(absGit)) {
        if (protectGitDir) {
            args << "--ro-bind" << absGit << "/workspace/.git";
        } else {
            // Even when git itself updates the index inside bwrap, keep config and hooks read-only
            const QString cfg = QDir(absGit).filePath("config");
            if (QFileInfo::exists(cfg)) {
                args << "--ro-bind" << cfg << "/workspace/.git/config";
            }
            const QString hooks = QDir(absGit).filePath("hooks");
            if (QFileInfo::exists(hooks)) {
                args << "--ro-bind" << hooks << "/workspace/.git/hooks";
            }
        }
    }

    args << "--chdir" << "/workspace";
    args << "--setenv" << "HOME" << "/tmp";
    args << "--setenv" << "PYTHONDONTWRITEBYTECODE" << "1";
    args << "--setenv" << "GIT_CONFIG_NOSYSTEM" << "1";
    args << "--setenv" << "GIT_CONFIG_GLOBAL" << "/dev/null";
    args << "--setenv" << "PATH" << "/usr/local/bin:/usr/bin:/bin";

    // Apply POSIX resource limits inside the isolated session:
    // -f 65536 (64 MiB max file size written by shell children), -t 30 (CPU seconds)
    QString wrappedCmd = shellCommand;
    if (applyResourceLimits) {
        wrappedCmd = QStringLiteral("ulimit -f 65536 -t 30 2>/dev/null; ") + shellCommand;
    }

    args << "/bin/sh" << "-c" << wrappedCmd;
    return args;
}

void BwrapSandbox::cancelRunningCommand() {
    if (m_activeProcess) {
        m_activeProcess->kill();
    }
}

SandboxRunResult BwrapSandbox::runCommand(const QString& shellCommand, int timeoutMs) {
    return runInternal(shellCommand, timeoutMs, /*protectGitDir=*/true, /*applyResourceLimits=*/true, /*emitUiSignals=*/true);
}

SandboxRunResult BwrapSandbox::runInternal(const QString& shellCommand,
                                           int timeoutMs,
                                           bool protectGitDir,
                                           bool applyResourceLimits,
                                           bool emitUiSignals) const {
    SandboxRunResult res;
    QString err;
    if (!const_cast<BwrapSandbox*>(this)->ensureWorkspaceInitialized(&err)) {
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

    res.bwrapUsed = true;
    if (emitUiSignals) {
        emit const_cast<BwrapSandbox*>(this)->commandStarted(shellCommand);
    }

    QProcess proc;
    m_activeProcess = &proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);

    qsizetype totalBytes = 0;

    auto appendCapped = [&](QString& dest, const QByteArray& raw) {
        if (res.truncated) return;
        const qsizetype remaining = kMaxOutputBytes - totalBytes;
        if (raw.size() <= remaining) {
            const QString chunk = QString::fromUtf8(raw);
            dest += chunk;
            totalBytes += raw.size();
            if (emitUiSignals) {
                emit const_cast<BwrapSandbox*>(this)->commandOutputChunk(chunk);
            }
        } else {
            if (remaining > 0) {
                const QString chunk = QString::fromUtf8(raw.left(remaining));
                dest += chunk;
                totalBytes += remaining;
                if (emitUiSignals) {
                    emit const_cast<BwrapSandbox*>(this)->commandOutputChunk(chunk);
                }
            }
            res.truncated = true;
            const QString truncMsg = "\n...[output truncated at 64 KiB]...\n";
            dest += truncMsg;
            if (emitUiSignals) {
                emit const_cast<BwrapSandbox*>(this)->commandOutputChunk(truncMsg);
            }
            proc.kill();
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
    proc.start(bwrapBin, buildBwrapArgs(shellCommand, protectGitDir, applyResourceLimits));
    loop.exec();
    m_activeProcess = nullptr;

    if (!res.truncated) {
        const QByteArray remOut = proc.readAllStandardOutput();
        if (!remOut.isEmpty()) appendCapped(res.stdoutText, remOut);
        const QByteArray remErr = proc.readAllStandardError();
        if (!remErr.isEmpty()) appendCapped(res.stderrText, remErr);
    }

    if (res.timedOut) {
        res.exitCode = 124;
        res.stderrText += QStringLiteral("\n[Sandbox timeout after %1 ms]").arg(timeoutMs);
    } else if (res.truncated && proc.exitStatus() != QProcess::NormalExit) {
        res.exitCode = 0;
    } else {
        res.exitCode = proc.exitStatus() == QProcess::NormalExit ? proc.exitCode() : 1;
    }

    if (emitUiSignals) {
        emit const_cast<BwrapSandbox*>(this)->commandFinished(res.exitCode, res.combinedOutput());
    }
    return res;
}

QString BwrapSandbox::gitDiff() const {
    if (m_workspacePath.isEmpty() || !QDir(m_workspacePath).exists()) {
        return {};
    }
    // Run git inside the bwrap sandbox with disabled fsmonitor/hooks/external diff
    const QString cmd =
        "git -c core.fsmonitor=false -c core.hooksPath=/dev/null -c diff.external= add -N . >/dev/null 2>&1; "
        "git -c core.fsmonitor=false -c core.hooksPath=/dev/null -c diff.external= diff --no-color --no-ext-diff";
    const SandboxRunResult r = runInternal(cmd, 5000, /*protectGitDir=*/false, /*applyResourceLimits=*/false, /*emitUiSignals=*/false);
    return r.stdoutText;
}

QString BwrapSandbox::gitStatusShort() const {
    if (m_workspacePath.isEmpty() || !QDir(m_workspacePath).exists()) {
        return {};
    }
    const QString cmd =
        "git -c core.fsmonitor=false -c core.hooksPath=/dev/null status --short";
    const SandboxRunResult r = runInternal(cmd, 3000, /*protectGitDir=*/true, /*applyResourceLimits=*/false, /*emitUiSignals=*/false);
    return r.stdoutText;
}

} // namespace agora
