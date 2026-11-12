#pragma once

#include "core/types.h"

#include <QJsonArray>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

namespace agora {

class LlamaRouterClient : public QObject {
    Q_OBJECT
public:
    explicit LlamaRouterClient(QObject* parent = nullptr);

    void setBaseUrl(const QString& url) { m_baseUrl = url.trimmed(); }
    QString baseUrl() const { return m_baseUrl; }

    void setApiKey(const QString& key) { m_apiKey = key.trimmed(); }
    QString apiKey() const { return m_apiKey; }

    void setModelsDir(const QString& dir) { m_modelsDir = dir.trimmed(); }
    QString modelsDir() const { return m_modelsDir; }

    void setRouterScriptPath(const QString& path) { m_routerScriptPath = path.trimmed(); }
    QString routerScriptPath() const { return m_routerScriptPath; }

    static QStringList planEvictionsForModel(const QVector<ModelInfo>& models,
                                             const QString& targetModelId,
                                             double vramBudgetGiB);

    QVector<ModelInfo> scanLocalGgufModels() const;
    QVector<ModelInfo> fetchModelsBlocking(bool* routerOnlineOut = nullptr, QString* errorOut = nullptr);
    bool unloadModelBlocking(const QString& modelId, QString* errorOut = nullptr);
    bool loadModelBlocking(const QString& modelId, QString* errorOut = nullptr);
    bool ensureModelReady(const QString& targetModelId, double vramBudgetGiB, QString* statusLogOut = nullptr);

    bool startRouterScript(QString* errorOut = nullptr);

    void startChatStream(const QString& modelId,
                         const QJsonArray& messages,
                         const QJsonArray& tools,
                         double temperature);
    void abortActiveStream();
    bool isStreaming() const { return m_activeReply != nullptr; }

    // For unit testing SSE parsing directly without a live socket
    void resetStreamState();
    void feedSseDataForTesting(const QByteArray& rawSse, bool flushAll = false);
    QString accumulatedContent() const { return m_accumContent; }
    QString accumulatedReasoning() const { return m_accumReasoning; }
    QVector<ToolCall> finalizedToolCalls() const;
    bool receivedDoneMarker() const { return m_receivedDoneMarker; }

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
    bool sendBlockingRequest(const QString& method,
                             const QString& path,
                             const QByteArray& body,
                             int timeoutMs,
                             QByteArray* responseOut,
                             QString* errorOut);
    static QString extractHttpErrorDetail(QNetworkReply* reply, const QByteArray& responseBody);
    void processSseBuffer(bool flushAll = false);
    void handleSsePayload(const QByteArray& jsonBytes);

    QString m_baseUrl = "http://127.0.0.1:8089";
    QString m_apiKey = "sk-local";
    QString m_modelsDir;
    QString m_routerScriptPath;
    QNetworkAccessManager m_nam;

    QPointer<QNetworkReply> m_activeReply;
    QByteArray m_sseBuffer;
    QByteArray m_rawHttpErrorBuffer;
    QString m_accumContent;
    QString m_accumReasoning;
    QMap<int, PartialToolCall> m_partialToolCalls;
    bool m_abortedByUser = false;
    bool m_receivedDoneMarker = false;
};

} // namespace agora
