#include "core/router_client.h"
#include "core/tools.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <algorithm>

namespace agora {

LlamaRouterClient::LlamaRouterClient(QObject* parent)
    : QObject(parent),
      m_modelsDir(QDir::homePath() + "/models"),
      m_routerScriptPath(QDir::homePath() + "/llama-scripts/serve-router.sh") {}

QNetworkRequest LlamaRouterClient::makeRequest(const QString& path, int transferTimeoutMs) const {
    return makeRequestTo(m_baseUrl, path, m_apiKey, transferTimeoutMs);
}

QNetworkRequest LlamaRouterClient::makeRequestTo(const QString& baseUrl, const QString& path,
                                                 const QString& apiKey, int transferTimeoutMs) {
    QString base = baseUrl.trimmed();
    while (base.endsWith('/')) base.chop(1);
    const QUrl url(base + path);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setTransferTimeout(transferTimeoutMs);
    if (!apiKey.isEmpty()) {
        req.setRawHeader("Authorization", ("Bearer " + apiKey).toUtf8());
        // Anthropic's chat endpoint takes the Bearer key, but its /models wants these
        if (url.host().endsWith("anthropic.com")) {
            req.setRawHeader("x-api-key", apiKey.toUtf8());
            req.setRawHeader("anthropic-version", "2023-06-01");
        }
    }
    return req;
}

QString LlamaRouterClient::extractHttpErrorDetail(QNetworkReply* reply, const QByteArray& responseBody) {
    const int statusCode = reply ? reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 0;
    QString detail;
    if (!responseBody.trimmed().isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(responseBody);
        if (doc.isObject()) {
            const QJsonObject root = doc.object();
            if (root["error"].isObject()) {
                detail = root["error"].toObject()["message"].toString();
            } else if (root["error"].isString()) {
                detail = root["error"].toString();
            } else if (root["message"].isString()) {
                detail = root["message"].toString();
            }
        }
        if (detail.isEmpty()) {
            detail = QString::fromUtf8(responseBody.trimmed().left(400));
        }
    }

    if (detail.isEmpty() && reply) {
        detail = reply->errorString();
    }
    if (statusCode > 0) {
        return QStringLiteral("HTTP %1: %2").arg(QString::number(statusCode), detail);
    }
    return detail;
}

void LlamaRouterClient::sendRequest(const QString& method,
                                    const QString& path,
                                    const QByteArray& body,
                                    int timeoutMs,
                                    RequestCallback done) {
    sendRequest(method, makeRequest(path, timeoutMs), body, std::move(done));
}

void LlamaRouterClient::sendRequest(const QString& method,
                                    const QNetworkRequest& req,
                                    const QByteArray& body,
                                    RequestCallback done) {
    QNetworkReply* reply = (method == "POST") ? m_nam.post(req, body) : m_nam.get(req);
    m_pendingRequests.append(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply, done = std::move(done)]() {
        m_pendingRequests.removeAll(reply);
        const QByteArray payload = reply->readAll();
        const bool ok = (reply->error() == QNetworkReply::NoError);
        const QString err = ok ? QString() : extractHttpErrorDetail(reply, payload);
        reply->deleteLater();
        if (done) done(ok, payload, err);
    });
}

void LlamaRouterClient::abortPendingRequests() {
    // Each aborted reply still finishes, so callbacks run with ok=false
    const auto pending = m_pendingRequests;
    for (const QPointer<QNetworkReply>& reply : pending) {
        if (reply) reply->abort();
    }
}

QVector<ModelInfo> LlamaRouterClient::scanLocalGgufModels() const {
    QVector<ModelInfo> list;
    QDir dir(m_modelsDir);
    if (!dir.exists()) return list;

    const QFileInfoList files = dir.entryInfoList({"*.gguf"}, QDir::Files, QDir::Name);
    for (const QFileInfo& fi : files) {
        ModelInfo info;
        info.id = fi.completeBaseName();
        info.path = fi.absoluteFilePath();
        info.status = "unloaded";
        info.sizeGiB = static_cast<double>(fi.size()) / (1024.0 * 1024.0 * 1024.0);
        list.append(info);
    }
    return list;
}

