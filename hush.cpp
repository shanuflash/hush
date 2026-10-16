// Hush - system-wide microphone mute bound to a mouse button or key.
//
// Input:   Raw Input with RIDEV_INPUTSINK. Passive listener: no hooks, no DLL
//          injection, no synthetic input, never touches other processes.
// Output:  Core Audio IAudioEndpointVolume::SetMute on capture endpoints, so the
//          mute applies to every app (Discord, games, browsers, ...).
// Display: tray icon + optional click-through overlay badge (own window only).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shellscalingapi.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <cstdarg>
#include <cstdio>
#include <share.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")

using Microsoft::WRL::ComPtr;

// --debug: log input and mute activity to Hush.log next to the exe.
static FILE* g_log;

static void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fwprintf(g_log, L"%02d:%02d:%02d.%03d  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfwprintf(g_log, fmt, args);
    va_end(args);
    fputwc(L'\n', g_log);
    fflush(g_log);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

enum : DWORD { MODE_TOGGLE = 0, MODE_PTT = 1, MODE_PTM = 2 };
enum : DWORD { BIND_MOUSE = 0, BIND_KEY = 1 };
enum : DWORD { SCOPE_ALL = 0, SCOPE_DEFAULT = 1 };
enum : DWORD { OVL_OFF = 0, OVL_MUTED = 1, OVL_ALWAYS = 2, OVL_FLASH = 3, OVL_LIVE = 4 };
enum : DWORD { POS_TL = 0, POS_TC, POS_TR, POS_BL, POS_BC, POS_BR };
// 3 and 4 were Chip and Corner; they stay unused so old saved settings fall back to Capsule.
enum : DWORD { STYLE_CAPSULE = 0, STYLE_ORB = 1, STYLE_EDGE = 2, STYLE_GHOST = 5, STYLE_GLOW = 6 };
struct StyleInfo { DWORD id; const wchar_t* name; };
static const StyleInfo kStyles[] = {
    {STYLE_CAPSULE, L"Capsule"}, {STYLE_ORB, L"Orb"}, {STYLE_EDGE, L"Edge"}, {STYLE_GHOST, L"Ghost"}, {STYLE_GLOW, L"Glow"},
};
static bool IsStyle(DWORD v) {
    for (auto& st : kStyles)
        if (st.id == v) return true;
    return false;
}

// Sound themes. Chirp is generated in code; every other theme's mute and unmute
// .wav files are embedded in hush.rc as RCDATA kThemeResBase + 2*id and + 2*id + 1.
// Ids are saved in settings, so never reuse one.
enum : DWORD { THEME_CHIRP = 0, THEME_DISCORD = 1 };
struct ThemeInfo { DWORD id; const wchar_t* name; };
static const ThemeInfo kThemes[] = {
    {THEME_CHIRP, L"Chirp"}, {THEME_DISCORD, L"Discord"},
};
static const int kThemeResBase = 200;
static bool IsTheme(DWORD v) {
    for (auto& t : kThemes)
        if (t.id == v) return true;
    return false;
}

struct Settings {
    DWORD mode = MODE_TOGGLE;
    DWORD bindKind = BIND_MOUSE;
    DWORD bindCode = 4;   // mouse: 3=middle 4=back 5=forward, key: virtual-key code
    DWORD bindScan = 0;   // key only: scan code | E0 << 8 | E1 << 9; 0 = match by bindCode
    DWORD scope = SCOPE_ALL;
    DWORD sounds = 1;
    DWORD overlay = OVL_FLASH;
    DWORD overlayPos = POS_TC;
    DWORD overlayStyle = STYLE_CAPSULE;
    DWORD soundTheme = THEME_CHIRP;
    std::wstring muteSound, unmuteSound;  // custom .wav files; empty = the theme's sound
};

static Settings g_cfg;
static const wchar_t* kRegKey = L"Software\\Hush";
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kAppName = L"Hush";

static bool LoadSettings() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    auto get = [&](const wchar_t* name, DWORD& v, DWORD max) {
        DWORD d, sz = sizeof(d), type;
        if (RegQueryValueExW(k, name, nullptr, &type, (BYTE*)&d, &sz) == ERROR_SUCCESS && type == REG_DWORD && d <= max) v = d;
    };
    get(L"Mode", g_cfg.mode, MODE_PTM);
    get(L"BindKind", g_cfg.bindKind, BIND_KEY);
    get(L"BindCode", g_cfg.bindCode, 0xFF);
    get(L"BindScan", g_cfg.bindScan, 0x3FF);
    get(L"Scope", g_cfg.scope, SCOPE_DEFAULT);
    get(L"Sounds", g_cfg.sounds, 1);
    get(L"Overlay", g_cfg.overlay, OVL_LIVE);
    get(L"OverlayPos", g_cfg.overlayPos, POS_BR);
    get(L"OverlayStyle", g_cfg.overlayStyle, STYLE_GLOW);
    if (!IsStyle(g_cfg.overlayStyle)) g_cfg.overlayStyle = STYLE_CAPSULE;  // a removed style
    get(L"SoundTheme", g_cfg.soundTheme, 0xFF);
    if (!IsTheme(g_cfg.soundTheme)) g_cfg.soundTheme = THEME_CHIRP;
    auto getStr = [&](const wchar_t* name, std::wstring& v) {
        wchar_t buf[MAX_PATH * 2];
        DWORD sz = sizeof(buf) - sizeof(wchar_t), type;
        if (RegQueryValueExW(k, name, nullptr, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS && type == REG_SZ) {
            buf[sz / sizeof(wchar_t)] = 0;
            v = buf;
        }
    };
    getStr(L"MuteSound", g_cfg.muteSound);
    getStr(L"UnmuteSound", g_cfg.unmuteSound);
    RegCloseKey(k);
    return true;
}

static void SaveSettings() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    auto set = [&](const wchar_t* name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v)); };
    set(L"Mode", g_cfg.mode);
    set(L"BindKind", g_cfg.bindKind);
    set(L"BindCode", g_cfg.bindCode);
    set(L"BindScan", g_cfg.bindScan);
    set(L"Scope", g_cfg.scope);
    set(L"Sounds", g_cfg.sounds);
    set(L"Overlay", g_cfg.overlay);
    set(L"OverlayPos", g_cfg.overlayPos);
    set(L"OverlayStyle", g_cfg.overlayStyle);
    set(L"SoundTheme", g_cfg.soundTheme);
    auto setStr = [&](const wchar_t* name, const std::wstring& v) {
        if (v.empty()) RegDeleteValueW(k, name);
        else RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    };
    setStr(L"MuteSound", g_cfg.muteSound);
    setStr(L"UnmuteSound", g_cfg.unmuteSound);
    RegCloseKey(k);
}

static bool AutostartEnabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool on = RegQueryValueExW(k, kAppName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

static void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(k, kAppName, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kAppName);
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------

struct MouseButton { DWORD code; USHORT down, up; const wchar_t* name; };
static const MouseButton kMouseButtons[] = {
    {3, RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, L"Middle button"},
    {4, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, L"Back side button"},
    {5, RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, L"Forward side button"},
};

static const MouseButton* FindMouseButton(DWORD code) {
    for (auto& b : kMouseButtons)
        if (b.code == code) return &b;
    return nullptr;
}

static std::wstring BindName() {
    if (g_cfg.bindKind == BIND_MOUSE) {
        auto b = FindMouseButton(g_cfg.bindCode);
        return b ? b->name : L"(none)";
    }
    LONG lp = (LONG)((g_cfg.bindScan & 0xFF) << 16);
    if (g_cfg.bindScan & 0x100) lp |= 1 << 24;
    if (g_cfg.bindScan & 0x200) lp = 0x45 << 16;  // Pause arrives as E1 1D; its name lives at 45
    wchar_t name[64] = {};
    if (GetKeyNameTextW(lp, name, 64) > 0) return name;
    wchar_t buf[32];
    wsprintfW(buf, L"Key 0x%02X", g_cfg.bindCode);
    return buf;
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

#define WM_APP_TRAY    (WM_APP + 1)
#define WM_APP_REFRESH (WM_APP + 2)  // mute changed outside this app
#define WM_APP_DEVICES (WM_APP + 3)  // capture devices added/removed/default changed

static HWND g_hwnd;
static ComPtr<IMMDeviceEnumerator> g_devices;
// Event context passed to SetMute so our own changes can be told apart.
static const GUID kMuteContext = {0x6d1c7a2e, 0x4b8f, 0x4e1a, {0x9c, 0x31, 0x52, 0x7e, 0x0f, 0x8a, 0x1d, 0x44}};
static std::atomic<bool> g_refreshPending, g_devicesPending;

static void CollectEndpoints(std::vector<ComPtr<IAudioEndpointVolume>>& out) {
    if (!g_devices) return;
    auto add = [&](IMMDevice* d) {
        ComPtr<IAudioEndpointVolume> v;
        if (SUCCEEDED(d->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)v.GetAddressOf())))
            out.push_back(v);
    };
    if (g_cfg.scope == SCOPE_ALL) {
        ComPtr<IMMDeviceCollection> list;
        if (FAILED(g_devices->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &list))) return;
        UINT n = 0;
        list->GetCount(&n);
        for (UINT i = 0; i < n; i++) {
            ComPtr<IMMDevice> d;
            if (SUCCEEDED(list->Item(i, &d))) add(d.Get());
        }
        return;
    }
    // Default device only: console and communications defaults can differ.
    std::wstring seen;
    for (ERole role : {eConsole, eCommunications}) {
        ComPtr<IMMDevice> d;
        if (FAILED(g_devices->GetDefaultAudioEndpoint(eCapture, role, &d))) continue;
        LPWSTR id = nullptr;
        d->GetId(&id);
        std::wstring s = id ? id : L"";
        CoTaskMemFree(id);
        if (s == seen) continue;
        seen = s;
        add(d.Get());
    }
}

