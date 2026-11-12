# YVR Bridge 已验证案例：Virtual Desktop on YVR 1

> 本文记录 YVR Bridge 的首个成功适配案例。这里的版本、路径映射、二进制哈希与修复规则属于该案例；其他应用需要独立验证。本文的命令均在源码包根目录执行。

这是一次已在 **YVR 1** 上完成基础显示与交互实测的移植方法，以及对应的应用级 OpenXR 兼容层源码。目标是修复启动前退出、手柄绑定、环境黑屏和投影反转；不是 Virtual Desktop 的完整开源实现，也不代表官方支持。

**验证日期：2026-11-12。** 用户已在头显中确认：场景可见，手柄/射线方向正确，菜单显示与点击位置一致，切换场景、悬停、按扳机和转头不再触发镜像反转或异常跟头。电脑桌面串流、SteamVR、长期稳定性与性能仍未完成验证。

## 1. 开源内容与兼容范围

本目录按 MIT 许可证发布本项目编写的兼容层、构建/部署脚本、启动属性模块和文档。第三方软件、SDK、原应用及系统运行库不受本许可证覆盖，也不包含在源码包中。使用者需自行准备有权使用的应用与原加载器。本项目不修改账号、授权、签名校验或付费功能。

| 项目 | 本次实测 |
| --- | --- |
| 设备 | YVR 1，arm64，设备代号 kona |
| 系统 | Android 10 / API 29，固件 1.3.0.499，原始 incremental=499 |
| 应用 | VirtualDesktop.AndroidPlus，1.34.25.0，versionCode 10723 |
| 已有分发层 | 本次环境已有 overport 3.4.2-patched，由其选择 YVR 运行时 |
| 系统运行时 | YVR loader 1.6.0；libvrruntimeclient.so / libvrruntimeservice.so |
| 本机工具链 | Linux x86_64，Android NDK 27.2.12479018，OpenXR 头文件 1.0.34 |
| 已验证运行参数 | 72 Hz 曾单独验证；后续设置为 90 Hz。最终修复不要求固定纯黑环境或 72 Hz |

构建脚本锁定本次使用的原始 arm64 加载器 SHA-256：


```text
da9b5977f24241eb6fb790b914bd0d8d70839f59fa826d7f1ea55379bcd65d09
```

脚本会拒绝原加载器哈希不同的输入；应用和固件版本需要使用者另行核对，不由构建脚本自动检测。其他组合必须重新核对导出、ABI 和图层行为，不能只改哈希就宣称兼容。原加载器必须已能选中并初始化 YVR 运行时；本适配器不提供通用的 Quest 运行时替代实现。

## 2. 修复思路

### 2.1 启动前播放更新提示后退出

该组合把 YVR 识别为 Pico，并把 Android 的 Build.VERSION.Incremental 与 Pico 的时间戳阈值 1692715005 比较。YVR 的 499 被误判为固件过旧，应用播放更新提示后在初始化头显之前退出。

已验证的规避方法是在 zygote 启动之前通过 Magisk post-fs-data 把 ro.build.version.incremental 设为 1692715005，然后重启。单纯在运行中 resetprop 不足以更新 zygote 已缓存的 Build.VERSION。

**这是设备级属性修改，会影响其他应用读取该属性。** 仅对出现同一问题的测试组合使用。移除模块并重启可撤销。本项目未修改系统 VR 服务或其二进制。

### 2.2 手柄模型缺失、无法点击

应用 Pico 分支提交的 neo3_controller 交互配置在该 YVR 运行时不受支持。适配器仅在设备品牌包含 yvr、且应用提交对应 Pico Neo3 建议绑定时：

- 将交互配置映射为 /interaction_profiles/oculus/touch_controller。
- 将左手 /input/back/click 映射为 /input/menu/click。
- 若存在右手 /input/back/click，则剔除该项。

保留 aim/grip pose、模拟扳机、握键、摇杆、触摸和震动的 action 类型。没有伪造追踪有效位，也没有用固定坐标或旋转偏移“矫正”手柄。应用既有的模拟扳机阈值点击路径继续工作。

### 2.3 切换天空场景后黑屏

SpaceSky、PurpleNebula 的原生 cube 图层尝试使用 ASTC 4×4 sRGB（GL 0x93d0）交换链，YVR 拒绝该格式，后续出现空交换链和无效图层。

仅对 YVR 从扩展枚举中隐藏 XR_KHR_composition_layer_cube，应用便启用自己的双眼投影回退：GPU 正常采样 ASTC 天空纹理，并写入普通 RGBA/sRGB 渲染目标。**不把 ASTC 冒充为 RGBA，也不把所有场景替换为黑色。**

