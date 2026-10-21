// =============================================================================
//  Nintendall setup / uninstall stub
//
//  Compiled twice from the same source:
//      setup.exe      (no -DUNINSTALLER)  -> installer, carries the payload
//      Uninstall.exe  (-DUNINSTALLER)    -> uninstaller, installed on disk
//
//  The installer deliberately avoids NSIS / Inno Setup / any other packaging
//  tool: the payload (the app plus its assets) is appended to the finished stub
//  and the stub unpacks itself on first run.  That is what lets the whole
//  product stay "download one exe" with nothing else required on the target
//  machine.
//
//  Layout of a packaged installer:
//
//      [ stub executable bytes            ]
//      [ footer: magic, count, entries    ]   <- 8-byte offset at EOF points here
//      [ blob: raw file contents          ]
//
//  Every entry records where its bytes live inside the blob, so extraction is a
//  straight copy from our own file to disk: no decompression, no temp archive.
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Build configuration (wide literals, because everything here is Win32 W)
// -----------------------------------------------------------------------------

#define WSTR_(x) L##x

#ifndef PRODUCT_NAME
#define PRODUCT_NAME      WSTR_("Nintendall")
#endif
#ifndef PRODUCT_VERSION
#define PRODUCT_VERSION   WSTR_("3.1.4")
#endif
#ifndef PRODUCT_PUBLISHER
#define PRODUCT_PUBLISHER WSTR_("Nintendall")
#endif
#ifndef PRODUCT_EXE
#define PRODUCT_EXE       WSTR_("nintendall.exe")
#endif

static const wchar_t* const kAppKey =
    L"Software\\" PRODUCT_NAME L"\\Setup";
static const wchar_t* const kUninstKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" PRODUCT_NAME;

// Path of the running stub; the installer reads its payload back out of this.
static wchar_t g_src_path[MAX_PATH];

// -----------------------------------------------------------------------------
// Payload format
// -----------------------------------------------------------------------------

#pragma pack(push, 1)
struct PkgEntry {
    uint64_t offset;      // byte offset inside the blob
    uint64_t size;        // byte length
    uint32_t path_bytes;  // UTF-8 length of the path that follows the table
    uint8_t  is_dir;
    uint8_t  pad[3];
};

struct PkgFooter {
    char     magic[8];    // "NTDLPKG"
    uint32_t version;
    uint32_t count;
    uint64_t blob_offset; // offset of the blob, relative to the package start
};
#pragma pack(pop)

// The payload lives in an RCDATA resource rather than being appended to the
// executable.  That matters: Authenticode signing treats everything after the
// last PE section as an overlay and relocates it, so a signed installer would
// come out with its payload moved and its offsets pointing at nothing. Keeping
// the payload inside the image means signing leaves it exactly where it is.
#define PKG_RESOURCE_ID MAKEINTRESOURCEA(1)
#define PKG_RESOURCE_TYPE MAKEINTRESOURCEA(10)   // RT_RCDATA, in ANSI form

static const BYTE* g_pkg_data = nullptr;   // the package, already in memory
static size_t     g_pkg_size = 0;

static void MapPackage() {
    if (g_pkg_data) return;
    HRSRC r = FindResourceA(nullptr, PKG_RESOURCE_ID, PKG_RESOURCE_TYPE);
    if (!r) return;
    HGLOBAL h = LoadResource(nullptr, r);
    if (!h) return;
    g_pkg_size = SizeofResource(nullptr, r);
    g_pkg_data = (const BYTE*)LockResource(h);
}

// Decoded relative paths, index-matched with the entry table.
static std::vector<std::wstring> g_pkg_paths;