// Muted only if every targeted endpoint is muted. Returns false, leaving `muted`
// alone, when there is no microphone to ask.
static bool ReadMuted(bool& muted) {
    std::vector<ComPtr<IAudioEndpointVolume>> eps;
    CollectEndpoints(eps);
    if (eps.empty()) return false;
    muted = true;
    for (auto& v : eps) {
        BOOL m = FALSE;
        if (SUCCEEDED(v->GetMute(&m)) && !m) { muted = false; break; }
    }
    return true;
}

static void WriteMuted(bool mute) {
    std::vector<ComPtr<IAudioEndpointVolume>> eps;
    CollectEndpoints(eps);
    for (auto& v : eps) v->SetMute(mute, &kMuteContext);
}

// Core Audio calls these on its own threads; they only post to the UI thread.
class VolumeWatcher : public IAudioEndpointVolumeCallback {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioEndpointVolumeCallback)) { *out = this; return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA d) override {
        if (d && d->guidEventContext != kMuteContext && !g_refreshPending.exchange(true))
            PostMessageW(g_hwnd, WM_APP_REFRESH, 0, 0);
        return S_OK;
    }
};

class DeviceWatcher : public IMMNotificationClient {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) { *out = this; return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { Post(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { Post(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { Post(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole, LPCWSTR) override {
        if (flow == eCapture) Post();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    static void Post() {
        if (!g_devicesPending.exchange(true)) PostMessageW(g_hwnd, WM_APP_DEVICES, 0, 0);
    }
};

static VolumeWatcher g_volumeWatcher;
static DeviceWatcher g_deviceWatcher;
static std::vector<ComPtr<IAudioEndpointVolume>> g_watched;

static void UnwatchEndpoints() {
    for (auto& v : g_watched) v->UnregisterControlChangeNotify(&g_volumeWatcher);
    g_watched.clear();
}

static void WatchEndpoints() {
    UnwatchEndpoints();
    CollectEndpoints(g_watched);
    for (auto& v : g_watched) v->RegisterControlChangeNotify(&g_volumeWatcher);
}

// ---------------------------------------------------------------------------
// Feedback sounds: the theme's pair (generated or embedded) or the user's own
// .wav files, loaded into memory once and played async.
// ---------------------------------------------------------------------------

static std::vector<BYTE> g_sndMute, g_sndUnmute;    // current theme
static std::vector<BYTE> g_userMute, g_userUnmute;  // custom files; empty = use the theme

// Reads a .wav file whole. Returns nothing for a missing, oversized or non-WAV file.
static std::vector<BYTE> LoadWav(const std::wstring& path) {
    std::vector<BYTE> data;
    if (path.empty()) return data;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return data;
    LARGE_INTEGER size;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 44 && size.QuadPart <= 8 * 1024 * 1024) {
        data.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr) || got != data.size()) data.clear();
    }
    CloseHandle(f);
    if (data.size() < 12 || memcmp(data.data(), "RIFF", 4) != 0 || memcmp(data.data() + 8, "WAVE", 4) != 0) data.clear();
    return data;
}

static std::vector<BYTE> MakeChirp(std::initializer_list<float> freqs, int msEach) {
    const int rate = 44100;
    const int each = rate * msEach / 1000;
    const int total = each * (int)freqs.size();
    const DWORD dataBytes = total * 2;
    std::vector<BYTE> buf(44 + dataBytes);
    BYTE* p = buf.data();
    auto u32 = [&](size_t off, uint32_t v) { memcpy(p + off, &v, 4); };
    auto u16 = [&](size_t off, uint16_t v) { memcpy(p + off, &v, 2); };
    memcpy(p, "RIFF", 4); u32(4, 36 + dataBytes);
    memcpy(p + 8, "WAVEfmt ", 8); u32(16, 16);
    u16(20, 1); u16(22, 1); u32(24, rate); u32(28, rate * 2); u16(32, 2); u16(34, 16);
    memcpy(p + 36, "data", 4); u32(40, dataBytes);

    int16_t* s = (int16_t*)(p + 44);
    const int fade = rate * 6 / 1000;
    double phase = 0;
    int i = 0;
    for (float f : freqs) {
        for (int k = 0; k < each; k++, i++) {
            double env = 1.0;
            if (k < fade) env = (double)k / fade;
            else if (k > each - fade) env = (double)(each - k) / fade;
            phase += 2.0 * 3.14159265358979 * f / rate;
            s[i] = (int16_t)(std::sin(phase) * env * 0.22 * 32767);
        }
    }
    return buf;
}

// Copies an embedded .wav out of the exe, byte for byte.
static std::vector<BYTE> LoadWavResource(int id) {
    std::vector<BYTE> data;
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const BYTE* p = g ? (const BYTE*)LockResource(g) : nullptr;
    if (p) data.assign(p, p + SizeofResource(nullptr, r));
    return data;
}

static void LoadTheme(DWORD theme) {
    PlaySoundW(nullptr, nullptr, 0);  // stop anything still playing from the buffers being replaced
    if (theme != THEME_CHIRP) {
        g_sndMute = LoadWavResource(kThemeResBase + 2 * theme);
        g_sndUnmute = LoadWavResource(kThemeResBase + 2 * theme + 1);
        if (!g_sndMute.empty() && !g_sndUnmute.empty()) return;
    }
    g_sndMute = MakeChirp({700.f, 440.f}, 70);
    g_sndUnmute = MakeChirp({440.f, 700.f}, 70);
}

static void PlaySoundFor(bool muted) {
    const auto& user = muted ? g_userMute : g_userUnmute;
    const auto& b = !user.empty() ? user : muted ? g_sndMute : g_sndUnmute;
    PlaySoundW((LPCWSTR)b.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

static void PlayFeedback(bool muted) {
    if (g_cfg.sounds) PlaySoundFor(muted);
}

static const wchar_t* ThemeName() {
    for (auto& t : kThemes)
        if (t.id == g_cfg.soundTheme) return t.name;
    return kThemes[0].name;
}

// For the menu: the file's name, or the theme's.
static std::wstring SoundName(bool muted) {
    const std::wstring& path = muted ? g_cfg.muteSound : g_cfg.unmuteSound;
    if (path.empty()) return ThemeName();
    const wchar_t* slash = wcsrchr(path.c_str(), L'\\');
    std::wstring name = slash ? slash + 1 : path;
    if ((muted ? g_userMute : g_userUnmute).empty()) name += std::wstring(L" (can't play, using ") + ThemeName() + L")";
    return name;
}

// ---------------------------------------------------------------------------
// Mic glyph (drawn procedurally, 4x4 supersampled)
// ---------------------------------------------------------------------------

static const uint32_t kRed = 0xF23F43, kGreen = 0x23A55A, kGrey = 0x80848E;

static float SegDist(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay;
    float len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    float ex = px - (ax + t * dx), ey = py - (ay + t * dy);
    return std::sqrt(ex * ex + ey * ey);
}

// The mic shape and its slash, in unit coordinates (0..1 across the glyph box).
static bool MicAt(float u, float v) {
    float da = std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.47f) * (v - 0.47f));
    return SegDist(u, v, 0.5f, 0.27f, 0.5f, 0.47f) <= 0.12f ||
           (std::fabs(da - 0.205f) <= 0.04f && v >= 0.47f) ||
           SegDist(u, v, 0.5f, 0.675f, 0.5f, 0.77f) <= 0.04f ||
           SegDist(u, v, 0.39f, 0.77f, 0.61f, 0.77f) <= 0.04f;
}
static float SlashDist(float u, float v) { return SegDist(u, v, 0.24f, 0.22f, 0.76f, 0.80f); }

// Coloured disc with a white mic (slashed when muted). ARGB, straight alpha.
// none: no microphone present, drawn grey and slashed.
static std::vector<uint32_t> RenderMicGlyph(bool muted, int size, bool none = false) {
    const uint32_t bg = none ? kGrey : muted ? kRed : kGreen;
    muted = muted || none;
    std::vector<uint32_t> px(size * size);
    const int ss = 4;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < ss; sy++) {
                for (int sx = 0; sx < ss; sx++) {
                    float u = (x + (sx + 0.5f) / ss) / size;
                    float v = (y + (sy + 0.5f) / ss) / size;
                    float dc = std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f));
                    if (dc > 0.5f) continue;
                    uint32_t c = bg;
                    if (MicAt(u, v)) c = 0xFFFFFF;
                    if (muted) {
                        float ds = SlashDist(u, v);
                        if (ds <= 0.095f) c = bg;
                        if (ds <= 0.045f) c = 0xFFFFFF;
                    }
                    r += (c >> 16) & 0xFF; g += (c >> 8) & 0xFF; b += c & 0xFF; a += 255;
                }
            }
            if (a <= 0) { px[y * size + x] = 0; continue; }
            float k = a / 255;
            uint32_t A = (uint32_t)(a / (ss * ss) + 0.5f);
            uint32_t R = (uint32_t)(r / k + 0.5f), G = (uint32_t)(g / k + 0.5f), B = (uint32_t)(b / k + 0.5f);
            px[y * size + x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    }
    return px;
}

