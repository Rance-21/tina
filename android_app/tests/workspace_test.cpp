#include "ffi_client_bridge.hpp"
#include "code_highlighter.hpp"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStyleHints>
#include <QTcpServer>
#include <QTest>
#include <QTextBlock>
#include <QTextLayout>
#include <QtEndian>
#include <optional>
#include <algorithm>

using namespace tina::ui;

namespace {
template<typename T> QByteArray integer(T value) {
    QByteArray result(sizeof(T), '\0');
    qToBigEndian(value, reinterpret_cast<uchar *>(result.data()));
    return result;
}
QByteArray string(const QByteArray &value) {
    return integer(quint16(value.size())) + value;
}
QString pathOf(const QByteArray &payload) {
    const auto size = qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(payload.constData()));
    return QString::fromUtf8(payload.mid(2, size));
}
QByteArray terminalPayload(quint32 id, const QByteArray &bytes) {
    return integer(quint32(bytes.size())) + integer(id) + bytes;
}
QByteArray terminalState(quint32 id) {
    return integer(quint32(4)) + integer(id);
}

struct Call {
    QPointer<QTcpSocket> peer;
    quint64 id;
    quint16 method;
    QByteArray payload;
};

// 使用真实 TCP 字节流模拟服务端，覆盖半包、乱序和请求重试，不写远程真实文件。
class FakeServer : public QObject {
  public:
    QTcpServer server;
    QHash<QString, QByteArray> files{{"demo/main.cpp", "int main() {\n    return 0;\n}\n"},
                                   {"demo/src/a.cpp", "// hello\n"},
                                   {"demo/other.cpp", "// other\n"}};
    quint64 revision = 0xfedcba9876543210ULL;
    QList<Call> calls;
    std::function<bool(const Call &)> intercept;
    quint32 nextTerminalId{1};

    FakeServer() {
        if (!server.listen(QHostAddress::LocalHost))
            qFatal("Unable to listen: %s", qPrintable(server.errorString()));
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto *peer = server.nextPendingConnection()) {
                auto input = std::make_shared<QByteArray>();
                connect(peer, &QTcpSocket::readyRead, this, [this, peer, input] {
                    input->append(peer->readAll());
                    while (input->size() >= 4) {
                        const auto *data = reinterpret_cast<const uchar *>(input->constData());
                        const auto size = qFromBigEndian<quint32>(data);
                        if (input->size() < 4 + size)
                            return;
                        Call call{peer, qFromBigEndian<quint64>(data + 5),
                                  qFromBigEndian<quint16>(data + 13), input->mid(17, size - 13)};
                        input->remove(0, size + 4);
                        calls.append(call);
                        if (!intercept || !intercept(call))
                            handle(call);
                    }
                });
            }
        });
    }

    void respond(const Call &call, quint16 status, const QByteArray &payload, bool fragment = false) {
        if (!call.peer)
            return;
        auto frame = integer(quint32(13 + payload.size())) + char(2) + integer(call.id)
                   + integer(call.method) + integer(status) + payload;
        if (!fragment) {
            call.peer->write(frame);
        } else {
            call.peer->write(frame.left(2));
            QTimer::singleShot(10, this, [call, frame] {
                if (call.peer) call.peer->write(frame.mid(2));
            });
        }
    }

    void event(const QString &path, const QByteArray &entries) {
        if (calls.isEmpty() || !calls.last().peer)
            return;
        const auto payload = string(path.toUtf8()) + entries;
        calls.last().peer->write(integer(quint32(13 + payload.size())) + char(3)
                                + integer(quint64(0)) + integer(quint16(0x0202))
                                + integer(quint16(0)) + payload);
    }

    void terminalEvent(quint16 method, const QByteArray &payload) {
        if (!calls.isEmpty() && calls.last().peer)
            calls.last().peer->write(integer(quint32(13 + payload.size())) + char(3)
                                    + integer(quint64(0)) + integer(method)
                                    + integer(quint16(0)) + payload);
    }

    int count(quint16 method) const {
        return std::count_if(calls.begin(), calls.end(), [method](const Call &call) {
            return call.method == method;
        });
    }

    void handle(const Call &call) {
        switch (call.method) {
        case 0x0303: {
            const auto id = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(call.payload.constData() + 4));
            respond(call, 0, terminalState(id ? id : nextTerminalId++));
            break;
        }
        case 0x0301: {
            const auto id = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(call.payload.constData() + 4));
            respond(call, 0, terminalPayload(id, "OK"));
            terminalEvent(0x0381, terminalPayload(id, call.payload.mid(8) + "\r\n"));
            terminalEvent(0x0303, terminalState(id));
            break;
        }
        case 0x0302: {
            const auto id = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(call.payload.constData() + 4));
            respond(call, 0, terminalPayload(id, call.payload.mid(8) + "pleted"));
            break;
        }
        case 0x0001: respond(call, 0, "PONG"); break;
        case 0x0201: respond(call, 0, string("demo") + string("other"), true); break;
        case 0x0202: {
            const auto path = pathOf(call.payload);
            const auto entries = path == "demo" ? QByteArray(1, char(0)) + string("main.cpp")
                                                   + char(1) + string("src")
                                                : QByteArray(1, char(0)) + string("a.cpp");
            respond(call, 0, string(path.toUtf8()) + entries);
            break;
        }
        case 0x0203: {
            const auto path = pathOf(call.payload);
            if (!files.contains(path)) {
                respond(call, 4, "missing");
                break;
            }
            const auto start = 2 + path.toUtf8().size();
            const auto offset = qFromBigEndian<quint64>(
                reinterpret_cast<const uchar *>(call.payload.constData() + start));
            const auto maxBytes = qFromBigEndian<quint32>(
                reinterpret_cast<const uchar *>(call.payload.constData() + start + 8));
            const auto bytes = files.value(path);
            respond(call, 0, integer(quint64(bytes.size())) + integer(revision)
                             + bytes.mid(offset, maxBytes));
            break;
        }
        case 0x0204: {
            const auto path = pathOf(call.payload);
            const auto start = 2 + path.toUtf8().size();
            const auto expected = qFromBigEndian<quint64>(
                reinterpret_cast<const uchar *>(call.payload.constData() + start));
            if (expected != revision) {
                respond(call, 6, integer(revision));
                break;
            }
            files[path] = call.payload.mid(start + 8);
            ++revision;
            respond(call, 0, integer(revision));
            break;
        }
        }
    }
};

