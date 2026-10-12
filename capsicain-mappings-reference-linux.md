# Capsicain Linux 参考文档

> 本文档记录 Capsicain Linux 版本的完整架构、平台适配方案、配置规则底层实现、及所有源文件的功能说明。
>
> **对应 Windows 版本**：`capsicainclion/`（使用 Interception 驱动）
> **核心逻辑**：与 Windows 版本完全相同，仅底层输入层替换为 Linux evdev/uinput

---

## 一、项目总览

| 项目 | Windows 版 | Linux 版 |
|------|-----------|---------|
| 目录 | `capsicainclion/` | `capsicainlinux/` |
| 平台 API | Interception 驱动 | evdev + uinput |
| 构建系统 | CMake + Visual Studio | CMake + GCC/Clang |
| 权限要求 | 管理员 | root（`sudo`） |
| 设备文件 | `interception.dll` | 无外部依赖 |
| 系统托盘 | 支持 | 不支持 |
| AHK 集成 | 支持 | 不支持 |
| 后台运行 | 系统托盘 | `-d` / `--daemon` 守护进程模式 |
| 可执行文件 | `capsicain.exe` | `capsicain` |

### 文件清单

```
capsicainlinux/
├── CMakeLists.txt           # CMake 构建配置（35 行）
├── capsicain.ini            # 配置文件
├── capsicain-mappings-reference-linux.md  # 本文档
└── src/
    ├── capsicain.h          # 主程序头文件（62 行）
    ├── capsicain.cpp        # 主程序核心逻辑（1694 行）
    ├── linux_input.h        # Linux 输入层抽象接口（35 行）
    ├── linux_input.cpp      # evdev/uinput 平台实现（574 行）
    ├── scancodes.h          # PS/2 Set 1 扫描码 + 虚拟键码枚举（190 行）
    ├── scancodes.cpp        # 扫描码标签定义（224 行）
    ├── modifiers.h          # 修饰符位掩码定义（53 行）
    ├── modifiers.cpp        # 修饰符位掩码转换（55 行）
    ├── constants.h          # 全局常量（38 行）
    ├── configUtils.h        # INI 解析函数声明（22 行）
    ├── configUtils.cpp      # INI 解析实现（645 行）
    ├── utils.h              # 工具函数声明（20 行）
    ├── utils.cpp            # 工具函数实现（122 行）
    ├── pch.h                # 预编译头（18 行）
    └── pch.cpp              # 预编译头占位
```

**总代码量**：3752 行（C++ 源文件 3314 行 + 头文件 438 行）

---

## 二、平台适配：Windows → Linux 对照表

### 2.1 核心 API 映射

| Windows API | Linux API | 说明 |
|------------|-----------|------|
| `interception_create_context()` | `open("/dev/input/eventX")` + `epoll_create1()` | 打开键盘设备并设置 epoll 多路复用 |
| `interception_set_filter()` | `ioctl(fd, EVIOCGRAB, 1)` | 独占抓取键盘（阻止事件到达其他进程） |
| `interception_receive()` | `linux_input_receive()` — `epoll_wait()` + `read()` + PS/2 转换 | 阻塞等待按键，将 evdev keycode 转换为 PS/2 Set 1 scancode |
| `interception_send()` | `linux_input_send()` — `write()` 到 `/dev/uinput` | 通过虚拟键盘发送按键事件 |
| `Sleep()` | `linux_sleep_ms()` — `std::this_thread::sleep_for()` | 毫秒级延迟 |
| `QueryPerformanceCounter()` | `std::chrono::steady_clock` | 高精度计时 |
| `GetKeyState(VK_CAPITAL)` | `globalState.capsLockLedState` | 内部状态跟踪 CapsLock/NumLock/ScrollLock |

### 2.2 evdev → PS/2 Set 1 扫描码映射

