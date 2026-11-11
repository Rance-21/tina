#include "ffi_client_bridge.hpp"

#include <QDateTime>
#include <QRegularExpression>
#include <QStringConverter>
#include <QtEndian>
#include <algorithm>

namespace tina::ui {
namespace {
constexpr quint16 GitStatus = 0x0401, GitDiff = 0x0402, GitLog = 0x0403;

// 与 ide.protocol / client_git 完全相同的网络字节序；Android 不引入 Linux RPC 模块。
class GitReader {
  public:
    explicit GitReader(const QByteArray &bytes) : bytes_(bytes) {}
    template<typename T> bool integer(T &value) {
        if (bytes_.size() - offset_ < qsizetype(sizeof(T))) return false;
        value = qFromBigEndian<T>(reinterpret_cast<const uchar *>(bytes_.constData() + offset_));
        offset_ += sizeof(T);
        return true;
    }
    template<typename T> bool string(QString &value, bool empty = true) {
        T size{};
        if (!integer(size) || (!empty && !size) || size > bytes_.size() - offset_) return false;
        QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
        value = decoder(QByteArrayView(bytes_.constData() + offset_, size));
        offset_ += size;
        return !decoder.hasError() && !value.contains(QChar::Null);
    }
    bool finished() const { return offset_ == bytes_.size(); }
  private:
    const QByteArray &bytes_;
    qsizetype offset_{};
};

struct Commit {
    QString hash, refs, author, subject;
    QStringList parents;
    quint64 timestamp{};
    int lane{};
};

bool decodeGraph(const QByteArray &payload, QVariantList &nodes, QVariantList &edges, int &lanes) {
    GitReader reader(payload);
    QList<Commit> commits;
    QHash<QString, int> indices;
    while (!reader.finished()) {
        Commit commit;
        quint8 count{};
        if (commits.size() >= 100 || !reader.string<quint8>(commit.hash, false)
            || indices.contains(commit.hash) || !reader.integer(count)) return false;
        for (int i = 0; i < count; ++i) {
            QString parent;
            if (!reader.string<quint8>(parent, false)) return false;
            commit.parents.append(parent);
        }
        if (!reader.string<quint16>(commit.refs) || !reader.string<quint16>(commit.author)
            || !reader.integer(commit.timestamp) || !reader.string<quint16>(commit.subject)) return false;
        indices.insert(commit.hash, commits.size());
        commits.append(std::move(commit));
    }
    // 移植 client_git::build_git_graph 的活跃泳道算法，所有边由真实父哈希生成。
    QStringList active;
    const auto allocate = [&active](const QString &hash) {
        int lane = active.indexOf(QString());
        if (lane < 0) { lane = active.size(); active.append(hash); }
        else active[lane] = hash;
        return lane;
    };
    lanes = 0;
    for (int i = 0; i < commits.size(); ++i) {
        auto &commit = commits[i];
        int lane = -1;
        for (int l = 0; l < active.size(); ++l) {
            if (active[l] != commit.hash) continue;
            if (lane < 0) lane = l;
            else active[l].clear();
        }
        if (lane < 0) lane = allocate(commit.hash);
        commit.lane = lane;
        active[lane].clear();
        if (!commit.parents.isEmpty()) {
            if (!active.contains(commit.parents[0])) active[lane] = commit.parents[0];
            for (int p = 1; p < commit.parents.size(); ++p)
                if (!active.contains(commit.parents[p])) allocate(commit.parents[p]);
        }
        lanes = std::max(lanes, int(active.size()));
        bool head = false;
        for (const auto &ref : commit.refs.split(',')) {
            const auto value = ref.trimmed();
            head |= value == "HEAD" || value.startsWith("HEAD -> ");
        }
        nodes.append(QVariantMap{{"index", i}, {"lane", lane}, {"hash", commit.hash},
                                 {"refs", commit.refs}, {"author", commit.author},
                                 {"subject", commit.subject}, {"isHead", head},
                                 {"time", QDateTime::fromSecsSinceEpoch(qint64(commit.timestamp))
                                              .toString(QStringLiteral("yyyy-MM-dd HH:mm"))}});
    }
    for (int i = 0; i < commits.size(); ++i) {
        const auto &commit = commits[i];
        QVariantList parents, children;
        for (int p = 0; p < commit.parents.size(); ++p) {
            const auto found = indices.constFind(commit.parents[p]);
            if (found == indices.cend()) continue; // 第 100 条以外的父节点不画虚构连线。
            if (*found <= i) return false;
            parents.append(*found);
            edges.append(QVariantMap{{"fromIndex", i}, {"toIndex", *found},
                                     {"fromLane", commit.lane}, {"toLane", commits[*found].lane},
                                     {"isMerge", p > 0}});
        }
        for (const auto &edge : std::as_const(edges)) {
            const auto map = edge.toMap();
            if (map.value("toIndex").toInt() == i) children.append(map.value("fromIndex"));
        }
        auto node = nodes[i].toMap();
        node.insert("parentIndices", parents);
        node.insert("childIndices", children);
        nodes[i] = node;
    }
    return true;
}

QByteArray lengthString(const QString &text) {
    const auto bytes = text.toUtf8();
    QByteArray result(2, '\0');
    qToBigEndian(quint16(bytes.size()), reinterpret_cast<uchar *>(result.data()));
    return result + bytes;
}

// Git 会为制表符、换行和引号使用 C 风格转义；不能按空格拆分文件名。
QString unquotePath(QString path) {
    if (!path.startsWith('"')) return path;
    if (!path.endsWith('"')) return {};
    const auto bytes = path.mid(1, path.size() - 2).toUtf8();
    QByteArray result;
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        char c = bytes[i];
        if (c != '\\') { result.append(c); continue; }
        if (++i == bytes.size()) return {};
        c = bytes[i];
        const QByteArray escapes("abfnrtv\\\"");
        const QByteArray values("\a\b\f\n\r\t\v\\\"");
        const int escape = escapes.indexOf(c);
        if (escape >= 0) result.append(values[escape]);
        else if (c >= '0' && c <= '7') {
            int value = c - '0';
            for (int n = 1; n < 3 && i + 1 < bytes.size()
                                && bytes[i + 1] >= '0' && bytes[i + 1] <= '7'; ++n)
                value = value * 8 + bytes[++i] - '0';
            result.append(char(value));
        } else return {};
    }
    return QString::fromUtf8(result);
}

QString quotePath(const QString &path) {
    QString result;
    bool quoted = false;
    const QString escapes = QString::fromLatin1("\a\b\f\n\r\t\v\\\"");
    const QString values = QStringLiteral("abfnrtv\\\"");
    for (const auto c : path) {
        const int index = escapes.indexOf(c);
        if (index >= 0) { result += '\\'; result += values[index]; quoted = true; }
        else if (c.unicode() < 32 || c.unicode() == 127) {
            result += QStringLiteral("\\%1").arg(uint(c.unicode()), 3, 8, QChar('0')); quoted = true;
        } else result += c;
    }
    return quoted ? '"' + result + '"' : result;
}

QString filePatch(const QString &diff, const QString &file) {
    QString result, section;
    bool matches = false, metadata = true;
    const auto finish = [&] { if (matches) result += section; section.clear(); matches = false; };
    const auto lines = diff.split('\n');
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const auto &line = lines[i];
        if (line.startsWith("diff --git ")) {
            finish();
            metadata = true;
            matches = line == "diff --git " + quotePath("a/" + file) + ' ' + quotePath("b/" + file);
        }
        // 块内的被删文本也可能长得像 --- a/文件，不能误当成文件头。
        if (line.startsWith("@@") || line == "GIT binary patch") metadata = false;
        if (metadata && (line.startsWith("--- ") || line.startsWith("+++ "))) {
            auto path = unquotePath(line.mid(4).section('\t', 0, 0));
            matches |= path == "a/" + file || path == "b/" + file;
        } else if (metadata) {
            for (const auto &prefix : {QStringLiteral("rename from "), QStringLiteral("rename to "),
                                       QStringLiteral("copy from "), QStringLiteral("copy to ")})
                if (line.startsWith(prefix)) matches |= unquotePath(line.mid(prefix.size())) == file;
        }
        section += line;
        if (i + 1 < lines.size()) section += '\n';
    }
    finish();
    return result;
}
} // namespace

