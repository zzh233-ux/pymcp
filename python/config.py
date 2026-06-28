"""AutoLinker MCP Server 配置。

所有配置项均可通过环境变量覆盖。也可在同目录下放置 config.json 覆盖默认值。
"""

import json
import os
from pathlib import Path

# ── 命名管道 ──────────────────────────────────────────────
BRIDGE_PIPE_NAME = os.environ.get(
    "AL_PIPE_NAME", r"\\.\pipe\AutoLinkerBridge"
)

# ── MCP 服务器 ────────────────────────────────────────────
MCP_HOST = os.environ.get("AL_MCP_HOST", "127.0.0.1")
MCP_PORT = int(os.environ.get("AL_MCP_PORT", "19207"))
MCP_PATH = os.environ.get("AL_MCP_PATH", "/mcp")

# ── 超时(秒) ───────────────────────────────────────────────
TOOL_TIMEOUT = int(os.environ.get("AL_TOOL_TIMEOUT", "60"))
COMPILE_TIMEOUT = int(os.environ.get("AL_COMPILE_TIMEOUT", "300"))
CONNECT_TIMEOUT = float(os.environ.get("AL_CONNECT_TIMEOUT", "5"))
HEALTH_CHECK_INTERVAL = int(os.environ.get("AL_HEALTH_INTERVAL", "10"))

# ── 本地工具 ──────────────────────────────────────────────
TAVILY_API_KEY = os.environ.get("TAVILY_API_KEY", "")
HTTP_USER_AGENT = "AutoLinker-MCP/1.0"

# ── 实例发现 ──────────────────────────────────────────────
# C++ 桥接插件将自身信息写入此文件,Python 服务器读取以自动发现管道名
INSTANCE_REGISTRY = os.path.join(
    os.environ.get("TEMP", os.getcwd()),
    "AutoLinker", "bridge_instances.json"
)


def load_config_file() -> None:
    """从同目录下的 config.json 加载配置(如果存在)。"""
    cfg_path = Path(__file__).parent / "config.json"
    if not cfg_path.exists():
        return
    try:
        with open(cfg_path, "r", encoding="utf-8") as f:
            cfg = json.load(f)
    except Exception:
        return

    g = globals()
    for key, val in cfg.items():
        if key in g:
            g[key] = val


load_config_file()
