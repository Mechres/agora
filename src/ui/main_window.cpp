#include "ui/main_window.h"

#include <QCheckBox>
#include <QColor>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
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
    setWindowTitle("Agora — Multi-Model Local Debate & Sandboxed Coding Studio");
    resize(1540, 920);

    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dataDir);
    m_store = std::make_unique<SessionStore>(dataDir + "/sessions.db");
    m_store->initialize();

    const QString defaultWs = dataDir + "/workspaces/session_" +
                              QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QDir().mkpath(defaultWs);

    m_sandbox = new BwrapSandbox(defaultWs, this);
    m_sandbox->ensureWorkspaceInitialized();

    m_toolEngine = new ToolEngine(m_sandbox, this);
    m_router = new LlamaRouterClient(this);
    loadAppSettings();

    m_orchestrator = new Orchestrator(m_router, m_toolEngine, this);
    m_orchestrator->session().workspacePath = defaultWs;

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
                    m_routerBadge->setText(QStringLiteral("● Router Online (%1 models | %2 loaded, %3 GiB)")
                                               .arg(QString::number(models.size()),
                                                    QString::number(loadedCount),
                                                    QString::number(loadedGiB, 'f', 1)));
                    m_routerBadge->setStyleSheet("color: #4ade80; font-weight: bold; padding: 4px 8px;");
                } else {
                    m_routerBadge->setText(QStringLiteral("○ Router Offline (%1 local GGUFs found)")
                                               .arg(QString::number(models.size())));
                    m_routerBadge->setStyleSheet("color: #f87171; font-weight: bold; padding: 4px 8px;");
                }
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
                    m_store->saveSession(m_orchestrator->session());
                    QTimer::singleShot(0, this, &MainWindow::refreshSessionsList);
                } else {
                    m_transcriptPane->setStatusText(
                        QStringLiteral("Round %1 / %2 active...")
                            .arg(QString::number(currentRound + 1), QString::number(maxRounds)));
                }
            });

    onRefreshModels();
    populateDefaultCouncil();
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
}

void MainWindow::saveAppSettings() {
    QSettings settings;
    settings.setValue("router/baseUrl", m_router->baseUrl());
    settings.setValue("router/apiKey", m_router->apiKey());
    settings.setValue("router/modelsDir", m_router->modelsDir());
    settings.setValue("router/scriptPath", m_router->routerScriptPath());
}

