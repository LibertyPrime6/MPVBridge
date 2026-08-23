# MPVBridge

MPVBridge 是一个纯 Win32、无第三方依赖的 MPV 会话级透明代理引导器。
它为每次播放锁定一个 MPV Profile，保留原始 MPV 参数，等待播放器退出并
返回相同的退出码。

## 启动模式

- 直接双击、没有媒体参数：打开居中的 **Profile 管理**窗口。
- 由浏览器、视频网站、文件关联或其他程序传入 URL/文件/MPV 参数：
  只有一个 Profile 时直接使用它播放；有多个 Profile 时根据“直接使用默认项”
  开关决定直接播放默认 Profile，或打开居中的 **Profile 选择**窗口。
- 带 `--bridge-profile=<ProfileID>`：跳过选择窗口，直接锁定指定 Profile。

选择窗口支持数字键 `1–9`、Enter 确认、Esc 取消和双击启动。关闭“直接使用
默认项”时，默认 Profile 只用于预选；打开开关后则直接使用当前默认项播放。

## Profile 管理

管理窗口可以新建、编辑、删除 Profile，浏览选择 `mpv.exe`，以及设置默认
Profile。配置保存在程序同目录的 `profiles.ini`，使用 Win32 Profile API
读写。启动时若文件不存在，程序会静默生成 UTF-16 LE 模板；首次双击启动会
直接进入 Profile 管理界面完成路径配置，不再额外弹出 INI 修改提示。

```ini
[General]
DefaultProfile=HQ_Anime
SkipProfilePicker=0
EnableLogging=0
AutoLaunchSeconds=3

[HQ_Anime]
Order=0
Name=画质增强版
Path=C:\Tools\MPV_HQ\mpv.exe
```

Profile 卡片支持直接拖动排序。拖动中的卡片会跟随鼠标，靠近有效落点时
相邻卡片会让出一个卡片高度的空间；在有效空位松手才会保存新的 `Order`，
在列表外或非落点区域松手则回到原位。

从空列表新建的第一个 Profile 会自动成为默认项。删除默认 Profile 时，
默认项会按当前列表顺序移到下一个；如果删除的是列表末项，则循环到第一个。
删除全部 Profile 后会清空 `DefaultProfile`；此时收到播放请求会提示“无可用 MPV
播放器”，不会继续尝试已删除的程序。

## 媒体文件关联

管理窗口的“系统集成”区域提供“媒体文件关联”入口。关联窗口按视频、音频和
播放列表分组，支持全部、全不选、仅视频、仅音频以及逐项勾选。保存后，
MPVBridge 会为所选扩展名注册应用名称、图标、启动命令和 Windows 应用能力，
随后打开系统“默认应用”页面完成最终授权。

Windows 10/11 不允许普通桌面程序静默改写受保护的 `UserChoice`，因此最后一次
确认需要在系统设置中完成；取消勾选只移除 MPVBridge 自己的候选注册，不会
破坏其他播放器的关联。

## 运行环境检测与便携安装

Profile 管理窗口的“系统集成”区域提供独立的“运行环境检测”入口。二级界面会
分别检测 yt-dlp、FFmpeg（同时检查 ffprobe）和 Node.js，并显示实际版本与来源。
Node.js 必须满足 yt-dlp 当前 EJS 支持要求（22 或更高版本）。

### 使用步骤

1. 双击 `MPVBridge.exe`，进入 **Profile 管理**窗口。
2. 在 **系统集成**区域点击 **运行环境检测…**，等待三项检测完成。
3. 状态为系统环境且版本受支持时，无需重复安装；MPVBridge 会优先使用它。
4. 某项未检测到可用环境时，点击该项自己的 **安装便携版**。yt-dlp、FFmpeg 和
   Node.js 可以分别下载，互不影响。
5. 下载期间，该项下方会显示独立的平滑进度条，按钮变为 **取消下载**。取消后会
   清理临时文件并隐藏进度条；安装和配置完成后，进度条同样会自动隐藏。
6. 安装完成后点击 **重新检测**，确认三项均显示可用，再关闭窗口开始网页播放。

播放 YouTube 时需要 Node.js。MPVBridge 会显式向 yt-dlp 传入
`--js-runtimes=node`；用户不需要手动把便携 Node.js 写入系统 `PATH`。

工具解析顺序固定为：

