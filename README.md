# GMVideo —— GM8 MP4 视频播放插件（Media Foundation）

独立 DLL 插件：为 GameMaker 8.0 提供 MP4（H.264/AAC）视频播放能力，基于 Windows Media Foundation（系统自带，0 字节分发）。

## 工程结构

```
C:\Project\C++\GMVideo\
├── GMVideo.sln / GMVideo.vcxproj   # VS2022 工程（v143 / Win10 SDK）
├── dllmain.cpp                     # DllMain: GMAPI 自举 + COM/MF 初始化
├── Main.h                          # GMReal/GMString/expReal 宏 + simplecatch
├── GMVideo.cpp                     # GM 导出 API（GMVideo* 前缀）
├── VideoMF.h / VideoMF.cpp         # MF SourceReader 解码核心（NV12 输出 + 音频 PCM）
├── BassOut.h / BassOut.cpp         # bass push stream 音频输出（后台线程填充）
├── bass\                           # bass SDK（bass.h / bass.lib / bass.dll）
├── d3d_adapter.*                   # D3D8/9 双后端适配（从 NatureEnhance 拷贝）
├── GMAPI/  GMAPICore/  Direct3D_8/  Direct3D_9/   # 依赖（整体拷贝）
├── Release/GMVideo.dll             # 编译产物（x86 Release）
└── Test/
    ├── GMVideoTest.gm80/           # GM8 测试工程（目录即工程）
    ├── test.mp4                    # 测试视频（ffmpeg 生成，H.264+AAC 10s）
    ├── bass.dll                    # bass 运行时（复制到 exe 目录）
    ├── mf_selftest.cpp             # 解码核心自测（NV12/BGRA/音频提取/Seek，软件解码）
    ├── audio_test.cpp              # 完整链路自测（MF 音频 + bass + 位置反馈节流 + 暂停恢复）
    ├── bass_test.cpp               # bass push stream 自测（正弦波 + 暂停恢复）
    ├── compare_check.cpp           # 软解帧校验（NV12 尺寸 + BGRA 非黑 + 通道完整）
    ├── stress_test.cpp             # 并发压力自测（视频解码 + 音频线程 + 随机 Seek）
    ├── sync_stress.cpp             # 暂停/恢复同步自测（反复暂停不累积漂移）
    ├── sync_bench.cpp              # 主线程负担 benchmark（测 update 耗时/帧率/CPU 并行度）
    ├── loop_test.cpp               # 循环播放同步自测（循环重置音画同步，old/new 对比）
    ├── loop_bug_test.cpp           # 循环静音回归（水位计数循环清零，old/new 对比）
    ├── speed_test.cpp              # 倍速设置同步自测（变速瞬间连续，old/new 对比）
    └── MfSelftest.vcxproj / AudioTest.vcxproj / BassTest.vcxproj   # 自测工程
```

## 编译

VS2022 打开 `GMVideo.sln`，选 `Release | x86`（GM8 是 32 位），生成 → `Release/GMVideo.dll`。
命令行：`MSBuild GMVideo.sln /p:Configuration=Release /p:Platform=x86`

**运行时需把 `bass\bass.dll` 复制到游戏 exe 目录**（仅这一个外部 DLL，约 139KB，bass 2.4.18）。bass_fx.dll 不需要（音频变速用 bass 核心采样率变速）。

**D3D9 后端上传**: GM8 的 surface 纹理是 `D3DPOOL_DEFAULT`（不可 `LockRect`），把帧数据写入它只能经 D3DX9（`D3DXLoadSurfaceFromMemory`）——纯 D3D9 API 无法把内存像素写入不可锁表面（`UpdateSurface` 要求源/目标同格式且目标非 RT；`StretchRect` 源必须是 RT）。因此 **`D3DX9_43.dll` 是 D3D9 后端的必需依赖**（GMDirectX9 的 gex 已自带），上传统一走 D3DX9，不做优先尝试。

## 解码核心自测（不依赖 GM8）

