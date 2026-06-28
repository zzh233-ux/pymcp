"""AutoLinker MCP Server — 外部 Python MCP 服务器。

架构:
  MCP 客户端 (Claude Code 等)
    ↓ streamable-http / SSE
  本服务器 (FastMCP, asyncio)
    ├── 本地工具 (无需 IDE):
    │   run_powershell_command / search_web_tavily / fetch_url / extract_web_document
    └── IDE 工具 (转发到 C++ 桥接):
        ↓ 命名管道 \\\\.\\pipe\\AutoLinkerBridge
      C++ 桥接插件 (IDE 进程内)
        ↓ SendMessage → IDE 主线程
      IDEFacade / EideInternalTextBridge / WorkspaceMirror

启动:
  pip install -r requirements.txt
  python server.py

环境变量:
  AL_MCP_PORT=19207        MCP 监听端口
  AL_PIPE_NAME=\\\\.\\pipe\\AutoLinkerBridge   桥接管道名
  TAVILY_API_KEY=xxx       Tavily 搜索 API 密钥
"""

import asyncio
import logging
import sys

import httpx

from fastmcp import FastMCP

import config
from bridge_client import BridgeClient, BridgeError

# ── 日志 ──────────────────────────────────────────────────
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("autolinker.server")

# ── FastMCP 实例 ──────────────────────────────────────────
mcp = FastMCP("AutoLinker")

# ── 桥接客户端(全局,懒连接) ────────────────────────────
bridge = BridgeClient(config.BRIDGE_PIPE_NAME)


async def _bridge_call(
    tool: str,
    arguments: dict | None = None,
    timeout: float | None = None,
) -> str:
    """调用桥接工具,统一处理错误。"""
    timeout = timeout or config.TOOL_TIMEOUT
    try:
        return await bridge.call_text(tool, arguments or {}, timeout=timeout)
    except ConnectionError as e:
        return (
            f"[ERROR] 无法连接 IDE 桥接: {e}\n"
            f"请确保易语言 IDE 已启动并加载了 AutoLinker 桥接插件。\n"
            f"管道: {config.BRIDGE_PIPE_NAME}"
        )
    except BridgeError as e:
        return f"[ERROR] 桥接工具 '{tool}' 返回错误: {e}"
    except asyncio.TimeoutError:
        return f"[ERROR] 工具 '{tool}' 执行超时({timeout}秒)。"


# ════════════════════════════════════════════════════════════════
#  IDE 桥接工具(转发到 C++ 桥接插件)
# ════════════════════════════════════════════════════════════════

@mcp.tool
async def list_files(path: str = "") -> str:
    """列出易语言工程镜像中的所有文件。

    Args:
        path: 可选路径前缀,仅列出匹配的文件。
    """
    return await _bridge_call("list_files", {"path": path})


@mcp.tool
async def search_code(query: str, file_pattern: str = "") -> str:
    """在易语言工程源码中搜索文本。

    Args:
        query: 搜索关键词。
        file_pattern: 可选文件名通配符,如 "*.txt" 或 "*程序集*"。
    """
    return await _bridge_call("search_code", {
        "query": query, "file_pattern": file_pattern
    })


@mcp.tool
async def read_file(file_path: str) -> str:
    """读取易语言工程中的文件,返回带行号的文本。

    Args:
        file_path: 工程镜像中的相对路径,如 "src/某程序集.txt"。
    """
    return await _bridge_call("read_file", {"file_path": file_path})


@mcp.tool
async def edit_file(file_path: str, old_text: str, new_text: str) -> str:
    """编辑易语言工程中的文件,将 old_text 替换为 new_text。

    文件路径会自动映射到 IDE 真实程序项,直接修改 IDE 内存中的代码。

    Args:
        file_path: 工程镜像中的相对路径。
        old_text: 要替换的原文(必须精确匹配)。
        new_text: 替换后的文本。
    """
    return await _bridge_call("edit_file", {
        "file_path": file_path, "old_text": old_text, "new_text": new_text
    })


