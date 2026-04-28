#define _WIN32_IE 0x0600
#define WINVER 0x0501
#define _WIN32_WINNT 0x0501

#include <windows.h>
#include <shellapi.h>
#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEVICE_IP   "192.168.1.53"
#define DEVICE_PORT 8888
#define CONTROL_URL "/Control/oap/RenderingControl"
#define VOL_MIN     0
#define VOL_MAX     100

#define WM_TRAY_ICON   (WM_USER + 1)
#define IDM_EXIT       1001
#define IDM_GET_VOL    1002
#define HOTKEY_VOL_DN  1
#define HOTKEY_VOL_UP  2
#define TIMER_REFRESH  1
#define REFRESH_MS     5000

static HWND g_hwnd = NULL;
static NOTIFYICONDATAA g_nid = {0};
static int g_volume = -1;
static BOOL g_running = TRUE;

/* --- FIX #1: TaskbarCreated message --- */
static UINT WM_TASKBARCREATED;

/* --- FIX #2: persistent icon handle --- */
static HICON g_hIcon = NULL;

/* ───────────────── SOAP ───────────────── */

static BOOL soap_request(const char *action, const char *body_xml,
                         char *resp_buf, DWORD resp_size)
{
    HINTERNET hInet = NULL, hConn = NULL, hReq = NULL;
    BOOL ok = FALSE;

    hInet = InternetOpenA("ReceiverTray/1.0",
                          INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInet) goto done;

    hConn = InternetConnectA(hInet, DEVICE_IP, DEVICE_PORT,
                             NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConn) goto done;

    hReq = HttpOpenRequestA(hConn, "POST", CONTROL_URL,
                            "HTTP/1.1", NULL, NULL,
                            INTERNET_FLAG_NO_CACHE_WRITE |
                            INTERNET_FLAG_RELOAD, 0);
    if (!hReq) goto done;

    char headers[512];
    snprintf(headers, sizeof(headers),
             "Content-Type: text/xml; charset=utf-8\r\n"
             "SOAPAction: \"%s\"\r\n",
             action);

    DWORD body_len = (DWORD)strlen(body_xml);

    if (!HttpSendRequestA(hReq, headers, (DWORD)strlen(headers),
                          (LPVOID)body_xml, body_len))
        goto done;

    if (resp_buf && resp_size > 0) {
        DWORD total = 0, read = 0;
        resp_buf[0] = '\0';

        while (total + 1 < resp_size) {
            if (!InternetReadFile(hReq,
                                  resp_buf + total,
                                  resp_size - total - 1, &read))
                break;
            if (read == 0) break;
            total += read;
        }
        resp_buf[total] = '\0';
    }

    ok = TRUE;

done:
    if (hReq) InternetCloseHandle(hReq);
    if (hConn) InternetCloseHandle(hConn);
    if (hInet) InternetCloseHandle(hInet);
    return ok;
}

/* ───────── volume ───────── */

static int get_volume(void)
{
    const char *action =
        "urn:schemas-upnp-org:service:RenderingControl:2#GetVolume";

    const char *body =
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
        "<s:Body>"
        "<u:GetVolume xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:2\">"
        "<InstanceID>0</InstanceID>"
        "<Channel>Master</Channel>"
        "</u:GetVolume>"
        "</s:Body>"
        "</s:Envelope>";

    char resp[4096] = {0};

    if (!soap_request(action, body, resp, sizeof(resp)))
        return -1;

    char *p = strstr(resp, "<CurrentVolume>");
    if (!p) return -1;

    p += 15;
    char *end = strchr(p, '<');
    if (!end) return -1;

    char buf[16] = {0};
    memcpy(buf, p, end - p);

    return atoi(buf);
}

static BOOL set_volume(int vol)
{
    if (vol < VOL_MIN) vol = VOL_MIN;
    if (vol > VOL_MAX) vol = VOL_MAX;

    const char *action =
        "urn:schemas-upnp-org:service:RenderingControl:2#SetVolume";

    char body[1024];
    snprintf(body, sizeof(body),
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
        "<s:Body>"
        "<u:SetVolume xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:2\">"
        "<InstanceID>0</InstanceID>"
        "<Channel>Master</Channel>"
        "<DesiredVolume>%d</DesiredVolume>"
        "</u:SetVolume>"
        "</s:Body>"
        "</s:Envelope>",
        vol);

    return soap_request(action, body, NULL, 0);
}

/* ───────── tray icon ───────── */

typedef struct {
    int r, g, b;
} RGB;

static RGB lerp(RGB a, RGB b, float t)
{
    RGB c;
    c.r = a.r + (int)((b.r - a.r) * t);
    c.g = a.g + (int)((b.g - a.g) * t);
    c.b = a.b + (int)((b.b - a.b) * t);
    return c;
}

static RGB volume_to_color(int vol)
{
    RGB light_blue = {120, 200, 255};
    RGB blue       = {0, 90, 255};
    RGB green      = {0, 220, 80};
    RGB yellow     = {255, 220, 0};
    RGB orange     = {255, 140, 0};
    RGB red        = {220, 40, 40};

    float t;
    RGB c;

    if (vol < 0)
        return (RGB){80, 80, 80};

    if (vol <= 20) {
        t = vol / 20.0f;
        c = lerp(light_blue, blue, t);
    }
    else if (vol <= 40) {
        t = (vol - 20) / 20.0f;
        c = lerp(blue, green, t);
    }
    else if (vol <= 60) {
        t = (vol - 40) / 20.0f;
        c = lerp(green, yellow, t);
    }
    else if (vol <= 80) {
        t = (vol - 60) / 20.0f;
        c = lerp(yellow, orange, t);
    }
    else {
        t = (vol - 80) / 20.0f;
        c = lerp(orange, red, t);
    }

    return c;
}

