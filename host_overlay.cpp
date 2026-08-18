#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <psapi.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "psapi.lib")

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

constexpr int kN = 49;
constexpr UINT kAtlasKnots = 17u * 10u * 9u * 64u;
constexpr float kUiWhiteScRgb = 203.0f / 80.0f;

struct Settings {
  bool enabled = true;
  float peak = 1000, paper = 50, contrast = 100, sat = 100, deband = 1;
};

struct CB {
  UINT width, height, n, flags;
  float peakNits, grayNits, contrast, saturation;
  float satScale, debandStrength, debandRadius, debandThresh;
  float blackFloor, uiWhite, pad0, pad1;
};

static const char *kLutCs = R"(
cbuffer Params : register(b0) {
  uint width; uint height; uint n; uint flags;
  float peakNits; float grayNits; float contrast; float saturation;
  float satScale; float debandStrength; float debandRadius; float debandThresh;
  float blackFloor; float uiWhite; float _p0; float _p1;
};
StructuredBuffer<float> Lut : register(t0);
Texture2D<float4> Src : register(t1);
StructuredBuffer<float> Atlas : register(t2);
RWTexture2D<float4> Dst : register(u0);
float3 lutTexel(uint r, uint g, uint b) {
  uint o = ((r * n + g) * n + b) * 3u;
  return float3(Lut[o], Lut[o+1u], Lut[o+2u]);
}
float3 sampleLut(float3 v) {
  v = saturate(v);
  float3 pos = v * (float(n) - 1.0);
  uint3 i0 = min(uint3(pos), uint3(n - 2, n - 2, n - 2));
  float3 f = pos - float3(i0);
  float3 c000 = lutTexel(i0.x, i0.y, i0.z);
  float3 c100 = lutTexel(i0.x+1, i0.y, i0.z);
  float3 c010 = lutTexel(i0.x, i0.y+1, i0.z);
  float3 c110 = lutTexel(i0.x+1, i0.y+1, i0.z);
  float3 c001 = lutTexel(i0.x, i0.y, i0.z+1);
  float3 c101 = lutTexel(i0.x+1, i0.y, i0.z+1);
  float3 c011 = lutTexel(i0.x, i0.y+1, i0.z+1);
  float3 c111 = lutTexel(i0.x+1, i0.y+1, i0.z+1);
  float3 c00 = lerp(c000, c100, f.x);
  float3 c01 = lerp(c001, c101, f.x);
  float3 c10 = lerp(c010, c110, f.x);
  float3 c11 = lerp(c011, c111, f.x);
  return lerp(lerp(c00, c10, f.y), lerp(c01, c11, f.y), f.z);
}
float hash21(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
float ign(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }
float3 dither8bit(int2 pixel, float3 sdr) {
  float tpdf = ign(float2(pixel)+0.5) + ign(float2(pixel)+float2(19.19,47.47)) - 1.0;
  return saturate(sdr + tpdf / 255.0);
}
float3 debandSdr(int2 pixel, float3 center) {
  if (debandStrength <= 0.001) return center;
  float radius = max(debandRadius, 1.0);
  int2 tile = pixel >> 3;
  float pr1 = hash21(float2(tile)+0.5);
  float pr2 = hash21(float2(tile)+float2(19,7));
  float2 d = (pr1 * radius) * float2(cos(pr2*6.28318530718), sin(pr2*6.28318530718));
  int2 a = clamp(pixel + int2(d), int2(0,0), int2(width,height)-1);
  int2 b = clamp(pixel - int2(d), int2(0,0), int2(width,height)-1);
  float3 avg = 0.5 * (Src.Load(int3(a,0)).rgb + Src.Load(int3(b,0)).rgb);
  float3 res = lerp(avg, center, step(3.0/1000.0, abs(center-avg)));
  return saturate(lerp(center, res, debandStrength));
}
float axisCoord(float value, float origin, float step, uint count) {
  float u = clamp((value-origin)/step, 0.0, float(count-1u));
  return min(floor(u), float(count-2u));
}
float axisFrac(float value, float origin, float step, uint count) {
  float u = clamp((value-origin)/step, 0.0, float(count-1u));
  float i0 = min(floor(u), float(count-2u));
  return u - i0;
}
float atlasCurve(uint ip, uint ig, uint ic, uint knot) {
  return Atlas[(((ip*10u+ig)*9u+ic)*64u)+knot];
}
float sampleToneAt(float sdrLuma, float peak, float gray, float contrastV) {
  float ipf = axisCoord(peak,400,100,17), igf = axisCoord(gray,10,10,10), icf = axisCoord(contrastV,0,25,9);
  float fp = axisFrac(peak,400,100,17), fg = axisFrac(gray,10,10,10), fc = axisFrac(contrastV,0,25,9);
  uint ip=uint(ipf), ig=uint(igf), ic=uint(icf);
  float pos = saturate(sdrLuma)*63.0; uint k0=min(uint(floor(pos)),62u); float fk=pos-float(k0);
  float c000=lerp(atlasCurve(ip,ig,ic,k0),atlasCurve(ip,ig,ic,k0+1),fk);
  float c100=lerp(atlasCurve(ip+1,ig,ic,k0),atlasCurve(ip+1,ig,ic,k0+1),fk);
  float c010=lerp(atlasCurve(ip,ig+1,ic,k0),atlasCurve(ip,ig+1,ic,k0+1),fk);
  float c110=lerp(atlasCurve(ip+1,ig+1,ic,k0),atlasCurve(ip+1,ig+1,ic,k0+1),fk);
  float c001=lerp(atlasCurve(ip,ig,ic+1,k0),atlasCurve(ip,ig,ic+1,k0+1),fk);
  float c101=lerp(atlasCurve(ip+1,ig,ic+1,k0),atlasCurve(ip+1,ig,ic+1,k0+1),fk);
  float c011=lerp(atlasCurve(ip,ig+1,ic+1,k0),atlasCurve(ip,ig+1,ic+1,k0+1),fk);
  float c111=lerp(atlasCurve(ip+1,ig+1,ic+1,k0),atlasCurve(ip+1,ig+1,ic+1,k0+1),fk);
  return lerp(lerp(lerp(c000,c100,fp),lerp(c010,c110,fp),fg), lerp(lerp(c001,c101,fp),lerp(c011,c111,fp),fg), fc);
}
float3 applyRtxControls(float3 scRgb, float sdrLuma) {
  float yNew = max(sampleToneAt(sdrLuma, peakNits, grayNits, contrast), 1e-6);
  float yDef = max(sampleToneAt(sdrLuma, 1000, 50, 100), 1e-6);
  float3 outc = scRgb * (yNew / yDef);
  float y = max(dot(outc, float3(0.2126,0.7152,0.0722)), 1e-6);
  return y + (outc - y) * satScale;
}
float3 srgbToLinear(float3 c) { return (c <= 0.04045) ? c/12.92 : pow((c+0.055)/1.055, 2.4); }
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height) return;
  float3 raw = saturate(Src.Load(int3(id.xy,0)).rgb);
  if ((flags & 1u) == 0u) { Dst[id.xy] = float4(srgbToLinear(raw),1); return; }
  float3 sdr = dither8bit(int2(id.xy), debandSdr(int2(id.xy), raw));
  float inputLuma = dot(sdr, float3(0.2126,0.7152,0.0722));
  float gate = saturate(inputLuma / max(blackFloor,1e-6));
  gate = gate*gate*(3-2*gate);
  float3 hdr = sampleLut(sdr);
  if (flags & 2u) hdr = applyRtxControls(hdr, inputLuma);
  Dst[id.xy] = float4(hdr * gate, 1);
}
)";

