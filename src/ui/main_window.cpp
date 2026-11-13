#include "ui/main_window.h"

#include <QCheckBox>
#include <QColor>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <algorithm>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QPainter>
#include <QPixmap>
#include <QFormLayout>
#include <QFrame>
#include <QJsonArray>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

namespace agora {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Agora");
    resize(1540, 920);

    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dataDir);
    m_store = std::make_unique<SessionStore>(dataDir + "/sessions.db");
    m_store->initialize();

    // Reopen the room from last time when it still exists; otherwise start a fresh one
    std::optional<RoomSession> restored;
    const QString lastSessionId = QSettings().value("room/lastSessionId").toString();
    if (!lastSessionId.isEmpty()) {
        restored = m_store->loadSession(lastSessionId);
        if (restored && (restored->workspacePath.isEmpty() || !QDir(restored->workspacePath).exists())) {
            restored.reset();
        }
    }
    const QString defaultWs = restored ? restored->workspacePath : newWorkspacePath();
    QDir().mkpath(defaultWs);

    m_sandbox = new BwrapSandbox(defaultWs, this);
    m_sandbox->ensureWorkspaceInitialized();

    m_toolEngine = new ToolEngine(m_sandbox, this);
    m_checkpoints = new CheckpointStore(m_sandbox, this);
    m_router = new LlamaRouterClient(this);
    m_orchestrator = new Orchestrator(m_router, m_toolEngine, this);
    m_orchestrator->setCheckpointStore(m_checkpoints);
    loadAppSettings();

    if (restored) {
        m_sandbox->setAllowNetwork(restored->allowSandboxNetwork);
        m_orchestrator->setSession(*restored);
    } else {
        QSettings settings;
        RoomSession& fresh = m_orchestrator->session();
        fresh.workspacePath = defaultWs;
        fresh.title = "New Agora Debate";
        fresh.maxRounds = settings.value("room/maxRounds", fresh.maxRounds).toInt();
        fresh.vramBudgetGiB = settings.value("room/vramBudgetGiB", fresh.vramBudgetGiB).toDouble();
        fresh.mode = static_cast<DebateMode>(settings.value("room/mode", static_cast<int>(fresh.mode)).toInt());
        fresh.participants = loadSavedRoster();
    }

    m_fsWatcher = new QFileSystemWatcher(this);
    m_fsWatcher->addPath(defaultWs);

    setupUi();
    applyDarkTheme();

    connect(m_router, &LlamaRouterClient::routerStatusChanged, this,
            [this](bool online, const QVector<ModelInfo>& models) {
                m_availableModels = models;
                int loadedCount = 0;
                double loadedGiB = 0.0;
                for (const auto& m : models) {
                    if (m.status == "loaded" || m.status == "sleeping") {
                        loadedCount++;
                        loadedGiB += m.sizeGiB;
                    }
                }
                if (online) {
                    m_routerBadge->setText(QStringLiteral("● Router online · %1 models · %2 loaded (%3 GiB)")
                                               .arg(QString::number(models.size()),
                                                    QString::number(loadedCount),
                                                    QString::number(loadedGiB, 'f', 1)));
                    m_routerBadge->setProperty("state", "online");
                } else {
                    m_routerBadge->setText(QStringLiteral("○ Router offline · %1 local GGUFs")
                                               .arg(QString::number(models.size())));
                    m_routerBadge->setProperty("state", "offline");
                }
                m_routerBadge->style()->unpolish(m_routerBadge);
                m_routerBadge->style()->polish(m_routerBadge);
                refreshParticipantsList();
            });

    connect(m_router, &LlamaRouterClient::vramSwapNotification, this, [this](const QString& msg) {
        m_transcriptPane->setStatusText(msg);
        statusBar()->showMessage(msg, 6000);
    });

    connect(m_toolEngine, &ToolEngine::workspaceModified, this, [this](const QString&) {
        m_workspacePane->refreshViews();
    });
    connect(m_fsWatcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString&) {
        m_workspacePane->refreshViews();
    });
    connect(m_fsWatcher, &QFileSystemWatcher::fileChanged, this, [this](const QString&) {
        m_workspacePane->refreshViews();
    });

    connect(m_orchestrator, &Orchestrator::messageAdded, this, [this](int idx, const ChatMessage& msg) {
        m_transcriptPane->onMessageAdded(idx, msg, m_orchestrator->session().messages);
    });
    connect(m_orchestrator, &Orchestrator::messageUpdated, this, [this](int idx, const ChatMessage& msg) {
        m_transcriptPane->onMessageUpdated(idx, msg, m_orchestrator->session().messages);
    });
    connect(m_orchestrator, &Orchestrator::statusNotification, this, [this](const QString& text) {
        m_transcriptPane->setStatusText(text);
        statusBar()->showMessage(text, 5000);
    });
    connect(m_orchestrator, &Orchestrator::errorNotification, this, [this](const QString& err) {
        m_transcriptPane->setStatusText("Error: " + err);
        statusBar()->showMessage("Error: " + err, 8000);
    });
    connect(m_orchestrator, &Orchestrator::toolApprovalRequested, this, &MainWindow::showToolApprovalDialog);
    connect(m_orchestrator, &Orchestrator::toolApprovalCancelled, this, [this](quint64 requestId) {
        if (QPointer<QDialog> dlg = m_approvalDialogs.take(requestId)) dlg->close();
    });
    connect(m_orchestrator, &Orchestrator::workspaceChanged, this, [this]() {
        m_workspacePane->refreshViews();
    });
    connect(m_transcriptPane, &TranscriptView::viewTurnChangesRequested, this, &MainWindow::showTurnDiff);
    connect(m_transcriptPane, &TranscriptView::undoTurnRequested, this, [this](int messageIndex) {
        if (messageIndex < 0 || messageIndex >= m_orchestrator->session().messages.size()) return;
        const ChatMessage& msg = m_orchestrator->session().messages[messageIndex];
        const auto answer = QMessageBox::question(
            this, "Undo Turn",
            QStringLiteral("Restore the workspace to its state before %1's turn?\n\n"
                           "Files changed since then are reverted and files added since then are removed. "
                           "The current state is saved as a checkpoint first, so this can be undone.")
                .arg(msg.senderHandle));
        if (answer == QMessageBox::Yes) m_orchestrator->restoreCheckpointBefore(messageIndex);
    });
    connect(m_orchestrator, &Orchestrator::activeSpeakerChanged, this,
            [this](const QString&, const QString&, const QString&) {
                refreshParticipantsList();
            });
    connect(m_orchestrator, &Orchestrator::debateStateChanged, this,
            [this](bool running, int currentRound, int maxRounds) {
                m_btnStart->setEnabled(!running);
                m_btnStep->setEnabled(!running);
                m_btnStop->setEnabled(running);
                if (!running) {
                    persistSession();
                    QTimer::singleShot(0, this, &MainWindow::refreshSessionsList);
                } else {
                    m_transcriptPane->setStatusText(
                        QStringLiteral("Round %1 / %2 active...")
                            .arg(QString::number(currentRound + 1), QString::number(maxRounds)));
                }
            });

    // Pick default models from local GGUFs right away; the router refresh is asynchronous
    m_availableModels = m_router->scanLocalGgufModels();
    onRefreshModels();
    if (m_orchestrator->session().participants.isEmpty()) {
        populateDefaultCouncil(); // first start, or the saved roster was emptied
    } else {
        refreshParticipantsList();
        m_transcriptPane->setMentionParticipants(m_orchestrator->session().participants);
    }
    syncRoomControls();
    refreshSessionsList();
    m_workspacePane->refreshViews();
    m_transcriptPane->setMessages(m_orchestrator->session().messages);
}

