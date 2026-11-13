#pragma once

#include "core/checkpoints.h"
#include "core/orchestrator.h"
#include "core/router_client.h"
#include "core/sandbox.h"
#include "core/storage.h"
#include "core/tools.h"
#include "core/types.h"
#include "ui/transcript_view.h"
#include "ui/workspace_pane.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QHash>
#include <QPointer>
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

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onRefreshModels();
    void onStartRouterClicked();
    void onOpenSettingsClicked();
    void onManageProvidersClicked();
    void onStartDebateClicked();
    void onStepTurnClicked();
    void onStopDebateClicked();
    void onSendUserMessage(const QString& text, bool autoStartDebate);

    void onAddParticipantClicked();
    void onEditParticipantClicked();
    void onRemoveParticipantClicked();

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
    // Builds the template's roles; participants already in the room with the same handle are kept as-is
    void loadCouncil(const CouncilTemplate& council);
    // Call after any change to the roster: refreshes the UI and persists it
    void onRosterChanged();
    void saveRoster() const;
    QVector<Participant> loadSavedRoster() const;
    // Saves the current room (if it has messages) and remembers it for the next start
    void persistSession();
    void openSession(const RoomSession& session);
    void syncRoomControls();
    QString newWorkspacePath() const;
    void refreshParticipantsList();
    void refreshSessionsList();
    bool showParticipantDialog(Participant* inOutParticipant, const QString& dialogTitle);
    bool showProviderEditDialog(ProviderConfig* inOutProvider, const QString& dialogTitle);
    QString providerName(const QString& providerId) const;
    void showToolApprovalDialog(quint64 requestId, const QString& speakerHandle, const ToolCall& call);
    void showTurnDiff(int messageIndex);

    std::unique_ptr<SessionStore> m_store;
    BwrapSandbox* m_sandbox = nullptr;
    CheckpointStore* m_checkpoints = nullptr;
    ToolEngine* m_toolEngine = nullptr;
    LlamaRouterClient* m_router = nullptr;
    Orchestrator* m_orchestrator = nullptr;
    QFileSystemWatcher* m_fsWatcher = nullptr;

    QVector<ModelInfo> m_availableModels;
    QHash<quint64, QPointer<QDialog>> m_approvalDialogs;

    // Top bar widgets
    QLabel* m_routerBadge = nullptr;
    QPushButton* m_btnRefreshModels = nullptr;
    QPushButton* m_btnStartRouter = nullptr;
    QPushButton* m_btnSettings = nullptr;
    QPushButton* m_btnProviders = nullptr;
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
