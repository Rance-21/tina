#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

namespace tina::ui {

class CodeHighlighter;
class FfiClientBridge;

// 每个标签独立保存文本、版本、撤销栈和高亮器，切换时不重新读取或重建编辑器。
class WorkspaceDocument : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("文件由工作区客户端创建")
    Q_PROPERTY(QString path READ path CONSTANT)
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY stateChanged)
    Q_PROPERTY(bool saving READ saving NOTIFY stateChanged)
    Q_PROPERTY(bool refreshing READ refreshing NOTIFY stateChanged)
    Q_PROPERTY(bool conflict READ conflict NOTIFY stateChanged)
    Q_PROPERTY(bool stale READ stale NOTIFY stateChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY stateChanged)
    Q_PROPERTY(bool readOnly READ readOnly NOTIFY stateChanged)
    Q_PROPERTY(QString remoteText READ remoteText NOTIFY stateChanged)
    Q_PROPERTY(bool preview READ preview CONSTANT)
    Q_PROPERTY(int layoutRevision READ layoutRevision NOTIFY layoutChanged)

  public:
    explicit WorkspaceDocument(QString path, QObject *parent = nullptr);
    QString path() const { return path_; }
    QString name() const { return displayName_.isEmpty() ? path_.section('/', -1) : displayName_; }
    QString text() const { return text_; }
    QString remoteText() const { return savedText_; }
    bool dirty() const { return loaded_ && text_ != savedText_; }
    bool saving() const { return saving_; }
    bool refreshing() const { return refreshing_; }
    bool conflict() const { return conflict_; }
    bool stale() const { return stale_; }
    bool loaded() const { return loaded_; }
    bool readOnly() const { return readOnly_ || !loaded_; }
    bool preview() const { return preview_; }
    int layoutRevision() const { return layoutRevision_; }
    void setText(const QString &text);
    Q_INVOKABLE void attach(QQuickTextDocument *document);
    Q_INVOKABLE QVariantList visibleLines(qreal top, qreal height) const;

  signals:
    void textChanged();
    void stateChanged();
    void layoutChanged();

  private:
    friend class FfiClientBridge;
    bool applyRemote(const QByteArray &bytes, quint64 revision);
    QByteArray encodedText() const;
    QString path_;
    QString text_;
    QString savedText_;
    // Diff 复用只读代码标签，但不参与远程文件读写及断线重读。
    QString displayName_, gitRepository_, gitBase_, gitTarget_, gitFile_;
    bool preview_{};
    int layoutRevision_{};
    quint64 revision_{}; // 留在 C++，避免 QML double 丢失 64 位 revision 精度。
    quint64 loadGeneration_{};
    bool loaded_{};
    bool saving_{};
    bool refreshing_{};
    bool conflict_{};
    bool stale_{true};
    bool readOnly_{};
    bool crlf_{};
    bool bom_{};
    QPointer<QQuickTextDocument> quickDocument_;
    CodeHighlighter *highlighter_{};
};

} // namespace tina::ui
