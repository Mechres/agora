#pragma once

#include "core/types.h"

#include <QJsonArray>
#include <QHash>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QSet>
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

    // Only loaded/loading models hold VRAM; a sleeping one has freed its weights and KV cache
    static bool occupiesVram(const QString& status) { return status == "loaded" || status == "loading"; }

    // Models to unload so the target fits the budget. Models whose next use comes soonest in
    // `upcoming` are evicted last (largest first among equals); models in `keep` never are.
    // nullopt when the target can't fit without evicting a kept model.
    static std::optional<QStringList> planEvictions(const QVector<ModelInfo>& models,
                                                    const QString& targetModelId,
                                                    double vramBudgetGiB,
                                                    const QStringList& upcoming = {},
                                                    const QSet<QString>& keep = {});
    static QStringList planEvictionsForModel(const QVector<ModelInfo>& models,
                                             const QString& targetModelId,
                                             double vramBudgetGiB);

    // Waiting for a model to finish loading (the router's /models/load returns immediately)
    void setModelLoadTimeoutMs(int ms) { m_modelLoadTimeoutMs = qMax(1000, ms); }
    void setStatusPollIntervalMs(int ms) { m_statusPollIntervalMs = qMax(10, ms); }

    using ModelsCallback = std::function<void(bool online, const QVector<ModelInfo>& models, const QString& error)>;
    using ReadyCallback = std::function<void(bool ok, const QString& log, qint64 elapsedMs)>;

    QVector<ModelInfo> scanLocalGgufModels() const;
    // All network calls are asynchronous; callbacks run on the event loop
    void fetchModels(ModelsCallback done = {});
    void unloadModel(const QString& modelId, std::function<void(bool ok, const QString& error)> done = {});
    void loadModel(const QString& modelId, std::function<void(bool ok, const QString& error)> done = {});
    // Evicts models over the VRAM budget (those needed latest per `upcoming` first), loads or wakes
    // the target and waits until it is up, then learns its context window
    void ensureModelReady(const QString& targetModelId, double vramBudgetGiB, ReadyCallback done,
                          const QStringList& upcoming = {});
    // Starts loading a model ahead of its turn without disturbing the models in `keep`. When it
    // can't fit next to them, its file is read into the page cache instead so the later load
    // skips the disk. `done` reports what was done, once the load has been started.
    void prefetchModel(const QString& targetModelId, double vramBudgetGiB, const QStringList& upcoming,
                       const QSet<QString>& keep, std::function<void(const QString& outcome)> done);
    // Asynchronous readahead of a file into the page cache (skipped when RAM is short)
    static void warmFileCache(const QString& path);
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
    // Polls /models until `ready` holds (ok=true) or the timeout passes or requests are aborted
    void waitForModels(std::function<bool(const QVector<ModelInfo>&)> ready, int timeoutMs,
                       std::function<void(bool ok, const QVector<ModelInfo>& models)> done);
    // Unloads each model, then waits until they have actually released their memory
    void unloadAndWait(const QStringList& modelIds, std::function<void(const QString& log)> done);
    void loadAndWait(const QString& modelId, std::function<void(bool ok, const QString& error)> done);
    // A sleeping model only wakes on a real request; a one-token completion does it
    void wakeModel(const QString& modelId, std::function<void(bool ok, const QString& error)> done);
    void processSseBuffer(bool flushAll = false);
    void handleSsePayload(const QByteArray& jsonBytes);

    QString m_baseUrl = "http://127.0.0.1:8089";
    QString m_apiKey = "sk-local";
    QString m_modelsDir;
    QString m_routerScriptPath;
    int m_streamIdleTimeoutMs = 600000;
    int m_modelLoadTimeoutMs = 300000;
    int m_statusPollIntervalMs = 250;
    quint64 m_abortGeneration = 0; // bumped by abortPendingRequests to stop status polling
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
