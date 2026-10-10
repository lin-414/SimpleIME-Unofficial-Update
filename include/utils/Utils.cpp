//
// Created by jamie on 2026/3/11.
//
#include "Utils.h"

#include "ImeApp.h"
#include "RE/B/BSTDerivedCreator.h"
#include "RE/B/ButtonEvent.h"
#include "RE/B/BSInputEventQueue.h"
#include "RE/B/BSUIScaleformData.h"
#include "RE/C/CharEvent.h"
#include "RE/G/GFxEvent.h"
#include "RE/GFxCharEvent.h"
#include "RE/I/IMenu.h"
#include "RE/I/InterfaceStrings.h"
#include "RE/U/UI.h"
#include "RE/U/UIMessage.h"
#include "RE/U/UIMessageQueue.h"
#include "RE/ControlMap.h"
#include "core/State.h"
#include "hooks/MeridianBridge.h"
#include "hooks/PrismaBridge.h"
#include "hooks/SkseMenuFrameworkBridge.h"
#include "menu/MenuNames.h"
#include "utils/ImGuiHostChannel.h"
#include "utils/InputFocusAnchor.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <windows.h>

namespace Ime::Skyrim
{
namespace
{
using ScaleformMessageCreator = RE::BSTDerivedCreator<RE::BSUIScaleformData, RE::IUIMessageData>;

/// 提交文字到"原生 ImGui 界面"的待投队列。IME 线程只入队，游戏线程每帧投递：
/// 引擎的 charEvents 环每帧只有 5 槽（buttonEvents 10 槽，一次退格占按下+抬起），
/// 超槽的条目会被环复用覆盖，所以必须按帧分批。
struct PendingCommit
{
    std::wstring text;
    std::size_t  eraseLetters{}; ///< 组字期间被引擎原样读进字面的拼音字母数
};

std::mutex             g_commitMutex;
std::deque<PendingCommit> g_commitQueue;

/// 游戏线程写、IME 线程读：当前持有文本输入的是不是"没有 Scaleform 字段"的界面。
std::atomic_bool g_externalImGuiSurface{false};

// ---- 与界面方的握手（窗口属性，同进程，零协商成本）---------------------
// 宿主界面在初始化时挂上 ImeAware 属性，声明"外部输入法组字期间我自己丢弃原始
// 字母"；我们便不再退格补偿（那会删掉玩家真正的文字）。Composing 属性带最后一次
// 刷新的 tick，界面方按新鲜度判定——我们崩了它也会自动恢复收字母。
constexpr const wchar_t *COMPOSING_PROPERTY = L"SimpleIME.Composing";
constexpr const wchar_t *IME_AWARE_PROPERTY = L"SimpleIME.ImeAware";
std::atomic_bool         g_imeAwareHost{false};

auto PropertyAtom(const wchar_t *name) -> ATOM
{
    static std::mutex        atomMutex;
    static std::unordered_map<std::wstring, ATOM> atoms;
    const std::lock_guard    lock(atomMutex);
    auto                    &slot = atoms[name];
    if (slot == 0)
    {
        // 自己加原子，之后一律用 MAKEINTATOM 存取：SetProp 传字符串名会每次
        // GlobalAddAtom，按帧调用会把原子表填满。
        slot = GlobalAddAtomW(name);
    }
    return slot;
}

auto PropertyName(const wchar_t *name) -> LPCWSTR
{
    return reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(PropertyAtom(name)));
}

auto Send(RE::BSUIScaleformData *scaleformData, const uint32_t code, RE::UIMessageQueue *messageQueue, const RE::BSFixedString &menuName) -> bool
{
    auto *charEvent               = new GFxCharEvent(code);
    scaleformData->scaleformEvent = charEvent;

    logger::debug("send code {:#x} to Skyrim", code);
    messageQueue->AddMessage(menuName, RE::UI_MESSAGE_TYPE::kScaleformEvent, scaleformData);
    return true;
}
} // namespace

