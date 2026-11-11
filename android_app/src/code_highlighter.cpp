#include "code_highlighter.hpp"

#include <QFileInfo>
#include <QSet>

namespace tina::ui {

CodeHighlighter::CodeHighlighter(QTextDocument *document, const QString &path)
    : QSyntaxHighlighter(document) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    diff_ = suffix == "diff" || suffix == "patch";
    const QSet<QString> cLanguages{"c", "h", "cc", "hh", "cpp", "hpp", "cxx", "cppm",
                                  "ixx", "rs", "qml", "js", "ts", "jsx", "tsx", "java",
                                  "kt", "kts", "css", "json", "jsonc"};
    hashComments_ = QSet<QString>{"py", "sh", "bash", "cmake", "toml", "yaml", "yml"}.contains(suffix)
                    || QFileInfo(path).fileName() == QStringLiteral("CMakeLists.txt");
    cComments_ = cLanguages.contains(suffix) && suffix != QStringLiteral("json");
    enabled_ = cLanguages.contains(suffix) || hashComments_;
    // 先消费字符串和注释，避免把字符串中的 // 误判为注释。
    tokens_.setPattern(QStringLiteral(
        R"re(("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')|(/\*|//|#)|(\b(?:0[xX][0-9a-fA-F]+|\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\b)|(\b[A-Za-z_][A-Za-z_0-9]*\b))re"));
    tokens_.optimize();
}

void CodeHighlighter::highlightBlock(const QString &text) {
    setCurrentBlockState(0);
    if (diff_) {
        // Unified diff 保留原文，仅按整行着色，方便辨认新增、删除和块标题。
        if (text.startsWith("+++ ") || text.startsWith("--- ") || text.startsWith("diff --git "))
            setFormat(0, text.size(), QColor("#91bfff"));
        else if (text.startsWith('+'))
            setFormat(0, text.size(), QColor("#8cdaa4"));
        else if (text.startsWith('-'))
            setFormat(0, text.size(), QColor("#f19494"));
        else if (text.startsWith("@@"))
            setFormat(0, text.size(), QColor("#cfabff"));
        return;
    }
    if (!enabled_)
        return;
    // 压缩文件的超长单行跳过着色，避免平板主线程卡顿。
    if (text.size() > 16384) {
        setCurrentBlockState(previousBlockState());
        return;
    }
    static const QSet<QString> keywords = [] {
        const auto words = QStringLiteral(
            "alignas auto bool break case catch char class concept const constexpr consteval constinit "
            "continue co_await co_return co_yield default delete do double else enum explicit export extern "
            "false float for friend if import inline int long module mutable namespace new noexcept nullptr "
            "operator private protected public requires return short signed sizeof static static_assert "
            "struct switch template this thread_local throw true try typedef typename union unsigned using "
            "virtual void volatile while let mut fn impl pub use mod crate trait async await move match "
            "self Self dyn unsafe where type as in def elif except finally from lambda None True False "
            "pass raise with yield and or not is function var null undefined interface extends instanceof "
            "property signal readonly required on Component id package val fun when object override").split(' ');
        return QSet<QString>(words.begin(), words.end());
    }();
    const QColor comment(QStringLiteral("#7e9b7a"));
    qsizetype position = 0;
    if (previousBlockState() == 1) {
        const auto end = text.indexOf(QStringLiteral("*/"));
        if (end < 0) {
            setFormat(0, text.size(), comment);
            setCurrentBlockState(1);
            return;
        }
        setFormat(0, end + 2, comment);
        position = end + 2;
    }
    while (position < text.size()) {
        const auto match = tokens_.match(text, position);
        if (!match.hasMatch())
            break;
        const auto start = match.capturedStart();
        position = match.capturedEnd();
        if (!match.captured(1).isEmpty()) {
            setFormat(start, match.capturedLength(), QColor(QStringLiteral("#ceaa83")));
        } else if (!match.captured(2).isEmpty()) {
            const auto token = match.captured(2);
            if ((token == "#" && hashComments_) || (token == "//" && cComments_)) {
                setFormat(start, text.size() - start, comment);
                break;
            }
            if (token == "/*" && cComments_) {
                const auto end = text.indexOf(QStringLiteral("*/"), position);
                position = end < 0 ? text.size() : end + 2;
                setFormat(start, position - start, comment);
                if (end < 0)
                    setCurrentBlockState(1);
            }
        } else if (!match.captured(3).isEmpty()) {
            setFormat(start, match.capturedLength(), QColor(QStringLiteral("#b5cea8")));
        } else if (keywords.contains(match.captured(4))) {
            setFormat(start, match.capturedLength(), QColor(QStringLiteral("#91bfff")));
        } else if (position < text.size() && text.at(position) == '(') {
            setFormat(start, match.capturedLength(), QColor(QStringLiteral("#e6d797")));
        }
    }
}

} // namespace tina::ui
