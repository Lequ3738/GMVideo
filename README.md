# GMVideo

为 GameMaker 8.0 打造的 MP4 视频播放插件。

基于 [Windows Media Foundation](https://learn.microsoft.com/zh-cn/windows/win32/medfound/media-foundation-programming--essential-concepts) 解码，画面输出到 GM8 surface，音频经 [bass](https://www.un4seen.com/bass.html) 播放，支持循环、暂停恢复与倍速。

## 特性

- **开箱即播**：主流 MP4（H.264 / AAC）直接播放，解码器来自操作系统
- **surface 输出**：视频帧写入 GM8 surface，可像普通 surface 一样绘制、缩放、混合
- **秒开**：后台线程边解边播，不落地临时文件、不做全量预解，大视频瞬间开始播放
- **音画同步**：以 bass 播放进度驱动选帧，循环、暂停恢复、倍速切换全程保持连续
- **双后端**：同时适配 GM8 的 D3D8 / D3D9（[GMDirectX9](https://github.com/Lequ3738/GMDirectX9)） 渲染后端

## 环境要求

- GameMaker 8.0，Windows Vista 及以上
- 构建：Visual Studio 2022（v143 工具集 + Win10 SDK）
- 运行时依赖：
  - `bass/bass.dll`（约 139KB，bass 2.4.18）——复制到游戏 exe 目录

## 编译

VS2022 打开 `GMVideo.sln`，选择 `Release | x86` 直接生成，产物位于 `Release/GMVideo.dll`。

也可以用命令行：

```
MSBuild GMVideo.sln /p:Configuration=Release /p:Platform=x86
```

## 快速上手

1. 把 `GMVideo.dll`（以及 `bass.dll`）复制到游戏 exe 同目录；
2. 用 `external_define` 绑定导出函数，完整绑定代码见示例工程的 `Test/GMVideoTest.gm80/objects/objVideoCtrl.gml`；
3. 参照下面的顺序调用：

```gml
// Create 事件
video_dll_init();                      // 初始化 Media Foundation，进程内一次即可
video_play(working_directory + "\intro.mp4", true);

// Step 事件：每帧推进解码与上传
video_update();

// Draw 事件：取出 surface 绘制
surf = video_get_surface();
if (surface_exists(surf))
	draw_surface(surf, 0, 0);
```


## API

| 函数 | 说明 |
|---|---|
| `video_dll_init()` | 初始化插件 |
| `video_dll_free()` | 释放插件 |
| `video_play(path, loop)` | 打开 MP4 并开始播放 |
| `video_update()` | 每帧调用：推进视频播放 |
| `video_pause()`<br>`video_resume()`<br>`video_stop()`<br>`video_reset()` | 播放控制 |
| `video_free()` | 释放视频资源 |
| `video_get_surface()` | 视频 surface id，无效时返回 `noone` |
| `video_get_width()`<br>`video_get_height()`<br>`video_get_fps()`<br>`video_get_frames()`<br>`video_get_position()`<br>`video_get_speed()`<br>`video_get_loop()`<br>`video_get_duration()` | 状态查询 |
| `video_set_speed(speed)` | 倍速（>0），音频与视频一起变速 |
| `video_set_loop(loop)` | 循环开关 |


## 已知限制

- 可播放格式取决于系统解码器：MP4 / H.264 / AAC 开箱即用，HEVC / VP9 需安装 Windows 扩展。
- 无 DXVA2 硬件解码，4K 软解较吃力；1080p 在现代 CPU 上余量充足。
- 倍速播放时音调随速度变化。
