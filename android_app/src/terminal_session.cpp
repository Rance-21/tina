#include "terminal_session.hpp"

#include <algorithm>

namespace tina::ui {

TerminalSession::TerminalSession(int sessionId, QObject *parent)
    : QObject(parent), sessionId_(sessionId) {
    // 高频输出合并通知，避免每个 TCP 分片都重新排版 QML 文本。
    outputTimer_.setSingleShot(true);
    outputTimer_.setInterval(33);
    connect(&outputTimer_, &QTimer::timeout, this, &TerminalSession::outputChanged);
}

void TerminalSession::setDraft(const QString &draft) {
    if (draft_ == draft)
        return;
    draft_ = draft;
    ++draftRevision_;
    emit draftChanged();
}

QString TerminalSession::output() const {
    auto first = first_;
    // 环绕淘汰可能恰好切在 UTF-16 代理对中间。
    if (first < end_ && buffer_[first % BufferCapacity].isLowSurrogate())
        ++first;
    const auto length = qsizetype(end_ - first);
    const auto offset = qsizetype(first % BufferCapacity);
    const auto firstChunk = std::min(length, BufferCapacity - offset);
    QString result;
    result.reserve(length);
    result.append(buffer_.data() + offset, firstChunk);
    result.append(buffer_.data(), length - firstChunk);
    return result;
}

void TerminalSession::scheduleOutput() {
    if (!outputTimer_.isActive())
        outputTimer_.start();
}

void TerminalSession::clear() {
    first_ = end_ = lineStart_ = cursor_ = 0;
    lines_ = 0;
    scheduleOutput();
}

void TerminalSession::dropFirst() {
    if (buffer_[first_ % BufferCapacity] == '\n')
        --lines_;
    ++first_;
    lineStart_ = std::max(lineStart_, first_);
    cursor_ = std::max(cursor_, first_);
}

void TerminalSession::put(QChar ch) {
    if (ch == '\n') {
        cursor_ = end_;
        ++lines_;
    }
    if (cursor_ == end_) {
        if (end_ - first_ == BufferCapacity)
            dropFirst();
        ++end_;
    }
    buffer_[cursor_++ % BufferCapacity] = ch;
    if (ch == '\n')
        lineStart_ = cursor_;
    while (lines_ > MaxLines)
        dropFirst();
}

void TerminalSession::finishCsi(QChar final) {
    const auto parameter = parameters_.section(';', 0, 0);
    const int n = parameter.toInt();
    switch (final.unicode()) {
    case 'K':
        if (n == 0) end_ = cursor_;
        else if (n == 2) end_ = cursor_ = lineStart_;
        break;
    case 'J':
        if (n == 2 || n == 3) clear();
        break;
    case 'D': cursor_ -= std::min<quint64>(cursor_ - lineStart_, std::max(1, n)); break;
    case 'C': cursor_ += std::min<quint64>(end_ - cursor_, std::max(1, n)); break;
    case 'G': cursor_ = lineStart_ + std::min<quint64>(end_ - lineStart_, std::max(1, n) - 1); break;
    default: break; // 颜色、光标可见性等不写入纯文本输出。
    }
}

void TerminalSession::appendOutput(QByteArrayView bytes) {
    if (bytes.isEmpty())
        return;
    // 解码器和 ANSI 状态都按会话保存，中文、emoji、控制序列可跨任意 TCP 分片。
    const QString text = decoder_(bytes);
    for (const auto ch : text) {
        if (escape_ == Escape::Osc || escape_ == Escape::OscEnd) {
            if (ch == '\a' || (escape_ == Escape::OscEnd && ch == '\\'))
                escape_ = Escape::None;
            else
                escape_ = ch.unicode() == 0x1b ? Escape::OscEnd : Escape::Osc;
            continue;
        }
        if (escape_ == Escape::Csi) {
            if (ch.unicode() >= 0x40 && ch.unicode() <= 0x7e) {
                finishCsi(ch);
                escape_ = Escape::None;
            } else if (parameters_.size() < 64) {
                parameters_.append(ch);
            }
            continue;
        }
        if (escape_ == Escape::Charset) {
            escape_ = Escape::None;
            continue;
        }
        if (escape_ == Escape::Start) {
            escape_ = ch == '[' ? Escape::Csi : ch == ']' ? Escape::Osc
                      : (ch == '(' || ch == ')') ? Escape::Charset : Escape::None;
            parameters_.clear();
            continue;
        }
        switch (ch.unicode()) {
        case 0x1b: escape_ = Escape::Start; break;
        case '\r': cursor_ = lineStart_; break;
        case '\b':
            if (cursor_ > lineStart_) {
                --cursor_;
                if (buffer_[cursor_ % BufferCapacity].isLowSurrogate() && cursor_ > lineStart_)
                    --cursor_;
            }
            break;
        default:
            if (ch == '\n' || ch == '\t' || (ch.unicode() >= 0x20 && ch.unicode() != 0x7f))
                put(ch);
            break;
        }
    }
    scheduleOutput();
}

} // namespace tina::ui
