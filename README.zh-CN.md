# 釉光 · Overglaze

[English](README.md)

釉光 · Overglaze 是一个实验性的第三方工具：在 DX12 单机游戏里运行 NVIDIA 的 NR（Neural Rendering，神经渲染）模型，并在游戏内直接控制它。

*名字来自陶瓷工艺：釉面烧成之后，再在釉面上加绘一层彩。NR 做的事情很像——在游戏已经渲染完成的画面上，再画一层。*

> 釉光不是 NVIDIA 的产品，与 NVIDIA 及任何游戏开发商、发行商都没有关联，也未获其认可。本项目不附带 NR 模型，需要你自行准备。

## 0.2.0-preview.2

这个预览版重新整理了桌面游戏库和游戏内面板，统一使用炭灰与浅绿配色，加入原创的双层方环图标。游戏库可以搜索、按状态筛选，并为选中的游戏显示下一步操作；检查明细和安装信息按需展开。「设置与关于」可查看本地模型状态。游戏内面板集中呈现常用 NR 控件，模型读取回执和排错信息收进「高级与诊断」。

新增 Windows 安装包：可选择安装位置，创建快捷方式，支持升级和卸载；首次启动可直接选择并导入自己的模型。更新保留模型、设置和采集资料，游戏里的插件在游戏库中分别更新。见[安装、升级与卸载](docs/INSTALLER.md)。便携 ZIP 继续提供，附带文件哈希清单。模型仍需自行提供，本次预览不新增游戏兼容性、实机画质或性能结论。

## 它做什么

游戏本身要用 DLSS 光线重建（Ray Reconstruction）或 DLSS 超分辨率（Super Resolution）。釉光在游戏的上采样器算完一帧之后，把这一帧交给 NR 模型（也就是常说的「DLSS 5」），再把结果写回去；之后游戏照常做它自己的后期和 UI。

- **NR 默认关闭**，每次启动游戏都是关的。
- **游戏内面板，按 Insert 打开**：NR 开关、Tone、Structure、Style、Skin 与 AutoMask、输入曝光，以及左右分屏对比。
- **面板显示的是实际发生了什么**，不只是你请求了什么：模型读到了哪些参数、跳过了多少帧、为什么跳过。
- **桌面程序**：在「游戏库」检查游戏、把釉光的文件装进去、更新、卸载；「采集浏览」单独打开兼容的已保存采集包。

## 它不是什么

- **不是官方接入。** 釉光直接加载模型，不走 NVIDIA 自己的接入路径，输入准备和颜色处理都是自己写的。效果不等同于官方支持 NR 的游戏。
- **不是超分，也不是帧生成。** 游戏必须自己开着 DLSS 光线重建或超分；没有 DLSS 的游戏，釉光不会给它加上。
- **不用于联网游戏，也不用于带反作弊的游戏。** 见 [POLICY.md](POLICY.md)。
- **不提供模型下载，也不能让其他显卡跑 NR。**
- **不保证画质。** 有的游戏变化很明显，有的几乎看不出来；游戏给的输入和模型的预期对不上时，可能出现噪点或拖影。

## 运行要求

- NVIDIA GeForce RTX 50 系显卡（在 RTX 5090 上测试）。不支持其他显卡。
- NVIDIA 驱动 615 或更新（在 617.14 上测试）。
- 64 位 Windows 11（目前只在这个系统上测试过）。
- Microsoft Visual C++ v14 x64 运行库。安装包和带运行库的 ZIP 将微软签名的 DLL 放在程序旁边；不安装全局运行库。不带这些 DLL 的自建包仍需另行安装匹配的运行库，见 [微软官方下载说明](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)。
- 使用 DLSS 光线重建或超分的 DX12 游戏，经由 Streamline 或直接调用 NGX 都可以。见 [SUPPORTED_GAMES.md](SUPPORTED_GAMES.md)。
- 你自己的 `nvngx_dlssnr.dll`（见下文「模型」）。
- 磁盘余量：安装时会在釉光数据文件夹所在的盘上保留一定空间（目前是 30 GiB）。

