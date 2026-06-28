"""命名管道客户端 — 与 IDE 内的 C++ 桥接插件通信。

协议: 长度前缀 JSON
  [4 字节 little-endian uint32 长度][UTF-8 JSON 载荷]

连接模型: 单条持久连接 + asyncio.Lock 串行化。
IDE 操作本身在主线程串行执行,因此单连接不会成为瓶颈。
所有阻塞 I/O 通过 asyncio.to_thread() 卸载到线程池,事件循环永不阻塞。
"""

import json
import struct
import asyncio
import uuid
import logging
from typing import Any

logger = logging.getLogger("autolinker.bridge")

# ── 协议常量 ──────────────────────────────────────────────
HEADER_SIZE = 4
HEADER_FORMAT = "<I"          # little-endian uint32
RECV_CHUNK = 65536            # 64KB

# pywin32 在非 Windows 或未安装时不可用
try:
    import win32file
    import win32pipe
    import pywintypes
    _HAS_PYWIN32 = True
except ImportError:
    _HAS_PYWIN32 = False


class BridgeError(Exception):
    """桥接返回的错误。"""

    def __init__(self, code: int, message: str, data: Any = None):
        self.code = code
        self.message = message
        self.data = data
        super().__init__(f"[{code}] {message}")


class BridgeClient:
    """异步命名管道客户端。

    用法::

        bridge = BridgeClient(pipe_name)
        result = await bridge.call_raw("read_file", {"file_path": "src/x.txt"})
    """

    def __init__(self, pipe_name: str):
        self.pipe_name = pipe_name
        self._handle = None
        self._connected = False
        self._lock = asyncio.Lock()

    # ── 连接管理 ──────────────────────────────────────────

    async def connect(self, timeout: float = 5.0) -> None:
        """连接到命名管道。"""
        if self._connected and self._handle is not None:
            return
        if not _HAS_PYWIN32:
            raise RuntimeError(
                "pywin32 未安装。请运行: pip install pywin32"
            )

        def _do_connect():
            handle = win32file.CreateFile(
                self.pipe_name,
                win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                0,            # 独占
                None,         # 默认安全属性
                win32file.OPEN_EXISTING,
                0,            # 同步(非 overlapped)
                None,
            )
            # 确保管道处于字节模式
            win32pipe.SetNamedPipeHandleState(
                handle, win32pipe.PIPE_READMODE_BYTE, None, None
            )
            return handle

        try:
            self._handle = await asyncio.wait_for(
                asyncio.to_thread(_do_connect), timeout=timeout
            )
            self._connected = True
            logger.info("已连接桥接管道: %s", self.pipe_name)
        except pywintypes.error as e:
            self._connected = False
            raise ConnectionError(
                f"无法连接桥接管道({e.winerror}): {e.strerror}"
            ) from e
        except asyncio.TimeoutError:
            raise ConnectionError(
                f"连接桥接超时: {self.pipe_name}"
            )

    async def disconnect(self) -> None:
        """关闭管道连接。"""
        if self._handle is not None and _HAS_PYWIN32:
            try:
                win32file.CloseHandle(self._handle)
            except Exception:
                pass
        self._handle = None
        self._connected = False
        logger.info("已断开桥接管道")

    async def reconnect(self) -> None:
        """断开后重新连接。"""
        await self.disconnect()
        await self.connect()

    @property
    def is_connected(self) -> bool:
        return self._connected

    # ── 工具调用 ──────────────────────────────────────────

    async def call(
        self,
        tool: str,
        arguments: dict | None = None,
        timeout: float = 60.0,
    ) -> dict:
        """发送工具调用到 C++ 桥接,返回 result 字典。

        Raises:
            BridgeError: 桥接返回错误
            ConnectionError: 管道断开
        """
        arguments = arguments or {}
        async with self._lock:
            if not self._connected:
                await self.connect()

            request = {
                "id": str(uuid.uuid4()),
                "tool": tool,
                "arguments": arguments,
            }
            try:
                resp = await asyncio.wait_for(
                    asyncio.to_thread(self._send_recv, request),
                    timeout=timeout,
                )
            except pywintypes.error as e:
                self._connected = False
                logger.warning("管道 I/O 错误: %s", e)
                raise ConnectionError(f"桥接管道错误: {e}") from e
            except asyncio.TimeoutError:
                raise

            if not resp.get("ok", False):
                err = resp.get("error") or {}
                raise BridgeError(
                    code=err.get("code", -32603),
                    message=err.get("message", "未知错误"),
                    data=err.get("data"),
                )
            return resp.get("result", {})

    async def call_text(
        self,
        tool: str,
        arguments: dict | None = None,
        timeout: float = 60.0,
    ) -> str:
        """调用桥接工具并以字符串形式返回结果。"""
        result = await self.call(tool, arguments, timeout)
        if isinstance(result, str):
            return result
        if isinstance(result, dict):
            if "text" in result:
                return result["text"]
            if "content" in result:
                return result["content"]
            return json.dumps(result, ensure_ascii=False, indent=2)
        return str(result)

    async def health_check(self) -> dict | None:
        """检查桥接是否存活。返回健康信息或 None。"""
        try:
            return await asyncio.wait_for(
                self.call("__health__", {}, timeout=3.0), timeout=5.0
            )
        except Exception:
            return None

    # ── 底层 I/O(在线程中调用) ─────────────────────────

    def _send_recv(self, request: dict) -> dict:
        """发送请求,接收响应(阻塞,在 to_thread 中调用)。"""
        payload = json.dumps(request, ensure_ascii=False).encode("utf-8")
        header = struct.pack(HEADER_FORMAT, len(payload))

        # 发送 header + payload
        win32file.WriteFile(self._handle, header + payload)

        # 接收响应
        return self._recv_json()

    def _recv_json(self) -> dict:
        """接收长度前缀 JSON 消息。"""
        header = self._read_exact(HEADER_SIZE)
        length = struct.unpack(HEADER_FORMAT, header)[0]
        payload = self._read_exact(length)
        return json.loads(payload.decode("utf-8"))

    def _read_exact(self, n: int) -> bytes:
        """从管道精确读取 n 字节。"""
        data = b""
        while len(data) < n:
            remaining = n - len(data)
            chunk_size = min(remaining, RECV_CHUNK)
            _, chunk = win32file.ReadFile(self._handle, chunk_size)
            if not chunk:
                raise ConnectionError("管道被对端关闭")
            data += chunk
        return data
