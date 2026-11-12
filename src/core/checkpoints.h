#pragma once

#include "core/sandbox.h"

#include <QObject>
#include <QString>
#include <functional>

namespace agora {

struct Checkpoint {
    bool ok = false;
    bool changed = false; // false when the workspace matched the previous checkpoint
    QString commit;
    QString parent;
    QString error;
};

// Snapshots of the workspace stored as commits under refs/agora/<session>. They use a
// private index file and never move HEAD, touch branches or the user's staging area, so
// they are safe to take inside a real project. Untracked files are included; files
// matched by .gitignore (and common dependency folders) are not.
class CheckpointStore : public QObject {
    Q_OBJECT
public:
    explicit CheckpointStore(BwrapSandbox* sandbox, QObject* parent = nullptr);

    static QString refForSession(const QString& sessionId);
    static bool isValidCommitId(const QString& id);

    void snapshot(const QString& sessionId, const QString& label,
                  std::function<void(const Checkpoint&)> done);
    // Unified diff between two checkpoints; an empty `fromCommit` diffs against nothing
    void diff(const QString& fromCommit, const QString& toCommit,
              std::function<void(bool ok, const QString& diffOrError)> done);
    // Saves the current state as a checkpoint first, then makes the workspace match
    // `targetCommit` (restoring modified/deleted files, removing files added since)
    void restore(const QString& sessionId, const QString& targetCommit,
                 std::function<void(bool ok, const QString& savedCommitOrError)> done);

private:
    static QString sanitizedSessionKey(const QString& sessionId);

    BwrapSandbox* m_sandbox = nullptr;
};

} // namespace agora
