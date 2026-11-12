#include "core/checkpoints.h"
#include "core/orchestrator.h"
#include "core/router_client.h"
#include "core/sandbox.h"
#include "core/tools.h"
#include "fake_llama_server.h"

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace agora;

namespace {

QString readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

void writeFile(const QString& path, const QByteArray& data) {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(data);
}

QString hostGit(const QString& dir, const QStringList& args) {
    QProcess git;
    git.setWorkingDirectory(dir);
    git.start("git", args);
    git.waitForFinished(5000);
    return QString::fromUtf8(git.readAllStandardOutput()).trimmed();
}

Checkpoint snapshotBlocking(CheckpointStore& store, const QString& session, const QString& label) {
    Checkpoint out;
    bool done = false;
    store.snapshot(session, label, [&](const Checkpoint& cp) { out = cp; done = true; });
    if (!QTest::qWaitFor([&]() { return done; }, 30000)) out.error = "timeout";
    return out;
}

RoomSession singleCoderSession(const QString& workspace) {
    RoomSession s;
    s.id = "flow_session";
    s.workspacePath = workspace;
    s.maxRounds = 5;
    Participant coder;
    coder.id = "p_coder";
    coder.handle = "@coder";
    coder.displayName = "Coder";
    coder.modelId = "fake-model";
    s.participants = {coder};
    return s;
}

} // namespace

class AgoraFlowTest : public QObject {
    Q_OBJECT
private slots:
    void testStartCommandIsAsyncAndConcurrent();
    void testCancelKillsAllRunningCommands();
    void testToolCallAsyncNeverCompletesSynchronously();
    void testCheckpointSnapshotDiffRestore();
    void testCheckpointsLeaveUserRepoUntouched();
    void testStreamStatsParsedFromFinalChunk();
    void testPolicyHelpers();
    void testEndToEndApprovalRetryCheckpointDone();
    void testDeniedToolAndStopDuringApproval();
};

void AgoraFlowTest::testStartCommandIsAsyncAndConcurrent() {
    if (!BwrapSandbox::isBwrapAvailable()) QSKIP("bwrap not installed");
    QTemporaryDir ws;
    BwrapSandbox sandbox(ws.path());

    QStringList finished;
    bool returnedBeforeCallback = true;
    sandbox.startCommand("sleep 0.4; echo slow", 10000, [&](const SandboxRunResult& r) {
        finished << r.stdoutText.trimmed();
    });
    sandbox.startCommand("echo fast", 10000, [&](const SandboxRunResult& r) {
        finished << r.stdoutText.trimmed();
    });
    returnedBeforeCallback = finished.isEmpty();

    QVERIFY(returnedBeforeCallback);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 15000);
    QCOMPARE(finished, (QStringList{"fast", "slow"})); // ran concurrently, fast one first
}

void AgoraFlowTest::testCancelKillsAllRunningCommands() {
    if (!BwrapSandbox::isBwrapAvailable()) QSKIP("bwrap not installed");
    QTemporaryDir ws;
    BwrapSandbox sandbox(ws.path());

    QVector<int> exitCodes;
    QElapsedTimer clock;
    clock.start();
    for (int i = 0; i < 2; ++i) {
        sandbox.startCommand("sleep 30", 60000, [&](const SandboxRunResult& r) { exitCodes << r.exitCode; });
    }
    QTest::qWait(300);
    sandbox.cancelRunningCommand();
    QTRY_COMPARE_WITH_TIMEOUT(exitCodes.size(), 2, 10000);
    QVERIFY(clock.elapsed() < 10000);
    QVERIFY(exitCodes[0] != 0 && exitCodes[1] != 0);
}

void AgoraFlowTest::testToolCallAsyncNeverCompletesSynchronously() {
    QTemporaryDir ws;
    BwrapSandbox sandbox(ws.path());
    ToolEngine tools(&sandbox);

    bool called = false;
    tools.executeToolCallAsync({"t1", "write_file", R"({"path":"a.txt","content":"x"})"},
                               [&](const ToolResult& r) { called = !r.isError; });
    QVERIFY(!called); // callers can rely on state not changing under them
    QTRY_VERIFY(called);
}