QStringList LlamaRouterClient::planEvictionsForModel(const QVector<ModelInfo>& models,
                                                     const QString& targetModelId,
                                                     double vramBudgetGiB) {
    auto isResident = [](const QString& st) {
        return st == "loaded" || st == "sleeping" || st == "loading";
    };

    double targetSize = 2.0;
    for (const auto& m : models) {
        if (m.id == targetModelId) {
            if (isResident(m.status)) {
                return {};
            }
            if (m.sizeGiB > 0.01) targetSize = m.sizeGiB;
            break;
        }
    }

    QVector<ModelInfo> residents;
    double residentSum = 0.0;
    for (const auto& m : models) {
        if (m.id != targetModelId && isResident(m.status)) {
            residents.append(m);
            residentSum += (m.sizeGiB > 0.01 ? m.sizeGiB : 2.0);
        }
    }

    if (residents.isEmpty() || (residentSum + targetSize <= vramBudgetGiB)) {
        return {};
    }

    std::sort(residents.begin(), residents.end(), [](const ModelInfo& a, const ModelInfo& b) {
        return a.sizeGiB > b.sizeGiB;
    });

    QStringList toEvict;
    for (const auto& r : residents) {
        if (residentSum + targetSize <= vramBudgetGiB) break;
        toEvict.append(r.id);
        residentSum -= (r.sizeGiB > 0.01 ? r.sizeGiB : 2.0);
    }
    return toEvict;
}

void LlamaRouterClient::fetchModels(ModelsCallback done) {
    const QVector<ModelInfo> localModels = scanLocalGgufModels();

    sendRequest("GET", "/models", {}, 2500,
                [this, localModels, done = std::move(done)](bool ok, const QByteArray& payload, const QString& err) {
        const QJsonDocument doc = QJsonDocument::fromJson(payload);
        if (!ok || !doc.isObject()) {
            emit routerStatusChanged(false, localModels);
            if (done) done(false, localModels, ok ? QStringLiteral("Invalid JSON response from /models") : err);
            return;
        }

        QMap<QString, double> sizeMap;
        QMap<QString, QString> pathMap;
        for (const auto& lm : localModels) {
            sizeMap[lm.id] = lm.sizeGiB;
            pathMap[lm.id] = lm.path;
        }

        QVector<ModelInfo> merged;
        const QJsonArray dataArr = doc.object()["data"].toArray();
        for (const auto& v : dataArr) {
            const QJsonObject o = v.toObject();
            ModelInfo info;
            info.id = o["id"].toString();
            info.path = o["path"].toString(pathMap.value(info.id));
            const QJsonObject stObj = o["status"].toObject();
            info.status = stObj["value"].toString("unloaded");
            info.failed = stObj["failed"].toBool(false);
            if (sizeMap.contains(info.id)) {
                info.sizeGiB = sizeMap.value(info.id);
            } else if (!info.path.isEmpty() && QFileInfo::exists(info.path)) {
                info.sizeGiB = static_cast<double>(QFileInfo(info.path).size()) / (1024.0 * 1024.0 * 1024.0);
            }
            merged.append(info);
        }
        if (merged.isEmpty()) {
            merged = localModels;
        }

        emit routerStatusChanged(true, merged);
        if (done) done(true, merged, {});
    });
}

void LlamaRouterClient::unloadModel(const QString& modelId, std::function<void(bool, const QString&)> done) {
    m_contextLengthCache.remove(modelId); // may come back with different launch args
    QJsonObject body;
    body["model"] = modelId;
    sendRequest("POST", "/models/unload", QJsonDocument(body).toJson(QJsonDocument::Compact), 15000,
                [done = std::move(done)](bool ok, const QByteArray&, const QString& err) {
                    if (done) done(ok, err);
                });
}

