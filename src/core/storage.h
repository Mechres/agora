#pragma once

#include "core/types.h"
#include <QString>
#include <QVector>
#include <optional>

namespace agora {

struct SessionSummary {
    QString id;
    QString title;
    QString updatedAt;
    int messageCount = 0;
};

class SessionStore {
public:
    explicit SessionStore(const QString& dbPath);
    ~SessionStore();

    bool initialize(QString* errorOut = nullptr);
    bool saveSession(const RoomSession& session, QString* errorOut = nullptr);
    std::optional<RoomSession> loadSession(const QString& sessionId, QString* errorOut = nullptr);
    QVector<SessionSummary> listSessionSummaries(QString* errorOut = nullptr);
    QVector<RoomSession> listSessions(QString* errorOut = nullptr);
    bool deleteSession(const QString& sessionId, QString* errorOut = nullptr);

    static QString exportToMarkdown(const RoomSession& session);

private:
    QString m_dbPath;
    QString m_connectionName;
};

} // namespace agora
