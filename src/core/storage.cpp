#include "core/storage.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace agora {

SessionStore::SessionStore(const QString& dbPath)
    : m_dbPath(dbPath),
      m_connectionName("agora_sql_" + QUuid::createUuid().toString(QUuid::Id128)) {}

SessionStore::~SessionStore() {
    if (QSqlDatabase::contains(m_connectionName)) {
        {
            QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
            if (db.isOpen()) {
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool SessionStore::initialize(QString* errorOut) {
    QFileInfo fi(m_dbPath);
    if (!fi.dir().exists() && !QDir().mkpath(fi.dir().absolutePath())) {
        if (errorOut) *errorOut = "Failed to create database directory: " + fi.dir().absolutePath();
        return false;
    }

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    db.setDatabaseName(m_dbPath);
    if (!db.open()) {
        if (errorOut) *errorOut = db.lastError().text();
        return false;
    }

    QSqlQuery query(db);
    const QString ddlSessions = R"(
        CREATE TABLE IF NOT EXISTS sessions (
            id TEXT PRIMARY KEY,
            title TEXT NOT NULL,
            workspace_path TEXT NOT NULL,
            mode INTEGER NOT NULL,
            max_rounds INTEGER NOT NULL,
            vram_budget REAL NOT NULL,
            allow_network INTEGER NOT NULL,
            participants_json TEXT NOT NULL,
            messages_json TEXT NOT NULL,
            updated_at TEXT NOT NULL
        )
    )";
    if (!query.exec(ddlSessions)) {
        if (errorOut) *errorOut = query.lastError().text();
        return false;
    }

    const QString ddlMessages = R"(
        CREATE TABLE IF NOT EXISTS session_messages (
            session_id TEXT NOT NULL,
            seq INTEGER NOT NULL,
            message_json TEXT NOT NULL,
            PRIMARY KEY (session_id, seq)
        )
    )";
    if (!query.exec(ddlMessages)) {
        if (errorOut) *errorOut = query.lastError().text();
        return false;
    }
    return true;
}

bool SessionStore::saveSession(const RoomSession& session, QString* errorOut) {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) {
        if (errorOut) *errorOut = "Database is not open";
        return false;
    }

    db.transaction();

    QJsonArray pArr;
    for (const auto& p : session.participants) pArr.append(p.toJson());

    QSqlQuery q(db);
    q.prepare(R"(
        INSERT OR REPLACE INTO sessions
        (id, title, workspace_path, mode, max_rounds, vram_budget, allow_network, participants_json, messages_json, updated_at)
        VALUES (:id, :title, :workspace_path, :mode, :max_rounds, :vram_budget, :allow_network, :participants_json, :messages_json, :updated_at)
    )");
    q.bindValue(":id", session.id);
    q.bindValue(":title", session.title);
    q.bindValue(":workspace_path", session.workspacePath);
    q.bindValue(":mode", static_cast<int>(session.mode));
    q.bindValue(":max_rounds", session.maxRounds);
    q.bindValue(":vram_budget", session.vramBudgetGiB);
    q.bindValue(":allow_network", session.allowSandboxNetwork ? 1 : 0);
    q.bindValue(":participants_json", QString::fromUtf8(QJsonDocument(pArr).toJson(QJsonDocument::Compact)));
    q.bindValue(":messages_json", QStringLiteral("[]"));
    q.bindValue(":updated_at", session.updatedAt.isEmpty()
                                   ? QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
                                   : session.updatedAt);

    if (!q.exec()) {
        db.rollback();
        if (errorOut) *errorOut = q.lastError().text();
        return false;
    }

    QSqlQuery delMsg(db);
    delMsg.prepare("DELETE FROM session_messages WHERE session_id = :sid");
    delMsg.bindValue(":sid", session.id);
    delMsg.exec();

    QSqlQuery insMsg(db);
    insMsg.prepare("INSERT INTO session_messages (session_id, seq, message_json) VALUES (:sid, :seq, :mjson)");
    for (int i = 0; i < session.messages.size(); ++i) {
        insMsg.bindValue(":sid", session.id);
        insMsg.bindValue(":seq", i);
        insMsg.bindValue(":mjson", QString::fromUtf8(QJsonDocument(session.messages[i].toJson()).toJson(QJsonDocument::Compact)));
        if (!insMsg.exec()) {
            db.rollback();
            if (errorOut) *errorOut = insMsg.lastError().text();
            return false;
        }
    }

    db.commit();
    return true;
}

std::optional<RoomSession> SessionStore::loadSession(const QString& sessionId, QString* errorOut) {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) {
        if (errorOut) *errorOut = "Database is not open";
        return std::nullopt;
    }

    QSqlQuery q(db);
    q.prepare("SELECT id, title, workspace_path, mode, max_rounds, vram_budget, allow_network, participants_json, messages_json, updated_at FROM sessions WHERE id = :id");
    q.bindValue(":id", sessionId);
    if (!q.exec() || !q.next()) {
        if (errorOut && q.lastError().isValid()) *errorOut = q.lastError().text();
        return std::nullopt;
    }

    RoomSession s;
    s.id = q.value(0).toString();
    s.title = q.value(1).toString();
    s.workspacePath = q.value(2).toString();
    s.mode = static_cast<DebateMode>(q.value(3).toInt());
    s.maxRounds = q.value(4).toInt();
    s.vramBudgetGiB = q.value(5).toDouble();
    s.allowSandboxNetwork = q.value(6).toInt() != 0;
    s.updatedAt = q.value(9).toString();

    const QJsonArray pArr = QJsonDocument::fromJson(q.value(7).toString().toUtf8()).array();
    for (const auto& v : pArr) s.participants.append(Participant::fromJson(v.toObject()));

    QSqlQuery mq(db);
    mq.prepare("SELECT message_json FROM session_messages WHERE session_id = :sid ORDER BY seq ASC");
    mq.bindValue(":sid", sessionId);
    if (mq.exec()) {
        while (mq.next()) {
            const QJsonObject mObj = QJsonDocument::fromJson(mq.value(0).toString().toUtf8()).object();
            s.messages.append(ChatMessage::fromJson(mObj));
        }
    }

    // Fallback for legacy sessions stored before session_messages table
    if (s.messages.isEmpty()) {
        const QJsonArray mArr = QJsonDocument::fromJson(q.value(8).toString().toUtf8()).array();
        for (const auto& v : mArr) s.messages.append(ChatMessage::fromJson(v.toObject()));
    }

    return s;
}

