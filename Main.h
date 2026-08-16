#pragma once
#include "Gmapi.h"
#include "d3d_adapter.h"
#include <string>

// ============================================================================
// GMVideo 独立插件头 —— 精简自给自足版(不依赖 NatureEnhance 的 Main.h)
//   - GMReal/GMString 与导出宏与 NatureEnhance 完全一致, GM 侧 external_define
//     调用方式不变。
//   - simplecatch / ShowMessage 在此独立实现, 错误弹窗直接用 MessageBox。
//   - BufferToTexture 是本地实现(走 d3d:: 双后端适配), 与 NatureEnhance 的
//     GMBuffer.cpp 等价但不共享代码。
// ============================================================================

typedef double GMReal;
typedef const char* GMString;

#define expReal extern "C" __declspec(dllexport) GMReal _cdecl
#define expString extern "C" __declspec(dllexport) GMString _cdecl

#define fnReal GMReal _cdecl
#define fnString GMString _cdecl

#define finish return 1.0
#define fail return 0.0
#define reterror return gm::noone

extern gm::CGMAPI* gmapi;
extern bool show_error;
extern HWND GMWindowsHandle;
extern void* Device;   // D3D8/9 设备对象, 不透明指针, 只经 d3d:: 适配器使用

std::string ErrorText(HRESULT hr);   // d3d::error_text 的薄封装

void ShowMessage(std::string&& str, std::string&& caption, UINT type);

// 统一错误处理: 按 show_error 弹窗。供 simplecatch 宏调用。
void CatchError(const char* func, const std::exception& e);

#define simplecatch(funcname, returns)											\
	catch (const std::exception& e)												\
	{																			\
		CatchError(funcname, e);												\
		return returns;															\
	}

// 帧数据(BGRA 字节序)上传到 GM 表面 —— 与 NatureEnhance BufferToTexture 等价。
// 返回 1.0 成功, 0.0 失败。
GMReal BufferToTexture(const void* bgra, GMReal w, GMReal h, GMReal surface);

// 墙钟计时(秒), 与 NatureEnhance TimerGet 一致。
GMReal TimerGet();
