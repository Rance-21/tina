#pragma once

#include "workspace_document.hpp"
#include "terminal_session.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QVariantList>
#include <functional>
#include <memory>
#include <QtQmlIntegration/qqmlintegration.h>

namespace tina::ui {

// Qt 异步套接字实现 ide.client 的 Workspace 和 Terminal 操作。
// Android 不依赖服务端 Linux epoll / C++ 模块构建链，线上格式保持完全一致。
class FfiClientBridge : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool connected READ connected NOTIFY connectionChanged)
    Q_PROPERTY(QString directory READ directory NOTIFY browserChanged)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY browserChanged)
    Q_PROPERTY(bool browsing READ browsing NOTIFY browserChanged)
    Q_PROPERTY(QVariantList documents READ documents NOTIFY documentsChanged)
    Q_PROPERTY(int activeIndex READ activeIndex WRITE selectDocument NOTIFY activeIndexChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(QVariantList terminals READ terminals NOTIFY terminalsChanged)
    Q_PROPERTY(int activeTerminalIndex READ activeTerminalIndex WRITE selectTerminal NOTIFY activeTerminalChanged)
    Q_PROPERTY(tina::ui::TerminalSession *activeTerminal READ activeTerminal NOTIFY activeTerminalChanged)
    Q_PROPERTY(QVariantList tabs READ tabs NOTIFY tabsChanged)
    Q_PROPERTY(int activeTabIndex READ activeTabIndex WRITE selectTab NOTIFY activeTabChanged)
    Q_PROPERTY(bool terminalActive READ terminalActive NOTIFY activeTabChanged)

  public:
    explicit FfiClientBridge(QObject *parent = nullptr);
    ~FfiClientBridge() override;
    bool connected() const;
    QString directory() const { return directory_; }
    QVariantList entries() const { return entries_; }
    bool browsing() const;
    QVariantList documents() const;
    int activeIndex() const { return activeIndex_; }
    QString message() const { return message_; }
    QVariantList terminals() const;
    int activeTerminalIndex() const { return activeTerminalIndex_; }
    TerminalSession *activeTerminal() const;
    QVariantList tabs() const;
    int activeTabIndex() const { return tabs_.indexOf(activeTab_); }
    bool terminalActive() const { return qobject_cast<TerminalSession *>(activeTab_) != nullptr; }

    Q_INVOKABLE void connectToServer(const QString &host, int port = 3490);
    Q_INVOKABLE void ping();
    Q_INVOKABLE void openFileBrowser();
    Q_INVOKABLE void listProjects();
    Q_INVOKABLE void listDir(const QString &path);
    Q_INVOKABLE void activateEntry(int index);
    Q_INVOKABLE void setEntryExpanded(int index, bool expanded);
    Q_INVOKABLE void readFile(const QString &path);
    Q_INVOKABLE void selectDocument(int index);
    Q_INVOKABLE void saveCurrent();
    Q_INVOKABLE void writeFile(WorkspaceDocument *document);
    Q_INVOKABLE void refreshCurrent();
    Q_INVOKABLE void useServerVersion();
    Q_INVOKABLE void saveLocalVersion();
    Q_INVOKABLE void selectTab(int index);
    Q_INVOKABLE bool createTerminal(int id = 0);
    Q_INVOKABLE void selectTerminal(int index);
    Q_INVOKABLE bool closeTerminal(int index);
    Q_INVOKABLE bool handleTerminalCommand(const QString &input);
    Q_INVOKABLE bool submitCommand(const QString &command);
    Q_INVOKABLE void completeTerminalInput();
    Q_INVOKABLE void interruptTerminal();
    Q_INVOKABLE void submitAgentInput(const QString &input);

  signals:
    void connectionChanged();
    void connectionStateChanged(bool connected);
    void pingResult(bool ok, const QString &message);
    void browserChanged();
    void documentsChanged();
    void activeIndexChanged();
    void messageChanged();
    void fileOpened();
    void commandSubmitted(const QString &command);
    void openAgentWindowRequested();
    void terminalsChanged();
    void activeTerminalChanged();
    void tabsChanged();
    void activeTabChanged();

  private:
    using Reply = std::function<void(quint16, const QByteArray &)>;
    struct Request {
        quint16 method{};
        QByteArray payload;
        Reply reply;
        quint64 wireId{};
        int attempts{};
        qint64 deadline{};
        bool waiting{};
    };
    struct ReadTransfer {
        QByteArray bytes;
        quint64 size{};
        quint64 revision{};
        quint64 generation{};
        int restarts{};
    };
    struct BrowserDirectory {
        QVariantList children;
        quint64 requestId{};
        bool expanded{};
        bool refreshPending{};
    };
    void request(quint16 method, QByteArray payload, Reply reply);
    void sendRequest(quint64 token);
    void completeRequest(quint64 token, quint16 status, const QByteArray &payload);
    void failRequests(const QByteArray &reason);
    void onReadyRead();
    void pollRequests();
    void scheduleReconnect();
    void setBrowserRoot(const QString &path);
    void fetchDirectory(QString path);
    void applyDirectoryEntries(const QString &path, QVariantList entries);
    void discardDirectory(const QString &path);
    void rebuildBrowserEntries();
    bool decodeDirectory(const QByteArray &payload, QString &path, QVariantList &entries);
    void refreshDocument(WorkspaceDocument *document);
    void readChunk(WorkspaceDocument *document, std::shared_ptr<ReadTransfer> transfer);
    WorkspaceDocument *currentDocument() const;
    void setMessage(const QString &message);
    void setActiveTab(QObject *tab);
    void startTerminal(TerminalSession *terminal);
    TerminalSession *findTerminal(quint32 id) const;
    void handleTerminalEvent(quint16 method, const QByteArray &payload);
    void invalidateTerminals();
    void terminalError(TerminalSession *terminal, const QString &message);

    QTcpSocket socket_;
    QTimer pollTimer_;
    QTimer reconnectTimer_;
    QElapsedTimer clock_;
    QByteArray input_;
    QHash<quint64, Request> requests_;
    quint64 nextId_{1};
    QString host_;
    quint16 port_{3490};
    int reconnectAttempts_{};
    QString directory_;
    QHash<QString, BrowserDirectory> browserDirectories_;
    QVariantList entries_;
    bool browserRequested_{};
    QList<WorkspaceDocument *> documents_;
    int activeIndex_{-1};
    QString message_;
    QList<TerminalSession *> terminals_;
    int activeTerminalIndex_{-1};
    QList<QObject *> tabs_;
    QObject *activeTab_{};
    // start 的响应可能晚于首条输出；暂存同样有容量和会话数量上限。
    QHash<quint32, QByteArray> earlyTerminalOutput_;
};

} // namespace tina::ui
