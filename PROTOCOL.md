# IPC 协议规范

## 概述

Python MCP 服务器与 C++ IDE 桥接插件之间通过 **Windows 命名管道** 通信,使用 **长度前缀 JSON** 消息格式。

## 传输层

| 属性 | 值 |
|------|-----|
| 管道名 | `\\.\pipe\AutoLinkerBridge` |
| 方向 | 双向 (PIPE_ACCESS_DUPLEX) |
| 模式 | 字节流 (PIPE_TYPE_BYTE \| PIPE_READMODE_BYTE) |
| 缓冲区 | 64KB |
| 连接模型 | 持久连接,请求-响应 |

## 消息帧格式

```
┌──────────────────┬──────────────────────────────────┐
│ 4 字节长度头      │ JSON 载荷 (UTF-8)                │
│ (小端 uint32)     │ (长度 = 长度头的值)              │
└──────────────────┴──────────────────────────────────┘
```

- 长度头: 4 字节,小端序 unsigned 32 位整数,表示 JSON 载荷的字节数
- 载荷: UTF-8 编码的 JSON 文本
- 最大载荷: 16 MB

## 请求 (Python → C++)

```json
{
    "id": "550e8400-e29b-41d4-a716-446655440000",
    "tool": "read_file",
    "arguments": {
        "file_path": "src/某程序集.txt"
    }
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| id | string | 请求唯一标识(UUID),用于关联响应 |
| tool | string | 工具名称 |
| arguments | object | 工具参数,结构因工具而异 |

## 响应 (C++ → Python)

### 成功

```json
{
    "id": "550e8400-e29b-41d4-a716-446655440000",
    "ok": true,
    "result": "文件内容...",
    "error": null
}
```

### 失败

```json
{
    "id": "550e8400-e29b-41d4-a716-446655440000",
    "ok": false,
    "result": null,
    "error": {
        "code": -32603,
        "message": "页面不存在",
        "data": null
    }
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| id | string | 对应请求的 ID |
| ok | bool | 是否成功 |
| result | any | 成功时的结果(字符串或对象) |
| error | object\|null | 失败时的错误信息 |

### 错误码

| 码 | 含义 |
|-----|------|
| -32600 | 无效请求(缺少工具名等) |
| -32601 | 方法未找到 |
| -32602 | 无效参数 |
| -32603 | 内部错误 |

## 特殊工具

### `__health__`

健康检查,不经过工具派发,由桥接直接响应:

```json
// 请求
{"id": "h1", "tool": "__health__", "arguments": {}}

// 响应
{"id": "h1", "ok": true, "result": {"pid": 12345, "pipe": "\\\\.\pipe\\AutoLinkerBridge", "status": "ok"}, "error": null}
```

## 工具列表

### IDE 桥接工具(11个)

| 工具 | 参数 | 说明 |
|------|------|------|
| list_files | path? | 列出工程镜像文件 |
| search_code | query, file_pattern? | 搜索源码 |
| read_file | file_path | 读取文件 |
| edit_file | file_path, old_text, new_text | 替换文本 |
| multi_edit_file | file_path, edits[] | 批量替换 |
| write_file | file_path, content | 整页覆盖 |
| diff_file | file_path, new_text | 预览差异 |
| restore_file_snapshot | file_path | 恢复快照 |
| get_current_page_info | (无) | 当前页信息 |
| get_current_eide_info | (无) | IDE 实例信息 |
| compile_with_output_path | output_path | 编译 |

### Python 本地工具(4个,不经管道)

| 工具 | 参数 | 说明 |
|------|------|------|
| run_powershell_command | command, timeout? | 执行 PowerShell |
| search_web_tavily | query, max_results? | Tavily 搜索 |
| fetch_url | url | 抓取 URL |
| extract_web_document | url | 提取网页正文 |

## 连接生命周期

1. **C++ 桥接启动**: IDE 加载插件 → `BridgeServer::Initialize()` → 创建管道实例 → 等待连接
2. **Python 连接**: MCP 服务器首次收到 IDE 工具调用 → `BridgeClient.connect()` → 连接管道
3. **请求-响应循环**: Python 发送请求 → C++ 读取并派发到主线程 → 返回响应
4. **断开**: Python 进程退出或管道断开 → C++ 检测到 `ReadFile` 失败 → 创建新管道实例等待重连
5. **C++ 关闭**: IDE 关闭 → `BridgeServer::Shutdown()` → 关闭管道 → join 线程
