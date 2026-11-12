#include "core/tools.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>

namespace agora {

namespace {

int openNoFollowReadFd(const QString& path, QString* errorOut) {
    const QByteArray encoded = QFile::encodeName(path);
    const int fd = ::open(encoded.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errorOut) {
            *errorOut = (errno == ELOOP)
                            ? QStringLiteral("Refusing to follow symlink: %1").arg(path)
                            : QStringLiteral("Cannot open file for reading: %1").arg(path);
        }
    }
    return fd;
}

int openNoFollowWriteFd(const QString& path, QString* errorOut) {
    const QByteArray encoded = QFile::encodeName(path);
    const int fd = ::open(encoded.constData(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
        if (errorOut) {
            *errorOut = (errno == ELOOP)
                            ? QStringLiteral("Refusing to write through symlink: %1").arg(path)
                            : QStringLiteral("Cannot open file for writing: %1").arg(path);
        }
    }
    return fd;
}

} // namespace

ToolEngine::ToolEngine(BwrapSandbox* sandbox, QObject* parent)
    : QObject(parent), m_sandbox(sandbox) {}

QJsonArray ToolEngine::openAiToolsSchema() {
    auto makeProp = [](const QString& type, const QString& desc) {
        QJsonObject o;
        o["type"] = type;
        o["description"] = desc;
        return o;
    };

    auto makeTool = [](const QString& name, const QString& desc, const QJsonObject& props, const QStringList& req) {
        QJsonObject params;
        params["type"] = "object";
        params["properties"] = props;
        QJsonArray reqArr;
        for (const auto& r : req) reqArr.append(r);
        params["required"] = reqArr;

        QJsonObject fn;
        fn["name"] = name;
        fn["description"] = desc;
        fn["parameters"] = params;

        QJsonObject tool;
        tool["type"] = "function";
        tool["function"] = fn;
        return tool;
    };

    QJsonArray tools;

    {
        QJsonObject p;
        p["path"] = makeProp("string", "Relative directory path inside workspace (default '.')");
        tools.append(makeTool("list_files", "List files and directories inside the shared workspace.", p, {}));
    }
    {
        QJsonObject p;
        p["path"] = makeProp("string", "Relative file path inside workspace");
        p["start_line"] = makeProp("integer", "1-based start line (default 1)");
        p["end_line"] = makeProp("integer", "1-based end line (default 250)");
        tools.append(makeTool("read_file", "Read lines of a file from the shared workspace with line numbers.", p, {"path"}));
    }
    {
        QJsonObject p;
        p["path"] = makeProp("string", "Relative file path inside workspace");
        p["content"] = makeProp("string", "Full file content to write");
        tools.append(makeTool("write_file", "Create or overwrite a file in the shared workspace.", p, {"path", "content"}));
    }
    {
        QJsonObject p;
        p["path"] = makeProp("string", "Relative file path inside workspace");
        p["search"] = makeProp("string", "Exact unique text block to find");
        p["replace"] = makeProp("string", "Replacement text block");
        tools.append(makeTool("patch_file", "Replace an exact unique text block in an existing file inside the shared workspace.", p, {"path", "search", "replace"}));
    }
    {
        QJsonObject p;
        p["command"] = makeProp("string", "Shell command to run inside the Bubblewrap sandbox (/workspace)");
        p["timeout_sec"] = makeProp("integer", "Timeout in seconds (default 30)");
        tools.append(makeTool("run_command", "Execute a shell command inside the isolated Bubblewrap sandbox mounted at /workspace.", p, {"command"}));
    }
    {
        QJsonObject p;
        tools.append(makeTool("git_diff", "Inspect git diff of all changes made in the shared workspace so far.", p, {}));
    }

    return tools;
}

QVector<ToolCall> ToolEngine::extractFallbackToolCalls(const QString& rawText, QString* cleanedTextOut) {
    QVector<ToolCall> calls;
    QString cleaned = rawText;

    static const QRegularExpression re(
        R"((?:<tool_call>\s*(\{[\s\S]*?\})\s*</tool_call>|```tool_call\s*(\{[\s\S]*?\})\s*```))",
        QRegularExpression::MultilineOption);

    auto it = re.globalMatch(rawText);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString jsonStr = !m.captured(1).isEmpty() ? m.captured(1) : m.captured(2);
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            const QJsonObject obj = doc.object();
            const QString name = obj["name"].toString();
            if (!name.isEmpty()) {
                ToolCall tc;
                tc.id = "call_" + QUuid::createUuid().toString(QUuid::Id128).left(12);
                tc.name = name;
                const QJsonValue argsVal = obj.contains("arguments") ? obj["arguments"] : obj["parameters"];
                if (argsVal.isObject()) {
                    tc.argumentsJson = QString::fromUtf8(QJsonDocument(argsVal.toObject()).toJson(QJsonDocument::Compact));
                } else if (argsVal.isString()) {
                    tc.argumentsJson = argsVal.toString();
                } else {
                    tc.argumentsJson = "{}";
                }
                calls.append(tc);
                cleaned.remove(m.captured(0));
            }
        }
    }

    if (cleanedTextOut) {
        *cleanedTextOut = cleaned.trimmed();
    }
    return calls;
}