static HBITMAP CreateDib32(HDC dc, int w, int h, void** bits) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(dc, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
}

static HICON MakeTrayIcon(bool muted, int size, bool none = false) {
    std::vector<uint32_t> px = RenderMicGlyph(muted, size, none);
    void* bits = nullptr;
    HBITMAP color = CreateDib32(nullptr, size, size, &bits);
    if (!color) return nullptr;
    memcpy(bits, px.data(), px.size() * 4);
    std::vector<BYTE> maskBits(((size + 15) / 16) * 2 * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// ---------------------------------------------------------------------------
// Badge rendering: a small float canvas, signed-distance shapes, blur for glow
// and shadow. Only runs when the badge changes, never per frame.
// ---------------------------------------------------------------------------

struct Mask {
    int w = 0, h = 0;
    std::vector<float> a;
    Mask(int w_, int h_) : w(w_), h(h_), a((size_t)w_ * h_, 0.f) {}
};

static float Coverage(float sd) { return std::min(1.f, std::max(0.f, 0.5f - sd)); }
static float Clamp01(float v) { return std::min(1.f, std::max(0.f, v)); }

static float SdRoundRect(float px, float py, float x, float y, float w, float h, float r) {
    float qx = std::fabs(px - (x + w / 2)) - (w / 2 - r);
    float qy = std::fabs(py - (y + h / 2)) - (h / 2 - r);
    float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
}

template <class F>
static Mask MakeMask(int w, int h, F coverage) {
    Mask m(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) m.a[(size_t)y * w + x] = coverage(x + 0.5f, y + 0.5f);
    return m;
}

// Copy a smaller mask into a canvas-sized one at (ox, oy).
static Mask Placed(const Mask& src, int w, int h, int ox, int oy) {
    Mask m(w, h);
    for (int y = 0; y < src.h; y++)
        for (int x = 0; x < src.w; x++) {
            int tx = ox + x, ty = oy + y;
            if (tx >= 0 && ty >= 0 && tx < w && ty < h) m.a[(size_t)ty * w + tx] = src.a[(size_t)y * src.w + x];
        }
    return m;
}

// Three box passes per axis approximate a Gaussian of the given sigma.
static void Blur(Mask& m, float sigma) {
    int r = (int)((std::sqrt(4 * sigma * sigma + 1) - 1) / 2 + 0.5f);
    if (r < 1) return;
    std::vector<float> tmp(std::max(m.w, m.h));
    const float inv = 1.f / (2 * r + 1);
    auto pass = [&](float* data, int n, int stride) {
        for (int i = 0; i < n; i++) tmp[i] = data[(size_t)i * stride];
        float sum = 0;
        for (int i = 0; i <= r && i < n; i++) sum += tmp[i];
        for (int i = 0; i < n; i++) {
            data[(size_t)i * stride] = sum * inv;
            if (i + r + 1 < n) sum += tmp[i + r + 1];
            if (i - r >= 0) sum -= tmp[i - r];
        }
    };
    for (int k = 0; k < 3; k++) {
        for (int y = 0; y < m.h; y++) pass(&m.a[(size_t)y * m.w], m.w, 1);
        for (int x = 0; x < m.w; x++) pass(&m.a[x], m.h, m.w);
    }
}

struct Canvas {
    int w, h;
    std::vector<float> px;  // premultiplied RGBA
    Canvas(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_ * 4, 0.f) {}

    void Blend(int x, int y, float r, float g, float b, float a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        float* p = &px[((size_t)y * w + x) * 4];
        p[0] = r * a + p[0] * (1 - a);
        p[1] = g * a + p[1] * (1 - a);
        p[2] = b * a + p[2] * (1 - a);
        p[3] = a + p[3] * (1 - a);
    }
    // Solid colour through a mask placed at (ox, oy).
    void Fill(const Mask& m, int ox, int oy, uint32_t rgb, float alpha) {
        float r = ((rgb >> 16) & 0xFF) / 255.f, g = ((rgb >> 8) & 0xFF) / 255.f, b = (rgb & 0xFF) / 255.f;
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) Blend(ox + x, oy + y, r, g, b, m.a[(size_t)y * m.w + x] * alpha);
    }
    // Straight-alpha ARGB image.
    void Image(const std::vector<uint32_t>& img, int size, int ox, int oy) {
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                uint32_t c = img[(size_t)y * size + x];
                Blend(ox + x, oy + y, ((c >> 16) & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f, (c & 0xFF) / 255.f,
                      (c >> 24) / 255.f);
            }
    }
};

struct FontChoice { const wchar_t* face; int weight; };

// Segoe UI Variable (Windows 11) at its Text optical size, else classic Segoe UI.
// GDI exposes each named instance as its own family, truncated to 31 characters.
static FontChoice BadgeFont() {
    static const FontChoice kFaces[] = {
        {L"Segoe UI Variable Text Semibold", FW_SEMIBOLD},
        {L"Segoe UI Semibold", FW_SEMIBOLD},
    };
    static FontChoice chosen = [] {
        HDC dc = GetDC(nullptr);
        FontChoice pick = {L"Segoe UI", FW_BOLD};
        for (auto& f : kFaces) {
            LOGFONTW lf = {};
            lf.lfCharSet = DEFAULT_CHARSET;
            lstrcpynW(lf.lfFaceName, f.face, LF_FACESIZE);
            bool found = false;
            EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
                *(bool*)p = true;
                return 0;
            }, (LPARAM)&found, 0);
            if (found) { pick = f; break; }
        }
        ReleaseDC(nullptr, dc);
        return pick;
    }();
    return chosen;
}