static ID3D11Device *g_dev = nullptr;
static ID3D11DeviceContext *g_ctx = nullptr;
static IDXGISwapChain3 *g_swap = nullptr;
static ID3D11Texture2D *g_bb_tex[2] = {};
static ID3D11UnorderedAccessView *g_bb_uav[2] = {};
static ID3D11ComputeShader *g_cs = nullptr;
static ID3D11Buffer *g_lut = nullptr, *g_atlas = nullptr, *g_cb = nullptr;
static ID3D11ShaderResourceView *g_lut_srv = nullptr, *g_atlas_srv = nullptr;
static ID3D11Texture2D *g_cap_tex[2] = {};
static ID3D11ShaderResourceView *g_cap_srv[2] = {};
static HWND g_overlay = nullptr;
static HWND g_target = nullptr;
static UINT g_w = 0, g_h = 0;
static RECT g_placed{};
static Settings g_set{};
static bool g_menu = false;
static bool g_has_atlas = false;
static bool g_cb_dirty = true;
static float g_sat_scales[9] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
static std::atomic<bool> g_alt_x{false};
static IDirect3DDevice g_winrt_dev{nullptr};
static GraphicsCaptureItem g_item{nullptr};
static Direct3D11CaptureFramePool g_pool{nullptr};
static GraphicsCaptureSession g_session{nullptr};

