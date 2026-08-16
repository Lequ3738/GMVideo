#include "Main.h"
#include "VideoMF.h"
#include "BassOut.h"
#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

bool show_error = true;
HWND GMWindowsHandle = nullptr;
void* Device = nullptr;

static VideoMF g_video;
static bool    g_mfReady = false;

static double g_fps = 0.0;
static double g_duration = 0.0;
static int    g_totalFrames = 0;      // 帧总数(最后帧序号 = totalFrames - 1)
static int    g_videoCurrent = -1;    // 已解码到的帧序号
static bool   g_playing = false;
static bool   g_loop = false;
static double g_speed = 1.0;
static double g_frameTime = 0.0;
static double g_lastFrame = 0.0;
static int    g_exportSurface = gm::noone;

// ---- 音频(后台线程 + bass push stream) ----
static BassOut g_audio;
static std::thread g_audioThread;
static HANDLE     g_audioWake = nullptr;       // 唤醒音频线程(auto-reset event)
static std::atomic<bool> g_audioStop{false};
static std::atomic<bool> g_audioPaused{false}; // 暂停: 停止填充, 保持缓冲连续
static std::atomic<bool> g_audioEof{false};    // 音频流读到结尾
static bool       g_audioActive = false;       // 音频线程运行中
static std::atomic<ULONGLONG> g_pushedBytes{0}; // 音频线程累计成功入队字节(节流水位用)

expReal GMVideoFree();

void CatchError(const char* func, const std::exception& e)
{
    if (show_error)
    {
        ShowMessage("在执行函数 " + std::string(func) + " 出现错误：\n" + std::string(e.what()),
            "GMVideo Error", MB_OK | MB_ICONERROR);
    }
}

// 播放中不可恢复错误(解码/上传失败): 熔断 —— 置 g_playing=false 停播并只弹一次窗。
// 不能 throw: GM8 每帧调 GMVideoUpdate, 持续失败会每帧弹窗刷屏卡死。
static bool g_fatalShown = false;

static void FailPlaying(const std::string& msg)
{
    g_playing = false;
    if (!g_fatalShown)
    {
        g_fatalShown = true;
        CatchError("GMVideoUpdate", std::runtime_error(msg));
    }
}

// ---- 基础工具 ----

GMReal TimerGet()
{
    static LARGE_INTEGER freq = { 0 };
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
}

void ShowMessage(std::string&& str, std::string&& caption, UINT type)
{
    int str_size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    int caption_size = MultiByteToWideChar(CP_UTF8, 0, caption.c_str(), (int)caption.size(), nullptr, 0);

    std::wstring wstr(str_size, L'\0');
    std::wstring wcaption(caption_size, L'\0');

    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), wstr.data(), str_size);
    MultiByteToWideChar(CP_UTF8, 0, caption.c_str(), (int)caption.size(), wcaption.data(), caption_size);

    MessageBoxW(GMWindowsHandle, wstr.c_str(), wcaption.c_str(), type);
}

std::string ErrorText(HRESULT hr)
{
    return d3d::error_text(hr);
}

// GM8 字符串是 ACP(中文系统 GBK) -> 宽字符。
static std::wstring ToWide(GMString str)
{
    int len = MultiByteToWideChar(CP_ACP, 0, str, -1, nullptr, 0);
    std::wstring wstr(len > 0 ? len - 1 : 0, L'\0');
    MultiByteToWideChar(CP_ACP, 0, str, -1, wstr.data(), len);
    return wstr;
}

// GM8 的 ACP 路径转 UTF-8: 错误消息/日志里的路径必须与源码 UTF-8 字面量一致,
// 否则 GBK 字节混进 UTF-8 串后 ShowMessage 转码会花屏。
static std::string GBKToUTF8(GMString str)
{
    std::wstring w = ToWide(str);
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string utf8(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, utf8.data(), len, nullptr, nullptr);
    return utf8;
}

// ---- 音频线程 ----

// 后台线程: 从 SourceReader 音频流边解边喂 DirectSound 环形缓冲。
static void AudioThreadProc()
{
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<uint8_t> chunk;

    for (;;)
    {
        WaitForSingleObject(g_audioWake, 30);   // 30ms 轮询: DS 播放腾空间
        if (g_audioStop.load()) break;

        // 推送 PCM。队列水位控制填充速率, 精确匹配 bass 播放速率。
        // 暂停时停手: 不消耗 SourceReader 音频流。
        // 循环重置由主线程 ResetInternal 同步完成(g_video.Seek(0) + g_audio.Reset),
        // 音频线程无需参与; MF 音频流已随 Seek 归零, 这里从 0 继续读。
        while (!g_audioStop.load())
        {
            if (g_audioPaused.load()) break;
            // 已入队未播时长 = 累计入队 - bass 已播位置。达到预填窗口就停,
            // 等 bass 消费后再补。不能用 Sleep 估算节流: MF 每块只有 ~21ms,
            // Sleep 精度误差会让填充快于播放, 队列满时 BASS_StreamPutData 返回
            // -1、被拒 chunk 永久丢失 -> 音频持续变快(实测长视频 40s 后累积 0.2s)。
            double queuedSec = (double)g_pushedBytes.load() / (double)g_audio.ByteRate()
                               - g_audio.GetPositionSec();
            if (queuedSec >= 0.9) break;   // 预填 ~0.9s 音频

            chunk.clear();
            if (!g_video.ReadAudioChunk(chunk))
            {
                g_audioEof.store(true);   // 音频流读完
                break;
            }
            if (!g_audio.Put(chunk.data(), chunk.size()))
                break;   // 队列满(水位已限, 不应发生), 下次循环重试
            g_pushedBytes.fetch_add(chunk.size());   // 累计入队字节(节流水位用)
        }
    }

    CoUninitialize();
}

