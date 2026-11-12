#pragma once

#include "core/types.h"

#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextBrowser>
#include <QTimer>
#include <QVector>
#include <QWidget>

namespace agora {

class TranscriptView : public QWidget {
    Q_OBJECT
public:
    explicit TranscriptView(QWidget* parent = nullptr);

    void setStatusText(const QString& text);
    void setMentionParticipants(const QVector<Participant>& participants);

    // Full rebuild (when switching rooms or clearing)
    void setMessages(const QVector<ChatMessage>& messages);
    // Incremental append / single-message update during streaming
    void onMessageAdded(int index, const ChatMessage& message, const QVector<ChatMessage>& allMessages);
    void onMessageUpdated(int index, const ChatMessage& message, const QVector<ChatMessage>& allMessages);

signals:
    void sendUserMessageRequested(const QString& text, bool autoStartDebate);

private:
    static QString formatMessageHtml(const ChatMessage& msg, bool isLatestTurn);
    void scheduleFlush();
    void flushHtmlToBrowser();

    QLabel* m_lblActiveStatus = nullptr;
    QTextBrowser* m_browser = nullptr;
    QWidget* m_mentionChipsContainer = nullptr;
    QPlainTextEdit* m_inputComposer = nullptr;
    QPushButton* m_btnSendStart = nullptr;
    QPushButton* m_btnInterject = nullptr;
    QTimer* m_renderTimer = nullptr;

    QVector<QString> m_cachedBlocks;
    bool m_stickToBottom = true;
    bool m_updatingHtml = false;
};

} // namespace agora
