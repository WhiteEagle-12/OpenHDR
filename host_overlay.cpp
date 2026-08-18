#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <dxgi1_6.h>
#include <commctrl.h>
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
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
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "psapi.lib")

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

constexpr UINT WM_TOGGLE_HDR = WM_APP + 30;
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
static HWND g_ui = nullptr;
static HWND g_target = nullptr;
static HWND g_host = nullptr;
static HWND g_last_game = nullptr;
static HWND g_attach_fail = nullptr;
static bool g_monitor_cap = false;
static HANDLE g_toggle_ev = nullptr;
static HANDLE g_frame_ev = nullptr;
static HANDLE g_present_th = nullptr;
static HANDLE g_present_stop = nullptr;
static IDXGIOutput *g_output = nullptr;
static ID3D11Texture2D *g_owned = nullptr;
static UINT g_owned_w = 0, g_owned_h = 0;
static CRITICAL_SECTION g_cap_cs;
static std::atomic<bool> g_present_run{false};
static std::atomic<bool> g_allow_present{false};
static std::atomic<ULONGLONG> g_frame_tick{0};
static winrt::event_token g_frame_tok{};
static ULONGLONG g_toggle_at = 0;
static void try_attach(HWND host);
static void hide_overlay();
static HWND resolve_game();
static ID3D11UnorderedAccessView *uav_for_backbuffer();
static wchar_t g_target_name[64] = L"";
static UINT g_w = 0, g_h = 0;
static RECT g_placed{};
static int g_pool_w = 0, g_pool_h = 0;
static Settings g_set{};
static bool g_menu = true;
static bool g_hidden = true;
static bool g_has_atlas = false;
static bool g_cb_dirty = true;
static float g_sat_scales[9] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
static std::atomic<bool> g_alt_x{false};
static std::atomic<ULONGLONG> g_hotkey_at{0};

static void arm_hotkey() {
  ULONGLONG now = GetTickCount64();
  if (now - g_hotkey_at.load() < 400) return;
  g_hotkey_at.store(now);
  g_alt_x.store(true);
}
static HWND g_status = nullptr;
static HWND g_enabled = nullptr;
static HWND g_tracks[5]{};
static HWND g_values[5]{};
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
  static const wchar_t *bad[] = {
      L"explorer.exe", L"SearchHost.exe", L"TextInputHost.exe",
      L"ShellExperienceHost.exe", L"StartMenuExperienceHost.exe", L"SystemSettings.exe",
      L"chrome.exe", L"msedge.exe", L"msedgewebview2.exe", L"firefox.exe", L"brave.exe", L"opera.exe",
      L"Discord.exe", L"Slack.exe", L"Spotify.exe", L"Teams.exe", L"OUTLOOK.EXE", L"WINWORD.EXE",
      L"EXCEL.EXE", L"POWERPNT.EXE", L"Code.exe", L"Cursor.exe", L"devenv.exe", L"notepad.exe",
      L"notepad++.exe", L"WindowsTerminal.exe", L"cmd.exe", L"powershell.exe", L"pwsh.exe",
      L"steam.exe", L"steamwebhelper.exe", L"EpicGamesLauncher.exe", L"OpenHDR.exe", L"OpenHDROverlay.exe",
      L"Lossless.dll", L"LosslessScaling.exe"};
  for (auto *b : bad)
    if (_wcsicmp(name, b) == 0) return true;
  return false;
}

static bool is_cloaked(HWND hwnd) {
  int hidden = 0;
  return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &hidden, sizeof(hidden))) && hidden != 0;
}

static bool window_frame(HWND hwnd, RECT *out) {
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, out, sizeof(*out)))) return true;
  return GetWindowRect(hwnd, out) != 0;
}