void MainWindow::setupUi() {
    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(6);

    // 1. Top Control Bar
    auto* topBar = new QWidget(central);
    auto* topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(4, 2, 4, 2);
    topLayout->setSpacing(8);

    m_routerBadge = new QLabel("Checking router...", topBar);
    m_btnRefreshModels = new QPushButton("Refresh Models", topBar);
    m_btnStartRouter = new QPushButton("Start Router", topBar);
    m_btnSettings = new QPushButton("⚙ Settings", topBar);

    m_comboMode = new QComboBox(topBar);
    m_comboMode->addItem("Round-Robin + @mention", static_cast<int>(DebateMode::RoundRobinWithMentions));
    m_comboMode->addItem("@mention Only", static_cast<int>(DebateMode::MentionsOnly));
    m_comboMode->addItem("Single Turn", static_cast<int>(DebateMode::SingleTurn));

    m_spinMaxRounds = new QSpinBox(topBar);
    m_spinMaxRounds->setRange(1, 50);
    m_spinMaxRounds->setValue(8);
    m_spinMaxRounds->setSuffix(" turns");

    m_spinVramBudget = new QDoubleSpinBox(topBar);
    m_spinVramBudget->setRange(1.0, 64.0);
    m_spinVramBudget->setSingleStep(0.5);
    m_spinVramBudget->setValue(8.5);
    m_spinVramBudget->setSuffix(" GiB VRAM");
    m_spinVramBudget->setToolTip("Max combined GGUF size kept resident before auto-unloading previous models");

    m_btnStart = new QPushButton("▶ Start / Continue", topBar);
    m_btnStep = new QPushButton("⏭ Step 1 Turn", topBar);
    m_btnStop = new QPushButton("⏹ Stop", topBar);
    m_btnStop->setEnabled(false);
    m_btnExportMd = new QPushButton("Export .md", topBar);

    topLayout->addWidget(m_routerBadge);
    topLayout->addWidget(m_btnRefreshModels);
    topLayout->addWidget(m_btnStartRouter);
    topLayout->addWidget(m_btnSettings);
    topLayout->addStretch();
    topLayout->addWidget(new QLabel("Mode:", topBar));
    topLayout->addWidget(m_comboMode);
    topLayout->addWidget(new QLabel("Max:", topBar));
    topLayout->addWidget(m_spinMaxRounds);
    topLayout->addWidget(m_spinVramBudget);
    topLayout->addWidget(m_btnStart);
    topLayout->addWidget(m_btnStep);
    topLayout->addWidget(m_btnStop);
    topLayout->addWidget(m_btnExportMd);

    rootLayout->addWidget(topBar);

    // 2. Main 3-Pane Splitter
    auto* mainSplitter = new QSplitter(Qt::Horizontal, central);

    // --- LEFT PANE: Participants & Sessions ---
    auto* leftPane = new QWidget(mainSplitter);
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(4, 4, 4, 4);

    auto* lblRoster = new QLabel("<b>Room Participants (Models)</b>", leftPane);
    m_listParticipants = new QListWidget(leftPane);

    auto* rosterBtnRow = new QHBoxLayout();
    auto* btnAddP = new QPushButton("+ Add", leftPane);
    auto* btnEditP = new QPushButton("Edit", leftPane);
    auto* btnDelP = new QPushButton("Remove", leftPane);
    rosterBtnRow->addWidget(btnAddP);
    rosterBtnRow->addWidget(btnEditP);
    rosterBtnRow->addWidget(btnDelP);

    auto* btnPresetCouncil = new QPushButton("⚡ Load Default Coding Council", leftPane);

    auto* lblSessions = new QLabel("<b>Saved Sessions</b>", leftPane);
    m_listSessions = new QListWidget(leftPane);
    auto* sessionBtnRow = new QHBoxLayout();
    auto* btnNewSession = new QPushButton("New Room", leftPane);
    auto* btnSaveSession = new QPushButton("Save Room", leftPane);
    sessionBtnRow->addWidget(btnNewSession);
    sessionBtnRow->addWidget(btnSaveSession);

    leftLayout->addWidget(lblRoster);
    leftLayout->addWidget(m_listParticipants, 3);
    leftLayout->addLayout(rosterBtnRow);
    leftLayout->addWidget(btnPresetCouncil);
    leftLayout->addSpacing(8);
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
    connect(btnPresetCouncil, &QPushButton::clicked, this, &MainWindow::onResetCouncilPresetClicked);
    connect(m_listParticipants, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
        onEditParticipantClicked();
    });

    connect(btnNewSession, &QPushButton::clicked, this, &MainWindow::onNewSessionClicked);
    connect(btnSaveSession, &QPushButton::clicked, this, &MainWindow::onSaveSessionClicked);
    connect(m_listSessions, &QListWidget::itemDoubleClicked, this, &MainWindow::onSessionListDoubleClicked);

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

void MainWindow::applyDarkTheme() {
    setStyleSheet(R"(
        QMainWindow, QWidget {
            background-color: #0f172a;
            color: #e2e8f0;
            font-size: 13px;
        }
        QListWidget, QPlainTextEdit, QTextBrowser, QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
            background-color: #1e293b;
            color: #f8fafc;
            border: 1px solid #334155;
            border-radius: 6px;
            padding: 4px;
        }
        QPushButton {
            background-color: #1d4ed8;
            color: #ffffff;
            border: 1px solid #2563eb;
            border-radius: 6px;
            padding: 5px 10px;
            font-weight: 600;
        }
        QPushButton:hover {
            background-color: #2563eb;
        }
        QPushButton:disabled {
            background-color: #334155;
            color: #64748b;
            border-color: #334155;
        }
        QTabWidget::pane {
            border: 1px solid #334155;
            border-radius: 6px;
        }
        QTabBar::tab {
            background: #1e293b;
            color: #94a3b8;
            padding: 6px 12px;
            border-top-left-radius: 6px;
            border-top-right-radius: 6px;
            margin-right: 2px;
        }
        QTabBar::tab:selected {
            background: #334155;
            color: #f8fafc;
            font-weight: bold;
        }
    )");
}

