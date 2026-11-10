#include "ffi_client_bridge.hpp"

#include <QPointer>
#include <QRegularExpression>
#include <QtEndian>
#include <algorithm>
#include <utility>

namespace tina::ui {
namespace {
constexpr quint16 TerminalInput = 0x0021;
constexpr quint16 TerminalTab = 0x0022;
constexpr quint16 TerminalOutput = 0x0281;
constexpr quint16 TerminalState = 0x0282;
constexpr qsizetype MaxInputBytes = 1024 * 1024 - 21; // 帧头 13 字节 + 终端负载头 8 字节。

template<typename T> void appendInteger(QByteArray &bytes, T value) {
    const auto offset = bytes.size();
    bytes.resize(offset + sizeof(T));
    qToBigEndian(value, reinterpret_cast<uchar *>(bytes.data() + offset));
}

QByteArray terminalStatePayload(quint32 id) {
    QByteArray payload;
    appendInteger(payload, quint32(4));
    appendInteger(payload, id);
    return payload;
}

QByteArray terminalPayload(quint32 id, const QByteArray &text) {
    QByteArray payload;
    appendInteger(payload, quint32(text.size()));
    appendInteger(payload, id);
    payload.append(text);
    return payload;
}

bool decodeTerminalPayload(const QByteArray &payload, quint32 &id) {
    if (payload.size() < 8
        || qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(payload.constData()))
               != payload.size() - 8)
        return false;
    id = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(payload.constData() + 4));
    return id != 0;
}

bool decodeTerminalState(const QByteArray &payload, quint32 &id) {
    return payload.size() == 8
           && qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(payload.constData())) == 4
           && (id = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(payload.constData() + 4)));
}

QString terminalErrorText(const QByteArray &payload) {
    if (payload.size() >= 4) {
        const auto size = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(payload.constData()));
        if (size == payload.size() - 4)
            return QString::fromUtf8(payload.mid(4));
        if (payload.size() >= 8 && size == payload.size() - 8)
            return QString::fromUtf8(payload.mid(8));
    }
    return QString::fromUtf8(payload);
}

} // namespace

QVariantList FfiClientBridge::terminals() const {
    QVariantList result;
    for (auto *terminal : terminals_)
        result.append(QVariant::fromValue(terminal));
    return result;
}

TerminalSession *FfiClientBridge::activeTerminal() const {
    return terminals_.value(activeTerminalIndex_, nullptr);
}

TerminalSession *FfiClientBridge::findTerminal(quint32 id) const {
    for (auto *terminal : terminals_)
        if (terminal->available_ && terminal->terminalId_ == id)
            return terminal;
    return nullptr;
}

bool FfiClientBridge::handleTerminalCommand(const QString &input) {
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    const auto parts = input.trimmed().split(whitespace, Qt::SkipEmptyParts);
    const auto usage = [this] {
        setMessage(QStringLiteral("用法：@terminal add [id] 或 @terminal del id"));
        return false;
    };
    if (parts.isEmpty() || parts[0].compare(QStringLiteral("@terminal"), Qt::CaseInsensitive))
        return usage();
    if (parts.size() == 1) {
        if (!activeTerminal())
            return createTerminal();
        selectTerminal(activeTerminalIndex_);
        return true;
    }
    if (parts.size() > 3)
        return usage();
    const bool add = !parts[1].compare(QStringLiteral("add"), Qt::CaseInsensitive);
    const bool del = !parts[1].compare(QStringLiteral("del"), Qt::CaseInsensitive);
    if (!add && !del)
        return usage();
    int id{};
    if (parts.size() == 3) {
        bool ok{};
        id = parts[2].toInt(&ok);
        if (!ok || id <= 0)
            return usage();
    } else if (del) {
        return usage();
    }
    if (add)
        return createTerminal(id);
    for (int i = 0; i < terminals_.size(); ++i)
        if (terminals_[i]->sessionId() == id)
            return closeTerminal(i);
    setMessage(QStringLiteral("Terminal %1 不存在").arg(id));
    return false;
}