static bool exe_name(HWND hwnd, wchar_t *out, size_t outn) {
  out[0] = 0;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid) return false;
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return false;
  wchar_t path[MAX_PATH]{};
  DWORD n = MAX_PATH;
  BOOL ok = QueryFullProcessImageNameW(p, 0, path, &n);
  CloseHandle(p);
  if (!ok) return false;
  const wchar_t *base = path;
  for (wchar_t *c = path; *c; ++c)
    if (*c == L'\\' || *c == L'/') base = c + 1;
  lstrcpynW(out, base, (int)outn);
  return true;
}

static bool is_our_hwnd(HWND fg) {
  if (!fg || fg == g_overlay || fg == g_ui) return true;
  wchar_t cls[64]{};
  GetClassNameW(fg, cls, 64);
  return !_wcsicmp(cls, L"OpenHDROverlay") || !_wcsicmp(cls, L"OpenHDRSettings") || !_wcsicmp(cls, L"OpenHDRMain");
}

static bool is_game_hwnd(HWND fg) {
  if (!fg || !IsWindow(fg) || is_our_hwnd(fg)) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  if (!pid || pid == GetCurrentProcessId()) return false;
  wchar_t name[64]{};
  exe_name(fg, name, 64);
  if (name[0] && skip_exe(name)) return false;
  RECT rc{};
  GetClientRect(fg, &rc);
  if ((rc.right - rc.left) < 320 || (rc.bottom - rc.top) < 240) return false;
  return true;
}

static HWND pick_target_from(HWND fg) {
  if (!is_game_hwnd(fg)) return nullptr;
  LONG style = GetWindowLongW(fg, GWL_STYLE);
  if (!(style & WS_VISIBLE)) return nullptr;
  if (!IsWindowVisible(fg) || IsIconic(fg) || is_cloaked(fg)) return nullptr;
  return fg;
}

struct PidPick {
  DWORD pid;
  HWND best;
  long area;
};

static BOOL CALLBACK enum_pid_window(HWND w, LPARAM lp) {
  auto *pick = reinterpret_cast<PidPick *>(lp);
  DWORD pid = 0;
  GetWindowThreadProcessId(w, &pid);
  if (pid != pick->pid || is_our_hwnd(w) || !IsWindowVisible(w)) return TRUE;
  RECT rc{};
  GetClientRect(w, &rc);
  long area = (long)(rc.right - rc.left) * (long)(rc.bottom - rc.top);
  if (area > pick->area && (rc.right - rc.left) >= 64 && (rc.bottom - rc.top) >= 64) {
    pick->best = w;
    pick->area = area;
  }
  return TRUE;
}

static HWND best_window_for_pid(DWORD pid) {
  if (!pid) return nullptr;
  PidPick pick{pid, nullptr, 0};
  EnumWindows(enum_pid_window, reinterpret_cast<LPARAM>(&pick));
  return pick.best;
}

static HWND window_for_host(HWND host) {
  HWND w = pick_target_from(host);
  if (w) return w;
  if (!host || !IsWindow(host) || is_our_hwnd(host)) return nullptr;
  DWORD pid = 0;
  GetWindowThreadProcessId(host, &pid);
  wchar_t name[64]{};
  exe_name(host, name, 64);
  if (name[0] && skip_exe(name)) return nullptr;
  HWND best = best_window_for_pid(pid);
  if (best && (pick_target_from(best) || is_game_hwnd(best))) return best;
  return nullptr;
}

static BOOL CALLBACK enum_top_game(HWND w, LPARAM lp) {
  if (!pick_target_from(w)) return TRUE;
  *reinterpret_cast<HWND *>(lp) = w;
  return FALSE;
}

static HWND top_game() {
  HWND found = nullptr;
  EnumWindows(enum_top_game, reinterpret_cast<LPARAM>(&found));
  return found;
}

static HWND resolve_game() {
  HWND fg = window_for_host(GetForegroundWindow());
  if (fg) return fg;
  if (g_last_game && (pick_target_from(g_last_game) || (IsWindow(g_last_game) && is_game_hwnd(g_last_game))))
    return g_last_game;
  return top_game();
}

static HWND pick_target() { return pick_target_from(GetForegroundWindow()); }