std::optional<QString> ToolEngine::resolveSafeWorkspacePath(const QString& relativePath, QString* errorOut) const {
    if (!m_sandbox || m_sandbox->workspacePath().trimmed().isEmpty()) {
        if (errorOut) *errorOut = "Sandbox workspace path is not configured";
        return std::nullopt;
    }

    QString rel = relativePath.trimmed();
    if (rel.startsWith("/workspace/")) {
        rel = rel.mid(QStringLiteral("/workspace/").size());
    } else if (rel == "/workspace" || rel.isEmpty()) {
        rel = ".";
    }

    if (rel.startsWith('/')) {
        if (errorOut) *errorOut = "Absolute paths outside /workspace are forbidden: " + relativePath;
        return std::nullopt;
    }

    std::error_code ec;
    const std::filesystem::path wsRoot = std::filesystem::canonical(
        std::filesystem::path(m_sandbox->workspacePath().toStdString()), ec);
    if (ec) {
        if (errorOut) *errorOut = "Failed to canonicalize workspace root";
        return std::nullopt;
    }

    const std::filesystem::path relNorm = std::filesystem::path(rel.toStdString()).lexically_normal();
    std::filesystem::path current = wsRoot;

    bool firstComp = true;
    for (const auto& part : relNorm) {
        const std::string pStr = part.string();
        if (pStr.empty() || pStr == ".") continue;
        if (pStr == "..") {
            if (errorOut) *errorOut = "Path traversal ('..') is forbidden: " + relativePath;
            return std::nullopt;
        }
        if (firstComp && pStr == ".git") {
            if (errorOut) *errorOut = "Direct access to .git directory is forbidden: " + relativePath;
            return std::nullopt;
        }
        firstComp = false;

        current /= part;
        const auto st = std::filesystem::symlink_status(current, ec);
        if (!ec && std::filesystem::is_symlink(st)) {
            if (errorOut) *errorOut = "Symlinks inside workspace are forbidden: " + relativePath;
            return std::nullopt;
        }
    }

    const auto rootStr = wsRoot.generic_string();
    const auto candStr = current.generic_string();
    if (candStr != rootStr && !candStr.starts_with(rootStr + "/")) {
        if (errorOut) *errorOut = "Path traversal outside workspace is forbidden: " + relativePath;
        return std::nullopt;
    }

    return QString::fromStdString(candStr);
}

ToolResult ToolEngine::executeToolCall(const ToolCall& call) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(call.argumentsJson.toUtf8(), &err);
    const QJsonObject args = doc.isObject() ? doc.object() : QJsonObject{};

    if (call.name == "list_files") return execListFiles(call.id, args);
    if (call.name == "read_file") return execReadFile(call.id, args);
    if (call.name == "write_file") return execWriteFile(call.id, args);
    if (call.name == "patch_file") return execPatchFile(call.id, args);
    if (call.name == "run_command") return execRunCommand(call.id, args);
    if (call.name == "git_diff") return execGitDiff(call.id);

    ToolResult tr;
    tr.toolCallId = call.id;
    tr.name = call.name;
    tr.isError = true;
    tr.exitCode = 1;
    tr.output = "Unknown tool: " + call.name;
    return tr;
}