static std::wstring exe_dir() {
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring p(buf);
  size_t s = p.find_last_of(L"\\/");
  return s == std::wstring::npos ? L"." : p.substr(0, s);
}

static std::wstring ini_path() {
  wchar_t ad[MAX_PATH];
  GetEnvironmentVariableW(L"LOCALAPPDATA", ad, MAX_PATH);
  std::wstring d = std::wstring(ad) + L"\\OpenHDR";
  CreateDirectoryW(d.c_str(), nullptr);
  return d + L"\\settings.ini";
}

static void load_settings() {
  auto path = ini_path();
  auto rd = [&](const wchar_t *k, int def) { return (int)GetPrivateProfileIntW(L"OpenHDR", k, def, path.c_str()); };
  g_set.enabled = rd(L"enabled", 1) != 0;
  g_set.peak = (float)std::clamp(rd(L"peak", 1000), 400, 2000);
  g_set.paper = (float)std::clamp(rd(L"paper", 50), 10, 100);
  g_set.contrast = (float)std::clamp(rd(L"contrast", 100), 0, 200);
  g_set.sat = (float)std::clamp(rd(L"saturation", 100), 0, 200);
  g_set.deband = std::clamp(rd(L"deband", 100), 0, 100) / 100.0f;
}

static void save_settings() {
  auto path = ini_path();
  wchar_t buf[32];
  WritePrivateProfileStringW(L"OpenHDR", L"enabled", g_set.enabled ? L"1" : L"0", path.c_str());
  swprintf_s(buf, L"%.0f", g_set.peak);
  WritePrivateProfileStringW(L"OpenHDR", L"peak", buf, path.c_str());
  swprintf_s(buf, L"%.0f", g_set.paper);
  WritePrivateProfileStringW(L"OpenHDR", L"paper", buf, path.c_str());
  swprintf_s(buf, L"%.0f", g_set.contrast);
  WritePrivateProfileStringW(L"OpenHDR", L"contrast", buf, path.c_str());
  swprintf_s(buf, L"%.0f", g_set.sat);
  WritePrivateProfileStringW(L"OpenHDR", L"saturation", buf, path.c_str());
  swprintf_s(buf, L"%.0f", g_set.deband * 100);
  WritePrivateProfileStringW(L"OpenHDR", L"deband", buf, path.c_str());
}

static bool load_bin(const wchar_t *name, std::vector<char> &out) {
  std::ifstream f(exe_dir() + L"\\" + name, std::ios::binary);
  if (!f) return false;
  out.assign((std::istreambuf_iterator<char>(f)), {});
  return !out.empty();
}