QVector<SessionSummary> SessionStore::listSessionSummaries(QString* errorOut) {
    QVector<SessionSummary> out;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) {
        if (errorOut) *errorOut = "Database is not open";
        return out;
    }

    QSqlQuery q(db);
    const QString sql = R"(
        SELECT s.id, s.title, s.updated_at,
               (SELECT COUNT(*) FROM session_messages m WHERE m.session_id = s.id) AS msg_count
        FROM sessions s
        ORDER BY s.updated_at DESC
    )";
    if (!q.exec(sql)) {
        if (errorOut) *errorOut = q.lastError().text();
        return out;
    }

    while (q.next()) {
        SessionSummary sum;
        sum.id = q.value(0).toString();
        sum.title = q.value(1).toString();
        sum.updatedAt = q.value(2).toString();
        sum.messageCount = q.value(3).toInt();
        out.append(sum);
    }
    return out;
}

QVector<RoomSession> SessionStore::listSessions(QString* errorOut) {
    QVector<RoomSession> out;
    const auto summaries = listSessionSummaries(errorOut);
    for (const auto& sum : summaries) {
        if (auto s = loadSession(sum.id, errorOut)) {
            out.append(*s);
        }
    }
    return out;
}

bool SessionStore::deleteSession(const QString& sessionId, QString* errorOut) {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery delMsg(db);
    delMsg.prepare("DELETE FROM session_messages WHERE session_id = :id");
    delMsg.bindValue(":id", sessionId);
    delMsg.exec();

    QSqlQuery q(db);
    q.prepare("DELETE FROM sessions WHERE id = :id");
    q.bindValue(":id", sessionId);
    if (!q.exec()) {
        if (errorOut) *errorOut = q.lastError().text();
        return false;
    }
    return true;
}

QString SessionStore::exportToMarkdown(const RoomSession& session) {
    QString md;
    md += "# " + (session.title.isEmpty() ? QStringLiteral("Agora Debate Session") : session.title) + "\n\n";
    md += "- **Workspace**: `" + session.workspacePath + "`\n";
    md += "- **Updated**: " + session.updatedAt + "\n\n";
    md += "## Participants\n\n";
    for (const auto& p : session.participants) {
        md += QStringLiteral("- **%1** (`%2`) — Model: `%3`\n").arg(p.displayName, p.handle, p.modelId);
    }
    md += "\n---\n\n## Transcript\n\n";
    for (const auto& m : session.messages) {
        md += QStringLiteral("### %1 (`%2`) %3\n\n")
                  .arg(m.displayName.isEmpty() ? m.senderHandle : m.displayName,
                       m.senderHandle,
                       m.modelId.isEmpty() ? QString() : QStringLiteral("[`%1`]").arg(m.modelId));
        if (!m.reasoningContent.trimmed().isEmpty()) {
            md += "<details><summary>Reasoning</summary>\n\n" + m.reasoningContent.trimmed() + "\n\n</details>\n\n";
        }
        if (!m.content.trimmed().isEmpty()) {
            md += m.content.trimmed() + "\n\n";
        }
        for (const auto& tr : m.toolResults) {
            md += QStringLiteral("```text\n[Tool: %1 | exit=%2]\n%3\n```\n\n")
                      .arg(tr.name, QString::number(tr.exitCode), tr.output.trimmed());
        }
    }
    return md;
}

} // namespace agora