void MainWindow::loadAppSettings() {
    QSettings settings;
    m_router->setBaseUrl(settings.value("router/baseUrl", m_router->baseUrl()).toString());
    m_router->setApiKey(settings.value("router/apiKey", m_router->apiKey()).toString());
    m_router->setModelsDir(settings.value("router/modelsDir", m_router->modelsDir()).toString());
    m_router->setRouterScriptPath(settings.value("router/scriptPath", m_router->routerScriptPath()).toString());
    m_router->setStreamIdleTimeoutMs(
        settings.value("router/streamIdleTimeoutSec", m_router->streamIdleTimeoutMs() / 1000).toInt() * 1000);

    SandboxLimits limits = m_sandbox->limits();
    limits.memoryMaxMiB = settings.value("sandbox/memoryMaxMiB", limits.memoryMaxMiB).toInt();
    limits.tasksMax = settings.value("sandbox/tasksMax", limits.tasksMax).toInt();
    m_sandbox->setLimits(limits);

    m_orchestrator->setMaxToolDepth(settings.value("orchestrator/maxToolDepth", m_orchestrator->maxToolDepth()).toInt());
    m_orchestrator->setAutoCheckpoints(settings.value("orchestrator/autoCheckpoints", true).toBool());
    m_orchestrator->setToolApprovalPolicy(static_cast<ToolApprovalPolicy>(
        settings.value("orchestrator/approvalPolicy", static_cast<int>(ToolApprovalPolicy::Off)).toInt()));

    QVector<ProviderConfig> providers;
    const QJsonArray provArr = QJsonDocument::fromJson(settings.value("providers/list").toByteArray()).array();
    for (const auto& v : provArr) providers.append(ProviderConfig::fromJson(v.toObject()));
    m_orchestrator->setProviders(providers);
}

void MainWindow::saveAppSettings() {
    QSettings settings;
    settings.setValue("router/baseUrl", m_router->baseUrl());
    settings.setValue("router/apiKey", m_router->apiKey());
    settings.setValue("router/modelsDir", m_router->modelsDir());
    settings.setValue("router/scriptPath", m_router->routerScriptPath());
    settings.setValue("router/streamIdleTimeoutSec", m_router->streamIdleTimeoutMs() / 1000);
    settings.setValue("sandbox/memoryMaxMiB", m_sandbox->limits().memoryMaxMiB);
    settings.setValue("sandbox/tasksMax", m_sandbox->limits().tasksMax);
    settings.setValue("orchestrator/maxToolDepth", m_orchestrator->maxToolDepth());
    settings.setValue("orchestrator/autoCheckpoints", m_orchestrator->autoCheckpoints());
    settings.setValue("orchestrator/approvalPolicy", static_cast<int>(m_orchestrator->toolApprovalPolicy()));

    const RoomSession& room = m_orchestrator->session();
    settings.setValue("room/maxRounds", room.maxRounds);
    settings.setValue("room/vramBudgetGiB", room.vramBudgetGiB);
    settings.setValue("room/mode", static_cast<int>(room.mode));

    QJsonArray provArr;
    for (const auto& prov : m_orchestrator->providers()) provArr.append(prov.toJson());
    settings.setValue("providers/list", QJsonDocument(provArr).toJson(QJsonDocument::Compact));
}

void MainWindow::setupUi() {
    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(6);

    // 1. Top Control Bar: router status on the left, run controls on the right
    auto* topBar = new QWidget(central);
    topBar->setObjectName("topBar");
    auto* topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(10, 6, 10, 6);
    topLayout->setSpacing(6);

    auto* brand = new QLabel("◆ Agora", topBar);
    brand->setObjectName("brand");
    m_routerBadge = new QLabel("Checking router…", topBar);
    m_routerBadge->setObjectName("routerBadge");
    m_btnRefreshModels = new QPushButton("⟳", topBar);
    m_btnRefreshModels->setToolTip("Refresh the model list from the router");
    m_btnRefreshModels->setProperty("role", "ghost");
    m_btnStartRouter = new QPushButton("Start Router", topBar);
    m_btnStartRouter->setProperty("role", "ghost");
    m_btnSettings = new QPushButton("⚙ Settings", topBar);
    m_btnSettings->setProperty("role", "ghost");
    m_btnProviders = new QPushButton("☁ Providers", topBar);
    m_btnProviders->setProperty("role", "ghost");
    m_btnProviders->setToolTip("External OpenAI-compatible APIs (OpenAI, Anthropic, Gemini, OpenRouter, Ollama, ...)");

    m_comboMode = new QComboBox(topBar);
    m_comboMode->addItem("Round-robin + @mention", static_cast<int>(DebateMode::RoundRobinWithMentions));
    m_comboMode->addItem("@mention only", static_cast<int>(DebateMode::MentionsOnly));
    m_comboMode->addItem("Single turn", static_cast<int>(DebateMode::SingleTurn));
    m_comboMode->setToolTip("Who speaks next: round-robin with @mention hand-offs, only @mentioned "
                            "participants, or one turn at a time");

    m_spinMaxRounds = new QSpinBox(topBar);
    m_spinMaxRounds->setRange(1, 50);
    m_spinMaxRounds->setValue(8);
    m_spinMaxRounds->setSuffix(" turns");
    m_spinMaxRounds->setToolTip("Maximum turns per run");

    m_spinVramBudget = new QDoubleSpinBox(topBar);
    m_spinVramBudget->setRange(1.0, 64.0);
    m_spinVramBudget->setSingleStep(0.5);
    m_spinVramBudget->setValue(8.5);
    m_spinVramBudget->setSuffix(" GiB VRAM");
    m_spinVramBudget->setToolTip("Max combined GGUF size kept resident before auto-unloading previous models");

    m_btnStart = new QPushButton("▶ Start", topBar);
    m_btnStart->setProperty("role", "primary");
    m_btnStart->setToolTip("Start or continue the debate");
    m_btnStep = new QPushButton("⏭ Step", topBar);
    m_btnStep->setToolTip("Run a single turn");
    m_btnStop = new QPushButton("⏹ Stop", topBar);
    m_btnStop->setProperty("role", "danger");
    m_btnStop->setEnabled(false);
    m_btnExportMd = new QPushButton("Export", topBar);
    m_btnExportMd->setProperty("role", "ghost");
    m_btnExportMd->setToolTip("Export the transcript as Markdown");

    auto separator = [topBar]() {
        auto* line = new QFrame(topBar);
        line->setObjectName("vsep");
        line->setFixedSize(1, 22);
        return line;
    };

    topLayout->addWidget(brand);
    topLayout->addSpacing(6);
    topLayout->addWidget(m_routerBadge);
    topLayout->addWidget(m_btnRefreshModels);
    topLayout->addWidget(m_btnStartRouter);
    topLayout->addStretch();
    topLayout->addWidget(m_comboMode);
    topLayout->addWidget(m_spinMaxRounds);
    topLayout->addWidget(m_spinVramBudget);
    topLayout->addWidget(separator());
    topLayout->addWidget(m_btnStart);
    topLayout->addWidget(m_btnStep);
    topLayout->addWidget(m_btnStop);
    topLayout->addWidget(separator());
    topLayout->addWidget(m_btnProviders);
    topLayout->addWidget(m_btnSettings);
    topLayout->addWidget(m_btnExportMd);

    rootLayout->addWidget(topBar);

    // 2. Main 3-Pane Splitter
    auto* mainSplitter = new QSplitter(Qt::Horizontal, central);

    // --- LEFT PANE: Participants & Sessions ---
    auto* leftPane = new QWidget(mainSplitter);
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(4, 4, 4, 4);

    auto* lblRoster = new QLabel("PARTICIPANTS", leftPane);
    lblRoster->setProperty("role", "section");
    m_listParticipants = new QListWidget(leftPane);
    m_listParticipants->setObjectName("participantList");
    m_listParticipants->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_listParticipants->setSpacing(2);

    auto* rosterBtnRow = new QHBoxLayout();
    auto* btnAddP = new QPushButton("+ Add", leftPane);
    auto* btnEditP = new QPushButton("Edit", leftPane);
    auto* btnDelP = new QPushButton("Remove", leftPane);
    btnDelP->setProperty("role", "ghost");
    rosterBtnRow->addWidget(btnAddP);
    rosterBtnRow->addWidget(btnEditP);
    rosterBtnRow->addWidget(btnDelP);

    auto* btnPresetCouncil = new QPushButton("⚡ Load a Council…", leftPane);
    btnPresetCouncil->setToolTip("Replace the room with a ready-made team of roles");
    auto* councilMenu = new QMenu(btnPresetCouncil);
    for (const auto& council : defaultCouncilTemplates()) {
        QStringList handles = council.handles;
        QAction* act = councilMenu->addAction(QStringLiteral("%1 — %2").arg(council.name, handles.join(' ')));
        act->setToolTip(council.description);
        connect(act, &QAction::triggered, this, [this, council]() {
            if (!m_orchestrator->session().participants.isEmpty() &&
                QMessageBox::question(this, "Load Council",
                                      QStringLiteral("Replace the current participants with the %1?\n\n"
                                                     "Participants with the same handle keep their model, "
                                                     "provider and prompt.").arg(council.name)) != QMessageBox::Yes) {
                return;
            }
            loadCouncil(council);
        });
    }
    btnPresetCouncil->setMenu(councilMenu);

    auto* lblSessions = new QLabel("SESSIONS", leftPane);
    lblSessions->setProperty("role", "section");
    m_listSessions = new QListWidget(leftPane);
    m_listSessions->setObjectName("sessionList");
    m_listSessions->setContextMenuPolicy(Qt::CustomContextMenu);
    m_listSessions->setToolTip("Double-click to open; right-click to delete");
    auto* sessionBtnRow = new QHBoxLayout();
    auto* btnNewSession = new QPushButton("+ New Room", leftPane);
    auto* btnSaveSession = new QPushButton("Save", leftPane);
    btnSaveSession->setProperty("role", "ghost");
    sessionBtnRow->addWidget(btnNewSession);
    sessionBtnRow->addWidget(btnSaveSession);

    leftLayout->addWidget(lblRoster);
    leftLayout->addWidget(m_listParticipants, 3);
    leftLayout->addLayout(rosterBtnRow);
    leftLayout->addWidget(btnPresetCouncil);
    leftLayout->addSpacing(12);
    leftLayout->addWidget(lblSessions);
    leftLayout->addWidget(m_listSessions, 2);
    leftLayout->addLayout(sessionBtnRow);

    // --- CENTER PANE: Modular TranscriptView ---
    m_transcriptPane = new TranscriptView(mainSplitter);

    // --- RIGHT PANE: Modular WorkspacePane ---
    m_workspacePane = new WorkspacePane(m_sandbox, m_toolEngine, mainSplitter);

    mainSplitter->addWidget(leftPane);
    mainSplitter->addWidget(m_transcriptPane);
    mainSplitter->addWidget(m_workspacePane);
    mainSplitter->setSizes({310, 690, 520});

    rootLayout->addWidget(mainSplitter, 1);
    setCentralWidget(central);

    connect(m_btnRefreshModels, &QPushButton::clicked, this, &MainWindow::onRefreshModels);
    connect(m_btnStartRouter, &QPushButton::clicked, this, &MainWindow::onStartRouterClicked);
    connect(m_btnSettings, &QPushButton::clicked, this, &MainWindow::onOpenSettingsClicked);
    connect(m_btnProviders, &QPushButton::clicked, this, &MainWindow::onManageProvidersClicked);
    connect(m_btnStart, &QPushButton::clicked, this, &MainWindow::onStartDebateClicked);
    connect(m_btnStep, &QPushButton::clicked, this, &MainWindow::onStepTurnClicked);
    connect(m_btnStop, &QPushButton::clicked, this, &MainWindow::onStopDebateClicked);
    connect(m_btnExportMd, &QPushButton::clicked, this, &MainWindow::onExportMarkdownClicked);

    connect(m_comboMode, &QComboBox::currentIndexChanged, this, [this](int) {
        m_orchestrator->session().mode = static_cast<DebateMode>(m_comboMode->currentData().toInt());
    });
    connect(m_spinMaxRounds, &QSpinBox::valueChanged, this, [this](int val) {
        m_orchestrator->session().maxRounds = val;
    });
    connect(m_spinVramBudget, &QDoubleSpinBox::valueChanged, this, [this](double val) {
        m_orchestrator->session().vramBudgetGiB = val;
    });

    connect(btnAddP, &QPushButton::clicked, this, &MainWindow::onAddParticipantClicked);
    connect(btnEditP, &QPushButton::clicked, this, &MainWindow::onEditParticipantClicked);
    connect(btnDelP, &QPushButton::clicked, this, &MainWindow::onRemoveParticipantClicked);
    connect(m_listParticipants, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
        onEditParticipantClicked();
    });

    connect(btnNewSession, &QPushButton::clicked, this, &MainWindow::onNewSessionClicked);
    connect(btnSaveSession, &QPushButton::clicked, this, &MainWindow::onSaveSessionClicked);
    connect(m_listSessions, &QListWidget::itemDoubleClicked, this, &MainWindow::onSessionListDoubleClicked);
    connect(m_listSessions, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* item = m_listSessions->itemAt(pos);
        if (!item) return;
        const QString id = item->data(Qt::UserRole).toString();
        QMenu menu(this);
        QAction* openAct = menu.addAction("Open");
        QAction* deleteAct = menu.addAction("Delete…");
        QAction* chosen = menu.exec(m_listSessions->viewport()->mapToGlobal(pos));
        if (chosen == openAct) {
            onSessionListDoubleClicked(item);
        } else if (chosen == deleteAct) {
            if (QMessageBox::question(this, "Delete Session",
                                      QStringLiteral("Delete \"%1\" from saved sessions?\n\n"
                                                     "Its workspace folder on disk is kept.")
                                          .arg(item->data(Qt::UserRole + 1).toString())) != QMessageBox::Yes) {
                return;
            }
            m_store->deleteSession(id);
            if (id == m_orchestrator->session().id) QSettings().remove("room/lastSessionId");
            refreshSessionsList();
        }
    });

    connect(m_transcriptPane, &TranscriptView::sendUserMessageRequested, this, &MainWindow::onSendUserMessage);
    connect(m_workspacePane, &WorkspacePane::browseWorkspaceRequested, this, &MainWindow::onBrowseWorkspaceClicked);
    connect(m_workspacePane, &WorkspacePane::allowNetworkToggled, this, [this](bool checked) {
        m_sandbox->setAllowNetwork(checked);
        m_orchestrator->session().allowSandboxNetwork = checked;
    });
    connect(m_workspacePane, &WorkspacePane::statusMessage, this, [this](const QString& msg) {
        statusBar()->showMessage(msg, 4000);
    });
}