void MainWindow::onRefreshModels() {
    m_router->fetchModelsBlocking();
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
    dlg.resize(520, 220);

    auto* form = new QFormLayout(&dlg);
    auto* editUrl = new QLineEdit(m_router->baseUrl(), &dlg);
    auto* editKey = new QLineEdit(m_router->apiKey(), &dlg);
    auto* editModelsDir = new QLineEdit(m_router->modelsDir(), &dlg);
    auto* editScript = new QLineEdit(m_router->routerScriptPath(), &dlg);

    form->addRow("Router Base URL:", editUrl);
    form->addRow("Router API Key:", editKey);
    form->addRow("Local GGUF Models Folder:", editModelsDir);
    form->addRow("Router Start Script:", editScript);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);

    if (dlg.exec() != QDialog::Accepted) return;

    m_router->setBaseUrl(editUrl->text());
    m_router->setApiKey(editKey->text());
    m_router->setModelsDir(editModelsDir->text());
    m_router->setRouterScriptPath(editScript->text());
    saveAppSettings();
    onRefreshModels();
}

void MainWindow::populateDefaultCouncil() {
    auto& parts = m_orchestrator->session().participants;
    parts.clear();

    auto pickModel = [this](const QStringList& preferredSubstrings, int fallbackIdx) -> QString {
        for (const QString& sub : preferredSubstrings) {
            for (const auto& m : m_availableModels) {
                if (m.id.contains(sub, Qt::CaseInsensitive)) {
                    return m.id;
                }
            }
        }
        if (!m_availableModels.isEmpty()) {
            return m_availableModels[fallbackIdx % m_availableModels.size()].id;
        }
        return "Qwen3.5-4B-Q4_K_M";
    };

    const auto presets = defaultRolePresets();
    if (presets.size() >= 3) {
        Participant arch;
        arch.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        arch.handle = presets[0].handle;
        arch.displayName = presets[0].displayName;
        arch.colorHex = presets[0].colorHex;
        arch.temperature = presets[0].temperature;
        arch.systemPrompt = presets[0].systemPrompt;
        arch.modelId = pickModel({"Ornith", "Qwen3.8", "Xing", "Qwen3.5"}, 0);
        parts.append(arch);

        Participant coder;
        coder.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        coder.handle = presets[1].handle;
        coder.displayName = presets[1].displayName;
        coder.colorHex = presets[1].colorHex;
        coder.temperature = presets[1].temperature;
        coder.systemPrompt = presets[1].systemPrompt;
        coder.modelId = pickModel({"Qwopus", "coder", "Qwen3.5"}, 1);
        parts.append(coder);

        Participant rev;
        rev.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        rev.handle = presets[2].handle;
        rev.displayName = presets[2].displayName;
        rev.colorHex = presets[2].colorHex;
        rev.temperature = presets[2].temperature;
        rev.systemPrompt = presets[2].systemPrompt;
        rev.modelId = pickModel({"Qwen3.5-4B", "LFM2.5", "minicpm"}, 2);
        parts.append(rev);
    }

    refreshParticipantsList();
    m_transcriptPane->setMentionParticipants(parts);
}

void MainWindow::refreshParticipantsList() {
    m_listParticipants->clear();
    QMap<QString, ModelInfo> modelMap;
    for (const auto& m : m_availableModels) {
        modelMap[m.id] = m;
    }

    for (const auto& p : m_orchestrator->session().participants) {
        const ModelInfo mi = modelMap.value(p.modelId);
        const QString stateStr = mi.status.isEmpty() ? QStringLiteral("unloaded") : mi.status;
        const bool isSpeaking = (p.id == m_orchestrator->activeSpeakerId());
        const QString sizeTag = mi.sizeGiB > 0.01
                                    ? QStringLiteral(" · %1 GiB").arg(QString::number(mi.sizeGiB, 'f', 1))
                                    : QString();

        const QString label = QStringLiteral("%1 %2 (%3)\n   Model: %4 [%5%6]")
                                  .arg(isSpeaking ? QStringLiteral("🔊") : (p.enabled ? QStringLiteral("●") : QStringLiteral("○")),
                                       p.handle,
                                       p.displayName,
                                       p.modelId,
                                       stateStr,
                                       sizeTag);

        auto* item = new QListWidgetItem(label, m_listParticipants);
        item->setForeground(QColor(p.colorHex));
    }
}

