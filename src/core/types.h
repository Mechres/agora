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

// An external OpenAI-compatible chat API (OpenAI, Anthropic, Gemini, OpenRouter, Ollama, ...).
// Participants with an empty providerId use the local llama-server router instead.
struct ProviderConfig {
    QString id;
    QString name;           // e.g. "OpenAI"
    QString baseUrl;        // up to and including the version, e.g. "https://api.openai.com/v1"
    QString apiKey;         // the key itself, or "$ENV_VAR" to read it from the environment
    int contextTokens = 32000; // history sent per request; caps cost as well as fitting the window
    QStringList models;     // last fetched from GET {baseUrl}/models

    // "$NAME" -> value of environment variable NAME; anything else is the key itself
    QString resolvedApiKey() const {
        const QString key = apiKey.trimmed();
        if (key.startsWith('$') && key.size() > 1) {
            return qEnvironmentVariable(key.mid(1).toUtf8().constData()).trimmed();
        }
        return key;
    }

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["id"] = id;
        obj["name"] = name;
        obj["baseUrl"] = baseUrl;
        obj["apiKey"] = apiKey;
        obj["contextTokens"] = contextTokens;
        obj["models"] = QJsonArray::fromStringList(models);
        return obj;
    }

    static ProviderConfig fromJson(const QJsonObject& obj) {
        ProviderConfig p;
        p.id = obj["id"].toString();
        p.name = obj["name"].toString();
        p.baseUrl = obj["baseUrl"].toString();
        p.apiKey = obj["apiKey"].toString();
        p.contextTokens = obj["contextTokens"].toInt(32000);
        for (const auto& v : obj["models"].toArray()) p.models.append(v.toString());
        return p;
    }
};

struct ProviderPreset {
    QString name;
    QString baseUrl;
    QString apiKeyHint; // suggested environment variable
};

inline QVector<ProviderPreset> defaultProviderPresets() {
    return {
        {"OpenAI", "https://api.openai.com/v1", "$OPENAI_API_KEY"},
        {"Anthropic", "https://api.anthropic.com/v1", "$ANTHROPIC_API_KEY"},
        {"Google Gemini", "https://generativelanguage.googleapis.com/v1beta/openai", "$GEMINI_API_KEY"},
        {"OpenRouter", "https://openrouter.ai/api/v1", "$OPENROUTER_API_KEY"},
        {"Groq", "https://api.groq.com/openai/v1", "$GROQ_API_KEY"},
        {"DeepSeek", "https://api.deepseek.com/v1", "$DEEPSEEK_API_KEY"},
        {"Mistral", "https://api.mistral.ai/v1", "$MISTRAL_API_KEY"},
        {"Ollama (local)", "http://127.0.0.1:11434/v1", ""},
        {"LM Studio (local)", "http://127.0.0.1:1234/v1", ""},
    };
}

struct Participant {
    QString id;
    QString handle;       // e.g. "@architect", "@coder", "@reviewer"
    QString displayName;  // e.g. "System Architect"
    QString modelId;      // e.g. "Qwopus3.5-9B-coder-Exp-Q4_K_M"
    QString systemPrompt;
    QString colorHex = "#60a5fa";
    double temperature = 0.7;
    bool enabled = true;
    QString providerId;   // external ProviderConfig id; empty = local llama-server router