WorkspaceDocument *document(FfiClientBridge &bridge, int index = 0) {
    return qvariant_cast<WorkspaceDocument *>(bridge.documents().value(index));
}

// Repeater 创建的项按视觉树挂载，QObject 的父子树不一定包含这些代理。
QQuickItem *findVisualItem(QQuickItem *root, const QString &name) {
    if (root->objectName() == name)
        return root;
    for (auto *child : root->childItems()) {
        if (auto *found = findVisualItem(child, name))
            return found;
    }
    return nullptr;
}
} // namespace

class WorkspaceTest : public QObject {
    Q_OBJECT
  private slots:
    void terminalRingAndSplitControlSequences() {
        TerminalSession terminal(1);
        const auto unicode = QString::fromUtf8("中文🙂").toUtf8();
        for (auto byte : unicode)
            terminal.appendOutput(QByteArray(1, byte));
        QCOMPARE(terminal.output(), QString::fromUtf8(unicode));
        terminal.appendOutput("\r\n\x1b[3");
        terminal.appendOutput("1mred\x1b[0m\x1b]0;ignored");
        terminal.appendOutput("\x1b\\\r\nprogress 100%");
        terminal.appendOutput("\rOK\x1b[K\r\n");
        QVERIFY(terminal.output().endsWith("red\nOK\n"));
        QVERIFY(!terminal.output().contains("ignored"));
        terminal.clear();
        terminal.appendOutput(QByteArray(TerminalSession::BufferCapacity * 3, 'x') + "tail");
        QCOMPARE(terminal.output().size(), TerminalSession::BufferCapacity);
        QVERIFY(terminal.output().endsWith("tail"));
        terminal.appendOutput(QByteArray("\r\nrow").repeated(TerminalSession::MaxLines * 2));
        QCOMPARE(terminal.output().count('\n'), TerminalSession::MaxLines);
        QVERIFY(!terminal.output().contains('x'));
        terminal.clear();
        QCOMPARE(terminal.output(), QString());
    }

    void terminalSessionsCompletionAndReconnect() {
        FakeServer server;
        FfiClientBridge bridge;
        bridge.connectToServer("127.0.0.1", server.server.serverPort());
        QTRY_VERIFY(bridge.activeTerminal() && bridge.activeTerminal()->available());
        QPointer<TerminalSession> first = bridge.activeTerminal();
        first->setDraft("first draft");
        bridge.createTerminal();
        QTRY_VERIFY(bridge.activeTerminal()->available());
        auto *second = bridge.activeTerminal();
        second->setDraft("com");
        bridge.completeTerminalInput();
        QTRY_COMPARE(second->draft(), QString("completed"));
        QCOMPARE(first->draft(), QString("first draft"));
        QVERIFY(bridge.submitCommand(" echo second "));
        QTRY_VERIFY(second->output().contains(" echo second \n"));
        QVERIFY(first->output().isEmpty());
        bridge.selectTerminal(0);
        QCOMPARE(bridge.activeTerminal(), first.data());
        server.terminalEvent(0x0381, terminalPayload(second->terminalId(), "background\r\n"));
        QTRY_VERIFY(second->output().contains("background"));
        QVERIFY(first->output().isEmpty());
        // 补全返回之前继续编辑，迟到结果不能覆盖新的草稿。
        std::optional<Call> completion;
        server.intercept = [&](const Call &call) {
            if (call.method == 0x0302) { completion = call; return true; }
            return false;
        };
        first->setDraft("old");
        bridge.completeTerminalInput();
        QTRY_VERIFY(completion.has_value());
        first->setDraft("new");
        server.respond(*completion, 0, terminalPayload(first->terminalId(), "obsolete"));
        QTRY_VERIFY(!first->completing());
        QCOMPARE(first->draft(), QString("new"));
        server.intercept = {};
        const auto previousId = first->terminalId();
        const auto inputCalls = server.count(0x0301);
        server.calls.last().peer->abort();
        QTRY_VERIFY(!bridge.connected());
        QVERIFY(!first->available());
        QTRY_VERIFY_WITH_TIMEOUT(first->available(), 5000);
        QVERIFY(first->terminalId() != previousId);
        QCOMPARE(first->sessionId(), 1); // 用户编号独立于重连后重新分配的 RPC ID。
        QCOMPARE(server.count(0x0301), inputCalls);
        QVERIFY(second->output().contains("background"));
        QCOMPARE(first->draft(), QString("new"));
        bridge.closeTerminal(0);
        QTRY_VERIFY(first.isNull());
        QCOMPARE(bridge.terminals().size(), 1);
        QCOMPARE(bridge.activeTerminal(), second);
    }

