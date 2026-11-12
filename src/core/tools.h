#pragma once

#include "core/sandbox.h"
#include "core/types.h"

#include <QJsonArray>
#include <QObject>
#include <QString>
#include <functional>
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

    // Tools that cannot modify the workspace or run code
    static bool isReadOnlyTool(const QString& toolName);

    std::optional<QString> resolveSafeWorkspacePath(const QString& relativePath, QString* errorOut = nullptr) const;
    // `done` is always invoked later from the event loop, never synchronously
    void executeToolCallAsync(const ToolCall& call, std::function<void(const ToolResult&)> done);
    // Blocking wrapper (spins a local event loop for run_command/git_diff). Tests and
    // non-GUI callers only.
    ToolResult executeToolCall(const ToolCall& call);

signals:
    void workspaceModified(const QString& relativePath);

private:
    ToolResult execListFiles(const QString& callId, const QJsonObject& args);
    ToolResult execReadFile(const QString& callId, const QJsonObject& args);
    ToolResult execWriteFile(const QString& callId, const QJsonObject& args);
    ToolResult execPatchFile(const QString& callId, const QJsonObject& args);
    void execRunCommandAsync(const QString& callId, const QJsonObject& args,
                             std::function<void(const ToolResult&)> done);
    void execGitDiffAsync(const QString& callId, std::function<void(const ToolResult&)> done);

    BwrapSandbox* m_sandbox = nullptr;
};

} // namespace agora