static void StopAudioThread()
{
    if (g_audioThread.joinable())
    {
        g_audioStop.store(true);
        if (g_audioWake) SetEvent(g_audioWake);
        g_audioThread.join();
    }
    if (g_audioWake) { CloseHandle(g_audioWake); g_audioWake = nullptr; }
    g_audioActive = false;
}

// ---- 帧上传 ----

GMReal BufferToTexture(const void* bgra, GMReal w, GMReal h, GMReal surface)
{
    try
    {
        UINT width = (UINT)w, height = (UINT)h;
        int gmtex = gm::surface_get_texture((int)surface);
        void* texture = gm::CGMAPI::GetTextureArray()[(int)gmtex].texture;

        void* surf = nullptr;
        HRESULT hr = d3d::get_surface_level(texture, 0, &surf);
        if (FAILED(hr))
            throw std::runtime_error("get_surface_level 失败: " + ErrorText(hr));

        RECT rect = { 0, 0, (long)width, (long)height };
        hr = d3d::load_surface_from_memory(surf, &rect, bgra, D3DFMT_A8R8G8B8, width * 4, &rect);
        if (FAILED(hr))
        {
            d3d::release(surf);
            throw std::runtime_error("load_surface_from_memory 失败: " + ErrorText(hr));
        }
        d3d::add_dirty_rect(texture, &rect);
        d3d::release(surf);

        finish;
    }
    simplecatch("BufferToTexture", 0.0)
}

// ---- 内部辅助 ----

static void ResetInternal()
{
    if (g_video.IsOpen())
        g_video.Seek(0.0);

    g_videoCurrent = -1;
    if (g_audioActive)
    {
        g_audio.Reset();
        g_audio.Play(false);
        g_pushedBytes.store(0);
    }

    g_audioEof.store(false);
    g_playing = true;
}

// ---- 导出 API ----

expReal GMVideoInit()
{
    try
    {
        if (!g_mfReady)
        {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (hr == RPC_E_CHANGED_MODE) {}  // 进程已用 STA 初始化(GM8 常见), 接受即可。
            else if (FAILED(hr))
                throw std::runtime_error("CoInitializeEx 失败。");

            hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
            if (FAILED(hr))
                throw std::runtime_error("MFStartup 失败: 需要 Windows 7 及以上。");

            g_mfReady = true;
        }

        finish;
    }
    simplecatch("GMVideoInit", 0)
}

// 取 GM 窗口句柄 + D3D 设备并判定后端(D3D8/D3D9)。
expReal GetGMWindowsHandle(GMReal handle)
{
    GMWindowsHandle = (HWND)(DWORD)handle;
    Device = gmapi->GetDirect3DDevice();
    d3d::ensure_version(Device, (void*)gmapi->GetDirect3DInterface());

    finish;
}

expReal GMVideoShowErrors(GMReal mode)
{
    show_error = (mode > 0.5);
    finish;
}

expReal GMVideoPlay(GMString path, GMReal loop)
{
    try
    {
        GMVideoFree();

        if (!g_mfReady)
            throw std::runtime_error("请先调用 GMVideoInit()。");
        if (Device == nullptr)
            throw std::runtime_error("请先调用 GetGMWindowsHandle()。");

        std::wstring wpath = ToWide(path);
        if (GetFileAttributesW(wpath.c_str()) == INVALID_FILE_ATTRIBUTES)
            throw std::runtime_error("尝试打开不存在的文件：" + GBKToUTF8(path));

        if (!g_video.Open(wpath.c_str()))
            throw std::runtime_error("打开视频失败: " + g_video.LastError());

        g_fps = g_video.GetFPS();
        g_duration = g_video.GetDurationSec();
        g_totalFrames = std::max(1, (int)std::ceil(g_duration * g_fps));
        g_frameTime = 1.0 / g_fps;

        VideoMF::AudioFormatInfo af = g_video.GetAudioFormat();
        if (g_video.HasAudio() && af.valid && g_audio.Init(GMWindowsHandle) &&
            g_audio.CreateStream(af.rate, af.channels, (WORD)af.bits))
        {
            g_audio.Play(false);

            g_audioStop.store(false);
            g_audioPaused.store(false);
            g_audioEof.store(false);
            g_audioWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            g_audioThread = std::thread(AudioThreadProc);
            g_audioActive = true;
        }
        else
        {
            g_lastFrame = TimerGet();
        }

        g_exportSurface = gm::surface_create((int)g_video.GetWidth(), (int)g_video.GetHeight());

        g_playing = true;
        g_speed = 1.0;
        g_pushedBytes.store(0);
        g_fatalShown = false;   // 新播放重置熔断标记
        g_loop = (loop > 0.5);
        g_videoCurrent = -1;

        finish;
    }
    simplecatch("GMVideoPlay", 0)
}