// Parse the embedded package.  Returns false for a stub with no payload, which
// is exactly what Uninstall.exe is.
static bool LoadPackage(std::vector<PkgEntry>* entries, uint64_t* blob_at) {
    MapPackage();
    if (!g_pkg_data || g_pkg_size < sizeof(PkgFooter)) return false;

    PkgFooter f;
    memcpy(&f, g_pkg_data, sizeof(f));
    if (memcmp(f.magic, "NTDLPKG", 8) != 0) return false;

    const uint64_t table_end =
        (uint64_t)sizeof(PkgFooter) + (uint64_t)f.count * sizeof(PkgEntry);
    if (f.count == 0 || table_end > g_pkg_size) return false;

    std::vector<PkgEntry> raw(f.count);
    memcpy(raw.data(), g_pkg_data + sizeof(PkgFooter), f.count * sizeof(PkgEntry));

    // The path strings follow the whole record table, in the same order.
    uint64_t at = table_end;
    entries->assign(raw.begin(), raw.end());
    g_pkg_paths.clear();
    g_pkg_paths.reserve(f.count);

    for (uint32_t i = 0; i < f.count; ++i) {
        if (at + raw[i].path_bytes > g_pkg_size) return false;
        const char* s = (const char*)(g_pkg_data + at);
        at += raw[i].path_bytes;

        int need = MultiByteToWideChar(CP_UTF8, 0, s, raw[i].path_bytes, nullptr, 0);
        std::wstring w((need > 0) ? need : 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s, raw[i].path_bytes, &w[0], need);
        g_pkg_paths.push_back(w);
    }

    // Every record must point inside the blob.
    if (f.blob_offset > g_pkg_size) return false;
    for (size_t i = 0; i < entries->size(); ++i) {
        if ((*entries)[i].is_dir) continue;
        if (f.blob_offset + (*entries)[i].offset + (*entries)[i].size > g_pkg_size)
            return false;
    }

    *blob_at = f.blob_offset;
    return true;
}

// -----------------------------------------------------------------------------
// Small helpers
// -----------------------------------------------------------------------------

static std::wstring GetEnv(const wchar_t* name) {
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(name, buf, 32768);
    if (!n || n >= 32768) return L"";
    return std::wstring(buf, n);
}

static std::wstring DefaultInstallDir() {
    std::wstring local = GetEnv(L"LOCALAPPDATA");
    if (local.empty()) local = GetEnv(L"USERPROFILE");
    if (local.empty()) local = L"C:\\";
    return local + L"\\Programs\\" PRODUCT_NAME;
}

static std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    std::wstring r = a;
    if (r.back() != L'\\') r += L'\\';
    r += b;
    return r;
}

static bool EnsureDirs(const std::wstring& dir) {
    if (dir.empty()) return false;
    size_t i = 0;
    if (dir.size() > 1 && dir[1] == L':') i = 2;
    while (i <= dir.size()) {
        size_t j = dir.find(L'\\', i);
        if (j == std::wstring::npos) j = dir.size();
        std::wstring part = dir.substr(0, j);
        if (part.size() > (dir[1] == L':' ? 2u : 0u)) {
            if (!CreateDirectoryW(part.c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                return false;
            }
        }
        if (j == dir.size()) break;
        i = j + 1;
    }
    return true;
}

// Turn "a/b/c.txt" into "a\b\c.txt" and refuse anything that could climb out
// of the destination directory.
static bool SafeRelPath(const std::wstring& rel, std::wstring* out) {
    if (rel.empty()) return false;
    std::wstring s = rel;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '/') s[i] = L'\\';
        else if (s[i] == ':' || s[i] == L'*' || s[i] == L'?' || s[i] == L'"') return false;
    }
    if (s.compare(0, 3, L"..\\") == 0 || s == L"..") return false;
    *out = s;
    return true;
}

static void MakeShortcut(const wchar_t* link_path, const wchar_t* target,
                         const wchar_t* desc) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&sl))) {
        return;
    }
    sl->SetPath(target);
    sl->SetWorkingDirectory(L"");
    sl->SetDescription(desc);
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        pf->Save(link_path, TRUE);
        pf->Release();
    }
    sl->Release();
}