@mcp.tool
async def multi_edit_file(file_path: str, edits: list[dict[str, str]]) -> str:
    """对同一文件批量执行多处文本替换。

    Args:
        file_path: 工程镜像中的相对路径。
        edits: 替换列表,每项包含 old_text 和 new_text。
    """
    return await _bridge_call("multi_edit_file", {
        "file_path": file_path, "edits": edits
    })


@mcp.tool
async def write_file(file_path: str, content: str) -> str:
    """整体覆盖写入易语言工程文件。

    Args:
        file_path: 工程镜像中的相对路径。
        content: 文件完整内容。
    """
    return await _bridge_call("write_file", {
        "file_path": file_path, "content": content
    })


@mcp.tool
async def diff_file(file_path: str, new_text: str) -> str:
    """预览文件修改后的差异(不实际修改)。

    Args:
        file_path: 工程镜像中的相对路径。
        new_text: 预期修改后的完整内容。
    """
    return await _bridge_call("diff_file", {
        "file_path": file_path, "new_text": new_text
    })


@mcp.tool
async def restore_file_snapshot(file_path: str) -> str:
    """恢复文件到编辑前的快照状态。

    Args:
        file_path: 工程镜像中的相对路径。
    """
    return await _bridge_call("restore_file_snapshot", {
        "file_path": file_path
    })


@mcp.tool
async def get_current_page_info() -> str:
    """获取 IDE 当前打开的页面信息(页名、类型、源文件路径)。"""
    return await _bridge_call("get_current_page_info", {})


@mcp.tool
async def get_current_eide_info() -> str:
    """获取 IDE 实例信息(进程、项目类型、MCP 端点等)。"""
    return await _bridge_call("get_current_eide_info", {})


@mcp.tool
async def compile_with_output_path(output_path: str) -> str:
    """编译易语言工程到指定输出路径。

    Args:
        output_path: 编译产物(EXE/DLL)的完整输出路径。
    """
    return await _bridge_call(
        "compile_with_output_path",
        {"output_path": output_path},
        timeout=config.COMPILE_TIMEOUT,
    )


# ════════════════════════════════════════════════════════════════
#  本地工具(Python 直接实现,不需要 IDE)
# ════════════════════════════════════════════════════════════════