    void terminalCommandsAndMixedTabs() {
        FakeServer server;
        FfiClientBridge bridge;
        bridge.connectToServer("127.0.0.1", server.server.serverPort());
        QTRY_VERIFY(bridge.activeTerminal() && bridge.activeTerminal()->available());
        auto *first = bridge.activeTerminal();
        bridge.readFile("demo/main.cpp");
        QTRY_VERIFY(document(bridge)->loaded());
        auto *file = document(bridge);
        QCOMPARE(bridge.tabs().size(), 2);
        QCOMPARE(bridge.activeTabIndex(), 1);
        QVERIFY(!bridge.terminalActive());
        QVERIFY(bridge.handleTerminalCommand("  @TeRmInAl   ADD 7  "));
        auto *seventh = bridge.activeTerminal();
        QTRY_VERIFY(seventh->available());
        QCOMPARE(seventh->sessionId(), 7);
        QCOMPARE(bridge.activeTabIndex(), 2);
        QVERIFY(bridge.terminalActive());
        bridge.readFile("demo/other.cpp");
        QTRY_VERIFY(document(bridge, 1)->loaded());
        QCOMPARE(qvariant_cast<QObject *>(bridge.tabs()[0]), first);
        QCOMPARE(qvariant_cast<QObject *>(bridge.tabs()[1]), file);
        QCOMPARE(qvariant_cast<QObject *>(bridge.tabs()[2]), seventh);
        QCOMPARE(qvariant_cast<QObject *>(bridge.tabs()[3]), document(bridge, 1));
        bridge.selectTab(0);
        QCOMPARE(bridge.activeTerminal(), first);
        bridge.selectTab(1);
        QCOMPARE(bridge.activeIndex(), 0);
        QVERIFY(!bridge.terminalActive());
        // 相同文件/终端索引也能从另一类标签切回来。
        bridge.selectTerminal(0);
        QVERIFY(bridge.terminalActive());
        bridge.selectDocument(0);
        QVERIFY(!bridge.terminalActive());
        const auto shellRequests = server.count(0x0303);
        for (const auto &input : {"@terminal add 7", "@terminal del 42", "@terminal del",
                                 "@terminal add 0", "@terminal del -1", "@terminal add nope",
                                 "@terminal add 2147483648", "@terminal add 3 extra"})
            QVERIFY2(!bridge.handleTerminalCommand(input), input);
        QCOMPARE(server.count(0x0303), shellRequests);
        QCOMPARE(bridge.tabs().size(), 4);
        QVERIFY(bridge.handleTerminalCommand("@terminal del 7"));
        QCOMPARE(bridge.tabs().size(), 3);
        QCOMPARE(bridge.activeTabIndex(), 1); // 删除后台终端，不切走当前文件。
        QCOMPARE(document(bridge), file);
        QVERIFY(bridge.handleTerminalCommand("@terminal add"));
        QTRY_VERIFY(bridge.activeTerminal()->available());
        QCOMPARE(bridge.activeTerminal()->sessionId(), 2);
        QVERIFY(bridge.handleTerminalCommand("@terminal del 2"));
        QVERIFY(!bridge.terminalActive());
        QVERIFY(bridge.handleTerminalCommand("@terminal"));
        QCOMPARE(bridge.activeTerminal(), first);
        QVERIFY(bridge.terminalActive());
        QVERIFY(bridge.handleTerminalCommand("@terminal del 1"));
        QVERIFY(bridge.terminals().isEmpty());
        QVERIFY(!bridge.terminalActive());
        QVERIFY(bridge.handleTerminalCommand("@terminal add 5"));
        QTRY_VERIFY(bridge.activeTerminal()->available());
        QCOMPARE(bridge.activeTerminal()->sessionId(), 5);
    }