void AgoraFlowTest::testCheckpointSnapshotDiffRestore() {
    if (!BwrapSandbox::isBwrapAvailable()) QSKIP("bwrap not installed");
    QTemporaryDir ws;
    BwrapSandbox sandbox(ws.path());
    CheckpointStore store(&sandbox);
    QVERIFY(sandbox.ensureWorkspaceInitialized());

    writeFile(ws.filePath("keep.txt"), "original\n");
    writeFile(ws.filePath("doomed.txt"), "will be deleted\n");
    const Checkpoint base = snapshotBlocking(store, "s1", "base");
    QVERIFY2(base.ok, qPrintable(base.error));
    QVERIFY(base.changed);

    // Unchanged workspace -> no new checkpoint
    const Checkpoint same = snapshotBlocking(store, "s1", "same");
    QVERIFY(same.ok);
    QVERIFY(!same.changed);
    QCOMPARE(same.commit, base.commit);

    // A "turn": modify, delete, add (including a file in a new folder)
    writeFile(ws.filePath("keep.txt"), "modified by agent\n");
    QFile::remove(ws.filePath("doomed.txt"));
    QDir(ws.path()).mkpath("newdir");
    writeFile(ws.filePath("newdir/added.txt"), "new\n");
    const Checkpoint after = snapshotBlocking(store, "s1", "after");
    QVERIFY(after.ok && after.changed);
    QCOMPARE(after.parent, base.commit);

    QString diffText;
    bool diffOk = false;
    store.diff(base.commit, after.commit, [&](bool ok, const QString& t) { diffOk = ok; diffText = t; });
    QTRY_VERIFY_WITH_TIMEOUT(diffOk, 15000);
    QVERIFY(diffText.contains("+modified by agent"));
    QVERIFY(diffText.contains("newdir/added.txt"));
    QVERIFY(diffText.contains("doomed.txt"));

    QString saved;
    bool restored = false;
    store.restore("s1", base.commit, [&](bool ok, const QString& s) { restored = ok; saved = s; });
    QTRY_VERIFY_WITH_TIMEOUT(restored, 15000);
    QCOMPARE(readFile(ws.filePath("keep.txt")), QString("original\n"));
    QCOMPARE(readFile(ws.filePath("doomed.txt")), QString("will be deleted\n"));
    QVERIFY(!QFileInfo::exists(ws.filePath("newdir/added.txt")));

    // The pre-restore state was saved, so the restore itself can be undone
    QCOMPARE(saved, after.commit);
    bool redone = false;
    store.restore("s1", saved, [&](bool ok, const QString&) { redone = ok; });
    QTRY_VERIFY_WITH_TIMEOUT(redone, 15000);
    QCOMPARE(readFile(ws.filePath("newdir/added.txt")), QString("new\n"));
}

void AgoraFlowTest::testCheckpointsLeaveUserRepoUntouched() {
    if (!BwrapSandbox::isBwrapAvailable()) QSKIP("bwrap not installed");
    QTemporaryDir ws;
    hostGit(ws.path(), {"init", "-q", "-b", "main"});
    writeFile(ws.filePath("tracked.txt"), "v1\n");
    hostGit(ws.path(), {"add", "tracked.txt"});
    hostGit(ws.path(), {"-c", "user.name=U", "-c", "user.email=u@x", "commit", "-qm", "init"});
    writeFile(ws.filePath("staged.txt"), "staged\n");
    hostGit(ws.path(), {"add", "staged.txt"});
    const QString headBefore = hostGit(ws.path(), {"rev-parse", "HEAD"});
    const QString statusBefore = hostGit(ws.path(), {"status", "--porcelain"});

    BwrapSandbox sandbox(ws.path());
    CheckpointStore store(&sandbox);
    const Checkpoint cp = snapshotBlocking(store, "s2", "probe");
    QVERIFY2(cp.ok, qPrintable(cp.error));
    writeFile(ws.filePath("untracked.txt"), "agent file\n");
    QVERIFY(snapshotBlocking(store, "s2", "probe2").changed);

    QCOMPARE(hostGit(ws.path(), {"rev-parse", "HEAD"}), headBefore);
    QCOMPARE(hostGit(ws.path(), {"branch", "--show-current"}), QString("main"));
    // Staging area unchanged: staged.txt still staged, untracked.txt still untracked
    QCOMPARE(hostGit(ws.path(), {"status", "--porcelain"}), statusBefore + "\n?? untracked.txt");
    QVERIFY(hostGit(ws.path(), {"show-ref"}).contains("refs/agora/s2"));
}

