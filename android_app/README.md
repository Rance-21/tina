# Tina 平板客户端

Qt Quick 客户端通过异步 TCP 实现与 `src/ide/client.cppm` 对应的
`listProjects`、`listDir`、`readFile`、`writeFile`，不依赖服务端的 Linux 网络模块。

## 使用

- 输入 `@FILE`，或点击底部文件夹按钮、按 `Ctrl+P`，在 AI / GIT / FILE 毛玻璃弹窗中浏览文件。
- 首次列出项目。选中项目后，文件夹会在树中原地展开和折叠，再次打开文件窗口时会更新当前展开的分支。
- `↑` / `↓` 选择，`Enter` 或单击打开文件或展开、折叠目录，`←` / `→` 在树中导航。长按文件夹按钮、按 `Alt+↑` 或退格可返回项目列表，`Esc` 关闭弹窗。
- 文件在左侧生成标签，默认按 UTF-8 读取和保存。支持多标签编辑、撤销、语法高亮和水平/垂直滚动。
- 未保存标签显示琥珀色边缘。按 `Ctrl+S` 写入服务端；保存期间继续输入的内容仍保留未保存状态。
- `Ctrl+Tab` / `Ctrl+Shift+Tab` 切换标签，保留各自的光标、滚动位置和撤销记录。

## 失败与重新获取

临时服务端错误和读请求超时最多尝试三次，之后可手动刷新。目录被删除时重新获取上级目录。
客户端接收目录变更事件；如果事件与目录请求交错，会再次获取目录，避免使用旧列表。

保存失败、版本冲突或响应丢失时，客户端先重新读取文件，不自动重发写请求。
本地修改始终保留。冲突时可查看最新服务端内容，再选择“保留本地并保存”或“采用服务端版本”。
断线后自动重连并刷新已打开的文档。64 位 revision 始终在 C++ 内处理，不经过 JavaScript 数值转换。

按协议每段读取最多 256 KiB。分段间版本变化会从头重读，防止混合不同版本。
服务端单帧上限为 1 MiB，因此超过可写负载上限的文件只读；平板端单文件读取上限为 8 MiB。
无效 UTF-8 或含零字节的二进制内容不会进入可编辑缓冲区。
已有 UTF-8 BOM 和 CRLF 换行在保存时保留，不提供额外编码选择。

## 构建与验证

桌面预览（使用本机 Qt 6 开发环境）：

```sh
cmake -S android_app -B android_app/build-workspace -DCMAKE_BUILD_TYPE=Debug -DTINA_BUILD_TESTS=ON
cmake --build android_app/build-workspace -j 4
ctest --test-dir android_app/build-workspace --output-on-failure
cmake --build android_app/build-workspace --target tina_ide_client_qmllint
```

测试使用仅监听本机的模拟 TCP 服务端，覆盖分包、失败重试、目录事件、响应乱序、分段版本变化、
保存冲突、保存期间继续编辑、断线恢复、UTF-8、写入上限、语法高亮和外接键盘交互。

可通过 `--host 地址 --port 端口` 更改桌面预览连接地址，默认沿用原来的 `100.65.173.4:3490`。
`--smoke-test` 启动界面后自动退出。

Android 请使用 Qt 的 Android **arm64-v8a** Kit 和配套 SDK/NDK 配置此 CMake 项目。
测试目标不会进入 Android 构建。界面使用 48 像素触控行、低分辨率毛玻璃采样和 150 ms 位移/透明度动画；
隐藏标签不重新创建编辑器，高亮按文本块增量更新，行号只创建可见区域附近的对象。
