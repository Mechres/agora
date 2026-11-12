#include "ui/transcript_view.h"

#include <QHBoxLayout>
#include <QRegularExpression>
#include <QScrollBar>
#include <QVBoxLayout>

namespace agora {

TranscriptView::TranscriptView(QWidget* parent) : QWidget(parent) {
    m_renderTimer = new QTimer(this);
    m_renderTimer->setSingleShot(true);
    m_renderTimer->setInterval(50);
    connect(m_renderTimer, &QTimer::timeout, this, &TranscriptView::flushHtmlToBrowser);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    auto* statusHeaderRow = new QHBoxLayout();
    m_lblActiveStatus = new QLabel("Ready — configure participants or send a prompt to begin.", this);
    m_lblActiveStatus->setStyleSheet("color: #93c5fd; font-weight: bold; padding: 2px 4px;");
    auto* btnScrollBottom = new QPushButton("⬇ Bottom", this);
    btnScrollBottom->setToolTip("Jump to bottom and re-enable auto-scroll");
    statusHeaderRow->addWidget(m_lblActiveStatus, 1);
    statusHeaderRow->addWidget(btnScrollBottom);

    m_browser = new QTextBrowser(this);
    m_browser->setOpenExternalLinks(false);

    QScrollBar* vScroll = m_browser->verticalScrollBar();
    connect(vScroll, &QScrollBar::valueChanged, this, [this, vScroll](int val) {
        if (m_updatingHtml) return;
        m_stickToBottom = (val >= vScroll->maximum() - 40);
    });
    connect(vScroll, &QScrollBar::sliderReleased, this, [this, vScroll]() {
        m_stickToBottom = (vScroll->value() >= vScroll->maximum() - 40);
        if (m_renderTimer->isActive()) {
            flushHtmlToBrowser();
        }
    });
    connect(btnScrollBottom, &QPushButton::clicked, this, [this, vScroll]() {
        m_stickToBottom = true;
        vScroll->setValue(vScroll->maximum());
    });

    m_mentionChipsContainer = new QWidget(this);
    auto* chipsLayout = new QHBoxLayout(m_mentionChipsContainer);
    chipsLayout->setContentsMargins(0, 0, 0, 0);
    chipsLayout->setSpacing(6);

    m_inputComposer = new QPlainTextEdit(this);
    m_inputComposer->setPlaceholderText(
        "Type a task, topic, or code specification for the models... "
        "Use @handle (e.g. @architect, @coder, @reviewer) to direct who speaks first.");
    m_inputComposer->setMaximumHeight(105);

    auto* composerBtnRow = new QHBoxLayout();
    m_btnSendStart = new QPushButton("Send & Start Debate", this);
    m_btnInterject = new QPushButton("Interject (Add Without Restarting)", this);
    composerBtnRow->addStretch();
    composerBtnRow->addWidget(m_btnInterject);
    composerBtnRow->addWidget(m_btnSendStart);

    layout->addLayout(statusHeaderRow);
    layout->addWidget(m_browser, 1);
    layout->addWidget(m_mentionChipsContainer);
    layout->addWidget(m_inputComposer);
    layout->addLayout(composerBtnRow);

    connect(m_btnSendStart, &QPushButton::clicked, this, [this]() {
        const QString text = m_inputComposer->toPlainText().trimmed();
        m_inputComposer->clear();
        emit sendUserMessageRequested(text, true);
    });
    connect(m_btnInterject, &QPushButton::clicked, this, [this]() {
        const QString text = m_inputComposer->toPlainText().trimmed();
        m_inputComposer->clear();
        emit sendUserMessageRequested(text, false);
    });
}

void TranscriptView::setStatusText(const QString& text) {
    m_lblActiveStatus->setText(text);
}

void TranscriptView::setMentionParticipants(const QVector<Participant>& participants) {
    auto* layout = qobject_cast<QHBoxLayout*>(m_mentionChipsContainer->layout());
    if (!layout) return;
    QLayoutItem* child;
    while ((child = layout->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }

    auto* lbl = new QLabel("Quick @mention:", m_mentionChipsContainer);
    lbl->setStyleSheet("color: #94a3b8;");
    layout->addWidget(lbl);

    for (const auto& p : participants) {
        if (!p.enabled) continue;
        auto* btn = new QPushButton(p.handle, m_mentionChipsContainer);
        btn->setStyleSheet(QStringLiteral("background-color: #1e293b; color: %1; border: 1px solid %1; padding: 2px 8px;")
                               .arg(p.colorHex));
        connect(btn, &QPushButton::clicked, this, [this, handle = p.handle]() {
            m_inputComposer->insertPlainText(handle + " ");
            m_inputComposer->setFocus();
        });
        layout->addWidget(btn);
    }
    layout->addStretch();
}

QString TranscriptView::formatMessageHtml(const ChatMessage& msg, bool isLatestTurn) {
    const QString borderColor = msg.colorHex.isEmpty() ? QStringLiteral("#60a5fa") : msg.colorHex;
    QString html = QStringLiteral("<div style='border-left: 4px solid %1; background-color: #162032; "
                                  "padding: 8px 12px; margin-bottom: 10px; border-radius: 6px;'>")
                       .arg(borderColor);

    html += QStringLiteral("<div style='margin-bottom: 4px;'>"
                           "<span style='color: %1; font-weight: bold;'>%2</span> "
                           "<span style='color: #cbd5e1;'>(%3)</span> ")
                .arg(borderColor,
                     msg.senderHandle.toHtmlEscaped(),
                     msg.displayName.toHtmlEscaped());

    if (!msg.modelId.isEmpty()) {
        html += QStringLiteral("<span style='background-color: #0f172a; color: #94a3b8; "
                               "padding: 1px 6px; border-radius: 4px; font-size: 11px;'>%1</span> ")
                    .arg(msg.modelId.toHtmlEscaped());
    }
    html += QStringLiteral("<span style='color: #64748b; font-size: 11px; float: right;'>%1</span></div>")
                .arg(msg.timestamp.toHtmlEscaped());

    const QString trimmedReasoning = msg.reasoningContent.trimmed();
    if (!trimmedReasoning.isEmpty()) {
        QString displayReasoning = trimmedReasoning;
        if (!isLatestTurn && displayReasoning.size() > 600) {
            displayReasoning = displayReasoning.left(600) + QStringLiteral(" ... [reasoning shortened]");
        }
        html += QStringLiteral("<div style='background-color: #0f172a; color: #94a3b8; "
                               "padding: 6px 8px; margin: 4px 0; border-radius: 4px; font-size: 12px; font-style: italic;'>"
                               "<b>💭 Reasoning:</b><br>%1</div>")
                    .arg(displayReasoning.toHtmlEscaped().replace("\n", "<br>"));
    }

    if (!msg.content.isEmpty()) {
        QString formatted = msg.content.toHtmlEscaped();
        static const QRegularExpression mentionRe(R"((@[A-Za-z0-9_\-]+))");
        formatted.replace(mentionRe, "<b style='color: #38bdf8;'>\\1</b>");
        formatted.replace("\n", "<br>");
        html += QStringLiteral("<div style='color: #f1f5f9; margin-top: 4px;'>%1</div>").arg(formatted);
    }

    for (int i = 0; i < msg.toolCalls.size(); ++i) {
        const auto& tc = msg.toolCalls[i];
        html += QStringLiteral("<div style='background-color: #0b1324; border: 1px solid #334155; "
                               "padding: 6px 8px; margin-top: 6px; border-radius: 4px; font-family: monospace; font-size: 12px;'>"
                               "<span style='color: #fbbf24; font-weight: bold;'>🛠 Tool Call: %1</span>"
                               "<br><span style='color: #94a3b8;'>%2</span>")
                    .arg(tc.name.toHtmlEscaped(), tc.argumentsJson.toHtmlEscaped());

        if (i < msg.toolResults.size()) {
            const auto& tr = msg.toolResults[i];
            const QString statusColor = tr.isError ? QStringLiteral("#f87171") : QStringLiteral("#4ade80");
            html += QStringLiteral("<hr style='border: 0; border-top: 1px solid #1e293b;'>"
                                   "<span style='color: %1;'>↳ Result (exit=%2):</span><br>"
                                   "<pre style='margin: 2px 0; white-space: pre-wrap; color: #cbd5e1;'>%3</pre>")
                        .arg(statusColor,
                             QString::number(tr.exitCode),
                             tr.output.left(1500).toHtmlEscaped());
        }
        html += "</div>";
    }

    html += "</div>";
    return html;
}

void TranscriptView::setMessages(const QVector<ChatMessage>& messages) {
    m_cachedBlocks.resize(messages.size());
    for (int i = 0; i < messages.size(); ++i) {
        m_cachedBlocks[i] = formatMessageHtml(messages[i], i == messages.size() - 1);
    }
    m_stickToBottom = true;
    flushHtmlToBrowser();
}

void TranscriptView::onMessageAdded(int index, const ChatMessage& message, const QVector<ChatMessage>& allMessages) {
    if (index < 0 || index != m_cachedBlocks.size()) {
        setMessages(allMessages);
        return;
    }
    // Re-format previous last message so its reasoning can be compactly collapsed
    if (!m_cachedBlocks.isEmpty() && allMessages.size() >= 2) {
        const int prevIdx = m_cachedBlocks.size() - 1;
        m_cachedBlocks[prevIdx] = formatMessageHtml(allMessages[prevIdx], false);
    }
    m_cachedBlocks.append(formatMessageHtml(message, true));
    m_stickToBottom = true;
    flushHtmlToBrowser();
}

void TranscriptView::onMessageUpdated(int index, const ChatMessage& message, const QVector<ChatMessage>& allMessages) {
    if (index < 0 || index >= m_cachedBlocks.size()) {
        setMessages(allMessages);
        return;
    }
    // Only re-render the single modified message block!
    m_cachedBlocks[index] = formatMessageHtml(message, index == allMessages.size() - 1);
    scheduleFlush();
}

void TranscriptView::scheduleFlush() {
    if (!m_renderTimer->isActive()) {
        m_renderTimer->start();
    }
}

void TranscriptView::flushHtmlToBrowser() {
    QScrollBar* sb = m_browser->verticalScrollBar();
    if (sb->isSliderDown()) {
        scheduleFlush();
        return;
    }

    const int prevScrollValue = sb->value();
    const bool stickBottom = m_stickToBottom;

    QString html = QStringLiteral("<div style='font-family: sans-serif; line-height: 1.45;'>");
    for (const QString& block : m_cachedBlocks) {
        html += block;
    }
    html += QStringLiteral("</div>");

    m_updatingHtml = true;
    m_browser->setHtml(html);
    if (stickBottom) {
        sb->setValue(sb->maximum());
    } else {
        sb->setValue(qMin(prevScrollValue, sb->maximum()));
    }
    m_updatingHtml = false;
}

} // namespace agora