void AgoraFlowTest::testStreamStatsParsedFromFinalChunk() {
    LlamaRouterClient router;
    router.resetStreamState();
    router.feedSseDataForTesting(FakeLlamaServer::sse("hello", {}, {}, 4321, 99), true);
    const TurnStats st = router.lastStreamStats();
    QCOMPARE(st.promptTokens, 4321);
    QCOMPARE(st.completionTokens, 99);
    QCOMPARE(st.tokensPerSecond, 30.0);
    QCOMPARE(router.accumulatedContent(), QString("hello"));

    // Without usage, total prompt = processed + cached prefix
    router.resetStreamState();
    router.feedSseDataForTesting(
        R"(data: {"choices":[],"timings":{"prompt_n":10,"cache_n":990,"predicted_n":5}})" "\n\n", true);
    QCOMPARE(router.lastStreamStats().promptTokens, 1000);
}

void AgoraFlowTest::testPolicyHelpers() {
    QVERIFY(!Orchestrator::needsApproval(ToolApprovalPolicy::Off, "run_command", {}));
    QVERIFY(Orchestrator::needsApproval(ToolApprovalPolicy::AskForWrites, "run_command", {}));
    QVERIFY(Orchestrator::needsApproval(ToolApprovalPolicy::AskForWrites, "write_file", {}));
    QVERIFY(!Orchestrator::needsApproval(ToolApprovalPolicy::AskForWrites, "read_file", {}));
    QVERIFY(Orchestrator::needsApproval(ToolApprovalPolicy::AskForAll, "read_file", {}));
    QVERIFY(!Orchestrator::needsApproval(ToolApprovalPolicy::AskForAll, "read_file", {"read_file"}));

    QVERIFY(Orchestrator::declaresDone("All tests pass.\n[DONE]"));
    QVERIFY(!Orchestrator::declaresDone("Not done yet"));

    QVERIFY(Orchestrator::isContextOverflowError(
        "HTTP 400: the request exceeds the available context size, try increasing it"));
    QVERIFY(Orchestrator::isContextOverflowError("This model's maximum context length is 8192 tokens"));
    QVERIFY(!Orchestrator::isContextOverflowError("HTTP 500: model failed to load"));

    // A model that packs fewer chars per token gets a smaller character budget
    QVERIFY(Orchestrator::contextCharBudget(32768, 2.0) < Orchestrator::contextCharBudget(32768, 4.0));
}

void AgoraFlowTest::testEndToEndApprovalRetryCheckpointDone() {
    QTemporaryDir ws;
    FakeLlamaServer server;
    server.enqueueChat({200, FakeLlamaServer::sse("Creating the file.", "write_file",
                                                  {{"path", "hello.txt"}, {"content", "hi\n"}})});
    server.enqueueChat({400, R"({"error":{"code":400,"type":"exceed_context_size_error",)"
                             R"("message":"the request exceeds the available context size"}})"});
    server.enqueueChat({200, FakeLlamaServer::sse("Wrote hello.txt.\n\n[DONE]", {}, {}, 200, 12)});

    BwrapSandbox sandbox(ws.path());
    ToolEngine tools(&sandbox);
    CheckpointStore checkpoints(&sandbox);
    LlamaRouterClient router;
    router.setBaseUrl(server.baseUrl());
    Orchestrator orch(&router, &tools);
    orch.setCheckpointStore(&checkpoints);
    orch.setToolApprovalPolicy(ToolApprovalPolicy::AskForWrites);
    orch.setSession(singleCoderSession(ws.path()));
    orch.addUserMessage("Please create hello.txt");

    QSignalSpy approvals(&orch, &Orchestrator::toolApprovalRequested);
    QSignalSpy statuses(&orch, &Orchestrator::statusNotification);
    QObject::connect(&orch, &Orchestrator::toolApprovalRequested, &orch,
                     [&orch](quint64 id, const QString&, const ToolCall& call) {
        QCOMPARE(call.name, QString("write_file"));
        // Approve from a later event-loop iteration, like a user clicking the dialog
        QTimer::singleShot(100, &orch, [&orch, id]() { orch.resolveToolApproval(id, ToolApprovalDecision::AllowOnce); });
    });

    orch.startDebate();
    QVERIFY(orch.isRunning());
    QTRY_VERIFY_WITH_TIMEOUT(!orch.isRunning(), 30000);

    QCOMPARE(approvals.size(), 1);
    QCOMPARE(readFile(ws.filePath("hello.txt")), QString("hi\n"));
    QCOMPARE(server.chatRequests().size(), 3); // tool turn, overflow, successful retry

    const auto& msgs = orch.session().messages;
    const ChatMessage& last = msgs.last();
    QVERIFY(last.content.contains("[DONE]"));
    QCOMPARE(last.stats.completionTokens, 12);
    QCOMPARE(last.stats.tokensPerSecond, 30.0);
    QVERIFY(msgs[1].stats.modelPrepMs >= 0);
    QVERIFY(orch.charsPerTokenFor("fake-model") != Orchestrator::kDefaultCharsPerToken);

    bool sawDone = false;
    bool sawRetry = false;
    for (const auto& args : statuses) {
        sawDone |= args.at(0).toString().contains("marked the task complete");
        sawRetry |= args.at(0).toString().contains("retrying");
    }
    QVERIFY(sawDone);
    QVERIFY(sawRetry);

    // Every request asked for usage so stats and calibration work
    QVERIFY(server.chatRequests().first()["stream_options"].toObject()["include_usage"].toBool());

    if (BwrapSandbox::isBwrapAvailable()) {
        QTRY_VERIFY_WITH_TIMEOUT(!orch.session().messages.last().checkpointCommit.isEmpty(), 10000);
        const ChatMessage& done = orch.session().messages.last();
        QVERIFY(CheckpointStore::isValidCommitId(done.checkpointParent));
        QVERIFY(done.checkpointParent != done.checkpointCommit);
    }
}

