#pragma once

// Minimal stand-in for llama-server in router mode, enough for end-to-end tests:
// GET /models, GET /props, POST /models/load|unload and a scripted queue of
// /v1/chat/completions responses (SSE streams or HTTP errors).

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQueue>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVector>

class FakeLlamaServer {
public:
    struct ScriptedReply {
        int status = 200;
        QByteArray body;
    };
    struct RequestRecord {
        QByteArray method;
        QByteArray path;
        QByteArray authorization;
    };

    explicit FakeLlamaServer(QString modelId = "fake-model", int contextTokens = 8192)
        : m_modelId(std::move(modelId)), m_contextTokens(contextTokens) {
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this]() {
            while (QTcpSocket* sock = m_server.nextPendingConnection()) {
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [this, sock]() { onReadyRead(sock); });
                QObject::connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
            }
        });
        m_server.listen(QHostAddress::LocalHost, 0);
    }

    QString baseUrl() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

    void enqueueChat(const ScriptedReply& reply) { m_chatReplies.enqueue(reply); }

    // Multi-model router mode, modelled on llama.cpp's router: /models/load returns at once and the
    // model turns "loaded" after a delay; unloading takes a moment to release memory; a sleeping
    // model holds no VRAM and wakes on any completion request
    void addModel(const QString& id, const QString& status, double sizeGiB) {
        m_models.append({id, status, sizeGiB});
    }
    void setLoadDelayMs(int ms) { m_loadDelayMs = ms; }
    QString modelStatus(const QString& id) const {
        for (const auto& m : m_models) {
            if (m.id == id) return m.status;
        }
        return {};
    }
    // "load X", "unload X", "wake X", "chat X" in arrival order
    const QStringList& events() const { return m_events; }
    int chatsBeforeModelReady() const { return m_chatsBeforeReady; }
    double peakVramGiB() const { return m_peakVramGiB; }
    const QVector<QJsonObject>& chatRequests() const { return m_chatRequests; }
    const QVector<RequestRecord>& requestLog() const { return m_requestLog; }

    // SSE body for a reply with optional content, one tool call and final usage/timings
    static QByteArray sse(const QString& content,
                          const QString& toolName = {},
                          const QJsonObject& toolArgs = {},
                          int promptTokens = 120,
                          int completionTokens = 15) {
        QByteArray out;
        auto chunk = [&out](const QJsonObject& obj) {
            out += "data: " + QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n\n";
        };
        if (!content.isEmpty()) {
            chunk({{"choices", QJsonArray{QJsonObject{{"delta", QJsonObject{{"content", content}}}}}}});
        }
        if (!toolName.isEmpty()) {
            const QJsonObject fn{{"name", toolName},
                                 {"arguments", QString::fromUtf8(QJsonDocument(toolArgs).toJson(QJsonDocument::Compact))}};
            const QJsonObject tc{{"index", 0}, {"id", "call_fake_1"}, {"type", "function"}, {"function", fn}};
            chunk({{"choices", QJsonArray{QJsonObject{{"delta", QJsonObject{{"tool_calls", QJsonArray{tc}}}}}}}});
        }
        chunk({{"choices", QJsonArray{}},
               {"usage", QJsonObject{{"prompt_tokens", promptTokens}, {"completion_tokens", completionTokens}}},
               {"timings", QJsonObject{{"prompt_n", promptTokens}, {"prompt_ms", 50.0},
                                       {"predicted_n", completionTokens}, {"predicted_ms", 500.0},
                                       {"predicted_per_second", 30.0}}}});
        out += "data: [DONE]\n\n";
        return out;
    }