ToolResult ToolEngine::execListFiles(const QString& callId, const QJsonObject& args) {
    ToolResult tr{callId, "list_files", {}, false, 0};
    QString err;
    const QString relPath = args["path"].toString(".");
    const auto safePath = resolveSafeWorkspacePath(relPath, &err);
    if (!safePath) {
        tr.isError = true;
        tr.exitCode = 1;
        tr.output = err;
        return tr;
    }

    const QDir rootDir(m_sandbox->workspacePath());
    QDir targetDir(*safePath);
    if (!targetDir.exists()) {
        tr.isError = true;
        tr.exitCode = 1;
        tr.output = "Directory does not exist: " + relPath;
        return tr;
    }

    static const QStringList ignoredPrefixes = {
        ".git/", "node_modules/", "__pycache__/", ".venv/", "venv/", "target/", "build/"
    };

    QStringList entries;
    int totalMatched = 0;
    QDirIterator it(*safePath, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString full = it.next();
        const QFileInfo fi(full);
        if (fi.isSymLink()) continue;

        const QString rel = rootDir.relativeFilePath(full);
        bool skip = (rel == ".git" || rel == "node_modules" || rel == "__pycache__" || rel == ".venv");
        for (const QString& pfx : ignoredPrefixes) {
            if (rel.startsWith(pfx)) {
                skip = true;
                break;
            }
        }
        if (skip) continue;

        totalMatched++;
        if (entries.size() < kMaxListEntries) {
            entries << (fi.isDir()
                            ? (rel + "/")
                            : QStringLiteral("%1 (%2 B)").arg(rel, QString::number(fi.size())));
        }
    }
    entries.sort();
    if (entries.isEmpty()) {
        tr.output = "(workspace is empty)";
    } else {
        tr.output = entries.join('\n');
        if (totalMatched > kMaxListEntries) {
            tr.output += QStringLiteral("\n... (truncated: showing %1 of %2 entries)")
                             .arg(QString::number(kMaxListEntries), QString::number(totalMatched));
        }
    }
    return tr;
}

ToolResult ToolEngine::execReadFile(const QString& callId, const QJsonObject& args) {
    ToolResult tr{callId, "read_file", {}, false, 0};
    QString err;
    const QString relPath = args["path"].toString();
    const auto safePath = resolveSafeWorkspacePath(relPath, &err);
    if (!safePath) {
        tr.isError = true;
        tr.exitCode = 1;
        tr.output = err;
        return tr;
    }

    const int fd = openNoFollowReadFd(*safePath, &err);
    if (fd < 0) {
        tr.isError = true;
        tr.exitCode = 1;
        tr.output = err;
        return tr;
    }

    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        tr.isError = true;
        tr.exitCode = 1;
        tr.output = "Cannot open file for reading: " + relPath;
        return tr;
    }

    const QString content = QString::fromUtf8(file.readAll());
    file.close();

    if (content.isEmpty()) {
        tr.output = "(empty file — 0 lines)";
        return tr;
    }

    const QStringList lines = content.split('\n');
    const int totalLines = lines.size();
    const int startLine = qMax(1, args["start_line"].toInt(1));
    const int endLine = qMin(totalLines, args["end_line"].toInt(250));

    QStringList numbered;
    for (int i = startLine; i <= endLine; ++i) {
        numbered << QStringLiteral("%1: %2").arg(QString::number(i).rightJustified(4, ' '), lines[i - 1]);
    }

    QString header = QStringLiteral("[File: %1 | Showing lines %2-%3 of %4 total]\n")
                         .arg(relPath,
                              QString::number(startLine),
                              QString::number(endLine),
                              QString::number(totalLines));
    tr.output = header + numbered.join('\n');
    if (endLine < totalLines) {
        tr.output += QStringLiteral("\n... (%1 more lines; call read_file with start_line=%2 to continue)")
                         .arg(QString::number(totalLines - endLine), QString::number(endLine + 1));
    }
    return tr;
}

