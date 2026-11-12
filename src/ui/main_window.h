#pragma once

#include "core/orchestrator.h"
#include "core/router_client.h"
#include "core/sandbox.h"
#include "core/storage.h"
#include "core/tools.h"
#include "core/types.h"
#include "ui/transcript_view.h"
#include "ui/workspace_pane.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileSystemWatcher>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QSpinBox>
#include <memory>

namespace agora {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

private slots:
    void onRefreshModels();
    void onStartRouterClicked();
    void onOpenSettingsClicked();
    void onStartDebateClicked();
    void onStepTurnClicked();
    void onStopDebateClicked();
    void onSendUserMessage(const QString& text, bool autoStartDebate);

    void onAddParticipantClicked();
    void onEditParticipantClicked();
    void onRemoveParticipantClicked();
    void onResetCouncilPresetClicked();

    void onNewSessionClicked();
    void onSaveSessionClicked();
    void onExportMarkdownClicked();
    void onSessionListDoubleClicked(QListWidgetItem* item);

    void onBrowseWorkspaceClicked();

private:
    void setupUi();
    void applyDarkTheme();
    void loadAppSettings();
    void saveAppSettings();
    void populateDefaultCouncil();
    void refreshParticipantsList();
    void refreshSessionsList();
    bool showParticipantDialog(Participant* inOutParticipant, const QString& dialogTitle);

    std::unique_ptr<SessionStore> m_store;
    BwrapSandbox* m_sandbox = nullptr;
    ToolEngine* m_toolEngine = nullptr;
    LlamaRouterClient* m_router = nullptr;
    Orchestrator* m_orchestrator = nullptr;
    QFileSystemWatcher* m_fsWatcher = nullptr;

    QVector<ModelInfo> m_availableModels;

    // Top bar widgets
    QLabel* m_routerBadge = nullptr;
    QPushButton* m_btnRefreshModels = nullptr;
    QPushButton* m_btnStartRouter = nullptr;
    QPushButton* m_btnSettings = nullptr;
    QComboBox* m_comboMode = nullptr;
    QSpinBox* m_spinMaxRounds = nullptr;
    QDoubleSpinBox* m_spinVramBudget = nullptr;
    QPushButton* m_btnStart = nullptr;
    QPushButton* m_btnStep = nullptr;
    QPushButton* m_btnStop = nullptr;
    QPushButton* m_btnExportMd = nullptr;

    // Left pane: Roster & Sessions
    QListWidget* m_listParticipants = nullptr;
    QListWidget* m_listSessions = nullptr;

    // Center & Right modular panes
    TranscriptView* m_transcriptPane = nullptr;
    WorkspacePane* m_workspacePane = nullptr;
};

} // namespace agora
