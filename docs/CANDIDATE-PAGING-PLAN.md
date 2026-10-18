# 候选框翻页方案（SimpleIME 自绘候选窗）

> 状态：方案 v2（已合并 TSF/IMM32 调研结论；尚未实现）
> 适用：TSF（TextStore）与 IMM32 两条候选路径共用的 ImGui 候选窗

## 0. TL;DR

- **目标**：候选窗支持左右箭头按钮 / PageUp / PageDown 翻页，能选中当前页之外的候选。
- **首选（推荐先做）**：**把翻页键转发给 TIP**——`ITfIntegratableCandidateListUIElement::OnKeyDown` 优先，`ITfKeystrokeMgr::KeyDown` 备选。TIP 翻页后发 `TF_CLUIE_CURRENTPAGE`，现有 `DoUpdateUIElement` 已会全量重读 → UI 自动刷新。改动集中在：UI 按钮 + 转发逻辑 + 提交索引修复。
- **回退**：**本地分页**——IMM32 已确认可读整表（`dwCount`=总数、`dwPageStart`=当前页首、`dwPageSize`=每页条数）；TSF 取决于 TIP 的 `GetCount` 是否只报当前页（需实测，若只报一页则本地翻页无解）。
- **先做诊断实验**（§6）确认 TIP 行为，再决定实现哪条路径。

## 1. 现状与根因（代码事实）

### 1.1 数据只有当前页

- **TSF**：`src/tsf/TextStore.cpp` 的 `DoUpdateUIElement()`（:1262-1385）通过 `GetCandInfo()`（:1216-1260）取 `pageStart/pageEnd`，只对 `[pageStart, pageEnd)` 调 `GetString()` 填充 `CandidateUi::m_candidateList`。`CandidateUi`（`include/ime/CandidateUi.h`）只有 `m_dwFirstIndex/m_dwSelection`，没有 pageCount/currentPage/totalCount。
- **IMM32**：`src/ime/Imm32TextService.cpp` 的 `DoUpdateCandidateList()`（:416-442）只取 `dwPageStart .. min(dwPageStart+dwPageSize, dwCount)`。

### 1.2 没有翻页入口

- `src/ui/ImeWindow.cpp` 的 `DrawCandidates()`（:150-199）与 `DrawVerticalCandidates()`（:201-254）只画候选 pill，无翻页控件；`CommitClickedCandidate()`（:137-148）只处理点击。
- `src/ImeWnd.cpp` 的 `HandleKeyDown()`（:701-724）只处理 `VK_SHIFT` 与 Prisma 转发，不处理 `VK_PRIOR/VK_NEXT`。
- 没有任何地方触发 TIP 翻页（TSF 无"应用侧下一页"API；`ITfCandidateListUIElement::SetPageIndex` 是声明页表，不是翻页命令）。

### 1.3 已有的翻页基础设施（半成品）

- `CandidateUi::m_dwFirstIndex`（页首全局索引）已由 TSF 在 `DoUpdateUIElement` 里 `SetFirstIndex(info.pageStart)`。
- `DoUpdateUIElement` 已有注释（:1318-1323）：`// Selection is a global candidate index; the UI list is page-relative.`
- TSF 的 `GetCandInfo` 已经读了 `GetPageIndex`/`GetCurrentPage`（pageCount/currentPage 都在手边），只是没暴露到 UI。
- `TF_CLUIE_CURRENTPAGE`（TIP 翻页通知）会走 `DoUpdateUIElement` 全量重读 → **理论上 TIP 自己翻页时 UI 会刷新，只缺"把翻页键交给 TIP"这一环**。

### 1.4 已存在的隐患（翻页后会暴露）

