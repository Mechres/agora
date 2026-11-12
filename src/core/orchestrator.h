#pragma once

#include "core/router_client.h"
#include "core/tools.h"
#include "core/types.h"

#include <QJsonArray>
#include <QObject>
#include <optional>

namespace agora {

class Orchestrator : public QObject {
    Q_OBJECT
public:
    static constexpr int kDefaultMaxContextChars = 48000;

    explicit Orchestrator(LlamaRouterClient* router,
                          ToolEngine* toolEngine,
                          QObject* parent = nullptr);

    RoomSession& session() { return m_session; }
    const RoomSession& session() const { return m_session; }
    void setSession(const RoomSession& s);

    bool isRunning() const { return m_running; }
    QString activeSpeakerId() const { return m_activeSpeakerId; }

    static QStringList extractMentions(const QString& text,
                                       const QVector<Participant>& participants,
                                       bool handoffOnly = false);
    static std::optional<Participant> selectNextSpeaker(const QVector<Participant>& participants,
                                                        const QVector<ChatMessage>& history,
                                                        DebateMode mode,
                                                        int* roundRobinIndexInOut);
    static int contextCharBudget(int contextTokens);
    static QJsonArray buildMessagesForSpeaker(const Participant& speaker,
                                              const QVector<Participant>& allParticipants,
                                              const QVector<ChatMessage>& history,
                                              const QString& workspacePath = QStringLiteral("/workspace"),
                                              int maxContextChars = kDefaultMaxContextChars);

    void addUserMessage(const QString& text);
    void startDebate();
    void stepSingleTurn();
    void stopDebate();

signals:
    void debateStateChanged(bool running, int currentRound, int maxRounds);
    void activeSpeakerChanged(const QString& participantId, const QString& handle, const QString& modelId);
    void messageAdded(int messageIndex, const ChatMessage& message);
    void messageUpdated(int messageIndex, const ChatMessage& message);
    void statusNotification(const QString& statusText);
    void errorNotification(const QString& errorText);

private:
    void scheduleNextTurn();
    void launchSpeakerTurn(const Participant& speaker, int messageIndex, int toolDepth);
    void onModelStreamFinished(const QString& content,
                               const QString& reasoning,
                               const QVector<ToolCall>& toolCalls);

    LlamaRouterClient* m_router = nullptr;
    ToolEngine* m_toolEngine = nullptr;
    RoomSession m_session;

    bool m_running = false;
    bool m_singleStepMode = false;
    int m_completedTurns = 0;
    int m_roundRobinCursor = 0;
    int m_currentToolDepth = 0;
    int m_maxToolDepth = 5;
    int m_activeMessageIndex = -1;
    quint64 m_generationId = 0;
    QString m_activeSpeakerId;
};

} // namespace agora
