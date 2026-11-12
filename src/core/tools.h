#pragma once

#include "core/sandbox.h"
#include "core/types.h"

#include <QJsonArray>
#include <QObject>
#include <QString>
#include <optional>

namespace agora {

class ToolEngine : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxListEntries = 200;

    explicit ToolEngine(BwrapSandbox* sandbox, QObject* parent = nullptr);

    BwrapSandbox* sandbox() const { return m_sandbox; }

    static QJsonArray openAiToolsSchema();
    static QVector<ToolCall> extractFallbackToolCalls(const QString& rawText, QString* cleanedTextOut = nullptr);

    std::optional<QString> resolveSafeWorkspacePath(const QString& relativePath, QString* errorOut = nullptr) const;
    ToolResult executeToolCall(const ToolCall& call);

signals:
    void workspaceModified(const QString& relativePath);

private:
    ToolResult execListFiles(const QString& callId, const QJsonObject& args);
    ToolResult execReadFile(const QString& callId, const QJsonObject& args);
    ToolResult execWriteFile(const QString& callId, const QJsonObject& args);
    ToolResult execPatchFile(const QString& callId, const QJsonObject& args);
    ToolResult execRunCommand(const QString& callId, const QJsonObject& args);
    ToolResult execGitDiff(const QString& callId);

    BwrapSandbox* m_sandbox = nullptr;
};

} // namespace agora