void SendUiString(std::wstring_view wstringView, std::size_t eraseLetters)
{
    if (wstringView.empty()) return;

    // While our own ToolWindow (the settings overlay) is showing, it is the
    // text target: the bridge routes below would steal its keystrokes into an
    // underlying mod view (PMCM search box, Meridian DOM field, SKSEMF ImGui
    // field). Fall through to the Scaleform fallback, which feeds ImeMenu's
    // ImGui via its own char events. The flag is written on the render thread
    // and read here on the IME thread; a one-frame staleness only misroutes a
    // keystroke across an open/close boundary, which is unobservable.
    const bool toolWindowShowing = ImeApp::GetInstance().GetSettings().runtimeData.toolWindowShowing;

    if (!toolWindowShowing)
    {
        // Meridian views (CEF) live outside the menu stack: GFx char events never
        // reach their DOM fields. While a Meridian view is focused, hand the text
        // to the bridge instead — it queues here (IME thread) and commits into
        // the focused DOM field from the game thread's frame tick.
        if (Hooks::MeridianBridge::ShouldRoute())
        {
            Hooks::MeridianBridge::QueueText(wstringView);
            return;
        }

        // Prisma views (Ultralight — PMCM, Outfit Wheeler, ...) live outside the
        // menu stack too, and their fields are fed exclusively by PrismaUI's
        // game-window subclass reading the WM_CHAR stream. While a Prisma view
        // owns input, post the committed text there: the subclass queues it into
        // the view (surrogate recombination included), Win32 focus notwithstanding.
        if (Hooks::PrismaBridge::ShouldRoute())
        {
            Hooks::PrismaBridge::QueueText(wstringView);
            return;
        }

        // SKSE Menu Framework fields are ImGui, not Scaleform: same routing idea,
        // but the injection happens inside the framework's own render-event
        // callback (the thread its ImGui frames run on).
        if (Hooks::SkseMenuFrameworkBridge::ShouldRoute())
        {
            Hooks::SkseMenuFrameworkBridge::QueueText(wstringView);
            return;
        }

        // A mod menu that draws with its own private Dear ImGui context (Tailor
        // 3.x, ModExplorerMenu ...) has no Scaleform field to write into and
        // exposes no bridge to inject through — but the two families read text
        // from disjoint places, so the probe asks the surface which one it
        // consumes. Hosts that forward Scaleform char events into their ImGui
        // keep the GFx route below; hosts that read the engine's input queue get
        // the commit as the engine's own CharEvents, with the pinyin letters
        // Skyrim already wrote into the field erased first (we cannot intercept
        // them at the source).
        if (ImeApp::GetInstance().GetSettings().input.imguiSurfaceInput &&
            g_externalImGuiSurface.load(std::memory_order_acquire) &&
            CurrentImGuiHostChannel() == ImGuiHostChannel::EngineEvents)
        {
            const std::wstring text(wstringView);
            // 界面自己丢弃了组字期的原始字母，就绝不能再去退格——那会删掉玩家
            // 真正写下的文字。
            const std::size_t erase = g_imeAwareHost.load(std::memory_order_acquire) ? 0 : eraseLetters;
            {
                const std::lock_guard lock(g_commitMutex);
                g_commitQueue.push_back(PendingCommit{text, erase});
            }
            logger::info(
                "Queued {} committed unit(s) and {} backspace(s) for an ImGui-family surface",
                text.size(),
                erase
            );
            return;
        }
    }

    auto             *messageQueue  = RE::UIMessageQueue::GetSingleton();
    const auto *const strings       = RE::InterfaceStrings::GetSingleton();
    auto             *msgFactoryMgr = RE::MessageDataFactoryManager::GetSingleton();
    if (strings == nullptr || messageQueue == nullptr || msgFactoryMgr == nullptr)
    {
        logger::warn("Can't send string to Skyrim. May game already closed?");
        return;
    }
    const auto *scaleformDataCreator = msgFactoryMgr->GetCreator<RE::BSUIScaleformData>(strings->bsUIScaleformData);
    if (scaleformDataCreator == nullptr)
    {
        logger::warn("Unexpected error. Can't create scaleform message data creator!");
        return;
    }

    for (const wchar_t c : wstringView)
    {
        if (ShouldStripCommittedChar(c))
        {
            continue;
        }
        const auto wcharCode = static_cast<uint32_t>(c);
        auto *scaleformData = scaleformDataCreator->Create();
        if (scaleformData != nullptr)
        {
            Send(scaleformData, wcharCode, messageQueue, ImeMenuName);
        }
        else
        {
            logger::error("Unexpected error. Can't create scaleform message data!");
            break;
        }
    }
}