    QJsonObject toJson() const {
        QJsonObject obj;
        obj["id"] = id;
        if (!providerId.isEmpty()) obj["providerId"] = providerId;
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
        p.providerId = obj["providerId"].toString();
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
        },
        {
            "@tester",
            "Test Engineer",
            "#a3e635",
            0.4,
            "You are @tester, the Test Engineer. Turn the requirements into concrete test cases, including edge cases "
            "and failure paths. Write the tests into the workspace, run them with run_command, and report exactly "
            "which pass and which fail with the relevant output. Never mark something tested without running it."
        },
        {
            "@security",
            "Security Auditor",
            "#f87171",
            0.3,
            "You are @security, the Security Auditor. Read the code with read_file and git_diff looking for injection, "
            "path traversal, unsafe deserialization, secrets in code, missing input validation and unsafe defaults. "
            "Rank findings by severity, show the exact lines, and propose a minimal fix. Write a failing test that "
            "demonstrates a vulnerability where you can."
        },
        {
            "@debugger",
            "Debugger",
            "#fb923c",
            0.3,
            "You are @debugger. Reproduce the reported problem in the sandbox first and show the failing output. "
            "Form one hypothesis at a time, test it with a command or a small script, and narrow down to the root "
            "cause before proposing a fix. Explain the cause in one or two sentences, then hand the fix to @coder "
            "or apply it with patch_file and re-run the reproduction."
        },
        {
            "@perf",
            "Performance Engineer",
            "#22d3ee",
            0.4,
            "You are @perf, the Performance Engineer. Measure before optimizing: write a small benchmark, run it, "
            "and report numbers. Identify the hot path and algorithmic complexity, propose changes with the expected "
            "gain, and re-measure after each change. Reject optimizations that do not show a measurable improvement."
        },
        {
            "@devops",
            "DevOps & Build Engineer",
            "#94a3b8",
            0.4,
            "You are @devops, the Build and DevOps Engineer. Own the build files, dependency setup, scripts, "
            "Dockerfiles and CI configuration in the workspace. Make the project build and test with one command, "
            "keep configuration minimal and reproducible, and verify every change by running it in the sandbox."
        },
        {
            "@docs",
            "Technical Writer",
            "#e879f9",
            0.6,
            "You are @docs, the Technical Writer. Read the code and write clear README files, usage examples, "
            "API references and code comments where they help. Every example you write must match the actual code; "
            "run examples in the sandbox when possible. Prefer short, concrete text over long explanations."
        },
        {
            "@product",
            "Product Manager",
            "#facc15",
            0.7,
            "You are @product, the Product Manager. Clarify what the user actually needs: restate the goal, list "
            "user stories and acceptance criteria, and cut scope to what matters most. Keep the team focused on the "
            "user's requirements and check finished work against the acceptance criteria."
        },
        {
            "@ux",
            "UX Designer",
            "#c084fc",
            0.7,
            "You are @ux, the UX and Interface Designer. Think about how a person will use the result: flows, "
            "error messages, defaults, accessibility and command-line or UI ergonomics. Propose concrete changes "
            "with examples of the exact text or layout, and review interfaces for confusing or inconsistent behavior."
        },
        {
            "@skeptic",
            "Devil's Advocate",
            "#ef4444",
            0.8,
            "You are @skeptic, the Devil's Advocate. Challenge the current plan or conclusion: look for hidden "
            "assumptions, missing cases, simpler alternatives and ways it could fail. Be specific and constructive; "
            "every objection must say what evidence or test would resolve it. Concede clearly when an argument holds."
        },
        {
            "@researcher",
            "Researcher & Analyst",
            "#2dd4bf",
            0.6,
            "You are @researcher, the Researcher and Analyst. Investigate before others build: explore the existing "
            "workspace with list_files and read_file, compare possible approaches with their trade-offs, and run "
            "small experiments in the sandbox to check facts. Summarize findings with evidence and a recommendation."
        }
    };
}

// A ready-made team: the handles of its role presets, in speaking order
struct CouncilTemplate {
    QString name;
    QString description;
    QStringList handles;
};

inline QVector<CouncilTemplate> defaultCouncilTemplates() {
    return {
        {"Coding Council", "Plan, implement, review", {"@architect", "@coder", "@reviewer"}},
        {"Full Dev Team", "Requirements to tested code", {"@product", "@architect", "@coder", "@tester", "@reviewer"}},
        {"Debug Squad", "Reproduce, find the root cause, fix", {"@debugger", "@coder", "@tester"}},
        {"Security Review", "Audit and harden existing code", {"@security", "@reviewer", "@coder"}},
        {"Performance Pass", "Measure and optimize", {"@perf", "@coder", "@tester"}},
        {"Design Debate", "Argue a design out before building", {"@architect", "@skeptic", "@researcher", "@moderator"}},
        {"Product & Docs", "Spec, UX and documentation", {"@product", "@ux", "@docs"}},
    };
}

} // namespace agora