**文件**：[linux_input.cpp:L37-L208](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/linux_input.cpp#L37-L208)

Linux 内核使用 `KEY_*` 枚举值（定义在 `<linux/input-event-codes.h>`），与 PS/2 Set 1 扫描码不同。我们在 `init_scancode_maps()` 中建立双向映射表：

- `ps2_to_linux[256]`：PS/2 scancode → Linux keycode
- `linux_to_ps2[KEY_MAX+1]`：Linux keycode → PS/2 scancode

标准键（0x01-0x58）的 Linux keycode 与 PS/2 scancode 恰好相同（历史原因，Linux 的 KEY_* 值沿用了 PS/2 Set 1），但扩展键（E0 前缀）不同：

| 按键 | PS/2 Scancode | Linux KEY_* | 映射 |
|------|:---:|:---:|------|
| Right Ctrl | E0 1D → 0x9D | 97 (KEY_RIGHTCTRL) | `MAP(0x9D, KEY_RIGHTCTRL)` |
| Right Alt | E0 38 → 0xB8 | 100 (KEY_RIGHTALT) | `MAP(0xB8, KEY_RIGHTALT)` |
| Home | E0 47 → 0xC7 | 102 (KEY_HOME) | `MAP(0xC7, KEY_HOME)` |
| Left Meta (Win) | E0 5B → 0xDB | 125 (KEY_LEFTMETA) | `MAP(0xDB, KEY_LEFTMETA)` |
| KP Enter | E0 1C → 0x9C | 96 (KEY_KPENTER) | `MAP(0x9C, KEY_KPENTER)` |

> **设计要点**：核心逻辑（capsicain.cpp）完全使用 PS/2 scancode，与 Windows 版本一致。只有在输入/输出边界才做 PS/2 ↔ Linux keycode 转换。这意味着所有 REWIRE/COMBO/MODIFIER 逻辑无需任何修改。

---

## 三、启动流程（关键！）

**文件**：[capsicain.cpp:L254-L475](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L254-L475)

```
main()
  ├── signal(SIGINT/SIGTERM, signal_handler)  ← Ctrl+C 优雅退出
  ├── 解析命令行参数 (-d/--daemon, --kill, -h/--help)
  │     ├── --kill  → 读取 /tmp/capsicain.pid，发送 SIGTERM，退出
  │     └── -d      → 设置 daemon_mode = true
  │
  ├── already_running()                       ← 防止重复运行
  ├── [如果 -d] linux_daemonize()             ← 双 fork 脱离终端，成为守护进程
  │     ├── fork() → 父进程退出，终端立即返回
  │     ├── setsid() → 子进程成为新会话组长
  │     ├── fork() → 二次 fork，防止重新获取终端
  │     ├── stdin → /dev/null
  │     └── stdout/stderr → /tmp/capsicain.log
  │
  ├── create_pid_file() + write_pid()         ← 写入 /tmp/capsicain.pid
  ├── printHelloHeader()
  ├── defineAllPrettyVKLabels()
  ├── readSanitizeIniFile("capsicain.ini")    ← 注意：用相对路径，保持工作目录不变
  ├── parseIniGlobals()
  ├── switchConfig(activeConfigOnStartup)
  │
  ├── linux_input_init()                       ← 平台初始化（关键四步）
  │     ├── Step 1: create_uinput_device()     ← 创建虚拟键盘
  │     ├── Step 2: wait_for_uinput_device()   ← 轮询等待 udev 识别（最多 2s）
  │     ├── Step 3: linux_sleep_ms(1000)       ← 让 libinput/X11/Wayland 初始化
  │     └── Step 4: grab 物理键盘 + epoll 监听  ← 最后才 grab！
  │
  └── while (g_running) { ... }                ← 主循环
```

### 3.1 为什么按 Step 1→4 的顺序执行？（重要！）

**错误顺序**（初始版本的问题）：
```
创建 uinput → 立即 grab 键盘 → 开始主循环
    ↑ 此时系统还没检测到虚拟键盘！物理键盘被 grab，输入完全丢失！
```

**正确顺序**：
```
创建 uinput → 等待系统检测(2s) → 稳定(1s) → grab 键盘 → 开始主循环
    ↑                                              ↑
   udev/libinput 已经完成设备注册                  此时虚拟键盘已可用
```

### 3.2 linux_input_init() 详细流程

**文件**：[linux_input.cpp:L346-L436](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/linux_input.cpp#L346-L436)

```
linux_input_init()
  ├── init_scancode_maps()                ← 初始化 PS/2 ↔ Linux 映射表
  │
  ├── create_uinput_device()              ← 打开 /dev/uinput，创建虚拟键盘
  │     ├── UI_SET_EVBIT(EV_KEY)          ← 启用按键事件
  │     ├── UI_SET_EVBIT(EV_SYN)          ← 启用同步事件
  │     ├── for i in 0..KEY_MAX:          ← 注册所有键码
  │     │     UI_SET_KEYBIT(i)
  │     ├── UI_DEV_SETUP("Capsicain Virtual Keyboard")
  │     └── UI_DEV_CREATE()
  │
  ├── wait_for_uinput_device()            ← 轮询 /dev/input/ 检测虚拟键盘
  │     └── for 20 attempts (100ms each):
  │           ├── 遍历 /dev/input/event*
  │           ├── ioctl(EVIOCGNAME) 读取设备名
  │           └── 匹配 "Capsicain Virtual Keyboard" → 返回 true
  │
  ├── linux_sleep_ms(1000)                ← 额外稳定时间
  │
  ├── epoll_create1(0)                    ← 创建 epoll 实例
  │
  ├── 遍历 /dev/input/event*              ← 扫描物理键盘
  │     └── for each device:
  │           ├── is_keyboard_device()     ← 检查是否有 KEY_A/ESC/Space/Enter
  │           ├── 跳过 "Capsicain Virtual Keyboard" ← 不 grab 自己
  │           ├── grab_device(fd)          ← ioctl(EVIOCGRAB, 1)
  │           └── epoll_ctl(EPOLL_CTL_ADD) ← 加入监听
  │
  └── 输出 "Total keyboards grabbed: N"
```

### 3.3 键盘设备识别

**文件**：[linux_input.cpp:L229-L257](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/linux_input.cpp#L229-L257)

```cpp
static bool is_keyboard_device(int fd) {
    // 检查 EV_KEY + EV_SYN 能力位
    // 必须至少有 KEY_A, KEY_ESC, KEY_SPACE, KEY_ENTER 中的 3 个
    // 这样可以过滤掉鼠标、触摸板等非键盘设备
}
```

### 3.4 安全退出

**文件**：[capsicain.cpp:L215-L218](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L215-L218) 和 [linux_input.cpp:L542-L562](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/linux_input.cpp#L542-L562)

- `SIGINT`（Ctrl+C）/ `SIGTERM` → 设置 `g_running = 0` → 主循环退出
- `linux_input_cleanup()`：
  - `ioctl(fd, EVIOCGRAB, 0)` — 释放所有物理键盘的 grab
  - `ioctl(uinput_fd, UI_DEV_DESTROY)` — 销毁虚拟键盘设备
  - `close()` 所有 fd
  - 删除 `/tmp/capsicain.pid`

---

## 四、核心处理循环

**文件**：[capsicain.cpp:L477-L670](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L477-L670)

```
while (g_running)
  ├── linux_input_receive()               ← 阻塞等待物理键盘输入
  │
  ├── [特殊处理]
  │     ├── capsicainOn == false?         → 直接透传（pass-through）
  │     ├── altTabPassthrough == true?    → 直接透传，直到物理 Alt 释放
  │     └── scancode == SC_ESCAPE?        → 检测 ESC 命令或退出
  │
  ├── processMessyKeys()                  ← 处理混乱键码（如 Alt+Print）
  ├── detectTapping()                     ← 检测 tap/tap-hold 模式
  │
  ├── processRewireScancodeToVirtualcode() ← REWIRE 重映射
  │     └── 查 allMaps.rewiremap[scancode][REWIRE_OUT/TAP/TAPHOLD]
  │
  ├── processModifierState()              ← 更新 modifierDown 位掩码
  ├── processCombos()                     ← 匹配 COMBO 修饰符规则
  ├── processMapAlphaKeys()               ← alpha_from/alpha_to 布局重映射
  │
  └── sendResultingKeyOrSequence()
        ├── 如果 resultingVKeyEventSequence 非空 → playKeyEventSequence()
        └── 否则 → sendVKeyEvent() → linux_input_send()
```

### 4.1 linux_input_receive() — 从物理键盘读取

**文件**：[linux_input.cpp:L440-L487](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/linux_input.cpp#L440-L487)

```cpp
VKeyEvent linux_input_receive() {
    while (true) {
        // epoll_wait 阻塞等待任意被 grab 的键盘设备有输入
        nfds = epoll_wait(epoll_fd, events, MAX_EVDEV_DEVICES, -1);

        for each ready fd:
            while (read all pending events from fd):
                if (ev.type == EV_KEY) {
                    ps2_sc = linuxToPs2Keycode(ev.code);  // Linux keycode → PS/2
                    is_down = (ev.value != 0);             // 0=release, 1=press, 2=autorepeat
                    return {ps2_sc, is_down};              // 返回 PS/2 scancode
                }
                // EV_SYN, EV_MSC 等事件在此被静默消费，不返回
    }
}
```

### 4.2 linux_input_send() — 通过虚拟键盘发送

**文件**：[linux_input.cpp:L489-L540](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/linux_input.cpp#L489-L540)

```cpp
void linux_input_send(int ps2_scancode, bool is_down) {
    linux_kc = ps2ToLinuxKeycode(ps2_scancode);  // PS/2 → Linux keycode

    // 发送 EV_KEY 事件（带 EAGAIN 重试，最多 3 次）
    ev.type = EV_KEY;
    ev.code = linux_kc;
    ev.value = is_down ? 1 : 0;
    write(uinput_fd, &ev, sizeof(ev));

    // 发送 EV_SYN / SYN_REPORT（带 EAGAIN 重试）
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    write(uinput_fd, &ev, sizeof(ev));
}
```

---

## 五、修饰符位掩码速查表

**文件**：[modifiers.h](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/modifiers.h) + [modifiers.cpp](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/modifiers.cpp)

与 Windows 版本**完全相同**：

| 修饰符 | 位掩码 | .ini 中位置 |
|--------|--------|-----------|
| LSHIFT | `0x0001` | `[..............&]` |
| LCTRL  | `0x0002` | `[.............&.]` |
| LWIN   | `0x0004` | `[............&..]` |
| LALT   | `0x0008` | `[...........&...]` |
| RSHIFT | `0x0010` | `[..........&....]` |
| RCTRL  | `0x0020` | `[.........&.....]` |
| RWIN   | `0x0040` | `[........&......]` |
| RALT   | `0x0080` | `[.......&.......]` |
| MOD9   | `0x0100` | `[......&........]` |
| MOD10  | `0x0200` | `[.....&.........]` |
| MOD11  | `0x0400` | `[....&..........]` |
| MOD12  | `0x0800` | `[...&...........]` |
| MOD13  | `0x1000` | `[..&............]` |
| MOD14  | `0x2000` | `[.&.............]` |
| MOD15  | `0x4000` | `[&..............]` |

**COMBO 掩码格式**：`[^t.. .... .... ....]` — 16 个字符，从左到右对应 bit15→bit0

| 符号 | 含义 | 对应列 |
|------|------|--------|
| `&` | 修饰符必须按下（AND） | `mods[1]` = modAnd |
| `\|` | 修饰符至少一个按下（OR） | `mods[2]` = modOr |
| `^` | 修饰符不能按下（NOT） | `mods[3]` = modNot |
| `t` | 修饰符必须是刚松开的（Tapped） | `mods[4]` = modTap |

---

## 六、逐条映射解析

### 6.1 CapsLock：单击 = Esc，组合 = LCtrl

```
REWIRE CAPS LCTRL ESC
```

**涉及文件**：无修改（内置功能）
**相关函数**：[processRewireScancodeToVirtualcode()](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L557-L630)、[detectTapping()](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L518-L542)

```
物理 CapsLock 按下 (PS/2 0x3A)
  → processRewireScancodeToVirtualcode()
    rewiremap[0x3A][REWIRE_OUT] = SC_LCTRL (0x1D)
    → loopState.vcode = 0x1D (LCTRL)
  → processModifierState()
    modifierDown |= BITMASK_LCTRL (0x0002)
  → detectTapping()
    如果短按后释放（tap）→ rewiremap[0x3A][REWIRE_TAP] = SC_ESC (0x01)
    → 发送 ESC down + ESC up
    如果是长按（hold）→ 保持 LCTRL，后续组合键正常使用
```

### 6.2 LAlt → 虚拟 MOD9

```
REWIRE LALT MOD9 NOP
```

**涉及文件**：无修改（内置功能）
**相关函数**：[processRewireScancodeToVirtualcode()](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L557-L630)

```
物理 LAlt 按下 (PS/2 0x38)
  → processRewireScancodeToVirtualcode()
    rewiremap[0x38][REWIRE_OUT] = VK_MOD9 (0x109)
    → loopState.vcode = 0x109 (MOD9)
  → 0x109 > 0xFF → sendCapsicainCodeHandler()
    虚拟键不发送到 Linux
  → processModifierState()
    modifierDown |= BITMASK_MOD9 (0x0100)
```

**为什么这样设计**：
- MOD9 是虚拟修饰键，永远不会通过 uinput 发送给系统
- 应用程序看不到物理 LAlt → Alt+HJKL 不会触发应用快捷键（Alt+H→Help 等）
- COMBO 规则通过 `[.... ...& .... ....]`（即 modAnd = 0x0100 = MOD9）来感知 Alt 状态

### 6.3 LShift + RShift → CapsLock 切换

```
COMBO LSHF [.... .... ...& ....] > key(CAPS)
COMBO RSHF [.... .... .... ...&] > key(CAPS)
```

**涉及文件**：无修改（内置功能）
**相关函数**：[processCombos()](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L632-L670)、[parseKeywordCombo()](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/configUtils.cpp#L384-L641)

```
按下 RShift → modifierDown |= BITMASK_RSHIFT (0x0010)
按下 LShift → vcode = SC_LSHIFT (0x2A)
  → processCombos()
    遍历 allMaps.modCombos
    vcode == 0x2A && (modifierDown & 0x0010) == 0x0010 → 命中第一条 COMBO
  → 生成 resultingVKeyEventSequence = [{SC_CAPS, true}, {SC_CAPS, false}]
  → playKeyEventSequence() 发送 CapsLock down+up
  → Linux 系统收到 CapsLock 点击 → CapsLock 状态翻转
```

### 6.4 Alt+Tab → 原生窗口切换（关键功能）

```
COMBO TAB [.... ...& .... ....] > alttab()
```

**涉及的文件和修改**：
| 文件 | 修改 |
|------|------|
| [scancodes.h:L176](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/scancodes.h#L176) | 定义 `VK_CPS_ALTTAB = 0x107` |
| [capsicain.cpp:L116-L117](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L116-L117) | GlobalState 新增 `altTabPassthrough`, `altTabExitScancode` |
| [capsicain.cpp:L504-L515](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L504-L515) | 主循环 passthrough 分支 |
| [capsicain.cpp:L1392-L1398](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1392-L1398) | sendCapsicainCodeHandler 处理 VK_CPS_ALTTAB |
| [configUtils.cpp:L534-L541](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/configUtils.cpp#L534-L541) | parseKeywordCombo 解析 `alttab` |

**完整流程**：
```
1. 按下 LAlt → REWIRE 为 MOD9 → modifierDown |= BITMASK_MOD9
2. 按下 Tab  → COMBO 匹配 [MOD9] → alttab()
   → parseKeywordCombo() 解析 "alttab"
   → 生成 strokeSeq = [{VK_CPS_ALTTAB, true}]

3. playKeyEventSequence() 播放序列
   → sendVKeyEvent({0x107, true})
   → 0x107 > 0xFF → sendCapsicainCodeHandler()
   → case VK_CPS_ALTTAB:
        linux_input_send(SC_LALT, true)      ← 发送真实 LAlt 给系统
        linux_sleep_ms(10)
        linux_input_send(SC_TAB, true)       ← 发送 Tab 给系统
        globalState.altTabPassthrough = true  ← 进入透传模式
        globalState.altTabExitScancode = SC_LALT

4. 后续所有按键 → 主循环检测到 altTabPassthrough == true
   → 直接调用 linux_input_send() 透传     ← 跳过所有 REWIRE/COMBO 处理

5. 用户释放物理 LAlt
   → scancode == SC_LALT && !isDownstroke
   → linux_input_send(SC_LALT, false)      ← 透传 LAlt UP → 关闭窗口切换器
   → altTabPassthrough = false, altTabExitScancode = 0
   → reset()                               ← 重置所有状态
```

### 6.5 Alt+HJKLUDIA → 光标移动

```
COMBO H [.... ...& .... ....] > moddedKey(LEFT + .... .... .... ....)
```

**涉及文件**：无修改（纯配置）
**相关函数**：[parseFunctionModdedkey()](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/configUtils.cpp#L333-L382)

```
按下 LAlt → MOD9 → modifierDown |= BITMASK_MOD9 (0x0100)
按下 H    → COMBO 匹配 [MOD9] → moddedKey(LEFT + .... .... .... ....)

parseFunctionModdedkey() 生成序列:
  1. VK_CPS_TEMPRELEASEKEYS, true   ← 释放所有已按下的键
  2. (无修饰键要按下, + .... 全为 0)
  3. SC_LEFT, true                   ← LEFT DOWN
  4. SC_LEFT, false                  ← LEFT UP
  5. (无修饰键要释放)
  6. VK_CPS_TEMPRESTOREKEYS, false  ← 恢复所有已按下的键

playKeyEventSequence():
  Step 1: TEMPRELEASE → 释放任何被发送程序按住的键
  Step 3: sendVKeyEvent(SC_LEFT, true) → linux_input_send() → uinput
  Step 4: sendVKeyEvent(SC_LEFT, false)→ linux_input_send() → uinput
  Step 6: TEMPRESTORE → 恢复键状态

Linux 系统收到: 干净的 LEFT 键（不带任何修饰符）→ 光标左移一格
```

### 6.6 Alt+Shift+HJKLUDIA → 选中文本

```
COMBO H [.... ...& .... ...&] > key(LEFT)
```

**涉及文件**：无修改（纯配置）
**为什么用 `key()` 而不是 `moddedKey()`**：物理 Shift 键已被 Linux 系统感知（未被 REWIRE），直接发 `key(LEFT)` 就是 Shift+Left。

### 6.7 Alt+CapsLock+HL → 按单词移动

```
COMBO H [.... ...& .... ..&.] > moddedKey(LEFT + .... .... .... ..&.)
```

**涉及文件**：无修改（纯配置）

CapsLock 被 REWIRE 为 LCTRL，所以 Alt+CapsLock+H = MOD9 + LCTRL + H。必须用 `moddedKey()` 确保在发送 Ctrl+Left 之前释放/恢复修饰符状态。

---

## 七、COMBO 匹配规则（顺序敏感！）

**文件**：[processCombos()](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L632-L670)

```
规则: 同一 vkey 的多个 COMBO，先匹配先胜，更具体的修饰符必须排在前面！

配置顺序:
  1. COMBO H [MOD9+LSHIFT] > key(LEFT)          ← 最具体: modAnd = 0x0101
  2. COMBO H [MOD9+LCTRL]  > moddedKey(LEFT+LCTRL) ← 次具体: modAnd = 0x0102
  3. COMBO H [MOD9]        > moddedKey(LEFT)        ← 最宽泛: modAnd = 0x0100

匹配逻辑：
  for each ModifierCombo:
    if (vcode == combo.vkey                         // 键码匹配
        && deadkey == combo.deadkey                 // 死键匹配
        && (modDown & modAnd) == modAnd             // AND 修饰符全部满足
        && (modOr == 0 || (modDown & modOr) > 0)   // OR 修饰符至少一个
        && (modDown & modNot) == 0                  // NOT 修饰符都不在
        && (modTapped & modTap) == modTap)          // Tapped 修饰符全部满足
    {
        → 使用该 COMBO 的 keyEventSequence
        → break（不再检查后续规则）
    }
```

---

## 八、虚拟键码处理

**文件**：[scancodes.h:L170-L191](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/scancodes.h#L170-L191)

所有 `VK_CPS_*` 虚拟键码的值 > 0xFF，在 `sendVKeyEvent()` 中检测并通过 `sendCapsicainCodeHandler()` 处理：

| 虚拟键码 | 值 | 处理函数 | 行为 |
|---------|-----|---------|------|
| `VK_CPS_ALTTAB` | 0x107 | [capsicain.cpp:L1392-L1398](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1392-L1398) | 发送真实 Alt+Tab，进入透传模式 |
| `VK_CPS_CAPSON` | 0x110 | [capsicain.cpp:L1373-L1376](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1373-L1376) | 如果 CapsLock 关闭，发送 CapsLock 点击 |
| `VK_CPS_CAPSOFF` | 0x111 | [capsicain.cpp:L1378-L1381](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1378-L1381) | 如果 CapsLock 开启，发送 CapsLock 点击 |
| `VK_CPS_CONFIGSWITCH` | 0x105 | [capsicain.cpp:L1484-L1487](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1484-L1487) | 切换配置层 |
| `VK_CPS_CONFIGPREVIOUS` | 0x106 | [capsicain.cpp:L1383-L1385](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1383-L1385) | 切换到上一个配置 |
| `VK_CPS_TEMPRELEASEKEYS` | 0x101 | [capsicain.cpp:L1514-L1521](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1514-L1521) | 临时释放所有已发送的按键 |
| `VK_CPS_TEMPRESTOREKEYS` | 0x102 | [capsicain.cpp:L1522-L1530](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1522-L1530) | 恢复之前释放的按键 |
| `VK_CPS_SLEEP` | 0x103 | [capsicain.cpp:L1470-L1472](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1470-L1472) | 延迟（sequence 中的 sleep:） |
| `VK_CPS_RECORDMACRO` | 0x112 | [capsicain.cpp:L1490-L1500](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1490-L1500) | 开始录制宏 |
| `VK_CPS_PLAYMACRO` | 0x114 | [capsicain.cpp:L1503-L1509](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1503-L1509) | 播放已录制宏 |

---

## 九、全部核心函数速查表

### 9.1 主程序（capsicain.cpp, 1694 行）

| 函数 | 行号 | 作用 |
|------|:---:|------|
| `main()` | [254](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L254) | 入口：初始化 → 主循环 → 清理 |
| `InterceptionSendCurrentKeystroke()` | [199](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L199) | 透传当前按键（保留原名以保持一致性） |
| `linux_get_key_state()` | [204](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L204) | 查询 CapsLock/NumLock/ScrollLock LED 状态 |
| `processOnOffKey()` | [482](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L482) | 切换 Capsicain 开关 |
| `processMessyKeys()` | [506](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L506) | 处理混乱键码（如 Alt+Print → Print） |
| `detectTapping()` | [518](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L518) | 检测 tap/tap-slow/tap-hold 模式 |
| `processModifierState()` | [544](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L544) | 更新 modifierDown / modifierTapped |
| `processRewireScancodeToVirtualcode()` | [557](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L557) | REWIRE 重映射 scancode → vcode |
| `processCombos()` | [632](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L632) | 匹配 COMBO 修饰符规则 |
| `processMapAlphaKeys()` | [672](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L672) | alpha_from/alpha_to 布局键映射 |
| `processCommand()` | [692](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L692) | ESC 命令处理（ESC+X 退出等） |
| `parseIniGlobals()` | [832](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L832) | 解析 [GLOBAL] 段 |
| `parseIniOptions()` | [885](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L885) | 解析 OPTION 指令 |
| `parseIniRewires()` | [956](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L956) | 解析 REWIRE 指令 |
| `parseIniCombos()` | [989](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L989) | 解析 COMBO 指令 |
| `parseIniAlphaLayout()` | [1042](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1042) | 解析 ALPHA_FROM..ALPHA_TO |
| `assembleConfig()` | [1078](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1078) | 组装配置（含 INCLUDE 展开） |
| `switchConfig()` | [1176](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1176) | 切换活跃配置层 |
| `reset()` | [1224](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1224) | 重置所有运行时状态 |
| `reload()` | [1255](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1255) | 重新加载 .ini 文件 |
| `releaseAllSentKeys()` | [1267](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1267) | 释放所有标记为"已发送"的按键 |
| `sendCapsicainCodeHandler()` | [1365](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1365) | 处理 vcode > 0xFF 的特殊控制码 |
| `sendResultingKeyOrSequence()` | [1411](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1411) | 发送最终结果：序列或单键 |
| `playKeyEventSequence()` | [1432](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1432) | 播放按键序列（处理 VK_CPS_* 控制码和宏播放） |
| `sendVKeyEvent()` | [1574](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1574) | 发送单个按键（keysDownSent 跟踪、宏录制） |
| `SendShiftDownUp()` | [1653](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1653) | 发送 Shift 弹起再按下（抑制 WinKey 菜单） |
| `obfuscateVKey()` / `deObfuscateVKey()` | [1688](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/capsicain.cpp#L1688) | XOR 混淆/解混淆密钥（加密宏录制） |

### 9.2 Linux 输入层（linux_input.cpp, 574 行）

| 函数 | 行号 | 作用 |
|------|:---:|------|
| `init_scancode_maps()` | [37](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/linux_input.cpp#L37) | 初始化 PS/2 ↔ Linux keycode 双向映射表 |
| `ps2ToLinuxKeycode()` | [213](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/linux_input.cpp#L213) | PS/2 scancode → Linux KEY_* |
| `linuxToPs2Keycode()` | [219](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/linux_input.cpp#L219) | Linux KEY_* → PS/2 scancode |
| `is_keyboard_device()` | [229](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/linux_input.cpp#L229) | 判断 /dev/input/eventX 是否为键盘设备 |
| `create_uinput_device()` | [259](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/linux_input.cpp#L259) | 创建 "Capsicain Virtual Keyboard" uinput 设备 |
| `grab_device()` | [305](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/linux_input.cpp#L305) | `ioctl(EVIOCGRAB, 1)` 独占抓取设备 |
| `wait_for_uinput_device()` | [314](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/linux_input.cpp#L314) | 轮询等待 udev 检测到虚拟键盘 |
| `linux_input_init()` | [346](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/linux_input.cpp#L346) | 初始化：uinput + 检测等待 + grab 键盘 + epoll |
| `linux_input_receive()` | [440](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/linux_input.cpp#L440) | epoll_wait 阻塞读取物理键盘事件，转换为 PS/2 scancode |
| `linux_input_send()` | [489](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/linux_input.cpp#L489) | 通过 uinput 虚拟键盘发送事件 |
| `linux_input_cleanup()` | [542](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/linux_input.cpp#L542) | 释放 grab、销毁 uinput、关闭 fd |
| `linux_sleep_ms()` | [566](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/linux_input.cpp#L566) | 毫秒级延迟 |
| `linux_daemonize()` | [565](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/linux_input.cpp#L565) | 双 fork 脱离终端，重定向 I/O 到日志文件 |

### 9.3 配置解析（configUtils.cpp, 645 行）

| 函数 | 行号 | 作用 |
|------|:---:|------|
| `readSanitizeIniFile()` | [67](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/configUtils.cpp#L67) | 读取并清理 .ini 文件 |
| `parseKeywordRewire()` | [257](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/configUtils.cpp#L257) | 解析 `REWIRE keyA keyB [keyC] [keyD]` |
| `parseModString()` | [297](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/configUtils.cpp#L297) | 解析 `[&^t .... ...&]` 修饰符字符串→位掩码 |
| `parseFunctionCombo()` | [310](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/configUtils.cpp#L310) | 解析 `combo(key1+key2+...)` 生成按键序列 |
| `parseFunctionModdedkey()` | [333](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/configUtils.cpp#L333) | 解析 `moddedKey(key + mods)` 生成 TEMPRELEASE 序列 |
| `parseKeywordCombo()` | [384](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/configUtils.cpp#L384) | 完整解析 `COMBO key [mods] > func(params)` |
| `parseKeywordsAlpha_FromTo()` | [223](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/configUtils.cpp#L223) | 解析 `ALPHA_FROM key1 key2 \n ALPHA_TO key3 key4` |

---

## 十、构建和运行

### 构建

```bash
cd capsicainlinux
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

**构建选项**：
| CMake 选项 | 默认值 | 说明 |
|-----------|--------|------|
| `CAPSICAIN_DEBUG` | OFF | 编译时永久启用调试输出 |
| `CAPSICAIN_TRACE` | OFF | 启用跟踪输出 |
| `CAPSICAIN_PROFILE` | OFF | 启用性能分析 |
| `CMAKE_BUILD_TYPE` | Release | Debug 构建自动启用 CAPSICAIN_DEBUG |

### 运行

```bash
# 确保 uinput 模块已加载（首次使用）
sudo modprobe uinput

# 前台运行（可以看到调试输出，Ctrl+C 退出）
sudo ./build/capsicain

# 后台守护进程模式（-d 或 --daemon）
sudo ./build/capsicain -d
sudo ./build/capsicain --daemon

# 停止后台运行的 capsicain
sudo ./build/capsicain --kill

# 查看帮助
./build/capsicain --help
```

**命令行参数**：
| 参数 | 说明 |
|------|------|
| `-d`, `--daemon` | 以守护进程模式在后台运行，不占用终端 |
| `--kill` | 停止正在运行的 capsicain 守护进程（发送 SIGTERM） |
| `-h`, `--help` | 显示帮助信息 |

### 退出

- `Ctrl+C`（前台模式，发送 SIGINT）
- `ESC + X`（通过键盘命令，前后台均可用）
- `sudo ./capsicain --kill`（后台模式）
- `sudo pkill capsicain`（强制终止）

### 调试模式

```
ESC + D  → 切换调试输出（运行时）
```

调试输出显示每个按键的处理过程：
```
[0x23=        H >     LEFTv] [M: 100 T:      D:      ]   --> SEQUENCE (6)  {LEFTv #0} {LEFT^ #0}
```

---

## 十一、Linux 专属注意事项

### 11.1 uinput 虚拟设备许可

如果 `wait_for_uinput_device()` 超时未检测到虚拟键盘：
- 尝试 `sudo modprobe uinput`
- 检查 `/dev/uinput` 权限（应为 `crw------- 1 root root`）
- 某些桌面环境可能需要安装 `libinput` 或 `xf86-input-libinput`

### 11.2 PID 文件管理

程序启动时在 `/tmp/capsicain.pid` 写入 PID，防止重复运行。退出时自动删除。

- 前台运行时若已有一个实例在运行，打印提示并拒绝启动
- 后台上运行时同样通过 PID 文件检测重复实例
- 异常退出导致残留的 PID 文件会被 `--kill` 自动清理

### 11.3 信号处理

| 信号 | 行为 |
|------|------|
| SIGINT (Ctrl+C) | 优雅退出（重置键盘 grab + 销毁 uinput） |
| SIGTERM | 同 SIGINT |
| SIGKILL | 强制终止（内核自动释放 grab 和 fd） |

### 11.4 已知限制

| 功能 | Windows | Linux | 原因 |
|------|---------|-------|------|
| 系统托盘图标 | ✓ | ✗ | Linux 无统一托盘 API |
| AHK 集成 | ✓ | ✗ | AutoHotkey 仅 Windows |
| 键盘 LED 控制 | ✓ | 内部跟踪 | 通过 CapsLock LED 判断状态的 GetKeyState 用内部变量替代 |
| 设备 ID 过滤 | ✓ | 简化版 | Linux 设备识别方式不同 |
| 多键盘区分 | ✓ | ✓ | epoll 多路复用支持 |

### 11.5 Daemon 模式详解

使用 `-d` 或 `--daemon` 参数可以让 capsicain 脱离终端在后台运行。

**内部机制**（[linux_daemonize()](file:///home/mountainrain/Downloads/capsicain/capsicainlinux/src/linux_input.cpp#L565-L600)）：

```
1. 第一次 fork()  → 父进程退出，终端返回控制权给用户
2. setsid()       → 子进程成为新会话组长，脱离控制终端
3. 第二次 fork()  → 孙子进程运行，防止重新获取终端（经典双 fork 模式）
4. umask(0)       → 重置文件权限掩码
5. stdin  → /dev/null        （守护进程不需要标准输入）
6. stdout → /tmp/capsicain.log（附加模式，带时间戳）
7. stderr → /tmp/capsicain.log（所有错误输出也写入日志）
```

**日志文件**：`/tmp/capsicain.log`，每次启动追加一行时间戳标记：
```
=== capsicain daemon started at Sun Oct 12 14:30:00 2025 (PID 12345) ===
```

> **注意**：daemon 模式**不会**切换工作目录（与传统的 `chdir("/")` 不同），因此 `capsicain.ini` 仍然使用相对于启动目录的路径。请确保在 `capsicain.ini` 所在目录执行 `sudo ./capsicain -d`。

**典型工作流**：
```bash
# 1. 进入程序目录
cd /home/user/capsicainlinux/build

# 2. 后台启动
sudo ./capsicain -d
# 终端立即返回，程序在后台运行

# 3. 确认运行状态
ps aux | grep capsicain
cat /tmp/capsicain.log

# 4. 停止
sudo ./capsicain --kill
```

---

## 十二、完整需求映射表

| 物理操作 | 效果 | 配置行 | 核心函数 |
|----------|------|--------|----------|
| CapsLock 单击 | Esc | `REWIRE CAPS LCTRL ESC` | detectTapping |
| CapsLock 组合 | LCtrl | `REWIRE CAPS LCTRL ESC` | processRewireScancodeToVirtualcode |
| LShift + RShift | CapsLock 切换 | `COMBO LSHF/RSHF [SHIFT] > key(CAPS)` | processCombos |
| Alt+Tab | 窗口切换（可长按循环） | `COMBO TAB [MOD9] > alttab()` | VK_CPS_ALTTAB + passthrough |
| Alt+HJKL | 方向键 | `COMBO H [MOD9] > moddedKey(LEFT+....)` | moddedKey |
| Alt+IA | Home/End | `COMBO I/A [MOD9] > moddedKey(HOME/END+....)` | moddedKey |
| Alt+UD | PageUp/PageDown | `COMBO U/D [MOD9] > moddedKey(PGUP/PGDOWN+....)` | moddedKey |
| Alt+Shift+HJKL | 选中方向移动 | `COMBO H [MOD9+LSHIFT] > key(LEFT)` | key（利用物理 Shift） |
| Alt+Shift+IA | 选中到 Home/End | `COMBO I/A [MOD9+LSHIFT] > key(HOME/END)` | key |
| Alt+Shift+UD | 选中翻页 | `COMBO U/D [MOD9+LSHIFT] > key(PGUP/PGDOWN)` | key |
| Alt+Caps+HL | 按单词左右移动 | `COMBO H/L [MOD9+LCTRL] > moddedKey(LEFT/RIGHT+LCTRL)` | moddedKey |
| ESC+X | 退出程序 | 内置命令 | processCommand |
| ESC+D | 切换调试 | 内置命令 | processCommand |
| ESC+R | 重载 .ini | 内置命令 | reload |
| ESC+0..9 | 切换配置层 | 内置命令 | switchConfig |