#include "core/orchestrator.h"
#include "core/router_client.h"
#include "core/sandbox.h"
#include "core/storage.h"
#include "core/tools.h"
#include "core/types.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <unistd.h>

using namespace agora;

class AgoraCoreTest : public QObject {
    Q_OBJECT
private slots:
    void testPathTraversalBlocked();
    void testSymlinkEscapeBlocked();
    void testGitConfigFsmonitorEscapeBlocked();
    void testSandboxWritePatchRunAndDiff();
    void testSandboxOutputTruncationAndHardening();
    void testPatchAmbiguityAndReadLineTotals();
    void testFallbackToolCallExtraction();
    void testMentionRoutingAndRoundRobin();
    void testPerspectiveFormattingAndContextBudget();
    void testSseParserAndFormatArgSafety();
    void testVramEvictionPlanner();
    void testSqliteSessionStore();
    void testAbortStreamAndSwitchSessionNoCrash();
};

void AgoraCoreTest::testPathTraversalBlocked() {
    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());

    BwrapSandbox sandbox(tmpDir.path());
    ToolEngine tools(&sandbox);

    QString err;
    auto okRel = tools.resolveSafeWorkspacePath("src/solution.py", &err);
    QVERIFY2(okRel.has_value(), qPrintable(err));

    auto okMounted = tools.resolveSafeWorkspacePath("/workspace/tests/test_main.py", &err);
    QVERIFY2(okMounted.has_value(), qPrintable(err));

    auto badParent = tools.resolveSafeWorkspacePath("../../etc/passwd", &err);
    QVERIFY(!badParent.has_value());

    auto badAbs = tools.resolveSafeWorkspacePath("/etc/shadow", &err);
    QVERIFY(!badAbs.has_value());

    auto badGit = tools.resolveSafeWorkspacePath(".git/config", &err);
    QVERIFY(!badGit.has_value());
}

void AgoraCoreTest::testSymlinkEscapeBlocked() {
    QTemporaryDir wsDir;
    QTemporaryDir outsideDir;
    QVERIFY(wsDir.isValid());
    QVERIFY(outsideDir.isValid());

    BwrapSandbox sandbox(wsDir.path());
    QString err;
    QVERIFY2(sandbox.ensureWorkspaceInitialized(&err), qPrintable(err));
    ToolEngine tools(&sandbox);

    const QString outsideTarget = outsideDir.filePath("escaped_host_file.txt");
    const QString symlinkInWs = wsDir.filePath("link_to_outside");

    // 1. Dangling symlink to non-existent host file (exact scenario from security audit)
    QCOMPARE(::symlink(outsideTarget.toUtf8().constData(), symlinkInWs.toUtf8().constData()), 0);

    ToolCall writeCall;
    writeCall.id = "sym_w";
    writeCall.name = "write_file";
    QJsonObject wArgs;
    wArgs["path"] = "link_to_outside";
    wArgs["content"] = "pwned\n";
    writeCall.argumentsJson = QString::fromUtf8(QJsonDocument(wArgs).toJson(QJsonDocument::Compact));

    const ToolResult wRes = tools.executeToolCall(writeCall);
    QVERIFY(wRes.isError);
    QVERIFY2(!QFileInfo::exists(outsideTarget), "Host file must NOT be created through workspace symlink!");

    // 2. Symlink to existing host file via read_file and patch_file
    {
        QFile hostFile(outsideTarget);
        QVERIFY(hostFile.open(QIODevice::WriteOnly));
        hostFile.write("secret_host_content\n");
        hostFile.close();
    }

    ToolCall readCall{"sym_r", "read_file", R"({"path":"link_to_outside"})"};
    const ToolResult rRes = tools.executeToolCall(readCall);
    QVERIFY(rRes.isError);
    QVERIFY(!rRes.output.contains("secret_host_content"));

    ToolCall patchCall{"sym_p", "patch_file", R"({"path":"link_to_outside","search":"secret","replace":"hacked"})"};
    const ToolResult pRes = tools.executeToolCall(patchCall);
    QVERIFY(pRes.isError);

    // 3. Symlinked parent directory inside workspace
    const QString symlinkDirInWs = wsDir.filePath("sub_link");
    QCOMPARE(::symlink(outsideDir.path().toUtf8().constData(), symlinkDirInWs.toUtf8().constData()), 0);

    ToolCall nestedWrite{"sym_dir_w", "write_file", R"({"path":"sub_link/nested.txt","content":"bad"})"};
    const ToolResult nwRes = tools.executeToolCall(nestedWrite);
    QVERIFY(nwRes.isError);
    QVERIFY(!QFileInfo::exists(outsideDir.filePath("nested.txt")));
}