namespace
{
//! CommonLibSSE-NG 的 IMenu 虚表：dtor 00、Accept 01、PostCreate 02、Unk_03 03、
//! ProcessMessage 04。这里只用它认出"这个菜单由哪个模块实现"，真正的调用走编译器
//! 解析的虚函数——本插件自己的 ImeMenu::ProcessMessage 覆写就落在这同一格。
constexpr std::ptrdiff_t PROCESS_MESSAGE_VTABLE_SLOT = 4;

//! 当前 ImGui 家族界面吃哪条投递通道。游戏线程写、IME 线程读；默认
//! EngineEvents，即这个特性上线时的行为，探测拿不到结论时不会把 Tailor 那类界面弄坏。
std::atomic<int>       g_hostChannel{static_cast<int>(ImGuiHostChannel::EngineEvents)};
std::atomic<void *>    g_probedMenu{nullptr}; ///< 缓存结论所属的那个宿主菜单对象
std::atomic<ULONGLONG> g_nextProbeMs{0};

auto ModuleOf(const void *address) -> HMODULE
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address),
            &module))
    {
        return nullptr;
    }
    return module;
}

std::string ModuleLeafName(const HMODULE module)
{
    wchar_t path[MAX_PATH]{};
    if (module == nullptr || GetModuleFileNameW(module, path, MAX_PATH) == 0)
    {
        return std::string("?");
    }
    const std::wstring_view name(path);
    const auto              leaf = name.find_last_of(L"\\/");
    const auto              start = leaf == std::wstring_view::npos ? 0U : leaf + 1;
    const std::wstring      file(name.substr(start));
    char                    utf8[MAX_PATH] = {};
    const int               length = WideCharToMultiByte(CP_UTF8, 0, file.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr);
    return length > 0 ? std::string(utf8) : std::string("?");
}

auto ProcessMessageHandler(RE::IMenu *menu) -> void *
{
    const std::uintptr_t vptr  = *reinterpret_cast<const std::uintptr_t *>(menu);
    const auto          *vtable = reinterpret_cast<const void *const *>(vptr);
    return const_cast<void *>(vtable[PROCESS_MESSAGE_VTABLE_SLOT]);
}

//! 自顶向下收集候选宿主菜单：ProcessMessage 由"既不是原版也不是本插件"的模块实现的
//! 菜单。原版要排除，是因为能被它收下 GFx 字符事件的界面必然有 Scaleform 字段，而那
//! 已经让 PollExternalImGuiSurface 判成普通界面了；本插件自己的 ImeMenu 要排除，是因为
//! 它在输入法接管键盘期间截走字符事件，它答"我收"对投递目标毫无意义。
constexpr std::size_t MAX_HOST_CANDIDATES = 8;

auto CollectHostMenus(RE::IMenu **out) -> std::size_t
{
    auto *ui = RE::UI::GetSingleton();
    if (ui == nullptr)
    {
        return 0;
    }
    const HMODULE self = ModuleOf(reinterpret_cast<const void *>(&CollectHostMenus));
    const HMODULE game = GetModuleHandleW(nullptr);
    std::size_t   count = 0;
    for (auto i = ui->menuStack.size(); i-- > 0 && count < MAX_HOST_CANDIDATES;)
    {
        auto *menu = ui->menuStack[i].get();
        if (menu == nullptr)
        {
            continue;
        }
        const HMODULE owner = ModuleOf(ProcessMessageHandler(menu));
        if (owner != nullptr && owner != game && owner != self)
        {
            out[count++] = menu;
        }
    }
    return count;
}

//! 探测问不出来的那一档。能问出的是"这个菜单收不收 Scaleform 字符事件"（看返回值），
//! 问不出的是"收了之后有没有转进它自己的 ImGui"。Tailor 的 `TailorMenu::ProcessMessage`
//! 见 kScaleformEvent 直接 `return kHandled`，且从头到尾不读事件载荷（它的原始输入另有
//! 一路：引擎 InputEvent，见其 `InputDispatchHook`），只按返回值判就会把这种"吞而不读"
//! 误判成 gfx 通道。此类宿主按模块名强制指派——新增一个模组最多补一行。
struct ForcedHostChannel
{
    const char      *moduleLeaf; ///< 小写的模块文件名
    ImGuiHostChannel channel;
};

constexpr ForcedHostChannel kForcedHostChannels[] = {
    { "tailor.dll", ImGuiHostChannel::EngineEvents },
};

auto Lower(std::string text) -> std::string
{
    std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(::tolower(c)); });
    return text;
}