1. 优先使用系统 `PATH` 中已经安装且受支持的版本；
2. 系统缺失或版本不受支持时，回退到 MPVBridge 程序目录下的便携副本。

每项环境都有独立的“安装便携版”按钮。点击后会在请求发生时解析上游最新版本，
下载并自动配置到以下相对布局。三项下载各自显示平滑递增的进度条，下载期间对应
按钮会变为“取消下载”，可以分别取消而不影响其他下载；取消完成后会清理本次临时
文件并隐藏该进度条。进度显示会快速平滑追赶服务器报告的真实下载百分比；安装与
配置确认完成后，对应进度条也会自动清零并隐藏。

```text
MPVBridge.exe
Tools\yt-dlp\yt-dlp.exe
Tools\ffmpeg\bin\ffmpeg.exe
Tools\ffmpeg\bin\ffprobe.exe
Tools\node\node.exe
```

yt-dlp 使用官方最新 Windows x64 可执行文件，Node.js 使用官方 `latest/win-x64`
可执行文件，FFmpeg 使用 BtbN 最新 Windows x64 GPL 构建。连接或连续接收数据超过
30 秒会提示超时，单次总下载超过 10 分钟也会终止并提示；FFmpeg 解压超过 5 分钟
同样会停止。未完成的临时文件不会被当作已安装环境。

MPV 启动时只临时构造子进程环境，不修改系统 `PATH`，也不把电脑专属绝对路径
写入 INI、脚本或播放参数。系统目录排在前面，`Tools` 相对布局作为回退。因此可将
`MPVBridge.exe`、`profiles.ini` 和 `Tools` 整个文件夹复制到其他电脑继续使用。
配套网页播放会显式传入 yt-dlp 的 `--js-runtimes=node`；Node 的实际位置仍由上述
系统优先、便携回退规则解析。

