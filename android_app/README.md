# Tina 平板客户端

Qt Quick 客户端通过异步 TCP 实现与 `src/ide/client_file.cppm`、
`src/ide/client_terminal.cppm` 对应的工作区和终端操作，不依赖服务端的 Linux 网络模块。

## 使用

- Android 使用沉浸式全屏，隐藏状态栏和系统导航栏；从屏幕边缘滑动可临时呼出系统栏。
- 左侧按打开顺序混排紧凑的文件和终端标签，代码顶部不再重复显示文件名；输入框与代码区左右对齐，AI Agent 面板与输入框底边对齐。
- 文件树、菜单和 Agent 图标使用矢量路径绘制，不依赖系统字体中的特殊符号。
- 输入 `@` 显示 AI / GIT / FILE / TERMINAL 四行菜单。输入 `@FILE`、点击底部文件夹按钮或按 `Ctrl+P` 可浏览文件。
- 首次列出项目。选中项目后，文件夹会在树中原地展开和折叠，再次打开文件窗口时会更新当前展开的分支。
- `↑` / `↓` 选择，`Enter` 或单击打开文件或展开、折叠目录，`←` / `→` 在树中导航。长按文件夹按钮、按 `Alt+↑` 或退格可返回项目列表，`Esc` 关闭弹窗。
- 文件在左侧生成标签，默认按 UTF-8 读取和保存。支持多标签编辑、撤销、语法高亮和水平/垂直滚动。
- 未保存标签显示琥珀色边缘。按 `Ctrl+S` 写入服务端；保存期间继续输入的内容仍保留未保存状态。
- `Ctrl+Tab` / `Ctrl+Shift+Tab` 在左侧混排标签中切换，文件保留各自的光标、滚动位置和撤销记录。
- 底部默认保留原来的单行输入框，不显示终端工具栏、用户名、主机或路径提示符。拖动上边缘可向上展开终端输出、向下收起；切到文件标签时恢复单行。
- 连接后自动打开一个终端；点击左侧终端标签切换，最多同时打开 16 个。点击菜单的 TERMINAL 行可填入 `@terminal `。
- 输入 `@terminal add` 自动分配编号，`@terminal add 3` 添加 Terminal 3，`@terminal del 3` 删除并关闭它；`@terminal` 返回最近使用的终端，无终端时新建。编号与左侧标签一致，重连后保持不变。
- 每个终端独立保存输入草稿和输出环形缓冲区，最多保留 128K 个 UTF-16 单元（256 KiB）及 2000 个换行，超限丢弃最旧内容，长行同样受限。
- 回车或右侧箭头发送输入，支持运行中的程序继续读取输入；`Tab` 补全，`Ctrl+C` 中断（有选中文本时复制），`Ctrl+L` 清空当前输出。
- 在输出底部自动跟随新内容，上滚查看历史时停止跟随。中文、emoji 和常见 ANSI 控制序列支持跨输出分片处理，输出以纯文本显示。
- 断线保留各终端的历史输出和草稿，重连后启动新的 Shell；创建、关闭、输入和补全均不自动重发，避免重复执行命令。

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

### 本地连接配置

首次构建先复制示例配置，然后在 `android_app/client.local.cmake` 填写实际域名或 IP 和端口：

```sh
cp android_app/client.local.cmake.example android_app/client.local.cmake
```

`TINA_SERVER_HOST` 只填写地址，不带协议和端口；`TINA_SERVER_PORT` 填写端口。
本地配置已加入 `.gitignore`，真实地址不会写入可提交的源码或文档。
Android APK 和 PC 预览程序在构建时共用此配置，修改后再次构建会自动更新。

### 桌面预览

桌面预览（使用本机 Qt 6 开发环境）：

```sh
cmake -S android_app -B android_app/build-workspace -DCMAKE_BUILD_TYPE=Debug -DTINA_BUILD_TESTS=ON
cmake --build android_app/build-workspace -j 4
ctest --test-dir android_app/build-workspace --output-on-failure
cmake --build android_app/build-workspace --target tina_ide_client_qmllint
```

测试使用仅监听本机的模拟 TCP 服务端，覆盖分包、失败重试、目录事件、响应乱序、分段版本变化、
保存冲突、保存期间继续编辑、断线恢复、UTF-8、写入上限、语法高亮、外接键盘交互、
多终端输出隔离、环形缓冲区淘汰、分片 UTF-8/ANSI、终端重连、按编号增删、混排标签和拖动扩展。

PC 客户端输出为项目根目录的 `tina_ide_client`（Windows 为 `tina_ide_client.exe`）。
可运行 `./tina_ide_client` 启动，通过 `--host 地址 --port 端口` 临时覆盖本地构建配置。
`--smoke-test` 启动界面后自动退出。

### Android APK

Android 请使用 Qt 的 Android **arm64-v8a** Kit 和配套 SDK/NDK 配置此 CMake 项目。
配置完成后执行：

```sh
cmake --build android_app/build-android-arm64 --target apk -j 4
```

标准 APK 构建（默认构建、`apk` 或 `tina_ide_client_make_apk` 目标）完成后，
会自动将最新安装包更新到项目根目录的 `tina_ide_client.apk`。
APK、PC 客户端及构建中间文件均已忽略，不提交到 Git。

测试目标不会进入 Android 构建。界面使用 48 像素触控行、低分辨率毛玻璃采样和 150 ms 位移/透明度动画；
隐藏标签不重新创建编辑器，高亮按文本块增量更新，行号只创建可见区域附近的对象。