auto ForcedChannelOf(const std::string &moduleLeaf) -> std::optional<ImGuiHostChannel>
{
    for (const auto &entry : kForcedHostChannels)
    {
        if (moduleLeaf == entry.moduleLeaf)
        {
            return entry.channel;
        }
    }
    return std::nullopt;
}

//! 探测结论：走哪条通道，以及是哪个模块的菜单收下了字符事件（写日志用）。
struct ProbeOutcome
{
    ImGuiHostChannel channel = ImGuiHostChannel::EngineEvents;
    std::string      handlerModule; ///< 收下的那个菜单所属模块，无人收下时为空
};

/// 用提交文字时同一种消息问候选菜单收不收。事件带 code=0：ImGui 的 AddInputCharacter
/// 忽略 0，Scaleform 文本字段同样忽略，所以探测本身不会在字面上留下任何字符。
/// 第一个答 kHandled 的是引擎向下派发时会停住的那一家，即"GFx 通道对它有效"——和真实
/// 提交的路径一致。注意它只证明菜单收下事件，不证明它把字符转进了自己的 ImGui，那种
/// "吞而不读"的宿主得靠上面的强制指派表。kIgnore 不算收下：引擎同样会在那里停住，但那
/// 是把字符丢掉。
auto ProbeHostMenus(RE::IMenu **menus, std::size_t count) -> ProbeOutcome
{
    ProbeOutcome outcome;
    const auto  *strings      = RE::InterfaceStrings::GetSingleton();
    auto        *msgFactoryMgr = RE::MessageDataFactoryManager::GetSingleton();
    if (strings == nullptr || msgFactoryMgr == nullptr || count == 0)
    {
        return outcome;
    }
    const auto *creator = msgFactoryMgr->GetCreator<RE::BSUIScaleformData>(strings->bsUIScaleformData);
    if (creator == nullptr)
    {
        return outcome;
    }
    auto *data = creator->Create();
    if (data == nullptr)
    {
        return outcome;
    }

    auto *event          = new GFxCharEvent(0);
    event->type          = RE::GFxEvent::EventType::kCharEvent;
    data->scaleformEvent = event;

    RE::UIMessage probe{};
    probe.type = RE::UI_MESSAGE_TYPE::kScaleformEvent;
    probe.data = data;

    int answer = -1;
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto result = menus[i]->ProcessMessage(probe);
        answer            = static_cast<int>(result);
        if (result != RE::UI_MESSAGE_RESULTS::kHandled)
        {
            continue;
        }
        // This menu is where the engine's own walk would stop, so it owns the
        // commit — but "took the event" is not "put the characters into its
        // ImGui". Check the declaration for exactly this menu (not the topmost
        // one: another mod's menu can sit above the text owner, and it passed).
        const auto leaf = Lower(ModuleLeafName(ModuleOf(ProcessMessageHandler(menus[i]))));
        if (const auto forced = ForcedChannelOf(leaf))
        {
            logger::info(
                "ImGui host {} has a declared delivery channel -> {}",
                leaf,
                *forced == ImGuiHostChannel::Scaleform ? "Scaleform" : "engine input queue");
            outcome.channel       = *forced;
            outcome.handlerModule = leaf;
            break;
        }
        outcome.channel       = ImGuiHostChannel::Scaleform;
        outcome.handlerModule = leaf;
        break;
    }

    // 事件故意留在堆上不释放：宿主菜单若把收到的指针存起来，我们先放掉就是悬垂指针，
    // 而每次换界面泄漏 12 字节是更便宜的失败模式。data 先把指针抹掉再 Destroy，避免
    // 它的析构把同一个事件再释放一次。
    data->scaleformEvent = nullptr;
    creator->Destroy(data);

    if (outcome.channel == ImGuiHostChannel::Scaleform)
    {
        logger::info(
            "ImGui host menu {} consumes Scaleform char events ({} mod menu(s) probed) -> GFx delivery",
            outcome.handlerModule,
            count);
    }
    else if (outcome.handlerModule.empty())
    {
        // Nothing on the stack took the event: only the engine's input queue can
        // reach this surface. A declared engine host logged its own line above.
        logger::info(
            "No ImGui host menu took the char-event probe ({} mod menu(s), last answer {}) -> engine input queue delivery",
            count,
            answer);
    }
    return outcome;
}