### 2.4 场景、手柄与射线反转，悬停/扳机后画面异常跟头

原生只读调查确认：YVR 服务的 Quad 合成会处理 CompositionLayerImageLayoutFB.VerticalFlip，但 Stereo Projection 合成没有读取同一扩展标志。菜单与三维画面因此产生不一致。

本次固件中 setQuadLayerXR 位于 0xb10b0，setStereoLayerXR 位于 0xaeef8。comp_layer.data 起于 +96；基础 flip_y 在 comp+116，扩展存在位在 +172，扩展值在 +176，额外翻转在 +224。Quad 会合并这些字段，Stereo 只读取基础翻转。这些偏移只用于解释当前二进制，不能跨版本照搬。

v5 在 xrEndFrame 中识别实际请求 VerticalFlip 的投影交换链，记录 GLES 纹理和 FIFO 获取/成功等待状态；在真正 xrReleaseSwapchainImage **之前**，用同格式临时纹理及两次 framebuffer blit 做一次物理上下翻转。保存并恢复读/写 framebuffer、纹理绑定和裁剪开关。保留 sRGB 与 alpha，不做 CPU 回读或强制 glFinish。

修正只作用于已识别的投影纹理，保留原始头部/手柄姿态、逻辑光标、菜单图层和提交空间。仅删除翻转标志不会奏效，因为 Stereo 路径本来就在忽略它。实测 VIEW 空间眼姿态正常，“重复应用头部姿态”的假设已排除。

### 2.5 菜单沉到脚下是另一个问题

本机曾保存 PositionOffset.Y=-21.263536 米；这是持久化设置异常，与上述图层翻转分开处理。停止应用、备份 ScreenSettings.json 后，曾把 PositionOffset 归零、Orientation 设为单位矩阵、ScreenHeight 设为 0，并保留原距离。用户确认恢复。

不要照抄 2.872254 米等本机数值，也不要清除应用数据。优先使用应用的重置位置功能；只有确认配置异常并已备份时才修改对应键。脚本不会重置用户设置。

## 3. 源码结构


```text
.
├── README.md
├── LICENSE
├── adapter.c
├── build.py
├── deploy.py
├── .gitignore
└── magisk/vd_incremental_spoof/
    ├── module.prop
    └── post-fs-data.sh
```

适配器保留原加载器全部 56 个 OpenXR 导出。未挂钩入口通过 AArch64 尾跳转转发；xrGetInstanceProcAddr 返回的相关入口也被接管。原加载器仅改名为 libvd_original.so，仍负责选择 YVR 运行时。非 YVR 设备转发到原实现。

## 4. 构建

准备 Python 3.9+、adb、上述 NDK 和 OpenXR SDK 头文件。以下命令在本源码目录执行，路径按自己的环境填写；原库需由使用者自行取得，项目不提供下载。

```bash
python3 build.py \
  --original "/absolute/path/to/your/libopenxr_loader.so" \
  --ndk "/absolute/path/to/android-ndk/27.2.12479018" \
  --openxr-include "/absolute/path/to/OpenXR-SDK/include"
```

输出位于 build/：

- libopenxr_loader.so：本项目编译的应用级代理。
- libvd_original.so：从使用者原库生成的改名副本，**不可作为本项目源码再分发**。
- manifest.json：原件、改名副本、适配器的 SHA-256、导出及构建参数。
- exports.h、forward.S、exports.map：生成文件。

脚本校验原库哈希、SONAME 替换次数与 56 个导出，并使用 -Wall -Wextra -Werror 和 --no-undefined 构建。运行库依赖 Android 的 EGL/GLESv3。

## 5. 首次部署与检查

需要已授权 adb，以及能够写入当前应用原生库目录的 root/Magisk。此流程不是免 root 安装方法，也不会生成重签 APK。部署会停止目标应用。先确认应用版本和设备，再执行：

```bash
adb devices
python3 deploy.py status --serial YOUR_ADB_SERIAL
python3 deploy.py deploy --serial YOUR_ADB_SERIAL
python3 deploy.py status --serial YOUR_ADB_SERIAL
```

脚本从 pm path 动态解析当前安装目录，不包含固定设备序列号、用户名或随机安装路径。实际修改限于该安装实例的 lib/arm64/libopenxr_loader.so，并新增 libvd_original.so；不会替换系统 VR 库。

首次部署将原库备份到设备 /data/local/tmp/vd_yvr2_backup_<原库哈希前12位>/；随后还会保留本机 build/original-device-backup.so。每次写入检查哈希，并设置 system:system、0755、apk_data_file 的 SELinux 上下文。status 仅普通 adb 读取，不调用 su，避免 Magisk 窗口抢走 VR 焦点。

