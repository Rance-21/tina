#include "ffi_client_bridge.hpp"

#include <QSet>
#include <QStringConverter>
#include <QtEndian>
#include <algorithm>
#include <utility>

namespace tina::ui {
namespace {
constexpr quint16 Ping = 0x0001;
constexpr quint16 ListProjects = 0x0201;
constexpr quint16 ListDir = 0x0202;
constexpr quint16 ReadFile = 0x0203;
constexpr quint16 WriteFile = 0x0204;
constexpr quint16 CloseDir = 0x0205;
constexpr quint16 Unavailable = 11;
constexpr quint32 TransientStatusMask = (1u << 6) | (1u << 7) | (1u << 10) | (1u << 11) | (1u << 12);
constexpr quint32 MaxFrame = 1024 * 1024;
constexpr quint32 ReadChunk = 256 * 1024;
constexpr quint64 MaxEditableRead = 8 * 1024 * 1024;

template<typename T> void appendInteger(QByteArray &bytes, T value) {
    const auto offset = bytes.size();
    bytes.resize(offset + sizeof(T));
    qToBigEndian(value, reinterpret_cast<uchar *>(bytes.data() + offset));
}

QByteArray pathPayload(const QString &path) {
    auto bytes = path.toUtf8();
    QByteArray payload;
    appendInteger(payload, quint16(bytes.size()));
    payload.append(bytes);
    return payload;
}

bool validPath(const QString &path) {
    if (path.isEmpty() || path.toUtf8().size() > 65535 || path.contains(QChar::Null))
        return false;
    for (const auto &part : path.split('/')) {
        if (part.isEmpty() || part == "." || part == "..")
            return false;
    }
    return true;
}

// 响应解析先检查边界和 UTF-8，错误数据不得进入可保存的编辑缓冲区。
bool readString(const QByteArray &bytes, qsizetype &offset, QString &value) {
    if (bytes.size() - offset < 2)
        return false;
    const auto size = qFromBigEndian<quint16>(
        reinterpret_cast<const uchar *>(bytes.constData() + offset));
    offset += 2;
    if (!size || size > bytes.size() - offset)
        return false;
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    value = decoder(QByteArrayView(bytes.constData() + offset, size));
    offset += size;
    return !decoder.hasError() && !value.contains(QChar::Null);
}

QVariantMap entry(const QString &name, const QString &path, int type) {
    return {{"name", name}, {"path", path}, {"type", type}};
}

void sortEntries(QVariantList &entries) {
    std::sort(entries.begin(), entries.end(), [](const QVariant &a, const QVariant &b) {
        const auto left = a.toMap(), right = b.toMap();
        const bool leftDir = left.value("type").toInt() == 1;
        const bool rightDir = right.value("type").toInt() == 1;
        if (leftDir != rightDir)
            return leftDir;
        return QString::compare(left.value("name").toString(), right.value("name").toString(),
                                Qt::CaseInsensitive) < 0;
    });
}
} // namespace

FfiClientBridge::FfiClientBridge(QObject *parent) : QObject(parent) {
    clock_.start();
    pollTimer_.setInterval(100);
    connect(&pollTimer_, &QTimer::timeout, this, &FfiClientBridge::pollRequests);
    pollTimer_.start();
    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, [this] {
        socket_.abort();
        socket_.connectToHost(host_, port_);
        scheduleReconnect();
    });
    connect(&socket_, &QTcpSocket::connected, this, [this] {
        reconnectTimer_.stop();
        reconnectAttempts_ = 0;
        input_.clear();
        emit connectionChanged();
        emit connectionStateChanged(true);
        setMessage(QStringLiteral("已连接服务端"));
        ping();
        if (browserRequested_)
            openFileBrowser();
        // 重连后读回最新版本；包括写入已成功但响应丢失的情况。
        for (auto *document : documents_)
            if (!document->preview_)
                refreshDocument(document);
        if (terminals_.isEmpty())
            createTerminal();
        else
            for (auto *terminal : std::as_const(terminals_))
                startTerminal(terminal); // 新 Shell，不重放断线前的命令。
    });
    connect(&socket_, &QTcpSocket::disconnected, this, [this] {
        input_.clear();
        failRequests(QStringLiteral("连接断开").toUtf8());
        invalidateTerminals();
        for (auto *document : documents_) {
            if (document->preview_)
                continue;
            document->stale_ = true;
            emit document->stateChanged();
        }
        emit connectionChanged();
        emit connectionStateChanged(false);
        scheduleReconnect();
    });
    connect(&socket_, &QAbstractSocket::errorOccurred, this, [this] {
        setMessage(QStringLiteral("连接失败，正在重连：") + socket_.errorString());
        emit pingResult(false, socket_.errorString());
        scheduleReconnect();
    });
    connect(&socket_, &QTcpSocket::readyRead, this, &FfiClientBridge::onReadyRead);
}