// White-on-black GDI text read back as a coverage mask (GDI text has no alpha).
static Mask TextMask(const wchar_t* text, int px, int spacing) {
    HDC dc = CreateCompatibleDC(nullptr);
    const FontChoice fc = BadgeFont();
    HFONT font = CreateFontW(-px, 0, 0, 0, fc.weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, fc.face);
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetTextCharacterExtra(dc, spacing);
    const int len = lstrlenW(text);
    SIZE ts = {1, 1};
    GetTextExtentPoint32W(dc, text, len, &ts);
    Mask m(std::max(1L, ts.cx), std::max(1L, ts.cy));
    void* bits = nullptr;
    if (HBITMAP bmp = CreateDib32(dc, m.w, m.h, &bits)) {
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        memset(bits, 0, (size_t)m.w * m.h * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutW(dc, 0, 0, text, len);
        GdiFlush();
        const uint32_t* p = (const uint32_t*)bits;
        for (size_t i = 0; i < m.a.size(); i++)
            m.a[i] = std::max({(p[i] >> 16) & 0xFF, (p[i] >> 8) & 0xFF, p[i] & 0xFF}) / 255.f;
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);
    return m;
}

// Dark translucent surface behind the Capsule: drop shadow, fill, hairline.
static void PaintMaterial(Canvas& cv, float x, float y, float w, float h, float r, float s) {
    Mask shadow = MakeMask(cv.w, cv.h, [&](float px, float py) { return Coverage(SdRoundRect(px, py, x, y + 6 * s, w, h, r)); });
    Blur(shadow, 9 * s);
    cv.Fill(shadow, 0, 0, 0x000000, 0.28f);
    Mask body = MakeMask(cv.w, cv.h, [&](float px, float py) { return Coverage(SdRoundRect(px, py, x, y, w, h, r)); });
    cv.Fill(body, 0, 0, 0x1E1F22, 0.86f);
    Mask hair = MakeMask(cv.w, cv.h, [&](float px, float py) {
        float d = SdRoundRect(px, py, x, y, w, h, r);
        return Coverage(d) - Coverage(d + 1.f);
    });
    cv.Fill(hair, 0, 0, 0xFFFFFF, 0.07f);
}

struct Badge {
    Canvas cv;
    RECT body;  // the visible shape inside the canvas; glow and shadow spill around it
};

static int Px(float v, float s) { return (int)(v * s + 0.5f); }

static Badge BuildCapsule(bool muted, bool none, const std::wstring& label, float s) {
    const wchar_t* text = !label.empty() ? label.c_str() : muted ? L"MUTED" : L"LIVE";
    Mask t = TextMask(text, Px(13, s), 0);
    const int h = Px(30, s), g = Px(20, s), padL = Px(5, s), gap = Px(7, s), padR = Px(13, s), P = Px(28, s);
    const int w = padL + g + gap + t.w + padR;
    Canvas cv(w + 2 * P, h + 2 * P);
    PaintMaterial(cv, (float)P, (float)P, (float)w, (float)h, h / 2.f, s);
    cv.Image(RenderMicGlyph(muted, g, none), g, P + padL, P + (h - g) / 2);
    cv.Fill(t, P + padL + g + gap, P + (h - t.h) / 2, 0xFFFFFF, 1.f);
    return {std::move(cv), {P, P, P + w, P + h}};
}

static Badge BuildOrb(bool muted, float s) {
    const uint32_t c = muted ? kRed : kGreen;
    const int d = Px(40, s), g = Px(34, s), P = Px(24, s);
    Canvas cv(d + 2 * P, d + 2 * P);
    const float cx = P + d / 2.f;
    Mask disc = MakeMask(cv.w, cv.h, [&](float px, float py) { return Coverage(std::hypot(px - cx, py - cx) - d / 2.f); });
    Mask glow = disc;
    Blur(glow, 9 * s);
    cv.Fill(glow, 0, 0, c, 0.5f);
    cv.Fill(disc, 0, 0, c, 0.32f);
    cv.Image(RenderMicGlyph(muted, g), g, P + (d - g) / 2, P + (d - g) / 2);
    return {std::move(cv), {P, P, P + d, P + d}};
}

static float EaseOut(float t) { t = Clamp01(t); return 1 - (1 - t) * (1 - t) * (1 - t); }

static const UINT kEdgeAnimMs = 600;

// anim: 0..1 through the state-change animation; 1 is the resting badge. The line
// draws out from the centre, its bloom flares and settles, and the word fades in.
static Badge BuildEdge(bool muted, bool bottom, float s, float anim = 1.f) {
    const uint32_t c = muted ? kRed : kGreen;
    const int W = Px(240, s), lh = Px(4, s), gap = Px(7, s), P = Px(20, s), spacing = Px(1.76f, s);
    Mask t = TextMask(muted ? L"MUTED" : L"LIVE", Px(11, s), spacing);
    const int bodyH = lh + gap + t.h;
    Canvas cv(W + 2 * P, bodyH + 2 * P);
    const float ms = anim * kEdgeAnimMs;
    const float draw = EaseOut(ms / 480), settle = EaseOut(ms / kEdgeAnimMs), word = EaseOut((ms - 120) / 340);
    const float lineY = (float)(bottom ? P + bodyH - lh : P);
    const int textY = (bottom ? P : P + lh + gap) - (int)std::lround(3 * s * (1 - word));
    // Line fades out over its outer 22% on each side. Muted is dashed, so the state
    // reads without relying on red versus green.
    const float cx = P + W / 2.f, dash = 12 * s, period = 18 * s;
    const float feather = 24 * s, reach = (W / 2.f + feather) * (0.12f + 0.88f * draw);
    Mask line = MakeMask(cv.w, cv.h, [&](float px, float py) {
        float u = (px - P) / W;
        float f = Clamp01(u < 0.22f ? u / 0.22f : u > 0.78f ? (1 - u) / 0.22f : 1.f);
        f *= Clamp01((reach - std::fabs(px - cx)) / feather);  // how far it has drawn out
        if (!muted) return Coverage(SdRoundRect(px, py, (float)P, lineY, (float)W, (float)lh, lh / 2.f)) * f;
        // Nearest dash, centred on the middle of the line, with rounded ends.
        float dc = cx + std::round((px - cx) / period) * period;
        return Coverage(SdRoundRect(px, py, dc - dash / 2, lineY, dash, (float)lh, lh / 2.f)) * f;
    });
    Mask glow = line;
    Blur(glow, (12 - 5 * settle) * s);
    cv.Fill(glow, 0, 0, c, 1 - 0.3f * settle);
    cv.Fill(line, 0, 0, c, 0.3f + 0.7f * draw);
    if (word <= 0) return {std::move(cv), {P, P, P + W, P + bodyH}};

    const int tx = P + (W - t.w + spacing) / 2;
    // Soft dark halo so the white word survives bright scenes; invisible over dark ones.
    // Blurred thin strokes spread very faint, so the halo is boosted back up after blurring.
    Mask halo = Placed(t, cv.w, cv.h, tx, textY);
    Blur(halo, 2.5f * s);
    for (float& v : halo.a) v = std::min(1.f, v * 3.f);
    cv.Fill(halo, 0, 0, 0x000000, 0.55f * word);
    Mask shade = Placed(t, cv.w, cv.h, tx, textY + Px(1, s));
    Blur(shade, 1 * s);
    cv.Fill(shade, 0, 0, 0x000000, 0.7f * word);
    Mask tglow = Placed(t, cv.w, cv.h, tx, textY);
    Blur(tglow, 4 * s);
    cv.Fill(tglow, 0, 0, c, 0.7f * word);
    cv.Fill(t, tx, textY, 0xFFFFFF, word);
    return {std::move(cv), {P, P, P + W, P + bodyH}};
}

// Coverage of a unit-coordinate shape in a size x size box, 4x4 supersampled.
template <class F>
static Mask GlyphMask(int size, F inside) {
    Mask m(size, size);
    const int ss = 4;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int hits = 0;
            for (int sy = 0; sy < ss; sy++)
                for (int sx = 0; sx < ss; sx++)
                    hits += inside((x + (sx + 0.5f) / ss) / size, (y + (sy + 0.5f) / ss) / size);
            m.a[(size_t)y * size + x] = hits / float(ss * ss);
        }
    return m;
}

// Ghost: the mic on its own, white with a coloured glow. Muted cuts a red slash through it.
static Badge BuildGhost(bool muted, float s) {
    const uint32_t c = muted ? kRed : kGreen;
    const int g = Px(36, s), P = Px(16, s);
    Canvas cv(g + 2 * P, g + 2 * P);
    Mask mic = Placed(GlyphMask(g, [&](float u, float v) { return MicAt(u, v) && !(muted && SlashDist(u, v) <= 0.095f); }),
                      cv.w, cv.h, P, P);
    Mask slash = Placed(GlyphMask(g, [&](float u, float v) { return muted && SlashDist(u, v) <= 0.045f; }), cv.w, cv.h, P, P);
    Mask shape = mic;
    for (size_t i = 0; i < shape.a.size(); i++) shape.a[i] = std::max(shape.a[i], slash.a[i]);
    Mask glow = shape;
    Blur(glow, 6 * s);
    for (float& v : glow.a) v = std::min(1.f, v * 2.f);  // thin strokes blur faint; bring the glow back up
    cv.Fill(glow, 0, 0, c, 0.85f);
    Mask shade = Placed(shape, cv.w, cv.h, 0, Px(1, s));
    Blur(shade, 2 * s);
    cv.Fill(shade, 0, 0, 0x000000, 0.6f);
    cv.Fill(mic, 0, 0, 0xFFFFFF, 1.f);
    cv.Fill(slash, 0, 0, c, 1.f);
    return {std::move(cv), {P, P, P + g, P + g}};
}