namespace {

// Stylesheets can't draw arrows without an image and the app ships no image files, so
// small chevrons are drawn once into the cache directory
QString arrowImagePath(bool up) {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/ui";
    const QString path = dir + (up ? "/arrow-up.png" : "/arrow-down.png");
    if (QFileInfo::exists(path)) return path;
    QDir().mkpath(dir);
    QPixmap pix(16, 16);
    pix.fill(Qt::transparent);
    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor("#94a3b8"), 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QPolygonF chevron = up ? QPolygonF({{3.5, 11}, {8, 5.5}, {12.5, 11}})
                                 : QPolygonF({{3.5, 5.5}, {8, 11}, {12.5, 5.5}});
    painter.drawPolyline(chevron);
    painter.end();
    pix.save(path);
    return path;
}

} // namespace

void MainWindow::applyDarkTheme() {
    // Palette: slate background, blue accent; buttons come in default / primary / danger / ghost
    QString css = QStringLiteral(R"(
        QWidget {
            color: #e2e8f0;
            font-size: 13px;
        }
        QMainWindow, QDialog, QMessageBox {
            background-color: #0f172a;
        }
        QLabel {
            background: transparent;
        }
        QLabel[role="section"] {
            color: #64748b;
            font-size: 11px;
            font-weight: 700;
            padding: 2px 2px 0px 2px;
        }
        #topBar {
            background-color: #111c31;
            border: 1px solid #1e293b;
            border-radius: 8px;
        }
        #brand {
            font-size: 15px;
            font-weight: 800;
            color: #f8fafc;
        }
        #routerBadge {
            border-radius: 10px;
            padding: 3px 10px;
            font-weight: 600;
            color: #94a3b8;
            background-color: #1e293b;
        }
        #routerBadge[state="online"] {
            color: #4ade80;
            background-color: rgba(74, 222, 128, 0.10);
        }
        #routerBadge[state="offline"] {
            color: #f87171;
            background-color: rgba(248, 113, 113, 0.10);
        }
        #vsep {
            background-color: #334155;
        }
        QListWidget, QPlainTextEdit, QTextBrowser, QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
            background-color: #1e293b;
            color: #f8fafc;
            border: 1px solid #334155;
            border-radius: 6px;
            padding: 4px;
            selection-background-color: #2563eb;
        }
        QLineEdit:focus, QPlainTextEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus {
            border-color: #3b82f6;
        }
        QLineEdit:read-only {
            color: #94a3b8;
        }
        QSpinBox, QDoubleSpinBox {
            padding-right: 18px;
        }
        QSpinBox::up-button, QDoubleSpinBox::up-button,
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            subcontrol-origin: border;
            width: 16px;
            background: #273449;
            border-left: 1px solid #334155;
        }
        QSpinBox::up-button, QDoubleSpinBox::up-button {
            subcontrol-position: top right;
            border-top-right-radius: 6px;
        }
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            subcontrol-position: bottom right;
            border-bottom-right-radius: 6px;
        }
        QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
        QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
            background: #334155;
        }
        QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {
            image: url(@ARROW_UP@);
            width: 8px; height: 8px;
        }
        QSpinBox::down-arrow, QDoubleSpinBox::down-arrow,
        QComboBox::down-arrow {
            image: url(@ARROW_DOWN@);
            width: 8px; height: 8px;
        }
        QComboBox::drop-down {
            border: none;
            width: 18px;
        }
        QComboBox QAbstractItemView {
            background-color: #1e293b;
            border: 1px solid #334155;
            selection-background-color: #2563eb;
            outline: 0;
        }
        QListWidget {
            outline: 0;
        }
        QListWidget::item {
            border-radius: 6px;
            padding: 4px 6px;
        }
        QListWidget::item:hover {
            background-color: #243247;
        }
        QListWidget::item:selected {
            background-color: #1e3a5f;
            color: #f8fafc;
        }
        #participantCard, #participantCard QWidget {
            background: transparent;
        }
        #participantModel {
            color: #64748b;
            font-size: 11px;
        }
        QPushButton {
            background-color: #1e293b;
            color: #e2e8f0;
            border: 1px solid #334155;
            border-radius: 6px;
            padding: 5px 12px;
            font-weight: 600;
        }
        QPushButton:hover {
            background-color: #273449;
            border-color: #475569;
        }
        QPushButton:pressed {
            background-color: #334155;
        }
        QPushButton:disabled {
            background-color: #172033;
            color: #475569;
            border-color: #1e293b;
        }
        QPushButton::menu-indicator {
            subcontrol-position: right center;
            right: 8px;
        }
        QPushButton[role="primary"] {
            background-color: #2563eb;
            border-color: #3b82f6;
            color: #ffffff;
        }
        QPushButton[role="primary"]:hover {
            background-color: #3b82f6;
        }
        QPushButton[role="primary"]:disabled {
            background-color: #172033;
            color: #475569;
            border-color: #1e293b;
        }
        QPushButton[role="danger"] {
            background-color: rgba(239, 68, 68, 0.15);
            border-color: #ef4444;
            color: #fca5a5;
        }
        QPushButton[role="danger"]:hover {
            background-color: rgba(239, 68, 68, 0.28);
        }
        QPushButton[role="danger"]:disabled {
            background-color: #172033;
            color: #475569;
            border-color: #1e293b;
        }
        QPushButton[role="ghost"] {
            background-color: transparent;
            border-color: transparent;
            color: #94a3b8;
        }
        QPushButton[role="ghost"]:hover {
            background-color: #1e293b;
            color: #e2e8f0;
        }
        QCheckBox {
            spacing: 6px;
            background: transparent;
        }
        QCheckBox::indicator {
            width: 14px;
            height: 14px;
            border: 1px solid #475569;
            border-radius: 4px;
            background: #1e293b;
        }
        QCheckBox::indicator:checked {
            background: #3b82f6;
            border-color: #3b82f6;
        }
        QTabWidget::pane {
            border: 1px solid #1e293b;
            border-radius: 6px;
            top: -1px;
        }
        QTabBar::tab {
            background: transparent;
            color: #94a3b8;
            padding: 6px 14px;
            border-bottom: 2px solid transparent;
            margin-right: 2px;
        }
        QTabBar::tab:hover {
            color: #e2e8f0;
        }
        QTabBar::tab:selected {
            color: #f8fafc;
            border-bottom: 2px solid #3b82f6;
            font-weight: bold;
        }
        QSplitter::handle {
            background: transparent;
        }
        QSplitter::handle:hover {
            background: #334155;
        }
        QSplitter::handle:horizontal {
            width: 6px;
        }
        QSplitter::handle:vertical {
            height: 6px;
        }
        QScrollBar:vertical {
            background: transparent;
            width: 10px;
            margin: 2px;
        }
        QScrollBar:horizontal {
            background: transparent;
            height: 10px;
            margin: 2px;
        }
        QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
            background: #334155;
            border-radius: 3px;
            min-height: 28px;
            min-width: 28px;
        }
        QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover {
            background: #475569;
        }
        QScrollBar::add-line, QScrollBar::sub-line {
            width: 0px;
            height: 0px;
        }
        QScrollBar::add-page, QScrollBar::sub-page {
            background: none;
        }
        QMenu {
            background-color: #1e293b;
            border: 1px solid #334155;
            padding: 4px;
        }
        QMenu::item {
            padding: 6px 18px;
            border-radius: 4px;
        }
        QMenu::item:selected {
            background-color: #2563eb;
        }
        QToolTip {
            background-color: #1e293b;
            color: #e2e8f0;
            border: 1px solid #334155;
            padding: 4px 6px;
        }
        QStatusBar {
            background-color: #0b1220;
            color: #94a3b8;
            border-top: 1px solid #1e293b;
        }
    )");
    css.replace("@ARROW_UP@", arrowImagePath(true));
    css.replace("@ARROW_DOWN@", arrowImagePath(false));
    setStyleSheet(css);
}

