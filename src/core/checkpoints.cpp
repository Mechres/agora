#include "core/checkpoints.h"

#include <QRegularExpression>
#include <QTimer>

namespace agora {

namespace {

constexpr int kCheckpointTimeoutMs = 60000;

const SandboxRunOptions kQuietRun{/*applyResourceLimits=*/false, /*emitUiSignals=*/false};

// Common prelude: hardened git wrapper plus the session's private index
QString scriptPrelude(const QString& key) {
    return QStringLiteral(
               "set -e\n"
               "g() { git -c core.fsmonitor=false -c core.hooksPath=/dev/null \"$@\"; }\n"
               "export GIT_INDEX_FILE=.git/agora-index-%1\n")
        .arg(key);
}

} // namespace

CheckpointStore::CheckpointStore(BwrapSandbox* sandbox, QObject* parent)
    : QObject(parent), m_sandbox(sandbox) {}

QString CheckpointStore::sanitizedSessionKey(const QString& sessionId) {
    QString key = sessionId;
    key.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")));
    return key.isEmpty() ? QStringLiteral("default") : key.left(64);
}

QString CheckpointStore::refForSession(const QString& sessionId) {
    return "refs/agora/" + sanitizedSessionKey(sessionId);
}

bool CheckpointStore::isValidCommitId(const QString& id) {
    static const QRegularExpression re(QStringLiteral("^[0-9a-f]{7,64}$"));
    return re.match(id).hasMatch();
}

void CheckpointStore::snapshot(const QString& sessionId, const QString& label,
                               std::function<void(const Checkpoint&)> done) {
    const QString key = sanitizedSessionKey(sessionId);
    const QString ref = refForSession(sessionId);

    // The label ends up inside single quotes, so keep it to a harmless character set
    QString safeLabel = label;
    safeLabel.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9 @_.:-]")), QStringLiteral("_"));
    safeLabel = "agora: " + safeLabel.left(100);

    const QString script = scriptPrelude(key) + QStringLiteral(
        "g add -A -- . ':(exclude,glob)**/node_modules/**' ':(exclude,glob)**/.venv/**' "
        "':(exclude,glob)**/venv/**' ':(exclude,glob)**/__pycache__/**' >/dev/null 2>&1\n"
        "tree=$(g write-tree)\n"
        "parent=$(g rev-parse -q --verify %1 2>/dev/null || true)\n"
        "if [ -n \"$parent\" ] && [ \"$(g rev-parse \"$parent^{tree}\")\" = \"$tree\" ]; then\n"
        "  echo \"same $parent\"; exit 0\n"
        "fi\n"
        "if [ -n \"$parent\" ]; then c=$(g commit-tree \"$tree\" -p \"$parent\" -m '%2')\n"
        "else c=$(g commit-tree \"$tree\" -m '%2'); fi\n"
        "g update-ref %1 \"$c\"\n"
        "echo \"new $c $parent\"\n").arg(ref, safeLabel);

    m_sandbox->startCommand(script, kCheckpointTimeoutMs, [done](const SandboxRunResult& r) {
        Checkpoint cp;
        const QStringList parts = r.stdoutText.trimmed().split(' ', Qt::SkipEmptyParts);
        if (r.exitCode != 0 || parts.size() < 2 || !isValidCommitId(parts[1])) {
            cp.error = r.combinedOutput().trimmed();
            if (cp.error.isEmpty()) cp.error = QStringLiteral("snapshot failed (exit %1)").arg(r.exitCode);
            done(cp);
            return;
        }
        cp.ok = true;
        cp.commit = parts[1];
        if (parts[0] == "new") {
            cp.changed = true;
            cp.parent = parts.value(2);
        } else {
            cp.parent = cp.commit;
        }
        done(cp);
    }, kQuietRun);
}

void CheckpointStore::diff(const QString& fromCommit, const QString& toCommit,
                           std::function<void(bool, const QString&)> done) {
    if (!isValidCommitId(toCommit) || (!fromCommit.isEmpty() && !isValidCommitId(fromCommit))) {
        QTimer::singleShot(0, this, [done]() { done(false, QStringLiteral("Invalid checkpoint id")); });
        return;
    }
    const QString from = fromCommit.isEmpty() ? QStringLiteral("$(g hash-object -t tree /dev/null)") : fromCommit;
    const QString script = QStringLiteral(
        "g() { git -c core.fsmonitor=false -c core.hooksPath=/dev/null -c diff.external= \"$@\"; }\n"
        "g diff --no-color --no-ext-diff --stat --patch %1 %2\n").arg(from, toCommit);
    m_sandbox->startCommand(script, kCheckpointTimeoutMs, [done](const SandboxRunResult& r) {
        if (r.exitCode != 0) {
            done(false, r.combinedOutput().trimmed());
            return;
        }
        done(true, r.stdoutText);
    }, kQuietRun);
}

void CheckpointStore::restore(const QString& sessionId, const QString& targetCommit,
                              std::function<void(bool, const QString&)> done) {
    if (!isValidCommitId(targetCommit)) {
        QTimer::singleShot(0, this, [done]() { done(false, QStringLiteral("Invalid checkpoint id")); });
        return;
    }
    const QString key = sanitizedSessionKey(sessionId);

    snapshot(sessionId, QStringLiteral("before restore to %1").arg(targetCommit.left(12)),
             [this, key, targetCommit, done](const Checkpoint& saved) {
        if (!saved.ok) {
            done(false, "Could not save the current state before restoring: " + saved.error);
            return;
        }
        const QString script = scriptPrelude(key) + QStringLiteral(
            "g diff -z --name-only --diff-filter=A %1 %2 | xargs -0 -r rm -f --\n"
            "g read-tree %1\n"
            "g checkout-index -a -f\n").arg(targetCommit, saved.commit);
        m_sandbox->startCommand(script, kCheckpointTimeoutMs, [saved, done](const SandboxRunResult& r) {
            if (r.exitCode != 0) {
                done(false, r.combinedOutput().trimmed());
                return;
            }
            done(true, saved.commit);
        }, kQuietRun);
    });
}

} // namespace agora