static float sat_scale(float sat) {
  float u = std::clamp(sat / 25.0f, 0.0f, 8.0f);
  int i = (int)u;
  if (i >= 8) return g_sat_scales[8];
  return g_sat_scales[i] + (g_sat_scales[i + 1] - g_sat_scales[i]) * (u - i);
}

static bool skip_exe(const wchar_t *name) {
  static const wchar_t *bad[] = {L"explorer.exe", L"SearchHost.exe", L"TextInputHost.exe", L"ApplicationFrameHost.exe",
                                 L"chrome.exe",   L"msedge.exe",     L"firefox.exe",       L"Discord.exe",
                                 L"Code.exe",     L"Cursor.exe",     L"devenv.exe",        L"steam.exe",
                                 L"steamwebhelper.exe", L"OpenHDR.exe", L"OpenHDROverlay.exe"};
  for (auto *b : bad)
    if (_wcsicmp(name, b) == 0) return true;
  return false;
}

static bool has_graphics(DWORD pid) {
  HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!p) return false;
  HMODULE mods[256];
  DWORD need = 0;
  bool ok = false;
  if (EnumProcessModules(p, mods, sizeof(mods), &need)) {
    unsigned n = need / sizeof(HMODULE);
    if (n > 256) n = 256;
    wchar_t name[MAX_PATH];
    for (unsigned i = 0; i < n; i++) {
      if (!GetModuleBaseNameW(p, mods[i], name, MAX_PATH)) continue;
      if (!_wcsicmp(name, L"d3d11.dll") || !_wcsicmp(name, L"d3d12.dll") || !_wcsicmp(name, L"vulkan-1.dll") ||
          !_wcsicmp(name, L"opengl32.dll") || !_wcsicmp(name, L"dxgi.dll")) {
        ok = true;
        break;
      }
    }
  }
  CloseHandle(p);
  return ok;
}

static HWND pick_target() {
  HWND fg = GetForegroundWindow();
  if (!fg || fg == g_overlay) return nullptr;
  wchar_t cls[64]{};
  GetClassNameW(fg, cls, 64);
  if (!_wcsicmp(cls, L"OpenHDROverlay") || !_wcsicmp(cls, L"OpenHDRInstall")) return nullptr;
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  if (!pid || pid == GetCurrentProcessId()) return nullptr;
  wchar_t path[MAX_PATH]{};
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (p) {
    DWORD n = MAX_PATH;
    QueryFullProcessImageNameW(p, 0, path, &n);
    CloseHandle(p);
  }
  const wchar_t *base = path;
  for (wchar_t *c = path; *c; ++c)
    if (*c == L'\\' || *c == L'/') base = c + 1;
  if (skip_exe(base)) return nullptr;
  RECT rc{};
  GetClientRect(fg, &rc);
  if ((rc.right - rc.left) < 640 || (rc.bottom - rc.top) < 360) return nullptr;
  if (!IsWindowVisible(fg) || IsIconic(fg)) return nullptr;
  if (!has_graphics(pid)) return nullptr;
  return fg;
}

static void set_clickthrough(bool through) {
  if (!g_overlay) return;
  LONG ex = GetWindowLongW(g_overlay, GWL_EXSTYLE);
  if (through) ex |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
  else ex &= ~WS_EX_TRANSPARENT;
  SetWindowLongW(g_overlay, GWL_EXSTYLE, ex);
}