static void RegSetStr(const wchar_t* sub, const wchar_t* name, const std::wstring& v) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, nullptr, 0, KEY_WRITE,
                        nullptr, &k, nullptr) != ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(),
                   (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

// -----------------------------------------------------------------------------
// Uninstaller
// -----------------------------------------------------------------------------

static std::wstring ReadInstallLocation() {
    HKEY k = nullptr;
    std::wstring dir;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD n = 0, type = 0;
        if (RegQueryValueExW(k, L"InstallLocation", nullptr, &type, nullptr, &n) == ERROR_SUCCESS &&
            type == REG_SZ && n > 0 && n < 65536 * sizeof(wchar_t)) {
            std::wstring buf(n / sizeof(wchar_t), L'\0');
            if (RegQueryValueExW(k, L"InstallLocation", nullptr, &type,
                                 (LPBYTE)&buf[0], &n) == ERROR_SUCCESS) {
                while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
                dir = buf;
            }
        }
        RegCloseKey(k);
    }
    return dir;
}

// Recursive delete.  Done by hand rather than through SHFileOperation so the
// behaviour is identical on every SDK, and so the uninstaller can be sure the
// install folder itself is gone.
static void DeleteRec(const std::wstring& path) {
    WIN32_FIND_DATAW fd;
    std::wstring pattern = JoinPath(path, L"*");
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const wchar_t* name = fd.cFileName;
            if (!wcscmp(name, L".") || !wcscmp(name, L"..")) continue;
            std::wstring child = JoinPath(path, name);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                DeleteRec(child);
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(path.c_str());
}

#ifdef UNINSTALLER
static int RunUninstall(HWND hwnd, bool silent) {
    std::wstring dir = ReadInstallLocation();
    if (dir.empty()) {
        if (!silent)
            MessageBoxW(hwnd, PRODUCT_NAME L" does not appear to be installed.",
                        PRODUCT_NAME, MB_OK | MB_ICONINFORMATION);
        return 1;
    }

    wchar_t buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, buf))) {
        std::wstring menuDir = JoinPath(buf, PRODUCT_NAME);
        DeleteFileW(JoinPath(menuDir, PRODUCT_NAME L".lnk").c_str());
        RemoveDirectoryW(menuDir.c_str());
    }
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, buf))) {
        DeleteFileW(JoinPath(buf, PRODUCT_NAME L".lnk").c_str());
    }

    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, kAppKey);

    // Uninstall.exe is running, so it cannot delete itself straight away; it is
    // handed to Windows to remove once the machine next reboots.
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);

    DeleteRec(dir);

    {
        std::wstring selfPath = self;
        MoveFileExW(selfPath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }

    if (!silent)
        MessageBoxW(hwnd,
                     PRODUCT_NAME L" has been removed.\n\n"
                     L"If the folder is still present, it will disappear after a restart.",
                     PRODUCT_NAME, MB_OK | MB_ICONINFORMATION);
    return 0;
}
#endif  // UNINSTALLER

// -----------------------------------------------------------------------------
// Installer UI
// -----------------------------------------------------------------------------

#define ID_PATH      1001
#define ID_BROWSE    1002
#define ID_DESKTOP   1003
#define ID_LAUNCH    1004
#define ID_INSTALL   1005
#define ID_CANCEL    1006
#define ID_PROGRESS  1007
#define ID_STATUS    1008
#define ID_BANNER    1009
#define ID_LABEL     1100

static HWND g_path, g_browse, g_desktop, g_launch, g_install, g_cancel;
static HWND g_progress, g_status, g_label;
static std::wstring g_dir;
static bool g_done = false;
static bool g_desktop_link = true;
static bool g_launch_after  = true;

static void SetStatusText(const wchar_t* s) { SetWindowTextW(g_status, s); }