void AgoraFlowTest::testDeniedToolAndStopDuringApproval() {
    QTemporaryDir ws;
    FakeLlamaServer server;
    server.enqueueChat({200, FakeLlamaServer::sse("", "run_command", {{"command", "rm -rf ./*"}})});
    server.enqueueChat({200, FakeLlamaServer::sse("Understood, I will not do that. [DONE]")});

    BwrapSandbox sandbox(ws.path());
    ToolEngine tools(&sandbox);
    LlamaRouterClient router;
    router.setBaseUrl(server.baseUrl());
    Orchestrator orch(&router, &tools);
    orch.setAutoCheckpoints(false);
    orch.setToolApprovalPolicy(ToolApprovalPolicy::AskForAll);
    orch.setSession(singleCoderSession(ws.path()));
    orch.addUserMessage("Clean up");
    writeFile(ws.filePath("precious.txt"), "keep me\n");

    QObject::connect(&orch, &Orchestrator::toolApprovalRequested, &orch, [&orch](quint64 id, const QString&, const ToolCall&) {
        QTimer::singleShot(50, &orch, [&orch, id]() {
            orch.resolveToolApproval(id, ToolApprovalDecision::Deny, "never delete files");
        });
    });
    orch.startDebate();
    QTRY_VERIFY_WITH_TIMEOUT(!orch.isRunning(), 30000);

    QVERIFY(QFileInfo::exists(ws.filePath("precious.txt")));
    const ChatMessage& toolMsg = orch.session().messages[1];
    QCOMPARE(toolMsg.toolResults.size(), 1);
    QVERIFY(toolMsg.toolResults[0].isError);
    QVERIFY(toolMsg.toolResults[0].output.contains("never delete files"));
    // The model saw the denial in its next request
    const QString secondRequest = QString::fromUtf8(QJsonDocument(server.chatRequests()[1]).toJson());
    QVERIFY(secondRequest.contains("Denied by the user"));

    // Stopping while an approval is pending cancels it and ends the run
    FakeLlamaServer server2;
    server2.enqueueChat({200, FakeLlamaServer::sse("", "write_file", {{"path", "x.txt"}, {"content", "x"}})});
    router.setBaseUrl(server2.baseUrl());
    QObject::disconnect(&orch, &Orchestrator::toolApprovalRequested, &orch, nullptr);
    QSignalSpy cancelled(&orch, &Orchestrator::toolApprovalCancelled);
    QSignalSpy requested(&orch, &Orchestrator::toolApprovalRequested);
    orch.setSession(singleCoderSession(ws.path()));
    orch.addUserMessage("write x");
    orch.startDebate();
    QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 1, 15000);
    orch.stopDebate();
    QCOMPARE(cancelled.size(), 1);
    QVERIFY(!orch.isRunning());
    orch.resolveToolApproval(requested.first().at(0).toULongLong(), ToolApprovalDecision::AllowOnce);
    QTest::qWait(200);
    QVERIFY(!QFileInfo::exists(ws.filePath("x.txt"))); // late approval after stop is ignored
}

QTEST_GUILESS_MAIN(AgoraFlowTest)
#include "test_flow.moc"