// Glow: coloured light pouring in from the screen corner. No shape, no text.
static Badge BuildGlow(bool muted, DWORD pos, float s) {
    const uint32_t c = muted ? kRed : kGreen;
    const int R = Px(220, s);
    Canvas cv(R, R);
    const float ox = (pos == POS_TL || pos == POS_BL) ? 0.f : (float)R, oy = pos <= POS_TR ? 0.f : (float)R;
    // Main pool: strong at the corner and holding its colour well out, easing to nothing by
    // the box's edges so it never clips.
    Mask light = MakeMask(R, R, [&](float px, float py) {
        float t = Clamp01(std::hypot(px - ox, py - oy) / R);
        return std::pow(1 - t, 1.4f);
    });
    cv.Fill(light, 0, 0, c, 0.9f);
    // A small, lighter hot spot right in the corner, so it reads as a light source.
    const float core = 0.22f * R;
    Mask hot = MakeMask(R, R, [&](float px, float py) {
        float t = Clamp01(std::hypot(px - ox, py - oy) / core);
        return (1 - t) * (1 - t);
    });
    auto mix = [&](int shift) { return (uint32_t)((((c >> shift) & 0xFF) * 65 + 255 * 35) / 100) << shift; };
    cv.Fill(hot, 0, 0, mix(16) | mix(8) | mix(0), 0.6f);
    return {std::move(cv), {0, 0, R, R}};
}

// Edge only sits at a center position, Glow only in a corner.
static DWORD EffectivePos(DWORD style, DWORD pos) {
    const bool top = pos <= POS_TR;
    if (style == STYLE_EDGE) return top ? POS_TC : POS_BC;
    if (style == STYLE_GLOW && (pos == POS_TC || pos == POS_BC)) return top ? POS_TR : POS_BR;
    return pos;
}

// ---------------------------------------------------------------------------
// Overlay window: layered, click-through, never activated, hidden from capture
// ---------------------------------------------------------------------------

enum : UINT_PTR { TIMER_OVL_HOLD = 1, TIMER_OVL_FADE = 2, TIMER_OVL_TOPMOST = 3, TIMER_CAPTURE = 4, TIMER_OVL_ANIM = 5 };

// Last known mic state. While no microphone is present g_noMic is set and g_muted
// keeps its old value, so a mic that comes back is put back the way it was.
static bool g_muted, g_noMic;

static HWND g_overlay;
static bool g_overlayVisible;
static int g_overlayAlpha;
static bool g_overlayMuted, g_overlayNone;
static int g_fadeFrom, g_fadeTo;  // alpha animation, driven by TIMER_OVL_FADE
static ULONGLONG g_fadeStart;
static UINT g_fadeMs;
static std::wstring g_overlayLabel;  // non-empty: a text prompt, always drawn as a Capsule
static float g_edgeAnim = 1.f;       // Edge's change animation, 0..1, driven by TIMER_OVL_ANIM
static ULONGLONG g_edgeAnimStart;

static void PaintOverlay() {
    HMONITOR mon = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    const float s = dpiX / 96.0f;

    const DWORD style = g_overlayLabel.empty() ? g_cfg.overlayStyle : STYLE_CAPSULE;
    const DWORD pos = EffectivePos(style, g_cfg.overlayPos);
    const bool muted = g_overlayMuted;
    Badge b = style == STYLE_ORB   ? BuildOrb(muted, s)
            : style == STYLE_EDGE  ? BuildEdge(muted, pos >= POS_BL, s, g_edgeAnim)
            : style == STYLE_GHOST ? BuildGhost(muted, s)
            : style == STYLE_GLOW  ? BuildGlow(muted, pos, s)
                                   : BuildCapsule(muted, g_overlayNone, g_overlayLabel, s);

    // Place the visible shape; Edge and Glow sit flush against the screen edge.
    const int bw = b.body.right - b.body.left, bh = b.body.bottom - b.body.top;
    const int margin = (style == STYLE_EDGE || style == STYLE_GLOW) ? 0 : Px(16, s);
    const RECT& rc = mi.rcWork;
    int sx = (pos == POS_TL || pos == POS_BL) ? rc.left + margin
           : (pos == POS_TC || pos == POS_BC) ? (rc.left + rc.right - bw) / 2
                                              : rc.right - margin - bw;
    int sy = pos <= POS_TR ? rc.top + margin : rc.bottom - margin - bh;
    RECT win = {sx - b.body.left, sy - b.body.top, sx - b.body.left + b.cv.w, sy - b.body.top + b.cv.h};
    RECT vis;  // keep glow from spilling onto a neighbouring monitor
    if (!IntersectRect(&vis, &win, &mi.rcMonitor)) return;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP bmp = CreateDib32(mem, b.cv.w, b.cv.h, &bits);
    if (bmp) {
        uint32_t* p = (uint32_t*)bits;
        for (size_t i = 0; i < (size_t)b.cv.w * b.cv.h; i++) {
            const float* c = &b.cv.px[i * 4];
            p[i] = ((uint32_t)(c[3] * 255 + 0.5f) << 24) | ((uint32_t)(c[0] * 255 + 0.5f) << 16) |
                   ((uint32_t)(c[1] * 255 + 0.5f) << 8) | (uint32_t)(c[2] * 255 + 0.5f);
        }
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        POINT dst = {vis.left, vis.top};
        SIZE size = {vis.right - vis.left, vis.bottom - vis.top};
        POINT src = {vis.left - win.left, vis.top - win.top};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, (BYTE)g_overlayAlpha, AC_SRC_ALPHA};
        UpdateLayeredWindow(g_overlay, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

static void SetOverlayAlpha(int alpha) {
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, (BYTE)alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_overlay, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &blend, ULW_ALPHA);
}

static const UINT kFadeInMs = 140, kFadeOutMs = 240;

// Animate alpha from its current value; the duration shrinks if we start part way.
static void StartFade(int to, UINT fullMs) {
    g_fadeFrom = g_overlayAlpha;
    g_fadeTo = to;
    g_fadeStart = GetTickCount64();
    g_fadeMs = std::max(1u, (UINT)(fullMs * std::abs(to - g_overlayAlpha) / 255));
    SetTimer(g_hwnd, TIMER_OVL_FADE, 10, nullptr);
}

static void HideOverlay() {
    KillTimer(g_hwnd, TIMER_OVL_HOLD);
    KillTimer(g_hwnd, TIMER_OVL_FADE);
    KillTimer(g_hwnd, TIMER_OVL_TOPMOST);
    KillTimer(g_hwnd, TIMER_OVL_ANIM);
    ShowWindow(g_overlay, SW_HIDE);
    g_overlayVisible = false;
    g_overlayAlpha = 0;
    g_edgeAnim = 1.f;
}

// Honours "Animation effects" in Windows settings.
static bool AnimationsEnabled() {
    BOOL on = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
    return on != FALSE;
}

// Fade in eases out (fast start, soft landing); fade out eases in and out.
static void StepFade() {
    float t = Clamp01((float)(GetTickCount64() - g_fadeStart) / g_fadeMs);
    float e = g_fadeTo > g_fadeFrom ? 1 - (1 - t) * (1 - t) * (1 - t)
                                    : t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.f) / 2;
    g_overlayAlpha = (int)(g_fadeFrom + (g_fadeTo - g_fadeFrom) * e + 0.5f);
    if (t >= 1) {
        KillTimer(g_hwnd, TIMER_OVL_FADE);
        if (g_fadeTo == 0) { HideOverlay(); return; }
    }
    SetOverlayAlpha(g_overlayAlpha);
}

static void ShowOverlay(bool muted, const std::wstring& label = L"") {
    KillTimer(g_hwnd, TIMER_OVL_HOLD);
    KillTimer(g_hwnd, TIMER_OVL_FADE);
    // Edge plays its draw-out whenever what it shows changes; a repaint of the same state doesn't.
    const bool changed = !g_overlayVisible || muted != g_overlayMuted || g_noMic != g_overlayNone || !g_overlayLabel.empty();
    if (label.empty() && g_cfg.overlayStyle == STYLE_EDGE && changed && AnimationsEnabled()) {
        g_edgeAnim = 0.f;
        g_edgeAnimStart = GetTickCount64();
        SetTimer(g_hwnd, TIMER_OVL_ANIM, 10, nullptr);
    } else if (!label.empty()) {
        KillTimer(g_hwnd, TIMER_OVL_ANIM);
        g_edgeAnim = 1.f;
    }
    g_overlayMuted = muted;
    g_overlayNone = g_noMic;
    g_overlayLabel = label;
    if (!g_overlayVisible) g_overlayAlpha = 0;  // appear from nothing; a mid-fade badge picks up where it is
    PaintOverlay();
    SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_overlayVisible = true;
    if (g_overlayAlpha < 255) StartFade(255, kFadeInMs);
    // Borderless games can sit above other topmost windows; re-assert now and then.
    SetTimer(g_hwnd, TIMER_OVL_TOPMOST, 2000, nullptr);
}

static void FlashOverlay(bool muted, UINT holdMs, const std::wstring& label = L"") {
    ShowOverlay(muted, label);
    SetTimer(g_hwnd, TIMER_OVL_HOLD, holdMs, nullptr);
}

static bool g_capturing;  // waiting for a new hotkey

