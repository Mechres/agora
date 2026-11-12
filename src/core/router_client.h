#pragma once

#include "core/types.h"

#include <QJsonArray>
#include <QHash>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <functional>
#include <optional>

namespace agora {

// Target of a chat request when it isn't the local router: an external OpenAI-compatible API
struct ChatEndpoint {
    QString baseUrl;  // up to and including the version, e.g. "https://api.openai.com/v1"
    QString apiKey;   // already resolved (no "$ENV" references)
    bool omitTemperature = false; // some reasoning models reject any temperature
};

class LlamaRouterClient : public QObject {
    Q_OBJECT
public:
    explicit LlamaRouterClient(QObject* parent = nullptr);

    void setBaseUrl(const QString& url) {
        m_baseUrl = url.trimmed();
        m_contextLengthCache.clear();
    }
    QString baseUrl() const { return m_baseUrl; }

    void setApiKey(const QString& key) { m_apiKey = key.trimmed(); }
    QString apiKey() const { return m_apiKey; }

    void setModelsDir(const QString& dir) { m_modelsDir = dir.trimmed(); }
    QString modelsDir() const { return m_modelsDir; }

    void setRouterScriptPath(const QString& path) { m_routerScriptPath = path.trimmed(); }
    QString routerScriptPath() const { return m_routerScriptPath; }

    // llama-server sends nothing while processing the prompt, so this must cover the
    // slowest prompt prefill of the largest model, not just the gap between tokens.
    void setStreamIdleTimeoutMs(int ms) { m_streamIdleTimeoutMs = qMax(10000, ms); }
    int streamIdleTimeoutMs() const { return m_streamIdleTimeoutMs; }

    static QStringList planEvictionsForModel(const QVector<ModelInfo>& models,
                                             const QString& targetModelId,
                                             double vramBudgetGiB);

    using ModelsCallback = std::function<void(bool online, const QVector<ModelInfo>& models, const QString& error)>;
    using ReadyCallback = std::function<void(bool ok, const QString& log, qint64 elapsedMs)>;

    QVector<ModelInfo> scanLocalGgufModels() const;
    // All network calls are asynchronous; callbacks run on the event loop
    void fetchModels(ModelsCallback done = {});
    void unloadModel(const QString& modelId, std::function<void(bool ok, const QString& error)> done = {});
    void loadModel(const QString& modelId, std::function<void(bool ok, const QString& error)> done = {});
    // Evicts models over the VRAM budget, loads the target, then learns its context window
    void ensureModelReady(const QString& targetModelId, double vramBudgetGiB, ReadyCallback done);
    // Aborts model-management requests (not the chat stream); their callbacks get ok=false
    void abortPendingRequests();

    // Context window (tokens) via GET /props?model=..., cached per model; 0 when unknown
    void fetchContextLength(const QString& modelId, std::function<void(int contextTokens)> done);
    int cachedContextLength(const QString& modelId) const;
    static int parseContextLengthFromProps(const QByteArray& propsJson);

    bool startRouterScript(QString* errorOut = nullptr);

    // Streams from the local router, or from an external API when an endpoint is given
    void startChatStream(const QString& modelId,
                         const QJsonArray& messages,
                         const QJsonArray& tools,
                         double temperature,
                         const std::optional<ChatEndpoint>& external = std::nullopt);

    // GET {baseUrl}/models of an external API; model ids sorted
    using ProviderModelsCallback = std::function<void(bool ok, const QStringList& models, const QString& error)>;
    void fetchProviderModels(const ChatEndpoint& endpoint, ProviderModelsCallback done);
    static QStringList parseProviderModels(const QByteArray& modelsJson);
    // Hosted APIs validate strictly: drop the llama.cpp-only "name" field from tool messages
    static QJsonArray sanitizeMessagesForExternal(const QJsonArray& messages);
    void abortActiveStream();
    bool isStreaming() const { return m_activeReply != nullptr; }

    // For unit testing SSE parsing directly without a live socket
    void resetStreamState();
    void feedSseDataForTesting(const QByteArray& rawSse, bool flushAll = false);
    QString accumulatedContent() const { return m_accumContent; }
    QString accumulatedReasoning() const { return m_accumReasoning; }
    QVector<ToolCall> finalizedToolCalls() const;
    bool receivedDoneMarker() const { return m_receivedDoneMarker; }
    // Token counts and timings of the last completed stream (empty if the server sent none)
    TurnStats lastStreamStats() const { return m_lastStats; }

signals:
    void routerStatusChanged(bool online, const QVector<ModelInfo>& models);
    void vramSwapNotification(const QString& message);
    void streamReasoningDelta(const QString& delta);
    void streamContentDelta(const QString& delta);
    void streamFinished(const QString& fullContent,
                        const QString& fullReasoning,
                        const QVector<ToolCall>& toolCalls);
    void streamError(const QString& errorMessage);

private:
    struct PartialToolCall {
        QString id;
        QString name;
        QString arguments;
    };

    QNetworkRequest makeRequest(const QString& path, int transferTimeoutMs = 30000) const;
    static QNetworkRequest makeRequestTo(const QString& baseUrl, const QString& path,
                                         const QString& apiKey, int transferTimeoutMs);
    using RequestCallback = std::function<void(bool ok, const QByteArray& payload, const QString& error)>;
    void sendRequest(const QString& method,
                     const QString& path,
                     const QByteArray& body,
                     int timeoutMs,
                     RequestCallback done);
    void sendRequest(const QString& method, const QNetworkRequest& req, const QByteArray& body, RequestCallback done);
    static QString extractHttpErrorDetail(QNetworkReply* reply, const QByteArray& responseBody);
    void processSseBuffer(bool flushAll = false);
    void handleSsePayload(const QByteArray& jsonBytes);

    QString m_baseUrl = "http://127.0.0.1:8089";
    QString m_apiKey = "sk-local";
    QString m_modelsDir;
    QString m_routerScriptPath;
    int m_streamIdleTimeoutMs = 600000;
    QHash<QString, int> m_contextLengthCache;
    QNetworkAccessManager m_nam;
    QList<QPointer<QNetworkReply>> m_pendingRequests;

    QPointer<QNetworkReply> m_activeReply;
    QByteArray m_sseBuffer;
    QByteArray m_rawHttpErrorBuffer;
    QString m_accumContent;
    QString m_accumReasoning;
    QMap<int, PartialToolCall> m_partialToolCalls;
    bool m_abortedByUser = false;
    bool m_receivedDoneMarker = false;
    TurnStats m_lastStats;
};

} // namespace agora
