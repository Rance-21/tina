#pragma once

#include <QRegularExpression>
#include <QSyntaxHighlighter>

namespace tina::ui {

// Qt 按受影响的文本块增量高亮，无需每次输入重新解析整份文件。
class CodeHighlighter : public QSyntaxHighlighter {
  public:
    CodeHighlighter(QTextDocument *document, const QString &path);

  protected:
    void highlightBlock(const QString &text) override;

  private:
    bool enabled_{};
    bool hashComments_{};
    bool cComments_{};
    bool diff_{};
    QRegularExpression tokens_;
};

// 在 Qt 已解析的 Markdown 文档上配色，保留链接、表格、字号和原始源码。
class MarkdownHighlighter : public QSyntaxHighlighter {
  public:
    explicit MarkdownHighlighter(QTextDocument *document) : QSyntaxHighlighter(document) {}

  protected:
    void highlightBlock(const QString &text) override;
};

} // namespace tina::ui
