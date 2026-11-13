#include "ui/transcript_view.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScrollBar>
#include <QShortcut>
#include <QTextDocument>
#include <QUrl>
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
    m_lblActiveStatus->setStyleSheet("color: #93c5fd; font-weight: 600; padding: 2px 4px;");
    m_lblActiveStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* btnScrollBottom = new QPushButton("↓ Latest", this);
    btnScrollBottom->setProperty("role", "ghost");
    btnScrollBottom->setToolTip("Jump to the latest message and re-enable auto-scroll");
    statusHeaderRow->addWidget(m_lblActiveStatus, 1);
    statusHeaderRow->addWidget(btnScrollBottom);

    m_browser = new QTextBrowser(this);
    m_browser->setStyleSheet("QTextBrowser { background-color: #0f172a; border: 1px solid #1e293b; padding: 6px; }");
    m_browser->setOpenExternalLinks(false);
    m_browser->setOpenLinks(false);
    connect(m_browser, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        if (url.scheme() == "agora") {
            bool ok = false;
            const int index = url.path().mid(1).toInt(&ok);
            if (!ok) return;
            if (url.host() == "diff") emit viewTurnChangesRequested(index);
            if (url.host() == "undo") emit undoTurnRequested(index);
        } else if (url.scheme() == "http" || url.scheme() == "https") {
            QDesktopServices::openUrl(url);
        }
    });

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
        "Describe a task or topic for the room… Start with an @handle to choose who speaks first.");
    m_inputComposer->setMaximumHeight(105);

    auto* composerBtnRow = new QHBoxLayout();
    auto* hint = new QLabel("Ctrl+Enter to send", this);
    hint->setStyleSheet("color: #64748b; font-size: 11px;");
    m_btnSendStart = new QPushButton("Send && Start", this);
    m_btnSendStart->setProperty("role", "primary");
    m_btnSendStart->setToolTip("Add your message and start (or continue) the debate");
    m_btnInterject = new QPushButton("Interject", this);
    m_btnInterject->setToolTip("Add your message to the room without starting a run");
    composerBtnRow->addWidget(hint);
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
    auto* sendShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), m_inputComposer);
    sendShortcut->setContext(Qt::WidgetShortcut);
    connect(sendShortcut, &QShortcut::activated, m_btnSendStart, &QPushButton::click);
    auto* sendShortcutKeypad = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Enter), m_inputComposer);
    sendShortcutKeypad->setContext(Qt::WidgetShortcut);
    connect(sendShortcutKeypad, &QShortcut::activated, m_btnSendStart, &QPushButton::click);

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

    auto* lbl = new QLabel("Mention:", m_mentionChipsContainer);
    lbl->setStyleSheet("color: #64748b; font-size: 12px;");
    layout->addWidget(lbl);

    for (const auto& p : participants) {
        if (!p.enabled) continue;
        auto* btn = new QPushButton(p.handle, m_mentionChipsContainer);
        btn->setStyleSheet(QStringLiteral("QPushButton { background-color: transparent; color: %1; border: 1px solid %1; "
                                          "border-radius: 10px; padding: 1px 10px; font-weight: 600; } "
                                          "QPushButton:hover { background-color: #1e293b; }")
                               .arg(p.colorHex));
        btn->setCursor(Qt::PointingHandCursor);
        connect(btn, &QPushButton::clicked, this, [this, handle = p.handle]() {
            m_inputComposer->insertPlainText(handle + " ");
            m_inputComposer->setFocus();
        });
        layout->addWidget(btn);
    }
    layout->addStretch();
}

QString TranscriptView::markdownToHtmlFragment(const QString& markdown) {
    QTextDocument doc;
    doc.setMarkdown(markdown, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub |
                                                              QTextDocument::MarkdownNoHTML));
    QString html = doc.toHtml();
    const qsizetype bodyTag = html.indexOf(QStringLiteral("<body"));
    const qsizetype bodyStart = bodyTag < 0 ? -1 : html.indexOf('>', bodyTag);
    const qsizetype bodyEnd = html.lastIndexOf(QStringLiteral("</body>"));
    if (bodyStart >= 0 && bodyEnd > bodyStart) {
        html = html.mid(bodyStart + 1, bodyEnd - bodyStart - 1);
    }
    // Model-written links may only point to the web, never at app actions (agora://),
    // local files or other schemes
    static const QRegularExpression unsafeHref(QStringLiteral("href=\"(?!https?://)[^\"]*\""),
                                               QRegularExpression::CaseInsensitiveOption);
    html.replace(unsafeHref, QStringLiteral("href=\"#blocked\""));
    html.replace(QStringLiteral("<pre style=\""),
                 QStringLiteral("<pre style=\"background-color:#0b1324; color:#e2e8f0; "));

    // Highlight @mentions in text runs only, never inside tags/attributes
    static const QRegularExpression tagRe(QStringLiteral("(<[^>]*>)"));
    static const QRegularExpression mentionRe(QStringLiteral("(^|[^A-Za-z0-9_.])(@[A-Za-z0-9_\\-]+)"));
    QString out;
    qsizetype pos = 0;
    auto it = tagRe.globalMatch(html);
    auto highlight = [](QString text) {
        return text.replace(mentionRe, QStringLiteral("\\1<b style='color: #38bdf8;'>\\2</b>"));
    };
    while (it.hasNext()) {
        const auto m = it.next();
        out += highlight(html.mid(pos, m.capturedStart() - pos));
        out += m.captured(0);
        pos = m.capturedEnd();
    }
    out += highlight(html.mid(pos));
    return out;
}

