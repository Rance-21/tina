#include "ffi_client_bridge.hpp"
#include <QDebug>

extern "C" {

TinaFfiClientHandle* tina_ffi_client_create(const char* /*host*/, std::uint16_t /*port*/) {
    return nullptr;
}

void tina_ffi_client_destroy(TinaFfiClientHandle* /*handle*/) {}

std::uint64_t tina_ffi_client_send_command(TinaFfiClientHandle* /*handle*/, const char* /*cmd_utf8*/) {
    return 0;
}

std::uint64_t tina_ffi_client_send_agent_prompt(TinaFfiClientHandle* /*handle*/, const char* /*prompt_utf8*/) {
    return 0;
}

} // extern "C"

namespace tina::ui {

FfiClientBridge::FfiClientBridge(QObject* parent) : QObject(parent) {}

void FfiClientBridge::submitCommand(const QString& command) {
    qInfo() << "[Tina IDE Shell] Command submitted:" << command;
    emit commandSubmitted(command);
}

void FfiClientBridge::submitAgentInput(const QString& input) {
    qInfo() << "[Tina IDE Shell] Agent input submitted:" << input;
    if (input.trimmed().compare(QStringLiteral("@AI"), Qt::CaseInsensitive) == 0) {
        emit openAgentWindowRequested();
    }
}

} // namespace tina::ui