static void place_overlay(HWND target) {
  RECT cr{};
  GetClientRect(target, &cr);
  POINT tl{0, 0};
  ClientToScreen(target, &tl);
  RECT want{tl.x, tl.y, tl.x + (cr.right - cr.left), tl.y + (cr.bottom - cr.top)};
  if (want.left == g_placed.left && want.top == g_placed.top && want.right == g_placed.right && want.bottom == g_placed.bottom)
    return;
  g_placed = want;
  SetWindowPos(g_overlay, HWND_TOPMOST, want.left, want.top, want.right - want.left, want.bottom - want.top,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void release_caps() {
  for (int i = 0; i < 2; i++) {
    if (g_cap_srv[i]) {
      g_cap_srv[i]->Release();
      g_cap_srv[i] = nullptr;
    }
    g_cap_tex[i] = nullptr;
  }
}

static void release_bb() {
  for (int i = 0; i < 2; i++) {
    if (g_bb_uav[i]) {
      g_bb_uav[i]->Release();
      g_bb_uav[i] = nullptr;
    }
    g_bb_tex[i] = nullptr;
  }
}

static ID3D11UnorderedAccessView *uav_for_backbuffer() {
  if (!g_swap) return nullptr;
  ID3D11Texture2D *bb = nullptr;
  if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&bb))) || !bb) return nullptr;
  for (int i = 0; i < 2; i++) {
    if (g_bb_tex[i] == bb) {
      bb->Release();
      return g_bb_uav[i];
    }
  }
  int slot = g_bb_uav[0] ? 1 : 0;
  if (g_bb_uav[slot]) {
    g_bb_uav[slot]->Release();
    g_bb_uav[slot] = nullptr;
  }
  g_bb_tex[slot] = bb;
  g_dev->CreateUnorderedAccessView(bb, nullptr, &g_bb_uav[slot]);
  bb->Release();
  return g_bb_uav[slot];
}

static ID3D11ShaderResourceView *srv_for_cap(ID3D11Texture2D *tex) {
  for (int i = 0; i < 2; i++) {
    if (g_cap_tex[i] == tex) return g_cap_srv[i];
  }
  int slot = g_cap_srv[0] ? 1 : 0;
  if (g_cap_srv[slot]) {
    g_cap_srv[slot]->Release();
    g_cap_srv[slot] = nullptr;
  }
  g_cap_tex[slot] = tex;
  g_dev->CreateShaderResourceView(tex, nullptr, &g_cap_srv[slot]);
  return g_cap_srv[slot];
}

static bool ensure_swapchain(UINT w, UINT h) {
  if (g_swap) {
    DXGI_SWAP_CHAIN_DESC sd{};
    g_swap->GetDesc(&sd);
    if (sd.BufferDesc.Width == w && sd.BufferDesc.Height == h) return true;
    release_bb();
    if (FAILED(g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))) return false;
    g_w = w;
    g_h = h;
    g_cb_dirty = true;
    return true;
  }
  IDXGIDevice *dxgi_dev = nullptr;
  g_dev->QueryInterface(&dxgi_dev);
  IDXGIAdapter *ad = nullptr;
  dxgi_dev->GetAdapter(&ad);
  IDXGIFactory2 *fac = nullptr;
  ad->GetParent(IID_PPV_ARGS(&fac));
  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Width = w;
  sd.Height = h;
  sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_UNORDERED_ACCESS;
  sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  IDXGISwapChain1 *sc1 = nullptr;
  HRESULT hr = fac->CreateSwapChainForHwnd(g_dev, g_overlay, &sd, nullptr, nullptr, &sc1);
  fac->Release();
  ad->Release();
  dxgi_dev->Release();
  if (FAILED(hr) || !sc1) return false;
  sc1->QueryInterface(&g_swap);
  sc1->Release();
  g_swap->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
  g_w = w;
  g_h = h;
  return true;
}

static void stop_capture() {
  try {
    if (g_session) g_session.Close();
  } catch (...) {
  }
  try {
    if (g_pool) g_pool.Close();
  } catch (...) {
  }
  g_session = nullptr;
  g_pool = nullptr;
  g_item = nullptr;
  release_caps();
}

static IDirect3DDevice wrap_device(ID3D11Device *dev) {
  IDXGIDevice *dxgi = nullptr;
  dev->QueryInterface(&dxgi);
  IInspectable *insp = nullptr;
  CreateDirect3D11DeviceFromDXGIDevice(dxgi, &insp);
  dxgi->Release();
  IDirect3DDevice out = nullptr;
  winrt::copy_from_abi(out, insp);
  if (insp) insp->Release();
  return out;
}

