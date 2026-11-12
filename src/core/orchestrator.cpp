#include "core/orchestrator.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>

namespace agora {

Orchestrator::Orchestrator(LlamaRouterClient* router, ToolEngine* toolEngine, QObject* parent)
    : QObject(parent), m_router(router), m_toolEngine(toolEngine) {
    m_session.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_session.title = "New Agora Debate";

    QObject::connect(m_router, &LlamaRouterClient::streamReasoningDelta, this, [this](const QString& delta) {
        if (m_activeMessageIndex >= 0 && m_activeMessageIndex < m_session.messages.size()) {
            ChatMessage& msg = m_session.messages[m_activeMessageIndex];
            msg.reasoningContent += delta;
            emit messageUpdated(m_activeMessageIndex, msg);
        }
    });

    QObject::connect(m_router, &LlamaRouterClient::streamContentDelta, this, [this](const QString& delta) {
        if (m_activeMessageIndex >= 0 && m_activeMessageIndex < m_session.messages.size()) {
            ChatMessage& msg = m_session.messages[m_activeMessageIndex];
            msg.content += delta;
            emit messageUpdated(m_activeMessageIndex, msg);
        }
    });

    QObject::connect(m_router, &LlamaRouterClient::streamFinished, this, &Orchestrator::onModelStreamFinished);

    QObject::connect(m_router, &LlamaRouterClient::streamError, this, [this](const QString& err) {
        m_running = false;
        m_activeSpeakerId.clear();
        emit activeSpeakerChanged({}, {}, {});
        emit debateStateChanged(false, m_completedTurns, m_session.maxRounds);
        emit errorNotification("Model stream error: " + err);
    });
}

void Orchestrator::setSession(const RoomSession& s) {
    ++m_generationId;
    m_router->abortActiveStream();
    if (m_toolEngine && m_toolEngine->sandbox()) {
        m_toolEngine->sandbox()->cancelRunningCommand();
    }
    m_running = false;
    m_activeSpeakerId.clear();
    m_activeMessageIndex = -1;
    m_session = s;
    m_completedTurns = 0;
    m_roundRobinCursor = 0;
    emit activeSpeakerChanged({}, {}, {});
    emit debateStateChanged(false, m_completedTurns, m_session.maxRounds);
}

QStringList Orchestrator::extractMentions(const QString& text,
                                          const QVector<Participant>& participants,
                                          bool handoffOnly) {
    QStringList found;
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return found;

    auto matchParticipantHandle = [&](const QString& token) -> QString {
        const QString lower = token.toLower();
        for (const auto& p : participants) {
            if (!p.enabled) continue;
            const QString h = p.handle.startsWith('@') ? p.handle.toLower() : ("@" + p.handle.toLower());
            if (lower == h) return p.handle;
        }
        return {};
    };

    if (handoffOnly && trimmed.size() > 220) {
        // 1. Check explicit handoff arrows anywhere in the message: "-> @coder" or "→ @coder"
        static const QRegularExpression arrowRe(R"((?:->|→|hand\s*off\s+to|over\s+to)\s*(@[A-Za-z0-9_\-]+))",
                                                QRegularExpression::CaseInsensitiveOption);
        auto ait = arrowRe.globalMatch(trimmed);
        while (ait.hasNext()) {
            const QString h = matchParticipantHandle(ait.next().captured(1));
            if (!h.isEmpty() && !found.contains(h)) found.append(h);
        }
        if (!found.isEmpty()) return found;

        // 2. Otherwise only inspect the tail (last 220 chars) so mid-message remarks like
        //    "I disagree with @coder on this point..." don't prematurely pass the floor.
        const QString tail = trimmed.right(220);
        static const QRegularExpression mentionRe(R"((@[A-Za-z0-9_\-]+))");
        auto it = mentionRe.globalMatch(tail);
        while (it.hasNext()) {
            const QString h = matchParticipantHandle(it.next().captured(1));
            if (!h.isEmpty() && !found.contains(h)) found.append(h);
        }
        return found;
    }

    static const QRegularExpression re(R"((@[A-Za-z0-9_\-]+))");
    auto it = re.globalMatch(trimmed);
    while (it.hasNext()) {
        const QString h = matchParticipantHandle(it.next().captured(1));
        if (!h.isEmpty() && !found.contains(h)) {
            found.append(h);
        }
    }
    return found;
}

std::optional<Participant> Orchestrator::selectNextSpeaker(const QVector<Participant>& participants,
                                                           const QVector<ChatMessage>& history,
                                                           DebateMode mode,
                                                           int* roundRobinIndexInOut) {
    QVector<Participant> enabledList;
    for (const auto& p : participants) {
        if (p.enabled) enabledList.append(p);
    }
    if (enabledList.isEmpty()) {
        return std::nullopt;
    }

    if (!history.isEmpty()) {
        const ChatMessage& lastMsg = history.last();
        const bool isModelTurn = (lastMsg.senderId != "user" && lastMsg.senderId != "system");
        const QStringList mentions = !lastMsg.mentions.isEmpty()
                                         ? lastMsg.mentions
                                         : extractMentions(lastMsg.content, enabledList, /*handoffOnly=*/isModelTurn);
        for (const QString& mHandle : mentions) {
            for (int i = 0; i < enabledList.size(); ++i) {
                const auto& cand = enabledList[i];
                if (cand.handle.compare(mHandle, Qt::CaseInsensitive) == 0 &&
                    cand.id != lastMsg.senderId) {
                    if (roundRobinIndexInOut) {
                        *roundRobinIndexInOut = (i + 1) % enabledList.size();
                    }
                    return cand;
                }
            }
        }
    }

    if (mode == DebateMode::MentionsOnly && !history.isEmpty() && history.last().senderId != "user") {
        return std::nullopt;
    }

    int idx = roundRobinIndexInOut ? (*roundRobinIndexInOut % enabledList.size()) : 0;
    if (idx < 0) idx = 0;

    if (!history.isEmpty() && enabledList.size() > 1 && enabledList[idx].id == history.last().senderId) {
        idx = (idx + 1) % enabledList.size();
    }

    const Participant chosen = enabledList[idx];
    if (roundRobinIndexInOut) {
        *roundRobinIndexInOut = (idx + 1) % enabledList.size();
    }
    return chosen;
}

QJsonArray Orchestrator::buildMessagesForSpeaker(const Participant& speaker,
                                                 const QVector<Participant>& allParticipants,
                                                 const QVector<ChatMessage>& history,
                                                 const QString& /*workspacePath*/,
                                                 int maxContextChars) {
    QStringList rosterLines;
    for (const auto& p : allParticipants) {
        if (!p.enabled) continue;
        rosterLines << QStringLiteral("- %1 (%2) [model: %3]%4")
                           .arg(p.handle, p.displayName, p.modelId,
                                p.id == speaker.id ? QStringLiteral(" <-- YOU") : QString());
    }

    const QString sysPrompt = QStringLiteral(
        "You are %1 (%2) participating in an Agora multi-model engineering & debate room.\n"
        "%3\n\n"
        "### Room Roster\n%4\n\n"
        "### Collaboration & Sandboxed Coding Rules\n"
        "1. You share an isolated workspace mounted at `/workspace`.\n"
        "2. You have tools: `list_files`, `read_file`, `write_file`, `patch_file`, `run_command` (runs inside Bubblewrap sandbox), and `git_diff`.\n"
        "3. To pass the floor or ask another participant a question, mention their `@handle` at the end of your response (e.g. `-> @coder` or `@reviewer`).\n"
        "4. Do not impersonate other participants; speak only as %1.\n")
        .arg(speaker.handle,
             speaker.displayName,
             speaker.systemPrompt,
             rosterLines.join('\n'));

    // Collect valid history entries (excluding empty pending placeholders)
    QVector<ChatMessage> validHistory;
    for (const auto& msg : history) {
        if (msg.id.startsWith("pending_") && msg.content.isEmpty() && msg.toolCalls.isEmpty()) {
            continue;
        }
        validHistory.append(msg);
    }

    // Format a single ChatMessage into one or more OpenAI message objects, shortening older tool outputs
    auto formatEntry = [&](const ChatMessage& msg, bool isRecentTurn) -> QVector<QJsonObject> {
        QVector<QJsonObject> items;
        const int toolOutputLimit = isRecentTurn ? 8000 : 1200;

        if (msg.senderId == speaker.id) {
            QJsonObject assistantMsg;
            assistantMsg["role"] = "assistant";
            assistantMsg["content"] = msg.content;
            if (!msg.toolCalls.isEmpty()) {
                QJsonArray tcArr;
                for (const auto& tc : msg.toolCalls) {
                    QJsonObject fn;
                    fn["name"] = tc.name;
                    fn["arguments"] = tc.argumentsJson;
                    QJsonObject item;
                    item["id"] = tc.id;
                    item["type"] = "function";
                    item["function"] = fn;
                    tcArr.append(item);
                }
                assistantMsg["tool_calls"] = tcArr;
            }
            items.append(assistantMsg);

            for (const auto& tr : msg.toolResults) {
                QJsonObject toolMsg;
                toolMsg["role"] = "tool";
                toolMsg["tool_call_id"] = tr.toolCallId;
                toolMsg["name"] = tr.name;
                QString outText = tr.output;
                if (outText.size() > toolOutputLimit) {
                    outText = outText.left(toolOutputLimit) +
                              QStringLiteral("\n...[earlier tool output shortened to fit context budget]...");
                }
                toolMsg["content"] = outText;
                items.append(toolMsg);
            }
        } else {
            const QString modelTag = msg.modelId.isEmpty() ? QString() : (QStringLiteral(" | ") + msg.modelId);
            QString formatted = QStringLiteral("[Speaker: %1 (%2%3)]\n%4")
                                    .arg(msg.senderHandle, msg.displayName, modelTag, msg.content);
            for (const auto& tr : msg.toolResults) {
                QString outText = tr.output.left(toolOutputLimit);
                if (tr.output.size() > toolOutputLimit) {
                    outText += QStringLiteral("\n...[shortened]...");
                }
                formatted += QStringLiteral("\n\n[Executed Tool `%1` (exit=%2)]:\n%3")
                                 .arg(tr.name, QString::number(tr.exitCode), outText);
            }
            QJsonObject userMsg;
            userMsg["role"] = "user";
            userMsg["content"] = formatted;
            items.append(userMsg);
        }
        return items;
    };

    auto estimateChars = [](const QVector<QJsonObject>& objs) -> int {
        int sum = 0;
        for (const auto& o : objs) {
            sum += o["content"].toString().size() + 32;
        }
        return sum;
    };

    const int budget = qMax(2000, maxContextChars - sysPrompt.size());
    QVector<QVector<QJsonObject>> formattedTurns(validHistory.size());
    QVector<int> turnSizes(validHistory.size(), 0);
    int totalChars = 0;

    for (int i = 0; i < validHistory.size(); ++i) {
        const bool isRecent = (i >= validHistory.size() - 2);
        formattedTurns[i] = formatEntry(validHistory[i], isRecent);
        turnSizes[i] = estimateChars(formattedTurns[i]);
        totalChars += turnSizes[i];
    }

    // If totalChars exceeds budget, keep turn 0 (if it's the initial user task and fits)
    // plus as many recent turns from the end as fit within budget.
    QVector<bool> keep(validHistory.size(), true);
    int droppedCount = 0;
    if (totalChars > budget && validHistory.size() > 1) {
        keep.fill(false);
        int used = 0;
        if (turnSizes[0] < budget / 2) {
            keep[0] = true;
            used += turnSizes[0];
        }
        for (int i = validHistory.size() - 1; i >= 0; --i) {
            if (keep[i]) continue;
            if (used + turnSizes[i] <= budget || i == validHistory.size() - 1) {
                keep[i] = true;
                used += turnSizes[i];
            } else {
                droppedCount++;
            }
        }
    }

    QVector<QJsonObject> rawSequence;
    bool insertedOmissionNotice = false;
    for (int i = 0; i < validHistory.size(); ++i) {
        if (!keep[i]) {
            if (!insertedOmissionNotice && droppedCount > 0) {
                QJsonObject omitMsg;
                omitMsg["role"] = "user";
                omitMsg["content"] = QStringLiteral("[System Notice: %1 earlier turn(s) omitted to stay within context window budget.]")
                                         .arg(QString::number(droppedCount));
                rawSequence.append(omitMsg);
                insertedOmissionNotice = true;
            }
            continue;
        }
        for (const auto& item : formattedTurns[i]) {
            rawSequence.append(item);
        }
    }

    // Merge consecutive "user" messages so strict alternating chat templates (Gemma, Mistral, etc.) never reject
    QJsonArray out;
    QJsonObject sysMsg;
    sysMsg["role"] = "system";
    sysMsg["content"] = sysPrompt;
    out.append(sysMsg);

    for (const auto& item : rawSequence) {
        if (!out.isEmpty() &&
            item["role"].toString() == "user" &&
            out.last().toObject()["role"].toString() == "user") {
            QJsonObject prev = out.last().toObject();
            prev["content"] = prev["content"].toString() + QStringLiteral("\n\n---\n\n") + item["content"].toString();
            out[out.size() - 1] = prev;
        } else {
            out.append(item);
        }
    }

    return out;
}

void Orchestrator::addUserMessage(const QString& text) {
    if (text.trimmed().isEmpty()) return;

    ChatMessage msg;
    msg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    msg.senderId = "user";
    msg.senderHandle = "@you";
    msg.displayName = "User";
    msg.colorHex = "#f8fafc";
    msg.content = text.trimmed();
    msg.mentions = extractMentions(msg.content, m_session.participants, /*handoffOnly=*/false);
    msg.timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");

    m_session.messages.append(msg);
    m_session.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    emit messageAdded(m_session.messages.size() - 1, msg);
}

void Orchestrator::startDebate() {
    if (m_running) return;
    m_running = true;
    m_singleStepMode = (m_session.mode == DebateMode::SingleTurn);
    m_completedTurns = 0;
    emit debateStateChanged(true, m_completedTurns, m_session.maxRounds);
    scheduleNextTurn();
}

void Orchestrator::stepSingleTurn() {
    if (m_running) return;
    m_running = true;
    m_singleStepMode = true;
    m_completedTurns = 0;
    emit debateStateChanged(true, m_completedTurns, 1);
    scheduleNextTurn();
}

void Orchestrator::stopDebate() {
    ++m_generationId;
    m_router->abortActiveStream();
    if (m_toolEngine && m_toolEngine->sandbox()) {
        m_toolEngine->sandbox()->cancelRunningCommand();
    }
    if (m_running) {
        m_running = false;
        m_activeSpeakerId.clear();
        m_activeMessageIndex = -1;
        emit activeSpeakerChanged({}, {}, {});
        emit debateStateChanged(false, m_completedTurns, m_session.maxRounds);
        emit statusNotification("Debate paused/stopped.");
    }
}

void Orchestrator::scheduleNextTurn() {
    if (!m_running) return;

    const int limit = m_singleStepMode ? 1 : m_session.maxRounds;
    if (m_completedTurns >= limit) {
        m_running = false;
        m_activeSpeakerId.clear();
        emit activeSpeakerChanged({}, {}, {});
        emit debateStateChanged(false, m_completedTurns, m_session.maxRounds);
        emit statusNotification("Completed target number of turns.");
        return;
    }

    auto nextOpt = selectNextSpeaker(
        m_session.participants, m_session.messages, m_session.mode, &m_roundRobinCursor);
    if (!nextOpt) {
        m_running = false;
        m_activeSpeakerId.clear();
        emit activeSpeakerChanged({}, {}, {});
        emit debateStateChanged(false, m_completedTurns, m_session.maxRounds);
        emit statusNotification("No eligible next speaker found.");
        return;
    }

    const Participant speaker = *nextOpt;
    m_activeSpeakerId = speaker.id;
    emit activeSpeakerChanged(speaker.id, speaker.handle, speaker.modelId);
    emit statusNotification(QStringLiteral("Preparing %1 (%2)...").arg(speaker.handle, speaker.modelId));

    ChatMessage msg;
    msg.id = "pending_" + QUuid::createUuid().toString(QUuid::Id128).left(8);
    msg.senderId = speaker.id;
    msg.senderHandle = speaker.handle;
    msg.displayName = speaker.displayName;
    msg.modelId = speaker.modelId;
    msg.colorHex = speaker.colorHex;
    msg.timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");

    m_session.messages.append(msg);
    m_activeMessageIndex = m_session.messages.size() - 1;
    emit messageAdded(m_activeMessageIndex, msg);

    launchSpeakerTurn(speaker, m_activeMessageIndex, 0);
}

void Orchestrator::launchSpeakerTurn(const Participant& speaker, int messageIndex, int toolDepth) {
    if (!m_running) return;
    const quint64 genId = m_generationId;
    m_currentToolDepth = toolDepth;
    m_activeMessageIndex = messageIndex;

    QString swapLog;
    const bool ready = m_router->ensureModelReady(speaker.modelId, m_session.vramBudgetGiB, &swapLog);

    if (!m_running || genId != m_generationId ||
        m_activeMessageIndex < 0 || m_activeMessageIndex >= m_session.messages.size()) {
        return;
    }

    if (!ready) {
        // Remove the empty pending message placeholder when model preparation fails
        if (m_activeMessageIndex >= 0 && m_activeMessageIndex < m_session.messages.size() &&
            m_session.messages[m_activeMessageIndex].content.isEmpty()) {
            m_session.messages.removeAt(m_activeMessageIndex);
        }
        m_running = false;
        m_activeSpeakerId.clear();
        m_activeMessageIndex = -1;
        emit activeSpeakerChanged({}, {}, {});
        emit debateStateChanged(false, m_completedTurns, m_session.maxRounds);
        emit errorNotification(swapLog.trimmed().isEmpty() ? QStringLiteral("Router/model preparation failed.") : swapLog.trimmed());
        return;
    }

    emit statusNotification(QStringLiteral("%1 (%2) is speaking...").arg(speaker.handle, speaker.modelId));

    const QJsonArray messages = buildMessagesForSpeaker(
        speaker, m_session.participants, m_session.messages, m_session.workspacePath);
    const QJsonArray tools = (toolDepth < m_maxToolDepth)
                                 ? ToolEngine::openAiToolsSchema()
                                 : QJsonArray{};

    m_router->startChatStream(speaker.modelId, messages, tools, speaker.temperature);
}

void Orchestrator::onModelStreamFinished(const QString& content,
                                         const QString& reasoning,
                                         const QVector<ToolCall>& toolCalls) {
    if (!m_running || m_activeMessageIndex < 0 || m_activeMessageIndex >= m_session.messages.size()) {
        return;
    }

    const quint64 genId = m_generationId;
    const int msgIdx = m_activeMessageIndex;
    const QString speakerId = m_session.messages[msgIdx].senderId;
    const QString speakerHandle = m_session.messages[msgIdx].senderHandle;

    m_session.messages[msgIdx].content = content;
    m_session.messages[msgIdx].reasoningContent = reasoning;
    m_session.messages[msgIdx].mentions = extractMentions(content, m_session.participants, /*handoffOnly=*/true);

    if (!toolCalls.isEmpty() && m_currentToolDepth < m_maxToolDepth) {
        for (const auto& tc : toolCalls) {
            if (!m_running || genId != m_generationId || msgIdx >= m_session.messages.size()) {
                return;
            }
            m_session.messages[msgIdx].toolCalls.append(tc);
            emit statusNotification(QStringLiteral("%1 executing tool `%2`...").arg(speakerHandle, tc.name));
            const ToolResult res = m_toolEngine->executeToolCall(tc);
            if (!m_running || genId != m_generationId || msgIdx >= m_session.messages.size()) {
                return;
            }
            m_session.messages[msgIdx].toolResults.append(res);
            emit messageUpdated(msgIdx, m_session.messages[msgIdx]);
        }

        for (const auto& p : m_session.participants) {
            if (p.id == speakerId) {
                QTimer::singleShot(50, this, [this, p, genId]() {
                    if (!m_running || genId != m_generationId) return;
                    ChatMessage followMsg;
                    followMsg.id = "pending_" + QUuid::createUuid().toString(QUuid::Id128).left(8);
                    followMsg.senderId = p.id;
                    followMsg.senderHandle = p.handle;
                    followMsg.displayName = p.displayName;
                    followMsg.modelId = p.modelId;
                    followMsg.colorHex = p.colorHex;
                    followMsg.timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
                    m_session.messages.append(followMsg);
                    m_activeMessageIndex = m_session.messages.size() - 1;
                    emit messageAdded(m_activeMessageIndex, followMsg);
                    launchSpeakerTurn(p, m_activeMessageIndex, m_currentToolDepth + 1);
                });
                return;
            }
        }
    }

    if (!m_running || genId != m_generationId || msgIdx >= m_session.messages.size()) {
        return;
    }

    m_session.messages[msgIdx].id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_session.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    emit messageUpdated(msgIdx, m_session.messages[msgIdx]);

    m_completedTurns++;
    emit debateStateChanged(m_running, m_completedTurns, m_session.maxRounds);

    QTimer::singleShot(150, this, [this, genId]() {
        if (!m_running || genId != m_generationId) return;
        scheduleNextTurn();
    });
}

} // namespace agora