static HICON create_icon(int vol)
{
    int sz = 16;

    RGB col = volume_to_color(vol);

    HDC hdc = GetDC(NULL);

    HDC mem = CreateCompatibleDC(hdc);

    HBITMAP color = CreateCompatibleBitmap(hdc, sz, sz);
    HBITMAP mask  = CreateBitmap(sz, sz, 1, 1, NULL); // ✔ ВАЖНО

    HBITMAP old = (HBITMAP)SelectObject(mem, color);

    RECT rc = {0,0,sz,sz};

    HBRUSH br = CreateSolidBrush(RGB(col.r, col.g, col.b));
    FillRect(mem, &rc, br);
    DeleteObject(br);

    char txt[8];
    if (vol < 0) strcpy(txt, "?");
    else sprintf(txt, "%d", vol);

    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255,255,255));
    DrawTextA(mem, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(mem, old);

    ICONINFO ii;
    ZeroMemory(&ii, sizeof(ii));

    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;   // ✔ ВАЖНО (исправляет прозрачность)

    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(color);
    DeleteObject(mask);
    DeleteDC(mem);
    ReleaseDC(NULL, hdc);

    return icon;
}

/* --- FIX #2: persistent icon --- */
static void update_tray_icon(void)
{
    if (g_hIcon) DestroyIcon(g_hIcon);
    g_hIcon = create_icon(g_volume);

    g_nid.hIcon = g_hIcon;

    if (g_volume < 0)
        strcpy(g_nid.szTip, "Receiver: ?");
    else
        sprintf(g_nid.szTip, "Receiver: %d", g_volume);

    Shell_NotifyIconA(NIM_MODIFY, &g_nid);
}

/* --- FIX #3: retry + version --- */
static void add_tray_icon(void)
{
    int tries = 5;

    while (tries--) {
        if (Shell_NotifyIconA(NIM_ADD, &g_nid))
            break;
        Sleep(200);
    }

    g_nid.uVersion = NOTIFYICON_VERSION;
    Shell_NotifyIconA(NIM_SETVERSION, &g_nid);
}

static void remove_tray_icon(void)
{
    Shell_NotifyIconA(NIM_DELETE, &g_nid);
}

/* ───────── volume control ───────── */

static void adjust(int d)
{
    if (g_volume < 0)
        g_volume = get_volume();

    int v = g_volume + d;

    if (set_volume(v)) {
        g_volume = v;
        update_tray_icon();
    }
}

/* ───────── window proc ───────── */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                WPARAM wp, LPARAM lp)
{
    if (msg == WM_TASKBARCREATED) {
        add_tray_icon();
        update_tray_icon();
        return 0;
    }

    switch (msg)
    {
    case WM_CREATE:
        RegisterHotKey(hwnd, HOTKEY_VOL_DN, 0, 0x7C);
        RegisterHotKey(hwnd, HOTKEY_VOL_UP, 0, 0x7D);

        SetTimer(hwnd, TIMER_REFRESH, REFRESH_MS, NULL);

        g_volume = get_volume();

        memset(&g_nid, 0, sizeof(g_nid));
        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = hwnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAY_ICON;

        add_tray_icon();
        update_tray_icon();
        return 0;

    case WM_HOTKEY:
        if (wp == HOTKEY_VOL_DN) adjust(-1);
        if (wp == HOTKEY_VOL_UP) adjust(+1);
        return 0;

    case WM_TIMER:
        g_volume = get_volume();
        update_tray_icon();
        return 0;

    case WM_TRAY_ICON:
        if (LOWORD(lp) == WM_RBUTTONUP)
        {
            POINT pt;
            GetCursorPos(&pt);

            HMENU menu = CreatePopupMenu();
            AppendMenuA(menu, MF_STRING, IDM_GET_VOL, "Refresh");
            AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuA(menu, MF_STRING, IDM_EXIT, "Exit");

            SetForegroundWindow(hwnd);

            TrackPopupMenu(menu,
                TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                pt.x, pt.y, 0, hwnd, NULL);

            DestroyMenu(menu);

            PostMessage(hwnd, WM_NULL, 0, 0);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;

        case IDM_GET_VOL:
            g_volume = get_volume();
            update_tray_icon();
            return 0;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REFRESH);
        UnregisterHotKey(hwnd, HOTKEY_VOL_DN);
        UnregisterHotKey(hwnd, HOTKEY_VOL_UP);

        remove_tray_icon();

        if (g_hIcon)
            DestroyIcon(g_hIcon);

        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ───────── main ───────── */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev,
                   LPSTR lpCmd, int nShow)
{
    WM_TASKBARCREATED = RegisterWindowMessageA("TaskbarCreated");

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "TrayApp";

    RegisterClassA(&wc);

    g_hwnd = CreateWindowA("TrayApp", "", 0,
                           0,0,0,0,
                           NULL,NULL,hInst,NULL);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return 0;
}