ToolResult ToolEngine::execWriteFile(const QString& callId, const QJsonObject& args) {
    ToolResult tr{callId, "write_file", {}, false, 0};
    QString err;
    if (!m_sandbox->ensureWorkspaceInitialized(&err)) {
        tr.isError = true;
        tr.exitCode = 1;
        tr.output = err;
        return tr;
    }

    const QString relPath = args["path"].toString();
    const auto safePath = resolveSafeWorkspacePath(relPath, &err);
    if (!safePath) {
        tr.isError = true;
        tr.exitCode = 1;
        tr.output = err;
        return tr;
    }

    const QFileInfo fi(*safePath);
    if (!fi.dir().exists() && !QDir().mkpath(fi.dir().absolutePath())) {
        tr.isError = true;
        tr.exitCode = 1;
        tr.output = "Failed to create parent directory for: " + relPath;
        return tr;
    }

    // Re-verify path after mkpath to ensure no symlink was introduced
    if (!resolveSafeWorkspacePath(relPath, &err)) {
        tr.isError = true;
        tr.exitCode = 1;
        tr.output = err;
        return tr;
    }

    const int fd = openNoFollowWriteFd(*safePath, &err);
    if (fd < 0) {
        tr.isError = true;
        tr.exitCode = 1;
        tr.output = err;
        return tr;
    }

    QFile file;
    if (!file.open(fd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        tr.isError = true;
        tr.exitCode = 1;
        tr.output = "Failed to write file: " + relPath;
        return tr;
    }

    const QByteArray bytes = args["content"].toString().toUtf8();
    file.write(bytes);
    file.close();

    emit workspaceModified(relPath);
    tr.output = QStringLiteral("Wrote %1 bytes to %2").arg(QString::number(bytes.size()), relPath);
    return tr;
}

ToolResult ToolEngine::execPatchFile(const QString& callId, const QJsonObject& args) {
    ToolResult tr{callId, "patch_file", {}, false, 0};
    QString err;
    const QString relPath = args["path"].toString();
    const QString search = args["search"].toString();
    const QString replace = args["replace"].toString();

    if (search.isEmpty()) {
        tr.isError = true;
        tr.exitCode = 1;
        tr.output = "patch_file requires a non-empty 'search' string";
        return tr;
    }

    const auto safePath = resolveSafeWorkspacePath(relPath, &err);
    if (!safePath) {
        tr.isError = true;
        tr.exitCode = 1;
        tr.output = err;
        return tr;
    }

    const int rfd = openNoFollowReadFd(*safePath, &err);
    if (rfd < 0) {
        tr.isError = true;
        tr.exitCode = 1;
        tr.output = err;
        return tr;
    }

    QFile rfile;
    if (!rfile.open(rfd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(rfd);
        tr.isError = true;
        tr.exitCode = 1;
        tr.output = "Cannot open file for patching: " + relPath;
        return tr;
    }

    QString content = QString::fromUtf8(rfile.readAll());
    rfile.close();

    const int matchCount = content.count(search);
    if (matchCount == 0) {
        tr.isError = true;
        tr.exitCode = 1;
        tr.output = "Target 'search' block was not found in " + relPath;
        return tr;
    }
    if (matchCount > 1) {
        tr.isError = true;
        tr.exitCode = 1;
        tr.output = QStringLiteral("Ambiguous patch: 'search' block matched %1 times in %2; include more surrounding context.")
                        .arg(QString::number(matchCount), relPath);
        return tr;
    }

    const int idx = content.indexOf(search);
    content.replace(idx, search.length(), replace);

    const int wfd = openNoFollowWriteFd(*safePath, &err);
    if (wfd < 0) {
        tr.isError = true;
        tr.exitCode = 1;
        tr.output = err;
        return tr;
    }

    QFile wfile;
    if (!wfile.open(wfd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
        ::close(wfd);
        tr.isError = true;
        tr.exitCode = 1;
        tr.output = "Failed to save patched file: " + relPath;
        return tr;
    }
    wfile.write(content.toUtf8());
    wfile.close();

    emit workspaceModified(relPath);
    tr.output = QStringLiteral("Successfully patched %1").arg(relPath);
    return tr;
}

ToolResult ToolEngine::execRunCommand(const QString& callId, const QJsonObject& args) {
    ToolResult tr{callId, "run_command", {}, false, 0};
    const QString cmd = args["command"].toString().trimmed();
    const int timeoutSec = qBound(1, args["timeout_sec"].toInt(30), 300);

    if (cmd.isEmpty()) {
        tr.isError = true;
        tr.exitCode = 1;
        tr.output = "Command is empty";
        return tr;
    }

    const SandboxRunResult runRes = m_sandbox->runCommand(cmd, timeoutSec * 1000);
    tr.exitCode = runRes.exitCode;
    tr.isError = (runRes.exitCode != 0);
    tr.output = runRes.combinedOutput().trimmed();
    if (tr.output.isEmpty()) {
        tr.output = QStringLiteral("(command exited with code %1 and no output)").arg(QString::number(runRes.exitCode));
    }
    emit workspaceModified(".");
    return tr;
}

ToolResult ToolEngine::execGitDiff(const QString& callId) {
    ToolResult tr{callId, "git_diff", {}, false, 0};
    const QString diff = m_sandbox->gitDiff().trimmed();
    const QString status = m_sandbox->gitStatusShort().trimmed();
    if (diff.isEmpty() && status.isEmpty()) {
        tr.output = "(no uncommitted changes in workspace)";
    } else {
        tr.output = (status.isEmpty() ? QString() : ("Status:\n" + status + "\n\n")) + diff;
    }
    return tr;
}

} // namespace agora