NR 很吃性能。我们在 RTX 5090、约 5K 输出下实测，每个渲染帧多出大约 9–12 ms 的 GPU 时间，帧率会明显下降。

## 模型

本项目不包含、也不分发 `nvngx_dlssnr.dll`。这个文件属于 NVIDIA，请自行准备。

釉光会把文件的 SHA-256 和已知版本逐一核对，对不上的一律拒绝，改动过的文件也一样。详见 [docs/MODEL.md](docs/MODEL.md)。

## 五步上手

1. **获取程序。** 运行 Setup，选择本地安装目录；也可以解压整个便携 ZIP。通过快捷方式或便携启动入口打开。
2. **导入模型。** 在首次配置或「设置与关于」中选择你自己的模型 DLL，程序校验后导入模型目录。手动放入该目录也可以。
3. **添加游戏。** 在「游戏库」点「添加游戏」，粘贴游戏的安装目录或 EXE 路径。程序会做一次只读检查：商店、NVIDIA 模块及其签名、反作弊与 Denuvo 标记、目录里有没有别的注入工具、游戏走哪条 DLSS 路线。
4. **安装。** 先退出游戏，在游戏库选中它，点「准备安装」生成适配包，再点「安装」并核对确认内容。釉光只写自己的文件，并记下写了哪些，以后卸载时只删这些。
5. **进游戏。** 照常启动游戏，在图形设置里打开 DLSS：有光线重建就开光线重建，没有就用超分或 DLAA。按 **Insert** 打开面板，打开 NR 开关。

### 需要晚加载的游戏

有的游戏不接受游戏目录里出现代理 DLL。这类游戏由釉光在游戏启动之后再加载进去，游戏根目录里不放任何文件。设置要用命令行工具 `overglaze_games.exe` 完成（见 [docs/ADDING-A-GAME.md](docs/ADDING-A-GAME.md)）。Steam 游戏在启动选项里填：

```
"<釉光所在文件夹>\app\overglaze_launch.exe" %command%
```

游戏照常启动；进游戏后第一次按 Insert，釉光才加载并打开面板。

### 移除釉光

**先**在「游戏库」逐个卸载，**再**移动或删除釉光的文件夹。指向 `overglaze_launch.exe` 的 Steam 启动选项也要删掉，否则 Steam 会启动不了那款游戏。

## 游戏内面板

| 控件 | 作用 |
|---|---|
| 启用 Neural Rendering | NR 开关。每次启动游戏都是关的。 |
| Tone（0–2，默认 1） | 设置 `DLSSNR.LocalToneStrength`。我们测下来主要影响色彩和色调。 |
| Structure（0–2，默认 1） | 设置 `DLSSNR.LocalStructureStrength`。我们测下来主要影响细节。 |
| Style（0、1、2） | 设置 `DLSSNR.Style`。三档没有对应到任何官方名称。 |
| AutoMask 与 Skin（0–2） | 设置 `DLSSNR.UseAutoMask` 和 `DLSSNR.SkinStructureStrength`。Skin 只在 AutoMask 打开时生效，0 表示几乎不动皮肤；拖动 Skin 会自动打开 AutoMask。 |
| 输入曝光 | 釉光自己的颜色预处理，不是模型参数。可以手动设定档数（stop），也可以自动测光，这时滑条是在自动值上的偏移。 |
| 对比分屏 | 左半边原图，右半边 NR 结果。仅用于诊断。 |
| 仅计算 | 照常运行 NR，但不把结果写回游戏。用来测开销。 |

顶部状态卡区分「请求开启」和实际帧回执。展开「高级与诊断」，可查看模型上一次运行时实际读到的值、跳帧原因，以及「仅计算」模式和面板设置。读取回执只说明接口收到了你的设置，不说明画质如何；Structure 为 0 也不等于关闭 NR。

除开关以外，设置按游戏分别保存。面板快捷键可以改成 F7、F8 或 F9。

## 支持的游戏