void FfiClientBridge::updateGitRepository() {
    // 文件浏览器选择的项目优先；尚未选择项目时取当前文件，否则用服务端默认仓库。
    QString repo = directory_.section('/', 0, 0);
    if (repo.isEmpty()) {
        if (auto *doc = currentDocument())
            repo = doc->preview_ ? doc->gitRepository_ : doc->path().section('/', 0, 0);
    }
    if (repo == gitRepository_) return;
    gitRepository_ = repo;
    ++gitLogGeneration_;
    ++gitStatusGeneration_;
    gitNodes_.clear(); gitEdges_.clear(); gitStatus_.clear(); gitLanes_ = 0;
    gitLogLoading_ = gitStatusLoading_ = false;
    gitLogError_.clear(); gitStatusError_.clear();
    emit gitChanged();
}

bool FfiClientBridge::handleGitCommand(const QString &input) {
    const auto parts = input.trimmed().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    const auto usage = [this] {
        setMessage(QStringLiteral("用法：@GIT tree / worktree / refresh / diff [分支1=main] [分支2=main]"));
        return false;
    };
    if (parts.isEmpty() || parts[0].compare("@GIT", Qt::CaseInsensitive)) return usage();
    const QString action = parts.size() == 1 ? QStringLiteral("tree") : parts[1].toLower();
    if (action == "diff") {
        if (parts.size() > 4) return usage();
        const QString base = parts.value(2, QStringLiteral("main"));
        const QString target = parts.value(3, QStringLiteral("main"));
        for (const auto &branch : {base, target}) {
            if (branch.toUtf8().size() > 65535 || branch.contains(QChar::Null)) return usage();
        }
        updateGitRepository();
        openGitDiff(base, target);
    } else {
        if (parts.size() > 2 || (action != "tree" && action != "worktree" && action != "refresh"))
            return usage();
        updateGitRepository();
        if (action != "refresh") gitView_ = action;
        if (action != "worktree") fetchGitLog();
        if (action != "tree") fetchGitStatus();
    }
    return true;
}