FfiClientBridge::~FfiClientBridge() {
    // 成员按逆序析构；先解除套接字信号，避免其析构时回调已经销毁的请求表。
    socket_.disconnect(this);
    socket_.abort();
}

bool FfiClientBridge::connected() const {
    return socket_.state() == QAbstractSocket::ConnectedState;
}

void FfiClientBridge::setMessage(const QString &message) {
    message_ = message;
    emit messageChanged();
}

void FfiClientBridge::connectToServer(const QString &host, int port) {
    if (host.trimmed().isEmpty() || port <= 0 || port > 65535) {
        setMessage(QStringLiteral("服务器地址或端口无效"));
        return;
    }
    host_ = host.trimmed();
    port_ = static_cast<quint16>(port);
    reconnectTimer_.stop();
    socket_.abort();
    failRequests(QStringLiteral("正在重新连接").toUtf8());
    input_.clear();
    socket_.connectToHost(host_, port_);
    // 同时覆盖连接建立阶段卡住的情况，退避上限为 10 秒。
    scheduleReconnect();
}

void FfiClientBridge::scheduleReconnect() {
    if (host_.isEmpty() || reconnectTimer_.isActive())
        return;
    const int delay = std::min(10000, 500 * (1 << std::min(reconnectAttempts_++, 4)));
    reconnectTimer_.start(delay);
}

void FfiClientBridge::request(quint16 method, QByteArray payload, Reply reply) {
    if (!connected()) {
        reply(Unavailable, QStringLiteral("未连接服务端，连接恢复后可重新获取").toUtf8());
        return;
    }
    if (payload.size() > MaxFrame - 13) {
        reply(1, QStringLiteral("请求超过服务端 1 MiB 帧上限").toUtf8());
        return;
    }
    const auto token = nextId_++;
    requests_.insert(token, Request{method, std::move(payload), std::move(reply)});
    sendRequest(token);
}

void FfiClientBridge::sendRequest(quint64 token) {
    auto it = requests_.find(token);
    if (it == requests_.end())
        return;
    auto &req = it.value();
    req.wireId = nextId_++;
    ++req.attempts;
    req.waiting = true;
    req.deadline = clock_.elapsed() + 6000;
    QByteArray frame;
    appendInteger(frame, quint32(13 + req.payload.size()));
    frame.append(char(1));
    appendInteger(frame, req.wireId);
    appendInteger(frame, req.method);
    appendInteger(frame, quint16(0));
    frame.append(req.payload);
    if (socket_.write(frame) != frame.size())
        completeRequest(token, Unavailable, QStringLiteral("发送失败").toUtf8());
}

void FfiClientBridge::completeRequest(quint64 token, quint16 status, const QByteArray &payload) {
    auto it = requests_.find(token);
    if (it == requests_.end())
        return;
    // 只有无副作用的读操作可以重试；Shell 创建、输入、补全不可重放。
    const bool transient = status <= 12 && ((TransientStatusMask >> status) & 1u) != 0;
    const bool retryable = it->method == Ping || it->method == ListProjects
                           || (it->method == ListDir
                               && !closingDirectories_.contains(QString::fromUtf8(it->payload.sliced(2))))
                           || it->method == ReadFile
                           || (it->method >= 0x0401 && it->method <= 0x0403);
    if (status && transient && retryable && it->attempts < 3 && connected()) {
        it->waiting = false;
        it->wireId = 0;
        it->deadline = clock_.elapsed() + 250 * it->attempts;
        setMessage(QStringLiteral("服务端暂时无法完成请求，正在重新获取…"));
        return;
    }
    auto reply = std::move(it->reply);
    requests_.erase(it);
    reply(status, payload);
}