void LlamaRouterClient::loadModel(const QString& modelId, std::function<void(bool, const QString&)> done) {
    QJsonObject body;
    body["model"] = modelId;
    sendRequest("POST", "/models/load", QJsonDocument(body).toJson(QJsonDocument::Compact), 120000,
                [done = std::move(done)](bool ok, const QByteArray&, const QString& err) {
                    if (done) done(ok, err);
                });
}

void LlamaRouterClient::ensureModelReady(const QString& targetModelId, double vramBudgetGiB, ReadyCallback done) {
    struct Chain {
        QString log;
        QStringList evictions;
        bool resident = false;
        QElapsedTimer clock;
    };
    auto chain = std::make_shared<Chain>();
    chain->clock.start();

    auto fail = [chain, done](const QString& msg) {
        chain->log += msg + "\n";
        done(false, chain->log.trimmed(), chain->clock.elapsed());
    };

    // Final step: refresh status and learn the context window (both best-effort)
    auto finishUp = [this, chain, targetModelId, done]() {
        fetchModels([this, chain, targetModelId, done](bool, const QVector<ModelInfo>&, const QString&) {
            fetchContextLength(targetModelId, [chain, done](int) {
                done(true, chain->log.trimmed(), chain->clock.elapsed());
            });
        });
    };

    auto loadIfNeeded = [this, chain, targetModelId, fail, finishUp]() {
        if (chain->resident) {
            finishUp();
            return;
        }
        loadModel(targetModelId, [targetModelId, fail, finishUp](bool ok, const QString& err) {
            if (!ok) {
                fail(QStringLiteral("Failed to load model %1: %2").arg(targetModelId, err));
                return;
            }
            finishUp();
        });
    };

    // Unload evictions one after another; a failed unload is logged but not fatal
    auto unloadNext = std::make_shared<std::function<void()>>();
    *unloadNext = [this, chain, targetModelId, vramBudgetGiB, loadIfNeeded, weakNext = std::weak_ptr(unloadNext)]() {
        if (chain->evictions.isEmpty()) {
            loadIfNeeded();
            return;
        }
        const QString evictId = chain->evictions.takeFirst();
        const QString msg = QStringLiteral("VRAM Manager: Unloading %1 to fit %2 within %3 GiB budget...")
                                .arg(evictId, targetModelId, QString::number(vramBudgetGiB, 'f', 1));
        emit vramSwapNotification(msg);
        chain->log += msg + "\n";
        // The pending request keeps the chain alive; the weak self-reference avoids a cycle
        unloadModel(evictId, [chain, evictId, self = weakNext.lock()](bool ok, const QString& err) {
            if (!ok) chain->log += QStringLiteral("Unload of %1 failed: %2\n").arg(evictId, err);
            (*self)();
        });
    };

    fetchModels([this, chain, targetModelId, vramBudgetGiB, fail, unloadNext](
                    bool online, const QVector<ModelInfo>& models, const QString& err) {
        if (!online) {
            fail(QStringLiteral("Router offline at %1 (%2)").arg(m_baseUrl, err));
            return;
        }
        chain->evictions = planEvictionsForModel(models, targetModelId, vramBudgetGiB);
        for (const auto& m : models) {
            if (m.id == targetModelId && (m.status == "loaded" || m.status == "sleeping" || m.status == "loading")) {
                chain->resident = true;
                break;
            }
        }
        (*unloadNext)();
    });
}

int LlamaRouterClient::parseContextLengthFromProps(const QByteArray& propsJson) {
    const QJsonObject root = QJsonDocument::fromJson(propsJson).object();
    const int nested = root["default_generation_settings"].toObject()["n_ctx"].toInt(0);
    return nested > 0 ? nested : root["n_ctx"].toInt(0);
}