void FfiClientBridge::fetchGitLog() {
    const auto generation = ++gitLogGeneration_;
    gitLogLoading_ = true;
    gitLogError_.clear();
    emit gitChanged();
    // status/log 的 path 直接占满 payload，没有文件接口使用的 u16 长度头。
    request(GitLog, gitRepository_.toUtf8(), [this, generation](quint16 status, const QByteArray &payload) {
        if (generation != gitLogGeneration_) return; // 切换仓库或连续刷新后丢弃旧响应。
        QVariantList nodes, edges;
        int lanes{};
        gitLogLoading_ = false;
        if (status || !decodeGraph(payload, nodes, edges, lanes)) {
            gitLogError_ = status ? QString::fromUtf8(payload) : QStringLiteral("Git log 响应格式无效");
            setMessage(gitLogError_);
        } else {
            gitNodes_ = std::move(nodes); gitEdges_ = std::move(edges); gitLanes_ = lanes;
        }
        emit gitChanged();
    });
}

void FfiClientBridge::fetchGitStatus() {
    const auto generation = ++gitStatusGeneration_;
    gitStatusLoading_ = true;
    gitStatusError_.clear();
    emit gitChanged();
    request(GitStatus, gitRepository_.toUtf8(), [this, generation](quint16 status, const QByteArray &payload) {
        if (generation != gitStatusGeneration_) return;
        GitReader reader(payload);
        QVariantList entries;
        bool valid = !status;
        while (valid && !reader.finished()) {
            quint8 x{}, y{};
            QString path;
            valid = reader.integer(x) && reader.integer(y) && reader.string<quint16>(path, false);
            if (valid) entries.append(QVariantMap{{"path", path},
                {"status", QString(QChar(x)) + QChar(y)}});
        }
        gitStatusLoading_ = false;
        if (!valid) {
            gitStatusError_ = status ? QString::fromUtf8(payload) : QStringLiteral("Git status 响应格式无效");
            setMessage(gitStatusError_);
        } else gitStatus_ = std::move(entries);
        emit gitChanged();
    });
}

