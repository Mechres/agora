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
#include <climits>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

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
    ++m_abortGeneration;
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

std::optional<QStringList> LlamaRouterClient::planEvictions(const QVector<ModelInfo>& models,
                                                           const QString& targetModelId,
                                                           double vramBudgetGiB,
                                                           const QStringList& upcoming,
                                                           const QSet<QString>& keep) {
    auto sizeOf = [](const ModelInfo& m) { return m.sizeGiB > 0.01 ? m.sizeGiB : 2.0; };

    double targetSize = 2.0;
    for (const auto& m : models) {
        if (m.id == targetModelId) {
            if (occupiesVram(m.status)) return QStringList{};
            targetSize = sizeOf(m);
            break;
        }
    }

    QVector<ModelInfo> candidates;
    double residentSum = 0.0;
    for (const auto& m : models) {
        if (m.id == targetModelId || !occupiesVram(m.status)) continue;
        residentSum += sizeOf(m);
        if (!keep.contains(m.id)) candidates.append(m);
    }
    if (residentSum + targetSize <= vramBudgetGiB) return QStringList{};

    // Furthest next use first (never-again counts as furthest), then largest
    auto nextUse = [&upcoming](const QString& id) {
        const qsizetype idx = upcoming.indexOf(id);
        return idx < 0 ? INT_MAX : static_cast<int>(idx);
    };
    std::sort(candidates.begin(), candidates.end(), [&](const ModelInfo& a, const ModelInfo& b) {
        const int ua = nextUse(a.id);
        const int ub = nextUse(b.id);
        if (ua != ub) return ua > ub;
        return a.sizeGiB > b.sizeGiB;
    });

    QStringList toEvict;
    for (const auto& c : candidates) {
        if (residentSum + targetSize <= vramBudgetGiB) break;
        toEvict.append(c.id);
        residentSum -= sizeOf(c);
    }
    if (residentSum + targetSize > vramBudgetGiB && !keep.isEmpty()) {
        return std::nullopt; // only evicting a kept model would make room
    }
    return toEvict; // with nothing kept, evicting everything is the best we can do
}