void AgoraCoreTest::testGitConfigFsmonitorEscapeBlocked() {
    if (!BwrapSandbox::isBwrapAvailable()) {
        QSKIP("bwrap not installed");
    }

    QTemporaryDir wsDir;
    QTemporaryDir outsideDir;
    QVERIFY(wsDir.isValid());
    QVERIFY(outsideDir.isValid());

    BwrapSandbox sandbox(wsDir.path());
    QString err;
    QVERIFY2(sandbox.ensureWorkspaceInitialized(&err), qPrintable(err));
    ToolEngine tools(&sandbox);

    const QString markerFile = outsideDir.filePath("host_fsmonitor_marker");

    // 1. Model tries to set core.fsmonitor inside sandbox via run_command
    ToolCall runGitCfg;
    runGitCfg.id = "git_cfg";
    runGitCfg.name = "run_command";
    QJsonObject args;
    args["command"] = QStringLiteral("git config core.fsmonitor 'touch %1'").arg(markerFile);
    runGitCfg.argumentsJson = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));

    const ToolResult cfgRes = tools.executeToolCall(runGitCfg);
    QVERIFY(cfgRes.isError); // .git is mounted read-only inside bwrap

    // 2. Even if .git/config on disk was somehow modified externally, gitDiff() runs inside bwrap
    //    with core.fsmonitor=false and core.hooksPath=/dev/null, so host marker must never exist.
    {
        QFile cfg(wsDir.filePath(".git/config"));
        QVERIFY(cfg.open(QIODevice::Append));
        cfg.write(QStringLiteral("\n[core]\n\tfsmonitor = touch %1\n").arg(markerFile).toUtf8());
        cfg.close();
    }

    ToolCall diffCall{"gd", "git_diff", "{}"};
    tools.executeToolCall(diffCall);
    QVERIFY2(!QFileInfo::exists(markerFile), "core.fsmonitor must NEVER execute on host!");
}

void AgoraCoreTest::testSandboxWritePatchRunAndDiff() {
    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());

    BwrapSandbox sandbox(tmpDir.path());
    QString err;
    QVERIFY2(sandbox.ensureWorkspaceInitialized(&err), qPrintable(err));

    ToolEngine tools(&sandbox);

    {
        ToolCall writeCall;
        writeCall.id = "c1";
        writeCall.name = "write_file";
        QJsonObject args;
        args["path"] = "calc.py";
        args["content"] = "def compute():\n    return 20\n\nif __name__ == '__main__':\n    print(compute())\n";
        writeCall.argumentsJson = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));

        ToolResult res = tools.executeToolCall(writeCall);
        QVERIFY2(!res.isError, qPrintable(res.output));
    }

    {
        ToolCall patchCall;
        patchCall.id = "c2";
        patchCall.name = "patch_file";
        QJsonObject args;
        args["path"] = "calc.py";
        args["search"] = "return 20";
        args["replace"] = "return 42";
        patchCall.argumentsJson = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));

        ToolResult res = tools.executeToolCall(patchCall);
        QVERIFY2(!res.isError, qPrintable(res.output));
    }

    if (BwrapSandbox::isBwrapAvailable()) {
        ToolCall runCall;
        runCall.id = "c3";
        runCall.name = "run_command";
        QJsonObject args;
        args["command"] = "python3 calc.py";
        runCall.argumentsJson = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));

        ToolResult res = tools.executeToolCall(runCall);
        QVERIFY2(!res.isError, qPrintable(res.output));
        QCOMPARE(res.output.trimmed(), QString("42"));

        SandboxRunResult roTest = sandbox.runCommand("touch /usr/agora_forbidden_write", 5000);
        QVERIFY(roTest.exitCode != 0);
    }

    {
        ToolCall diffCall{"c4", "git_diff", "{}"};
        ToolResult res = tools.executeToolCall(diffCall);
        QVERIFY(!res.isError);
        QVERIFY(res.output.contains("return 42"));
    }
}

