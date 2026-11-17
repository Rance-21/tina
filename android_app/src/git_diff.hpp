#pragma once

#include <QString>
#include <QVector>

namespace tina::ui {

struct DiffLine {
    int oldNumber{}, newNumber{}, change{}; // -1 删除，1 新增，0 未变化。
};

struct InlineDiff {
    QString text;
    QVector<DiffLine> lines;
    bool valid{};
};

// 用工作区完整文本补齐 patch 未包含的上下文，并核对每个 hunk 的新版本。
InlineDiff buildInlineDiff(const QString &patch, const QString &worktree, bool untracked = false);

} // namespace tina::ui