    void terminalEarlyEventsAndNoInputReplay() {
        FakeServer server;
        server.intercept = [&](const Call &call) {
            if (call.method == 0x0303) {
                server.terminalEvent(0x0381, terminalPayload(42, "early\r\n"));
                server.respond(call, 0, terminalState(42), true);
                return true;
            }
            if (call.method == 0x0301) {
                server.respond(call, 11, terminalPayload(42, "busy"));
                return true;
            }
            return false;
        };
        FfiClientBridge bridge;
        bridge.connectToServer("127.0.0.1", server.server.serverPort());
        QTRY_VERIFY(bridge.activeTerminal() && bridge.activeTerminal()->available());
        QCOMPARE(bridge.activeTerminal()->output(), QString("early\n"));
        QVERIFY(bridge.submitCommand("one-shot"));
        QTRY_VERIFY(!bridge.activeTerminal()->submitting());
        QTest::qWait(800); // 超过首个读请求的重试间隔。
        QCOMPARE(server.count(0x0301), 1);
        QVERIFY(bridge.activeTerminal()->output().contains("busy"));
    }

    void browserRetryEventsAndOutOfOrder() {
        FakeServer server;
        FfiClientBridge bridge;
        int failures = 0;
        std::optional<Call> delayed;
        server.intercept = [&](const Call &call) {
            if (call.method == 0x0201 && failures++ == 0) {
                server.respond(call, 11, "busy");
                return true;
            }
            if (call.method == 0x0202 && pathOf(call.payload) == "demo/slow") {
                delayed = call;
                return true;
            }
            return false;
        };
        bridge.connectToServer("127.0.0.1", server.server.serverPort());
        QTRY_VERIFY(bridge.connected());
        bridge.openFileBrowser();
        QTRY_COMPARE(bridge.entries().size(), 2);
        QCOMPARE(server.count(0x0201), 2);
        bridge.activateEntry(0);
        QTRY_COMPARE(bridge.directory(), QString("demo"));
        QTRY_VERIFY(!bridge.browsing());
        QCOMPARE(bridge.entries()[0].toMap().value("name").toString(), QString("src"));

        bridge.activateEntry(0);
        QTRY_COMPARE(bridge.entries().size(), 3);
        QCOMPARE(bridge.directory(), QString("demo"));
        QCOMPARE(bridge.entries()[1].toMap().value("depth").toInt(), 1);
        QCOMPARE(bridge.entries()[2].toMap().value("name").toString(), QString("main.cpp"));
        server.event("demo/src", QByteArray(1, char(0)) + string("new.cpp"));
        QTRY_COMPARE(bridge.entries()[1].toMap().value("name").toString(), QString("new.cpp"));
        bridge.activateEntry(0);
        QCOMPARE(bridge.entries().size(), 2);

        bridge.openFileBrowser();
        QTRY_COMPARE(server.count(0x0202), 3);
        QTRY_VERIFY(!bridge.browsing());
        server.event("demo", QByteArray(1, char(1)) + string("slow") + char(1) + string("src")
                             + char(0) + string("main.cpp"));
        QTRY_COMPARE(bridge.entries().size(), 3);
        bridge.activateEntry(0); // slow 的请求仍在等待时，可以展开别的分支。
        QTRY_VERIFY(delayed.has_value());
        bridge.activateEntry(1);
        QTRY_COMPARE(bridge.entries()[2].toMap().value("name").toString(), QString("a.cpp"));
        bridge.activateEntry(0);
        server.respond(*delayed, 0, string("demo/slow") + char(0) + string("old.cpp"));
        QTest::qWait(40);
        QCOMPARE(bridge.directory(), QString("demo"));
        QCOMPARE(bridge.entries().size(), 4); // 折叠后的迟到响应不会把子项重新显示出来。
        QCOMPARE(bridge.entries()[2].toMap().value("name").toString(), QString("a.cpp"));

        delayed.reset();
        bridge.activateEntry(0);
        QTRY_VERIFY(delayed.has_value());
        bridge.listProjects();
        QTRY_COMPARE(bridge.entries().size(), 2);
        bridge.activateEntry(1);
        QTRY_VERIFY(!bridge.browsing());
        server.respond(*delayed, 0, string("demo/slow") + char(0) + string("old.cpp"));
        QTest::qWait(40);
        QCOMPARE(bridge.directory(), QString("other"));
        QCOMPARE(bridge.entries()[0].toMap().value("path").toString(), QString("other/a.cpp"));
    }

    void chunkedReadRevisionChangeAndUtf8() {
        FakeServer server;
        FfiClientBridge bridge;
        // 第一个分段末尾落在 UTF-8 汉字中间，解码必须等完整文件接收后再进行。
        const QByteArray bytes = QByteArray(256 * 1024 - 1, 'x') + QStringLiteral("中文\n").toUtf8();
        server.files["demo/main.cpp"] = bytes;
        bool changed = false;
        server.intercept = [&](const Call &call) {
            if (call.method == 0x0203 && !changed) {
                server.handle(call);
                changed = true;
                ++server.revision;
                server.files["demo/main.cpp"].replace(0, 1, "y");
                return true;
            }
            return false;
        };
        bridge.connectToServer("127.0.0.1", server.server.serverPort());
        QTRY_VERIFY(bridge.connected());
        bridge.readFile("demo/main.cpp");
        QTRY_VERIFY(document(bridge)->loaded());
        QCOMPARE(server.count(0x0203), 4);
        QCOMPARE(document(bridge)->text(), QString::fromUtf8(server.files.value("demo/main.cpp")));
        QVERIFY(!document(bridge)->dirty());
        document(bridge)->setText(document(bridge)->text() + "// edited\n");
        bridge.saveCurrent();
        QTRY_VERIFY(!document(bridge)->dirty());
        QCOMPARE(server.count(0x0204), 1); // 高于 JS 安全整数范围的 revision 仍完整往返。
    }