void AgoraCoreTest::testSandboxOutputTruncationAndHardening() {
    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());

    BwrapSandbox sandbox(tmpDir.path());
    const QStringList args = sandbox.buildBwrapArgs("echo hi");
    QVERIFY(args.contains("--new-session"));
    QVERIFY(!args.contains("/etc/shadow"));

    if (BwrapSandbox::isBwrapAvailable()) {
        // Output > 64 KiB should be truncated cleanly without hanging or OOM
        const SandboxRunResult flood = sandbox.runCommand("python3 -c \"print('X' * 150000)\"", 5000);
        QVERIFY(flood.truncated);
        QVERIFY(flood.stdoutText.size() <= BwrapSandbox::kMaxOutputBytes + 128);
        QVERIFY(flood.stdoutText.contains("output truncated at 64 KiB"));
    }
}

void AgoraCoreTest::testPatchAmbiguityAndReadLineTotals() {
    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());

    BwrapSandbox sandbox(tmpDir.path());
    ToolEngine tools(&sandbox);

    QString manyLines;
    for (int i = 1; i <= 300; ++i) {
        manyLines += QStringLiteral("line_item = %1\n").arg(QString::number(i));
    }

    ToolCall wCall;
    wCall.id = "w1";
    wCall.name = "write_file";
    QJsonObject wArgs;
    wArgs["path"] = "items.py";
    wArgs["content"] = manyLines;
    wCall.argumentsJson = QString::fromUtf8(QJsonDocument(wArgs).toJson(QJsonDocument::Compact));
    QVERIFY(!tools.executeToolCall(wCall).isError);

    // Ambiguous patch ('line_item =' appears 300 times) must fail
    ToolCall pCall{"p1", "patch_file", R"({"path":"items.py","search":"line_item =","replace":"x ="})"};
    const ToolResult pRes = tools.executeToolCall(pCall);
    QVERIFY(pRes.isError);
    QVERIFY(pRes.output.contains("Ambiguous patch"));

    // read_file default (lines 1-250) must report total 301 lines and how to read more
    ToolCall rCall{"r1", "read_file", R"({"path":"items.py"})"};
    const ToolResult rRes = tools.executeToolCall(rCall);
    QVERIFY(!rRes.isError);
    QVERIFY(rRes.output.contains("Showing lines 1-250 of"));
    QVERIFY(rRes.output.contains("start_line=251"));
}

void AgoraCoreTest::testFallbackToolCallExtraction() {
    const QString sample =
        "I will inspect the test suite now.\n"
        "<tool_call>\n"
        "{\"name\": \"run_command\", \"arguments\": {\"command\": \"python3 -m unittest\"}}\n"
        "</tool_call>\n";

    QString cleaned;
    const QVector<ToolCall> calls = ToolEngine::extractFallbackToolCalls(sample, &cleaned);
    QCOMPARE(calls.size(), 1);
    QCOMPARE(calls[0].name, QString("run_command"));
    QVERIFY(calls[0].argumentsJson.contains("unittest"));
    QCOMPARE(cleaned, QString("I will inspect the test suite now."));
}

void AgoraCoreTest::testMentionRoutingAndRoundRobin() {
    Participant pArch{"id_arch", "@architect", "Architect", "ModelA", "sys", "#38bdf8", 0.6, true};
    Participant pCode{"id_code", "@coder", "Coder", "ModelB", "sys", "#4ade80", 0.4, true};
    Participant pRev{"id_rev", "@reviewer", "Reviewer", "ModelC", "sys", "#f472b6", 0.5, true};
    QVector<Participant> roster = {pArch, pCode, pRev};

    // 1. User mentions @coder explicitly -> @coder speaks first
    QVector<ChatMessage> history;
    ChatMessage uMsg;
    uMsg.id = "m1";
    uMsg.senderId = "user";
    uMsg.senderHandle = "@you";
    uMsg.content = "Hey @coder please implement the parser.";
    uMsg.mentions = Orchestrator::extractMentions(uMsg.content, roster, /*handoffOnly=*/false);
    history.append(uMsg);

    int rrCursor = 0;
    auto next1 = Orchestrator::selectNextSpeaker(roster, history, DebateMode::RoundRobinWithMentions, &rrCursor);
    QVERIFY(next1.has_value());
    QCOMPARE(next1->handle, QString("@coder"));

    // 2. Model mentions @architect in the middle of a long paragraph ("I disagree with @architect...")
    //    without a tail handoff -> should NOT hand off to @architect, should follow Round-Robin to @reviewer!
    ChatMessage cMsg;
    cMsg.id = "m2";
    cMsg.senderId = pCode.id;
    cMsg.senderHandle = pCode.handle;
    cMsg.content = "I disagree with @architect about using recursion here. " + QString(260, 'a');
    cMsg.mentions = Orchestrator::extractMentions(cMsg.content, roster, /*handoffOnly=*/true);
    QVERIFY(cMsg.mentions.isEmpty());
    history.append(cMsg);

    auto next2 = Orchestrator::selectNextSpeaker(roster, history, DebateMode::RoundRobinWithMentions, &rrCursor);
    QVERIFY(next2.has_value());
    QCOMPARE(next2->handle, QString("@reviewer"));
}