QString TranscriptView::formatStats(const TurnStats& st) {
    QStringList parts;
    auto tokens = [](int n) {
        return n >= 1000 ? QString::number(n / 1000.0, 'f', 1) + "k" : QString::number(n);
    };
    if (st.promptTokens > 0) parts << tokens(st.promptTokens) + " prompt tok";
    if (st.completionTokens > 0) parts << tokens(st.completionTokens) + " tok";
    if (st.tokensPerSecond > 0) parts << QString::number(st.tokensPerSecond, 'f', 1) + " tok/s";
    if (st.promptMs > 0) parts << "prefill " + QString::number(st.promptMs / 1000.0, 'f', 1) + "s";
    if (st.modelPrepMs >= 100) parts << "model prep " + QString::number(st.modelPrepMs / 1000.0, 'f', 1) + "s";
    return parts.join(QStringLiteral(" · "));
}

namespace {

// A short human summary of a tool call's arguments instead of raw JSON
QString describeToolCall(const ToolCall& tc) {
    const QJsonObject args = QJsonDocument::fromJson(tc.argumentsJson.toUtf8()).object();
    if (tc.name == "run_command") return "$ " + args["command"].toString();
    if (tc.name == "write_file") {
        return QStringLiteral("%1 (%2 bytes)").arg(args["path"].toString(),
                                                   QString::number(args["content"].toString().toUtf8().size()));
    }
    if (tc.name == "read_file" || tc.name == "patch_file" || tc.name == "list_files") {
        return args["path"].toString().isEmpty() ? QStringLiteral(".") : args["path"].toString();
    }
    QString raw = tc.argumentsJson;
    if (raw.size() > 300) raw = raw.left(300) + "…";
    return raw;
}

} // namespace

QString TranscriptView::formatMessageHtml(const ChatMessage& msg, int index, bool isLatestTurn) {
    const bool isUser = (msg.senderId == "user");
    const bool isSystem = (msg.senderId == "system");
    const QString accent = msg.colorHex.isEmpty() ? QStringLiteral("#60a5fa") : msg.colorHex;
    const QString cardBg = isUser ? QStringLiteral("#17243b") : (isSystem ? QStringLiteral("#131c2e")
                                                                          : QStringLiteral("#162032"));

    // Qt's rich text has no border-radius or reliable border-left, so a card is a table:
    // a thin accent column plus the body cell
    QString html = QStringLiteral("<table width='100%' cellspacing='0' cellpadding='0' style='margin-bottom: 10px;'>"
                                  "<tr><td width='3' bgcolor='%1'></td>"
                                  "<td bgcolor='%2' style='padding: 8px 12px;'>")
                       .arg(accent, cardBg);

    // Header: handle, name, model on the left; time on the right
    html += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='0'><tr><td>"
                           "<span style='color: %1; font-weight: bold;'>%2</span>"
                           "&nbsp;<span style='color: #94a3b8;'>%3</span>")
                .arg(accent, msg.senderHandle.toHtmlEscaped(), msg.displayName.toHtmlEscaped());
    if (!msg.modelId.isEmpty()) {
        html += QStringLiteral("&nbsp;&nbsp;<span style='color: #64748b; font-size: 11px;'>%1</span>")
                    .arg(msg.modelId.toHtmlEscaped());
    }
    html += QStringLiteral("</td><td align='right' style='color: #64748b; font-size: 11px;'>%1</td></tr></table>")
                .arg(msg.timestamp.toHtmlEscaped());

    const QString stats = formatStats(msg.stats);
    if (!stats.isEmpty()) {
        html += QStringLiteral("<div style='color: #64748b; font-size: 11px;'>⏱ %1</div>")
                    .arg(stats.toHtmlEscaped());
    }

    const QString trimmedReasoning = msg.reasoningContent.trimmed();
    if (!trimmedReasoning.isEmpty()) {
        QString displayReasoning = trimmedReasoning;
        if (!isLatestTurn && displayReasoning.size() > 600) {
            displayReasoning = displayReasoning.left(600) + QStringLiteral(" … [reasoning shortened]");
        }
        html += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='0' style='margin-top: 6px;'>"
                               "<tr><td bgcolor='#101828' style='padding: 6px 8px; color: #94a3b8; font-size: 12px;'>"
                               "<i><b>💭 Reasoning</b><br>%1</i></td></tr></table>")
                    .arg(displayReasoning.toHtmlEscaped().replace("\n", "<br>"));
    }

    if (!msg.content.isEmpty()) {
        html += QStringLiteral("<div style='color: #f1f5f9;'>%1</div>").arg(markdownToHtmlFragment(msg.content));
    } else if (msg.id.startsWith("pending_") && trimmedReasoning.isEmpty() && msg.toolCalls.isEmpty()) {
        html += QStringLiteral("<div style='color: #64748b; margin-top: 4px;'><i>Waiting for the model…</i></div>");
    }

    for (int i = 0; i < msg.toolCalls.size(); ++i) {
        const auto& tc = msg.toolCalls[i];
        html += QStringLiteral("<table width='100%' cellspacing='0' cellpadding='0' style='margin-top: 6px;'>"
                               "<tr><td bgcolor='#0b1324' style='padding: 6px 8px; font-size: 12px;'>"
                               "<span style='color: #fbbf24; font-weight: bold;'>🛠 %1</span>"
                               "&nbsp;<code style='color: #cbd5e1;'>%2</code>")
                    .arg(tc.name.toHtmlEscaped(), describeToolCall(tc).toHtmlEscaped());

        if (i < msg.toolResults.size()) {
            const auto& tr = msg.toolResults[i];
            const QString statusColor = tr.isError ? QStringLiteral("#f87171") : QStringLiteral("#4ade80");
            QString output = tr.output.trimmed();
            const bool truncated = output.size() > 1500;
            if (truncated) output = output.left(1500);
            html += QStringLiteral("<br><span style='color: %1; font-size: 11px;'>%2 exit %3</span>")
                        .arg(statusColor, tr.isError ? QStringLiteral("✗") : QStringLiteral("✓"),
                             QString::number(tr.exitCode));
            if (!output.isEmpty()) {
                html += QStringLiteral("<pre style='margin: 4px 0 0 0; white-space: pre-wrap; color: #94a3b8;'>%1%2</pre>")
                            .arg(output.toHtmlEscaped(),
                                 truncated ? QStringLiteral("\n… [output shortened]") : QString());
            }
        } else {
            html += QStringLiteral("<br><span style='color: #64748b; font-size: 11px;'>… running</span>");
        }
        html += "</td></tr></table>";
    }

    if (!msg.checkpointCommit.isEmpty()) {
        html += QStringLiteral("<div style='margin-top: 6px; font-size: 11px; color: #64748b;'>"
                               "📌 checkpoint %1 · "
                               "<a style='color: #93c5fd;' href='agora://diff/%2'>View changes</a> · "
                               "<a style='color: #fca5a5;' href='agora://undo/%2'>Undo this turn</a></div>")
                    .arg(msg.checkpointCommit.left(8).toHtmlEscaped(), QString::number(index));
    }

    html += "</td></tr></table>";
    return html;
}