//! 当前文本目标的投递通道：问菜单栈谁收下字符事件，收下那一家若有声明通道以声明为准。
auto DecideChannel(RE::IMenu **candidates, std::size_t count) -> ImGuiHostChannel
{
    if (count == 0)
    {
        logger::info("ImGui-family surface has no mod menu on the stack -> engine input queue delivery");
        return ImGuiHostChannel::EngineEvents;
    }
    return ProbeHostMenus(candidates, count).channel;
}
} // namespace

void RefreshImGuiHostChannel()
{
    RE::IMenu *candidates[MAX_HOST_CANDIDATES];
    const auto   count  = CollectHostMenus(candidates);
    auto        *topmost = count > 0 ? candidates[0] : nullptr;
    if (topmost == g_probedMenu.load(std::memory_order_acquire))
    {
        return; // 还是同一个界面：缓存的结论照用
    }
    const auto now = GetTickCount64();
    if (now < g_nextProbeMs.load(std::memory_order_relaxed))
    {
        return; // 菜单指针在频繁变动，等它稳定下来再问
    }
    g_nextProbeMs.store(now + 250U, std::memory_order_relaxed);

    g_hostChannel.store(static_cast<int>(DecideChannel(candidates, count)), std::memory_order_release);
    g_probedMenu.store(topmost, std::memory_order_release);
}

void InvalidateImGuiHostChannel()
{
    g_probedMenu.store(nullptr, std::memory_order_release);
    g_nextProbeMs.store(0, std::memory_order_relaxed);
    g_hostChannel.store(static_cast<int>(ImGuiHostChannel::EngineEvents), std::memory_order_release);
}

auto CurrentImGuiHostChannel() -> ImGuiHostChannel
{
    return static_cast<ImGuiHostChannel>(g_hostChannel.load(std::memory_order_acquire));
}

/// 游戏线程每帧调用：判断当前持有文本输入的是不是"没有 Scaleform 字段"的界面
/// （模组的私有 Dear ImGui 菜单）。InputFocusAnchor 要 Invoke Scaleform，只能在游戏
/// 线程上问；带 2 次连续确认的去抖，避免 Scaleform 字段刚获得焦点、插入符尚未布局时
/// 被误判成 ImGui 表面而抢走本该走 GFx 通道的文字。
void PollExternalImGuiSurface()
{
    static int       s_negativeStreak = 0;
    static ULONGLONG s_lastPoll       = 0;

    // ComputeScreenMetrics 要 Invoke Scaleform，而"有文本输入"会持续整个编辑过程
    // （包括玩家只是打英文），所以按 50ms 节流。两次去抖即 100ms，远快于任何一次
    // 从点击输入框到提交汉字的间隔。
    const ULONGLONG now = GetTickCount64();
    if (now - s_lastPoll < 50U)
    {
        return;
    }
    s_lastPoll = now;

    auto *controlMap = ControlMap::GetSingleton();
    if (!ImeApp::GetInstance().GetSettings().input.imguiSurfaceInput || controlMap == nullptr || !controlMap->HasTextEntry())
    {
        s_negativeStreak = 0;
        g_externalImGuiSurface.store(false, std::memory_order_release);
        InvalidateImGuiHostChannel();
        return;
    }

    if (InputFocusAnchor::GetInstance().ComputeScreenMetrics())
    {
        s_negativeStreak = 0;
        g_externalImGuiSurface.store(false, std::memory_order_release);
        InvalidateImGuiHostChannel();
        return;
    }

    if (++s_negativeStreak >= 2 && !g_externalImGuiSurface.load(std::memory_order_acquire))
    {
        logger::info("Text target has no Scaleform field: treating it as an ImGui-family surface");
        g_externalImGuiSurface.store(true, std::memory_order_release);
    }
    // 已经被判成 ImGui 家族：它到底吃 GFx 字符事件（Modex 那类把 Scaleform 事件
    // 转进自己 ImGui 的菜单），还是只吃引擎输入队列的 CharEvent（Tailor 3.x），
    // 决定了上屏文字走哪条路。探测带缓存，只在最上层宿主菜单换人时重跑。
    if (g_externalImGuiSurface.load(std::memory_order_acquire))
    {
        RefreshImGuiHostChannel();
    }
}