// Whether the current settings keep a badge on screen once any flash is over.
static bool WantsPersistentBadge() {
    if (g_noMic) return false;
    return g_cfg.overlay == OVL_ALWAYS || (g_cfg.overlay == OVL_MUTED && g_muted) ||
           (g_cfg.overlay == OVL_LIVE && !g_muted);
}

// changed: the mic state just changed (or the hotkey was pressed with no mic),
// so transient badges should flash.
static void UpdateOverlay(bool changed) {
    if (g_capturing) return;  // keep the "press new hotkey" prompt up
    if (g_noMic) {
        // Never claim LIVE or MUTED for a mic that isn't there.
        if (changed && g_cfg.overlay != OVL_OFF) FlashOverlay(false, 1500, L"No microphone");
        else if (!changed) HideOverlay();
        return;
    }
    switch (g_cfg.overlay) {
        case OVL_ALWAYS: ShowOverlay(g_muted); return;
        case OVL_MUTED:
        case OVL_LIVE:
            // Steady badge in the chosen state; a short flash when entering the other one.
            if (WantsPersistentBadge()) ShowOverlay(g_muted);
            else if (changed) FlashOverlay(g_muted, 1000);
            else HideOverlay();
            return;
        case OVL_FLASH:
            // Nothing stays on screen, so a fullscreen game is never left composited.
            if (changed) FlashOverlay(g_muted, 1500);
            return;
        default: HideOverlay(); return;
    }
}

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------

enum : UINT {
    ID_TOGGLE = 1,
    ID_MODE_TOGGLE = 10, ID_MODE_PTT, ID_MODE_PTM,
    ID_REBIND = 20,
    ID_SCOPE_ALL = 30, ID_SCOPE_DEFAULT,
    ID_SOUNDS = 40, ID_AUTOSTART, ID_SOUND_MUTE, ID_SOUND_UNMUTE,
    ID_OVL_OFF = 50, ID_OVL_MUTED, ID_OVL_ALWAYS, ID_OVL_FLASH, ID_OVL_LIVE,  // ID_OVL_OFF + OVL_*
    ID_POS_FIRST = 60,    // + POS_*
    ID_STYLE_FIRST = 70,  // + STYLE_*
    ID_THEME_FIRST = 80,  // + THEME_*
    ID_EXIT = 99,
};

static HICON g_iconLive, g_iconMuted, g_iconNone;
static NOTIFYICONDATAW g_nid;
static UINT g_msgTaskbarCreated;
static bool g_pressed;     // bound button currently held
static bool g_regMouse, g_regKbd;

static void RegisterRaw(bool mouse, bool kbd) {
    auto apply = [](USHORT usage, bool want, bool& have) {
        if (want == have) return;
        RAWINPUTDEVICE r = {0x01, usage, want ? (DWORD)RIDEV_INPUTSINK : (DWORD)RIDEV_REMOVE, want ? g_hwnd : nullptr};
        BOOL ok = RegisterRawInputDevices(&r, 1, sizeof(r));
        Log(L"RegisterRawInputDevices usage=0x%02X %s -> %s (err %lu)", usage, want ? L"add" : L"remove",
            ok ? L"ok" : L"FAILED", ok ? 0 : GetLastError());
        if (ok) have = want;
    };
    apply(0x02, mouse, g_regMouse);
    apply(0x06, kbd, g_regKbd);
}

static void RegisterForBinding() {
    if (g_capturing) RegisterRaw(true, true);
    else if (g_log) RegisterRaw(true, g_cfg.bindKind == BIND_KEY);  // debug: see every mouse button
    else RegisterRaw(g_cfg.bindKind == BIND_MOUSE, g_cfg.bindKind == BIND_KEY);
}