void MainWindow::onRefreshModels() {
    m_router->fetchModels();
}

void MainWindow::onStartRouterClicked() {
    QString err;
    if (!m_router->startRouterScript(&err)) {
        QMessageBox::warning(this, "Router Launch Failed", err);
        return;
    }
    statusBar()->showMessage("Starting router script: " + m_router->routerScriptPath(), 5000);
    QTimer::singleShot(2500, this, &MainWindow::onRefreshModels);
}

void MainWindow::onOpenSettingsClicked() {
    QDialog dlg(this);
    dlg.setWindowTitle("Agora Settings");
    dlg.resize(560, 420);

    auto* form = new QFormLayout(&dlg);
    auto* editUrl = new QLineEdit(m_router->baseUrl(), &dlg);
    auto* editKey = new QLineEdit(m_router->apiKey(), &dlg);
    auto* editModelsDir = new QLineEdit(m_router->modelsDir(), &dlg);
    auto* editScript = new QLineEdit(m_router->routerScriptPath(), &dlg);

    form->addRow("Router Base URL:", editUrl);
    form->addRow("Router API Key:", editKey);
    form->addRow("Local GGUF Models Folder:", editModelsDir);
    form->addRow("Router Start Script:", editScript);

    auto* spinStreamTimeout = new QSpinBox(&dlg);
    spinStreamTimeout->setRange(10, 3600);
    spinStreamTimeout->setSuffix(" s");
    spinStreamTimeout->setValue(m_router->streamIdleTimeoutMs() / 1000);
    spinStreamTimeout->setToolTip("Abort a model reply after this long with no data. "
                                  "Must cover prompt processing of the largest model.");
    form->addRow("Stream Idle Timeout:", spinStreamTimeout);

    auto* spinSandboxMem = new QSpinBox(&dlg);
    spinSandboxMem->setRange(256, 262144);
    spinSandboxMem->setSingleStep(256);
    spinSandboxMem->setSuffix(" MiB");
    spinSandboxMem->setValue(m_sandbox->limits().memoryMaxMiB);
    form->addRow("Sandbox Memory Limit:", spinSandboxMem);

    auto* spinSandboxTasks = new QSpinBox(&dlg);
    spinSandboxTasks->setRange(16, 65536);
    spinSandboxTasks->setValue(m_sandbox->limits().tasksMax);
    spinSandboxTasks->setEnabled(BwrapSandbox::cgroupLimitsAvailable());
    spinSandboxTasks->setToolTip(BwrapSandbox::cgroupLimitsAvailable()
                                     ? "Maximum processes/threads per sandboxed command."
                                     : "Unavailable: needs a systemd user session (systemd-run --user --scope).");
    form->addRow("Sandbox Process Limit:", spinSandboxTasks);

    auto* comboApproval = new QComboBox(&dlg);
    comboApproval->addItem("Never ask (run all tools)", static_cast<int>(ToolApprovalPolicy::Off));
    comboApproval->addItem("Ask before writes & commands", static_cast<int>(ToolApprovalPolicy::AskForWrites));
    comboApproval->addItem("Ask before every tool call", static_cast<int>(ToolApprovalPolicy::AskForAll));
    comboApproval->setCurrentIndex(comboApproval->findData(static_cast<int>(m_orchestrator->toolApprovalPolicy())));
    form->addRow("Tool Approval:", comboApproval);

    auto* spinToolDepth = new QSpinBox(&dlg);
    spinToolDepth->setRange(1, 100);
    spinToolDepth->setValue(m_orchestrator->maxToolDepth());
    spinToolDepth->setToolTip("How many rounds of tool calls a participant may chain in one turn.");
    form->addRow("Max Tool Rounds per Turn:", spinToolDepth);

    auto* chkCheckpoints = new QCheckBox("Snapshot the workspace after every turn (per-turn diff & undo)", &dlg);
    chkCheckpoints->setChecked(m_orchestrator->autoCheckpoints());
    form->addRow("Checkpoints:", chkCheckpoints);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);

    if (dlg.exec() != QDialog::Accepted) return;

    m_router->setBaseUrl(editUrl->text());
    m_router->setApiKey(editKey->text());
    m_router->setModelsDir(editModelsDir->text());
    m_router->setRouterScriptPath(editScript->text());
    m_router->setStreamIdleTimeoutMs(spinStreamTimeout->value() * 1000);
    SandboxLimits limits = m_sandbox->limits();
    limits.memoryMaxMiB = spinSandboxMem->value();
    limits.tasksMax = spinSandboxTasks->value();
    m_sandbox->setLimits(limits);
    m_orchestrator->setToolApprovalPolicy(static_cast<ToolApprovalPolicy>(comboApproval->currentData().toInt()));
    m_orchestrator->setMaxToolDepth(spinToolDepth->value());
    m_orchestrator->setAutoCheckpoints(chkCheckpoints->isChecked());
    saveAppSettings();
    onRefreshModels();
}