若已有其他补丁、备份缺失或当前库哈希不认识，部署会拒绝覆盖。迁移旧版本时必须先核实原件备份；可用 --backup-dir 指定已确认的 /data/local/tmp/ 下备份目录。不要把当前代理当作原件备份，也不要删除校验来强行安装。

部署成功后从头显正常启动应用。脚本不会安装 Magisk 模块，也不会清除或覆盖用户配置。重新安装/升级 APK 可能覆盖补丁；需要重新检查版本、原库哈希和安装路径。

### 可选：启动属性模块

仅在第 2.1 节问题确实存在时使用。模块源码位于 magisk/vd_incremental_spoof/。可在本机生成标准模块 ZIP，通过设备现有的 Magisk 界面安装并重启：

```bash
chmod 755 magisk/vd_incremental_spoof/post-fs-data.sh
(cd magisk/vd_incremental_spoof && zip -r ../../vd_incremental_spoof.zip module.prop post-fs-data.sh)
adb -s YOUR_ADB_SERIAL push vd_incremental_spoof.zip /sdcard/Download/
```

不要重复安装已有的同 ID 模块。卸载该模块后需再次重启才能撤销缓存属性。不要在已正常启动的其他系统版本上盲目应用。

## 6. 回退

```bash
python3 deploy.py rollback --serial YOUR_ADB_SERIAL
```

脚本先校验备份与当前库，再停止应用并还原原始加载器；无用的 libvd_original.so 可以保留。回退原库也会撤销手柄、天空回退和投影修正。属性模块需单独卸载并重启。部署不是事务操作；若中途失败，保留 manifest 与原库备份，先检查设备当前哈希再恢复，不要反复盲写。

## 7. 验证清单与证据边界

发布整理时已用上述工具链重新编译便携源码，并确认 adapter.c 与头显验证过的 v5 完全一致。便携部署脚本的状态读取、备份部署、回退和未知库拒绝已用模拟设备验证；当时 adb 设备离线，本轮没有用该便携脚本重新写入头显。前述实际显示结论来自已部署 v5 的用户测试。

1. 启动不再播放错误更新提示后退出。
2. 手柄模型和射线出现，扳机能点击菜单。
3. 切换 SpaceSky/PurpleNebula 等天空环境后仍可见。
4. 左右转头、把指针移到菜单上、按扳机：场景不反转，手柄方向正确，菜单显示与命中位置一致。
5. 之后单独测试电脑发现、桌面串流、SteamVR、延迟与长时间稳定性；这些不能由菜单正常推断。

查看关键日志：

```bash
adb -s YOUR_ADB_SERIAL logcat -s VDYVRInput overportOXR oxr_error oxr_logger
```

本机四条 1504×1576、GL_SRGB8_ALPHA8 投影链均报告 GPU 修正启用且 FBO 完整，包括声明 samples=4 和 samples=1 的链。代码未调用 glGetError，FBO 完整本身不能证明每次复制无错误；最终画面由用户头显复测确认。启动早期仍可能有一次 60 Hz 请求被拒绝，以及原分发层额外尝试 Pico 配置的错误，不能只凭存在这些日志就判定最终绑定或显示失败。

## 8. 已知限制

- 当前面向已测试的动态、全幅、普通 2D 投影纹理；不是任意 OpenXR 应用的通用修复。
- 新投影链第一次被识别前，可能有一帧尚未翻转。
- 不覆盖静态一次性投影、局部 ImageRect、纹理数组、多 EGL 上下文或并发访问。
- 投影标记在该交换链生命周期内保持；同一链若在投影与普通菜单间复用，需要进一步实现分类变更。
- GPU 翻转增加两次 blit；长期功耗、发热、串流性能未完成测量。
- Android 之外的宿主、其他 YVR 固件/设备和其他应用版本未验证。
- 只读诊断会输出有限次数的姿态与图层信息；公开日志前需自行去除设备信息等隐私数据。

## 9. 发布与贡献

只发布本目录的源文件、文档与 LICENSE；不要把整个调试工作区上传。禁止混入 APK、原应用/系统 .so、反编译 DLL/C#、设备日志、用户配置或账号信息。build/ 已加入忽略规则，但复制文件前仍需检查发布清单。

建议反馈时说明设备/固件、应用版本、原库哈希、NDK/头文件版本、具体复现步骤及已脱敏的关键日志，并区分“日志显示初始化成功”和“头显实际显示正确”。

许可证见 LICENSE。MIT 仅覆盖本项目原创内容，不授予第三方应用或运行库的分发权。