void AgoraCoreTest::testPerspectiveFormattingAndContextBudget() {
    Participant pArch{"id_arch", "@architect", "Architect", "ModelA", "sys", "#38bdf8", 0.6, true};
    Participant pCode{"id_code", "@coder", "Coder", "ModelB", "sys", "#4ade80", 0.4, true};
    QVector<Participant> roster = {pArch, pCode};

    ChatMessage m1;
    m1.id = "1";
    m1.senderId = "user";
    m1.senderHandle = "@you";
    m1.displayName = "User";
    m1.content = "Build a fast Prime sieve.";

    ChatMessage m2;
    m2.id = "2";
    m2.senderId = pArch.id;
    m2.senderHandle = pArch.handle;
    m2.displayName = pArch.displayName;
    m2.modelId = pArch.modelId;
    m2.content = "Let's create sieve.py. -> @coder";

    ChatMessage m3;
    m3.id = "3";
    m3.senderId = pCode.id;
    m3.senderHandle = pCode.handle;
    m3.displayName = pCode.displayName;
    m3.modelId = pCode.modelId;
    m3.content = "Done!";

    const QJsonArray forCoder = Orchestrator::buildMessagesForSpeaker(
        pCode, roster, {m1, m2, m3}, "/home/secret_host_path/ws");

    // 1. System prompt must NOT leak "/home/secret_host_path/ws"
    QVERIFY(!forCoder[0].toObject()["content"].toString().contains("secret_host_path"));

    // 2. Consecutive user messages (m1 from @you and m2 from @architect) must be merged into a single "user" turn!
    QCOMPARE(forCoder.size(), 3);
    QCOMPARE(forCoder[0].toObject()["role"].toString(), QString("system"));
    QCOMPARE(forCoder[1].toObject()["role"].toString(), QString("user"));
    QVERIFY(forCoder[1].toObject()["content"].toString().contains("@you"));
    QVERIFY(forCoder[1].toObject()["content"].toString().contains("@architect"));
    QCOMPARE(forCoder[2].toObject()["role"].toString(), QString("assistant"));
    QCOMPARE(forCoder[2].toObject()["content"].toString(), QString("Done!"));

    // 3. Test context budget trimming when history is huge
    QVector<ChatMessage> longHistory;
    for (int i = 0; i < 20; ++i) {
        ChatMessage m;
        m.id = QString::number(i);
        m.senderId = (i % 2 == 0) ? pArch.id : pCode.id;
        m.senderHandle = (i % 2 == 0) ? pArch.handle : pCode.handle;
        m.content = QString(1000, QChar('x'));
        longHistory.append(m);
    }
    const QJsonArray budgeted = Orchestrator::buildMessagesForSpeaker(pCode, roster, longHistory, "/workspace", /*maxContextChars=*/6000);
    QVERIFY(budgeted.size() < 15);
}

void AgoraCoreTest::testSseParserAndFormatArgSafety() {
    LlamaRouterClient router;
    router.resetStreamState();

    const QByteArray sseSample =
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"Thinking step 1...\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello @coder\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_99\",\"function\":{\"name\":\"write_\",\"arguments\":\"{\\\"path\\\":\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"name\":\"file\",\"arguments\":\"\\\"a.txt\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";

    router.feedSseDataForTesting(sseSample, true);
    QCOMPARE(router.accumulatedReasoning(), QString("Thinking step 1..."));
    QCOMPARE(router.accumulatedContent(), QString("Hello @coder"));
    QVERIFY(router.receivedDoneMarker());

    const QVector<ToolCall> calls = router.finalizedToolCalls();
    QCOMPARE(calls.size(), 1);
    QCOMPARE(calls[0].id, QString("call_99"));
    QCOMPARE(calls[0].name, QString("write_file"));
    QCOMPARE(calls[0].argumentsJson, QString("{\"path\":\"a.txt\"}"));

    // Verify multi-argument QString::arg safety when tool name contains "%3"
    RoomSession s;
    s.title = "Format Safety";
    ChatMessage m;
    m.id = "1";
    m.senderId = "other";
    m.senderHandle = "@other";
    ToolResult tr;
    tr.name = "weird_tool_%3_name";
    tr.exitCode = 0;
    tr.output = "SECRET_OUTPUT";
    m.toolResults.append(tr);
    s.messages.append(m);

    const QString md = SessionStore::exportToMarkdown(s);
    QVERIFY(md.contains("[Tool: weird_tool_%3_name | exit=0]"));
}

