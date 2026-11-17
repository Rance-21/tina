#include "git_diff.hpp"

#include <QRegularExpression>

namespace tina::ui {

InlineDiff buildInlineDiff(const QString &patch, const QString &worktree, bool untracked) {
    auto current = worktree.split('\n');
    if (worktree.isEmpty()) current.clear();
    else if (worktree.endsWith('\n')) current.removeLast();
    InlineDiff result;
    QStringList output;
    int oldLine = 1, newLine = 1, position = 0;
    const auto append = [&](const QString &text, int change) {
        output.append(text);
        result.lines.append({change == 1 ? 0 : oldLine++, change == -1 ? 0 : newLine++, change});
    };
    if (untracked) {
        for (const auto &line : current) append(line, 1);
    } else {
        static const QRegularExpression header(QStringLiteral(
            R"(^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@)"));
        const auto patchLines = patch.split('\n');
        int oldRemaining = 0, newRemaining = 0;
        bool inHunk = false;
        for (const auto &line : patchLines) {
            const auto match = header.match(line);
            if (match.hasMatch()) {
                if (oldRemaining || newRemaining) return {};
                oldRemaining = match.captured(2).isEmpty() ? 1 : match.captured(2).toInt();
                newRemaining = match.captured(4).isEmpty() ? 1 : match.captured(4).toInt();
                const int start = match.captured(3).toInt() - (newRemaining ? 1 : 0);
                if (start < position || start > current.size()) return {};
                while (position < start) append(current[position++], 0);
                if (oldLine != match.captured(1).toInt() + (oldRemaining ? 0 : 1)) return {};
                inHunk = true;
                continue;
            }
            if (!inHunk || (!oldRemaining && !newRemaining) || line.startsWith('\\')) continue;
            if (line.isEmpty()) return {};
            const auto kind = line[0];
            const auto content = line.mid(1);
            if (kind == ' ' || kind == '+') {
                if (newRemaining <= 0 || position >= current.size() || current[position++] != content) return {};
                --newRemaining;
            }
            if (kind == ' ' || kind == '-') {
                if (oldRemaining <= 0) return {};
                --oldRemaining;
            }
            if (kind != ' ' && kind != '+' && kind != '-') return {};
            append(content, kind == '+' ? 1 : kind == '-' ? -1 : 0);
        }
        if (oldRemaining || newRemaining) return {};
        while (position < current.size()) append(current[position++], 0);
    }
    result.text = output.join('\n');
    result.valid = true;
    return result;
}

} // namespace tina::ui