void FfiClientBridge::pollRequests() {
    const auto now = clock_.elapsed();
    for (const auto token : requests_.keys()) {
        const auto it = requests_.constFind(token);
        if (it == requests_.cend() || it->deadline > now)
            continue;
        if (it->waiting)
            completeRequest(token, 12, QStringLiteral("请求超时").toUtf8());
        else
            sendRequest(token);
    }
}

void FfiClientBridge::failRequests(const QByteArray &reason) {
    auto requests = std::move(requests_);
    requests_.clear();
    for (auto &request : requests)
        request.reply(Unavailable, reason);
}

void FfiClientBridge::onReadyRead() {
    input_.append(socket_.readAll());
    while (input_.size() >= 4) {
        const auto *data = reinterpret_cast<const uchar *>(input_.constData());
        const auto length = qFromBigEndian<quint32>(data);
        if (length < 13 || length > MaxFrame) {
            setMessage(QStringLiteral("服务端帧长度无效，重新连接"));
            socket_.abort();
            return;
        }
        if (input_.size() < 4 + length)
            return; // TCP 半包：保留缓冲区，等下一次 readyRead。
        const auto kind = data[4];
        const auto id = qFromBigEndian<quint64>(data + 5);
        const auto method = qFromBigEndian<quint16>(data + 13);
        const auto status = qFromBigEndian<quint16>(data + 15);
        const QByteArray payload(input_.constData() + 17, length - 13);
        input_.remove(0, 4 + length);
        if ((kind != 2 && kind != 3) || (kind == 2 && (!id || status > 12))
            || (kind == 3 && (id || status))) {
            setMessage(QStringLiteral("服务端帧头无效，重新连接"));
            socket_.abort();
            return;
        }
        if (kind == 3 && method == ListDir) {
            QString path;
            QVariantList entries;
            if (!decodeDirectory(payload, path, entries))
                continue;
            auto directory = browserDirectories_.find(path);
            if (directory != browserDirectories_.end() && !closingDirectories_.contains(path)) {
                if (directory->requestId)
                    directory->refreshPending = true;
                else
                    applyDirectoryEntries(path, std::move(entries));
            }
            continue;
        }
        if (kind == 3) {
            handleTerminalEvent(method, payload);
            continue;
        }
        // 按请求 ID 和方法匹配，允许多个文件同时读取、响应乱序及迟到响应。
        for (auto it = requests_.cbegin(); it != requests_.cend(); ++it) {
            if (it->wireId == id && it->method == method) {
                completeRequest(it.key(), status, payload);
                break;
            }
        }
    }
}

void FfiClientBridge::ping() {
    request(Ping, {}, [this](quint16 status, const QByteArray &payload) {
        emit pingResult(status == 0 && payload == "PONG", QString::fromUtf8(payload));
    });
}

bool FfiClientBridge::decodeDirectory(const QByteArray &payload, QString &path,
                                     QVariantList &entries) {
    qsizetype offset = 0;
    if (!readString(payload, offset, path) || !validPath(path))
        return false;
    while (offset < payload.size()) {
        const int type = static_cast<quint8>(payload.at(offset++));
        QString name;
        if (type > 2 || !readString(payload, offset, name) || !validPath(name) || name.contains('/'))
            return false;
        entries.append(entry(name, path + '/' + name, type));
    }
    sortEntries(entries);
    return true;
}

void FfiClientBridge::openFileBrowser() {
    browserRequested_ = true;
    // 重开弹窗或重连时重新获取项目根和当前可见的展开分支，保留树的展开状态。
    const auto visibleRows = entries_;
    fetchDirectory(directory_);
    for (const auto &row : visibleRows) {
        const auto item = row.toMap();
        if (item.value("expanded").toBool())
            fetchDirectory(item.value("path").toString());
    }
}