void FfiClientBridge::selectTerminal(int index) {
    if (index < 0 || index >= terminals_.size())
        return;
    if (index != activeTerminalIndex_) {
        activeTerminalIndex_ = index;
        emit activeTerminalChanged();
    }
    setActiveTab(terminals_[index]);
}

bool FfiClientBridge::createTerminal(int id) {
    if (!connected()) {
        setMessage(QStringLiteral("连接服务端后可新建终端"));
        return false;
    }
    if (terminals_.size() >= 16) {
        setMessage(QStringLiteral("最多打开 16 个终端，请先关闭不用的终端"));
        return false;
    }
    const auto exists = [this](int candidate) {
        return std::any_of(terminals_.cbegin(), terminals_.cend(), [candidate](auto *t) {
            return t->sessionId() == candidate;
        });
    };
    if (id < 0 || (id > 0 && exists(id))) {
        setMessage(id < 0 ? QStringLiteral("终端编号必须为正整数")
                          : QStringLiteral("Terminal %1 已存在").arg(id));
        return false;
    }
    if (id == 0)
        for (id = 1; exists(id); ++id) {}
    auto *terminal = new TerminalSession(id, this);
    terminals_.append(terminal);
    tabs_.append(terminal);
    emit terminalsChanged();
    emit tabsChanged();
    selectTerminal(terminals_.size() - 1);
    startTerminal(terminal);
    return true;
}

void FfiClientBridge::terminalError(TerminalSession *terminal, const QString &message) {
    setMessage(message);
    terminal->appendOutput((QStringLiteral("\r\n[") + message + QStringLiteral("]\r\n")).toUtf8());
}

void FfiClientBridge::startTerminal(TerminalSession *terminal) {
    terminal->starting_ = true;
    terminal->terminalId_ = 0;
    terminal->decoder_.resetState();
    terminal->escape_ = TerminalSession::Escape::None;
    emit terminal->stateChanged();
    request(TerminalState, terminalStatePayload(0),
            [this, terminal = QPointer(terminal)](quint16 status, const QByteArray &payload) {
        if (!terminal)
            return;
        terminal->starting_ = false;
        quint32 id{};
        if (status || !decodeTerminalState(payload, id) || findTerminal(id)) {
            terminalError(terminal, QStringLiteral("启动终端失败：") + terminalErrorText(payload));
            // 超时后服务端是否已创建 Shell 无法确定，断开以回收该连接下的会话。
            if (status == 12)
                socket_.abort();
        } else {
            terminal->terminalId_ = id;
            terminal->available_ = true;
            terminal->appendOutput(earlyTerminalOutput_.take(id));
        }
        if (std::none_of(terminals_.cbegin(), terminals_.cend(), [](auto *t) { return t->starting_; })) {
            earlyTerminalOutput_.clear();
        }
        emit terminal->stateChanged();
    });
}

bool FfiClientBridge::closeTerminal(int index) {
    auto *terminal = terminals_.value(index, nullptr);
    if (!terminal)
        return false;
    if (terminal->starting_) {
        setMessage(QStringLiteral("终端启动中，请稍后删除"));
        return false;
    }
    const auto id = terminal->terminalId_;
    const bool wasAvailable = terminal->available_;
    const auto tabIndex = tabs_.indexOf(terminal);
    const bool wasActive = activeTab_ == terminal;
    terminals_.removeAt(index);
    tabs_.removeAt(tabIndex);
    if (activeTerminalIndex_ > index)
        --activeTerminalIndex_;
    else if (activeTerminalIndex_ == index)
        activeTerminalIndex_ = std::min(index, int(terminals_.size()) - 1);
    terminal->deleteLater();
    emit terminalsChanged();
    emit tabsChanged();
    emit activeTerminalChanged();
    if (wasActive) {
        if (tabs_.isEmpty())
            setActiveTab(nullptr);
        else
            selectTab(std::min(int(tabIndex), int(tabs_.size()) - 1));
    } else {
        emit activeTabChanged(); // 删除前面的标签后，当前索引也会变化。
    }
    if (wasAvailable && connected()) {
        request(TerminalState, terminalStatePayload(id), [this](quint16 status, const QByteArray &payload) {
            if (status) {
                setMessage(QStringLiteral("关闭终端失败：") + terminalErrorText(payload));
                if (status != 4) socket_.abort(); // 连接关闭后服务端回收可能遗留的 Shell。
            }
        });
    }
    return true;
}