```
MSBuild Test\MfSelftest.vcxproj /p:Configuration=Release /p:Platform=Win32
Test\Release\MfSelftest.exe Test\test.mp4

MSBuild Test\AudioTest.vcxproj /p:Configuration=Release /p:Platform=Win32
Test\Release\AudioTest.exe Test\test.mp4

MSBuild Test\CompareCheck.vcxproj /p:Configuration=Release /p:Platform=Win32
Test\Release\CompareCheck.exe Test\test2.mp4

cl /nologo /std:c++20 /EHsc /utf-8 /I.. Test\stress_test.cpp VideoMF.cpp /Fe:stress_test.exe mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib
Test\stress_test.exe Test\test.mp4

cl /nologo /std:c++20 /EHsc /utf-8 /I.. /I..\bass Test\sync_stress.cpp VideoMF.cpp BassOut.cpp /Fe:sync_stress.exe mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib bass\bass.lib
Test\sync_stress.exe Test\test.mp4 0 3.0 2.0 5 1.5

cl /nologo /std:c++20 /EHsc /utf-8 /I.. /I..\bass Test\loop_test.cpp VideoMF.cpp BassOut.cpp /Fe:loop_test.exe mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib bass\bass.lib
Test\loop_test.exe Test\test.mp4 new

cl /nologo /std:c++20 /EHsc /utf-8 /I.. /I..\bass Test\loop_bug_test.cpp VideoMF.cpp BassOut.cpp /Fe:loop_bug_test.exe mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib bass\bass.lib
Test\loop_bug_test.exe Test\test.mp4 new

cl /nologo /std:c++20 /EHsc /utf-8 /I.. /I..\bass Test\speed_test.cpp VideoMF.cpp BassOut.cpp /Fe:speed_test.exe mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib bass\bass.lib
Test\speed_test.exe Test\test.mp4
```

MfSelftest：解码前 5 帧、NV12 尺寸、BGRA 转换、音频提取 WAV、Seek。AudioTest：模拟后台线程边解边喂 bass，验证播放位置实时推进与暂停恢复。CompareCheck：软解帧校验（NV12 尺寸 + BGRA 非黑）。stress_test：视频解码 + 音频线程 + 随机 Seek 并发压力（验证锁安全）。sync_stress：反复暂停/恢复验证同步漂移不累积。loop_test：循环播放同步（`old` 复刻旧逻辑复现 +9.9s 漂移，`new` 修复后 +0.000s）。loop_bug_test：循环后音频恢复（水位计数清零）。speed_test：倍速同步（音频视频一起变速、切换连续）。全部通过输出 `=== ALL PASS ===`。

## 在 GM8 中使用

1. 把 `GMVideo.dll` 复制到游戏 exe 同目录。
2. 用 `external_define` 绑定导出函数（详见 `Test/GMVideoTest.gm80/objects/objVideoCtrl.gml` 的 Create 事件）。
3. **调用顺序**：
   - `GMVideoInit(tempPath)` —— 一次性 MF 初始化；`tempPath` 参数保留（ABI 兼容）但不再使用。
   - `GetGMWindowsHandle(window_handle())` —— 传入 GM 窗口句柄，插件据此取 D3D 设备。
   - `GMVideoPlay(path, loop, interframe)` —— 打开 MP4 开始播放。
   - 每帧调用 `GMVideoUpdate()`（放 Step 事件）。
   - 用 `GMVideoGetSurface()` 拿 surface，`draw_surface` 绘制。

### 导出函数一览