    void saveSnapshotConflictAndRefreshFailure() {
        FakeServer server;
        FfiClientBridge bridge;
        bridge.connectToServer("127.0.0.1", server.server.serverPort());
        QTRY_VERIFY(bridge.connected());
        bridge.readFile("demo/main.cpp");
        QTRY_VERIFY(document(bridge)->loaded());
        auto *doc = document(bridge);
        std::optional<Call> pending;
        server.intercept = [&](const Call &call) {
            if (call.method == 0x0204) { pending = call; return true; }
            return false;
        };
        doc->setText("first edit\n");
        bridge.saveCurrent();
        QTRY_VERIFY(pending.has_value());
        doc->setText("newer edit\n");
        server.handle(*pending);
        QTRY_VERIFY(!doc->saving());
        QVERIFY(doc->dirty());
        QCOMPARE(doc->text(), QString("newer edit\n"));

        server.intercept = {};
        server.files["demo/main.cpp"] = "external edit\n";
        ++server.revision;
        bridge.saveCurrent();
        QTRY_VERIFY(doc->conflict());
        QCOMPARE(doc->text(), QString("newer edit\n"));
        QCOMPARE(doc->remoteText(), QString("external edit\n"));
        const int writes = server.count(0x0204);
        bridge.saveCurrent();
        QCOMPARE(server.count(0x0204), writes); // 冲突未决时不自动覆盖。
        bridge.refreshCurrent();
        QTRY_VERIFY(!doc->refreshing());
        QVERIFY(doc->conflict()); // 重复刷新也不能清除待处理的冲突。
        bridge.saveLocalVersion();
        QTRY_VERIFY(!doc->dirty());
        QCOMPARE(server.files.value("demo/main.cpp"), QByteArray("newer edit\n"));

        doc->setText("local retained\n");
        server.intercept = [&](const Call &call) {
            if (call.method == 0x0204 || call.method == 0x0203) {
                server.respond(call, 3, "permission denied");
                return true;
            }
            return false;
        };
        bridge.saveCurrent();
        QTRY_VERIFY(doc->stale());
        QTRY_VERIFY(!doc->refreshing());
        QCOMPARE(doc->text(), QString("local retained\n"));
        QVERIFY(doc->dirty());
    }

    void lostWriteReplyReconnect() {
        FakeServer server;
        FfiClientBridge bridge;
        bridge.connectToServer("127.0.0.1", server.server.serverPort());
        QTRY_VERIFY(bridge.connected());
        bridge.readFile("demo/main.cpp");
        QTRY_VERIFY(document(bridge)->loaded());
        auto *doc = document(bridge);
        doc->setText("saved before disconnect\n");
        server.intercept = [&](const Call &call) {
            if (call.method != 0x0204) return false;
            server.files["demo/main.cpp"] = "saved before disconnect\n";
            ++server.revision;
            call.peer->abort(); // 已写入，但 ACK 丢失。
            return true;
        };
        bridge.saveCurrent();
        QTRY_VERIFY(!bridge.connected());
        QTRY_VERIFY_WITH_TIMEOUT(bridge.connected(), 4000);
        QTRY_VERIFY(!doc->stale());
        QVERIFY(!doc->dirty());
        QCOMPARE(doc->text(), QString("saved before disconnect\n"));
        QCOMPARE(server.count(0x0204), 1);
    }

    void utf8AndFrameLimits() {
        FakeServer server;
        FfiClientBridge bridge;
        server.files["demo/main.cpp"] = QByteArray("\xef\xbb\xbf") + "line\r\n";
        bridge.connectToServer("127.0.0.1", server.server.serverPort());
        QTRY_VERIFY(bridge.connected());
        bridge.readFile("demo/main.cpp");
        QTRY_VERIFY(document(bridge)->loaded());
        auto *doc = document(bridge);
        QCOMPARE(doc->text(), QString("line\n"));
        doc->setText("line\nnew\n");
        bridge.saveCurrent();
        QTRY_VERIFY(!doc->dirty());
        QCOMPARE(server.files.value("demo/main.cpp"), QByteArray("\xef\xbb\xbf") + "line\r\nnew\r\n");
        doc->setText(QString(1024 * 1024, QChar('x')));
        bridge.saveCurrent();
        QVERIFY(doc->dirty());
        QCOMPARE(server.count(0x0204), 1);
        server.files["demo/other.cpp"] = QByteArray("invalid\xe4\xb8");
        bridge.readFile("demo/other.cpp");
        QTRY_VERIFY(!document(bridge, 1)->refreshing());
        QVERIFY(!document(bridge, 1)->loaded());
        QVERIFY(document(bridge, 1)->readOnly());
    }