void MainWindow::refreshSessionsList() {
    m_listSessions->clear();
    const auto summaries = m_store->listSessionSummaries();
    for (const auto& s : summaries) {
        auto* item = new QListWidgetItem(
            QStringLiteral("%1 (%2 msgs)").arg(s.title, QString::number(s.messageCount)),
            m_listSessions);
        item->setData(Qt::UserRole, s.id);
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
    m_store->saveSession(m_orchestrator->session());
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
    auto* comboModel = new QComboBox(&dlg);
    comboModel->setEditable(true);
    for (const auto& m : m_availableModels) {
        comboModel->addItem(m.id);
    }
    comboModel->setCurrentText(inOutParticipant->modelId);

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
    form->addRow("Local Model:", comboModel);
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
        refreshParticipantsList();
        m_transcriptPane->setMentionParticipants(m_orchestrator->session().participants);
    }
}

void MainWindow::onEditParticipantClicked() {
    const int row = m_listParticipants->currentRow();
    auto& parts = m_orchestrator->session().participants;
    if (row < 0 || row >= parts.size()) return;

    Participant copy = parts[row];
    if (showParticipantDialog(&copy, "Edit Participant")) {
        parts[row] = copy;
        refreshParticipantsList();
        m_transcriptPane->setMentionParticipants(parts);
    }
}

void MainWindow::onRemoveParticipantClicked() {
    const int row = m_listParticipants->currentRow();
    auto& parts = m_orchestrator->session().participants;
    if (row < 0 || row >= parts.size()) return;
    parts.removeAt(row);
    refreshParticipantsList();
    m_transcriptPane->setMentionParticipants(parts);
}

void MainWindow::onResetCouncilPresetClicked() {
    populateDefaultCouncil();
}

void MainWindow::onNewSessionClicked() {
    m_orchestrator->stopDebate();
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString newWs = dataDir + "/workspaces/session_" +
                          QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QDir().mkpath(newWs);

    RoomSession s;
    s.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    s.title = "New Agora Debate";
    s.workspacePath = newWs;
    s.vramBudgetGiB = m_spinVramBudget->value();
    s.maxRounds = m_spinMaxRounds->value();
    s.participants = m_orchestrator->session().participants;

    if (!m_fsWatcher->directories().isEmpty()) {
        m_fsWatcher->removePaths(m_fsWatcher->directories());
    }
    m_sandbox->setWorkspacePath(newWs);
    m_sandbox->ensureWorkspaceInitialized();
    m_fsWatcher->addPath(newWs);

    m_workspacePane->clearSelectedFile();
    m_workspacePane->setWorkspacePath(newWs);
    m_orchestrator->setSession(s);

    m_workspacePane->refreshViews();
    m_transcriptPane->setMessages(m_orchestrator->session().messages);
}

void MainWindow::onSaveSessionClicked() {
    QString err;
    if (!m_store->saveSession(m_orchestrator->session(), &err)) {
        QMessageBox::warning(this, "Save Failed", err);
        return;
    }
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
    if (id.isEmpty()) return;

    QTimer::singleShot(0, this, [this, id]() {
        auto loaded = m_store->loadSession(id);
        if (!loaded) return;

        m_workspacePane->clearSelectedFile();

        if (!m_fsWatcher->directories().isEmpty()) {
            m_fsWatcher->removePaths(m_fsWatcher->directories());
        }
        if (!m_fsWatcher->files().isEmpty()) {
            m_fsWatcher->removePaths(m_fsWatcher->files());
        }

        m_sandbox->setWorkspacePath(loaded->workspacePath);
        m_sandbox->ensureWorkspaceInitialized();
        if (!loaded->workspacePath.isEmpty()) {
            m_fsWatcher->addPath(loaded->workspacePath);
        }

        m_orchestrator->setSession(*loaded);
        m_workspacePane->setWorkspacePath(loaded->workspacePath);
        m_workspacePane->setAllowNetwork(loaded->allowSandboxNetwork);
        m_spinMaxRounds->setValue(loaded->maxRounds);
        m_spinVramBudget->setValue(loaded->vramBudgetGiB);

        refreshParticipantsList();
        m_transcriptPane->setMentionParticipants(loaded->participants);
        m_workspacePane->refreshViews();
        m_transcriptPane->setMessages(loaded->messages);
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

} // namespace agora