已在游戏内确认：

| 游戏 | DLSS 路线 | 加载方式 |
|---|---|---|
| 心灵杀手 2（Alan Wake 2） | Streamline · 光线重建（关闭路径追踪时为超分） | 代理 DLL |
| 光环：战役进化（Halo: Campaign Evolved） | NGX 直连 · 光线重建 | 代理 DLL |
| 地狱之刃 2：塞娜的传说（Senua's Saga: Hellblade II） | NGX 直连 · 超分 | 代理 DLL |
| CONTROL Resonant | Streamline · 光线重建 | 代理 DLL |
| 瘟疫传说：共鸣（A Plague Tale: Resonance） | Streamline · 超分 | 代理 DLL |
| 生化危机 安魂曲（Resident Evil Requiem） | Streamline · 光线重建 | 晚加载（Steam 启动选项） |

另有一些游戏列为实验性。各游戏的说明、商店版本以及「已确认」的含义见 [SUPPORTED_GAMES.md](SUPPORTED_GAMES.md)。这些是已有的逐游戏观察，不等于这个预览版通过了所有游戏的认证。没列出的游戏也可能能用：在「游戏库」添加它，检查结果会告诉你它走哪条路线。

## 政策摘要

- 只用于离线单人游戏。发现已知的反作弊或 Denuvo 标记就拒绝安装；但没发现不等于没有，以你自己的确认为准。
- 不修补、不欺骗、不调试、不 dump 任何 DRM。四款带 Denuvo 的游戏附有需要显式开启的「被动共存」配方，它们只关掉釉光自己可能触发保护的行为；使用风险自负。
- 不修改任何 NVIDIA 文件；认不出的模型文件一律拒绝。
- 不改驱动、系统和安全设置。不联网，没有遥测。
- 安装可以撤回：只写自己的文件，卸载时按记录的哈希删除。
- 修改游戏可能违反游戏的最终用户许可协议或服务条款，这个风险由你承担。

完整内容见 [POLICY.md](POLICY.md)。

## 文档

- [docs/PORTABLE.md](docs/PORTABLE.md)：便携预览版的运行与打包
- [docs/MODEL.md](docs/MODEL.md)：釉光对模型文件的要求
- [docs/ADDING-A-GAME.md](docs/ADDING-A-GAME.md)：添加、安装、更新、移除游戏
- [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)：常见问题
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)：工作原理
- [docs/CONTROL-PROTOCOL.md](docs/CONTROL-PROTOCOL.md)：本机控制管道与命令行客户端
- [CONTRIBUTING.md](CONTRIBUTING.md)、[SECURITY.md](SECURITY.md)、[CHANGELOG.md](CHANGELOG.md)

以上文档目前只有英文版。

## 许可证

釉光的原创代码以 [MIT 许可证](LICENSE) 发布。第三方组件沿用各自的许可证，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。MIT 许可证不覆盖构建时用到的 NVIDIA DLSS SDK 头文件，也不覆盖模型。

## 商标

NVIDIA、GeForce、RTX、DLSS 和 Streamline 是 NVIDIA Corporation 在美国及其他国家的商标或注册商标。游戏名称是各自权利人的商标。釉光与 NVIDIA 及任何游戏开发商、发行商都没有关联，未获其赞助或认可；文中提到这些名称，只是为了说明釉光能配合什么使用。

## 致谢

- [Dear ImGui](https://github.com/ocornut/imgui)（Omar Cornut）：游戏内面板和桌面程序的界面。
- [nlohmann/json](https://github.com/nlohmann/json)（Niels Lohmann）。
- [MinHook](https://github.com/TsudaKageyu/minhook)（Tsuda Kageyu）。
- [ReShade](https://github.com/crosire/reshade)（Patrick Mours）：釉光的绑定状态跟踪改编自它的状态跟踪示例。
- [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline)：公开头文件。
- [BakingLab](https://github.com/TheRealMJP/BakingLab)（MJP）及 Stephen Hill 的 ACES 拟合：用于查看器。
