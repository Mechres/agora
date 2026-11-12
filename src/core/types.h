#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace agora {

enum class DebateMode {
    RoundRobinWithMentions = 0,
    MentionsOnly = 1,
    SingleTurn = 2
};

struct ModelInfo {
    QString id;
    QString path;
    QString status; // "loaded", "sleeping", "loading", "unloaded"
    double sizeGiB = 0.0;
    bool failed = false;
};

struct Participant {
    QString id;
    QString handle;       // e.g. "@architect", "@coder", "@reviewer"
    QString displayName;  // e.g. "System Architect"
    QString modelId;      // e.g. "Qwopus3.5-9B-coder-Exp-Q4_K_M"
    QString systemPrompt;
    QString colorHex = "#60a5fa";
    double temperature = 0.7;
    bool enabled = true;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["id"] = id;
        obj["handle"] = handle;
        obj["displayName"] = displayName;
        obj["modelId"] = modelId;
        obj["systemPrompt"] = systemPrompt;
        obj["colorHex"] = colorHex;
        obj["temperature"] = temperature;
        obj["enabled"] = enabled;
        return obj;
    }

    static Participant fromJson(const QJsonObject& obj) {
        Participant p;
        p.id = obj["id"].toString();
        p.handle = obj["handle"].toString();
        p.displayName = obj["displayName"].toString();
        p.modelId = obj["modelId"].toString();
        p.systemPrompt = obj["systemPrompt"].toString();
        p.colorHex = obj["colorHex"].toString("#60a5fa");
        p.temperature = obj["temperature"].toDouble(0.7);
        p.enabled = obj["enabled"].toBool(true);
        return p;
    }
};

struct ToolCall {
    QString id;
    QString name;
    QString argumentsJson;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["id"] = id;
        obj["name"] = name;
        obj["argumentsJson"] = argumentsJson;
        return obj;
    }

    static ToolCall fromJson(const QJsonObject& obj) {
        ToolCall tc;
        tc.id = obj["id"].toString();
        tc.name = obj["name"].toString();
        tc.argumentsJson = obj["argumentsJson"].toString();
        return tc;
    }
};

struct ToolResult {
    QString toolCallId;
    QString name;
    QString output;
    bool isError = false;
    int exitCode = 0;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["toolCallId"] = toolCallId;
        obj["name"] = name;
        obj["output"] = output;
        obj["isError"] = isError;
        obj["exitCode"] = exitCode;
        return obj;
    }

    static ToolResult fromJson(const QJsonObject& obj) {
        ToolResult tr;
        tr.toolCallId = obj["toolCallId"].toString();
        tr.name = obj["name"].toString();
        tr.output = obj["output"].toString();
        tr.isError = obj["isError"].toBool(false);
        tr.exitCode = obj["exitCode"].toInt(0);
        return tr;
    }
};

// Per-reply numbers reported by llama-server (usage/timings) plus our own model-prep time
struct TurnStats {
    int promptTokens = 0;
    int completionTokens = 0;
    double promptMs = 0.0;
    double generationMs = 0.0;
    double tokensPerSecond = 0.0;
    qint64 modelPrepMs = 0;   // VRAM swap + load before the request

    bool isEmpty() const { return promptTokens == 0 && completionTokens == 0 && modelPrepMs == 0; }

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["promptTokens"] = promptTokens;
        obj["completionTokens"] = completionTokens;
        obj["promptMs"] = promptMs;
        obj["generationMs"] = generationMs;
        obj["tokensPerSecond"] = tokensPerSecond;
        obj["modelPrepMs"] = modelPrepMs;
        return obj;
    }

    static TurnStats fromJson(const QJsonObject& obj) {
        TurnStats st;
        st.promptTokens = obj["promptTokens"].toInt();
        st.completionTokens = obj["completionTokens"].toInt();
        st.promptMs = obj["promptMs"].toDouble();
        st.generationMs = obj["generationMs"].toDouble();
        st.tokensPerSecond = obj["tokensPerSecond"].toDouble();
        st.modelPrepMs = obj["modelPrepMs"].toInteger();
        return st;
    }
};