void FfiClientBridge::listProjects() {
    setBrowserRoot({});
}

void FfiClientBridge::listDir(const QString &path) {
    if (!validPath(path)) {
        setMessage(QStringLiteral("目录路径无效"));
        return;
    }
    setBrowserRoot(path);
}

void FfiClientBridge::setBrowserRoot(const QString &path) {
    browserRequested_ = true;
    if (directory_ != path) {
        directory_ = path;
        // 切换项目也会移除整棵树，逐个释放原项目已加载目录的服务端监听。
        const auto paths = browserDirectories_.keys();
        for (const auto &oldPath : paths)
            discardDirectory(oldPath);
    }
    fetchDirectory(path);
}

bool FfiClientBridge::browsing() const {
    const auto root = browserDirectories_.constFind(directory_);
    return root != browserDirectories_.cend()
           && (root->requestId != 0 || closingDirectories_.contains(directory_));
}

void FfiClientBridge::fetchDirectory(QString path) {
    auto &directory = browserDirectories_[path];
    if (directory.requestId || closingDirectories_.contains(path)) {
        rebuildBrowserEntries();
        return;
    }
    const auto requestId = nextId_++;
    directory.requestId = requestId;
    rebuildBrowserEntries();
    request(path.isEmpty() ? ListProjects : ListDir, path.isEmpty() ? QByteArray{} : pathPayload(path),
            [this, path, requestId](quint16 status, const QByteArray &payload) {
        const auto closing = closingDirectories_.constFind(path);
        if (closing != closingDirectories_.cend() && closing.value() == requestId) {
            // 服务端在 list_dir 完成时才添加监听，必须等旧读取结束再关闭，避免监听泄漏。
            closeDirectory(path);
            return;
        }
        auto directory = browserDirectories_.find(path);
        if (directory == browserDirectories_.end() || directory->requestId != requestId)
            return; // 切换项目或删除分支后，丢弃旧请求的响应。
        directory->requestId = 0;
        const bool refreshPending = std::exchange(directory->refreshPending, false);
        QVariantList entries;
        bool valid = true;
        if (!status && path.isEmpty()) {
            qsizetype offset = 0;
            while (offset < payload.size()) {
                QString name;
                if (!readString(payload, offset, name) || !validPath(name) || name.contains('/')) {
                    valid = false;
                    break;
                }
                entries.append(entry(name, name, 1));
            }
            sortEntries(entries);
        } else if (!status) {
            QString receivedPath;
            valid = decodeDirectory(payload, receivedPath, entries) && receivedPath == path;
        }
        if (status || !valid) {
            setMessage(QStringLiteral("目录获取失败：")
                       + (status ? QString::fromUtf8(payload) : QStringLiteral("响应格式无效")));
            if (status == 4 && path == directory_ && !path.isEmpty()) {
                listProjects();
                return;
            }
            if (status == 4 && !path.isEmpty()) {
                // 分支已删除，重取它的父目录，其他分支仍留在原位。
                discardDirectory(path);
                fetchDirectory(path.left(path.lastIndexOf('/')));
            }
            rebuildBrowserEntries();
            return;
        }
        setMessage(entries.isEmpty() ? QStringLiteral("此处没有文件或项目")
                                    : QStringLiteral("↑ ↓ 选择，← → 折叠或展开，Enter 打开"));
        applyDirectoryEntries(path, std::move(entries));
        // 通知与读取交错时再取一次同一分支，避免迟到的快照盖掉变更。
        if (refreshPending)
            fetchDirectory(path);
    });
}

void FfiClientBridge::applyDirectoryEntries(const QString &path, QVariantList entries) {
    auto &directory = browserDirectories_[path];
    QSet<QString> removed;
    for (const auto &row : directory.children) {
        const auto child = row.toMap().value("path").toString();
        if (browserDirectories_.contains(child))
            removed.insert(child);
    }
    for (const auto &row : entries) {
        const auto item = row.toMap();
        if (item.value("type").toInt() == 1)
            removed.remove(item.value("path").toString());
    }
    directory.children = std::move(entries);
    for (const auto &child : removed)
        discardDirectory(child);
    rebuildBrowserEntries();
}