void LlamaRouterClient::fetchContextLength(const QString& modelId, std::function<void(int)> done) {
    if (const auto it = m_contextLengthCache.constFind(modelId); it != m_contextLengthCache.constEnd()) {
        const int cached = it.value();
        QTimer::singleShot(0, this, [done, cached]() { done(cached); });
        return;
    }
    const QString path = "/props?model=" + QString::fromUtf8(QUrl::toPercentEncoding(modelId));
    sendRequest("GET", path, {}, 3000, [this, modelId, done](bool ok, const QByteArray& payload, const QString&) {
        if (!ok) {
            done(0); // not cached: the model may simply not be up yet
            return;
        }
        const int nCtx = parseContextLengthFromProps(payload);
        m_contextLengthCache.insert(modelId, nCtx);
        done(nCtx);
    });
}

int LlamaRouterClient::cachedContextLength(const QString& modelId) const {
    return m_contextLengthCache.value(modelId, 0);
}

bool LlamaRouterClient::startRouterScript(QString* errorOut) {
    if (m_routerScriptPath.isEmpty() || !QFileInfo::exists(m_routerScriptPath)) {
        if (errorOut) *errorOut = "Router script not found at " + m_routerScriptPath;
        return false;
    }
    const bool started = QProcess::startDetached("/bin/bash", {m_routerScriptPath});
    if (!started && errorOut) {
        *errorOut = "Failed to start " + m_routerScriptPath;
    }
    return started;
}

void LlamaRouterClient::resetStreamState() {
    m_sseBuffer.clear();
    m_rawHttpErrorBuffer.clear();
    m_accumContent.clear();
    m_accumReasoning.clear();
    m_partialToolCalls.clear();
    m_lastStats = {};
    m_abortedByUser = false;
    m_receivedDoneMarker = false;
}

void LlamaRouterClient::feedSseDataForTesting(const QByteArray& rawSse, bool flushAll) {
    m_sseBuffer.append(rawSse);
    processSseBuffer(flushAll);
}

QVector<ToolCall> LlamaRouterClient::finalizedToolCalls() const {
    QVector<ToolCall> finalCalls;
    for (auto it = m_partialToolCalls.constBegin(); it != m_partialToolCalls.constEnd(); ++it) {
        if (!it.value().name.isEmpty()) {
            ToolCall tc;
            tc.id = it.value().id.isEmpty()
                        ? ("call_" + QUuid::createUuid().toString(QUuid::Id128).left(10))
                        : it.value().id;
            tc.name = it.value().name;
            tc.argumentsJson = it.value().arguments.trimmed().isEmpty()
                                   ? QStringLiteral("{}")
                                   : it.value().arguments;
            finalCalls.append(tc);
        }
    }
    if (finalCalls.isEmpty()) {
        finalCalls = ToolEngine::extractFallbackToolCalls(m_accumContent, nullptr);
    }
    return finalCalls;
}