QStringList LlamaRouterClient::planEvictionsForModel(const QVector<ModelInfo>& models,
                                                     const QString& targetModelId,
                                                     double vramBudgetGiB) {
    return planEvictions(models, targetModelId, vramBudgetGiB).value_or(QStringList{});
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
            const double metaBytes = o["meta"].toObject()["size"].toDouble(0);
            if (sizeMap.contains(info.id)) {
                info.sizeGiB = sizeMap.value(info.id);
            } else if (metaBytes > 0) {
                info.sizeGiB = metaBytes / (1024.0 * 1024.0 * 1024.0);
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

void LlamaRouterClient::waitForModels(std::function<bool(const QVector<ModelInfo>&)> ready, int timeoutMs,
                                      std::function<void(bool, const QVector<ModelInfo>&)> done) {
    struct Wait {
        QElapsedTimer clock;
        quint64 generation = 0;
    };
    auto wait = std::make_shared<Wait>();
    wait->clock.start();
    wait->generation = m_abortGeneration;

    auto poll = std::make_shared<std::function<void()>>();
    *poll = [this, wait, timeoutMs, ready, done, weakPoll = std::weak_ptr(poll)]() {
        // The pending request, then the timer, hold the only strong reference; the function
        // itself keeps a weak one to avoid a cycle
        fetchModels([this, wait, timeoutMs, ready, done, self = weakPoll.lock()](
                        bool online, const QVector<ModelInfo>& models, const QString&) {
            if (wait->generation != m_abortGeneration) {
                done(false, models); // aborted
                return;
            }
            if (online && ready(models)) {
                done(true, models);
                return;
            }
            if (wait->clock.elapsed() >= timeoutMs) {
                done(false, models);
                return;
            }
            QTimer::singleShot(m_statusPollIntervalMs, this, [self]() { (*self)(); });
        });
    };
    (*poll)();
}

void LlamaRouterClient::unloadAndWait(const QStringList& modelIds, std::function<void(const QString&)> done) {
    if (modelIds.isEmpty()) {
        QTimer::singleShot(0, this, [done]() { done({}); });
        return;
    }
    auto log = std::make_shared<QString>();
    auto remaining = std::make_shared<int>(modelIds.size());
    for (const QString& id : modelIds) {
        unloadModel(id, [this, id, log, remaining, modelIds, done](bool ok, const QString& err) {
            if (!ok) *log += QStringLiteral("Unload of %1 failed: %2\n").arg(id, err);
            if (--*remaining > 0) return;
            // Unloading only asks the process to exit; loading the next model while the old one
            // still holds VRAM would make llama.cpp's auto-fit push layers onto the CPU
            waitForModels([modelIds](const QVector<ModelInfo>& models) {
                for (const auto& m : models) {
                    if (modelIds.contains(m.id) && occupiesVram(m.status)) return false;
                }
                return true;
            }, 30000, [log, done](bool ok, const QVector<ModelInfo>&) {
                if (!ok) *log += QStringLiteral("Evicted models were still releasing memory after 30 s.\n");
                done(*log);
            });
        });
    }
}

void LlamaRouterClient::loadAndWait(const QString& modelId, std::function<void(bool, const QString&)> done) {
    loadModel(modelId, [this, modelId, done](bool ok, const QString& err) {
        // "already running" means someone else (e.g. a preload) started it; just wait
        if (!ok && !err.contains("already running")) {
            done(false, err);
            return;
        }
        waitForModels([modelId](const QVector<ModelInfo>& models) {
            for (const auto& m : models) {
                if (m.id == modelId) return m.status != "loading";
            }
            return true;
        }, m_modelLoadTimeoutMs, [this, modelId, done](bool ok, const QVector<ModelInfo>& models) {
            if (!ok) {
                done(false, QStringLiteral("not ready after %1 s").arg(m_modelLoadTimeoutMs / 1000));
                return;
            }
            for (const auto& m : models) {
                if (m.id != modelId) continue;
                if (m.status == "loaded" || m.status == "sleeping") {
                    done(true, {});
                } else {
                    done(false, m.failed ? QStringLiteral("the model process failed to start (see the router log)")
                                         : QStringLiteral("status is '%1'").arg(m.status));
                }
                return;
            }
            done(true, {});
        });
    });
}

void LlamaRouterClient::wakeModel(const QString& modelId, std::function<void(bool, const QString&)> done) {
    QJsonObject body;
    body["model"] = modelId;
    body["prompt"] = "ok";
    body["max_tokens"] = 1;
    sendRequest("POST", "/v1/completions", QJsonDocument(body).toJson(QJsonDocument::Compact), m_modelLoadTimeoutMs,
                [done](bool ok, const QByteArray&, const QString& err) { done(ok, err); });
}

void LlamaRouterClient::warmFileCache(const QString& path) {
    if (path.isEmpty()) return;
    const qint64 size = QFileInfo(path).size();
    if (size <= 0) return;
    // Don't push other programs' data out of RAM for a file that wouldn't stay cached anyway
    QFile meminfo("/proc/meminfo");
    if (meminfo.open(QIODevice::ReadOnly)) {
        for (const QByteArray& line : meminfo.readAll().split('\n')) {
            if (!line.startsWith("MemAvailable:")) continue;
            const qint64 availableBytes = line.mid(13).trimmed().split(' ').value(0).toLongLong() * 1024;
            if (availableBytes < size * 2) return;
        }
    }
    const QByteArray nativePath = QFile::encodeName(path);
    // posix_fadvise(WILLNEED) reads synchronously in large chunks, so keep it off the GUI thread
    std::thread([nativePath]() {
        const int fd = ::open(nativePath.constData(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return;
        ::posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
        ::close(fd);
    }).detach();
}

void LlamaRouterClient::ensureModelReady(const QString& targetModelId, double vramBudgetGiB, ReadyCallback done,
                                         const QStringList& upcoming) {
    struct Chain {
        QString log;
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
        fetchContextLength(targetModelId, [chain, done](int) {
            done(true, chain->log.trimmed(), chain->clock.elapsed());
        });
    };

    fetchModels([this, chain, targetModelId, vramBudgetGiB, upcoming, fail, finishUp](
                    bool online, const QVector<ModelInfo>& models, const QString& err) {
        if (!online) {
            fail(QStringLiteral("Router offline at %1 (%2)").arg(m_baseUrl, err));
            return;
        }
        QString status = "unloaded";
        for (const auto& m : models) {
            if (m.id == targetModelId) status = m.status;
        }
        if (status == "loaded") {
            finishUp();
            return;
        }

        auto bringUp = [this, chain, targetModelId, status, fail, finishUp]() {
            auto onUp = [targetModelId, fail, finishUp](bool ok, const QString& error) {
                if (!ok) {
                    fail(QStringLiteral("Failed to load model %1: %2").arg(targetModelId, error));
                    return;
                }
                finishUp();
            };
            if (status == "sleeping") {
                emit vramSwapNotification(QStringLiteral("Waking %1...").arg(targetModelId));
                wakeModel(targetModelId, onUp);
            } else {
                if (status != "loading") {
                    emit vramSwapNotification(QStringLiteral("Loading %1...").arg(targetModelId));
                }
                loadAndWait(targetModelId, onUp);
            }
        };

        // A loading model (e.g. preloaded) already has its room; a sleeping one needs it back
        const QStringList evictions = (status == "loading")
            ? QStringList{}
            : planEvictions(models, targetModelId, vramBudgetGiB, upcoming).value_or(QStringList{});
        if (evictions.isEmpty()) {
            bringUp();
            return;
        }
        const QString msg = QStringLiteral("VRAM Manager: Unloading %1 to fit %2 within %3 GiB budget...")
                                .arg(evictions.join(", "), targetModelId, QString::number(vramBudgetGiB, 'f', 1));
        emit vramSwapNotification(msg);
        chain->log += msg + "\n";
        const quint64 generation = m_abortGeneration;
        unloadAndWait(evictions, [this, chain, bringUp, fail, generation](const QString& unloadLog) {
            chain->log += unloadLog;
            if (generation != m_abortGeneration) {
                fail(QStringLiteral("Cancelled."));
                return;
            }
            bringUp();
        });
    });
}

void LlamaRouterClient::prefetchModel(const QString& targetModelId, double vramBudgetGiB, const QStringList& upcoming,
                                      const QSet<QString>& keep, std::function<void(const QString&)> done) {
    fetchModels([this, targetModelId, vramBudgetGiB, upcoming, keep, done](
                    bool online, const QVector<ModelInfo>& models, const QString&) {
        if (!online) {
            done(QStringLiteral("router offline"));
            return;
        }
        ModelInfo target;
        target.status = "unloaded";
        for (const auto& m : models) {
            if (m.id == targetModelId) target = m;
        }
        if (occupiesVram(target.status)) {
            done(QStringLiteral("already loaded"));
            return;
        }
        const auto plan = planEvictions(models, targetModelId, vramBudgetGiB, upcoming, keep);
        if (!plan) {
            QString path = target.path;
            if (path.isEmpty()) {
                for (const auto& local : scanLocalGgufModels()) {
                    if (local.id == targetModelId) path = local.path;
                }
            }
            warmFileCache(path);
            done(path.isEmpty() ? QStringLiteral("doesn't fit next to the current model")
                                : QStringLiteral("doesn't fit next to the current model; reading its file into RAM"));
            return;
        }
        const bool sleeping = (target.status == "sleeping");
        const quint64 generation = m_abortGeneration;
        unloadAndWait(*plan, [this, targetModelId, sleeping, plan, done, generation](const QString&) {
            if (generation != m_abortGeneration) {
                done(QStringLiteral("cancelled"));
                return;
            }
            const QString evicted = plan->isEmpty() ? QString() : QStringLiteral(" (unloaded %1)").arg(plan->join(", "));
            if (sleeping) {
                wakeModel(targetModelId, [](bool, const QString&) {});
                done("waking" + evicted);
            } else {
                loadModel(targetModelId, [done, evicted](bool ok, const QString& err) {
                    done(ok ? "loading" + evicted : "load failed: " + err);
                });
            }
        });
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