void FfiClientBridge::discardDirectory(const QString &path) {
    if (!browserDirectories_.contains(path))
        return;
    const auto directory = browserDirectories_.take(path);
    for (const auto &row : directory.children) {
        const auto item = row.toMap();
        if (item.value("type").toInt() == 1)
            discardDirectory(item.value("path").toString());
    }
    // 服务端只关闭指定目录，不会递归；子目录也要分别取消监听。
    if (!path.isEmpty() && !closingDirectories_.contains(path)) {
        closingDirectories_.insert(path, directory.requestId);
        if (!directory.requestId)
            closeDirectory(path);
    }
}

void FfiClientBridge::closeDirectory(const QString &path) {
    closingDirectories_[path] = 0;
    // 0x0205 请求与响应均为大端 u16 字节长度加 UTF-8 路径；关闭请求不自动重放。
    request(CloseDir, pathPayload(path), [this, path](quint16 status, const QByteArray &payload) {
        closingDirectories_.remove(path);
        qsizetype offset = 0;
        QString closedPath;
        // 与公共客户端一致，空响应表示已关闭请求路径；同时兼容四字节长度的旧响应。
        if (payload.startsWith(QByteArray(2, '\0')))
            offset = 2;
        const bool valid = payload.isEmpty()
                           || (readString(payload, offset, closedPath)
                               && offset == payload.size() && closedPath == path);
        if (status || !valid)
            setMessage(QStringLiteral("目录关闭失败：")
                       + (status ? QString::fromUtf8(payload) : QStringLiteral("响应格式无效")));
        // 用户快速重新展开时，先等 close_dir 收尾，再重新读取并恢复监听。
        const auto directory = browserDirectories_.constFind(path);
        if (connected() && directory != browserDirectories_.cend()
            && (path == directory_ || directory->expanded))
            fetchDirectory(path);
        else
            rebuildBrowserEntries();
    });
}

void FfiClientBridge::rebuildBrowserEntries() {
    // 用扁平列表显示可见行，仍由 ListView 按需创建代理；已关闭分支不保留子项缓存。
    entries_.clear();
    const auto appendVisible = [this](auto &&append, const QString &path, int depth) -> void {
        const auto directory = browserDirectories_.constFind(path);
        if (directory == browserDirectories_.cend())
            return;
        for (const auto &row : directory->children) {
            auto item = row.toMap();
            const auto childPath = item.value("path").toString();
            const auto child = browserDirectories_.constFind(childPath);
            const bool expanded = child != browserDirectories_.cend() && child->expanded;
            item.insert("depth", depth);
            item.insert("expanded", expanded);
            item.insert("loading", expanded
                        && (child->requestId != 0 || closingDirectories_.contains(childPath)));
            entries_.append(item);
            if (expanded)
                append(append, childPath, depth + 1);
        }
    };
    appendVisible(appendVisible, directory_, 0);
    emit browserChanged();
}

void FfiClientBridge::activateEntry(int index) {
    if (index < 0 || index >= entries_.size())
        return;
    const auto item = entries_.at(index).toMap();
    if (item.value("type").toInt() == 1)
        setEntryExpanded(index, !item.value("expanded").toBool());
    else if (item.value("type").toInt() == 2)
        setMessage(QStringLiteral("符号链接未区分目标类型，请选择实际文件或目录"));
    else
        readFile(item.value("path").toString());
}

void FfiClientBridge::setEntryExpanded(int index, bool expanded) {
    const auto item = entries_.value(index).toMap();
    if (item.value("type").toInt() != 1)
        return;
    const auto path = item.value("path").toString();
    if (directory_.isEmpty()) {
        if (expanded)
            listDir(path);
        return;
    }

    if (browserDirectories_.value(path).expanded == expanded)
        return;
    if (expanded) {
        browserDirectories_[path].expanded = true;
        fetchDirectory(path);
    } else {
        // 单击已展开目录、回车和左方向键共用此入口，立即折叠并清空整条分支。
        discardDirectory(path);
        rebuildBrowserEntries();
    }
}

