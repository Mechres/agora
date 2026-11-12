#include "core/router_client.h"
#include "core/tools.h"

#include <QDir>
#include <QEventLoop>
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
    QString base = m_baseUrl;
    while (base.endsWith('/')) base.chop(1);
    QNetworkRequest req(QUrl(base + path));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setTransferTimeout(transferTimeoutMs);
    if (!m_apiKey.isEmpty()) {
        req.setRawHeader("Authorization", ("Bearer " + m_apiKey).toUtf8());
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

bool LlamaRouterClient::sendBlockingRequest(const QString& method,
                                            const QString& path,
                                            const QByteArray& body,
                                            int timeoutMs,
                                            QByteArray* responseOut,
                                            QString* errorOut) {
    QNetworkRequest req = makeRequest(path, timeoutMs);
    QNetworkReply* reply = nullptr;
    if (method == "GET") {
        reply = m_nam.get(req);
    } else if (method == "POST") {
        reply = m_nam.post(req, body);
    } else {
        if (errorOut) *errorOut = "Unsupported HTTP method: " + method;
        return false;
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        reply->abort();
        loop.quit();
    });
    timer.start(qMax(500, timeoutMs));
    loop.exec();

    QByteArray payload;
    if (reply->isOpen() && reply->isReadable()) {
        payload = reply->readAll();
    }
    if (responseOut) *responseOut = payload;

    const bool ok = (reply->error() == QNetworkReply::NoError);
    if (!ok && errorOut) {
        *errorOut = extractHttpErrorDetail(reply, payload);
    }
    reply->deleteLater();
    return ok;
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

QVector<ModelInfo> LlamaRouterClient::fetchModelsBlocking(bool* routerOnlineOut, QString* errorOut) {
    QVector<ModelInfo> localModels = scanLocalGgufModels();
    QMap<QString, double> sizeMap;
    QMap<QString, QString> pathMap;
    for (const auto& lm : localModels) {
        sizeMap[lm.id] = lm.sizeGiB;
        pathMap[lm.id] = lm.path;
    }

    QByteArray payload;
    QString err;
    if (!sendBlockingRequest("GET", "/models", {}, 2500, &payload, &err)) {
        if (errorOut) *errorOut = err;
        if (routerOnlineOut) *routerOnlineOut = false;
        emit routerStatusChanged(false, localModels);
        return localModels;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject()) {
        if (errorOut) *errorOut = "Invalid JSON response from /models";
        if (routerOnlineOut) *routerOnlineOut = false;
        emit routerStatusChanged(false, localModels);
        return localModels;
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

    if (routerOnlineOut) *routerOnlineOut = true;
    emit routerStatusChanged(true, merged);
    return merged;
}

bool LlamaRouterClient::unloadModelBlocking(const QString& modelId, QString* errorOut) {
    m_contextLengthCache.remove(modelId); // may come back with different launch args
    QJsonObject body;
    body["model"] = modelId;
    return sendBlockingRequest(
        "POST",
        "/models/unload",
        QJsonDocument(body).toJson(QJsonDocument::Compact),
        15000,
        nullptr,
        errorOut);
}

bool LlamaRouterClient::loadModelBlocking(const QString& modelId, QString* errorOut) {
    QJsonObject body;
    body["model"] = modelId;
    return sendBlockingRequest(
        "POST",
        "/models/load",
        QJsonDocument(body).toJson(QJsonDocument::Compact),
        60000,
        nullptr,
        errorOut);
}

bool LlamaRouterClient::ensureModelReady(const QString& targetModelId, double vramBudgetGiB, QString* statusLogOut) {
    bool online = false;
    QString err;
    const QVector<ModelInfo> models = fetchModelsBlocking(&online, &err);
    if (!online) {
        if (statusLogOut) *statusLogOut = QStringLiteral("Router offline at %1 (%2)").arg(m_baseUrl, err);
        return false;
    }

    const QStringList evictions = planEvictionsForModel(models, targetModelId, vramBudgetGiB);
    for (const QString& evictId : evictions) {
        const QString msg = QStringLiteral("VRAM Manager: Unloading %1 to fit %2 within %3 GiB budget...")
                                .arg(evictId, targetModelId, QString::number(vramBudgetGiB, 'f', 1));
        emit vramSwapNotification(msg);
        if (statusLogOut) *statusLogOut += msg + "\n";
        unloadModelBlocking(evictId);
    }

    bool alreadyResident = false;
    for (const auto& m : models) {
        if (m.id == targetModelId && (m.status == "loaded" || m.status == "sleeping" || m.status == "loading")) {
            alreadyResident = true;
            break;
        }
    }

    if (!alreadyResident) {
        QString loadErr;
        if (!loadModelBlocking(targetModelId, &loadErr)) {
            if (statusLogOut) *statusLogOut += QStringLiteral("Failed to load model %1: %2\n").arg(targetModelId, loadErr);
            return false;
        }
    }

    fetchModelsBlocking();
    return true;
}

int LlamaRouterClient::parseContextLengthFromProps(const QByteArray& propsJson) {
    const QJsonObject root = QJsonDocument::fromJson(propsJson).object();
    const int nested = root["default_generation_settings"].toObject()["n_ctx"].toInt(0);
    return nested > 0 ? nested : root["n_ctx"].toInt(0);
}

int LlamaRouterClient::contextLengthForModel(const QString& modelId) {
    if (const auto it = m_contextLengthCache.constFind(modelId); it != m_contextLengthCache.constEnd()) {
        return it.value();
    }
    QByteArray payload;
    const QString path = "/props?model=" + QString::fromUtf8(QUrl::toPercentEncoding(modelId));
    if (!sendBlockingRequest("GET", path, {}, 3000, &payload, nullptr)) {
        return 0; // not cached: the model may simply not be up yet
    }
    const int nCtx = parseContextLengthFromProps(payload);
    m_contextLengthCache.insert(modelId, nCtx);
    return nCtx;
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
                                        double temperature) {
    abortActiveStream();
    resetStreamState();

    QJsonObject payload;
    payload["model"] = modelId;
    payload["messages"] = messages;
    payload["stream"] = true;
    payload["temperature"] = temperature;
    if (!tools.isEmpty()) {
        payload["tools"] = tools;
        payload["tool_choice"] = "auto";
    }

    QNetworkRequest req = makeRequest("/v1/chat/completions", m_streamIdleTimeoutMs);
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

    const QJsonArray choices = doc.object()["choices"].toArray();
    if (choices.isEmpty()) return;

    const QJsonObject choice0 = choices[0].toObject();
    if (!choice0["finish_reason"].isNull() && !choice0["finish_reason"].toString().isEmpty()) {
        m_receivedDoneMarker = true;
    }

    const QJsonObject delta = choice0["delta"].toObject();

    const QString rDelta = delta["reasoning_content"].toString();
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