| 函数 | 参数 | 说明 |
|---|---|---|
| `GMVideoInit` | tempPath | MF 初始化；tempPath 保留但忽略 |
| `GetGMWindowsHandle` | windowHandle | 取窗口句柄 + D3D 设备 + 后端判定 |
| `GMVideoShowErrors` | mode | 0/1 开关错误弹窗 |
| `GMVideoPlay` | path, loop, interframe | 播放；interframe 保留但忽略（MP4 自带运动补偿） |
| `GMVideoUpdate` | — | 每帧推进：同步选帧 → 解码 → 上传表面 |
| `GMVideoPause / GMVideoResume / GMVideoStop / GMVideoReset` | — | 播放控制 |
| `GMVideoFree` | — | 释放全部资源 |
| `GMVideoGetSurface` | — | 视频 surface id。注意：GM8 的 surface id **从 0 开始**，无效时返回 `noone`(-4)；GM 侧判断用 `if (surf != noone)` 而非 `surf > 0` |
| `GMVideoGetSoundtrack` | — | 恒返回 `noone`（音频由插件内部 DirectSound 直播，不暴露 handle） |
| `GMVideoGetWidth / Height / FPS / Frames / Position / Speed / Loop / Duration` | — | 查询 |
| `GMVideoSetSpeed` | speed | 倍速（>0）。音频与视频一起变速（bass 采样率变速，音调随速度变化） |
| `GMVideoSetLoop` | loop | 循环开关 |

### 同步模型

- **有音轨**：后台线程把 SourceReader 音频流边解边喂 bass push stream（`BASS_StreamPutData`），主线程用 `BASS_ChannelGetPosition` 累计进度驱动帧选择。**不落地文件、不全量解，大视频瞬间开始播放**；填充由**队列水位反馈**控制（已入队 − 已播 ≤ 0.9s 预填窗口），精确匹配播放速率，杜绝 `Sleep` 估算误差导致的丢块（实测旧节流长视频 40s 后丢 ~0.2s 音频、音频持续变快；位置反馈后 90s 零丢块）。
- **无音轨/音频创建失败**：墙钟（`QueryPerformanceCounter`）推帧。
- **倍速**：音频与视频一起变速。音频用 bass 核心采样率变速（`BASS_ATTRIB_FREQ = 采样率 × speed`），`GetPosition` 返回变速后的输出进度，视频直接跟它——切换倍速瞬间画面连续不跳、音画同步。音调随速度变化（磁带效果）。不用 bass_fx tempo（音调不变）：它需要可流式 source，本插件的 push stream 不支持（实测 `BASS_ERROR_UNSTREAMABLE`）。
- **循环**：视频 Seek(0) + 音频流重绕 + bass 重建 stream（`Reset()`，彻底清空队列/输出缓冲）。节流水位计数 `g_pushedBytes` 同步归零——否则音频线程按旧累计算出巨大水位、内层循环首行就退出，bass 空队列 stall，循环后音频静音、视频停开头（`loop_bug_test` 覆盖此回归）。
- **暂停/恢复**：`BASS_ChannelPause` / `BASS_ChannelPlay(FALSE)`，位置冻结、恢复连续。
- **与已有 bass 共存**：`BASS_Init` 重复调用返回 `BASS_ERROR_ALREADY`，视为已初始化直接复用（游戏里 NatureEnhance/MaizeMusic 可能已初始化 bass），且不 `BASS_Free` 避免杀掉别的流。

### 解码方式

全软件解码（Media Foundation 系统软解解码器），D3D8/D3D9 后端通用。不启用 DXVA2 硬解——实测在 GM8 环境硬解首帧初始化阻塞 ~23 秒、循环 Seek 内存持续增长；软解首帧 <50ms、内存稳定不增长，1080p 约 3ms/帧远快于实时。

## GM8 测试工程

打开 `Test/GMVideoTest.gm80` 目录（GM8 打开 .gm80 工程），把 `GMVideo.dll` 和 `test.mp4` 放到运行目录（F5 调试的 exe 目录，或编译后的 exe 目录），运行即可看到 640×360 测试视频循环播放。

- **空格**：暂停 / 恢复
- **↑ / ↓**：倍速 ×2 / ÷2（0.25× ~ 4×）
- **←**：切换循环
- **R**：重置
- **Esc**：退出

## 已知限制

- 格式覆盖 = 系统自带解码器（MP4 / H.264 / AAC 主流覆盖；HEVC/VP9 需 Windows 扩展）。
- 全软件解码，双后端（D3D8/D3D9）通用；无 DXVA2 硬解。
- 4K 软解吃力；1080p 现代 CPU 无压力。
- 音频经 MF 直播到 bass push stream，运行时需 bass.dll；倍速音频随视频一起变速（音调变化）。