QString TranscriptView::emptyStateHtml() {
    return QStringLiteral(
        "<table width='100%' height='100%'><tr><td align='center' valign='middle' style='padding-top: 80px;'>"
        "<div style='font-size: 22px; font-weight: bold; color: #e2e8f0;'>◆ Agora</div>"
        "<div style='color: #94a3b8; margin-top: 6px;'>A room where several models plan, build and review together.</div>"
        "<table cellspacing='0' cellpadding='4' style='margin-top: 18px; color: #94a3b8;'>"
        "<tr><td style='color: #38bdf8; font-weight: bold;'>1</td>"
        "<td>Pick participants on the left, or load a council of ready-made roles.</td></tr>"
        "<tr><td style='color: #38bdf8; font-weight: bold;'>2</td>"
        "<td>Describe a task below. Start with an @handle to choose who speaks first.</td></tr>"
        "<tr><td style='color: #38bdf8; font-weight: bold;'>3</td>"
        "<td>Press <b>Send &amp; Start</b>. Files they write appear in the workspace on the right.</td></tr>"
        "</table></td></tr></table>");
}

void TranscriptView::setMessages(const QVector<ChatMessage>& messages) {
    m_cachedBlocks.resize(messages.size());
    for (int i = 0; i < messages.size(); ++i) {
        m_cachedBlocks[i] = formatMessageHtml(messages[i], i, i == messages.size() - 1);
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
        m_cachedBlocks[prevIdx] = formatMessageHtml(allMessages[prevIdx], prevIdx, false);
    }
    m_cachedBlocks.append(formatMessageHtml(message, index, true));
    m_stickToBottom = true;
    flushHtmlToBrowser();
}

void TranscriptView::onMessageUpdated(int index, const ChatMessage& message, const QVector<ChatMessage>& allMessages) {
    if (index < 0 || index >= m_cachedBlocks.size()) {
        setMessages(allMessages);
        return;
    }
    // Only re-render the single modified message block!
    m_cachedBlocks[index] = formatMessageHtml(message, index, index == allMessages.size() - 1);
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

    QString html = QStringLiteral("<div style='line-height: 1.45;'>");
    for (const QString& block : m_cachedBlocks) {
        html += block;
    }
    if (m_cachedBlocks.isEmpty()) html += emptyStateHtml();
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
