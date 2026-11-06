#include "workspace_document.hpp"
#include "code_highlighter.hpp"

#include <QStringConverter>
#include <QTextDocument>

namespace tina::ui {

WorkspaceDocument::WorkspaceDocument(QString path, QObject *parent)
    : QObject(parent), path_(std::move(path)) {}

void WorkspaceDocument::setText(const QString &text) {
    if (text_ == text)
        return;
    text_ = text;
    emit textChanged();
    emit stateChanged();
}

void WorkspaceDocument::attach(QQuickTextDocument *document) {
    if (!document || document == quickDocument_)
        return;
    delete highlighter_;
    quickDocument_ = document;
    highlighter_ = new CodeHighlighter(document->textDocument(), path_);
    // 由本文档管理生命周期，编辑器提前销毁时高亮器会自动解除文档关联。
    highlighter_->setParent(this);
}

bool WorkspaceDocument::applyRemote(const QByteArray &bytes, quint64 revision) {
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    QString decoded = decoder(bytes);
    if (decoder.hasError() || decoded.contains(QChar::Null))
        return false;
    bom_ = bytes.startsWith("\xef\xbb\xbf");
    crlf_ = decoded.contains(QStringLiteral("\r\n"));
    decoded.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    decoded.replace(QChar('\r'), QChar('\n'));
    // 写入失败或断线后先重新读取，编辑中的本地文本不被后台刷新覆盖。
    const bool keepLocal = dirty();
    conflict_ = keepLocal && decoded != text_ && (conflict_ || decoded != savedText_);
    savedText_ = decoded;
    revision_ = revision;
    loaded_ = true;
    stale_ = false;
    refreshing_ = false;
    readOnly_ = bytes.size() > 1024 * 1024 - 23 - path_.toUtf8().size();
    if (!keepLocal)
        setText(decoded);
    emit stateChanged();
    return true;
}

QByteArray WorkspaceDocument::encodedText() const {
    QString content = text_;
    if (crlf_)
        content.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    QByteArray bytes = content.toUtf8();
    if (bom_)
        bytes.prepend("\xef\xbb\xbf");
    return bytes;
}

} // namespace tina::ui