static bool start_capture(HWND hwnd) {
  stop_capture();
  auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
  GraphicsCaptureItem item{nullptr};
  if (FAILED(interop->CreateForWindow(hwnd, guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), put_abi(item))))
    return false;
  auto size = item.Size();
  if (size.Width < 16 || size.Height < 16) return false;
  g_item = item;
  g_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(g_winrt_dev, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
  g_session = g_pool.CreateCaptureSession(g_item);
  try {
    g_session.IsCursorCaptureEnabled(false);
  } catch (...) {
  }
  try {
    g_session.IsBorderRequired(false);
  } catch (...) {
  }
  g_session.StartCapture();
  return true;
}

static ID3D11Texture2D *frame_tex() {
  if (!g_pool) return nullptr;
  Direct3D11CaptureFrame frame{nullptr};
  try {
    frame = g_pool.TryGetNextFrame();
  } catch (...) {
    return nullptr;
  }
  if (!frame) return nullptr;
  auto surf = frame.Surface();
  auto access = surf.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
  ID3D11Texture2D *tex = nullptr;
  access->GetInterface(IID_PPV_ARGS(&tex));
  return tex;
}

static void write_cb(UINT w, UINT h) {
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
  CB cb{};
  cb.width = w;
  cb.height = h;
  cb.n = kN;
  cb.flags = (g_set.enabled ? 1u : 0u) | (g_has_atlas ? 2u : 0u);
  cb.peakNits = g_set.peak;
  cb.grayNits = g_set.paper;
  cb.contrast = g_set.contrast;
  cb.saturation = g_set.sat;
  cb.satScale = sat_scale(g_set.sat);
  cb.debandStrength = g_set.deband;
  cb.debandRadius = 16;
  cb.debandThresh = 0.003f;
  cb.blackFloor = 0.02f;
  cb.uiWhite = kUiWhiteScRgb;
  memcpy(mapped.pData, &cb, sizeof(cb));
  g_ctx->Unmap(g_cb, 0);
  g_cb_dirty = false;
}

static void map_frame(ID3D11Texture2D *cap) {
  D3D11_TEXTURE2D_DESC td{};
  cap->GetDesc(&td);
  if (!ensure_swapchain(td.Width, td.Height)) return;
  ID3D11UnorderedAccessView *uav = uav_for_backbuffer();
  ID3D11ShaderResourceView *src = srv_for_cap(cap);
  if (!uav || !src) return;
  if (g_cb_dirty) write_cb(td.Width, td.Height);

  ID3D11ShaderResourceView *srvs[3] = {g_lut_srv, src, g_atlas_srv};
  g_ctx->CSSetShader(g_cs, nullptr, 0);
  g_ctx->CSSetConstantBuffers(0, 1, &g_cb);
  g_ctx->CSSetShaderResources(0, 3, srvs);
  g_ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  g_ctx->Dispatch((td.Width + 7) / 8, (td.Height + 7) / 8, 1);
  ID3D11UnorderedAccessView *noneU = nullptr;
  ID3D11ShaderResourceView *noneS[3] = {};
  g_ctx->CSSetUnorderedAccessViews(0, 1, &noneU, nullptr);
  g_ctx->CSSetShaderResources(0, 3, noneS);
  g_swap->Present(0, DXGI_PRESENT_ALLOW_TEARING);
}

