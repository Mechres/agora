#pragma once

#include "core/checkpoints.h"
#include "core/router_client.h"
#include "core/tools.h"
#include "core/types.h"

#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QSet>
#include <functional>
#include <optional>

namespace agora {

enum class ToolApprovalPolicy {
    Off = 0,          // run every tool call immediately
    AskForWrites = 1, // ask before write_file, patch_file and run_command
    AskForAll = 2     // ask before every tool call
};

enum class ToolApprovalDecision {
    AllowOnce,
    AllowToolForSession,
    Deny
};

// Drives a debate as an asynchronous state machine: every step (checkpoint, model swap,
// stream, tool call, approval) continues from a callback, so nothing blocks or spins a
// nested event loop. Callbacks from an aborted run are discarded via m_generationId.
class Orchestrator : public QObject {
    Q_OBJECT
public:
    static constexpr int kDefaultMaxContextChars = 48000;
    static constexpr double kDefaultCharsPerToken = 3.0;
    static constexpr int kDefaultMaxToolDepth = 8;
    static constexpr int kDefaultTransientRetryDelayMs = 5000;

    explicit Orchestrator(LlamaRouterClient* router,
                          ToolEngine* toolEngine,
                          QObject* parent = nullptr);

    RoomSession& session() { return m_session; }
    const RoomSession& session() const { return m_session; }
    void setSession(const RoomSession& s);

    bool isRunning() const { return m_running; }
    QString activeSpeakerId() const { return m_activeSpeakerId; }

    // Optional; without it (or with auto-checkpoints off) no snapshots are taken
    void setCheckpointStore(CheckpointStore* store) { m_checkpoints = store; }
    void setAutoCheckpoints(bool enabled) { m_autoCheckpoints = enabled; }
    bool autoCheckpoints() const { return m_autoCheckpoints; }

    void setMaxToolDepth(int depth) { m_maxToolDepth = qBound(1, depth, 100); }
    int maxToolDepth() const { return m_maxToolDepth; }

    void setTransientRetryDelayMs(int ms) { m_transientRetryDelayMs = qMax(0, ms); }

    void setToolApprovalPolicy(ToolApprovalPolicy policy) { m_approvalPolicy = policy; }
    ToolApprovalPolicy toolApprovalPolicy() const { return m_approvalPolicy; }

    // External OpenAI-compatible APIs that participants can use instead of the local router
    void setProviders(const QVector<ProviderConfig>& providers) { m_providers = providers; }
    const QVector<ProviderConfig>& providers() const { return m_providers; }
    // nullptr for local participants and for unknown provider ids
    const ProviderConfig* providerFor(const Participant& p) const;

    // Learned from usage.prompt_tokens; kDefaultCharsPerToken until the model has replied once.
    // Keyed by modelKey(): the model id, prefixed with the provider id for external models.
    double charsPerTokenFor(const QString& modelKey) const;
    static QString modelKey(const Participant& p);

    static QStringList extractMentions(const QString& text,
                                       const QVector<Participant>& participants,
                                       bool handoffOnly = false);
    static std::optional<Participant> selectNextSpeaker(const QVector<Participant>& participants,
                                                        const QVector<ChatMessage>& history,
                                                        DebateMode mode,
                                                        int* roundRobinIndexInOut);
    static int contextCharBudget(int contextTokens, double charsPerToken = kDefaultCharsPerToken);
    static QJsonArray buildMessagesForSpeaker(const Participant& speaker,
                                              const QVector<Participant>& allParticipants,
                                              const QVector<ChatMessage>& history,
                                              const QString& workspacePath = QStringLiteral("/workspace"),
                                              int maxContextChars = kDefaultMaxContextChars);
    static bool declaresDone(const QString& content);
    // Enabled participants with no turn since the user's latest message, in roster order
    static QStringList participantsYetToSpeak(const QVector<Participant>& participants,
                                              const QVector<ChatMessage>& history);
    static bool needsApproval(ToolApprovalPolicy policy,
                              const QString& toolName,
                              const QSet<QString>& approvedForSession);
    static bool isContextOverflowError(const QString& errorText);
    static bool isTemperatureRejectedError(const QString& errorText);
    // Rate limits and overload: worth one retry after a pause
    static bool isTransientError(const QString& errorText);