expReal GMVideoUpdate()
{
    try
    {
        if (!g_playing)
            fail;

        int pos;

        if (g_audioActive)
        {
            double apos = g_audio.GetPositionSec();
            if (apos <= 0.0)
                pos = g_videoCurrent;
            else
                pos = (int)(apos * g_fps);
            if (g_audioEof.load())
                pos = g_totalFrames - 1;   // 音频读完: 视频播到头。不能用 apos>=g_duration
                                           // 判断 —— 音轨比视频短时 apos 停在音频末尾(<dur),
                                           // pos 不再增长, 永远到不了结尾、不触发播完/循环。
        }
        else
        {
            pos = g_videoCurrent;
            double timer = TimerGet();
            if (timer >= g_lastFrame + g_frameTime / g_speed || g_videoCurrent == -1)
            {
                pos = g_videoCurrent + 1;
                g_lastFrame = timer;
            }
        }

        // 解码到目标帧(上限 512 帧/次, 防止位置大跳时冻帧)。
        int guard = 0;
        while (pos > g_videoCurrent && g_videoCurrent < g_totalFrames - 1 && guard++ < 512)
        {
            if (!g_video.DecodeNextFrame())
            {
                if (g_video.Eof())
                {
                    g_videoCurrent = g_totalFrames - 1;
                    break;
                }
                FailPlaying("解码帧失败: " + g_video.LastError());
                fail;   // 熔断: 停播, 不再每帧弹窗
            }
            ++g_videoCurrent;
        }

        // 上传最新帧到输出表面。
        bool uploaded = false;
        if (g_videoCurrent >= 0 && g_video.HasVideo())
        {
            if (!gm::surface_exists(g_exportSurface))
                g_exportSurface = gm::surface_create((int)g_video.GetWidth(), (int)g_video.GetHeight());

            g_video.ConvertNV12ToBGRA();
            uploaded = BufferToTexture(g_video.GetBGRA(), g_video.GetWidth(),
                g_video.GetHeight(), g_exportSurface) > 0.5;
            if (!uploaded)
            {
                FailPlaying("上传视频帧到表面失败。");
                fail;   // 熔断: 停播, 不再每帧弹窗
            }
        }

        if (g_videoCurrent >= g_totalFrames - 1)
        {
            if (g_loop)
                ResetInternal();
            else
                g_playing = false;
        }

        finish;
    }
    simplecatch("GMVideoUpdate", 0)
}

expReal GMVideoPause()
{
    g_playing = false;
    if (g_audioActive)
    {
        g_audioPaused.store(true);
        g_audio.Pause();
    }
    finish;
}

expReal GMVideoResume()
{
    g_playing = true;
    if (g_audioActive)
    {
        g_audioPaused.store(false);
        g_audio.Play(false);
    }
    finish;
}

expReal GMVideoStop()
{
    if (g_video.IsOpen())
        g_video.Seek(0.0);
    g_videoCurrent = -1;
    if (g_audioActive)
    {
        g_audio.Pause();
        g_audio.Reset();
        g_pushedBytes.store(0);
    }
    g_playing = false;
    finish;
}

expReal GMVideoReset()
{
    ResetInternal();
    finish;
}

expReal GMVideoGetWidth()  { return g_video.GetWidth(); }
expReal GMVideoGetHeight() { return g_video.GetHeight(); }
expReal GMVideoGetFPS()    { return g_fps; }
expReal GMVideoGetFrames() { return (double)(g_totalFrames - 1); }
expReal GMVideoGetPosition() { return (double)g_videoCurrent; }
expReal GMVideoGetSpeed()  { return g_speed; }
expReal GMVideoGetLoop()   { return g_loop ? 1.0 : 0.0; }
expReal GMVideoGetDuration() { return g_duration; }

expReal GMVideoGetSurface()
{
    return gm::surface_exists(g_exportSurface) ? g_exportSurface : gm::noone;
}

expReal GMVideoSetSpeed(GMReal speed)
{
    if (speed <= 0)
        fail;
    
    g_speed = speed;
    if (g_audioActive)
        g_audio.SetTempo(speed);
    finish;
}

expReal GMVideoSetLoop(GMReal loop)
{
    g_loop = (loop > 0.5);
    finish;
}

expReal GMVideoFree()
{
    if (g_audioActive)
        g_audio.Pause();
    StopAudioThread();
    g_audio.Destroy();

    if (gm::surface_exists(g_exportSurface))
        gm::surface_free(g_exportSurface);
    g_exportSurface = gm::noone;

    g_video.Close();

    g_playing = false;
    g_videoCurrent = -1;
    g_totalFrames = 0;
    g_fps = 0.0;
    g_duration = 0.0;
    g_frameTime = 0.0;
    g_speed = 1.0;
    g_loop = false;
    g_audioPaused.store(false);

    finish;
}
