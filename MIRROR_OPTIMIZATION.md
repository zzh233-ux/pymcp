# 镜像增量优化方案

## 问题分析

当前 `WorkspaceMirror` 的缓存策略是**写入后全量失效**:

```
edit_file/write_file → InvalidateMirror() → g_state.valid = false
↓
下一次 read_file/search_code/list_files → EnsureMirrorFresh()
↓
RebuildMirrorLocked():
  1. 清理旧镜像目录
  2. 导出 IDE 内存到 .e 快照文件(序列化整个工程)
  3. 启动 e-packager.exe 子进程解包(WaitForSingleObject(INFINITE))
  4. 解析 _meta.json 重建映射表
  5. 标记 valid = true
```

正常耗时 1-5 秒,云同步盘下可达几十秒。如果 AI 的工作模式是"编辑→读取→编辑→读取"交替,每次读取都触发完整重建。

根因在 `WorkspaceMirror.cpp:589-593`:
```cpp
void InvalidateMirror() {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_state.valid = false;  // ← 整个镜像失效,包括元数据映射表
}
```

## 优化 1: 增量更新(最高优先级)

**思路**: 写入操作后,不失效整个镜像,而是直接更新镜像中被修改的那一个文件。元数据映射表(`itemByRelativePath`)不变,因为源码编辑不改变工程结构。

### WorkspaceMirror.h 新增接口

```cpp
// 增量更新镜像中的单个文件(写入后不触发重建)
void UpdateMirrorFile(const std::string& relativePathUtf8,
                      const std::string& contentUtf8);
```

### WorkspaceMirror.cpp 实现

```cpp
void UpdateMirrorFile(const std::string& relativePathUtf8,
                      const std::string& contentUtf8) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_state.valid || g_state.mirrorRoot.empty()) {
        return;  // 镜像未构建,无需更新
    }
    namespace fs = std::filesystem;
    fs::path fullPath = fs::path(g_state.mirrorRoot) / relativePathUtf8;
    std::ofstream file(fullPath, std::ios::binary | std::ios::trunc);
    if (file.is_open()) {
        file.write(contentUtf8.data(), contentUtf8.size());
        file.close();
    }
}
```

### 调用点修改(AIChatToolingX86.cpp)

在 `ExecuteFileMappedRealPageToolForAI()` 写入成功后的逻辑中:

```cpp
// 修改前:
WorkspaceMirror::InvalidateMirror();

// 修改后(增量更新):
WorkspaceMirror::UpdateMirrorFile(relativePath, newCode);
// 仅在结构变化时(新增/删除程序项)才全量失效
// WorkspaceMirror::InvalidateMirror();  // 只在结构变化时调用
```

**预期效果**: 消除 90% 以上的 e-packager 子进程调用。AI 的"编辑→读取"交替不再触发重建。

---

## 优化 2: 源码文件直读 IDE

**思路**: `read_file` 对源码文件(映射到程序项的文件)直接从 IDE 真实页面读取,跳过镜像磁盘文件。这样始终返回最新代码,且不需要镜像重建。

### WorkspaceFileTools.cpp 修改

在 `ExecuteReadFile()` 中,解析到程序项后优先走 IDE:

```cpp
bool ExecuteReadFile(const std::string& filePath, std::string& outContent) {
    // 先尝试映射到程序项
    WorkspaceMirror::ProgramItemRef item;
    std::string err;
    if (WorkspaceMirror::ResolveFileToProgramItem(filePath, item, err)) {
        // 源码文件:直接从 IDE 真实页面读取
        std::string realCode;
        if (TryReadRealPageCodeForAI(item.pageNameLocal, item.kind, realCode)) {
            outContent = FormatWithLineNumbers(realCode);
            return true;
        }
        // 失败则回退到镜像文件
    }

    // 非源码文件或 IDE 读取失败:从镜像磁盘读取
    std::filesystem::path fullPath;
    if (!WorkspaceMirror::ResolveFilePath(filePath, fullPath, /*...*/)) {
        return false;
    }
    // ... 原有磁盘读取逻辑 ...
}
```

**注意**: `TryReadRealPageCodeForAI` 在主线程执行(通过 SendMessage),因此这个优化要求 `read_file` 已在主线程上下文中。当前 `read_file` 已经通过 `RequestToolExecutionFromMainThread` 派发到主线程,所以条件满足。

**预期效果**: `read_file` 不再依赖镜像磁盘文件,始终返回 IDE 最新代码。即使镜像过期也不影响读取。

---

## 优化 3: 元数据与内容分离失效

**思路**: 引入 `contentStale` 标志,区分"内容过期"和"结构过期"。内容过期只影响文件内容读取,不影响元数据映射表的有效性。

### MirrorState 新增字段

```cpp
struct MirrorState {
    // ... 现有字段 ...
    bool contentStale = false;  // 内容过期但元数据有效
};
```