static void Layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right;
    const int pad = 22;

    MoveWindow(GetDlgItem(hwnd, ID_BANNER), 0, 0, W, 92, TRUE);
    int y = 92 + 20;

    MoveWindow(g_label, pad, y, W - pad * 2, 18, TRUE);
    y += 24;

    const int btn_w = 96;
    int edit_w = W - pad * 2 - btn_w - 8;
    MoveWindow(g_path,   pad, y, edit_w, 26, TRUE);
    MoveWindow(g_browse, pad + edit_w + 8, y, btn_w, 26, TRUE);
    y += 38;

    MoveWindow(g_desktop, pad, y, W - pad * 2, 22, TRUE); y += 26;
    MoveWindow(g_launch,  pad, y, W - pad * 2, 22, TRUE); y += 34;

    MoveWindow(g_progress, pad, y, W - pad * 2, 16, TRUE); y += 28;
    MoveWindow(g_status,   pad, y, W - pad * 2 - 200, 26, TRUE); y += 36;

    const int bw = 104;
    MoveWindow(g_install, W - pad - bw * 2 - 10, y, bw, 30, TRUE);
    MoveWindow(g_cancel,  W - pad - bw,         y, bw, 30, TRUE);
}

// Owner-drawn header, so the installer matches the app's dark shell.
static LRESULT CALLBACK BannerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)w; (void)l;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);

        const COLORREF lo = RGB(24, 22, 58), hi = RGB(92, 48, 148);
        HBRUSH b = CreateSolidBrush(lo);
        FillRect(dc, &rc, b);
        DeleteObject(b);

        // Vertical gradient, one scanline at a time.
        int rows = rc.bottom;
        for (int y = 0; y < rows; ++y) {
            double t = rows > 1 ? (double)y / (double)(rows - 1) : 0.0;
            int r = (int)(GetRValue(lo)   + (GetRValue(hi)   - GetRValue(lo))   * t);
            int gg= (int)(GetGValue(lo)   + (GetGValue(hi)   - GetGValue(lo))   * t);
            int bb= (int)(GetBValue(lo)   + (GetBValue(hi)   - GetBValue(lo))   * t);
            SetPixelV(dc, 0, y, RGB(r, gg, bb));
            for (int x = 1; x < rc.right; ++x)
                SetPixelV(dc, x, y, RGB(r, gg, bb));
        }

        RECT line = { 0, rc.bottom - 4, rc.right, rc.bottom };
        b = CreateSolidBrush(RGB(150, 96, 230));
        FillRect(dc, &line, b);
        DeleteObject(b);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));

        LOGFONTW lf;
        ZeroMemory(&lf, sizeof(lf));
        lf.lfHeight = -32;
        lf.lfWeight = FW_BOLD;
        wcscpy_s(lf.lfFaceName, L"Segoe UI Semibold");
        HFONT f = CreateFontIndirectW(&lf);
        HGDIOBJ old = SelectObject(dc, f);

        wchar_t title[128];
        swprintf_s(title, L"%s  %s", PRODUCT_NAME, PRODUCT_VERSION);
        RECT tr = { 22, 14, rc.right - 22, rc.bottom - 6 };
        DrawTextW(dc, title, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        SelectObject(dc, old);
        DeleteObject(f);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void BrowseFolder(HWND hwnd) {
    BROWSEINFOW bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = hwnd;
    bi.lpszTitle = L"Choose the folder to install " PRODUCT_NAME L" into";

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;

    wchar_t tmp[MAX_PATH];
    if (SUCCEEDED(SHGetPathFromIDListW(pidl, tmp))) {
        std::wstring chosen = tmp;
        while (chosen.size() > 3 && chosen.back() == L'\\') chosen.pop_back();
        if (!chosen.empty()) SetWindowTextW(g_path, chosen.c_str());
    }
    CoTaskMemFree(pidl);
}

static void SetControlsEnabled(bool on) {
    EnableWindow(g_path, on);
    EnableWindow(g_browse, on);
    EnableWindow(g_desktop, on);
    EnableWindow(g_launch, on);
    EnableWindow(g_install, on);
}

// Copy every packaged file into the destination, reporting progress.
static int DoInstall(HWND hwnd) {
    std::vector<PkgEntry> entries;
    uint64_t blob_at = 0;
    if (!LoadPackage(&entries, &blob_at) || entries.empty()) {
        MessageBoxW(hwnd,
                     L"This installer is damaged or incomplete.\n"
                     L"Please download it again.",
                     PRODUCT_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }

    // The payload is already mapped in memory by LoadPackage, so extraction is
    // a straight copy out of g_pkg_data.
    const BYTE* blob = g_pkg_data + blob_at;

    uint64_t total = 0;
    for (size_t i = 0; i < entries.size(); ++i)
        if (!entries[i].is_dir) total += entries[i].size;
    uint64_t done = 0;
    const DWORD kChunk = 64 * 1024;
    std::vector<BYTE> buf(kChunk);
    (void)kChunk;   // writes are whole-file from memory now

    SetControlsEnabled(false);
    ShowWindow(g_install, SW_HIDE);
    SetStatusText(L"Installing...");

    int rc = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        std::wstring rel;
        if (!SafeRelPath(g_pkg_paths[i], &rel)) { rc = -2; break; }

        std::wstring full = JoinPath(g_dir, rel);
        std::wstring parent = full.substr(0, full.find_last_of(L'\\'));
        if (!EnsureDirs(parent)) { rc = -3; break; }

        if (entries[i].is_dir) {
            if (!EnsureDirs(full)) { rc = -3; break; }
            continue;
        }

        // Already the right size - leave it alone so reruns are quick and
        // user modifications are not clobbered.
        if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES) {
            HANDLE probe = CreateFileW(full.c_str(), 0, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (probe != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER have;
                if (GetFileSizeEx(probe, &have) &&
                    (uint64_t)have.QuadPart == entries[i].size) {
                    CloseHandle(probe);
                    done += entries[i].size;
                    SendMessageW(g_progress, PBM_SETPOS,
                                 (WPARAM)(done * 1000 / (total ? total : 1)), 0);
                    continue;
                }
                CloseHandle(probe);
            }
            SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(full.c_str());
        }

        HANDLE dst = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dst == INVALID_HANDLE_VALUE) { rc = -4; break; }

        const BYTE* bytes = blob + entries[i].offset;
        uint64_t left = entries[i].size;
        bool bad = false;
        while (left > 0) {
            DWORD want = (DWORD)((left < kChunk) ? left : kChunk);
            DWORD put = 0;
            if (!WriteFile(dst, bytes, want, &put, nullptr) || put != want) { bad = true; break; }
            bytes += want;
            left -= want;
            done += want;
            SendMessageW(g_progress, PBM_SETPOS,
                         (WPARAM)(done * 1000 / (total ? total : 1)), 0);
        }
        CloseHandle(dst);
        if (bad) { rc = -5; break; }
    }

    if (rc != 0) {
        SetControlsEnabled(true);
        ShowWindow(g_install, SW_SHOW);
        const wchar_t* why = L"Installation failed.";
        if (rc == -2)      why = L"The package contains an invalid file path.";
        else if (rc == -3) why = L"Could not create the destination folder.\n"
                                L"Try a different location.";
        else if (rc == -4) why = L"Could not write to the destination folder.\n"
                                L"Try a different location.";
        else               why = L"The installer appears to be damaged.\n"
                                L"Please download it again.";
        MessageBoxW(hwnd, why, PRODUCT_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }

    SendMessageW(g_progress, PBM_SETPOS, 1000, 0);

    // ---- shortcuts -----------------------------------------------------------
    // Read the checkbox state back rather than trusting the cached flags, so
    // what gets done always matches what the window is showing.
    if (g_desktop) g_desktop_link = SendMessageW(g_desktop, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (g_launch)  g_launch_after  = SendMessageW(g_launch,  BM_GETCHECK, 0, 0) == BST_CHECKED;

    wchar_t folder[MAX_PATH];
    std::wstring exe = JoinPath(g_dir, PRODUCT_EXE);

    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, folder))) {
        std::wstring menuDir = JoinPath(folder, PRODUCT_NAME);
        EnsureDirs(menuDir);
        MakeShortcut(JoinPath(menuDir, PRODUCT_NAME L".lnk").c_str(),
                     exe.c_str(), L"Run " PRODUCT_NAME);
    }
    if (g_desktop_link &&
        SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, folder))) {
        MakeShortcut(JoinPath(folder, PRODUCT_NAME L".lnk").c_str(),
                     exe.c_str(), L"Run " PRODUCT_NAME);
    }

    // ---- Add / Remove Programs ----------------------------------------------
    std::wstring uninstaller = JoinPath(g_dir, L"Uninstall.exe");
    RegSetStr(kAppKey,    L"InstallLocation", g_dir);
    RegSetStr(kUninstKey, L"InstallLocation", g_dir);
    RegSetStr(kUninstKey, L"DisplayName",    PRODUCT_NAME);
    RegSetStr(kUninstKey, L"DisplayVersion", PRODUCT_VERSION);
    RegSetStr(kUninstKey, L"Publisher",      PRODUCT_PUBLISHER);
    RegSetStr(kUninstKey, L"DisplayIcon",    exe);
    RegSetStr(kUninstKey, L"UninstallString", uninstaller);
    RegSetStr(kUninstKey, L"QuietUninstallString", uninstaller + L" /S");

    {
        HKEY k = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstKey, 0, nullptr, 0, KEY_WRITE,
                            nullptr, &k, nullptr) == ERROR_SUCCESS) {
            DWORD one = 1;
            RegSetValueExW(k, L"NoModify", 0, REG_DWORD, (BYTE*)&one, sizeof(one));
            RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, (BYTE*)&one, sizeof(one));
            RegCloseKey(k);
        }
    }

    SetStatusText(L"Finished.");
    g_done = true;
    SetWindowTextW(g_install, L"Finish");
    ShowWindow(g_install, SW_SHOW);
    EnableWindow(g_install, TRUE);
    EnableWindow(g_cancel, FALSE);

    if (g_launch_after)
        ShellExecuteW(hwnd, L"open", exe.c_str(), nullptr, g_dir.c_str(), SW_SHOWNORMAL);

    return 0;
}

static LRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE inst = ((CREATESTRUCTW*)l)->hInstance;
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        g_label = CreateWindowExW(0, L"STATIC", L"Install to",
                                  WS_CHILD | WS_VISIBLE | SS_LEFT,
                                  0, 0, 0, 0, hwnd, (HMENU)ID_LABEL, inst, nullptr);
        g_path = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_dir.c_str(),
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                 0, 0, 0, 0, hwnd, (HMENU)ID_PATH, inst, nullptr);
        g_browse = CreateWindowExW(0, L"BUTTON", L"Browse...",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   0, 0, 0, 0, hwnd, (HMENU)ID_BROWSE, inst, nullptr);

        g_desktop = CreateWindowExW(0, L"BUTTON", L"Create a desktop shortcut",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    0, 0, 0, 0, hwnd, (HMENU)ID_DESKTOP, inst, nullptr);
        g_launch  = CreateWindowExW(0, L"BUTTON", L"Start " PRODUCT_NAME L" when finished",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    0, 0, 0, 0, hwnd, (HMENU)ID_LAUNCH, inst, nullptr);
        SendMessageW(g_desktop, BM_SETCHECK, g_desktop_link ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_launch,  BM_SETCHECK, g_launch_after  ? BST_CHECKED : BST_UNCHECKED, 0);

        g_progress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
                                     WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                     0, 0, 0, 0, hwnd, (HMENU)ID_PROGRESS, inst, nullptr);
        SendMessageW(g_progress, PBM_SETRANGE32, 0, 1000);

        g_status = CreateWindowExW(0, L"STATIC", L"Ready.",
                                   WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                   0, 0, 0, 0, hwnd, (HMENU)ID_STATUS, inst, nullptr);

        g_install = CreateWindowExW(0, L"BUTTON", L"Install",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                    0, 0, 0, 0, hwnd, (HMENU)ID_INSTALL, inst, nullptr);
        g_cancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   0, 0, 0, 0, hwnd, (HMENU)ID_CANCEL, inst, nullptr);

        HWND ids[] = { g_label, g_path, g_browse, g_desktop, g_launch, g_status,
                       g_install, g_cancel };
        for (HWND c : ids) SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);

        HWND banner = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                      0, 0, 0, 0, hwnd, (HMENU)ID_BANNER, inst, nullptr);
        SetWindowLongPtrW(banner, GWLP_WNDPROC, (LONG_PTR)BannerProc);

        Layout(hwnd);
        SetFocus(g_path);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_BROWSE:
            BrowseFolder(hwnd);
            return 0;
        case ID_DESKTOP:
            g_desktop_link = SendMessageW(g_desktop, BM_GETCHECK, 0, 0) == BST_CHECKED;
            return 0;
        case ID_LAUNCH:
            g_launch_after = SendMessageW(g_launch, BM_GETCHECK, 0, 0) == BST_CHECKED;
            return 0;
        case ID_CANCEL:
            DestroyWindow(hwnd);
            return 0;
        case ID_INSTALL:
            if (g_done) { DestroyWindow(hwnd); return 0; }
            {
                wchar_t buf[MAX_PATH];
                GetWindowTextW(g_path, buf, MAX_PATH);
                std::wstring d(buf);
                while (d.size() > 3 && d.back() == L'\\') d.pop_back();
                if (d.size() < 3 || d.find(L':') == std::wstring::npos) {
                    MessageBoxW(hwnd,
                                 L"Please enter a valid folder, for example\n"
                                 L"C:\\Users\\You\\AppData\\Local\\Programs\\Nintendall",
                                 PRODUCT_NAME, MB_OK | MB_ICONWARNING);
                    return 0;
                }
                g_dir = d;
                DoInstall(hwnd);
            }
            return 0;
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_SIZE:
        Layout(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

// -----------------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev; (void)show;
    GetModuleFileNameW(nullptr, g_src_path, MAX_PATH);

#ifdef UNINSTALLER
    (void)inst; (void)cmdline;
    return RunUninstall(nullptr, cmdline && wcsstr(cmdline, L"/S") != nullptr);
#else
    std::wstring cmd = cmdline ? cmdline : L"";
    bool silent  = wcsstr(cmd.c_str(), L"/S")     != nullptr;
    bool no_run  = wcsstr(cmd.c_str(), L"/NORUN") != nullptr;
    bool no_icon = wcsstr(cmd.c_str(), L"/NOICON") != nullptr;
    g_launch_after  = !no_run;
    g_desktop_link = !no_icon;

    g_dir = DefaultInstallDir();
    // "/D=<path>" must stop at the first space: taking the whole remainder
    // swallowed any switches that followed and ended up inside the path.
    size_t d = cmd.find(L"/D=");
    if (d != std::wstring::npos) {
        std::wstring v = cmd.substr(d + 3);
        size_t end = v.find_first_of(L" \t");
        if (end != std::wstring::npos) v.resize(end);
        while (!v.empty() && v.back() == L'"') v.pop_back();
        if (!v.empty() && v.front() == L'"') v.erase(v.begin());
        if (v.size() >= 3 && v.find(L':') != std::wstring::npos) {
            while (v.size() > 3 && v.back() == L'\\') v.pop_back();
            g_dir = v;
        }
    }

    if (silent) {
        if (!EnsureDirs(g_dir)) return 1;
        DoInstall(nullptr);
        return g_done ? 0 : 1;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = DialogProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"NintendallSetupDlg";
    RegisterClassExW(&wc);

    wchar_t title[128];
    swprintf_s(title, L"%s %s Setup", PRODUCT_NAME, PRODUCT_VERSION);

    RECT r = { 0, 0, 540, 430 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW,
                                L"NintendallSetupDlg", title,
                                WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_SYSMENU |
                                WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    return g_done ? 0 : 1;
#endif
}