void LlamaRouterClient::startChatStream(const QString& modelId,
                                        const QJsonArray& messages,
                                        const QJsonArray& tools,
                                        double temperature,
                                        const std::optional<ChatEndpoint>& external) {
    abortActiveStream();
    resetStreamState();

    QJsonObject payload;
    payload["model"] = modelId;
    payload["messages"] = external ? sanitizeMessagesForExternal(messages) : messages;
    payload["stream"] = true;
    if (!external || !external->omitTemperature) {
        payload["temperature"] = temperature;
    }
    payload["stream_options"] = QJsonObject{{"include_usage", true}};
    if (!tools.isEmpty()) {
        payload["tools"] = tools;
        payload["tool_choice"] = "auto";
    }

    QNetworkRequest req = external
                              ? makeRequestTo(external->baseUrl, "/chat/completions", external->apiKey,
                                              m_streamIdleTimeoutMs)
                              : makeRequest("/v1/chat/completions", m_streamIdleTimeoutMs);
    req.setRawHeader("Accept", "text/event-stream");

    m_activeReply = m_nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));

    QObject::connect(m_activeReply, &QNetworkReply::readyRead, this, [this]() {
        if (!m_activeReply) return;
        const QByteArray chunk = m_activeReply->readAll();
        const int statusCode = m_activeReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode >= 400) {
            m_rawHttpErrorBuffer.append(chunk);
            return;
        }
        m_sseBuffer.append(chunk);
        processSseBuffer(false);
    });

    QObject::connect(m_activeReply, &QNetworkReply::finished, this, [this]() {
        if (!m_activeReply) return;
        QNetworkReply* reply = m_activeReply;
        m_activeReply = nullptr;

        if (reply->isOpen() && reply->isReadable()) {
            const QByteArray rem = reply->readAll();
            const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (statusCode >= 400) {
                m_rawHttpErrorBuffer.append(rem);
            } else {
                m_sseBuffer.append(rem);
            }
        }
        processSseBuffer(true);

        const auto netErr = reply->error();
        const QString errDetail = extractHttpErrorDetail(reply, m_rawHttpErrorBuffer.isEmpty() ? m_sseBuffer : m_rawHttpErrorBuffer);
        reply->deleteLater();

        if (m_abortedByUser) {
            return;
        }

        // Report error both when HTTP fails upfront AND when a stream breaks mid-response before [DONE]
        if (netErr != QNetworkReply::NoError && !m_receivedDoneMarker) {
            emit streamError(errDetail);
            return;
        }

        QVector<ToolCall> finalCalls;
        for (auto it = m_partialToolCalls.constBegin(); it != m_partialToolCalls.constEnd(); ++it) {
            if (!it.value().name.isEmpty()) {
                ToolCall tc;
                tc.id = it.value().id.isEmpty()
                            ? ("call_" + QUuid::createUuid().toString(QUuid::Id128).left(10))
                            : it.value().id;
                tc.name = it.value().name;
                tc.argumentsJson = it.value().arguments.trimmed().isEmpty()
                                       ? QStringLiteral("{}")
                                       : it.value().arguments;
                finalCalls.append(tc);
            }
        }

        QString cleanedContent = m_accumContent;
        if (finalCalls.isEmpty()) {
            finalCalls = ToolEngine::extractFallbackToolCalls(m_accumContent, &cleanedContent);
        }

        emit streamFinished(cleanedContent, m_accumReasoning, finalCalls);
    });
}

void LlamaRouterClient::fetchProviderModels(const ChatEndpoint& endpoint, ProviderModelsCallback done) {
    sendRequest("GET", makeRequestTo(endpoint.baseUrl, "/models", endpoint.apiKey, 20000), {},
                [done = std::move(done)](bool ok, const QByteArray& payload, const QString& error) {
        if (!done) return;
        if (!ok) {
            done(false, {}, error);
            return;
        }
        const QStringList models = parseProviderModels(payload);
        if (models.isEmpty()) {
            done(false, {}, QStringLiteral("The server answered but listed no models."));
            return;
        }
        done(true, models, {});
    });
}

QStringList LlamaRouterClient::parseProviderModels(const QByteArray& modelsJson) {
    const QJsonObject root = QJsonDocument::fromJson(modelsJson).object();
    // OpenAI-style {"data":[{"id":...}]}; Ollama's native {"models":[{"name":...}]} as a courtesy
    QJsonArray arr = root["data"].toArray();
    if (arr.isEmpty()) arr = root["models"].toArray();
    QStringList ids;
    for (const auto& v : arr) {
        const QJsonObject m = v.toObject();
        QString id = m["id"].toString();
        if (id.isEmpty()) id = m["name"].toString();
        if (id.startsWith("models/")) id = id.mid(7); // Gemini prefixes ids
        if (!id.isEmpty() && !ids.contains(id)) ids.append(id);
    }
    ids.sort(Qt::CaseInsensitive);
    return ids;
}

QJsonArray LlamaRouterClient::sanitizeMessagesForExternal(const QJsonArray& messages) {
    QJsonArray out;
    for (const auto& v : messages) {
        QJsonObject m = v.toObject();
        if (m["role"].toString() == "tool") m.remove("name");
        out.append(m);
    }
    return out;
}