static void set_clickthrough(bool) {
  if (!g_overlay) return;
  LONG ex = GetWindowLongW(g_overlay, GWL_EXSTYLE);
  ex &= ~WS_EX_LAYERED;
  ex |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP;
  SetWindowLongW(g_overlay, GWL_EXSTYLE, ex);
  SetWindowDisplayAffinity(g_overlay, WDA_EXCLUDEFROMCAPTURE);
  SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static void hide_overlay() {
  if (!g_overlay || g_hidden) return;
  ShowWindow(g_overlay, SW_HIDE);
  SetWindowPos(g_overlay, HWND_BOTTOM, -32000, -32000, 1, 1, SWP_NOACTIVATE | SWP_HIDEWINDOW);
  g_placed = {};
  g_hidden = true;
}

static bool settings_open() {
  return g_ui && !IsIconic(g_ui) && (GetWindowLongW(g_ui, GWL_STYLE) & WS_VISIBLE);
}

static void raise_ui() {
  if (!g_ui || IsIconic(g_ui) || !settings_open()) return;
  SetWindowPos(g_ui, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void place_ui() {
  if (!g_ui) return;
  constexpr int width = 540;
  constexpr int height = 490;
  HWND host = g_host;
  if (!host || host == g_ui || host == g_overlay || !IsWindow(host)) host = g_target;
  RECT frame{};
  int x = 48, y = 48;
  if (host && host != g_ui && host != g_overlay && window_frame(host, &frame)) {
    x = frame.left + 24;
    y = frame.top + 24;
    if (x + width > frame.right - 8) x = std::max(frame.left + 8, frame.right - width - 8);
    if (y + height > frame.bottom - 8) y = std::max(frame.top + 8, frame.bottom - height - 8);
  } else {
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(mon, &mi)) {
      x = mi.rcWork.left + 48;
      y = mi.rcWork.top + 48;
    }
  }
  SetWindowPos(g_ui, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW);
}

static void place_overlay(HWND target, UINT capture_width, UINT capture_height) {
  RECT frame{};
  if (g_monitor_cap) {
    HMONITOR mon = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(mon, &mi)) frame = mi.rcMonitor;
    else window_frame(target, &frame);
  } else {
    window_frame(target, &frame);
  }
  int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
  int w = (int)capture_width, h = (int)capture_height;
  if (fw > 32 && fh > 32 && std::abs(w - fw) <= 16 && std::abs(h - fh) <= 16) {
    w = fw;
    h = fh;
  }
  RECT want{frame.left, frame.top, frame.left + w, frame.top + h};
  if (want.left == g_placed.left && want.top == g_placed.top && want.right == g_placed.right && want.bottom == g_placed.bottom)
    return;
  g_placed = want;
  g_hidden = false;
  SetWindowPos(g_overlay, HWND_TOPMOST, want.left, want.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
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
  if (g_swap && g_w == w && g_h == h) return true;
  if (g_swap) {
    DXGI_SWAP_CHAIN_DESC sd{};
    g_swap->GetDesc(&sd);
    if (sd.BufferDesc.Width == w && sd.BufferDesc.Height == h) {
      g_w = w;
      g_h = h;
      return true;
    }
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
  sd.Scaling = DXGI_SCALING_STRETCH;
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

static HANDLE frame_event() {
  if (!g_frame_ev) g_frame_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  return g_frame_ev;
}

static void unbind_frames() {
  if (g_pool && g_frame_tok.value) {
    try {
      g_pool.FrameArrived(g_frame_tok);
    } catch (...) {
    }
  }
  g_frame_tok = {};
}

static void bind_frames() {
  unbind_frames();
  if (!g_pool) return;
  frame_event();
  g_frame_tok = g_pool.FrameArrived([](auto &&, auto &&) {
    if (g_frame_ev) SetEvent(g_frame_ev);
  });
}

static void stop_capture_unlocked() {
  unbind_frames();
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
  g_pool_w = g_pool_h = 0;
  release_caps();
}

static void stop_capture() {
  EnterCriticalSection(&g_cap_cs);
  stop_capture_unlocked();
  LeaveCriticalSection(&g_cap_cs);
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

static bool start_item(GraphicsCaptureItem const &item) {
  auto size = item.Size();
  if (size.Width < 16 || size.Height < 16) return false;
  g_item = item;
  g_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(g_winrt_dev, DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, size);
  g_pool_w = size.Width;
  g_pool_h = size.Height;
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
  bind_frames();
  return true;
}

static bool start_capture(HWND hwnd) {
  EnterCriticalSection(&g_cap_cs);
  stop_capture_unlocked();
  g_monitor_cap = false;
  auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  HWND cands[3] = {hwnd, GetAncestor(hwnd, GA_ROOT), best_window_for_pid(pid)};
  for (HWND cand : cands) {
    if (!cand || !IsWindow(cand)) continue;
    GraphicsCaptureItem item{nullptr};
    if (FAILED(interop->CreateForWindow(cand, guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                                        put_abi(item))))
      continue;
    if (start_item(item)) {
      LeaveCriticalSection(&g_cap_cs);
      return true;
    }
    stop_capture_unlocked();
  }
  LeaveCriticalSection(&g_cap_cs);
  return false;
}

static void sync_pool() {
  if (!g_pool || !g_item) return;
  auto size = g_item.Size();
  if (size.Width < 16 || size.Height < 16) return;
  if (size.Width == g_pool_w && size.Height == g_pool_h) return;
  try {
    g_pool.Recreate(g_winrt_dev, DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, size);
    g_pool_w = size.Width;
    g_pool_h = size.Height;
    release_caps();
    g_cb_dirty = true;
  } catch (...) {
  }
}

static ID3D11Texture2D *tex_from_frame(Direct3D11CaptureFrame const &frame) {
  if (!frame) return nullptr;
  auto surf = frame.Surface();
  auto access = surf.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
  ID3D11Texture2D *tex = nullptr;
  access->GetInterface(IID_PPV_ARGS(&tex));
  return tex;
}

static ID3D11Texture2D *frame_tex() {
  if (!g_pool) return nullptr;
  sync_pool();
  ID3D11Texture2D *latest = nullptr;
  try {
    for (;;) {
      auto frame = g_pool.TryGetNextFrame();
      if (!frame) break;
      if (latest) latest->Release();
      latest = tex_from_frame(frame);
    }
  } catch (...) {
    if (latest) latest->Release();
    return nullptr;
  }
  return latest;
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

static void bind_output(HWND hwnd) {
  if (g_output) {
    g_output->Release();
    g_output = nullptr;
  }
  if (!g_dev) return;
  IDXGIDevice *dd = nullptr;
  if (FAILED(g_dev->QueryInterface(&dd)) || !dd) return;
  IDXGIAdapter *ad = nullptr;
  dd->GetAdapter(&ad);
  dd->Release();
  if (!ad) return;
  HMONITOR mon = MonitorFromWindow(hwnd ? hwnd : g_overlay, MONITOR_DEFAULTTONEAREST);
  IDXGIOutput *out = nullptr;
  for (UINT i = 0; ad->EnumOutputs(i, &out) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_OUTPUT_DESC desc{};
    out->GetDesc(&desc);
    if (desc.Monitor == mon) {
      g_output = out;
      break;
    }
    out->Release();
  }
  ad->Release();
}

static bool ensure_owned(UINT w, UINT h, DXGI_FORMAT fmt) {
  if (g_owned && g_owned_w == w && g_owned_h == h) return true;
  if (g_owned) {
    g_owned->Release();
    g_owned = nullptr;
  }
  release_caps();
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_owned)) || !g_owned) return false;
  g_owned_w = w;
  g_owned_h = h;
  return true;
}

static bool shade_owned() {
  if (!g_owned) return false;
  D3D11_TEXTURE2D_DESC td{};
  g_owned->GetDesc(&td);
  if (!ensure_swapchain(td.Width, td.Height)) return false;
  ID3D11UnorderedAccessView *uav = uav_for_backbuffer();
  ID3D11ShaderResourceView *src = srv_for_cap(g_owned);
  if (!uav || !src) return false;
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
  return true;
}

static bool copy_latest_capture() {
  EnterCriticalSection(&g_cap_cs);
  ID3D11Texture2D *tex = frame_tex();
  if (!tex) {
    LeaveCriticalSection(&g_cap_cs);
    return false;
  }
  D3D11_TEXTURE2D_DESC td{};
  tex->GetDesc(&td);
  bool ok = ensure_owned(td.Width, td.Height, td.Format);
  if (ok) g_ctx->CopyResource(g_owned, tex);
  tex->Release();
  LeaveCriticalSection(&g_cap_cs);
  if (ok) {
    g_frame_tick.store(GetTickCount64());
    if (g_target) place_overlay(g_target, td.Width, td.Height);
  }
  return ok;
}

static DWORD WINAPI present_thread(void *) {
  while (g_present_run.load()) {
    if (!g_allow_present.load()) {
      WaitForSingleObject(g_present_stop, 8);
      continue;
    }
    if (!copy_latest_capture()) {
      HANDLE ev = frame_event();
      WaitForSingleObject(ev ? ev : g_present_stop, 8);
      continue;
    }
    if (!shade_owned()) {
      Sleep(1);
      continue;
    }
    if (!g_output && g_swap) g_swap->GetContainingOutput(&g_output);
    if (g_output) g_output->WaitForVBlank();
    if (g_swap && g_allow_present.load()) g_swap->Present(0, DXGI_PRESENT_ALLOW_TEARING);
  }
  return 0;
}

static const wchar_t *kSliderNames[5] = {L"Peak brightness (nits)", L"Paper white (nits)", L"Contrast",
                                          L"Saturation", L"Deband (%)"};
static int slider_value(int i) {
  if (i == 0) return (int)g_set.peak;
  if (i == 1) return (int)g_set.paper;
  if (i == 2) return (int)g_set.contrast;
  if (i == 3) return (int)g_set.sat;
  return (int)std::lround(g_set.deband * 100.0f);
}

static void set_slider_value(int i, int value) {
  if (i == 0) g_set.peak = (float)value;
  else if (i == 1) g_set.paper = (float)value;
  else if (i == 2) g_set.contrast = (float)value;
  else if (i == 3) g_set.sat = (float)value;
  else g_set.deband = value / 100.0f;
}

static void refresh_ui() {
  if (!g_ui) return;
  wchar_t text[160]{};
  swprintf_s(text, L"HDR %s. Alt+X toggles HDR.  %s", g_set.enabled ? L"ON" : L"OFF",
             g_target_name[0] ? g_target_name : L"");
  if (g_status) SetWindowTextW(g_status, text);
  if (g_enabled) SendMessageW(g_enabled, BM_SETCHECK, g_set.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
  for (int i = 0; i < 5; ++i) {
    if (g_tracks[i]) SendMessageW(g_tracks[i], TBM_SETPOS, TRUE, slider_value(i));
    if (g_values[i]) {
      swprintf_s(text, L"%d", slider_value(i));
      SetWindowTextW(g_values[i], text);
    }
  }
}

static void create_ui_controls(HWND hwnd) {
  HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  auto make_static = [&](const wchar_t *text, int x, int y, int w, int h) {
    HWND c = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, hwnd, nullptr, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
  };
  HWND title = make_static(L"OpenHDR", 24, 18, 300, 28);
  SendMessageW(title, WM_SETFONT, (WPARAM)font, TRUE);
  g_status = make_static(L"", 24, 50, 474, 36);
  g_enabled = CreateWindowExW(0, L"BUTTON", L"Enable HDR conversion", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                              24, 92, 220, 24, hwnd, (HMENU)100, nullptr, nullptr);
  SendMessageW(g_enabled, WM_SETFONT, (WPARAM)font, TRUE);
  const int mins[5] = {400, 10, 0, 0, 0};
  const int maxs[5] = {2000, 100, 200, 200, 100};
  const int pages[5] = {100, 10, 25, 25, 10};
  for (int i = 0; i < 5; ++i) {
    int y = 130 + i * 54;
    make_static(kSliderNames[i], 24, y, 220, 20);
    g_values[i] = make_static(L"", 454, y, 44, 20);
    g_tracks[i] = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                  22, y + 20, 476, 28, hwnd, (HMENU)(INT_PTR)(200 + i), nullptr, nullptr);
    SendMessageW(g_tracks[i], TBM_SETRANGE, TRUE, MAKELPARAM(mins[i], maxs[i]));
    SendMessageW(g_tracks[i], TBM_SETPAGESIZE, 0, pages[i]);
    SendMessageW(g_tracks[i], TBM_SETLINESIZE, 0, i == 0 ? 10 : 1);
  }
  HWND help = make_static(L"Alt+X toggles HDR on and off. If OpenHDR is closed, Alt+X starts it.", 24, 404, 474, 32);
  SendMessageW(help, WM_SETFONT, (WPARAM)font, TRUE);
  refresh_ui();
}

static void toggle_hdr() {
  g_set.enabled = !g_set.enabled;
  g_cb_dirty = true;
  save_settings();
  if (!g_set.enabled) hide_overlay();
  refresh_ui();
}

static void apply_toggle() {
  ULONGLONG now = GetTickCount64();
  if (now - g_toggle_at < 200) return;
  g_toggle_at = now;
  toggle_hdr();
  g_attach_fail = nullptr;
  if (g_set.enabled) {
    HWND game = resolve_game();
    if (game) try_attach(game);
  }
}

static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_TOGGLE_HDR) {
    apply_toggle();
    return 0;
  }
  if (msg == WM_DESTROY) PostQuitMessage(0);
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK ui_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_CREATE) {
    create_ui_controls(hwnd);
    return 0;
  }
  if (msg == WM_CLOSE) {
    save_settings();
    PostQuitMessage(0);
    return 0;
  }
  if (msg == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE) {
    raise_ui();
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
  if (msg == WM_TOGGLE_HDR) {
    apply_toggle();
    return 0;
  }
  if (msg == WM_COMMAND && LOWORD(wp) == 100 && HIWORD(wp) == BN_CLICKED) {
    g_set.enabled = SendMessageW(g_enabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_cb_dirty = true;
    save_settings();
    if (g_set.enabled) {
      g_attach_fail = nullptr;
      HWND game = resolve_game();
      if (game) try_attach(game);
    } else {
      hide_overlay();
    }
    refresh_ui();
    return 0;
  }
  if (msg == WM_HSCROLL) {
    HWND source = (HWND)lp;
    for (int i = 0; i < 5; ++i) {
      if (source != g_tracks[i]) continue;
      set_slider_value(i, (int)SendMessageW(source, TBM_GETPOS, 0, 0));
      g_cb_dirty = true;
      refresh_ui();
      save_settings();
      break;
    }
    return 0;
  }
  if (msg == WM_SIZE) {
    g_menu = wp != SIZE_MINIMIZED;
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
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

static void show_settings() {
  g_menu = true;
  if (IsIconic(g_ui)) ShowWindow(g_ui, SW_RESTORE);
  place_ui();
  refresh_ui();
  raise_ui();
}

static void hide_settings() {
  g_menu = false;
  save_settings();
  if (g_ui) ShowWindow(g_ui, SW_HIDE);
}

static HWND g_hook_wnd = nullptr;

static HANDLE toggle_event() {
  if (!g_toggle_ev) g_toggle_ev = CreateEventW(nullptr, FALSE, FALSE, L"Local\\OpenHDR_toggle");
  return g_toggle_ev;
}

static HWND find_main_hwnd() {
  HWND w = FindWindowW(L"OpenHDRMain", nullptr);
  if (w) return w;
  w = FindWindowW(L"OpenHDRSettings", nullptr);
  if (w) return w;
  return FindWindowW(L"OpenHDROverlay", nullptr);
}

static bool main_running() {
  HWND w = find_main_hwnd();
  return w && IsWindow(w);
}

static void reap_stuck_mains() {
  if (main_running()) return;
  DWORD self = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  PROCESSENTRY32W pe{sizeof(pe)};
  if (Process32FirstW(snap, &pe)) {
    do {
      if (pe.th32ProcessID == self || _wcsicmp(pe.szExeFile, L"OpenHDR.exe") != 0) continue;
      HANDLE p = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
      if (!p) continue;
      TerminateProcess(p, 0);
      CloseHandle(p);
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
}

static void launch_main() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  wchar_t cmd[MAX_PATH + 4]{};
  swprintf_s(cmd, L"\"%s\"", path);
  if (CreateProcessW(path, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

static void dispatch_hotkey() {
  ULONGLONG now = GetTickCount64();
  if (now - g_hotkey_at.load() < 250) return;
  g_hotkey_at.store(now);
  if (main_running()) {
    if (HANDLE ev = toggle_event()) SetEvent(ev);
    if (HWND main = find_main_hwnd()) PostMessageW(main, WM_TOGGLE_HDR, 0, 0);
    return;
  }
  reap_stuck_mains();
  launch_main();
}

static LRESULT CALLBACK hook_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_HOTKEY || msg == WM_APP) {
    dispatch_hotkey();
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK hook_llkb(int code, WPARAM wp, LPARAM lp) {
  if (code == HC_ACTION && (wp == WM_SYSKEYDOWN || wp == WM_KEYDOWN)) {
    auto *k = reinterpret_cast<KBDLLHOOKSTRUCT *>(lp);
    bool alt = k && ((k->flags & LLKHF_ALTDOWN) || (GetAsyncKeyState(VK_MENU) & 0x8000));
    if (k && k->vkCode == 'X' && alt && g_hook_wnd) {
      PostMessageW(g_hook_wnd, WM_APP, 0, 0);
      return 1;
    }
  }
  return CallNextHookEx(nullptr, code, wp, lp);
}

static int run_hook(HINSTANCE inst) {
  HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\OpenHDR_hotkey");
  if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = hook_wnd_proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"OpenHDRHook";
  RegisterClassExW(&wc);
  g_hook_wnd = CreateWindowExW(0, wc.lpszClassName, L"OpenHDRHook", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
  RegisterHotKey(g_hook_wnd, 1, MOD_ALT | MOD_NOREPEAT, 'X');
  SetWindowsHookExW(WH_KEYBOARD_LL, hook_llkb, inst, 0);
  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}

static ULONGLONG g_last_frame = 0;
static ULONGLONG g_fail_until = 0;

static bool target_alive(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid) return false;
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return true;
  DWORD code = STILL_ACTIVE;
  GetExitCodeProcess(p, &code);
  CloseHandle(p);
  return code == STILL_ACTIVE;
}

static void detach() {
  hide_overlay();
  stop_capture();
  g_monitor_cap = false;
  g_target = nullptr;
  g_target_name[0] = 0;
  g_placed = {};
  g_last_frame = 0;
  refresh_ui();
}

static void try_attach(HWND host) {
  HWND target = window_for_host(host);
  if (!target || !IsWindow(target)) return;
  if (target == g_target && g_session) return;
  if (target == g_attach_fail && GetTickCount64() < g_fail_until) return;
  hide_overlay();
  if (!start_capture(target)) {
    g_attach_fail = target;
    g_fail_until = GetTickCount64() + 1000;
    return;
  }
  g_attach_fail = nullptr;
  g_fail_until = 0;
  g_target = target;
  g_last_game = target;
  g_placed = {};
  exe_name(target, g_target_name, 64);
  bind_output(target);
  set_clickthrough(true);
  refresh_ui();
}

static void ensure_hook(HINSTANCE inst) {
  HANDLE mtx = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\OpenHDR_hotkey");
  if (mtx) {
    CloseHandle(mtx);
    return;
  }
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(inst, path, MAX_PATH);
  ShellExecuteW(nullptr, L"open", path, L"--hook", nullptr, SW_HIDE);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (cmd && wcsstr(cmd, L"--hook")) return run_hook(inst);
  INITCOMMONCONTROLSEX common{sizeof(common), ICC_BAR_CLASSES};
  InitCommonControlsEx(&common);
  bool background = cmd && wcsstr(cmd, L"--background");
  HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\OpenHDR_desktop_overlay");
  if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
  toggle_event();
  frame_event();
  InitializeCriticalSection(&g_cap_cs);

  WNDCLASSEXW mainwc{sizeof(mainwc)};
  mainwc.lpfnWndProc = overlay_proc;
  mainwc.hInstance = inst;
  mainwc.lpszClassName = L"OpenHDRMain";
  RegisterClassExW(&mainwc);
  CreateWindowExW(0, L"OpenHDRMain", L"OpenHDRMain", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);

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
  WNDCLASSEXW uiw{sizeof(uiw)};
  uiw.lpfnWndProc = ui_proc;
  uiw.hInstance = inst;
  uiw.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  uiw.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  uiw.lpszClassName = L"OpenHDRSettings";
  RegisterClassExW(&uiw);

  g_overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP,
                              wc.lpszClassName, L"OpenHDR", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, inst, nullptr);
  SetWindowDisplayAffinity(g_overlay, WDA_EXCLUDEFROMCAPTURE);
  set_clickthrough(true);
  g_present_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_present_run.store(true);
  g_present_th = CreateThread(nullptr, 0, present_thread, nullptr, 0, nullptr);
  if (g_present_th) SetThreadPriority(g_present_th, THREAD_PRIORITY_ABOVE_NORMAL);
  g_ui = CreateWindowExW(WS_EX_APPWINDOW, uiw.lpszClassName, L"OpenHDR",
                         WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 80, 80, 540, 490, nullptr, nullptr,
                         inst, nullptr);
  ensure_hook(inst);
  if (!background) {
    g_host = GetForegroundWindow();
    show_settings();
  }

  MSG msg{};
  HWND last_fg = nullptr;
  ULONGLONG last_scan = 0;
  for (;;) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        g_present_run.store(false);
        if (g_present_stop) SetEvent(g_present_stop);
        if (g_present_th) {
          WaitForSingleObject(g_present_th, 1000);
          CloseHandle(g_present_th);
          g_present_th = nullptr;
        }
        return 0;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (WaitForSingleObject(toggle_event(), 0) == WAIT_OBJECT_0) apply_toggle();
    HWND fg = GetForegroundWindow();
    ULONGLONG now = GetTickCount64();
    if (g_last_game && !IsWindow(g_last_game)) g_last_game = nullptr;
    if (g_target && !target_alive(g_target)) detach();
    if (fg != last_fg || now - last_scan >= 250) {
      last_fg = fg;
      last_scan = now;
      HWND game = window_for_host(fg);
      if (game && target_alive(game)) {
        g_last_game = game;
        g_attach_fail = nullptr;
        try_attach(game);
      }
      if (g_target && !target_alive(g_target)) detach();
    }
    if (!g_set.enabled || !g_target) {
      g_allow_present.store(false);
      hide_overlay();
      HANDLE idle = toggle_event();
      MsgWaitForMultipleObjects(1, &idle, FALSE, 16, QS_ALLINPUT);
      continue;
    }
    if (fg != g_target && fg != g_overlay && fg != g_ui) {
      g_allow_present.store(false);
      hide_overlay();
      HANDLE idle = toggle_event();
      MsgWaitForMultipleObjects(1, &idle, FALSE, 16, QS_ALLINPUT);
      continue;
    }
    g_allow_present.store(true);
    if (g_frame_tick.load() && now - g_frame_tick.load() > 250) hide_overlay();
    HANDLE evs[2] = {toggle_event(), frame_event()};
    MsgWaitForMultipleObjects(2, evs, FALSE, 16, QS_ALLINPUT);
  }
}