QString MainWindow::providerName(const QString& providerId) const {
    for (const auto& prov : m_orchestrator->providers()) {
        if (prov.id == providerId) return prov.name;
    }
    return providerId;
}

void MainWindow::onManageProvidersClicked() {
    QVector<ProviderConfig> providers = m_orchestrator->providers();

    QDialog dlg(this);
    dlg.setWindowTitle("External AI Providers");
    dlg.resize(620, 380);
    auto* layout = new QVBoxLayout(&dlg);

    auto* intro = new QLabel(
        "Any OpenAI-compatible chat API can host participants: OpenAI, Anthropic, Google Gemini, "
        "OpenRouter, Groq, DeepSeek, Mistral, or a local Ollama / LM Studio. "
        "Choose the provider per participant in its settings.", &dlg);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* list = new QListWidget(&dlg);
    layout->addWidget(list, 1);
    auto refreshList = [list, &providers]() {
        list->clear();
        for (const auto& prov : providers) {
            list->addItem(QStringLiteral("%1 — %2  (%3 models)")
                              .arg(prov.name, prov.baseUrl, QString::number(prov.models.size())));
        }
    };
    refreshList();

    auto* row = new QHBoxLayout();
    auto* btnAdd = new QPushButton("Add…", &dlg);
    auto* btnEdit = new QPushButton("Edit…", &dlg);
    auto* btnRemove = new QPushButton("Remove", &dlg);
    row->addWidget(btnAdd);
    row->addWidget(btnEdit);
    row->addWidget(btnRemove);
    row->addStretch();
    layout->addLayout(row);

    connect(btnAdd, &QPushButton::clicked, &dlg, [this, &providers, refreshList]() {
        ProviderConfig prov;
        prov.id = "prov_" + QUuid::createUuid().toString(QUuid::Id128).left(10);
        if (showProviderEditDialog(&prov, "Add Provider")) {
            providers.append(prov);
            refreshList();
        }
    });
    auto editCurrent = [this, list, &providers, refreshList]() {
        const int idx = list->currentRow();
        if (idx < 0 || idx >= providers.size()) return;
        ProviderConfig copy = providers[idx];
        if (showProviderEditDialog(&copy, "Edit Provider")) {
            providers[idx] = copy;
            refreshList();
        }
    };
    connect(btnEdit, &QPushButton::clicked, &dlg, editCurrent);
    connect(list, &QListWidget::itemDoubleClicked, &dlg, editCurrent);
    connect(btnRemove, &QPushButton::clicked, &dlg, [this, list, &providers, refreshList, &dlg]() {
        const int idx = list->currentRow();
        if (idx < 0 || idx >= providers.size()) return;
        QStringList users;
        for (const auto& p : m_orchestrator->session().participants) {
            if (p.providerId == providers[idx].id) users << p.handle;
        }
        if (!users.isEmpty() &&
            QMessageBox::question(&dlg, "Remove Provider",
                                  QStringLiteral("%1 still use %2. Remove it anyway?")
                                      .arg(users.join(", "), providers[idx].name)) != QMessageBox::Yes) {
            return;
        }
        providers.removeAt(idx);
        refreshList();
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted) return;
    m_orchestrator->setProviders(providers);
    saveAppSettings();
    refreshParticipantsList();
}

bool MainWindow::showProviderEditDialog(ProviderConfig* inOutProvider, const QString& dialogTitle) {
    QDialog dlg(this);
    dlg.setWindowTitle(dialogTitle);
    dlg.resize(560, 300);
    auto* form = new QFormLayout(&dlg);

    const auto presets = defaultProviderPresets();
    auto* comboPreset = new QComboBox(&dlg);
    comboPreset->addItem("-- Custom --");
    for (const auto& pr : presets) comboPreset->addItem(pr.name);

    auto* editName = new QLineEdit(inOutProvider->name, &dlg);
    auto* editUrl = new QLineEdit(inOutProvider->baseUrl, &dlg);
    editUrl->setPlaceholderText("https://api.example.com/v1");
    auto* editKey = new QLineEdit(inOutProvider->apiKey, &dlg);
    editKey->setPlaceholderText("$OPENAI_API_KEY, or the key itself");
    editKey->setEchoMode(inOutProvider->apiKey.startsWith('$') ? QLineEdit::Normal : QLineEdit::Password);
    editKey->setToolTip("Write $NAME to read the key from environment variable NAME. "
                        "A key typed here is stored in plain text in Agora's settings file.");
    connect(editKey, &QLineEdit::textChanged, &dlg, [editKey](const QString& text) {
        editKey->setEchoMode(text.startsWith('$') ? QLineEdit::Normal : QLineEdit::Password);
    });

    auto* spinContext = new QSpinBox(&dlg);
    spinContext->setRange(2000, 2000000);
    spinContext->setSingleStep(4000);
    spinContext->setSuffix(" tokens");
    spinContext->setValue(inOutProvider->contextTokens);
    spinContext->setToolTip("History sent with each request is trimmed to fit this. "
                            "Hosted APIs bill per token, so this is also a cost cap per turn.");

    auto models = std::make_shared<QStringList>(inOutProvider->models);
    auto* btnFetch = new QPushButton("Test connection && fetch models", &dlg);
    auto* fetchStatus = new QLabel(models->isEmpty() ? QStringLiteral("No models fetched yet.")
                                                     : QStringLiteral("%1 models known.").arg(models->size()),
                                   &dlg);
    fetchStatus->setWordWrap(true);

    connect(comboPreset, &QComboBox::currentIndexChanged, &dlg, [&, presets](int idx) {
        if (idx <= 0 || idx - 1 >= presets.size()) return;
        const auto& pr = presets[idx - 1];
        editName->setText(pr.name);
        editUrl->setText(pr.baseUrl);
        if (editKey->text().isEmpty() || editKey->text().startsWith('$')) editKey->setText(pr.apiKeyHint);
    });

    QPointer<QLabel> statusGuard(fetchStatus);
    QPointer<QPushButton> fetchGuard(btnFetch);
    connect(btnFetch, &QPushButton::clicked, &dlg, [this, editUrl, editKey, models, statusGuard, fetchGuard]() {
        ProviderConfig probe;
        probe.baseUrl = editUrl->text().trimmed();
        probe.apiKey = editKey->text().trimmed();
        if (probe.apiKey.startsWith('$') && probe.resolvedApiKey().isEmpty()) {
            statusGuard->setText(QStringLiteral("Environment variable %1 is not set for Agora.").arg(probe.apiKey.mid(1)));
            return;
        }
        statusGuard->setText("Connecting…");
        fetchGuard->setEnabled(false);
        m_router->fetchProviderModels({probe.baseUrl, probe.resolvedApiKey(), false},
                                      [models, statusGuard, fetchGuard](bool ok, const QStringList& ids,
                                                                        const QString& error) {
            if (!statusGuard || !fetchGuard) return; // dialog already closed
            fetchGuard->setEnabled(true);
            if (!ok) {
                statusGuard->setText("Failed: " + error.left(300));
                return;
            }
            *models = ids;
            statusGuard->setText(QStringLiteral("Connected: %1 models available.").arg(ids.size()));
        });
    });

    form->addRow("Preset:", comboPreset);
    form->addRow("Name:", editName);
    form->addRow("Base URL:", editUrl);
    form->addRow("API Key:", editKey);
    form->addRow("Context Budget:", spinContext);
    form->addRow(btnFetch, fetchStatus);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, [&]() {
        if (editName->text().trimmed().isEmpty() || !QUrl(editUrl->text().trimmed()).isValid() ||
            !editUrl->text().trimmed().startsWith("http")) {
            QMessageBox::warning(&dlg, "Provider", "A name and an http(s) base URL are required.");
            return;
        }
        dlg.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);

    if (dlg.exec() != QDialog::Accepted) return false;
    inOutProvider->name = editName->text().trimmed();
    inOutProvider->baseUrl = editUrl->text().trimmed();
    inOutProvider->apiKey = editKey->text().trimmed();
    inOutProvider->contextTokens = spinContext->value();
    inOutProvider->models = *models;
    return true;
}

void MainWindow::populateDefaultCouncil() {
    m_orchestrator->session().participants.clear();
    loadCouncil(defaultCouncilTemplates().first());
}

void MainWindow::loadCouncil(const CouncilTemplate& council) {
    // Model preferences for the default roles; other roles take the next available model
    static const QHash<QString, QStringList> modelHints = {
        {"@architect", {"Ornith", "Qwen3.8", "Xing", "Qwen3.5"}},
        {"@coder", {"Qwopus", "coder", "Qwen3.5"}},
        {"@reviewer", {"Qwen3.5-4B", "LFM2.5", "minicpm"}},
    };
    auto pickModel = [this](const QStringList& preferredSubstrings, int fallbackIdx) -> QString {
        for (const QString& sub : preferredSubstrings) {
            for (const auto& m : m_availableModels) {
                if (m.id.contains(sub, Qt::CaseInsensitive)) return m.id;
            }
        }
        if (!m_availableModels.isEmpty()) {
            return m_availableModels[fallbackIdx % m_availableModels.size()].id;
        }
        return "Qwen3.5-4B-Q4_K_M";
    };

    const QVector<Participant> previous = m_orchestrator->session().participants;
    const auto presets = defaultRolePresets();
    QVector<Participant> parts;
    for (int i = 0; i < council.handles.size(); ++i) {
        const QString& handle = council.handles[i];
        auto kept = std::find_if(previous.begin(), previous.end(),
                                 [&handle](const Participant& p) { return p.handle == handle; });
        if (kept != previous.end()) {
            parts.append(*kept);
            continue;
        }
        auto preset = std::find_if(presets.begin(), presets.end(),
                                   [&handle](const RolePreset& r) { return r.handle == handle; });
        if (preset == presets.end()) continue;
        Participant p;
        p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        p.handle = preset->handle;
        p.displayName = preset->displayName;
        p.colorHex = preset->colorHex;
        p.temperature = preset->temperature;
        p.systemPrompt = preset->systemPrompt;
        p.modelId = pickModel(modelHints.value(handle), i);
        parts.append(p);
    }
    m_orchestrator->session().participants = parts;
    onRosterChanged();
}

void MainWindow::onRosterChanged() {
    refreshParticipantsList();
    m_transcriptPane->setMentionParticipants(m_orchestrator->session().participants);
    saveRoster();
    persistSession();
}

void MainWindow::saveRoster() const {
    QJsonArray arr;
    for (const auto& p : m_orchestrator->session().participants) arr.append(p.toJson());
    QSettings().setValue("room/roster", QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QVector<Participant> MainWindow::loadSavedRoster() const {
    QVector<Participant> parts;
    const QJsonArray arr = QJsonDocument::fromJson(QSettings().value("room/roster").toByteArray()).array();
    for (const auto& v : arr) parts.append(Participant::fromJson(v.toObject()));
    return parts;
}

void MainWindow::persistSession() {
    const RoomSession& room = m_orchestrator->session();
    if (room.messages.isEmpty()) return; // nothing worth a saved-sessions entry yet
    QString err;
    if (!m_store->saveSession(room, &err)) {
        statusBar()->showMessage("Could not save session: " + err, 8000);
        return;
    }
    QSettings().setValue("room/lastSessionId", room.id);
}

void MainWindow::syncRoomControls() {
    const RoomSession& room = m_orchestrator->session();
    m_spinMaxRounds->setValue(room.maxRounds);
    m_spinVramBudget->setValue(room.vramBudgetGiB);
    m_comboMode->setCurrentIndex(qMax(0, m_comboMode->findData(static_cast<int>(room.mode))));
    m_workspacePane->setAllowNetwork(room.allowSandboxNetwork);
}

QString MainWindow::newWorkspacePath() const {
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return dataDir + "/workspaces/session_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
}

void MainWindow::closeEvent(QCloseEvent* event) {
    m_orchestrator->stopDebate();
    saveRoster();
    saveAppSettings();
    persistSession();
    QMainWindow::closeEvent(event);
}

void MainWindow::refreshParticipantsList() {
    const int previousRow = m_listParticipants->currentRow();
    m_listParticipants->clear();
    QMap<QString, ModelInfo> modelMap;
    for (const auto& m : m_availableModels) {
        modelMap[m.id] = m;
    }

    for (const auto& p : m_orchestrator->session().participants) {
        const bool isSpeaking = (p.id == m_orchestrator->activeSpeakerId());
        const bool external = !p.providerId.isEmpty();
        const ModelInfo mi = external ? ModelInfo{} : modelMap.value(p.modelId);

        // Status pill: speaking > disabled > provider / residency
        QString pillText;
        QString pillColor;
        if (isSpeaking) {
            pillText = "speaking";
            pillColor = "#38bdf8";
        } else if (!p.enabled) {
            pillText = "disabled";
            pillColor = "#64748b";
        } else if (external) {
            const bool known = m_orchestrator->providerFor(p) != nullptr;
            pillText = known ? ("☁ " + providerName(p.providerId)) : QStringLiteral("☁ provider missing");
            pillColor = known ? "#a78bfa" : "#f87171";
        } else if (mi.status == "loaded" || mi.status == "loading") {
            pillText = mi.status;
            pillColor = "#4ade80";
        } else if (mi.status == "sleeping") {
            pillText = "sleeping";
            pillColor = "#fbbf24";
        } else {
            pillText = mi.status.isEmpty() ? QStringLiteral("unloaded") : mi.status;
            pillColor = "#64748b";
        }

        auto* card = new QWidget();
        card->setObjectName("participantCard");
        auto* grid = new QVBoxLayout(card);
        grid->setContentsMargins(8, 6, 8, 6);
        grid->setSpacing(2);

        auto* top = new QHBoxLayout();
        top->setSpacing(6);
        auto* name = new QLabel(QStringLiteral("<span style='color:%1; font-weight:700;'>%2</span>"
                                               " <span style='color:#94a3b8;'>%3</span>")
                                    .arg(p.enabled ? p.colorHex : QStringLiteral("#64748b"),
                                         p.handle.toHtmlEscaped(), p.displayName.toHtmlEscaped()),
                                card);
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto* pill = new QLabel(pillText, card);
        pill->setStyleSheet(QStringLiteral("color:%1; border:1px solid %1; border-radius:9px; "
                                           "padding:1px 7px; font-size:11px;").arg(pillColor));
        top->addWidget(name, 1);
        top->addWidget(pill);

        QString modelLine = p.modelId;
        if (mi.sizeGiB > 0.01) modelLine += QStringLiteral(" · %1 GiB").arg(QString::number(mi.sizeGiB, 'f', 1));
        auto* model = new QLabel(modelLine, card);
        model->setObjectName("participantModel");
        model->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

        grid->addLayout(top);
        grid->addWidget(model);
        card->setToolTip(QStringLiteral("%1 (%2)\nModel: %3\nTemperature: %4\n\nDouble-click to edit.")
                             .arg(p.handle, p.displayName, p.modelId, QString::number(p.temperature, 'f', 2)));

        auto* item = new QListWidgetItem(m_listParticipants);
        card->ensurePolished();
        item->setSizeHint(QSize(0, qMax(card->sizeHint().height(), card->minimumSizeHint().height()) + 8));
        m_listParticipants->setItemWidget(item, card);
    }
    if (previousRow >= 0 && previousRow < m_listParticipants->count()) {
        m_listParticipants->setCurrentRow(previousRow);
    }
}

void MainWindow::refreshSessionsList() {
    m_listSessions->clear();
    const QString currentId = m_orchestrator->session().id;
    const auto summaries = m_store->listSessionSummaries();
    for (const auto& s : summaries) {
        QString when = s.updatedAt;
        const QDateTime dt = QDateTime::fromString(s.updatedAt, Qt::ISODate);
        if (dt.isValid()) when = dt.toLocalTime().toString("d MMM, HH:mm");
        const bool current = (s.id == currentId);
        auto* item = new QListWidgetItem(
            QStringLiteral("%1%2\n%3 messages · %4")
                .arg(current ? QStringLiteral("● ") : QString(), s.title, QString::number(s.messageCount), when),
            m_listSessions);
        item->setData(Qt::UserRole, s.id);
        item->setData(Qt::UserRole + 1, s.title);
        if (current) item->setForeground(QColor("#93c5fd"));
    }
}

void MainWindow::onStartDebateClicked() {
    m_orchestrator->session().maxRounds = m_spinMaxRounds->value();
    m_orchestrator->session().vramBudgetGiB = m_spinVramBudget->value();
    m_orchestrator->startDebate();
}

void MainWindow::onStepTurnClicked() {
    m_orchestrator->session().vramBudgetGiB = m_spinVramBudget->value();
    m_orchestrator->stepSingleTurn();
}

void MainWindow::onStopDebateClicked() {
    m_orchestrator->stopDebate();
}

void MainWindow::onSendUserMessage(const QString& text, bool autoStartDebate) {
    if (text.isEmpty()) {
        if (autoStartDebate && !m_orchestrator->isRunning()) {
            onStartDebateClicked();
        }
        return;
    }

    if (m_orchestrator->session().messages.isEmpty()) {
        m_orchestrator->session().title = text.left(48);
    }

    m_orchestrator->addUserMessage(text);
    persistSession();
    refreshSessionsList();

    if (autoStartDebate && !m_orchestrator->isRunning()) {
        onStartDebateClicked();
    }
}

bool MainWindow::showParticipantDialog(Participant* inOutParticipant, const QString& dialogTitle) {
    QDialog dlg(this);
    dlg.setWindowTitle(dialogTitle);
    dlg.resize(520, 420);

    auto* form = new QFormLayout(&dlg);

    auto* comboPreset = new QComboBox(&dlg);
    comboPreset->addItem("-- Custom Role --");
    const auto presets = defaultRolePresets();
    for (const auto& pr : presets) {
        comboPreset->addItem(QStringLiteral("%1 (%2)").arg(pr.displayName, pr.handle));
    }

    auto* editHandle = new QLineEdit(inOutParticipant->handle, &dlg);
    auto* editName = new QLineEdit(inOutParticipant->displayName, &dlg);
    auto* comboProvider = new QComboBox(&dlg);
    comboProvider->addItem("Local llama-server router", QString());
    for (const auto& prov : m_orchestrator->providers()) {
        comboProvider->addItem("☁ " + prov.name, prov.id);
    }
    if (!inOutParticipant->providerId.isEmpty() && comboProvider->findData(inOutParticipant->providerId) < 0) {
        comboProvider->addItem("☁ (missing provider)", inOutParticipant->providerId);
    }
    comboProvider->setCurrentIndex(qMax(0, comboProvider->findData(inOutParticipant->providerId)));

    auto* comboModel = new QComboBox(&dlg);
    comboModel->setEditable(true);
    auto* externalNote = new QLabel(&dlg);
    externalNote->setWordWrap(true);
    externalNote->setStyleSheet("color: #fbbf24;");

    auto fillModels = [this, comboProvider, comboModel, externalNote](const QString& keepText) {
        const QString providerId = comboProvider->currentData().toString();
        comboModel->clear();
        if (providerId.isEmpty()) {
            for (const auto& m : m_availableModels) comboModel->addItem(m.id);
            externalNote->hide();
        } else {
            QString name = comboProvider->currentText();
            for (const auto& prov : m_orchestrator->providers()) {
                if (prov.id == providerId) {
                    comboModel->addItems(prov.models);
                    name = prov.name;
                }
            }
            externalNote->setText(QStringLiteral(
                "This participant runs on %1. The room history, including file contents and command "
                "output from the workspace, is sent to that service with each turn.").arg(name));
            externalNote->show();
        }
        // Keep the model only if this provider offers it; a local GGUF name means nothing to a cloud API
        if (comboModel->findText(keepText) >= 0 || comboModel->count() == 0) {
            comboModel->setCurrentText(keepText);
        } else {
            comboModel->setCurrentIndex(0);
        }
    };
    fillModels(inOutParticipant->modelId);
    comboModel->setCurrentText(inOutParticipant->modelId); // as saved, even if no longer listed
    connect(comboProvider, &QComboBox::currentIndexChanged, &dlg, [fillModels, comboModel](int) {
        fillModels(comboModel->currentText());
    });

    auto* editColor = new QLineEdit(inOutParticipant->colorHex, &dlg);
    auto* spinTemp = new QDoubleSpinBox(&dlg);
    spinTemp->setRange(0.0, 2.0);
    spinTemp->setSingleStep(0.1);
    spinTemp->setValue(inOutParticipant->temperature);

    auto* chkEnabled = new QCheckBox("Participant Enabled in Debate", &dlg);
    chkEnabled->setChecked(inOutParticipant->enabled);

    auto* editPrompt = new QPlainTextEdit(inOutParticipant->systemPrompt, &dlg);

    connect(comboPreset, &QComboBox::currentIndexChanged, &dlg, [&](int idx) {
        if (idx <= 0 || idx - 1 >= presets.size()) return;
        const auto& pr = presets[idx - 1];
        editHandle->setText(pr.handle);
        editName->setText(pr.displayName);
        editColor->setText(pr.colorHex);
        spinTemp->setValue(pr.temperature);
        editPrompt->setPlainText(pr.systemPrompt);
    });

    form->addRow("Role Preset:", comboPreset);
    form->addRow("Handle (@mention):", editHandle);
    form->addRow("Display Name:", editName);
    form->addRow("Provider:", comboProvider);
    form->addRow("Model:", comboModel);
    form->addRow("", externalNote);
    form->addRow("Badge Color Hex:", editColor);
    form->addRow("Temperature:", spinTemp);
    form->addRow("", chkEnabled);
    form->addRow("System Prompt:", editPrompt);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);

    if (dlg.exec() != QDialog::Accepted) return false;

    QString handle = editHandle->text().trimmed();
    if (!handle.startsWith('@')) handle.prepend('@');

    inOutParticipant->handle = handle;
    inOutParticipant->displayName = editName->text().trimmed();
    inOutParticipant->modelId = comboModel->currentText().trimmed();
    inOutParticipant->providerId = comboProvider->currentData().toString();
    inOutParticipant->colorHex = editColor->text().trimmed();
    inOutParticipant->temperature = spinTemp->value();
    inOutParticipant->enabled = chkEnabled->isChecked();
    inOutParticipant->systemPrompt = editPrompt->toPlainText().trimmed();
    return true;
}

void MainWindow::onAddParticipantClicked() {
    const auto presets = defaultRolePresets();
    Participant p;
    p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p.handle = "@agent" + QString::number(m_orchestrator->session().participants.size() + 1);
    p.displayName = "Specialist Agent";
    p.modelId = m_availableModels.isEmpty() ? "Qwen3.5-4B-Q4_K_M" : m_availableModels.first().id;
    p.colorHex = "#a78bfa";
    p.temperature = 0.6;
    p.systemPrompt = presets.first().systemPrompt;

    if (showParticipantDialog(&p, "Add Room Participant")) {
        m_orchestrator->session().participants.append(p);
        onRosterChanged();
    }
}

void MainWindow::onEditParticipantClicked() {
    const int row = m_listParticipants->currentRow();
    auto& parts = m_orchestrator->session().participants;
    if (row < 0 || row >= parts.size()) return;

    Participant copy = parts[row];
    if (showParticipantDialog(&copy, "Edit Participant")) {
        parts[row] = copy;
        onRosterChanged();
    }
}

void MainWindow::onRemoveParticipantClicked() {
    const int row = m_listParticipants->currentRow();
    auto& parts = m_orchestrator->session().participants;
    if (row < 0 || row >= parts.size()) return;
    if (QMessageBox::question(this, "Remove Participant",
                              QStringLiteral("Remove %1 from the room?").arg(parts[row].handle)) != QMessageBox::Yes) {
        return;
    }
    parts.removeAt(row);
    onRosterChanged();
}

void MainWindow::onNewSessionClicked() {
    m_orchestrator->stopDebate();
    persistSession();
    const QString newWs = newWorkspacePath();
    QDir().mkpath(newWs);

    RoomSession s;
    s.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    s.title = "New Agora Debate";
    s.workspacePath = newWs;
    s.mode = m_orchestrator->session().mode;
    s.vramBudgetGiB = m_spinVramBudget->value();
    s.maxRounds = m_spinMaxRounds->value();
    s.participants = m_orchestrator->session().participants;
    openSession(s);
}

void MainWindow::openSession(const RoomSession& session) {
    m_workspacePane->clearSelectedFile();
    if (!m_fsWatcher->directories().isEmpty()) {
        m_fsWatcher->removePaths(m_fsWatcher->directories());
    }
    if (!m_fsWatcher->files().isEmpty()) {
        m_fsWatcher->removePaths(m_fsWatcher->files());
    }

    m_sandbox->setWorkspacePath(session.workspacePath);
    m_sandbox->setAllowNetwork(session.allowSandboxNetwork);
    m_sandbox->ensureWorkspaceInitialized();
    if (!session.workspacePath.isEmpty()) {
        m_fsWatcher->addPath(session.workspacePath);
    }

    m_orchestrator->setSession(session);
    m_workspacePane->setWorkspacePath(session.workspacePath);
    syncRoomControls();

    refreshParticipantsList();
    m_transcriptPane->setMentionParticipants(session.participants);
    m_workspacePane->refreshViews();
    m_transcriptPane->setMessages(session.messages);
    refreshSessionsList();
    if (!session.messages.isEmpty()) {
        QSettings().setValue("room/lastSessionId", session.id);
    }
}

void MainWindow::onSaveSessionClicked() {
    QString err;
    if (!m_store->saveSession(m_orchestrator->session(), &err)) {
        QMessageBox::warning(this, "Save Failed", err);
        return;
    }
    QSettings().setValue("room/lastSessionId", m_orchestrator->session().id);
    refreshSessionsList();
    statusBar()->showMessage("Session saved to SQLite.", 4000);
}

void MainWindow::onExportMarkdownClicked() {
    const QString path = QFileDialog::getSaveFileName(
        this, "Export Session Transcript", "agora_session.md", "Markdown (*.md)");
    if (path.isEmpty()) return;

    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        f.write(SessionStore::exportToMarkdown(m_orchestrator->session()).toUtf8());
        f.close();
        statusBar()->showMessage("Exported transcript to " + path, 5000);
    }
}

void MainWindow::onSessionListDoubleClicked(QListWidgetItem* item) {
    if (!item) return;
    const QString id = item->data(Qt::UserRole).toString();
    if (id.isEmpty() || id == m_orchestrator->session().id) return;

    QTimer::singleShot(0, this, [this, id]() {
        auto loaded = m_store->loadSession(id);
        if (!loaded) return;
        m_orchestrator->stopDebate();
        persistSession();
        openSession(*loaded);
        statusBar()->showMessage("Loaded room: " + loaded->title, 4000);
    });
}

void MainWindow::onBrowseWorkspaceClicked() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, "Select Shared Sandbox Workspace Directory", m_sandbox->workspacePath());
    if (dir.isEmpty()) return;

    m_sandbox->setWorkspacePath(dir);
    m_sandbox->ensureWorkspaceInitialized();
    m_orchestrator->session().workspacePath = dir;
    m_workspacePane->setWorkspacePath(dir);
    m_fsWatcher->addPath(dir);
    m_workspacePane->refreshViews();
}

