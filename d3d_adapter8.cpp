// D3D8 后端实现。本 TU 只认识 d3d8.h / d3dx8.h / dxerr8.h。
// 所有函数签名与 d3d_adapter.h 的中性声明一一对应。
// D3DX8 是静态库(无 d3dx8.dll), 这些 D3DX8 调用编译进本 DLL —— 原生行为, 不改动。
#include "d3d_adapter.h"
#include "d3dx8.h"
#include "dxerr8.h"

namespace d3d
{
    namespace impl8
    {
        static IDirect3DDevice8* dev() { return (IDirect3DDevice8*)device(); }

        HRESULT set_render_state(DWORD s, DWORD v)
        { return dev()->SetRenderState((D3DRENDERSTATETYPE)s, v); }

        // ---- 纹理 / 表面(一律不透明 void*, 内部转回 D3D8 类型) ----
        HRESULT get_device_from_texture(void* tex, void** out)
        { return ((IDirect3DTexture8*)tex)->GetDevice((IDirect3DDevice8**)out); }

        HRESULT get_surface_level(void* tex, UINT level, void** out)
        { return ((IDirect3DTexture8*)tex)->GetSurfaceLevel(level, (IDirect3DSurface8**)out); }

        // D3D8 专有 CreateImageSurface(D3D9 无此方法, 见 adapter9 的
        // CreateOffscreenPlainSurface 等价物)。
        HRESULT create_image_surface(UINT w, UINT h, DWORD fmt, void** out)
        { return dev()->CreateImageSurface(w, h, (D3DFORMAT)fmt, (IDirect3DSurface8**)out); }

        DWORD get_surface_format(void* surf)
        {
            D3DSURFACE_DESC desc;
            if (SUCCEEDED(((IDirect3DSurface8*)surf)->GetDesc(&desc)))
                return (DWORD)desc.Format;
            return 0;
        }

        HRESULT lock_rect(void* surf, LockedRect* out, const RECT* rect, DWORD flags)
        {
            D3DLOCKED_RECT lr;
            HRESULT hr = ((IDirect3DSurface8*)surf)->LockRect(&lr, rect, flags);
            if (SUCCEEDED(hr)) { out->Pitch = lr.Pitch; out->pBits = lr.pBits; }
            return hr;
        }

        HRESULT unlock_rect(void* surf)
        { return ((IDirect3DSurface8*)surf)->UnlockRect(); }

        HRESULT add_dirty_rect(void* tex, const RECT* rect)
        { return ((IDirect3DTexture8*)tex)->AddDirtyRect(rect); }

        UINT get_level_count(void* tex)
        { return ((IDirect3DTexture8*)tex)->GetLevelCount(); }

        // ---- D3DX8(静态库): 固定调色板/滤镜参数由调用方语义决定 ----
        HRESULT load_surface_from_memory(void* dst, const RECT* dstRect, const void* src,
                                         DWORD fmt, UINT pitch, const RECT* srcRect)
        {
            return D3DXLoadSurfaceFromMemory((IDirect3DSurface8*)dst, nullptr, dstRect,
                src, (D3DFORMAT)fmt, pitch, nullptr, srcRect, D3DX_FILTER_NONE, 0);
        }

        HRESULT load_surface_from_surface(void* dst, void* src)
        {
            return D3DXLoadSurfaceFromSurface((IDirect3DSurface8*)dst, nullptr, nullptr,
                (IDirect3DSurface8*)src, nullptr, nullptr, D3DX_FILTER_NONE, 0);
        }

        void release(void* com)
        { if (com) ((IUnknown*)com)->Release(); }

        // ---- 错误文本(D3D8 全表来自 DXGetErrorDescription8A) ----
        std::string error_text(HRESULT hr)
        {
            const char* p = DXGetErrorDescription8A(hr);
            return p ? p : "Unknown error";
        }
    }
}