void AgoraCoreTest::testVramEvictionPlanner() {
    QVector<ModelInfo> models = {
        {"LFM2.5-2.6B-Q4_K_M", "", "loaded", 1.56, false},
        {"Qwen3.5-4B-Q4_K_M", "", "loaded", 2.55, false},
        {"Qwopus3.5-9B-coder-Exp-Q4_K_M", "", "unloaded", 5.24, false},
        {"Qwen3.8-27B-UD-IQ2_S", "", "unloaded", 7.80, false}
    };

    QVERIFY(LlamaRouterClient::planEvictionsForModel(models, "Qwen3.5-4B-Q4_K_M", 8.5).isEmpty());

    const QStringList evictFor9B = LlamaRouterClient::planEvictionsForModel(
        models, "Qwopus3.5-9B-coder-Exp-Q4_K_M", 8.5);
    QCOMPARE(evictFor9B, QStringList({"Qwen3.5-4B-Q4_K_M"}));

    const QStringList evictFor27B = LlamaRouterClient::planEvictionsForModel(
        models, "Qwen3.8-27B-UD-IQ2_S", 8.5);
    QCOMPARE(evictFor27B.size(), 2);
}

void AgoraCoreTest::testSqliteSessionStore() {
    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());

    SessionStore store(tmpDir.filePath("test_sessions.db"));
    QString err;
    QVERIFY2(store.initialize(&err), qPrintable(err));

    RoomSession s;
    s.id = "sess_1";
    s.title = "Sieve Debate";
    s.workspacePath = "/tmp/ws";
    s.vramBudgetGiB = 8.5;
    s.maxRounds = 6;

    Participant p;
    p.id = "p1";
    p.handle = "@coder";
    p.displayName = "Lead Coder";
    p.modelId = "Qwopus3.5-9B-coder-Exp-Q4_K_M";
    s.participants.append(p);

    ChatMessage m;
    m.id = "msg1";
    m.senderId = "p1";
    m.senderHandle = "@coder";
    m.displayName = "Lead Coder";
    m.content = "Implemented sieve.py";
    s.messages.append(m);

    QVERIFY2(store.saveSession(s, &err), qPrintable(err));

    const auto summaries = store.listSessionSummaries(&err);
    QCOMPARE(summaries.size(), 1);
    QCOMPARE(summaries[0].title, QString("Sieve Debate"));
    QCOMPARE(summaries[0].messageCount, 1);

    auto loaded = store.loadSession("sess_1", &err);
    QVERIFY2(loaded.has_value(), qPrintable(err));
    QCOMPARE(loaded->title, QString("Sieve Debate"));
    QCOMPARE(loaded->participants.size(), 1);
    QCOMPARE(loaded->messages.size(), 1);
}

void AgoraCoreTest::testAbortStreamAndSwitchSessionNoCrash() {
    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());

    BwrapSandbox sandbox(tmpDir.path());
    ToolEngine tools(&sandbox);
    LlamaRouterClient router;
    Orchestrator orch(&router, &tools);

    router.startChatStream("dummy-model", QJsonArray{}, QJsonArray{}, 0.7);
    QVERIFY(router.isStreaming());
    router.abortActiveStream();
    QVERIFY(!router.isStreaming());

    router.startChatStream("dummy-model", QJsonArray{}, QJsonArray{}, 0.7);
    RoomSession nextRoom;
    nextRoom.id = "room_2";
    nextRoom.title = "Second Room";
    nextRoom.workspacePath = tmpDir.path();
    orch.setSession(nextRoom);
    QVERIFY(!router.isStreaming());
    QCOMPARE(orch.session().id, QString("room_2"));
}

QTEST_MAIN(AgoraCoreTest)
#include "test_core.moc"
