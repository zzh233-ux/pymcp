# AutoLinker MCP — 外部 Python MCP 服务器 + C++ 桥接

将易语言 IDE 的 AI Agent 能力从进程内单线程 C++ HTTP 服务器,重构为 **外部 Python 异步 MCP 服务器 + IDE 内精简 C++ 命名管道桥接**。

## 架构

```
MCP 客户端 (Claude Code / Cursor / 等)
    ↓ streamable-http / SSE
Python MCP 服务器 (FastMCP, asyncio)
    ├── 本地工具 (无需 IDE):
    │   run_powershell_command / search_web_tavily / fetch_url / extract_web_document
    └── IDE 工具 (转发到桥接):
        ↓ 命名管道 \\.\pipe\AutoLinkerBridge
    C++ 桥接插件 (IDE 进程内)
        ↓ SendMessage → IDE 主线程
    IDEFacade / EideInternalTextBridge / WorkspaceMirror
```

### 解决的问题

原 `LocalMcpServer` 的卡顿根因:
1. **单线程 + select 轮询** — 一个慢请求(如 600 秒 PowerShell)阻塞整个服务器
2. **无 SSE / 连接复用** — 每次请求 TCP 三次握手
3. **镜像全量重建** — 每次编辑后下一次读取触发 e-packager 子进程(1-5 秒)
4. **注册表 I/O 阻塞 accept 线程** — 每 2 秒文件读写 + 跨进程 mutex

新架构的改进:
- Python asyncio 处理多客户端并发,SSE/streamable-http 传输
- 非IDE工具(PowerShell/搜索)在 Python 进程执行,不阻塞 IDE
- C++ 桥接仅做管道 I/O + 工具派发,极轻量
- 镜像增量优化方案(见 `MIRROR_OPTIMIZATION.md`)

## 目录结构

```
├── python/                    # Python MCP 服务器
│   ├── server.py              # 主服务器 (FastMCP, 15 个工具)
│   ├── bridge_client.py       # 命名管道客户端
│   ├── config.py              # 配置 (环境变量 / config.json)
│   ├── requirements.txt       # Python 依赖
│   └── config.example.json    # 配置示例
├── cpp_bridge/                # C++ 桥接插件 (替换 LocalMcpServer)
│   ├── BridgeProtocol.h       # IPC 协议 (header-only)
│   ├── BridgeServer.h         # 桥接服务端声明
│   ├── BridgeServer.cpp       # 桥接服务端实现
│   └── INTEGRATION.md         # 集成到 AutoLinker VS 项目的步骤
├── PROTOCOL.md                # IPC 协议规范
├── MIRROR_OPTIMIZATION.md     # 镜像增量优化方案 (5 个优化策略)
└── README.md                  # 本文件
```

## 快速开始

### 前置条件

- Python 3.10+ (推荐 3.12)
- 易语言 IDE + AutoLinker 桥接插件 (需编译 C++ 代码)

### 1. 安装 Python 依赖

```powershell
cd python
pip install -r requirements.txt
```

### 2. 配置 (可选)

复制配置示例并按需修改:

```powershell
copy config.example.json config.json
```

或在环境变量中设置:

```powershell
set AL_MCP_PORT=19207
set TAVILY_API_KEY=your_key_here
```

### 3. 启动 MCP 服务器

```powershell
python server.py
```

服务器监听 `http://127.0.0.1:19207/mcp`。

### 4. 连接 MCP 客户端

#### Claude Code

在 Claude Code 的 MCP 配置中添加:

```json
{
    "mcpServers": {
        "autolinker": {
            "url": "http://127.0.0.1:19207/mcp"
        }
    }
}
```

#### Cursor / 其他

参考各工具的 MCP 配置文档,添加相同的 URL。

### 5. (C++ 侧) 集成桥接插件

参考 `cpp_bridge/INTEGRATION.md`,将 C++ 桥接代码集成到 AutoLinker VS 项目中,替换原有的 `LocalMcpServer`。

## 工具列表

### IDE 桥接工具 (11 个,转发到 C++)

| 工具 | 说明 |
|------|------|
| list_files | 列出工程镜像文件 |
| search_code | 搜索源码 |
| read_file | 读取文件(带行号) |
| edit_file | 替换文本(映射到 IDE 真实页面) |
| multi_edit_file | 批量替换 |
| write_file | 整页覆盖 |
| diff_file | 预览差异 |
| restore_file_snapshot | 恢复快照 |
| get_current_page_info | 当前页信息 |
| get_current_eide_info | IDE 实例信息 |
| compile_with_output_path | 编译到指定路径 |

### Python 本地工具 (4 个,直接执行)

| 工具 | 说明 |
|------|------|
| run_powershell_command | 执行 PowerShell 命令 |
| search_web_tavily | Tavily 网页搜索 |
| fetch_url | 抓取 URL 内容 |
| extract_web_document | 提取网页正文 |

## 配置项

| 配置 | 环境变量 | 默认值 | 说明 |
|------|---------|--------|------|
| 管道名 | AL_PIPE_NAME | `\\.\pipe\AutoLinkerBridge` | C++ 桥接管道 |
| 监听地址 | AL_MCP_HOST | 127.0.0.1 | MCP 服务器地址 |
| 监听端口 | AL_MCP_PORT | 19207 | MCP 服务器端口 |
| 路径 | AL_MCP_PATH | /mcp | MCP 端点路径 |
| 工具超时 | AL_TOOL_TIMEOUT | 60 | 默认工具超时(秒) |
| 编译超时 | AL_COMPILE_TIMEOUT | 300 | 编译工具超时(秒) |
| Tavily密钥 | TAVILY_API_KEY | (空) | Tavily 搜索 API |

## 故障排查

### 无法连接 IDE 桥接

1. 确认易语言 IDE 已启动并加载了 AutoLinker 桥接插件
2. 检查 IDE 日志中是否有 `[BridgeServer] 已启动`
3. 确认管道名匹配: `\\.\pipe\AutoLinkerBridge`
4. 用 PowerShell 检查管道是否存在: `Get-ChildItem \\.\pipe\ | Where-Object Name -like "*AutoLinker*"`

### pywin32 安装失败

```powershell
pip install pywin32 --index-url https://pypi.org/simple/
python -m pywin32_postinstall -install
```

### PowerShell 命令中文乱码

PowerShell 输出编码可能是 GBK。已在 `run_powershell_command` 中处理多编码回退。如仍有问题,在命令前加:
```powershell
[Console]::OutputEncoding=[System.Text.Encoding]::UTF8;
```

## 技术文档

- [IPC 协议规范](PROTOCOL.md) — 命名管道消息格式
- [C++ 集成指南](cpp_bridge/INTEGRATION.md) — 替换 LocalMcpServer 的步骤
- [镜像优化方案](MIRROR_OPTIMIZATION.md) — 解决编辑后读取卡顿的 5 个策略
