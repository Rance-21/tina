#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>
#include <cstdint>

// ============================================================================
// Rust / C++ FFI Client C-ABI 预留接口声明（供后续阶段链接 Rust / C++ RPC Client 使用）
// ============================================================================
extern "C" {
struct TinaFfiClientHandle;

// 预留 FFI 函数签名：后续阶段由 Rust staticlib/cdylib 或 C++ rpc::RpcClient 实现
TinaFfiClientHandle* tina_ffi_client_create(const char* host, std::uint16_t port);
void tina_ffi_client_destroy(TinaFfiClientHandle* handle);
std::uint64_t tina_ffi_client_send_command(TinaFfiClientHandle* handle, const char* cmd_utf8);
std::uint64_t tina_ffi_client_send_agent_prompt(TinaFfiClientHandle* handle, const char* prompt_utf8);
}

namespace tina::ui {

class FfiClientBridge : public QObject {
    Q_OBJECT
    QML_ELEMENT

public:
    explicit FfiClientBridge(QObject* parent = nullptr);
    ~FfiClientBridge() override = default;

    // 供 QML 前端调用的占位接口（第一阶段仅做 UI 壳与状态模拟，不依赖后端完成）
    Q_INVOKABLE void submitCommand(const QString& command);
    Q_INVOKABLE void submitAgentInput(const QString& input);

signals:
    void commandSubmitted(const QString& command);
    void openAgentWindowRequested();
};

} // namespace tina::ui
