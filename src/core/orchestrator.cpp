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
        if (m_running && m_activeMessageIndex >= 0 && m_activeMessageIndex < m_session.messages.size()) {
            ChatMessage& msg = m_session.messages[m_activeMessageIndex];
            msg.reasoningContent += delta;
            emit messageUpdated(m_activeMessageIndex, msg);
        }
    });

    QObject::connect(m_router, &LlamaRouterClient::streamContentDelta, this, [this](const QString& delta) {
        if (m_running && m_activeMessageIndex >= 0 && m_activeMessageIndex < m_session.messages.size()) {
            ChatMessage& msg = m_session.messages[m_activeMessageIndex];
            msg.content += delta;
            emit messageUpdated(m_activeMessageIndex, msg);
        }
    });

    QObject::connect(m_router, &LlamaRouterClient::streamFinished, this, &Orchestrator::onModelStreamFinished);
    QObject::connect(m_router, &LlamaRouterClient::streamError, this, &Orchestrator::onModelStreamError);
}

void Orchestrator::setSession(const RoomSession& s) {
    abortInFlight();
    m_running = false;
    m_activeSpeakerId.clear();
    m_activeMessageIndex = -1;
    m_session = s;
    m_completedTurns = 0;
    m_roundRobinCursor = 0;
    m_sessionApprovedTools.clear();
    emit activeSpeakerChanged({}, {}, {});
    emit debateStateChanged(false, m_completedTurns, m_session.maxRounds);
}

const ProviderConfig* Orchestrator::providerFor(const Participant& p) const {
    if (p.providerId.isEmpty()) return nullptr;
    for (const auto& prov : m_providers) {
        if (prov.id == p.providerId) return &prov;
    }
    return nullptr;
}

QString Orchestrator::modelKey(const Participant& p) {
    return p.providerId.isEmpty() ? p.modelId : (p.providerId + '/' + p.modelId);
}