static void UpdateTray(DWORD msg = NIM_MODIFY) {
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.hIcon = g_noMic ? g_iconNone : g_muted ? g_iconMuted : g_iconLive;
    static const wchar_t* modeNames[] = {L"toggle", L"push-to-talk", L"push-to-mute"};
    std::wstring tip = std::wstring(g_noMic ? L"No microphone" : g_muted ? L"Mic MUTED" : L"Mic LIVE") + L"\n" +
                       BindName() + L" → " + modeNames[g_cfg.mode];
    lstrcpynW(g_nid.szTip, tip.c_str(), ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(msg, &g_nid);
}

static void AddTray() {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    UpdateTray(NIM_ADD);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

static void Notify(const wchar_t* title, const std::wstring& text) {
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    lstrcpynW(n.szInfoTitle, title, ARRAYSIZE(n.szInfoTitle));
    lstrcpynW(n.szInfo, text.c_str(), ARRAYSIZE(n.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// The current mute was applied by Hush, so Hush lifts it again on exit. Cleared
// when the mic is unmuted, or muted by Windows or another app.
static bool g_muteOwned;

// Re-read the endpoints into g_muted / g_noMic. Returns whether muted/live flipped.
static bool ReadState() {
    bool m = g_muted;
    g_noMic = !ReadMuted(m);
    bool flipped = m != g_muted;
    g_muted = m;
    return flipped;
}

// Re-read the real state (it may have been changed by Windows or another app, or
// the mic may have come or gone). Returns whether muted/live flipped.
static bool RefreshState() {
    bool wasNone = g_noMic;
    bool flipped = ReadState();
    if (flipped || wasNone != g_noMic) {
        UpdateTray();
        UpdateOverlay(true);
    }
    return flipped;
}

static void SetMute(bool mute, bool feedback = true) {
    WriteMuted(mute);
    bool wasNone = g_noMic;
    bool flipped = ReadState();
    if (!g_noMic) g_muteOwned = g_muted;
    if (g_log) {
        std::vector<ComPtr<IAudioEndpointVolume>> eps;
        CollectEndpoints(eps);
        Log(L"SetMute(%d): %zu endpoint(s), now muted=%d", mute, eps.size(), g_muted);
    }
    UpdateTray();
    UpdateOverlay(flipped || g_noMic || wasNone);  // with no mic, every press says so
    if (flipped && feedback) PlayFeedback(g_muted);
}

static void ToggleMute() {
    bool m = g_muted;
    ReadMuted(m);  // the cached state can lag an outside change by a message
    SetMute(!m);
}

// Undo a mute Hush applied, so the mic isn't left dead with nothing on screen to say so.
static void ReleaseMute() {
    if (g_muteOwned && g_muted && !g_noMic) WriteMuted(false);
    g_muteOwned = false;
}

static void OnBindEvent(bool down) {
    if (down == g_pressed) return;  // ignore key auto-repeat and stray ups
    g_pressed = down;
    switch (g_cfg.mode) {
        case MODE_TOGGLE: if (down) ToggleMute(); break;
        case MODE_PTT:    SetMute(!down); break;
        case MODE_PTM:    SetMute(down); break;
    }
}

// Hotkey capture is prompted on screen: tray balloons are easy to miss or suppressed.
static void StartCapture() {
    g_capturing = true;
    RegisterForBinding();
    ShowOverlay(g_muted, L"Press new hotkey…  (Esc cancels)");
    SetTimer(g_hwnd, TIMER_CAPTURE, 10000, nullptr);
}

static void EndCapture(const std::wstring& message) {
    g_capturing = false;
    g_pressed = false;
    KillTimer(g_hwnd, TIMER_CAPTURE);
    RegisterForBinding();
    UpdateTray();
    FlashOverlay(g_muted, 1800, message);
}

static void FinishCapture(DWORD kind, DWORD code, DWORD scan) {
    g_cfg.bindKind = kind;
    g_cfg.bindCode = code;
    g_cfg.bindScan = scan;
    SaveSettings();
    EndCapture(L"Hotkey: " + BindName());
}

static void OnRawInput(HRAWINPUT h) {
    RAWINPUT ri;
    UINT size = sizeof(ri);
    if (GetRawInputData(h, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;

    if (ri.header.dwType == RIM_TYPEMOUSE) {
        USHORT f = ri.data.mouse.usButtonFlags;
        if (!f) return;  // plain movement, the overwhelmingly common case
        Log(L"mouse buttonFlags=0x%04X device=%p capturing=%d bind=%lu/%lu", f, ri.header.hDevice, g_capturing,
            g_cfg.bindKind, g_cfg.bindCode);
        if (g_capturing) {
            for (auto& b : kMouseButtons)
                if (f & b.down) { FinishCapture(BIND_MOUSE, b.code, 0); return; }
            return;
        }
        if (g_cfg.bindKind != BIND_MOUSE) return;
        auto b = FindMouseButton(g_cfg.bindCode);
        if (!b) return;
        if (f & b->down) OnBindEvent(true);
        if (f & b->up) OnBindEvent(false);
    } else if (ri.header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD& k = ri.data.keyboard;
        if (k.VKey == 0xFF || k.VKey == 0) return;  // fake keys from escape sequences
        // VKey can't tell left/right Ctrl, Shift and Alt apart, or Enter from numpad
        // Enter, so match on the physical key. Bindings saved without a scan code,
        // or keys injected with none, fall back to the virtual key.
        const DWORD scan = k.MakeCode ? (k.MakeCode & 0xFF) | ((k.Flags & RI_KEY_E0) ? 0x100 : 0) |
                                            ((k.Flags & RI_KEY_E1) ? 0x200 : 0)
                                      : 0;
        const bool isBound = g_cfg.bindKind == BIND_KEY &&
                             (g_cfg.bindScan && scan ? scan == g_cfg.bindScan : k.VKey == g_cfg.bindCode);
        // Never log ordinary typing: only the bound key, or the key chosen while capturing.
        if (g_capturing || isBound)
            Log(L"key vk=0x%02X scan=0x%03lX flags=0x%X capturing=%d", k.VKey, scan, k.Flags, g_capturing);
        bool down = !(k.Flags & RI_KEY_BREAK);
        if (g_capturing) {
            if (!down) return;
            if (k.VKey == VK_ESCAPE) {
                EndCapture(L"Hotkey unchanged: " + BindName());
                return;
            }
            FinishCapture(BIND_KEY, k.VKey, scan);
            return;
        }
        if (isBound) OnBindEvent(down);
    }
}

static void SetMode(DWORD mode) {
    g_cfg.mode = mode;
    g_pressed = false;
    SaveSettings();
    if (mode == MODE_PTT) SetMute(true);
    else if (mode == MODE_PTM) SetMute(false);
    else UpdateTray();
}

static void SetScope(DWORD scope) {
    g_cfg.scope = scope;
    SaveSettings();
    WatchEndpoints();
    RefreshState();
}

// Let the user pick their own .wav for the mute or unmute sound, then play it once.
static void ChooseSound(bool muted) {
    std::wstring& path = muted ? g_cfg.muteSound : g_cfg.unmuteSound;
    wchar_t file[MAX_PATH] = {};
    lstrcpynW(file, path.c_str(), MAX_PATH);
    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"WAV sound (*.wav)\0*.wav\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = muted ? L"Choose the mute sound" : L"Choose the unmute sound";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;
    std::vector<BYTE> wav = LoadWav(file);
    if (wav.empty()) {
        Notify(L"Can't use that file", L"Pick a .wav file under 8 MB.");
        return;
    }
    PlaySoundW(nullptr, nullptr, 0);  // stop anything still playing from the buffer being replaced
    (muted ? g_userMute : g_userUnmute) = std::move(wav);
    path = file;
    SaveSettings();
    PlaySoundFor(muted);
}

// Switch theme, dropping any custom files so the choice is actually heard, then preview it.
static void SetTheme(DWORD theme) {
    LoadTheme(theme);
    g_userMute.clear();
    g_userUnmute.clear();
    g_cfg.muteSound.clear();
    g_cfg.unmuteSound.clear();
    g_cfg.soundTheme = theme;
    SaveSettings();
    PlaySoundFor(g_muted);
}

// ---------------------------------------------------------------------------
// Dark menus: Win32 popup menus ignore the system theme unless the app opts in
// through uxtheme's unnamed exports (stable since Windows 10 1809, build 17763).
// ---------------------------------------------------------------------------

using FnSetPreferredAppMode = int(WINAPI*)(int);              // #135 (1809: AllowDarkModeForApp(BOOL))
using FnAllowDarkModeForWindow = bool(WINAPI*)(HWND, bool);   // #133
using FnThemeVoid = void(WINAPI*)();                          // #104 RefreshImmersiveColorPolicyState, #136 FlushMenuThemes
static FnThemeVoid g_refreshColorPolicy, g_flushMenuThemes;

static bool BuildAtLeast(DWORD build) {
    using FnRtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto rtl = (FnRtlGetVersion)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v = {sizeof(v)};
    return rtl && rtl(&v) == 0 && v.dwBuildNumber >= build;
}

static void ApplyMenuTheme() {
    if (g_refreshColorPolicy) g_refreshColorPolicy();
    if (g_flushMenuThemes) g_flushMenuThemes();
}

static void InitDarkMenus(HWND owner) {
    if (!BuildAtLeast(17763)) return;  // ordinals mean other things on older builds
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    auto setMode = (FnSetPreferredAppMode)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto allowWindow = (FnAllowDarkModeForWindow)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    g_refreshColorPolicy = (FnThemeVoid)GetProcAddress(ux, MAKEINTRESOURCEA(104));
    g_flushMenuThemes = (FnThemeVoid)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(1);  // AllowDark: follow the system light/dark setting
    if (allowWindow) allowWindow(owner, true);
    ApplyMenuTheme();
}

static void ShowMenu(int x, int y) {
    if (RefreshState()) g_muteOwned = false;  // changed outside Hush before its notification arrived
    HMENU m = CreatePopupMenu();
    HMENU mode = CreatePopupMenu();
    HMENU scope = CreatePopupMenu();
    HMENU ovl = CreatePopupMenu();
    auto chk = [](bool b) { return b ? MF_CHECKED : MF_UNCHECKED; };
    // Pick-one groups get a dot instead of a checkmark.
    auto radio = [&](HMENU menu, bool on, UINT id, const wchar_t* text, UINT extra = 0) {
        AppendMenuW(menu, MF_STRING | MFT_RADIOCHECK | chk(on) | extra, id, text);
    };

    AppendMenuW(m, MF_STRING | MF_GRAYED, 0,
                g_noMic ? L"Microphone: none found" : g_muted ? L"Microphone: MUTED" : L"Microphone: LIVE");
    AppendMenuW(m, MF_STRING | (g_noMic ? MF_GRAYED : 0), ID_TOGGLE, g_muted ? L"Unmute" : L"Mute");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    std::wstring hk = L"Hotkey: " + BindName();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, hk.c_str());
    AppendMenuW(m, MF_STRING, ID_REBIND, L"Change hotkey…");

    radio(mode, g_cfg.mode == MODE_TOGGLE, ID_MODE_TOGGLE, L"Toggle");
    radio(mode, g_cfg.mode == MODE_PTT, ID_MODE_PTT, L"Push to talk");
    radio(mode, g_cfg.mode == MODE_PTM, ID_MODE_PTM, L"Push to mute");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)mode, L"Mode");

    radio(scope, g_cfg.scope == SCOPE_ALL, ID_SCOPE_ALL, L"All microphones");
    radio(scope, g_cfg.scope == SCOPE_DEFAULT, ID_SCOPE_DEFAULT, L"Default microphone only");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)scope, L"Devices");

    // One indicator submenu, three groups: when it shows, what it looks like, where it sits.
    radio(ovl, g_cfg.overlay == OVL_OFF, ID_OVL_OFF, L"Off");
    radio(ovl, g_cfg.overlay == OVL_FLASH, ID_OVL_FLASH, L"Flash on change");
    radio(ovl, g_cfg.overlay == OVL_MUTED, ID_OVL_MUTED, L"Only while muted");
    radio(ovl, g_cfg.overlay == OVL_LIVE, ID_OVL_LIVE, L"Only while live");
    radio(ovl, g_cfg.overlay == OVL_ALWAYS, ID_OVL_ALWAYS, L"Always");
    AppendMenuW(ovl, MF_SEPARATOR, 0, nullptr);
    for (auto& st : kStyles)
        radio(ovl, g_cfg.overlayStyle == st.id, ID_STYLE_FIRST + st.id, st.name);
    AppendMenuW(ovl, MF_SEPARATOR, 0, nullptr);
    static const wchar_t* posNames[] = {L"Top left", L"Top center", L"Top right",
                                        L"Bottom left", L"Bottom center", L"Bottom right"};
    const DWORD effPos = EffectivePos(g_cfg.overlayStyle, g_cfg.overlayPos);
    for (DWORD i = POS_TL; i <= POS_BR; i++) {
        UINT allowed = EffectivePos(g_cfg.overlayStyle, i) == i ? 0 : MF_GRAYED;
        radio(ovl, effPos == i, ID_POS_FIRST + i, posNames[i], allowed);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)ovl, L"Indicator");

    HMENU snd = CreatePopupMenu();
    AppendMenuW(snd, MF_STRING | chk(g_cfg.sounds != 0), ID_SOUNDS, L"Play sounds");
    AppendMenuW(snd, MF_SEPARATOR, 0, nullptr);
    for (auto& t : kThemes)
        radio(snd, g_cfg.soundTheme == t.id, ID_THEME_FIRST + t.id, t.name);
    AppendMenuW(snd, MF_SEPARATOR, 0, nullptr);
    const std::wstring muteName = L"Mute: " + SoundName(true), unmuteName = L"Unmute: " + SoundName(false);
    AppendMenuW(snd, MF_STRING | MF_GRAYED, 0, muteName.c_str());
    AppendMenuW(snd, MF_STRING, ID_SOUND_MUTE, L"Choose mute sound…");
    AppendMenuW(snd, MF_STRING | MF_GRAYED, 0, unmuteName.c_str());
    AppendMenuW(snd, MF_STRING, ID_SOUND_UNMUTE, L"Choose unmute sound…");

    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)snd, L"Sounds");
    AppendMenuW(m, MF_STRING | chk(AutostartEnabled()), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");

    SetForegroundWindow(g_hwnd);
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);  // destroys submenus too

    if (cmd >= ID_POS_FIRST && cmd <= ID_POS_FIRST + POS_BR) {
        g_cfg.overlayPos = cmd - ID_POS_FIRST;
        SaveSettings();
        FlashOverlay(g_muted, 1500);  // preview the new position
        return;
    }
    if (cmd >= ID_STYLE_FIRST && cmd < ID_EXIT && IsStyle(cmd - ID_STYLE_FIRST)) {
        g_cfg.overlayStyle = cmd - ID_STYLE_FIRST;
        SaveSettings();
        FlashOverlay(g_muted, 1500);  // preview the new style
        return;
    }
    if (cmd >= ID_THEME_FIRST && cmd < ID_EXIT && IsTheme(cmd - ID_THEME_FIRST)) {
        SetTheme(cmd - ID_THEME_FIRST);
        return;
    }
    switch (cmd) {
        case ID_TOGGLE: ToggleMute(); break;
        case ID_MODE_TOGGLE: SetMode(MODE_TOGGLE); break;
        case ID_MODE_PTT: SetMode(MODE_PTT); break;
        case ID_MODE_PTM: SetMode(MODE_PTM); break;
        case ID_REBIND: StartCapture(); break;
        case ID_SCOPE_ALL: SetScope(SCOPE_ALL); break;
        case ID_SCOPE_DEFAULT: SetScope(SCOPE_DEFAULT); break;
        case ID_OVL_OFF: case ID_OVL_MUTED: case ID_OVL_ALWAYS: case ID_OVL_FLASH: case ID_OVL_LIVE:
            g_cfg.overlay = cmd - ID_OVL_OFF;
            SaveSettings();
            HideOverlay();
            UpdateOverlay(g_cfg.overlay == OVL_FLASH);  // flash once so the choice is visible
            break;
        case ID_SOUNDS: g_cfg.sounds = !g_cfg.sounds; SaveSettings(); break;
        case ID_SOUND_MUTE: ChooseSound(true); break;
        case ID_SOUND_UNMUTE: ChooseSound(false); break;
        case ID_AUTOSTART: SetAutostart(!AutostartEnabled()); break;
        case ID_EXIT: DestroyWindow(g_hwnd); break;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INPUT:
            // This window is never foreground, so input normally arrives as RIM_INPUTSINK.
            OnRawInput((HRAWINPUT)lp);
            if (GET_RAWINPUT_CODE_WPARAM(wp) == RIM_INPUT) return DefWindowProcW(hwnd, msg, wp, lp);  // cleanup
            return 0;
        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT: ToggleMute(); break;
                case WM_CONTEXTMENU: ShowMenu((short)LOWORD(wp), (short)HIWORD(wp)); break;
            }
            return 0;
        case WM_APP_REFRESH:
            g_refreshPending = false;
            if (RefreshState()) g_muteOwned = false;  // someone else changed it; it's theirs now
            return 0;
        case WM_APP_DEVICES: {
            g_devicesPending = false;
            // A newly connected mic shouldn't be live while Hush says "muted", or while
            // push-to-talk isn't held.
            bool want = g_cfg.mode == MODE_PTT ? !g_pressed : g_cfg.mode == MODE_PTM ? g_pressed : g_muted;
            WatchEndpoints();
            if (want) {
                WriteMuted(true);
                g_muteOwned = g_muteOwned || g_cfg.mode != MODE_TOGGLE;
            }
            RefreshState();
            return 0;
        }
        case WM_TIMER:
            switch (wp) {
                case TIMER_OVL_HOLD:
                    KillTimer(hwnd, TIMER_OVL_HOLD);
                    // After a flash, fall back to the steady badge if settings call for one.
                    if (WantsPersistentBadge()) ShowOverlay(g_muted);
                    else StartFade(0, kFadeOutMs);
                    break;
                case TIMER_CAPTURE:
                    if (g_capturing) EndCapture(L"Hotkey unchanged: " + BindName());
                    break;
                case TIMER_OVL_FADE:
                    StepFade();
                    break;
                case TIMER_OVL_ANIM:
                    g_edgeAnim = std::min(1.f, (float)(GetTickCount64() - g_edgeAnimStart) / kEdgeAnimMs);
                    if (g_edgeAnim >= 1) KillTimer(hwnd, TIMER_OVL_ANIM);
                    if (g_overlayVisible) PaintOverlay();
                    break;
                case TIMER_OVL_TOPMOST:
                    if (g_overlayVisible)
                        SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    break;
            }
            return 0;
        case WM_SETTINGCHANGE:
            // Light/dark switch: menus are rebuilt on every open, so just drop the cached theme.
            if (lp && lstrcmpiW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) ApplyMenuTheme();
            [[fallthrough]];
        case WM_DISPLAYCHANGE:  // resolution / work area / DPI change: re-place the badge
            if (g_overlayVisible) PaintOverlay();
            return 0;
        case WM_ENDSESSION:
            // Logoff/shutdown skips WM_DESTROY. If Hush comes back at login it shows the
            // mute itself; otherwise lift it so the mic isn't silently dead next session.
            if (wp && !AutostartEnabled()) ReleaseMute();
            return 0;
        case WM_DESTROY:
            ReleaseMute();
            if (g_devices) g_devices->UnregisterEndpointNotificationCallback(&g_deviceWatcher);
            UnwatchEndpoints();
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            DestroyWindow(g_overlay);
            PostQuitMessage(0);
            return 0;
    }
    if (msg == g_msgTaskbarCreated && msg) {  // Explorer restarted
        AddTray();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    CreateMutexW(nullptr, TRUE, L"Local\\Hush.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&g_devices));

    bool firstRun = !LoadSettings();
    if (firstRun) SaveSettings();

    if (wcsstr(GetCommandLineW(), L"--debug")) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        lstrcpyW(wcsrchr(path, L'\\') + 1, L"Hush.log");
        g_log = _wfsopen(path, L"w, ccs=UTF-8", _SH_DENYWR);  // readable while running
        HANDLE tok;
        TOKEN_ELEVATION elev = {};
        DWORD sz;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
            GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &sz);
            CloseHandle(tok);
        }
        Log(L"start: elevated=%lu mode=%lu bind=%lu/%lu scope=%lu", elev.TokenIsElevated, g_cfg.mode,
            g_cfg.bindKind, g_cfg.bindCode, g_cfg.scope);
    }

    LoadTheme(g_cfg.soundTheme);
    g_userMute = LoadWav(g_cfg.muteSound);  // a missing file just falls back to the built-in sound
    g_userUnmute = LoadWav(g_cfg.unmuteSound);
    int iconSize = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    g_iconLive = MakeTrayIcon(false, iconSize);
    g_iconMuted = MakeTrayIcon(true, iconSize);
    g_iconNone = MakeTrayIcon(false, iconSize, true);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HushWnd";
    RegisterClassExW(&wc);
    // Hidden top-level window: receives raw input, tray callbacks and timers.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, kAppName, WS_POPUP, 0, 0, 0, 0,
                             nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;
    InitDarkMenus(g_hwnd);
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW oc = {sizeof(oc)};
    oc.lpfnWndProc = DefWindowProcW;
    oc.hInstance = inst;
    oc.lpszClassName = L"HushOverlay";
    RegisterClassExW(&oc);
    g_overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                oc.lpszClassName, L"Hush indicator", WS_POPUP, 0, 0, 0, 0,
                                nullptr, nullptr, inst, nullptr);
    // Keep the badge out of screenshots, OBS and Discord screen share.
    SetWindowDisplayAffinity(g_overlay, WDA_EXCLUDEFROMCAPTURE);

    WatchEndpoints();
    if (g_devices) g_devices->RegisterEndpointNotificationCallback(&g_deviceWatcher);

    ReadState();
    AddTray();
    RegisterForBinding();
    if (g_cfg.mode == MODE_PTT) SetMute(true, false);  // quietly: no chirp at every login
    else UpdateOverlay(false);
    if (firstRun)
        Notify(L"Hush is running", BindName() + L" toggles your microphone. Right-click the tray icon for options.");

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    g_watched.clear();
    g_devices.Reset();
    CoUninitialize();
    return 0;
}