void FfiClientBridge::invalidateTerminals() {
    earlyTerminalOutput_.clear();
    for (auto *terminal : std::as_const(terminals_)) {
        terminal->available_ = terminal->starting_ = terminal->busy_ = false;
        terminal->completing_ = terminal->submitting_ = false;
        terminalError(terminal, QStringLiteral("连接断开；恢复后将启动新 Shell，历史输出保留"));
        emit terminal->stateChanged();
    }
}

void FfiClientBridge::handleTerminalEvent(quint16 method, const QByteArray &payload) {
    quint32 id{};
    if (method != TerminalOutput && method != TerminalState)
        return;
    if (!(method == TerminalOutput ? decodeTerminalPayload(payload, id) : decodeTerminalState(payload, id)))
        return;
    if (auto *terminal = findTerminal(id)) {
        if (method == TerminalOutput)
            terminal->appendOutput(QByteArrayView(payload).sliced(8));
        else {
            terminal->busy_ = false;
            emit terminal->stateChanged();
        }
        return;
    }
    const auto starting = std::count_if(terminals_.cbegin(), terminals_.cend(), [](auto *t) { return t->starting_; });
    if (!starting || (!earlyTerminalOutput_.contains(id) && earlyTerminalOutput_.size() >= starting))
        return;
    auto &bytes = earlyTerminalOutput_[id];
    if (method == TerminalOutput) {
        bytes.append(payload.constData() + 8, payload.size() - 8);
        constexpr qsizetype limit = TerminalSession::BufferCapacity;
        if (bytes.size() > limit) bytes.remove(0, bytes.size() - limit);
    }
}

bool FfiClientBridge::submitCommand(const QString &command) {
    auto *terminal = activeTerminal();
    if (!terminal || !terminal->available_ || terminal->starting_ || terminal->completing_
        || terminal->submitting_ || !connected())
        return false;
    const auto bytes = command.toUtf8();
    if (bytes.size() > MaxInputBytes) {
        terminalError(terminal, QStringLiteral("命令超过终端请求上限"));
        return false;
    }
    terminal->busy_ = terminal->submitting_ = true;
    emit terminal->stateChanged();
    request(TerminalInput, terminalPayload(terminal->terminalId_, bytes),
            [this, terminal = QPointer(terminal)](quint16 status, const QByteArray &payload) {
        if (!terminal || !terminals_.contains(terminal)) return;
        terminal->submitting_ = false;
        if (status) {
            terminal->busy_ = false;
            if (status == 4) terminal->available_ = false;
            terminalError(terminal, QStringLiteral("发送失败（未自动重发）：") + terminalErrorText(payload));
        }
        emit terminal->stateChanged();
    });
    emit commandSubmitted(command);
    return true;
}

void FfiClientBridge::completeTerminalInput() {
    auto *terminal = activeTerminal();
    if (!terminal || !terminal->available_ || terminal->busy_ || terminal->completing_)
        return;
    const auto revision = terminal->draftRevision_;
    terminal->completing_ = true;
    emit terminal->stateChanged();
    request(TerminalTab, terminalPayload(terminal->terminalId_, terminal->draft_.toUtf8()),
            [this, terminal = QPointer(terminal), revision](quint16 status, const QByteArray &payload) {
        if (!terminal || !terminals_.contains(terminal)) return;
        terminal->completing_ = false;
        quint32 id{};
        if (status || !decodeTerminalPayload(payload, id) || id != terminal->terminalId_)
            terminalError(terminal, QStringLiteral("补全失败：") + terminalErrorText(payload));
        else if (revision == terminal->draftRevision_)
            terminal->setDraft(QString::fromUtf8(payload.mid(8)));
        emit terminal->stateChanged();
    });
}

void FfiClientBridge::interruptTerminal() {
    submitCommand(QString(QChar(3)));
}

} // namespace tina::ui