private:
    void onReadyRead(QTcpSocket* sock) {
        QByteArray& buf = m_buffers[sock];
        buf += sock->readAll();
        const qsizetype headerEnd = buf.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;

        const QList<QByteArray> lines = buf.left(headerEnd).split('\n');
        const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
        qsizetype contentLength = 0;
        QByteArray authorization;
        for (const QByteArray& line : lines) {
            if (line.toLower().startsWith("content-length:")) {
                contentLength = line.mid(15).trimmed().toLongLong();
            } else if (line.toLower().startsWith("authorization:")) {
                authorization = line.mid(14).trimmed();
            }
        }
        if (buf.size() < headerEnd + 4 + contentLength) return;

        const QByteArray method = requestLine.value(0);
        const QByteArray path = requestLine.value(1);
        const QByteArray body = buf.mid(headerEnd + 4, contentLength);
        m_buffers.remove(sock);
        m_requestLog.append({method, path, authorization});
        respond(sock, route(method, path, body));
    }

    struct FakeModel {
        QString id;
        QString status;
        double sizeGiB = 0;
    };

    FakeModel* findModel(const QString& id) {
        for (auto& m : m_models) {
            if (m.id == id) return &m;
        }
        return nullptr;
    }

    void setStatus(const QString& id, const QString& status) {
        if (FakeModel* m = findModel(id)) m->status = status;
        double used = 0;
        for (const auto& m : m_models) {
            if (m.status == "loaded" || m.status == "loading") used += m.sizeGiB;
        }
        m_peakVramGiB = qMax(m_peakVramGiB, used);
    }

    ScriptedReply routeMultiModel(const QByteArray& method, const QByteArray& path, const QByteArray& body) {
        const QString model = QJsonDocument::fromJson(body).object()["model"].toString();
        if (method == "GET" && path.startsWith("/models")) {
            QJsonArray arr;
            for (const auto& m : m_models) {
                arr.append(QJsonObject{{"id", m.id},
                                       {"status", QJsonObject{{"value", m.status}}},
                                       {"meta", QJsonObject{{"size", m.sizeGiB * 1024.0 * 1024.0 * 1024.0},
                                                            {"n_ctx", m_contextTokens}}}});
            }
            return {200, QJsonDocument(QJsonObject{{"data", arr}}).toJson()};
        }
        if (method == "POST" && path == "/models/load") {
            FakeModel* m = findModel(model);
            if (!m) return {404, R"({"error":{"message":"model is not found"}})"};
            if (m->status != "unloaded") return {400, R"({"error":{"message":"model is already running"}})"};
            m_events << "load " + model;
            setStatus(model, "loading");
            QTimer::singleShot(m_loadDelayMs, &m_server, [this, model]() { setStatus(model, "loaded"); });
            return {200, R"({"success":true})"};
        }
        if (method == "POST" && path == "/models/unload") {
            m_events << "unload " + model;
            QTimer::singleShot(50, &m_server, [this, model]() { setStatus(model, "unloaded"); });
            return {200, R"({"success":true})"};
        }
        if (method == "POST" && path.startsWith("/v1/completions")) {
            if (modelStatus(model) == "sleeping") {
                m_events << "wake " + model;
                setStatus(model, "loaded");
            }
            return {200, R"({"choices":[{"text":"k"}]})"};
        }
        if (method == "POST" && path.startsWith("/v1/chat/completions")) {
            m_events << "chat " + model;
            if (modelStatus(model) != "loaded") ++m_chatsBeforeReady;
        }
        return route(method, path, body, /*multiModelHandled=*/true);
    }

    ScriptedReply route(const QByteArray& method, const QByteArray& path, const QByteArray& body,
                        bool multiModelHandled = false) {
        if (!m_models.isEmpty() && !multiModelHandled) return routeMultiModel(method, path, body);
        // Router-mode /models, or the OpenAI-style /v1/models of a hosted API
        if (method == "GET" && (path.startsWith("/models") || path.startsWith("/v1/models"))) {
            const QJsonObject model{{"id", m_modelId}, {"status", QJsonObject{{"value", "loaded"}}}};
            return {200, QJsonDocument(QJsonObject{{"data", QJsonArray{model}}}).toJson()};
        }
        if (method == "GET" && path.startsWith("/props")) {
            const QJsonObject props{{"default_generation_settings", QJsonObject{{"n_ctx", m_contextTokens}}}};
            return {200, QJsonDocument(props).toJson()};
        }
        if (method == "POST" && path.startsWith("/models/")) {
            return {200, R"({"success":true})"};
        }
        if (method == "POST" && path.startsWith("/v1/chat/completions")) {
            m_chatRequests.append(QJsonDocument::fromJson(body).object());
            if (m_chatReplies.isEmpty()) return {200, sse("(no scripted reply)")};
            return m_chatReplies.dequeue();
        }
        return {404, R"({"error":"not found"})"};
    }

    static void respond(QTcpSocket* sock, const ScriptedReply& reply) {
        const bool isSse = reply.status == 200 && reply.body.startsWith("data:");
        QByteArray out = "HTTP/1.1 " + QByteArray::number(reply.status) +
                         (reply.status == 200 ? " OK" : " Error") + "\r\n";
        out += isSse ? "Content-Type: text/event-stream\r\n" : "Content-Type: application/json\r\n";
        out += "Content-Length: " + QByteArray::number(reply.body.size()) + "\r\n";
        out += "Connection: close\r\n\r\n";
        out += reply.body;
        sock->write(out);
        sock->disconnectFromHost();
    }

    QTcpServer m_server;
    QString m_modelId;
    int m_contextTokens;
    QQueue<ScriptedReply> m_chatReplies;
    QVector<QJsonObject> m_chatRequests;
    QVector<RequestRecord> m_requestLog;
    QVector<FakeModel> m_models;
    int m_loadDelayMs = 100;
    QStringList m_events;
    int m_chatsBeforeReady = 0;
    double m_peakVramGiB = 0;
    QHash<QTcpSocket*, QByteArray> m_buffers;
};