void LlamaRouterClient::abortActiveStream() {
    if (m_activeReply) {
        m_abortedByUser = true;
        QNetworkReply* reply = m_activeReply;
        m_activeReply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
}

void LlamaRouterClient::processSseBuffer(bool flushAll) {
    while (true) {
        int newlineIdx = m_sseBuffer.indexOf('\n');
        if (newlineIdx < 0) {
            if (flushAll && !m_sseBuffer.trimmed().isEmpty()) {
                QByteArray line = m_sseBuffer.trimmed();
                m_sseBuffer.clear();
                if (line.startsWith("data:")) {
                    handleSsePayload(line.mid(5).trimmed());
                }
            }
            break;
        }
        QByteArray line = m_sseBuffer.left(newlineIdx).trimmed();
        m_sseBuffer.remove(0, newlineIdx + 1);
        if (line.startsWith("data:")) {
            handleSsePayload(line.mid(5).trimmed());
        }
    }
}

void LlamaRouterClient::handleSsePayload(const QByteArray& jsonBytes) {
    if (jsonBytes.isEmpty()) return;
    if (jsonBytes == "[DONE]") {
        m_receivedDoneMarker = true;
        return;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;

    // Usage/timings arrive on the final chunk (which may have no choices at all)
    const QJsonObject root = doc.object();
    if (root["usage"].isObject() || root["timings"].isObject()) {
        const QJsonObject usage = root["usage"].toObject();
        const QJsonObject timings = root["timings"].toObject();
        // timings.prompt_n excludes the cached prefix, so prefer usage for the total
        const int promptTotal = usage["prompt_tokens"].toInt(0);
        m_lastStats.promptTokens = promptTotal > 0
                                       ? promptTotal
                                       : timings["prompt_n"].toInt(0) + timings["cache_n"].toInt(0);
        m_lastStats.completionTokens = usage["completion_tokens"].toInt(timings["predicted_n"].toInt(0));
        if (!timings.isEmpty()) {
            m_lastStats.promptMs = timings["prompt_ms"].toDouble();
            m_lastStats.generationMs = timings["predicted_ms"].toDouble();
            m_lastStats.tokensPerSecond = timings["predicted_per_second"].toDouble();
        }
    }

    const QJsonArray choices = root["choices"].toArray();
    if (choices.isEmpty()) return;

    const QJsonObject choice0 = choices[0].toObject();
    if (!choice0["finish_reason"].isNull() && !choice0["finish_reason"].toString().isEmpty()) {
        m_receivedDoneMarker = true;
    }

    const QJsonObject delta = choice0["delta"].toObject();

    // reasoning_content: llama.cpp, DeepSeek; reasoning: OpenRouter, Ollama
    QString rDelta = delta["reasoning_content"].toString();
    if (rDelta.isEmpty()) rDelta = delta["reasoning"].toString();
    if (!rDelta.isEmpty()) {
        m_accumReasoning += rDelta;
        emit streamReasoningDelta(rDelta);
    }

    const QString cDelta = delta["content"].toString();
    if (!cDelta.isEmpty()) {
        m_accumContent += cDelta;
        emit streamContentDelta(cDelta);
    }

    if (delta.contains("tool_calls")) {
        const QJsonArray tcArr = delta["tool_calls"].toArray();
        for (int i = 0; i < tcArr.size(); ++i) {
            const QJsonObject tcObj = tcArr[i].toObject();
            const int idx = tcObj["index"].toInt(i);
            PartialToolCall& ptc = m_partialToolCalls[idx];
            if (tcObj.contains("id") && !tcObj["id"].toString().isEmpty()) {
                ptc.id = tcObj["id"].toString();
            }
            const QJsonObject fnObj = tcObj["function"].toObject();
            if (fnObj.contains("name") && !fnObj["name"].toString().isEmpty()) {
                ptc.name += fnObj["name"].toString();
            }
            if (fnObj.contains("arguments")) {
                ptc.arguments += fnObj["arguments"].toString();
            }
        }
    }
}

} // namespace agora