double Orchestrator::charsPerTokenFor(const QString& modelKey) const {
    return m_charsPerToken.value(modelKey, kDefaultCharsPerToken);
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

int Orchestrator::contextCharBudget(int contextTokens, double charsPerToken) {
    if (contextTokens <= 0) return kDefaultMaxContextChars;
    // The ratio is measured on message text only, so tool schema and chat-template
    // overhead are already priced in; keep a quarter of the window for the reply.
    const double cpt = qBound(1.0, charsPerToken, 8.0);
    return qMax(8000, static_cast<int>(contextTokens * 0.75 * cpt));
}

bool Orchestrator::declaresDone(const QString& content) {
    return content.contains(QStringLiteral("[DONE]"));
}

QStringList Orchestrator::participantsYetToSpeak(const QVector<Participant>& participants,
                                                 const QVector<ChatMessage>& history) {
    QSet<QString> spoke;
    for (int i = history.size() - 1; i >= 0; --i) {
        const ChatMessage& msg = history[i];
        if (msg.senderId == "user") break;
        if (msg.senderId != "system") spoke.insert(msg.senderId);
    }
    QStringList waiting;
    for (const auto& p : participants) {
        if (p.enabled && !spoke.contains(p.id)) waiting << p.handle;
    }
    return waiting;
}

bool Orchestrator::needsApproval(ToolApprovalPolicy policy,
                                 const QString& toolName,
                                 const QSet<QString>& approvedForSession) {
    if (policy == ToolApprovalPolicy::Off || approvedForSession.contains(toolName)) return false;
    if (policy == ToolApprovalPolicy::AskForWrites) return !ToolEngine::isReadOnlyTool(toolName);
    return true;
}

bool Orchestrator::isContextOverflowError(const QString& errorText) {
    const QString e = errorText.toLower();
    return e.contains("exceed_context_size") || e.contains("context_length_exceeded") ||
           (e.contains("context") && (e.contains("exceed") || e.contains("too long") ||
                                      e.contains("maximum context length")));
}

bool Orchestrator::isTemperatureRejectedError(const QString& errorText) {
    const QString e = errorText.toLower();
    return e.contains("temperature") &&
           (e.contains("unsupported") || e.contains("not support") || e.contains("only the default"));
}

bool Orchestrator::isTransientError(const QString& errorText) {
    static const QRegularExpression serverError(QStringLiteral("^HTTP (429|500|502|503|504|529):"));
    const QString e = errorText.toLower();
    return serverError.match(errorText).hasMatch() || e.contains("overloaded") || e.contains("rate limit");
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
        "3. Stay in your role: do your own part of the task, then pass the floor by ending your message with the "
        "`@handle` of the participant best suited for the next step (e.g. `-> @coder`). Do not do the whole task "
        "alone; every participant in the roster has their own job.\n"
        "4. Do not impersonate other participants; speak only as %1.\n"
        "5. Write `[DONE]` at the end of your message only when the user's task is fully complete, verified in "
        "the sandbox, and every participant has contributed. Never write it otherwise.\n")
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
            // Only calls that got a result: a run stopped mid-tool leaves one dangling, and
            // hosted APIs reject a tool_call without a matching tool message
            QSet<QString> answered;
            for (const auto& tr : msg.toolResults) answered.insert(tr.toolCallId);
            if (!msg.toolCalls.isEmpty()) {
                QJsonArray tcArr;
                for (const auto& tc : msg.toolCalls) {
                    if (!answered.contains(tc.id)) continue;
                    QJsonObject fn;
                    fn["name"] = tc.name;
                    fn["arguments"] = tc.argumentsJson;
                    QJsonObject item;
                    item["id"] = tc.id;
                    item["type"] = "function";
                    item["function"] = fn;
                    tcArr.append(item);
                }
                if (!tcArr.isEmpty()) assistantMsg["tool_calls"] = tcArr;
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

    // If totalChars exceeds budget, keep the first user message (the task) if it fits
    // in half the budget, plus as many recent turns from the end as fit.
    QVector<bool> keep(validHistory.size(), true);
    int droppedCount = 0;
    if (totalChars > budget && validHistory.size() > 1) {
        keep.fill(false);
        int used = 0;
        for (int i = 0; i < validHistory.size(); ++i) {
            if (validHistory[i].senderId != "user") continue;
            if (turnSizes[i] < budget / 2) {
                keep[i] = true;
                used += turnSizes[i];
            }
            break;
        }
        // Keep a contiguous run of recent turns: once one doesn't fit, everything older
        // (except the pinned task) is dropped, so the model never sees a gap mid-history
        bool full = false;
        for (int i = validHistory.size() - 1; i >= 0; --i) {
            if (keep[i]) continue;
            if (!full && (used + turnSizes[i] <= budget || i == validHistory.size() - 1)) {
                keep[i] = true;
                used += turnSizes[i];
            } else {
                full = true;
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
    abortInFlight();
    if (m_running) {
        endRun(QStringLiteral("Debate paused/stopped."));
    }
}

void Orchestrator::abortInFlight() {
    ++m_generationId;
    m_router->abortActiveStream();
    m_router->abortPendingRequests();
    if (m_toolEngine && m_toolEngine->sandbox()) {
        m_toolEngine->sandbox()->cancelRunningCommand();
    }
    cancelPendingApproval();
}

void Orchestrator::endRun(const QString& message, bool isError) {
    m_running = false;
    m_activeSpeakerId.clear();
    m_activeMessageIndex = -1;
    emit activeSpeakerChanged({}, {}, {});
    emit debateStateChanged(false, m_completedTurns, m_session.maxRounds);
    if (isError) {
        emit errorNotification(message);
    } else {
        emit statusNotification(message);
    }
}

void Orchestrator::dropEmptyPlaceholder() {
    if (m_activeMessageIndex >= 0 && m_activeMessageIndex == m_session.messages.size() - 1) {
        const ChatMessage& msg = m_session.messages[m_activeMessageIndex];
        if (msg.content.isEmpty() && msg.reasoningContent.isEmpty() && msg.toolCalls.isEmpty()) {
            m_session.messages.removeLast();
            m_activeMessageIndex = -1;
            emit messageUpdated(-1, {}); // forces a full transcript refresh
        }
    }
}

int Orchestrator::appendPendingMessage(const Participant& speaker) {
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
    return m_activeMessageIndex;
}

void Orchestrator::scheduleNextTurn() {
    if (!m_running) return;

    const int limit = m_singleStepMode ? 1 : m_session.maxRounds;
    if (m_completedTurns >= limit) {
        endRun(QStringLiteral("Completed target number of turns."));
        return;
    }

    auto nextOpt = selectNextSpeaker(
        m_session.participants, m_session.messages, m_session.mode, &m_roundRobinCursor);
    if (!nextOpt) {
        endRun(QStringLiteral("No eligible next speaker found."));
        return;
    }
    beginTurn(*nextOpt);
}

void Orchestrator::beginTurn(const Participant& speaker) {
    const quint64 genId = m_generationId;
    m_turnSpeaker = speaker;
    m_turnBaseCommit.clear();
    m_turnPrepMs = 0;
    m_budgetOverride = 0;
    m_contextRetryUsed = false;
    m_transientRetryUsed = false;
    m_activeSpeakerId = speaker.id;
    emit activeSpeakerChanged(speaker.id, speaker.handle, speaker.modelId);
    appendPendingMessage(speaker);

    auto prepareModel = [this, genId, speaker]() {
        if (!alive(genId)) return;
        if (!speaker.providerId.isEmpty()) {
            // External APIs host their own models: nothing to load or evict
            if (!providerFor(speaker)) {
                dropEmptyPlaceholder();
                endRun(QStringLiteral("%1 uses an external provider that is no longer configured. "
                                      "Pick a provider in the participant settings.").arg(speaker.handle), true);
                return;
            }
            startStream(0);
            return;
        }
        emit statusNotification(QStringLiteral("Preparing %1 (%2)...").arg(speaker.handle, speaker.modelId));
        m_router->ensureModelReady(speaker.modelId, m_session.vramBudgetGiB,
                                   [this, genId](bool ok, const QString& log, qint64 elapsedMs) {
            if (!alive(genId)) return;
            if (!ok) {
                dropEmptyPlaceholder();
                endRun(log.isEmpty() ? QStringLiteral("Router/model preparation failed.") : log, true);
                return;
            }
            m_turnPrepMs = elapsedMs;
            startStream(0);
        });
    };

    // Snapshot first so manual edits since the last turn aren't attributed to this speaker
    if (m_autoCheckpoints && m_checkpoints) {
        m_checkpoints->snapshot(m_session.id, "before " + speaker.handle + " turn",
                                [this, genId, prepareModel](const Checkpoint& cp) {
            if (!alive(genId)) return;
            if (cp.ok) m_turnBaseCommit = cp.commit;
            prepareModel();
        });
    } else {
        prepareModel();
    }
}

void Orchestrator::startStream(int toolDepth) {
    m_currentToolDepth = toolDepth;
    const Participant& speaker = m_turnSpeaker;
    emit statusNotification(QStringLiteral("%1 (%2) is speaking...").arg(speaker.handle, speaker.modelId));

    const ProviderConfig* provider = providerFor(speaker);
    const QString key = modelKey(speaker);
    const int contextTokens = provider ? provider->contextTokens : m_router->cachedContextLength(speaker.modelId);
    int budget = contextCharBudget(contextTokens, charsPerTokenFor(key));
    if (m_budgetOverride > 0) budget = qMin(budget, m_budgetOverride);
    m_lastBudget = budget;

    const QJsonArray messages = buildMessagesForSpeaker(
        speaker, m_session.participants, m_session.messages, m_session.workspacePath, budget);
    const QJsonArray tools = (toolDepth < m_maxToolDepth) ? ToolEngine::openAiToolsSchema() : QJsonArray{};

    m_lastSentChars = 0;
    for (const auto& v : messages) {
        m_lastSentChars += v.toObject()["content"].toString().size();
    }

    std::optional<ChatEndpoint> endpoint;
    if (provider) {
        endpoint = ChatEndpoint{provider->baseUrl, provider->resolvedApiKey(), m_omitTemperature.contains(key)};
    }
    m_router->startChatStream(speaker.modelId, messages, tools, speaker.temperature, endpoint);
}

void Orchestrator::clearPartialReply() {
    ChatMessage& msg = m_session.messages[m_activeMessageIndex];
    msg.content.clear();
    msg.reasoningContent.clear();
    emit messageUpdated(m_activeMessageIndex, msg);
}

void Orchestrator::onModelStreamError(const QString& error) {
    if (!m_running || m_activeMessageIndex < 0 || m_activeMessageIndex >= m_session.messages.size()) {
        return;
    }

    if (!m_contextRetryUsed && isContextOverflowError(error)) {
        // Retry once with much less history, and remember the model packs fewer chars per token
        m_contextRetryUsed = true;
        m_budgetOverride = static_cast<int>(m_lastBudget * 0.6);
        const QString key = modelKey(m_turnSpeaker);
        m_charsPerToken[key] = charsPerTokenFor(key) * 0.6;

        clearPartialReply();
        emit statusNotification(QStringLiteral("Context window exceeded; retrying %1 with less history...")
                                    .arg(m_turnSpeaker.handle));
        startStream(m_currentToolDepth);
        return;
    }

    const QString key = modelKey(m_turnSpeaker);
    if (!m_turnSpeaker.providerId.isEmpty() && !m_omitTemperature.contains(key) &&
        isTemperatureRejectedError(error)) {
        // Reasoning models (e.g. OpenAI o-series) accept only their default temperature
        m_omitTemperature.insert(key);
        clearPartialReply();
        emit statusNotification(QStringLiteral("%1 doesn't accept a temperature; retrying without it...")
                                    .arg(m_turnSpeaker.modelId));
        startStream(m_currentToolDepth);
        return;
    }

    if (!m_transientRetryUsed && isTransientError(error)) {
        m_transientRetryUsed = true;
        clearPartialReply();
        const quint64 genId = m_generationId;
        emit statusNotification(QStringLiteral("%1: %2 Retrying in %3 s...")
                                    .arg(m_turnSpeaker.handle, error.left(160),
                                         QString::number(m_transientRetryDelayMs / 1000.0, 'g', 2)));
        QTimer::singleShot(m_transientRetryDelayMs, this, [this, genId]() {
            if (alive(genId)) startStream(m_currentToolDepth);
        });
        return;
    }

    dropEmptyPlaceholder();
    endRun("Model stream error: " + error, true);
}

void Orchestrator::onModelStreamFinished(const QString& content,
                                         const QString& reasoning,
                                         const QVector<ToolCall>& toolCalls) {
    if (!m_running || m_activeMessageIndex < 0 || m_activeMessageIndex >= m_session.messages.size()) {
        return;
    }

    const quint64 genId = m_generationId;
    const int msgIdx = m_activeMessageIndex;
    ChatMessage& msg = m_session.messages[msgIdx];
    msg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    msg.content = content;
    msg.reasoningContent = reasoning;
    msg.mentions = extractMentions(content, m_session.participants, /*handoffOnly=*/true);

    msg.stats = m_router->lastStreamStats();
    msg.stats.modelPrepMs = (m_currentToolDepth == 0) ? m_turnPrepMs : 0;
    if (msg.stats.promptTokens > 0 && m_lastSentChars > 0) {
        const QString model = modelKey(m_turnSpeaker);
        const double measured = static_cast<double>(m_lastSentChars) / msg.stats.promptTokens;
        m_charsPerToken[model] = m_charsPerToken.contains(model)
                                     ? 0.5 * m_charsPerToken[model] + 0.5 * measured
                                     : measured;
    }
    m_budgetOverride = 0;
    m_contextRetryUsed = false;
    m_transientRetryUsed = false;
    emit messageUpdated(msgIdx, msg);

    if (!toolCalls.isEmpty() && m_currentToolDepth < m_maxToolDepth) {
        runToolCall(msgIdx, toolCalls, 0, genId);
        return;
    }
    finalizeTurn(msgIdx, genId);
}

void Orchestrator::runToolCall(int messageIndex, const QVector<ToolCall>& calls, int callIndex, quint64 genId) {
    if (!alive(genId) || messageIndex >= m_session.messages.size()) return;

    if (callIndex >= calls.size()) {
        // Let the speaker continue after seeing the tool output
        appendPendingMessage(m_turnSpeaker);
        startStream(m_currentToolDepth + 1);
        return;
    }

    const ToolCall call = calls[callIndex];
    const QString handle = m_turnSpeaker.handle;

    auto execute = [this, messageIndex, calls, callIndex, genId, call, handle]() {
        m_session.messages[messageIndex].toolCalls.append(call);
        emit messageUpdated(messageIndex, m_session.messages[messageIndex]);
        emit statusNotification(QStringLiteral("%1 executing tool `%2`...").arg(handle, call.name));
        m_toolEngine->executeToolCallAsync(call, [this, messageIndex, calls, callIndex, genId](const ToolResult& res) {
            if (!alive(genId) || messageIndex >= m_session.messages.size()) return;
            m_session.messages[messageIndex].toolResults.append(res);
            emit messageUpdated(messageIndex, m_session.messages[messageIndex]);
            runToolCall(messageIndex, calls, callIndex + 1, genId);
        });
    };

    if (!needsApproval(m_approvalPolicy, call.name, m_sessionApprovedTools)) {
        execute();
        return;
    }

    requestApproval(handle, call, [this, messageIndex, calls, callIndex, genId, call, execute](
                                      ToolApprovalDecision decision, const QString& reason) {
        if (!alive(genId) || messageIndex >= m_session.messages.size()) return;
        if (decision == ToolApprovalDecision::Deny) {
            ChatMessage& msg = m_session.messages[messageIndex];
            msg.toolCalls.append(call);
            QString output = QStringLiteral("Denied by the user; the tool was not run.");
            if (!reason.trimmed().isEmpty()) output += " Reason: " + reason.trimmed();
            msg.toolResults.append({call.id, call.name, output, true, 1});
            emit messageUpdated(messageIndex, msg);
            runToolCall(messageIndex, calls, callIndex + 1, genId);
            return;
        }
        if (decision == ToolApprovalDecision::AllowToolForSession) {
            m_sessionApprovedTools.insert(call.name);
        }
        execute();
    });
}

void Orchestrator::appendSystemNote(const QString& text) {
    ChatMessage note;
    note.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    note.senderId = "system";
    note.senderHandle = "@agora";
    note.displayName = "Agora";
    note.colorHex = "#a78bfa";
    note.timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    note.content = text;
    m_session.messages.append(note);
    m_session.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    emit messageAdded(m_session.messages.size() - 1, note);
}

void Orchestrator::finalizeTurn(int messageIndex, quint64 genId) {
    m_session.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    const QString handle = m_turnSpeaker.handle;
    bool done = declaresDone(m_session.messages[messageIndex].content);
    // In round-robin rooms one participant can't end the run for everyone: [DONE] counts only
    // once every enabled participant has had a turn since the user's latest message
    QStringList waiting;
    if (done && m_session.mode == DebateMode::RoundRobinWithMentions) {
        waiting = participantsYetToSpeak(m_session.participants, m_session.messages);
        if (!waiting.isEmpty()) done = false;
    }

    auto advance = [this, genId, done, handle, waiting]() {
        if (!alive(genId)) return;
        m_completedTurns++;
        emit debateStateChanged(true, m_completedTurns, m_session.maxRounds);
        if (done) {
            endRun(QStringLiteral("%1 marked the task complete [DONE].").arg(handle));
            return;
        }
        if (!waiting.isEmpty()) {
            // The first @mention in the latest message picks the next speaker
            appendSystemNote(QStringLiteral(
                "%1, it's your turn. %2 considers the task complete, but %3 not contributed yet, so the "
                "session continues. Review the work so far from your role's perspective: improve it, "
                "fix problems, or confirm it, then hand off.")
                .arg(waiting.first(), QString(handle).remove('@'),
                     waiting.size() == 1 ? QStringLiteral("you have")
                                         : waiting.join(", ").remove('@') + QStringLiteral(" have")));
            emit statusNotification(QStringLiteral("%1 said [DONE], but %2 still to speak; continuing.")
                                        .arg(handle, waiting.join(", ")));
        }
        QTimer::singleShot(150, this, [this, genId]() {
            if (alive(genId)) scheduleNextTurn();
        });
    };

    if (!(m_autoCheckpoints && m_checkpoints)) {
        advance();
        return;
    }

    const QString sessionId = m_session.id;
    const QString base = m_turnBaseCommit;
    m_checkpoints->snapshot(sessionId, "after " + handle + " turn",
                            [this, sessionId, base, messageIndex, advance](const Checkpoint& cp) {
        // Record the checkpoint even if the user pressed Stop meanwhile, but never into
        // a different session
        if (cp.ok && cp.changed && m_session.id == sessionId && messageIndex < m_session.messages.size()) {
            ChatMessage& msg = m_session.messages[messageIndex];
            msg.checkpointCommit = cp.commit;
            msg.checkpointParent = base.isEmpty() ? cp.parent : base;
            emit messageUpdated(messageIndex, msg);
        }
        advance();
    });
}

void Orchestrator::requestApproval(const QString& speakerHandle, const ToolCall& call,
                                   std::function<void(ToolApprovalDecision, const QString&)> onDecision) {
    cancelPendingApproval();
    m_pendingApprovalId = ++m_nextApprovalId;
    m_pendingApproval = std::move(onDecision);
    emit statusNotification(QStringLiteral("Waiting for your approval: %1 wants to run `%2`")
                                .arg(speakerHandle, call.name));
    emit toolApprovalRequested(m_pendingApprovalId, speakerHandle, call);
}

void Orchestrator::resolveToolApproval(quint64 requestId, ToolApprovalDecision decision, const QString& reason) {
    if (requestId == 0 || requestId != m_pendingApprovalId || !m_pendingApproval) return;
    auto callback = std::move(m_pendingApproval);
    m_pendingApproval = nullptr;
    m_pendingApprovalId = 0;
    callback(decision, reason);
}

void Orchestrator::cancelPendingApproval() {
    if (m_pendingApprovalId == 0) return;
    const quint64 id = m_pendingApprovalId;
    m_pendingApprovalId = 0;
    m_pendingApproval = nullptr;
    emit toolApprovalCancelled(id);
}

void Orchestrator::restoreCheckpointBefore(int messageIndex) {
    if (m_running) {
        emit errorNotification(QStringLiteral("Stop the debate before restoring a checkpoint."));
        return;
    }
    if (!m_checkpoints || messageIndex < 0 || messageIndex >= m_session.messages.size()) return;
    const ChatMessage& msg = m_session.messages[messageIndex];
    if (msg.checkpointParent.isEmpty()) {
        emit errorNotification(QStringLiteral("This turn has no checkpoint to restore."));
        return;
    }

    const QString sessionId = m_session.id;
    const QString target = msg.checkpointParent;
    const QString handle = msg.senderHandle;
    emit statusNotification(QStringLiteral("Restoring workspace to before %1's turn...").arg(handle));
    m_checkpoints->restore(sessionId, target, [this, sessionId, target, handle](bool ok, const QString& savedOrError) {
        if (m_session.id != sessionId) return;
        if (!ok) {
            emit errorNotification("Restore failed: " + savedOrError);
            return;
        }
        appendSystemNote(QStringLiteral("Workspace restored to its state before %1's turn (checkpoint %2). "
                                        "The state just before this restore was saved as checkpoint %3.")
                             .arg(handle, target.left(10), savedOrError.left(10)));
        emit workspaceChanged();
        emit statusNotification(QStringLiteral("Workspace restored."));
    });
}

} // namespace agora