### InvalidateMirror 改为两个版本

```cpp
// 轻量失效:仅标记内容过期,保留元数据
void InvalidateMirrorContent() {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_state.contentStale = true;
    // g_state.valid 保持 true
    // itemByRelativePath 不清空
}

// 全量失效:结构变化时使用(新增/删除程序项)
void InvalidateMirror() {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_state.valid = false;
    g_state.contentStale = false;
}
```

### EnsureMirrorFreshLocked 检查调整

```cpp
bool EnsureMirrorFreshLocked(std::string& outError) {
    // ... 现有路径检查 ...
    if (g_state.valid && !g_state.mirrorRoot.empty()
        && IsSamePath(g_state.sourcePath, sourcePath)
        && !g_state.itemByRelativePath.empty()) {

        if (std::filesystem::exists(g_state.mirrorRoot, ec) &&
            std::filesystem::is_directory(g_state.mirrorRoot, ec)) {

            // 新增:内容过期但元数据有效时,不重建
            if (g_state.contentStale) {
                // 元数据仍可用,read_file 会走 IDE 直读(优化2)
                // search_code/list_files 可以用旧镜像 + contentStale 标记
                return true;
            }
            return true;  // 完全有效
        }
        g_state.valid = false;
    }
    // ... 重建逻辑 ...
}
```

**预期效果**: 写入后只标记内容过期,`search_code` 和 `list_files` 仍可使用旧镜像(配合优化1的增量更新,镜像文件已被更新),不触发重建。

---

## 优化 4: 异步重建

**思路**: 当确实需要全量重建时(如工程结构变化),在后台线程执行 e-packager 解包,不阻塞当前请求。

### 挑战

`RebuildMirrorLocked()` 包含两个步骤:
1. `BuildSnapshot()` → 序列化 IDE 内存到 .e 文件(**必须在主线程**)
2. `RunProcessAndCapture(e-packager.exe)` → 解包(**可在后台线程**)

### 方案

```cpp
static std::atomic<bool> g_rebuildInProgress{false};
static std::thread g_rebuildThread;

void RebuildMirrorAsync() {
    if (g_rebuildInProgress.exchange(true)) {
        return;  // 已在重建中
    }

    // 步骤1:主线程序列化快照(同步,~100ms)
    std::string snapshotPath;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        snapshotPath = BuildSnapshot();  // 主线程
    }

    // 步骤2:后台线程执行解包(异步,1-5s)
    g_rebuildThread = std::thread([snapshotPath]() {
        std::lock_guard<std::mutex> guard(g_mutex);
        try {
            // 运行 e-packager 解包
            RunEpackagerUnpack(snapshotPath, g_state.mirrorRoot);
            ParseMetadata();
            g_state.valid = true;
            g_state.contentStale = false;
        } catch (...) {
            // 重建失败,保持失效状态
        }
        g_rebuildInProgress.store(false);
    });
}
```

**注意**: 此优化需要确保 `g_mutex` 在后台线程解包期间不被长时间持有,否则会阻塞 `read_file` 等操作。更精细的实现应将解包步骤的锁范围缩小到仅保护状态更新部分。

**预期效果**: 重建期间用户仍可读取旧镜像数据(配合优化2的 IDE 直读,源码文件不受影响)。

---

## 优化 5: 镜像预热与持久化

**思路**: 将镜像目录和元数据持久化到磁盘,IDE 启动时预热(后台构建),避免首次读取时阻塞。

### 预热

在 `FneInit()` 中(已有 `预热工程源码缓存` 的调用点,`AutoLinker.cpp:427`),添加:

```cpp
// 后台预热镜像(不阻塞 IDE 启动)
std::thread([]() {
    Sleep(2000);  // 等 IDE 完全就绪
    std::string err;
    WorkspaceMirror::EnsureMirrorFresh(err);
}).detach();
```

### 持久化

将 `itemByRelativePath` 映射表序列化到 `_meta.json` 旁的 `_meta_cache.json`,下次启动时直接加载,跳过 e-packager 解包(如果 .e 文件未变化)。

---

## 实施优先级

| 优化 | 预期效果 | 实施难度 | 优先级 |
|------|---------|---------|--------|
| 1. 增量更新 | 消除 90% e-packager 调用 | 低(新增1个函数+改1处调用) | P0 |
| 2. 源码直读IDE | read_file 始终返回最新 | 中(改 WorkspaceFileTools) | P0 |
| 3. 分离失效 | search/list 不触发重建 | 中(改状态管理) | P1 |
| 4. 异步重建 | 重建不阻塞读取 | 高(跨线程同步) | P2 |
| 5. 预热持久化 | 首次读取不阻塞 | 中 | P2 |

建议先实施 P0(优化1+2),这两个改动最小且效果最显著。P1 在 P0 验证后实施。P2 视实际效果决定是否需要。