void MarkCompositionActive()
{
    const HWND hwnd = ImeApp::GetInstance().GetGameHWND();
    if (hwnd == nullptr)
    {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    SetPropW(hwnd, PropertyName(COMPOSING_PROPERTY), reinterpret_cast<HANDLE>(static_cast<uintptr_t>(now)));
}

/// 游戏线程每帧调用：把"我们正在组字"发布给宿主界面，并读回它是否声明了自己处理。
void PublishCompositionState()
{
    const HWND hwnd = ImeApp::GetInstance().GetGameHWND();
    if (hwnd == nullptr)
    {
        return;
    }

    // 每帧读：界面方的声明发生在它自己的 ImGui 初始化时，节流会让"打开界面后第一
    // 次提交"错过它，那一次退格就会删掉玩家真正的文字。同进程 GetProp 只是一次
    // 窗口属性表查找，代价可以忽略。
    g_imeAwareHost.store(GetPropW(hwnd, PropertyName(IME_AWARE_PROPERTY)) != nullptr, std::memory_order_release);

    const bool composing = Core::State::GetInstance().HasAny(Core::State::IN_COMPOSING, Core::State::IN_CAND_CHOOSING);
    if (composing)
    {
        // 心跳：MarkCompositionActive 已经打过，这里只是维持新鲜度。
        MarkCompositionActive();
    }
    else
    {
        RemovePropW(hwnd, PropertyName(COMPOSING_PROPERTY));
    }
}

/// 游戏线程每帧调用：投递排队的提交文字（先退格、再字符）。按帧分批是因为引擎的事件环
/// 每帧容量有限——charEvents 只有 5 槽、buttonEvents 10 槽（一次退格占按下+抬起两条），
/// 超出槽位的 AddEvent 会被静默丢弃。
void DrainPendingCommittedText()
{
    PendingCommit item;
    {
        const std::lock_guard lock(g_commitMutex);
        if (g_commitQueue.empty())
        {
            return;
        }
        item = g_commitQueue.front();
        g_commitQueue.pop_front();
    }

    auto *queue = RE::BSInputEventQueue::GetSingleton();
    if (queue == nullptr)
    {
        logger::warn("BSInputEventQueue unavailable: dropped {} committed unit(s)", item.text.size());
        return;
    }

    // 退格与文字能同帧就同帧：ImGui 在一次 NewFrame 里按顺序吃掉整批事件，同帧到达
    // 意味着宿主界面渲染出来直接是最终文字，玩家看不到"逐字删除"。
    // 但绝不能边删边投——剩下的退格会把刚投进去的中文吃掉。分帧是引擎环容量的硬
    // 限制：buttonEvents 每帧 10 条（一次退格占按下+抬起），charEvents 每帧 5 条。
    const bool eraseFitsThisFrame = item.eraseLetters <= 5;
    if (item.eraseLetters > 0)
    {
        const std::size_t erased = std::min<std::size_t>(item.eraseLetters, 5);
        const auto        scan   = static_cast<std::uint32_t>(MapVirtualKeyW(VK_BACK, MAPVK_VK_TO_VSC));
        for (std::size_t i = 0; i < erased; ++i)
        {
            // value>0 且 held==0 才是"按下"，value==0 且 held>0 才是"抬起"。
            if (auto *down = RE::ButtonEvent::Create(RE::INPUT_DEVICE::kKeyboard, RE::BSFixedString{}, scan, 1.0F, 0.0F))
            {
                queue->PushOntoInputQueue(down);
            }
            if (auto *up = RE::ButtonEvent::Create(RE::INPUT_DEVICE::kKeyboard, RE::BSFixedString{}, scan, 0.0F, 1.0F))
            {
                queue->PushOntoInputQueue(up);
            }
        }
        item.eraseLetters -= erased;
        logger::info("Erased {} leaked preedit letter(s) ahead of the commit", erased);
    }

    std::size_t consumed = 0;
    std::size_t pushed   = 0;
    if (eraseFitsThisFrame)
    {
        for (const wchar_t c : item.text)
        {
            if (consumed >= 5)
            {
                break;
            }
            ++consumed;
            if (ShouldStripCommittedChar(c))
            {
                continue;
            }
            queue->AddCharEvent(static_cast<std::uint32_t>(c));
            ++pushed;
        }
    }
    logger::info(
        "Delivered {} committed unit(s) as engine CharEvents ({} of {} consumed, {} erase left)",
        pushed,
        consumed,
        item.text.size(),
        item.eraseLetters
    );

    if (item.eraseLetters > 0 || consumed < item.text.size())
    {
        item.text.erase(0, consumed);
        const std::lock_guard lock(g_commitMutex);
        g_commitQueue.push_front(item);
    }
}

} // namespace Ime::Skyrim
