# C++ 桥接集成指南

## 概述

将 `BridgeServer` 集成到现有 AutoLinker VS 项目中,替换原有的 `LocalMcpServer`。

## 文件清单

需要添加到项目的文件:

| 文件 | 说明 |
|------|------|
| `BridgeProtocol.h` | IPC 协议定义(header-only) |
| `BridgeServer.h` | 桥接服务端类声明 |
| `BridgeServer.cpp` | 桥接服务端实现 |

## 集成步骤

### 1. 添加源文件到 VS 项目

将三个文件复制到 `src/` 目录,然后在 `AutoLinker.vcxproj` 中添加:

```xml
<ClInclude Include="src\BridgeProtocol.h" />
<ClInclude Include="src\BridgeServer.h" />
<ClCompile Include="src\BridgeServer.cpp" />
```

或通过 VS 解决方案资源管理器: 右键 `src` 筛选器 → 添加 → 现有项。

### 2. 替换 LocalMcpServer

在 `src/AutoLinker.cpp` 的 `FneInit()` 函数中:

```cpp
// 修改前:
#include "LocalMcpServer.h"
// ...
LocalMcpServer::Initialize();

// 修改后:
#include "BridgeServer.h"
// ...
BridgeServer::Instance().Initialize();
```

在 IDE 关闭逻辑中(通常在 `~FneInit()` 或 `AutoLinker_MessageNotify` 的关闭通知中):

```cpp
// 修改前:
LocalMcpServer::Shutdown();

// 修改后:
BridgeServer::Instance().Shutdown();
```

### 3. 移除 LocalMcpServer(可选)

如果不再需要 HTTP MCP 服务器,可以从项目中移除:

```
src/LocalMcpServer.cpp
src/LocalMcpServer.h
src/LocalMcpInstanceRegistry.cpp  (可选保留,用于实例发现)
src/LocalMcpInstanceRegistry.h
```

从 `.vcxproj` 和 `.filters` 中删除对应条目。

**注意**: `LocalMcpInstanceRegistry` 提供跨进程实例发现功能。如果需要让外部工具自动发现 IDE 实例,可以保留它并让 `BridgeServer` 也写入注册表。参考 `BridgeServer.cpp` 中添加注册表刷新逻辑。

### 4. 保留的依赖

`BridgeServer.cpp` 依赖以下现有模块(不要移除):

| 模块 | 用途 |
|------|------|
| `AIChatFeature.h` | `ExecutePublicTool()` 工具派发入口 |
| `AIChatTooling.cpp` | 工具实现(第一层分发) |
| `AIChatToolingX86.cpp` | 工具实现(主线程执行) |
| `IDEFacade.cpp` | IDE 接口(NotifySys) |
| `EideInternalTextBridge.cpp` | IDE 内存级源码读写 |
| `WorkspaceMirror.cpp` | 工程镜像管理 |
| `WorkspaceFileTools.cpp` | 文件工具实现 |
| `EPackagerIntegration.cpp` | e-packager 集成 |
| `Logger.h` | 日志 |
| `StringHelper.h` | 字符串转换 |
| `thirdparty/json.hpp` | JSON 解析 |

### 5. 编码注意事项

- 源文件使用 **UTF-8 with BOM** 编码,使用 **CRLF** 换行
- 在 VS 中: 文件 → 另存为 → 编码选择 "Unicode (UTF-8 带签名) - 代码页 65001"
- 或在 `.editorconfig` 中配置: `charset = utf-8-bom`, `end_of_line = crlf`

### 6. 编译验证

```powershell
MSBuild.exe ..\AutoLinker.vcxproj /t:Build "/p:Configuration=fne_release;Platform=Win32" /m
```

确保无编译错误。常见问题:
- `AIChatFeature::ExecutePublicTool` 签名不匹配:检查 `AIChatFeature.h` 中的实际声明,调整 `BridgeServer.cpp` 中的调用
- `StringHelper::WToUtf8` 不存在:检查 `StringHelper.h` 中的实际函数名,可能是 `WToUtf8` 或 `WideToUtf8`

### 7. 测试

1. 将编译出的 `AutoLinker.fne` 覆盖到易语言 lib 目录
2. 启动易语言 IDE,打开任意 .e 工程
3. 检查日志中是否有 `[BridgeServer] 已启动` 信息
4. 启动 Python MCP 服务器: `python server.py`
5. 用 MCP 客户端连接 `http://127.0.0.1:19207/mcp`
6. 调用 `get_current_eide_info` 工具验证连通性

## 架构对比

```
修改前 (LocalMcpServer):
  MCP客户端 → HTTP(19207) → [C++ HTTP服务器 单线程] → IDE主线程
  问题: 单线程,慢请求阻塞所有连接,无SSE,无连接复用

修改后 (BridgeServer):
  MCP客户端 → HTTP(19207) → [Python MCP服务器 async] → 命名管道 → [C++桥接] → IDE主线程
  优势: Python处理并发/SSE/协议,C++只做管道I/O+工具派发,职责分离
```