    void syntaxHighlighting() {
        QTextDocument doc;
        CodeHighlighter highlighter(&doc, "test.cppm");
        doc.setPlainText("export module test;\n/* comment\nstill comment */ int x = 42;\n"
                         "const char *url = \"https://example.com\";");
        highlighter.rehighlight();
        QVERIFY(!doc.firstBlock().layout()->formats().isEmpty());
        QCOMPARE(doc.findBlockByNumber(1).userState(), 1);
        QCOMPARE(doc.findBlockByNumber(2).userState(), 0);
        const auto formats = doc.findBlockByNumber(3).layout()->formats();
        QVERIFY(std::any_of(formats.begin(), formats.end(), [](const auto &format) {
            return format.format.foreground().color() == QColor("#ceaa83");
        }));
    }

    void vectorIconsRenderAndUpdate() {
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(
            QStringLiteral(TINA_QML_SOURCE_DIR "/UiIcon.qml")));
        std::unique_ptr<QObject> object(component.createWithInitialProperties(
            {{"name", "chevron-right"}, {"color", QColor(Qt::red)}}));
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *icon = qobject_cast<QQuickItem *>(object.get());
        QVERIFY(icon);
        QQuickWindow window;
        window.setColor(Qt::black);
        window.resize(48, 48);
        icon->setParentItem(window.contentItem());
        icon->setSize(QSizeF(48, 48));
        window.show();
        const auto coloredPixels = [&window](bool red) {
            const auto image = window.grabWindow();
            int count = 0;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    const auto pixel = image.pixelColor(x, y);
                    if (red ? pixel.red() > 100 : pixel.green() > 100)
                        ++count;
                }
            }
            return count;
        };
        QTRY_VERIFY(coloredPixels(true) > 20);
        const auto rightArrow = window.grabWindow();
        icon->setProperty("name", "chevron-down");
        QTRY_VERIFY(window.grabWindow() != rightArrow);
        icon->setProperty("color", QColor(Qt::green));
        QTRY_VERIFY(coloredPixels(false) > 20);
        QTRY_COMPARE(coloredPixels(true), 0);
        // 每一种菜单、文件树和模型图标都必须有实际绘制内容。
        for (const auto *name : {"folder", "file", "link", "arrow-up", "sparkle",
                                 "diamond", "cluster", "branch", "more", "terminal"}) {
            icon->setProperty("name", "");
            QTRY_COMPARE(coloredPixels(false), 0);
            icon->setProperty("name", name);
            QTRY_VERIFY(coloredPixels(false) > 20);
        }
    }

    void keyboardAndEditorState() {
        FakeServer server;
        qmlRegisterType<FfiClientBridge>("TinaIde", 1, 0, "FfiClientBridge");
        qmlRegisterUncreatableType<WorkspaceDocument>("TinaIde", 1, 0, "WorkspaceDocument", "test");
        qmlRegisterUncreatableType<TerminalSession>("TinaIde", 1, 0, "TerminalSession", "test");
        QQmlApplicationEngine engine;
        QList<QQmlError> warnings;
        connect(&engine, &QQmlEngine::warnings, &engine, [&](const QList<QQmlError> &errors) {
            warnings.append(errors);
        });
        engine.setInitialProperties({{"serverHost", "127.0.0.1"},
                                     {"serverPort", server.server.serverPort()}});
        engine.load(QUrl::fromLocalFile(QStringLiteral(TINA_QML_SOURCE_DIR "/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        auto *bridge = window->findChild<FfiClientBridge *>("workspaceBridge");
        QVERIFY(bridge);
        QTRY_VERIFY(bridge->connected());
        window->requestActivate();
        QTRY_VERIFY(window->isActive());
        auto *command = window->findChild<QQuickItem *>("commandInput");
        QVERIFY(command);
        auto *bar = findVisualItem(window->contentItem(), "bottomDialogBar");
        auto *codeArea = findVisualItem(window->contentItem(), "centerCodeArea");
        auto *folderButton = findVisualItem(window->contentItem(), "folderButton");
        auto *submitButton = findVisualItem(window->contentItem(), "submitButton");
        QVERIFY(bar && codeArea && folderButton && submitButton);
        QCOMPARE(bar->height(), 43.0);
        QVERIFY(!findVisualItem(window->contentItem(), "newTerminalButton"));
        QVERIFY(!findVisualItem(window->contentItem(), "closeTerminal:0"));
        // 悬浮输入框与代码区等宽，代码区本身仍延伸到窗口底边。
        QCOMPARE(bar->mapToItem(codeArea, QPointF()).x(), 0.0);
        QCOMPARE(bar->width(), codeArea->width());
        QCOMPARE(codeArea->mapToScene(QPointF(0, codeArea->height())).y(), qreal(window->height()));
        QCOMPARE(folderButton->mapToItem(bar, QPointF()), QPointF(0, bar->height() - 43));
        QCOMPARE(folderButton->width(), 43.0);
        QCOMPARE(folderButton->height(), 43.0);
        QCOMPARE(submitButton->mapToItem(bar, QPointF(submitButton->width(), submitButton->height())),
                 QPointF(bar->width(), bar->height()));
        QCOMPARE(submitButton->width(), 43.0);
        QCOMPARE(submitButton->height(), 43.0);
        auto *resizeHandle = findVisualItem(window->contentItem(), "terminalResizeHandle");
        QVERIFY(resizeHandle);
        const auto handleCenter = resizeHandle->mapToScene(QPointF(resizeHandle->width() / 2, 8)).toPoint();
        const auto originalHeight = bar->height();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, handleCenter);
        QTest::mouseMove(window, handleCenter - QPoint(0, 200), 30);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, handleCenter - QPoint(0, 200));
        QTRY_COMPARE(bar->height(), originalHeight + 200);
        const auto newHandleCenter = resizeHandle->mapToScene(QPointF(resizeHandle->width() / 2, 8)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, newHandleCenter);
        QTest::mouseMove(window, newHandleCenter + QPoint(0, 300), 30);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, newHandleCenter + QPoint(0, 300));
        QTRY_COMPARE(bar->height(), originalHeight);
        bar->setProperty("requestedHeight", originalHeight);
        QTRY_VERIFY(bridge->activeTerminal() && bridge->activeTerminal()->available());
        command->forceActiveFocus();
        command->setProperty("text", "echo terminal");
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(bridge->activeTerminal()->output().contains("echo terminal\n"));
        QTRY_COMPARE(command->property("text").toString(), QString());
        const auto outputLines = QString::fromUtf8("构建终端测试 · 中文和 emoji 🙂\r\n")
                                     + "[1/3] Connecting to shell...\r\n"
                                       "[2/3] Per-terminal ring buffer ready\r\n"
                                       "[3/3] Build finished\r\n";
        server.terminalEvent(0x0381, terminalPayload(bridge->activeTerminal()->terminalId(), outputLines.toUtf8()));
        QTRY_VERIFY(bridge->activeTerminal()->output().contains("Build finished"));
        bar->setProperty("requestedHeight", 360);
        QTest::qWait(100);
        window->grabWindow().save(QStringLiteral("terminal-preview.png"));
        bar->setProperty("requestedHeight", originalHeight);
        command->setProperty("text", "com");
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_COMPARE(command->property("text").toString(), QString("completed"));
        auto *firstTerminal = bridge->activeTerminal();
        bridge->createTerminal();
        QTRY_VERIFY(bridge->activeTerminal()->available());
        QTRY_COMPARE(command->property("text").toString(), QString());
        bridge->selectTerminal(0);
        QCOMPARE(bridge->activeTerminal(), firstTerminal);
        QTRY_COMPARE(command->property("text").toString(), QString("completed"));
        command->setProperty("text", "@");
        auto *terminalMenu = findVisualItem(window->contentItem(), "commandMenu:TERMINAL");
        QVERIFY(terminalMenu);
        QTRY_VERIFY(terminalMenu->isVisible());
        QTest::qWait(160);
        window->grabWindow().save(QStringLiteral("terminal-menu-preview.png"));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          terminalMenu->mapToScene(QPointF(terminalMenu->width() / 2, terminalMenu->height() / 2)).toPoint());
        QTRY_COMPARE(command->property("text").toString(), QString("@terminal "));
        command->setProperty("text", "@terminal add 7");
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(bridge->activeTerminal()->available());
        QCOMPARE(bridge->activeTerminal()->sessionId(), 7);
        QTRY_COMPARE(command->property("text").toString(), QString());
        command->setProperty("text", "@terminal del 7");
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(bridge->terminals().size(), 2);
        auto *remainingTerminal = bridge->activeTerminal();
        remainingTerminal->setDraft("keep me");
        command->setProperty("text", "@terminal add 7");
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(bridge->activeTerminal()->available());
        QCOMPARE(remainingTerminal->draft(), QString());
        remainingTerminal->setDraft("keep me");
        command->setProperty("text", "@terminal del 7");
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(bridge->activeTerminal(), remainingTerminal);
        QCOMPARE(command->property("text").toString(), QString("keep me"));
        command->setProperty("text", "@terminal del 999");
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(command->property("text").toString(), QString("@terminal del 999"));
        QVERIFY(bar->property("commandError").toString().contains("999"));
        command->forceActiveFocus();
        command->setProperty("text", "@FILE");
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(bridge->entries().size(), 2);
        QTest::keyClick(window, Qt::Key_Return); // demo
        QTRY_COMPARE(bridge->directory(), QString("demo"));
        QTRY_VERIFY(!bridge->browsing());
        auto *fileList = findVisualItem(window->contentItem(), "fileBrowserList");
        QVERIFY(fileList);
        QTest::keyClick(window, Qt::Key_Right); // 展开 src，保持选中目录。
        QTRY_COMPARE(bridge->entries().size(), 3);
        QCOMPARE(fileList->property("currentIndex").toInt(), 0);
        QTest::keyClick(window, Qt::Key_Right); // 进入首个子项的选中态。
        QCOMPARE(fileList->property("currentIndex").toInt(), 1);
        QTest::keyClick(window, Qt::Key_Left);
        QCOMPARE(fileList->property("currentIndex").toInt(), 0);
        QTest::keyClick(window, Qt::Key_Left);
        QCOMPARE(bridge->entries().size(), 2);

        const auto *folder = findVisualItem(window->contentItem(), "fileRow:demo/src");
        QVERIFY(folder);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          folder->mapToScene(QPointF(folder->width() / 2, folder->height() / 2)).toPoint());
        QTRY_COMPARE(bridge->entries().size(), 3);
        QTest::keyClick(window, Qt::Key_Down); // a.cpp
        QTest::keyClick(window, Qt::Key_Down); // main.cpp 仍与 src 同级。
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(bridge->documents().size(), 1);
        QTRY_VERIFY(document(*bridge)->loaded());
        QCOMPARE(document(*bridge)->path(), QString("demo/main.cpp"));
        QTRY_COMPARE(bar->height(), originalHeight);
        auto *terminalTab = findVisualItem(window->contentItem(), "terminalTab:1");
        auto *fileTab = findVisualItem(window->contentItem(), "fileTab:demo/main.cpp");
        QVERIFY(terminalTab && fileTab);
        QCOMPARE(terminalTab->parentItem(), fileTab->parentItem());
        QCOMPARE(terminalTab->height(), fileTab->height());
        auto *editor = findVisualItem(window->contentItem(), "editor:demo/main.cpp");
        QVERIFY(editor);
        auto *viewport = findVisualItem(window->contentItem(), "codeViewport:demo/main.cpp");
        QVERIFY(viewport);
        // 无冲突时，第一行代码直接从代码区顶部开始，不再空出文件名栏。
        QTRY_COMPARE(viewport->mapToItem(codeArea, QPointF()).y(), 0.0);
        QTRY_VERIFY(editor->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_End, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_X);
        QTRY_VERIFY(document(*bridge)->dirty());
        window->grabWindow().save(QStringLiteral("workspace-preview.png"));
        QTest::keyClick(window, Qt::Key_S, Qt::ControlModifier);
        QTRY_VERIFY(!document(*bridge)->dirty());
        bridge->readFile("demo/other.cpp");
        QTRY_VERIFY(document(*bridge, 1)->loaded());
        QTest::keyClick(window, Qt::Key_Tab, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(bridge->activeIndex(), 0);
        QCOMPARE(findVisualItem(window->contentItem(), "editor:demo/main.cpp"), editor);
        QTRY_VERIFY(editor->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(document(*bridge)->dirty()); // 切换标签后撤销栈仍在。
        QCOMPARE(document(*bridge)->text(), QString("int main() {\n    return 0;\n}\n"));

        // 长按后松手仍停留在项目列表，触屏用户可以直接切换项目。
        const auto folderCenter = folderButton->mapToScene(
            QPointF(folderButton->width() / 2, folderButton->height() / 2)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, folderCenter);
        QTest::qWait(QGuiApplication::styleHints()->mousePressAndHoldInterval() + 100);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, folderCenter);
        QTRY_VERIFY(bridge->directory().isEmpty());
        QTRY_COMPARE(bridge->entries().size(), 2);
        QVERIFY(fileList->isVisible());
        QTest::keyClick(window, Qt::Key_Escape);
        command->forceActiveFocus();
        command->setProperty("text", "@AI");
        QTest::keyClick(window, Qt::Key_Return);
        auto *agent = findVisualItem(window->contentItem(), "agentPanel");
        auto *tabs = findVisualItem(window->contentItem(), "fileTabs");
        QVERIFY(agent && tabs);
        QTRY_VERIFY(agent->isVisible());
        for (const auto &size : {QSize(1280, 800), QSize(1600, 1000), QSize(960, 600)}) {
            window->resize(size);
            QTRY_COMPARE(bar->mapToScene(QPointF(0, bar->height())).y(),
                         agent->mapToScene(QPointF(0, agent->height())).y());
            QCOMPARE(bar->mapToItem(codeArea, QPointF()).x(), 0.0);
            QCOMPARE(bar->width(), codeArea->width());
            const auto gap = codeArea->x() - tabs->width();
            QVERIFY(gap > 0 && gap <= 12);
            QVERIFY(tabs->width() < 132);
        }
        window->resize(1280, 800);
        QTest::qWait(200);
        window->grabWindow().save(QStringLiteral("workspace-tablet-preview.png"));
        for (const auto &warning : warnings)
            qWarning() << warning;
        QVERIFY(warnings.isEmpty());
    }
};

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");
    WorkspaceTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "workspace_test.moc"