void MainWindow::showToolApprovalDialog(quint64 requestId, const QString& speakerHandle, const ToolCall& call) {
    // Non-modal: the debate's callbacks keep flowing while the dialog is open
    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle("Tool Approval");
    dlg->resize(640, 420);

    auto* layout = new QVBoxLayout(dlg);
    auto* header = new QLabel(QStringLiteral("<b>%1</b> wants to run <b>%2</b>:")
                                  .arg(speakerHandle.toHtmlEscaped(), call.name.toHtmlEscaped()), dlg);
    layout->addWidget(header);

    QString details;
    const QJsonObject args = QJsonDocument::fromJson(call.argumentsJson.toUtf8()).object();
    if (call.name == "run_command") {
        details = "$ " + args["command"].toString();
    } else if (call.name == "write_file") {
        details = "File: " + args["path"].toString() + "\n\n" + args["content"].toString().left(20000);
    } else if (call.name == "patch_file") {
        details = "File: " + args["path"].toString() + "\n\n--- search\n" + args["search"].toString() +
                  "\n+++ replace\n" + args["replace"].toString();
    } else {
        details = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Indented));
    }
    auto* preview = new QPlainTextEdit(details, dlg);
    preview->setReadOnly(true);
    preview->setFont(QFont("Monospace", 10));
    layout->addWidget(preview, 1);

    auto* reason = new QLineEdit(dlg);
    reason->setPlaceholderText("Optional note for the model if you deny (e.g. \"use patch_file instead\")");
    layout->addWidget(reason);

    auto* row = new QHBoxLayout();
    auto* btnDeny = new QPushButton("Deny", dlg);
    auto* btnSession = new QPushButton(QStringLiteral("Allow %1 for this session").arg(call.name), dlg);
    auto* btnOnce = new QPushButton("Allow once", dlg);
    btnOnce->setDefault(true);
    row->addWidget(btnDeny);
    row->addStretch();
    row->addWidget(btnSession);
    row->addWidget(btnOnce);
    layout->addLayout(row);

    auto resolve = [this, requestId, dlg, reason](ToolApprovalDecision decision) {
        m_approvalDialogs.remove(requestId);
        const QString note = reason->text();
        dlg->disconnect(this); // don't treat the close below as a denial
        dlg->close();
        m_orchestrator->resolveToolApproval(requestId, decision, note);
    };
    connect(btnOnce, &QPushButton::clicked, dlg, [resolve]() { resolve(ToolApprovalDecision::AllowOnce); });
    connect(btnSession, &QPushButton::clicked, dlg, [resolve]() { resolve(ToolApprovalDecision::AllowToolForSession); });
    connect(btnDeny, &QPushButton::clicked, dlg, [resolve]() { resolve(ToolApprovalDecision::Deny); });
    // Closing the window counts as a denial (no-op if the orchestrator already cancelled it)
    connect(dlg, &QDialog::finished, this, [this, requestId, reason]() {
        m_approvalDialogs.remove(requestId);
        m_orchestrator->resolveToolApproval(requestId, ToolApprovalDecision::Deny, reason->text());
    });

    m_approvalDialogs.insert(requestId, dlg);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

void MainWindow::showTurnDiff(int messageIndex) {
    if (messageIndex < 0 || messageIndex >= m_orchestrator->session().messages.size()) return;
    const ChatMessage msg = m_orchestrator->session().messages[messageIndex];
    if (msg.checkpointCommit.isEmpty()) return;

    m_checkpoints->diff(msg.checkpointParent, msg.checkpointCommit,
                        [this, handle = msg.senderHandle, time = msg.timestamp](bool ok, const QString& text) {
        if (!ok) {
            statusBar()->showMessage("Could not load turn diff: " + text, 8000);
            return;
        }
        auto* dlg = new QDialog(this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setWindowTitle(QStringLiteral("Changes made in %1's turn (%2)").arg(handle, time));
        dlg->resize(900, 640);
        auto* layout = new QVBoxLayout(dlg);
        auto* view = new QPlainTextEdit(text.isEmpty() ? QStringLiteral("(no file changes)") : text, dlg);
        view->setReadOnly(true);
        view->setFont(QFont("Monospace", 10));
        view->setLineWrapMode(QPlainTextEdit::NoWrap);
        new CodeDiffHighlighter(view->document(), /*diffMode=*/true);
        layout->addWidget(view);
        dlg->show();
    });
}

} // namespace agora