QVariantList FfiClientBridge::documents() const {
    QVariantList result;
    for (auto *document : documents_)
        result.append(QVariant::fromValue(document));
    return result;
}

WorkspaceDocument *FfiClientBridge::currentDocument() const {
    return activeIndex_ >= 0 && activeIndex_ < documents_.size() ? documents_[activeIndex_] : nullptr;
}

void FfiClientBridge::selectDocument(int index) {
    if (index < 0 || index >= documents_.size())
        return;
    if (index != activeIndex_) {
        activeIndex_ = index;
        emit activeIndexChanged();
    }
    setActiveTab(documents_[index]);
}

void FfiClientBridge::readFile(const QString &path) {
    if (!validPath(path)) {
        setMessage(QStringLiteral("文件路径无效"));
        return;
    }
    for (int i = 0; i < documents_.size(); ++i) {
        if (documents_[i]->path() == path) {
            selectDocument(i);
            if (documents_[i]->stale_ && !documents_[i]->refreshing_)
                refreshDocument(documents_[i]);
            emit fileOpened();
            return;
        }
    }
    auto *document = new WorkspaceDocument(path, this);
    documents_.append(document);
    tabs_.append(document);
    emit documentsChanged();
    emit tabsChanged();
    selectDocument(documents_.size() - 1);
    refreshDocument(document);
    emit fileOpened();
}

void FfiClientBridge::refreshDocument(WorkspaceDocument *document) {
    if (document->preview_) {
        fetchGitDiff(document);
        return;
    }
    if (document->saving_)
        return;
    document->refreshing_ = true;
    document->stale_ = true;
    emit document->stateChanged();
    auto transfer = std::make_shared<ReadTransfer>();
    transfer->generation = ++document->loadGeneration_;
    readChunk(document, transfer);
}

void FfiClientBridge::readChunk(WorkspaceDocument *document, std::shared_ptr<ReadTransfer> transfer) {
    auto payload = pathPayload(document->path_);
    appendInteger(payload, quint64(transfer->bytes.size()));
    appendInteger(payload, ReadChunk);
    request(ReadFile, payload, [this, document, transfer](quint16 status, const QByteArray &payload) {
        if (transfer->generation != document->loadGeneration_)
            return;
        auto fail = [this, document](const QString &reason) {
            document->refreshing_ = false;
            document->stale_ = true;
            emit document->stateChanged();
            setMessage(QStringLiteral("读取失败，本地内容已保留：") + reason);
        };
        if (status) {
            fail(QString::fromUtf8(payload));
            if (status == 4 && browserRequested_)
                openFileBrowser();
            return;
        }
        if (payload.size() < 16) {
            fail(QStringLiteral("响应格式无效"));
            return;
        }
        const auto *data = reinterpret_cast<const uchar *>(payload.constData());
        const auto size = qFromBigEndian<quint64>(data);
        const auto revision = qFromBigEndian<quint64>(data + 8);
        if (size > MaxEditableRead) {
            fail(QStringLiteral("文件超过平板编辑器 8 MiB 读取上限"));
            return;
        }
        if (!transfer->bytes.isEmpty() && (revision != transfer->revision || size != transfer->size)) {
            if (++transfer->restarts > 2) {
                fail(QStringLiteral("文件持续变化，请稍后重新获取"));
                return;
            }
            // 分段期间版本变化，从零读取，绝不拼接两个版本的内容。
            transfer->bytes.clear();
            readChunk(document, transfer);
            return;
        }
        transfer->size = size;
        transfer->revision = revision;
        const auto chunk = payload.mid(16);
        if (chunk.size() > ReadChunk || quint64(transfer->bytes.size() + chunk.size()) > size
            || (chunk.isEmpty() && quint64(transfer->bytes.size()) < size)) {
            fail(QStringLiteral("文件分段长度无效"));
            return;
        }
        transfer->bytes.append(chunk);
        if (quint64(transfer->bytes.size()) < size) {
            readChunk(document, transfer);
            return;
        }
        if (!document->applyRemote(transfer->bytes, revision)) {
            fail(QStringLiteral("文件不是有效的 UTF-8 文本"));
            return;
        }
        setMessage(document->conflict_ ? QStringLiteral("服务端文件已变化；本地修改已保留，请比较后选择保存版本")
                   : document->readOnly_ ? QStringLiteral("文件超过服务端写入上限，仅供阅读")
                   : document->dirty() ? QStringLiteral("已重新获取，本地修改仍待保存：") + document->path_
                   : QStringLiteral("已获取：") + document->path_);
    });
}

