#include "ui/transcript_view.h"

#include <QtTest>

using namespace agora;

class AgoraUiTest : public QObject {
    Q_OBJECT
private slots:
    void testMarkdownRendering();
    void testMarkdownBlocksRawHtml();
    void testStatsFormatting();
    void testRolePresetsAndCouncils();
};

void AgoraUiTest::testMarkdownRendering() {
    const QString html = TranscriptView::markdownToHtmlFragment(
        "Plan for @coder:\n\n- **first** item\n- second\n\n```python\nprint('hi')\n```\n");
    QVERIFY(html.contains("<li"));
    QVERIFY(html.contains("font-weight:")); // bold
    QVERIFY(html.contains("<pre"));
    QVERIFY(html.contains("print("));
    QVERIFY(html.contains("<b style='color: #38bdf8;'>@coder</b>"));
    QVERIFY(!html.contains("<body"));
}

void AgoraUiTest::testMarkdownBlocksRawHtml() {
    const QString html = TranscriptView::markdownToHtmlFragment(
        "<img src=x onerror=alert(1)>\n\n<a href='agora://undo/0'>click</a>\n\n"
        "[undo](agora://undo/1) [file](file:///etc/passwd) [web](https://example.com/@user)");
    // Raw HTML is shown as text, never as tags
    QVERIFY(!html.contains("<img"));
    QVERIFY(!html.contains("<a href='agora"));
    QVERIFY(html.contains("&lt;img"));
    // Markdown links can't reach app actions or local files...
    QVERIFY(!html.contains("href=\"agora:"));
    QVERIFY(!html.contains("href=\"file:"));
    // ...but web links survive, and mention highlighting leaves attributes alone
    QVERIFY(html.contains("href=\"https://example.com/@user\""));
}

void AgoraUiTest::testStatsFormatting() {
    TurnStats st;
    st.promptTokens = 12345;
    st.completionTokens = 321;
    st.tokensPerSecond = 28.44;
    st.modelPrepMs = 3100;
    const QString text = TranscriptView::formatStats(st);
    QVERIFY(text.contains("12.3k prompt tok"));
    QVERIFY(text.contains("321 tok"));
    QVERIFY(text.contains("28.4 tok/s"));
    QVERIFY(text.contains("model prep 3.1s"));
    QVERIFY(TranscriptView::formatStats({}).isEmpty());
}

void AgoraUiTest::testRolePresetsAndCouncils() {
    const auto presets = defaultRolePresets();
    QVERIFY(presets.size() >= 14);
    QSet<QString> handles;
    for (const auto& r : presets) {
        QVERIFY2(r.handle.startsWith('@'), qPrintable(r.handle));
        QVERIFY2(!handles.contains(r.handle), qPrintable("duplicate " + r.handle));
        QVERIFY(!r.systemPrompt.isEmpty());
        handles.insert(r.handle);
    }
    for (const auto& council : defaultCouncilTemplates()) {
        QVERIFY(!council.handles.isEmpty());
        for (const auto& h : council.handles) {
            QVERIFY2(handles.contains(h), qPrintable(council.name + " uses unknown role " + h));
        }
    }
}

QTEST_MAIN(AgoraUiTest)
#include "test_ui.moc"
