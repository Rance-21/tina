#include "workspace_document.hpp"
#include "code_highlighter.hpp"

#include <QStringConverter>
#include <QTextDocument>
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextLayout>
#include <algorithm>

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
    if (quickDocument_) {
        disconnect(quickDocument_->textDocument(), nullptr, this, nullptr);
        disconnect(quickDocument_->textDocument()->documentLayout(), nullptr, this, nullptr);
    }
    delete highlighter_;
    quickDocument_ = document;
    highlighter_ = new CodeHighlighter(document->textDocument(), path_);
    // 由本文档管理生命周期，编辑器提前销毁时高亮器会自动解除文档关联。
    highlighter_->setParent(this);
    const auto changed = [this] { ++layoutRevision_; emit layoutChanged(); };
    connect(document->textDocument(), &QTextDocument::contentsChanged, this, changed);
    connect(document->textDocument()->documentLayout(),
            &QAbstractTextDocumentLayout::documentSizeChanged, this, changed);
    changed();
}

QVariantList WorkspaceDocument::visibleLines(qreal top, qreal height) const {
    QVariantList lines;
    if (!quickDocument_ || height <= 0)
        return lines;
    auto *document = quickDocument_->textDocument();
    auto *layout = document->documentLayout();
    // 使用编辑器实际排版的行框与基线，避免 FontMetrics 的取整误差逐行累积。
    const int position = layout->hitTest(QPointF(document->documentMargin() + 1,
                                                std::max(qreal(0), top)), Qt::FuzzyHit);
    auto block = position >= 0 ? document->findBlock(position) : document->begin();
    for (; block.isValid(); block = block.next()) {
        const auto rect = layout->blockBoundingRect(block);
        if (rect.top() > top + height)
            break;
        if (rect.bottom() < top || !block.layout()->lineCount())
            continue;
        const auto line = block.layout()->lineAt(0);
        lines.append(QVariantMap{{"number", block.blockNumber() + 1},
                                 {"baseline", rect.y() + line.y() + line.ascent()}});
    }
    return lines;
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