struct ChatMessage {
    QString id;
    QString senderId;     // "user", "system", or participant.id
    QString senderHandle; // "@you", "@system", "@coder", etc.
    QString displayName;
    QString modelId;
    QString colorHex = "#94a3b8";
    QString content;
    QString reasoningContent;
    QVector<ToolCall> toolCalls;
    QVector<ToolResult> toolResults;
    QStringList mentions;
    QString timestamp;
    TurnStats stats;
    // Workspace checkpoints (git commits under refs/agora/<session>) bracketing the turn
    // that ended with this message; empty when the turn changed no files
    QString checkpointCommit;
    QString checkpointParent;

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["id"] = id;
        obj["senderId"] = senderId;
        obj["senderHandle"] = senderHandle;
        obj["displayName"] = displayName;
        obj["modelId"] = modelId;
        obj["colorHex"] = colorHex;
        obj["content"] = content;
        obj["reasoningContent"] = reasoningContent;
        obj["timestamp"] = timestamp;
        if (!stats.isEmpty()) obj["stats"] = stats.toJson();
        if (!checkpointCommit.isEmpty()) {
            obj["checkpointCommit"] = checkpointCommit;
            obj["checkpointParent"] = checkpointParent;
        }

        QJsonArray tcArr;
        for (const auto& tc : toolCalls) tcArr.append(tc.toJson());
        obj["toolCalls"] = tcArr;

        QJsonArray trArr;
        for (const auto& tr : toolResults) trArr.append(tr.toJson());
        obj["toolResults"] = trArr;

        QJsonArray mArr;
        for (const auto& m : mentions) mArr.append(m);
        obj["mentions"] = mArr;
        return obj;
    }

    static ChatMessage fromJson(const QJsonObject& obj) {
        ChatMessage m;
        m.id = obj["id"].toString();
        m.senderId = obj["senderId"].toString();
        m.senderHandle = obj["senderHandle"].toString();
        m.displayName = obj["displayName"].toString();
        m.modelId = obj["modelId"].toString();
        m.colorHex = obj["colorHex"].toString("#94a3b8");
        m.content = obj["content"].toString();
        m.reasoningContent = obj["reasoningContent"].toString();
        m.timestamp = obj["timestamp"].toString();
        m.stats = TurnStats::fromJson(obj["stats"].toObject());
        m.checkpointCommit = obj["checkpointCommit"].toString();
        m.checkpointParent = obj["checkpointParent"].toString();

        for (const auto& v : obj["toolCalls"].toArray()) {
            m.toolCalls.append(ToolCall::fromJson(v.toObject()));
        }
        for (const auto& v : obj["toolResults"].toArray()) {
            m.toolResults.append(ToolResult::fromJson(v.toObject()));
        }
        for (const auto& v : obj["mentions"].toArray()) {
            m.mentions.append(v.toString());
        }
        return m;
    }
};

struct RoomSession {
    QString id;
    QString title;
    QString workspacePath;
    DebateMode mode = DebateMode::RoundRobinWithMentions;
    int maxRounds = 8;
    double vramBudgetGiB = 8.5;
    bool allowSandboxNetwork = false;
    QVector<Participant> participants;
    QVector<ChatMessage> messages;
    QString updatedAt;
};

struct RolePreset {
    QString handle;
    QString displayName;
    QString colorHex;
    double temperature;
    QString systemPrompt;
};

inline QVector<RolePreset> defaultRolePresets() {
    return {
        {
            "@architect",
            "System Architect",
            "#38bdf8",
            0.6,
            "You are @architect, the System Architect in a collaborative multi-model engineering room. "
            "Break down problems cleanly, design modular files and interfaces in the shared sandbox workspace, "
            "and coordinate with @coder and @reviewer using @mentions. Prefer inspecting the workspace first with list_files/read_file."
        },
        {
            "@coder",
            "Lead Coder",
            "#4ade80",
            0.4,
            "You are @coder, the Lead Implementation Engineer. Write clean, complete, well-tested code in the shared "
            "workspace using write_file and patch_file, then run_command to compile/test your work in the sandbox. "
            "Hand off to @reviewer or @architect by mentioning their @handle when ready."
        },
        {
            "@reviewer",
            "Security & Test Reviewer",
            "#f472b6",
            0.5,
            "You are @reviewer, the Code Reviewer, QA & Security Auditor. Inspect changes via git_diff and read_file, "
            "write and execute edge-case tests using run_command, and point out bugs or concrete improvements to @coder or @architect."
        },
        {
            "@moderator",
            "Moderator & Judge",
            "#fbbf24",
            0.5,
            "You are @moderator. Synthesize the arguments and code produced by other participants, resolve disagreements, "
            "verify that all user requirements pass in the sandbox, and direct the next speaker via @handle."
        }
    };
}

} // namespace agora