@mcp.tool
async def run_powershell_command(command: str, timeout: int = 60) -> str:
    """执行 PowerShell 命令并返回输出。

    Args:
        command: PowerShell 命令文本。
        timeout: 超时秒数(默认 60,最大 600)。
    """
    timeout = min(max(timeout, 5), 600)
    try:
        proc = await asyncio.create_subprocess_exec(
            "powershell.exe",
            "-NoProfile", "-NonInteractive", "-Command",
            command,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
    except FileNotFoundError:
        return "[ERROR] 找不到 powershell.exe"

    try:
        stdout_b, stderr_b = await asyncio.wait_for(
            proc.communicate(), timeout=timeout
        )
    except asyncio.TimeoutError:
        proc.kill()
        await proc.wait()
        return f"[TIMEOUT] 命令在 {timeout} 秒内未完成,已终止。"

    # PowerShell 输出可能是 UTF-8 或 GBK
    def _decode(b: bytes) -> str:
        for enc in ("utf-8", "gbk", "latin-1"):
            try:
                return b.decode(enc)
            except (UnicodeDecodeError, LookupError):
                continue
        return b.decode("utf-8", errors="replace")

    out = _decode(stdout_b)
    err = _decode(stderr_b)

    parts = [f"Exit code: {proc.returncode}", "", "--- stdout ---", out]
    if err.strip():
        parts += ["", "--- stderr ---", err]
    return "\n".join(parts)


@mcp.tool
async def search_web_tavily(query: str, max_results: int = 5) -> str:
    """使用 Tavily API 搜索网页。需要配置 TAVILY_API_KEY 环境变量。

    Args:
        query: 搜索关键词。
        max_results: 最大结果数(1-10)。
    """
    if not config.TAVILY_API_KEY:
        return (
            "[ERROR] Tavily API 密钥未配置。\n"
            "请设置环境变量 TAVILY_API_KEY 或在 config.json 中配置。"
        )

    max_results = min(max(max_results, 1), 10)
    try:
        async with httpx.AsyncClient(timeout=30) as client:
            resp = await client.post(
                "https://api.tavily.ai/search",
                json={
                    "api_key": config.TAVILY_API_KEY,
                    "query": query,
                    "max_results": max_results,
                    "search_depth": "advanced",
                },
            )
            resp.raise_for_status()
            data = resp.json()
    except httpx.HTTPError as e:
        return f"[ERROR] Tavily 搜索请求失败: {e}"

    results = data.get("results", [])
    if not results:
        return f"未找到 '{query}' 的搜索结果。"

    lines = [f"找到 {len(results)} 条结果:\n"]
    for i, r in enumerate(results, 1):
        lines.append(f"{i}. {r.get('title', 'N/A')}")
        lines.append(f"   URL: {r.get('url', '')}")
        content = r.get("content", "")[:300]
        lines.append(f"   {content}")
        lines.append("")
    return "\n".join(lines)


@mcp.tool
async def fetch_url(url: str) -> str:
    """抓取指定 URL 的原始内容(HTML/文本)。

    Args:
        url: 要抓取的完整 URL。
    """
    try:
        async with httpx.AsyncClient(
            timeout=30, follow_redirects=True
        ) as client:
            resp = await client.get(
                url, headers={"User-Agent": config.HTTP_USER_AGENT}
            )
            resp.raise_for_status()
            return resp.text
    except httpx.HTTPError as e:
        return f"[ERROR] 抓取 URL 失败: {e}"


@mcp.tool
async def extract_web_document(url: str) -> str:
    """抓取 URL 并提取网页正文内容(去除导航、广告等)。

    Args:
        url: 要提取的网页 URL。
    """
    try:
        import trafilatura
    except ImportError:
        return (
            "[ERROR] trafilatura 未安装。请运行: pip install trafilatura"
        )

    try:
        async with httpx.AsyncClient(
            timeout=30, follow_redirects=True
        ) as client:
            resp = await client.get(
                url, headers={"User-Agent": config.HTTP_USER_AGENT}
            )
            resp.raise_for_status()
            html = resp.text
    except httpx.HTTPError as e:
        return f"[ERROR] 抓取 URL 失败: {e}"

    extracted = trafilatura.extract(html, include_comments=False, include_tables=True)
    if not extracted:
        return "[ERROR] 无法从页面中提取正文内容。页面可能是 JavaScript 渲染的。"
    return extracted


# ════════════════════════════════════════════════════════════════
#  启动
# ════════════════════════════════════════════════════════════════

def main():
    logger.info("AutoLinker MCP Server 启动中...")
    logger.info("  监听: http://%s:%d%s", config.MCP_HOST, config.MCP_PORT, config.MCP_PATH)
    logger.info("  桥接管道: %s", config.BRIDGE_PIPE_NAME)
    logger.info("  本地工具: run_powershell_command, search_web_tavily, fetch_url, extract_web_document")
    logger.info("  IDE 工具: list_files, search_code, read_file, edit_file, multi_edit_file,")
    logger.info("           write_file, diff_file, restore_file_snapshot, get_current_page_info,")
    logger.info("           get_current_eide_info, compile_with_output_path")

    # FastMCP run 会启动事件循环并阻塞
    mcp.run(
        transport="streamable-http",
        host=config.MCP_HOST,
        port=config.MCP_PORT,
    )


if __name__ == "__main__":
    main()