void FfiClientBridge::saveCurrent() {
    writeFile(currentDocument());
}

void FfiClientBridge::writeFile(WorkspaceDocument *document) {
    if (!document || !documents_.contains(document) || document->preview_
        || document->saving_ || document->refreshing_)
        return;
    if (document->stale_) {
        refreshDocument(document);
        return;
    }
    if (document->conflict_) {
        setMessage(QStringLiteral("请先比较服务端版本，再选择保留本地并保存或采用服务端版本"));
        return;
    }
    if (!document->dirty() || document->readOnly())
        return;
    const QString snapshot = document->text_;
    QByteArray payload = pathPayload(document->path_);
    appendInteger(payload, document->revision_);
    payload.append(document->encodedText());
    if (payload.size() > MaxFrame - 13) {
        setMessage(QStringLiteral("保存失败：文本超过服务端单次写入上限，请缩小文件"));
        return;
    }
    document->saving_ = true;
    emit document->stateChanged();
    request(WriteFile, payload, [this, document, snapshot](quint16 status, const QByteArray &payload) {
        document->saving_ = false;
        if (!status && payload.size() == 8) {
            document->revision_ = qFromBigEndian<quint64>(
                reinterpret_cast<const uchar *>(payload.constData()));
            // 只确认本次发送的快照；等待保存响应期间的新输入仍标为未保存。
            document->savedText_ = snapshot;
            document->stale_ = false;
            emit document->stateChanged();
            setMessage(QStringLiteral("已保存：") + document->path_);
        } else {
            setMessage(status == 6 ? QStringLiteral("保存版本冲突，正在重新获取服务端文件…")
                                   : QStringLiteral("保存未确认，正在重新获取；本地修改已保留"));
            refreshDocument(document);
        }
    });
}

void FfiClientBridge::refreshCurrent() {
    if (auto *document = currentDocument())
        refreshDocument(document);
}

void FfiClientBridge::useServerVersion() {
    if (auto *document = currentDocument(); document && document->conflict_
        && !document->stale_ && !document->refreshing_ && !document->saving_) {
        document->conflict_ = false;
        document->setText(document->savedText_);
        emit document->stateChanged();
        setMessage(QStringLiteral("已采用服务端版本"));
    }
}

void FfiClientBridge::saveLocalVersion() {
    if (auto *document = currentDocument(); document && document->conflict_
        && !document->stale_ && !document->refreshing_ && !document->saving_) {
        document->conflict_ = false;
        writeFile(document); // 显式选择本地版本后，仍用最新 revision 做条件写入。
        emit document->stateChanged();
    }
}

QVariantList FfiClientBridge::tabs() const {
    QVariantList result;
    for (auto *tab : tabs_)
        result.append(QVariant::fromValue(tab));
    return result;
}

void FfiClientBridge::setActiveTab(QObject *tab) {
    if (activeTab_ == tab)
        return;
    activeTab_ = tab;
    emit activeTabChanged();
}

void FfiClientBridge::selectTab(int index) {
    auto *tab = tabs_.value(index, nullptr);
    if (auto *document = qobject_cast<WorkspaceDocument *>(tab))
        selectDocument(documents_.indexOf(document));
    else if (auto *terminal = qobject_cast<TerminalSession *>(tab))
        selectTerminal(terminals_.indexOf(terminal));
}

} // namespace tina::ui