static void draw_menu(HDC dc, int w, int h) {
  RECT rc{16, 16, 420, 250};
  HBRUSH bg = CreateSolidBrush(RGB(16, 16, 18));
  FillRect(dc, &rc, bg);
  DeleteObject(bg);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(236, 236, 230));
  wchar_t line[128];
  int y = 28;
  auto row = [&](const wchar_t *name, float v) {
    swprintf_s(line, L"%s  %.0f", name, v);
    TextOutW(dc, 28, y, line, (int)wcslen(line));
    y += 28;
  };
  TextOutW(dc, 28, y, g_set.enabled ? L"OpenHDR  ON   (desktop overlay)" : L"OpenHDR  OFF  (desktop overlay)", -1);
  y += 32;
  row(L"Peak", g_set.peak);
  row(L"Paper white", g_set.paper);
  row(L"Contrast", g_set.contrast);
  row(L"Saturation", g_set.sat);
  row(L"Deband %", g_set.deband * 100);
  TextOutW(dc, 28, y + 8, L"Alt+X close   arrows change   1 2 3 4 5 = sliders", -1);
}

static void handle_menu_keys() {
  if (!g_menu) return;
  auto edge = [](int vk) { return (GetAsyncKeyState(vk) & 1) != 0; };
  bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
  float step = shift ? 10.0f : 1.0f;
  if (edge(VK_LEFT) || edge(VK_DOWN)) {
    if (g_menu) {
    }
  }
  if (GetAsyncKeyState('1') & 1) {
    g_set.enabled = !g_set.enabled;
    g_cb_dirty = true;
  }
  if (GetAsyncKeyState(VK_LEFT) & 1) {
    if (GetAsyncKeyState('2') & 0x8000) g_set.peak = std::max(400.0f, g_set.peak - step);
    else if (GetAsyncKeyState('3') & 0x8000) g_set.paper = std::max(10.0f, g_set.paper - step);
    else if (GetAsyncKeyState('4') & 0x8000) g_set.contrast = std::max(0.0f, g_set.contrast - step);
    else if (GetAsyncKeyState('5') & 0x8000) g_set.sat = std::max(0.0f, g_set.sat - step);
    else if (GetAsyncKeyState('6') & 0x8000) g_set.deband = std::max(0.0f, g_set.deband - step / 100.0f);
    else g_set.peak = std::max(400.0f, g_set.peak - step);
    g_cb_dirty = true;
    save_settings();
  }
  if (GetAsyncKeyState(VK_RIGHT) & 1) {
    if (GetAsyncKeyState('3') & 0x8000) g_set.paper = std::min(100.0f, g_set.paper + step);
    else if (GetAsyncKeyState('4') & 0x8000) g_set.contrast = std::min(200.0f, g_set.contrast + step);
    else if (GetAsyncKeyState('5') & 0x8000) g_set.sat = std::min(200.0f, g_set.sat + step);
    else if (GetAsyncKeyState('6') & 0x8000) g_set.deband = std::min(1.0f, g_set.deband + step / 100.0f);
    else g_set.peak = std::min(2000.0f, g_set.peak + step);
    g_cb_dirty = true;
    save_settings();
  }
}

static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_DESTROY) PostQuitMessage(0);
  if (msg == WM_PAINT && g_menu) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    draw_menu(dc, rc.right, rc.bottom);
    EndPaint(hwnd, &ps);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK llkb(int code, WPARAM wp, LPARAM lp) {
  if (code == HC_ACTION && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
    auto *k = reinterpret_cast<KBDLLHOOKSTRUCT *>(lp);
    if (k && (k->vkCode == 'X' || k->vkCode == 'x') && (GetAsyncKeyState(VK_MENU) & 0x8000)) {
      HWND fg = GetForegroundWindow();
      DWORD pid = 0;
      if (fg) GetWindowThreadProcessId(fg, &pid);
      if (pid && (fg == g_overlay || fg == g_target || pid == GetCurrentProcessId())) {
        g_alt_x = true;
        return 1;
      }
    }
  }
  return CallNextHookEx(nullptr, code, wp, lp);
}