- **提交索引**：UI 传的是页内 index（`ImeWindow.cpp` 的 `clicked`），TSF `TextStore::CommitCandidate(UINT index)`（:1191-1198）直接 `SetSelection(index)`——翻到第 2 页点第 2 个候选会选中全局第 2 个而不是第 11 个。IMM32 `Imm32TextService::CommitCandidate`（:220-232）同样直接用页内 index 调 `NI_SELECTCANDIDATESTR`。翻页功能落地前必须修。
- `docs/FIX-HANDOFF-2026-10-07.md:197` 已记录：`FirstIndex` 全仓唯一读者是翻页判定，提交路径无人使用。

## 2. 技术事实（调研结论）

| 问题 | 结论 | 来源 |
|---|---|---|
| `ITfCandidateListUIElement::SetPageIndex` 能翻页吗 | **不能**。语义=TIP 把页表（每页起始索引数组）声明给应用，方向是 TIP→应用，不是应用下发翻页命令；不改选中、不发通知。UILess 文档进一步说明：应用收到 `TF_CLUIE_STRING` 后**自行计算分页**，页表以 TIP 声明的为准 | [MSDN SetPageIndex](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfcandidatelistuielement-setpageindex)、[UILess 文档](https://learn.microsoft.com/en-us/windows/win32/tsf/uiless-mode-overview) |
| 应用侧有"下一页"API 吗 | **没有**。正规做法：`TestKeyDown`→`KeyDown`(VK_NEXT/VK_PRIOR)，`pfEaten=TRUE` 才吞键；TIP 翻页后发 `TF_CLUIE_CURRENTPAGE`，应用在回调重取 `GetCurrentPage/GetPageIndex` 重绘 | [ITfKeystrokeMgr::KeyDown](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfkeystrokemgr-keydown) |
| 有专为自绘候选宿主设计的接口吗 | **有**：`ITfIntegratableCandidateListUIElement::OnKeyDown(wParam, lParam, &eaten)`（Win8+），文档举例 VK_UP/VK_DOWN。TIP 若实现，应用可直接问"这个键你要不要处理" | [OnKeyDown](https://learn.microsoft.com/en-us/windows/win32/api/ctffunc/nf-ctffunc-itfintegratablecandidatelistuielement-onkeydown) |
| 微软拼音在 uiless 下会激活吗 | **会**。Win11 26300 注册表实测：微软拼音 CLSID `{81D4E9C9-1D3B-41BC-9E6C-4B40BF79E35E}` 注册了 `GUID_TFCAT_TIPCAP_UIELEMENTENABLED={49D2F9CF-1F5E-11D7-A6D3-00065B84435C}` | [预定义类别](https://learn.microsoft.com/en-us/windows/win32/tsf/predefined-category-values) |
| 微软拼音收到 VK_NEXT 会翻页吗 | **无文档，只能实测**。uiless 下 TIP 候选窗不显示，窗上 ‹ › 随之消失 | — |
| IMM32 候选列表是整表还是当前页 | **整表**：`ImmGetCandidateListW(hIMC, dwIndex, …)` 的 dwIndex 是候选列表编号（0..3），返回 `dwCount`=总数、`dwPageStart`=当前页首候选下标、`dwPageSize`=每页条数 | [ImmGetCandidateListW](https://learn.microsoft.com/en-us/windows/win32/api/immdev/nf-immdev-immgetcandidatelistw)、[CANDIDATELIST](https://learn.microsoft.com/en-us/windows/win32/api/imm/ns-imm-candidatellist) |
| IMM32 提交索引语义 | `NI_SELECTCANDIDATESTR` 的 dwValue 是**整表下标**，第 2 页第 k 个 = `dwPageStart + k`；更稳是先 `NI_SETCANDIDATE_PAGESTART` 同步页起点再选 | [ImmNotifyIME](https://learn.microsoft.com/en-us/windows/win32/api/immdev/nf-immdev-immnotifyime) |
| 开源参考 | SDL2 `SDL_windowskeyboard.c` 的 `UILess_GetCandidateList()` 是最完整的应用侧取页样本（但不翻页）；rime/weasel、metasequoiaime、corvusskk 均为 TIP 侧实现。**未找到"应用侧自绘+本地翻页"的成熟实现** | — |

### 2.1 本项目约束（不可违反）

- **禁用 `ITfMessagePump`**：`src/ImeWnd.cpp:269-302` 长注释——Skyrim 不向 mod 暴露键盘；微软拼音 + `ITfMessagePump` + `ITfKeystrokeMgr` 组合有 KB4564002 的嵌套消息循环冻结 bug（Shift 组字时），"Do not reintroduce the TSF message pump here."
- `ITfKeystrokeMgr` 已 QI 但全项目未使用（`src/tsf/TsfSupport.cpp:42-43`），单独 `KeyDown` 与 messagepump 是两回事，但 mspy 的 key sink 行为必须实测。
- 游戏内被 TIP 消费的键 ImeWnd 收不到（`HandleKeyDown` 只收到 TIP 没吃的键）；按钮是唯一可靠入口。

## 3. 方案

### 3.1 主方案：转发翻页键给 TIP（推荐先做）

**原理**：按钮/键盘 → `VK_NEXT`/`VK_PRIOR` 交给 TIP → TIP 自己翻页 → 发 `TF_CLUIE_CURRENTPAGE` → 现有 `DoUpdateUIElement` 全量重读 → UI 显示新页。与输入法原生行为完全一致，不改变候选数据的所有权。

#### 3.1.1 接口层

- `include/ime/ITextService.h`：新增 `virtual auto ChangeCandidatePage(int delta) -> bool = 0;`（或非纯虚默认 false）。
- `include/ime/ImeController.h/.cpp`：仿 `CommitCandidate`（:85-98）：
  ```cpp
  auto ImeController::ChangeCandidatePage(int delta) const -> void
  {
      if (!IsReady()) return;
      PostToImeThread([this, delta] {
          ImeWnd *imeWnd = m_imeWnd.load(std::memory_order_acquire);
          if (IsModEnabled() && imeWnd != nullptr) imeWnd->ChangeCandidatePage(delta);
      });
  }
  ```
- `src/ImeWnd.cpp`：`ImeWnd::ChangeCandidatePage(int delta)` → `m_textService->ChangeCandidatePage(delta)`；`HandleKeyDown()` 开头加：
  ```cpp
  if ((wParam == VK_PRIOR || wParam == VK_NEXT) &&
      State::GetInstance().HasAny(State::IN_CAND_CHOOSING)) {
      m_textService->ChangeCandidatePage(wParam == VK_NEXT ? 1 : -1);
      return;
  }
  ```
  （若 TIP 已消费这两个键，ImeWnd 收不到，但 TIP 已自行翻页，UI 仍会刷新——两条路殊途同归。）

#### 3.1.2 TSF 实现（src/tsf/TextStore.cpp）

```cpp
auto TextService::ChangeCandidatePage(int delta) -> bool
{
    if (m_currentCandidateUi == nullptr) return false;
    const UINT vk = delta > 0 ? VK_NEXT : VK_PRIOR;
    const LPARAM lParam = 0; // TIP 一般不依赖 lParam 的扫描码
    // 1) 首选：自绘候选宿主专用接口
    CComPtr<ITfIntegratableCandidateListUIElement> integrable;
    if (SUCCEEDED(m_currentCandidateUi->QueryInterface(IID_PPV_ARGS(&integrable))))
    {
        BOOL eaten = FALSE;
        if (SUCCEEDED(integrable->OnKeyDown(vk, lParam, &eaten)) && eaten) return true;
    }
    // 2) 备选：ITfKeystrokeMgr（只测试+转发，不引入 messagepump）
    if (auto *ksm = TsfSupport::GetInstance().GetKeystrokeMgr(); ksm != nullptr)
    {
        BOOL eaten = FALSE;
        if (SUCCEEDED(ksm->TestKeyDown(vk, lParam, &eaten)) && eaten)
        {
            ksm->KeyDown(vk, lParam, &eaten);
            return eaten != FALSE;
        }
    }
    return false; // 两条路都不吃键 → 回退本地分页（§3.2）
}
```

> 注意：`OnKeyDown`/`KeyDown` 必须在 IME 线程调用（`PostToImeThread` 保证）；返回 false 时不要吞掉原始按键。

#### 3.1.3 IMM32 实现（src/ime/Imm32TextService.cpp）

IMM32 是"应用驱动"模型，不转发 WM_KEYDOWN，直接通知 IME：

```cpp
auto Imm32TextService::ChangeCandidatePage(int delta) -> bool
{
    // 读整表拿 dwPageStart/dwPageSize（DoUpdateCandidateList 已缓存的话直接用）
    CANDIDATELIST *list = ...; // ImmGetCandidateListW(hIMC, 0, ...)
    const DWORD pageSize = list->dwPageSize;
    const DWORD total    = list->dwCount;
    const DWORD curStart = list->dwPageStart;
    const DWORD newStart = delta > 0 ? std::min(curStart + pageSize, total - 1)
                                     : (curStart >= pageSize ? curStart - pageSize : 0);
    if (newStart == curStart) return false;
    if (!ImmNotifyIME(m_hIMC, NI_SETCANDIDATE_PAGESTART, 0, newStart)) return false;
    // IME 会随后发 WM_IME_NOTIFY / 候选更新；本地直接按 newStart 重读整表
    UpdateCandidateListFromIme(); // 复用 DoUpdateCandidateList 的读取路径
    return true;
}
```

> 微软拼音实测坑：不先屏蔽候选窗时 `ImmGetCandidateList` 返回 0（社区博客）；若整表读不到，退化为"本地分页显示 + 全局下标提交"（数据源仍缺，只能转发 `WM_KEYDOWN` 给 IME 窗口默认过程——不可靠，需实测）。

#### 3.1.4 UI 按钮（见 §5）

按钮点击 → `ImeController::ChangeCandidatePage(±1)`；返回 false 时（TIP 不吃键）回退本地分页（§3.2）。

### 3.2 回退方案：本地分页（display-only paging）

仅当 §6 诊断证明"TIP 不响应转发键"时启用。

- **IMM32**：已确认整表可读 → 本地维护 `currentPage`，UI 显示 `dwPageStart..dwPageStart+dwPageSize` 之外的页（从整表切片），点击候选用**整表下标**（`pageStart + localIndex`）调 `NI_SELECTCANDIDATESTR`；翻页时先 `NI_SETCANDIDATE_PAGESTART` 同步 IME 再选。
- **TSF**：`GetString` 能否读非当前页取决于 TIP。若 `GetCount` 恒等于每页条数（TIP 只报当前页），本地翻页**无解**，只能转发键（§3.1）或放弃 TSF 路径的按钮（仅键盘交给 TIP）。
- 本地页状态规则：
  - TIP 发 `TF_CLUIE_CURRENTPAGE` → 本地 currentPage 跟随 TIP；
  - TIP 发 `TF_CLUIE_CANDIDATELIST`（候选集变）→ 本地 currentPage 重置；
  - 用户本地翻页 → 只改本地显示页，不通知 TIP。

### 3.3 数据模型（本地分页路径才需要）

`include/ime/CandidateUi.h` 增加：

```cpp
struct PageState {
    std::vector<DWORD> pageStarts;  // 每页第一个候选的全局索引
    DWORD pageSize    = 0;          // 每页条数
    DWORD currentPage = 0;          // 当前显示页（本地）
    DWORD pageCount   = 0;
    DWORD totalCount  = 0;
};
```

方法：`SetPageState` / `CurrentPage` / `PageCount` / `TotalCount` / `SetCurrentPage` / `GlobalIndex(local)` = `pageStarts[currentPage] + local`；`Close()` 重置 `m_page`。

## 4. 提交索引修复（必须，两条路径都要）

- **TSF**：`TextStore::CommitCandidate(UINT index)`（:1191-1198）改为
  `m_currentCandidateUi->SetSelection(m_candidateUi.FirstIndex() + index); m_currentCandidateUi->Finalize();`
  （`SetSelection` 参数是全局索引，见 :1318 注释）。
- **IMM32**：`Imm32TextService::CommitCandidate(DWORD index)`（:220-232）改为
  `ImmNotifyIME(m_hIMC, NI_SELECTCANDIDATESTR, 0, firstIndex + index)`（整表下标）。
- UI 层保持页内语义，由 TextService 统一加 `FirstIndex`。

## 5. UI 设计（src/ui/ImeWindow.cpp）

### 5.1 水平候选行（DrawCandidates :150-199）

候选 pill 画完后 `ImGui::SameLine(0.F, gap)` 追加翻页组：

```
[1 的] [2 都] … [9 答]   ‹  1/5  ›
```

- `ImGuiEx::M3::XSmallIconButton(ICON_CHEVRON_LEFT, ImGuiEx::M3::Spec::IconButtonColors::Standard)`（首页禁用）
- 页码 `std::format("{}/{}", currentPage + 1, pageCount)`（`onSurfaceVariant`，小字，居中垂直对齐 pill）
- `XSmallIconButton(ICON_CHEVRON_RIGHT, ...)`（末页禁用）
- `pageCount <= 1` 时整组不画（单页零侵入）。
- 点击 → `ImeController::GetInstance()->ChangeCandidatePage(±1)`。

### 5.2 垂直候选列表（DrawVerticalCandidates :201-254）

列表底部追加一行同样内容（左对齐，行高复用 44dp 或小一号）：

```
‹  1/5  ›
```

### 5.3 图标

- **推荐**：`assets/lucide/lucide-icons.txt` 追加 `chevron-left`、`chevron-right`（当前 25 个图标，U+E000..U+E018），重新生成图标字体；`include/icons.h` 追加
  `ICON_CHEVRON_LEFT = ImGuiEx::icon<0xE019>`、`ICON_CHEVRON_RIGHT = ImGuiEx::icon<0xE01A>`（编号按生成结果）。
- **省事**：`ImGui::InvisibleButton` + `drawList->AddTriangleFilled` 自绘 ‹ ›（不依赖图标字体）。

### 5.4 交互细节

- 翻页不改变 TIP 的选中项（只换显示页）；选中 pill 仍按 `CandidateUi::Selection()` 高亮。
- 用户点候选 → 提交全局索引（§4）。
- 输入新字母 → TIP 重置候选 → `DoUpdateUIElement` 的 CANDIDATELIST 路径 → 本地页重置。

## 6. 先做的诊断实验（30 秒，决定分支）

1. **TIP 是否支持转发键**（TSF）：
   - 在 `TextService::ChangeCandidatePage` 临时加日志，输出 QI `ITfIntegratableCandidateListUIElement` 是否成功、`OnKeyDown(VK_NEXT)` 的 `eaten` 值、`ITfKeystrokeMgr::TestKeyDown` 的 `eaten` 值。
   - 游戏内输入 `d` → 按 PageDown（或临时绑一个测试热键调 `ChangeCandidatePage(1)`）→ 看：
     - `OnUIUpdated` 是否收到 `TF_CLUIE_CURRENTPAGE`；
     - `GetCurrentPage()` 是否变化；
     - 候选框是否刷新出下一批。
2. **TIP 是否只报一页**（本地翻页可行性）：
   - 日志输出 `GetCount()` 与 `GetPageIndex` 的 pageCount；若 `GetCount() == 每页条数`，本地翻页无解。
3. **IMM32 整表**：
   - 日志输出 `ImmGetCandidateListW` 的 `dwCount/dwPageStart/dwPageSize`；确认 `dwCount > dwPageSize`。
4. 根据结果选择：
   - TIP 吃键 + CURRENTPAGE → 只做 §3.1 + §4 + §5（改动最小）。
   - TIP 不吃键 + `GetCount` 全量 → §3.2 本地分页（TSF）。
   - TIP 不吃键 + `GetCount` 只报一页 → TSF 路径只能放弃按钮，保留键盘交 TIP；IMM32 路径做本地分页。

## 7. 验证清单

- 水平/垂直两种布局：首页/中间/末页按钮状态正确；单页隐藏。
- 点击第 2 页第 2 个候选，提交的汉字正确（全局索引）。
- 翻页后继续输入字母 → 候选重置到第 0 页。
- 翻页后 TIP 自己更新（selection 变化）→ 本地页不被误重置。
- 游戏控制台/物品栏/Prisma 面板各输入一次。
- TSF 与 IMM32 两条路径（若系统支持切换）。
- 构建：`cmake --preset <现有 preset>` → 编译通过；无新增 warning。

## 8. 风险与边界

- **mspy 嵌套循环 bug**：`ITfMessagePump` 的已知坑；`ITfKeystrokeMgr::KeyDown` 单独调用理论上不同，但 mspy 的 key sink 若内部有 nested loop 仍可能冻结——必须实测，失败则回退本地分页。
- **TIP 不响应转发键**：TSF 若 `GetCount` 只报当前页，本地翻页无解 → 按钮只能做"提示"或放弃 TSF 按钮。
- **uiless 下 TIP 候选窗的 ‹ › 消失**：我们自绘的按钮正是替代品。
- **提交索引**：不修则翻页后点选错候选（§4）。
- 不要碰 `ITfMessagePump`（`src/ImeWnd.cpp:269-302` 硬约束）。
- 不要用 `ITfCandidateListUIElement::SetPageIndex` 翻页（语义是声明页表）。

## 9. 实现状态（第一版已落地，2026-10-18）

已实现并通过编译（`cmake --build build/RelWithDebInfo-clangcl-ninja-vcpkg`，`SimpleIME.dll` 已链接，无新增 warning）：

| 层 | 文件 | 内容 |
| --- | --- | --- |
| 数据 | `include/ime/CandidateUi.h` | 新增 `SetPageInfo/PageSize/CurrentPage/PageCount/TotalCount`；`Close()`/`swap()` 同步重置/交换 |
| 接口 | `include/ime/ITextService.h` | `virtual ChangeCandidatePage(int delta)`（默认 false）；Imm32 后端 override |
| TSF | `include/tsf/TextStore.h`、`src/tsf/TextStore.cpp` | `CandidateInfo` 增加 `pageCount/currentPage/totalCount`；`GetCandInfo` 填充；`DoUpdateUIElement` 写 `SetPageInfo`；`CommitCandidate` 加 `FirstIndex`（修页内/全局索引）；`ChangeCandidatePage` = `ITfIntegratableCandidateListUIElement::OnKeyDown` → `ITfKeystrokeMgr::TestKeyDown/KeyDown` |
| IMM32 | `src/ime/Imm32TextService.cpp` | `DoUpdateCandidateList` 写 `SetFirstIndex`+`SetPageInfo`；`CommitCandidate` 加 `FirstIndex`；`ChangeCandidatePage` = 读整表算新页起点 → `NI_SETCANDIDATE_PAGESTART`，成功后主动重读 |
| 调度 | `include/ime/ImeController.h`、`src/ime/ImeController.cpp` | `ChangeCandidatePage` → `PostToImeThread` → `ImeWnd::ChangeCandidatePage` |
| 键盘 | `src/ImeWnd.cpp` | `HandleKeyDown` 在 `IN_CAND_CHOOSING` 时拦截 `VK_PRIOR/VK_NEXT` 并转发给 IME（TIP 已吃掉的键不会到达这里） |
| UI | `src/ui/ImeWindow.cpp` | 水平候选行尾部 / 垂直列表底部新增 `‹ n/N ›` 控件（自绘三角，单页自动隐藏）；点击 → `ImeController::ChangeCandidatePage(±1)` |

**待实测**（对应 §6）：微软拼音是否响应 `OnKeyDown(VK_NEXT)` / `ITfKeystrokeMgr`；翻页后是否收到 `TF_CLUIE_CURRENTPAGE`；IMM32 的 `dwCount/dwPageStart/dwPageSize` 语义。诊断日志已加在 `TextStore::ChangeCandidatePage`（debug 级，`vk` + 转发路径）。

**已知边界**：TSF 路径若 TIP 不吃转发键，按钮点击无效果（仅打日志）；IMM32 路径若 IME 不支持 `NI_SETCANDIDATE_PAGESTART`，同样无效果。两条路径都失败时，下一步才是 §3.2 的本地分页（需要先确认 TIP `GetCount` 是否只报当前页）。

### 9.1 2026-10-18 第一轮实测后的调整

- **上下键导航改为本地实现**：`ImeWnd::Run` 是纯 PeekMessage 循环（禁用 `ITfMessagePump`，见 `src/ImeWnd.cpp:269-302`），TIP 根本收不到方向键。新增 `ITextService::MoveCandidateSelection(int delta)`（TSF/Imm32 各自在缓存 `CandidateUi` 上移动 `Selection` 并 `MarkDirty(CandidateSelection)`），`ImeWnd::HandleKeyDown` 在 `IN_CAND_CHOOSING` 时拦截 `VK_UP/VK_DOWN`（↑=左移、↓=右移）调用它；`VK_LEFT/VK_RIGHT` 完全放行给 TIP 移动拼音光标（2026-10-18 实测确认的键位分工）。
- **移除页码**：用户要求去掉 `1/3`，`DrawCandidatePageControl` 只保留 `◀ ▶` 两个箭头。
- **空格提交高亮项**：`ITextService::CommitSelectedCandidate()`（TSF/Imm32 各自读缓存 `CandidateUi::Selection()` 后走 `CommitCandidate`），`ImeWnd::HandleKeyDown` 在 `IN_CAND_CHOOSING` 时拦截 `VK_SPACE`。空格不再依赖 TIP（TIP 收不到键盘），提交回声由既有 `s_commitEchoSink` 机制丢弃。
- Enter 仍未拦截（游戏控制台里 Enter 是执行命令，先不改）。

### 9.2 2026-10-18 Review 修复（d74c4d9f）

- **[P1] Imm32 末页/单页 PageDown**：`newStart = std::min(pageStart + pageSize, total - 1)` 改为仅在 `pageStart + pageSize < total` 时前进，否则保持页起点（`src/ime/Imm32TextService.cpp`）。单页列表与末页不再把 `dwPageStart` 设成最后一个候选下标。
- **[P2] 禁用箭头仍派发翻页**：`DrawCandidatePageControl` 的左/右按钮点击现在分别要求 `currentPage > 0` / `currentPage + 1 < pageCount` 才设置 delta（`src/ui/ImeWindow.cpp`）。
- **[P2] Enter 提交旧选择**：`ImeWnd::HandleKeyDown` 新增 `VK_RETURN` 拦截（`IN_CAND_CHOOSING` 时 → `CommitSelectedCandidate()`），与 Space 一致；`ITextService.h` 注释同步为 click / Space / Enter。
- **[P3] 页码**：按用户 2026-10-18 的明确要求（"移除 1/3 显示页数"）不恢复页码，仅把 `DrawCandidatePageControl` 注释从 `"‹ 1/5 ›"` 改为 `"‹ ›"`。