    void addUserMessage(const QString& text);
    void startDebate();
    void stepSingleTurn();
    void stopDebate();

    void resolveToolApproval(quint64 requestId, ToolApprovalDecision decision, const QString& reason = {});
    // Undo the turn that ended at messageIndex: restore the checkpoint taken before it
    void restoreCheckpointBefore(int messageIndex);

signals:
    void debateStateChanged(bool running, int currentRound, int maxRounds);
    void activeSpeakerChanged(const QString& participantId, const QString& handle, const QString& modelId);
    void messageAdded(int messageIndex, const ChatMessage& message);
    void messageUpdated(int messageIndex, const ChatMessage& message);
    void statusNotification(const QString& statusText);
    void errorNotification(const QString& errorText);
    void toolApprovalRequested(quint64 requestId, const QString& speakerHandle, const ToolCall& call);
    void toolApprovalCancelled(quint64 requestId);
    void workspaceChanged();

private:
    bool alive(quint64 genId) const { return m_running && genId == m_generationId; }
    void abortInFlight();
    void endRun(const QString& message, bool isError = false);
    void dropEmptyPlaceholder();
    int appendPendingMessage(const Participant& speaker);

    void scheduleNextTurn();
    void beginTurn(const Participant& speaker);
    void startStream(int toolDepth);
    void onModelStreamFinished(const QString& content,
                               const QString& reasoning,
                               const QVector<ToolCall>& toolCalls);
    void onModelStreamError(const QString& error);
    void clearPartialReply();
    void runToolCall(int messageIndex, const QVector<ToolCall>& calls, int callIndex, quint64 genId);
    void finalizeTurn(int messageIndex, quint64 genId);
    void appendSystemNote(const QString& text);

    void requestApproval(const QString& speakerHandle, const ToolCall& call,
                         std::function<void(ToolApprovalDecision, const QString&)> onDecision);
    void cancelPendingApproval();

    LlamaRouterClient* m_router = nullptr;
    ToolEngine* m_toolEngine = nullptr;
    CheckpointStore* m_checkpoints = nullptr;
    RoomSession m_session;
    QVector<ProviderConfig> m_providers;

    // Settings
    bool m_autoCheckpoints = true;
    int m_maxToolDepth = kDefaultMaxToolDepth;
    int m_transientRetryDelayMs = kDefaultTransientRetryDelayMs;
    ToolApprovalPolicy m_approvalPolicy = ToolApprovalPolicy::Off;

    // Run state
    bool m_running = false;
    bool m_singleStepMode = false;
    int m_completedTurns = 0;
    int m_roundRobinCursor = 0;
    int m_activeMessageIndex = -1;
    quint64 m_generationId = 0;
    QString m_activeSpeakerId;

    // Current turn
    Participant m_turnSpeaker;
    QString m_turnBaseCommit;
    qint64 m_turnPrepMs = 0;
    int m_currentToolDepth = 0;
    int m_lastSentChars = 0;
    int m_lastBudget = 0;
    int m_budgetOverride = 0;
    bool m_contextRetryUsed = false;
    bool m_transientRetryUsed = false;

    QHash<QString, double> m_charsPerToken;
    QSet<QString> m_omitTemperature; // model keys whose API rejected our temperature

    // Tool approval
    QSet<QString> m_sessionApprovedTools;
    quint64 m_nextApprovalId = 0;
    quint64 m_pendingApprovalId = 0;
    std::function<void(ToolApprovalDecision, const QString&)> m_pendingApproval;
};

} // namespace agora