static bool init_gpu() {
  UINT flags = 0;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &fl, 1, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx)))
    return false;
  ID3DBlob *blob = nullptr, *err = nullptr;
  if (FAILED(D3DCompile(kLutCs, strlen(kLutCs), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &blob, &err))) {
    if (err) err->Release();
    return false;
  }
  g_dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_cs);
  blob->Release();
  if (err) err->Release();

  auto make_buf = [&](const void *data, UINT bytes, ID3D11Buffer **buf, ID3D11ShaderResourceView **srv, UINT elems) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(float);
    D3D11_SUBRESOURCE_DATA sd{data, 0, 0};
    g_dev->CreateBuffer(&bd, data ? &sd : nullptr, buf);
    D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
    svd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    svd.Buffer.NumElements = elems;
    g_dev->CreateShaderResourceView(*buf, &svd, srv);
  };
  std::vector<char> lut, atlas;
  if (!load_bin(L"lut3d_49.bin", lut)) return false;
  make_buf(lut.data(), (UINT)lut.size(), &g_lut, &g_lut_srv, (UINT)(lut.size() / sizeof(float)));
  if (load_bin(L"rtx_control_atlas.bin", atlas) && atlas.size() >= (kAtlasKnots + 9) * sizeof(float)) {
    auto *f = reinterpret_cast<float *>(atlas.data());
    make_buf(f, kAtlasKnots * sizeof(float), &g_atlas, &g_atlas_srv, kAtlasKnots);
    memcpy(g_sat_scales, f + kAtlasKnots, 9 * sizeof(float));
    g_has_atlas = true;
  } else {
    float z = 0;
    make_buf(&z, 4, &g_atlas, &g_atlas_srv, 1);
  }
  D3D11_BUFFER_DESC cbd{};
  cbd.ByteWidth = sizeof(CB);
  cbd.Usage = D3D11_USAGE_DYNAMIC;
  cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  g_dev->CreateBuffer(&cbd, nullptr, &g_cb);
  g_winrt_dev = wrap_device(g_dev);
  return g_cs && g_lut_srv && g_cb && g_winrt_dev;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\OpenHDR_desktop_overlay");
  if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  load_settings();
  if (!init_gpu()) {
    MessageBoxW(nullptr, L"OpenHDR overlay failed to start (GPU/LUT).", L"OpenHDR", MB_ICONERROR);
    return 1;
  }

  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = overlay_proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"OpenHDROverlay";
  RegisterClassExW(&wc);
  g_overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, wc.lpszClassName, L"OpenHDR",
                              WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, inst, nullptr);
  SetWindowsHookExW(WH_KEYBOARD_LL, llkb, inst, 0);

  MSG msg{};
  for (;;) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) return 0;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (g_alt_x.exchange(false)) {
      g_menu = !g_menu;
      set_clickthrough(!g_menu);
      if (!g_menu) save_settings();
      InvalidateRect(g_overlay, nullptr, TRUE);
    }
    if (g_menu) handle_menu_keys();

    HWND fg = GetForegroundWindow();
    bool locked = g_target && IsWindow(g_target) && !IsIconic(g_target) && (fg == g_target || fg == g_overlay);
    if (!locked) {
      HWND t = pick_target();
      if (!t) {
        if (g_target) {
          stop_capture();
          g_target = nullptr;
          g_placed = {};
          ShowWindow(g_overlay, SW_HIDE);
        }
        Sleep(50);
        continue;
      }
      if (t != g_target) {
        g_target = t;
        if (!start_capture(t)) {
          g_target = nullptr;
          Sleep(200);
          continue;
        }
      }
    }
    place_overlay(g_target);
    if (ID3D11Texture2D *tex = frame_tex()) {
      map_frame(tex);
      tex->Release();
    }
    if (g_menu) {
      HDC dc = GetDC(g_overlay);
      RECT rc{};
      GetClientRect(g_overlay, &rc);
      draw_menu(dc, rc.right, rc.bottom);
      ReleaseDC(g_overlay, dc);
    }
  }
}
