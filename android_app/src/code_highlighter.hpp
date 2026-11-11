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

} // namespace tina::ui