网页媒体不需要注册本地文件扩展名。若要从 Bilibili、YouTube 等网页将媒体交给
MPVBridge，请安装配套的
[External Player for MPVBridge](https://github.com/LibertyPrime6/external-player-mpvbridge)
油猴脚本；它通过 `mpvbridge://` 协议调用 MPVBridge，并继续使用相同的 Profile
选择与参数透传流程。

## 网页调用协议

Profile 管理窗口的“系统集成”区域提供 `mpvbridge://` 的注册与注销按钮。
注册写入当前用户的 `HKCU\Software\Classes\mpvbridge`，不需要管理员权限；
注销只会移除由当前这份 `MPVBridge.exe` 拥有的注册。

配套 External Player 油猴脚本使用以下格式调用：

```text
mpvbridge://MPV?<UTF-8 MPV 参数的 Base64URL>
```

MPVBridge 会验证并解码载荷；只有一个 Profile 时直接使用它播放，有多个时根据
“直接使用默认项”开关选择默认 Profile 或打开选择窗口。普通双击且没有媒体参数
时仍然打开 Profile 管理窗口，两条启动路径互不混淆。

### 油猴网页集成

只有配套油猴脚本同时传入合法的会话令牌、127.0.0.1 回传端口和网页集成标记时，
MPVBridge 才启用网页集成。普通播放不再先运行一遍阻塞式 yt-dlp 模拟预检，
而是直接让 MPV/yt-dlp 完成唯一一次正式解析；随后为本次 MPV 进程创建随机命名的
Windows JSON IPC 管道，并每 10 秒读取一次 `time-pos`、`duration`、`pause`、
`playlist-pos`、`media-title` 和 `path`，再由仅监听回环地址的 HTTP 端点返回播放
快照。Bridge 不持续订阅高频进度属性。

Cookie 经用户验证后只由油猴脚本持久保存。需要 yt-dlp 登录态时，脚本使用
随机会话令牌通过 127.0.0.1 POST 本次 Netscape Cookie 内容；MPVBridge 仅在
内存和临时文件中使用，验证失败或 MPV 退出后立即删除临时文件。Cookie
不会写入自定义协议 URL、MPVBridge 配置或日志。

从 Cookie 管理页导入 TXT 时，油猴脚本先验证站点登录，再启动
`validation-only` 会话执行 yt-dlp 预检。两项均成功后才写入油猴存储；该模式
自动使用默认（或首个可用）Profile 定位 yt-dlp，验证结束后直接退出，不启动
MPV 播放器。

播放时的认证顺序固定为：Bilibili 当前视频和直播使用“网页登录请求 → 自动
Cookie → 已保存 Cookie → 匿名”，Bilibili 播放列表以及 YouTube 当前/播放列表
使用“自动 Cookie → 已保存 Cookie → 匿名”。每次播放只更新实际检查到的认证
状态。普通播放选中的 Cookie 已先通过站点登录接口验证，不再为同一媒体重复运行
yt-dlp；Cookie 管理页的 `validation-only` 流程仍会执行专用 yt-dlp 预检。

YouTube yt-dlp 请求启用 Node.js EJS 运行时，并使用当前上游建议的
`default,web_embedded` 客户端规避登录用户 `tv_downgraded` 的“页面需要重新
加载”故障。MPVBridge 不再隐藏 yt-dlp 警告；若 YouTube 明确报告账号 Cookie
已被浏览器轮换，即使公开媒体可匿名解析，也不会把该 Cookie 标记为可用。

普通程序调用不启用上述集成，MPVBridge 不会解析或接管调用方自行提供的
`--input-ipc-server`；没有油猴专用参数的命令行继续按原始字符透传。

## 直接使用默认 Profile

管理窗口“系统集成”区域的“多 Profile 时直接使用默认项”开关默认关闭。打开后，
外部媒体调用会跳过 Profile 选择窗口并使用当前默认 Profile；列表中的任意 Profile
都可以先选中，再通过“设为默认”按钮成为新的默认项，后续播放会立即改用它。

```ini
[General]
SkipProfilePicker=1
```

如果默认 Profile 不存在，MPVBridge 会回退到选择窗口，以便重新选择和修复配置。
无论此开关是否打开，列表中只有一个 Profile 时都会直接使用它播放。

## 默认 Profile 自动进入

存在多个 Profile、直接播放开关关闭且外部媒体调用打开选择窗口时，如果默认
Profile 有效，MPVBridge 会在设定秒数后自动开始播放。倒计时期间点击、滚动、
按键或操作窗口会取消本次自动进入。延迟可在管理窗口“系统集成”区域按秒调整：

```ini
[General]
AutoLaunchSeconds=3
```

取值范围为 `0–3600`；设为 `0` 可关闭自动进入。

## 诊断日志

在管理窗口的“诊断与日志”区域可以启用日志并打开日志文件。开关对应：

```ini
[General]
EnableLogging=1
```

日志写入程序同目录的 `MPVBridge.log`，记录启动模式、Profile、目标程序、
进程 ID、错误和退出码。为了避免泄露视频网站令牌或隐私参数，日志不会写入
完整 URL 或原始命令行，仅记录待透传参数的字符数量。

## 命令行示例

```text
MPVBridge.exe --bridge-profile=HQ_Anime "D:\Video\ep01.mkv" --fs
MPVBridge.exe --bridge-profile="Light_Player" "https://example/video"
```

只有 Bridge 专用参数会被移除；其余原始命令行字符将追加到目标
`mpv.exe` 后。目标工作目录固定为 `mpv.exe` 的父目录，stdin/stdout/stderr
和可继承句柄会传递给 MPV。

## Visual Studio 2026 构建

打开 `MPVBridge.slnx`，选择 `x64`，构建 Debug 或 Release。项目已配置：

- MSVC `v145`、ISO C++20、Unicode 和 `/utf-8`。
- `/SUBSYSTEM:WINDOWS` 与 `wWinMain`，不会创建控制台窗口。
- 源码、头文件和 Win32 资源统一保存在 `src` 目录，仓库根目录只保留构建入口与文档。
- `AdditionalManifestFiles` 嵌入 `src\app.manifest`。
- Common Controls v6、PerMonitorV2 DPI、`asInvoker` 与圆角窗口。
- 内嵌包含 16–256 像素层级的 `src\app.ico`，供资源管理器、窗口和任务栏使用。
- Release 全程序优化、COMDAT 折叠和无引用代码移除。
- 仅链接 Windows 系统库，不需要分发额外 DLL。
- 环境安装器通过系统 WinHTTP 下载，并调用 Windows PowerShell 的 `Expand-Archive`
  解压 FFmpeg；MPVBridge 自身仍不依赖第三方运行时 DLL。
- 示例 `profiles.ini` 保留在项目目录中；构建不会覆盖输出目录里正在使用的配置。

输出位于 `x64\Debug` 或 `x64\Release`。