void FfiClientBridge::openGitFileDiff(int index) {
    if (index < 0 || index >= gitStatus_.size()) return;
    // 同名引用按当前服务端协议代表 worktree 对 HEAD；文件范围在客户端提取。
    openGitDiff(QStringLiteral("HEAD"), QStringLiteral("HEAD"), gitStatus_[index].toMap().value("path").toString());
}

void FfiClientBridge::openGitDiff(const QString &base, const QString &target, const QString &file) {
    WorkspaceDocument *document = nullptr;
    for (auto *candidate : documents_) {
        if (candidate->preview_ && candidate->gitRepository_ == gitRepository_
            && candidate->gitBase_ == base && candidate->gitTarget_ == target && candidate->gitFile_ == file) {
            document = candidate;
            break;
        }
    }
    if (!document) {
        document = new WorkspaceDocument(QStringLiteral("git-preview:%1.diff").arg(documents_.size()), this);
        document->preview_ = document->readOnly_ = true;
        document->gitRepository_ = gitRepository_;
        document->gitBase_ = base; document->gitTarget_ = target; document->gitFile_ = file;
        document->displayName_ = file.isEmpty() ? QStringLiteral("Diff · %1 → %2").arg(base, target)
                                               : QStringLiteral("Diff · %1").arg(file);
        documents_.append(document); tabs_.append(document);
        emit documentsChanged(); emit tabsChanged();
    }
    selectDocument(documents_.indexOf(document));
    fetchGitDiff(document);
    emit fileOpened();
}

void FfiClientBridge::fetchGitDiff(WorkspaceDocument *document) {
    const auto generation = ++document->loadGeneration_;
    document->refreshing_ = true;
    emit document->stateChanged();
    const auto payload = lengthString(document->gitRepository_) + lengthString(document->gitBase_)
                         + document->gitTarget_.toUtf8();
    request(GitDiff, payload, [this, document, generation](quint16 status, const QByteArray &payload) {
        if (generation != document->loadGeneration_) return;
        document->refreshing_ = false;
        QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
        QString text = decoder(payload);
        const bool failed = status || decoder.hasError() || text.contains(QChar::Null);
        if (failed) {
            text = QStringLiteral("差异获取失败：%1\n请重新输入 @GIT diff 或 @GIT worktree 后重试。")
                       .arg(status ? QString::fromUtf8(payload) : QStringLiteral("响应不是有效 UTF-8 文本"));
            document->stale_ = true;
            setMessage(text);
        } else {
            if (!document->gitFile_.isEmpty()) text = filePatch(text, document->gitFile_);
            if (text.isEmpty()) text = document->gitFile_.isEmpty() ? QStringLiteral("没有差异。")
                : QStringLiteral("%1\n没有可显示的文本差异（未跟踪文件不会出现在 git diff HEAD 中）。")
                      .arg(document->gitFile_);
            document->stale_ = false;
        }
        document->loaded_ = true;
        document->savedText_ = text;
        document->setText(text);
        emit document->stateChanged();
    });
}
} // namespace tina::ui
