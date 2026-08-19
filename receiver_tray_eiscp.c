/*
Pioneer AVR tray volume controller
Protocol: eISCP over TCP
*/

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <process.h>
#include <stdio.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

#pragma comment(lib, "ws2_32.lib")

#define DEVICE_IP    "192.168.1.53"
#define DEVICE_PORT  60128

#define WM_TRAY_ICON  (WM_USER + 1)
#define WM_SET_STATE  (WM_USER + 2)  /* wparam = RecvState, lparam = vol (-1 if n/a) */
#define WM_SET_SOURCE (WM_USER + 3)  /* wparam = source code, lparam unused */

#define IDM_EXIT      1001
#define IDM_POWER     1003

/* Source selection IDs */
#define IDM_SRC_HDMI5 2010
#define IDM_SRC_HDMI6 2011
#define IDM_SRC_BD    2012
#define IDM_SRC_CBL   2013
#define IDM_SRC_STRM  2014
#define IDM_SRC_TV    2015
#define IDM_SRC_GAME  2016
#define IDM_SRC_CD    2017
#define IDM_SRC_USB   2018
#define IDM_SRC_BT    2019
#define IDM_SRC_NET   2020

#define HOTKEY_VOL_DN 1
#define HOTKEY_VOL_UP 2

/* vol_raw: 0..200, dv: 0..100 */
#define VOL_RAW_TO_DV(r) (((r) + 1) / 2)

typedef enum {
    STATE_OFFLINE    = 0,
    STATE_CONNECTING = 1,
    STATE_STANDBY    = 2,
    STATE_ONLINE     = 3
} RecvState;

enum {
    SRC_HDMI5 = 0x55,
    SRC_HDMI6 = 0x56,
    SRC_BD    = 0x10,
    SRC_CBL   = 0x01,
    SRC_STRM  = 0x2F,
    SRC_TV    = 0x12,
    SRC_GAME  = 0x02,
    SRC_CD    = 0x23,
    SRC_USB   = 0x29,
    SRC_BT    = 0x2E,
    SRC_NET   = 0x2B
};

/* ------------------------------------------------------------------ */
/* Global shutdown flag                                               */
/* ------------------------------------------------------------------ */

static volatile LONG g_shutting_down = 0;

static int is_shutting_down(void)
{
    return InterlockedCompareExchange(&g_shutting_down, 0, 0) != 0;
}

static void begin_shutdown(void)
{
    InterlockedExchange(&g_shutting_down, 1);
}

/* ------------------------------------------------------------------ */
/* Command queue                                                      */
/* ------------------------------------------------------------------ */

#define CMD_MAXLEN 32
#define CMD_QUEUE  64

typedef struct {
    char cmd[CMD_MAXLEN];
} CmdEntry;

static CRITICAL_SECTION g_cmd_cs;
static CmdEntry         g_cmd_buf[CMD_QUEUE];
static int              g_cmd_head = 0;
static int              g_cmd_tail = 0;

static void enqueue_cmd(const char *cmd)
{
    if (is_shutting_down())
        return;

    EnterCriticalSection(&g_cmd_cs);

    int next = (g_cmd_tail + 1) % CMD_QUEUE;

    if (next != g_cmd_head) {
        strncpy_s(g_cmd_buf[g_cmd_tail].cmd, CMD_MAXLEN, cmd, _TRUNCATE);
        g_cmd_tail = next;
    } else {
        OutputDebugStringA("pioneer_tray: command queue overflow\n");
    }

    LeaveCriticalSection(&g_cmd_cs);
}

static int dequeue_cmd(char *out)
{
    int ok = 0;

    EnterCriticalSection(&g_cmd_cs);

    if (g_cmd_head != g_cmd_tail) {
        strncpy_s(out, CMD_MAXLEN, g_cmd_buf[g_cmd_head].cmd, _TRUNCATE);
        g_cmd_head = (g_cmd_head + 1) % CMD_QUEUE;
        ok = 1;
    }

    LeaveCriticalSection(&g_cmd_cs);

    return ok;
}

/* ------------------------------------------------------------------ */
/* Shutdown socket                                                    */
/* ------------------------------------------------------------------ */

static CRITICAL_SECTION g_shutdown_sock_cs;
static SOCKET           g_shutdown_sock = INVALID_SOCKET;

static void sock_set(SOCKET s)
{
    EnterCriticalSection(&g_shutdown_sock_cs);
    g_shutdown_sock = s;
    LeaveCriticalSection(&g_shutdown_sock_cs);
}

static void sock_shutdown(void)
{
    EnterCriticalSection(&g_shutdown_sock_cs);

    if (g_shutdown_sock != INVALID_SOCKET)
        shutdown(g_shutdown_sock, SD_BOTH);

    LeaveCriticalSection(&g_shutdown_sock_cs);
}

static void close_reader_sock(SOCKET *psock)
{
    SOCKET s = *psock;

    if (s != INVALID_SOCKET) {
        *psock = INVALID_SOCKET;
        sock_set(INVALID_SOCKET);
        closesocket(s);
    }
}

/* ------------------------------------------------------------------ */
/* eISCP helpers                                                      */
/* ------------------------------------------------------------------ */

static int build_iscp(const char *cmd, unsigned char *pkt, int pktsz)
{
    char payload[128];

    int plen = _snprintf_s(payload, sizeof(payload), _TRUNCATE, "%s\r", cmd);
    if (plen < 0)
        return -1;

    if (16 + plen > pktsz)
        return -1;

    memset(pkt, 0, 16);
    memcpy(pkt, "ISCP", 4);

    pkt[7]  = 0x10;
    pkt[8]  = (unsigned char)((plen >> 24) & 0xFF);
    pkt[9]  = (unsigned char)((plen >> 16) & 0xFF);
    pkt[10] = (unsigned char)((plen >> 8) & 0xFF);
    pkt[11] = (unsigned char)(plen & 0xFF);
    pkt[12] = 0x01;

    memcpy(pkt + 16, payload, plen);

    return 16 + plen;
}

static int send_all(SOCKET s, const unsigned char *buf, int len)
{
    int sent = 0;

    while (sent < len) {
        int n = send(s, (const char *)buf + sent, len - sent, 0);
        if (n <= 0)
            return 0;
        sent += n;
    }

    return 1;
}

static int parse_mvl(const char *payload, int len)
{
    for (int i = 0; i <= len - 5; i++) {
        if (payload[i] == 'M' && payload[i + 1] == 'V' && payload[i + 2] == 'L') {
            char hi = payload[i + 3];
            char lo = payload[i + 4];

            if (!isxdigit((unsigned char)hi) || !isxdigit((unsigned char)lo))
                continue;

            char tmp[3] = { hi, lo, 0 };
            long v = strtol(tmp, NULL, 16);

            if (v >= 0 && v <= 200)
                return (int)v;
        }
    }

    return -1;
}

static int parse_pwr(const char *payload, int len)
{
    for (int i = 0; i <= len - 3; i++) {
        if (payload[i] == 'P' && payload[i + 1] == 'W' && payload[i + 2] == 'R') {
            const char *val = payload + i + 3;
            int rem = len - i - 3;

            if (rem >= 2 && val[0] == '0' && val[1] == '1')
                return 1;

            if (rem >= 2 && strncmp(val, "ON", 2) == 0)
                return 1;

            if (rem >= 2 && val[0] == '0' && val[1] == '0')
                return -1;

            if (rem >= 7 && strncmp(val, "STANDBY", 7) == 0)
                return -1;

            if (rem >= 3 && strncmp(val, "OFF", 3) == 0)
                return -1;
        }
    }

    return 0;
}

static int parse_sli(const char *payload, int len)
{
    for (int i = 0; i <= len - 5; i++) {
        if (payload[i] == 'S' && payload[i + 1] == 'L' && payload[i + 2] == 'I') {
            char hi = payload[i + 3];
            char lo = payload[i + 4];

            if (!isxdigit((unsigned char)hi) || !isxdigit((unsigned char)lo))
                continue;

            char tmp[3] = { hi, lo, 0 };
            long v = strtol(tmp, NULL, 16);

            if (v >= 0 && v <= 255)
                return (int)v;
        }
    }

    return -1;
}

static int send_qstn(SOCKET s)
{
    unsigned char pkt[64];
    int plen = build_iscp("!1MVLQSTN", pkt, sizeof(pkt));
    return (plen > 0 && send_all(s, pkt, plen));
}

static int send_pwr_qstn(SOCKET s)
{
    unsigned char pkt[64];
    int plen = build_iscp("!1PWRQSTN", pkt, sizeof(pkt));
    return (plen > 0 && send_all(s, pkt, plen));
}

static int send_sli_qstn(SOCKET s)
{
    unsigned char pkt[64];
    int plen = build_iscp("!1SLIQSTN", pkt, sizeof(pkt));
    return (plen > 0 && send_all(s, pkt, plen));
}

/* ------------------------------------------------------------------ */
/* Reader thread                                                      */
/* ------------------------------------------------------------------ */

static HWND   g_hwnd;
static HANDLE g_thread;
static HANDLE g_stop_event;

static void post_state(RecvState st, int vol)
{
    if (!is_shutting_down() && IsWindow(g_hwnd))
        PostMessageA(g_hwnd, WM_SET_STATE, (WPARAM)st, (LPARAM)vol);
}

static void post_source(int src)
{
    if (!is_shutting_down() && IsWindow(g_hwnd))
        PostMessageA(g_hwnd, WM_SET_SOURCE, (WPARAM)src, 0);
}

static unsigned __stdcall reader_thread(void *arg)
{
    (void)arg;

    unsigned char stream[8192];
    int stream_len = 0;
    SOCKET sock = INVALID_SOCKET;

    while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
        if (sock == INVALID_SOCKET) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) {
                post_state(STATE_OFFLINE, -1);
                WaitForSingleObject(g_stop_event, 2000);
                continue;
            }

            BOOL ka = TRUE;
            setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (char *)&ka, sizeof(ka));

            u_long nb = 1;
            ioctlsocket(s, FIONBIO, &nb);

            struct sockaddr_in addr = {0};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(DEVICE_PORT);

            if (InetPtonA(AF_INET, DEVICE_IP, &addr.sin_addr) != 1) {
                closesocket(s);
                post_state(STATE_OFFLINE, -1);
                WaitForSingleObject(g_stop_event, 2000);
                continue;
            }

            int rc = connect(s, (struct sockaddr *)&addr, sizeof(addr));
            if (rc == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
                closesocket(s);
                post_state(STATE_OFFLINE, -1);
                WaitForSingleObject(g_stop_event, 2000);
                continue;
            }

            fd_set wfds, efds;
            FD_ZERO(&wfds);
            FD_SET(s, &wfds);
            FD_ZERO(&efds);
            FD_SET(s, &efds);

            struct timeval ctv = {3, 0};

            rc = select(0, NULL, &wfds, &efds, &ctv);
            if (rc <= 0) {
                closesocket(s);
                post_state(STATE_OFFLINE, -1);
                WaitForSingleObject(g_stop_event, 2000);
                continue;
            }

            int sockerr = 0;
            int errlen = sizeof(sockerr);

            if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&sockerr, &errlen) != 0 || sockerr != 0) {
                closesocket(s);
                post_state(STATE_OFFLINE, -1);
                WaitForSingleObject(g_stop_event, 2000);
                continue;
            }

            nb = 0;
            ioctlsocket(s, FIONBIO, &nb);

            sock = s;
            sock_set(sock);
            stream_len = 0;

            post_state(STATE_CONNECTING, -1);

            if (!send_pwr_qstn(sock)) {
                close_reader_sock(&sock);
                post_state(STATE_OFFLINE, -1);
                continue;
            }
        }

        if (WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0)
            break;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);

        struct timeval tv = {0, 100000};

        int sel = select(0, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            close_reader_sock(&sock);
            stream_len = 0;
            post_state(STATE_OFFLINE, -1);
            continue;
        }

        char cmd[CMD_MAXLEN];

        while (dequeue_cmd(cmd)) {
            unsigned char pkt[64];
            int plen = build_iscp(cmd, pkt, sizeof(pkt));

            if (plen <= 0 || !send_all(sock, pkt, plen)) {
                close_reader_sock(&sock);
                stream_len = 0;
                post_state(STATE_OFFLINE, -1);
                break;
            }
        }

        if (sock == INVALID_SOCKET)
            continue;

        if (!(sel > 0 && FD_ISSET(sock, &rfds)))
            continue;

        int room = (int)sizeof(stream) - stream_len - 1;
        if (room <= 0) {
            stream_len = 0;
            room = (int)sizeof(stream) - 1;
        }

        int r = recv(sock, (char *)stream + stream_len, room, 0);

        if (r == 0) {
            close_reader_sock(&sock);
            stream_len = 0;
            post_state(STATE_OFFLINE, -1);
            continue;
        }

        if (r < 0) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK)
                continue;

            close_reader_sock(&sock);
            stream_len = 0;
            post_state(STATE_OFFLINE, -1);
            continue;
        }

        stream_len += r;

        while (stream_len >= 16) {
            if (memcmp(stream, "ISCP", 4) != 0) {
                int skip = -1;

                for (int i = 1; i <= stream_len - 4; i++) {
                    if (memcmp(stream + i, "ISCP", 4) == 0) {
                        skip = i;
                        break;
                    }
                }

                if (skip < 0) {
                    stream_len = 0;
                    break;
                }

                memmove(stream, stream + skip, stream_len - skip);
                stream_len -= skip;
                continue;
            }

            uint32_t hdr_len =
                ((uint32_t)stream[4] << 24) |
                ((uint32_t)stream[5] << 16) |
                ((uint32_t)stream[6] << 8) |
                (uint32_t)stream[7];

            if (hdr_len != 16) {
                stream_len = 0;
                break;
            }

            uint32_t data_len =
                ((uint32_t)stream[8] << 24) |
                ((uint32_t)stream[9] << 16) |
                ((uint32_t)stream[10] << 8) |
                (uint32_t)stream[11];

            if (data_len > 4096) {
                stream_len = 0;
                break;
            }

            int total = (int)(hdr_len + data_len);
            if (stream_len < total)
                break;

            char payload[4097];
            memcpy(payload, stream + hdr_len, data_len);
            payload[data_len] = 0;

            int v = parse_mvl(payload, (int)data_len);
            if (v >= 0)
                post_state(STATE_ONLINE, v);

            int src = parse_sli(payload, (int)data_len);
            if (src >= 0)
                post_source(src);

            if (v < 0 && src < 0) {
                int pwr = parse_pwr(payload, (int)data_len);

                if (pwr < 0) {
                    post_state(STATE_STANDBY, -1);
                } else if (pwr > 0) {
                    post_state(STATE_CONNECTING, -1);

                    if (!send_qstn(sock) || !send_sli_qstn(sock)) {
                        close_reader_sock(&sock);
                        stream_len = 0;
                        post_state(STATE_OFFLINE, -1);
                        break;
                    }
                }
            }

            int remain = stream_len - total;
            if (remain > 0)
                memmove(stream, stream + total, (size_t)remain);

            stream_len = remain;
        }
    }

    close_reader_sock(&sock);
    return 0;
}

/* ------------------------------------------------------------------ */
/* ICON                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int r, g, b;
} Color;

static Color lerp_color(Color a, Color b, float t)
{
    Color c;
    c.r = a.r + (int)((b.r - a.r) * t);
    c.g = a.g + (int)((b.g - a.g) * t);
    c.b = a.b + (int)((b.b - a.b) * t);
    return c;
}

static Color vol_color(int dv)
{
    if (dv < 0)
        return (Color){60, 60, 60};

    if (dv <= 20)
        return lerp_color((Color){120, 200, 255}, (Color){0, 90, 255}, dv / 20.0f);

    if (dv <= 40)
        return lerp_color((Color){0, 90, 255}, (Color){0, 220, 80}, (dv - 20) / 20.0f);

    if (dv <= 60)
        return lerp_color((Color){0, 220, 80}, (Color){255, 220, 0}, (dv - 40) / 20.0f);

    if (dv <= 80)
        return lerp_color((Color){255, 220, 0}, (Color){255, 140, 0}, (dv - 60) / 20.0f);

    return lerp_color((Color){255, 140, 0}, (Color){220, 40, 40}, (dv - 80) / 20.0f);
}

static HICON create_icon(int vol_raw, RecvState state)
{
    int dv = (state == STATE_ONLINE && vol_raw >= 0) ? VOL_RAW_TO_DV(vol_raw) : -1;

    Color bg;

    switch (state) {
    case STATE_OFFLINE:
        bg = (Color){40, 40, 40};
        break;
    case STATE_CONNECTING:
        bg = (Color){40, 40, 80};
        break;
    case STATE_STANDBY:
        bg = (Color){60, 45, 20};
        break;
    default:
        bg = vol_color(dv);
        break;
    }

    HDC hdc = GetDC(NULL);
    HDC mem = CreateCompatibleDC(hdc);

    HBITMAP bmp = CreateCompatibleBitmap(hdc, 16, 16);
    HBITMAP mask = CreateBitmap(16, 16, 1, 1, NULL);
    HBITMAP old = SelectObject(mem, bmp);

    RECT rc = {0, 0, 16, 16};

    HBRUSH br = CreateSolidBrush(RGB(bg.r, bg.g, bg.b));
    FillRect(mem, &rc, br);
    DeleteObject(br);

    switch (state) {
    case STATE_OFFLINE: {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(180, 40, 40));
        HPEN op = SelectObject(mem, pen);

        MoveToEx(mem, 3, 3, NULL);
        LineTo(mem, 13, 13);

        MoveToEx(mem, 13, 3, NULL);
        LineTo(mem, 3, 13);

        SelectObject(mem, op);
        DeleteObject(pen);
        break;
    }

    case STATE_CONNECTING: {
        HBRUSH db = CreateSolidBrush(RGB(120, 120, 220));

        RECT d1 = {2, 6, 5, 10};
        RECT d2 = {6, 6, 10, 10};
        RECT d3 = {11, 6, 14, 10};

        FillRect(mem, &d1, db);
        FillRect(mem, &d2, db);
        FillRect(mem, &d3, db);

        DeleteObject(db);
        break;
    }

    case STATE_STANDBY: {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(220, 140, 30));
        HPEN op = SelectObject(mem, pen);

        SelectObject(mem, GetStockObject(NULL_BRUSH));

        Ellipse(mem, 3, 4, 13, 14);

        MoveToEx(mem, 8, 1, NULL);
        LineTo(mem, 8, 7);

        SelectObject(mem, op);
        DeleteObject(pen);
        break;
    }

    default: {
        char txt[8];

        if (dv < 0)
            strcpy_s(txt, sizeof(txt), "?");
        else
            _snprintf_s(txt, sizeof(txt), _TRUNCATE, "%d", dv);

        HFONT font = CreateFontA(
            -9, 0, 0, 0,
            FW_BOLD,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_SWISS,
            "Segoe UI"
        );

        HFONT oldf = SelectObject(mem, font);

        SetBkMode(mem, TRANSPARENT);

        int bright = bg.r * 30 + bg.g * 59 + bg.b * 11;
        SetTextColor(mem, (bright > 12000) ? RGB(20, 20, 20) : RGB(240, 240, 240));

        DrawTextA(mem, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(mem, oldf);
        DeleteObject(font);
        break;
    }
    }

    SelectObject(mem, old);

    ICONINFO ii = {0};
    ii.fIcon = TRUE;
    ii.hbmColor = bmp;
    ii.hbmMask = mask;

    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(bmp);
    DeleteObject(mask);

    DeleteDC(mem);
    ReleaseDC(NULL, hdc);

    return icon;
}

/* ------------------------------------------------------------------ */
/* TRAY globals                                                       */
/* ------------------------------------------------------------------ */

static NOTIFYICONDATAA g_nid;
static HICON     g_icon;
static int       g_vol_raw = -1;
static RecvState g_state   = STATE_OFFLINE;
static int       g_src     = -1;

static void update_tray(void)
{
    int dv = (g_state == STATE_ONLINE && g_vol_raw >= 0) ? VOL_RAW_TO_DV(g_vol_raw) : -1;

    switch (g_state) {
    case STATE_OFFLINE:
        strcpy_s(g_nid.szTip, sizeof(g_nid.szTip), "Pioneer: offline");
        break;

    case STATE_CONNECTING:
        strcpy_s(g_nid.szTip, sizeof(g_nid.szTip), "Pioneer: connecting...");
        break;

    case STATE_STANDBY:
        strcpy_s(g_nid.szTip, sizeof(g_nid.szTip), "Pioneer: standby");
        break;

    case STATE_ONLINE:
        if (dv < 0)
            strcpy_s(g_nid.szTip, sizeof(g_nid.szTip), "Pioneer: ?");
        else
            _snprintf_s(g_nid.szTip, sizeof(g_nid.szTip), _TRUNCATE, "Pioneer: %d", dv);
        break;
    }

    if (g_icon)
        DestroyIcon(g_icon);

    g_icon = create_icon(g_vol_raw, g_state);
    g_nid.hIcon = g_icon;

    Shell_NotifyIconA(NIM_MODIFY, &g_nid);
}

/* ------------------------------------------------------------------ */
/* VOLUME POPUP                                                       */
/* ------------------------------------------------------------------ */

#define POPUP_W   36
#define POPUP_H   180

#define TRACK_X   (POPUP_W / 2)
#define TRACK_TOP 12
#define TRACK_BOT (POPUP_H - 12)
#define TRACK_LEN (TRACK_BOT - TRACK_TOP)
#define THUMB_R   7

#define COL_BG       RGB(100, 100, 100)
#define COL_TRACK    RGB(160, 160, 160)
#define COL_FILL     RGB(80,  140, 220)
#define COL_THUMB    RGB(255, 255, 255)
#define COL_THUMB_HL RGB(180, 210, 255)

static HWND      g_popup_hwnd  = NULL;
static int       g_popup_dv    = 0;
static BOOL      g_popup_drag  = FALSE;
static ULONGLONG g_popup_last_user = 0;
static ULONGLONG g_popup_last_send = 0;

#define POPUP_SEND_INTERVAL_MS 40

static int dv_to_y(int dv)
{
    return TRACK_BOT - dv * TRACK_LEN / 100;
}

static int y_to_dv(int y)
{
    int dv = (TRACK_BOT - y) * 100 / TRACK_LEN;

    if (dv < 0)
        dv = 0;

    if (dv > 100)
        dv = 100;

    return dv;
}

static void popup_send_vol(int dv, BOOL force)
{
    ULONGLONG now = GetTickCount64();

    if (!force && g_popup_drag && (now - g_popup_last_send) < POPUP_SEND_INTERVAL_MS)
        return;

    int raw = dv * 2;
    if (raw > 200)
        raw = 200;

    char cmd[CMD_MAXLEN];
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "!1MVL%02X", raw);
    enqueue_cmd(cmd);

    g_popup_last_send = now;
    g_popup_last_user = now;
}

static void popup_net_update(int dv)
{
    if (!g_popup_hwnd)
        return;

    if (g_popup_drag)
        return;

    if (GetTickCount64() - g_popup_last_user < 700)
        return;

    if (dv == g_popup_dv)
        return;

    g_popup_dv = dv;
    InvalidateRect(g_popup_hwnd, NULL, FALSE);
}

static void popup_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, POPUP_W, POPUP_H);
    HBITMAP old = SelectObject(mem, bmp);

    RECT rcAll = {0, 0, POPUP_W, POPUP_H};

    HBRUSH hbg = CreateSolidBrush(COL_BG);
    FillRect(mem, &rcAll, hbg);
    DeleteObject(hbg);

    int ty = dv_to_y(g_popup_dv);

    HBRUSH hbt = CreateSolidBrush(COL_TRACK);
    RECT rt = {TRACK_X - 2, TRACK_TOP, TRACK_X + 3, ty};
    FillRect(mem, &rt, hbt);
    DeleteObject(hbt);

    HBRUSH hbf = CreateSolidBrush(COL_FILL);
    RECT rf = {TRACK_X - 2, ty, TRACK_X + 3, TRACK_BOT};
    FillRect(mem, &rf, hbf);
    DeleteObject(hbf);

    HBRUSH hbth = CreateSolidBrush(g_popup_drag ? COL_THUMB_HL : COL_THUMB);
    SelectObject(mem, hbth);
    SelectObject(mem, GetStockObject(NULL_PEN));

    Ellipse(mem, TRACK_X - THUMB_R, ty - THUMB_R, TRACK_X + THUMB_R, ty + THUMB_R);

    DeleteObject(hbth);

    BitBlt(hdc, 0, 0, POPUP_W, POPUP_H, mem, 0, 0, SRCCOPY);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);

    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK VolumePopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        popup_paint(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        int my = (short)HIWORD(lp);

        g_popup_drag = TRUE;
        SetCapture(hwnd);

        int dv = y_to_dv(my);
        g_popup_dv = dv;

        popup_send_vol(dv, FALSE);
        InvalidateRect(hwnd, NULL, FALSE);

        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!g_popup_drag)
            return 0;

        int my = (short)HIWORD(lp);
        int dv = y_to_dv(my);

        if (dv != g_popup_dv) {
            g_popup_dv = dv;
            popup_send_vol(dv, FALSE);
            InvalidateRect(hwnd, NULL, FALSE);
        }

        return 0;
    }

    case WM_LBUTTONUP:
        if (g_popup_drag) {
            g_popup_drag = FALSE;
            ReleaseCapture();

            popup_send_vol(g_popup_dv, TRUE);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int steps = delta / WHEEL_DELTA;

        if (steps == 0)
            steps = (delta > 0) ? 1 : -1;

        int new_dv = g_popup_dv + steps;

        if (new_dv < 0)
            new_dv = 0;

        if (new_dv > 100)
            new_dv = 100;

        if (new_dv != g_popup_dv) {
            g_popup_dv = new_dv;
            popup_send_vol(new_dv, TRUE);
            InvalidateRect(hwnd, NULL, FALSE);
        }

        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }

        if (wp == VK_UP || wp == VK_RIGHT) {
            int dv = g_popup_dv + 1;
            if (dv > 100)
                dv = 100;

            g_popup_dv = dv;
            popup_send_vol(dv, TRUE);
            InvalidateRect(hwnd, NULL, FALSE);

            return 0;
        }

        if (wp == VK_DOWN || wp == VK_LEFT) {
            int dv = g_popup_dv - 1;
            if (dv < 0)
                dv = 0;

            g_popup_dv = dv;
            popup_send_vol(dv, TRUE);
            InvalidateRect(hwnd, NULL, FALSE);

            return 0;
        }

        break;

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE)
            DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_popup_hwnd = NULL;
        g_popup_drag = FALSE;
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void create_volume_popup(void)
{
    if (g_popup_hwnd) {
        DestroyWindow(g_popup_hwnd);
        return;
    }

    RECT icon_rc = {0};
    BOOL got_icon_rc = FALSE;

    {
        HMODULE hShell = GetModuleHandleA("shell32.dll");

        if (hShell) {
            typedef HRESULT (WINAPI *PFN)(const NOTIFYICONIDENTIFIER *, RECT *);
            PFN pfn = (PFN)(void *)GetProcAddress(hShell, "Shell_NotifyIconGetRect");

            if (pfn) {
                NOTIFYICONIDENTIFIER nii = {0};
                nii.cbSize = sizeof(nii);
                nii.hWnd = g_nid.hWnd;
                nii.uID = g_nid.uID;

                got_icon_rc = SUCCEEDED(pfn(&nii, &icon_rc));
            }
        }
    }

    RECT work = {0};
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);

    int x, y;

    if (got_icon_rc) {
        int icx = (icon_rc.left + icon_rc.right) / 2;
        x = icx - POPUP_W / 2;
        y = icon_rc.top - POPUP_H - 4;
    } else {
        POINT pt;
        GetCursorPos(&pt);

        x = pt.x - POPUP_W / 2;
        y = work.bottom - POPUP_H - 4;
    }

    if (x < work.left)
        x = work.left;

    if (x + POPUP_W > work.right)
        x = work.right - POPUP_W;

    if (y < work.top)
        y = work.top;

    if (y + POPUP_H > work.bottom)
        y = work.bottom - POPUP_H;

    static BOOL registered = FALSE;

    if (!registered) {
        WNDCLASSA wc = {0};

        wc.lpfnWndProc = VolumePopupProc;
        wc.hInstance = (HINSTANCE)GetWindowLongPtrA(g_hwnd, GWLP_HINSTANCE);
        wc.hbrBackground = NULL;
        wc.lpszClassName = "PioneerVolPopup";

        if (!RegisterClassA(&wc)) {
            OutputDebugStringA("pioneer_tray: RegisterClassA for popup failed\n");
            return;
        }

        registered = TRUE;
    }

    g_popup_dv = (g_state == STATE_ONLINE && g_vol_raw >= 0) ? VOL_RAW_TO_DV(g_vol_raw) : 0;
    g_popup_drag = FALSE;
    g_popup_last_user = 0;
    g_popup_last_send = 0;

    g_popup_hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        "PioneerVolPopup",
        "",
        WS_POPUP | WS_BORDER,
        x, y,
        POPUP_W, POPUP_H,
        g_hwnd,
        NULL,
        (HINSTANCE)GetWindowLongPtrA(g_hwnd, GWLP_HINSTANCE),
        NULL
    );

    if (!g_popup_hwnd)
        return;

    SetLayeredWindowAttributes(g_popup_hwnd, 0, 166, LWA_ALPHA);

    ShowWindow(g_popup_hwnd, SW_SHOWNA);

    SetForegroundWindow(g_popup_hwnd);
    SetFocus(g_popup_hwnd);
}

/* ------------------------------------------------------------------ */
/* Source helpers                                                     */
/* ------------------------------------------------------------------ */

static void enqueue_sli(int code)
{
    char cmd[CMD_MAXLEN];
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "!1SLI%02X", code);
    enqueue_cmd(cmd);
}

static void select_source(int code)
{
    enqueue_sli(code);
    g_src = code;
}

static const char *src_name(int code)
{
    switch (code) {
    case SRC_HDMI5:
        return "HDMI 5";
    case SRC_HDMI6:
        return "HDMI 6";
    case SRC_BD:
        return "BD/DVD";
    case SRC_CBL:
        return "CBL/SAT";
    case SRC_STRM:
        return "STRM BOX";
    case SRC_TV:
        return "TV";
    case SRC_GAME:
        return "GAME";
    case SRC_CD:
        return "CD";
    case SRC_USB:
        return "USB";
    case SRC_BT:
        return "BLUETOOTH";
    case SRC_NET:
        return "NET";
    default:
        return NULL;
    }
}

static void append_src_item(HMENU menu, UINT id, int code, const char *text)
{
    UINT flags = MF_STRING;

    if (g_src == code)
        flags |= MF_CHECKED;

    AppendMenuA(menu, flags, id, text);
}

/* ------------------------------------------------------------------ */
/* WINDOW PROC                                                        */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        WSADATA wsa;

        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            OutputDebugStringA("pioneer_tray: WSAStartup failed\n");
            return -1;
        }

        g_stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!g_stop_event) {
            OutputDebugStringA("pioneer_tray: CreateEventA failed\n");
            WSACleanup();
            return -1;
        }

        InitializeCriticalSection(&g_cmd_cs);
        InitializeCriticalSection(&g_shutdown_sock_cs);

        if (!RegisterHotKey(hwnd, HOTKEY_VOL_DN, 0, 0x7C))
            OutputDebugStringA("Hotkey F13 failed\n");

        if (!RegisterHotKey(hwnd, HOTKEY_VOL_UP, 0, 0x7D))
            OutputDebugStringA("Hotkey F14 failed\n");

        memset(&g_nid, 0, sizeof(g_nid));

        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = hwnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAY_ICON;

        g_icon = create_icon(-1, STATE_OFFLINE);
        g_nid.hIcon = g_icon;

        strcpy_s(g_nid.szTip, sizeof(g_nid.szTip), "Pioneer: connecting...");

        if (!Shell_NotifyIconA(NIM_ADD, &g_nid))
            OutputDebugStringA("Shell_NotifyIconA(NIM_ADD) failed\n");

        g_thread = (HANDLE)_beginthreadex(NULL, 0, reader_thread, NULL, 0, NULL);

        if (!g_thread) {
            OutputDebugStringA("pioneer_tray: _beginthreadex failed\n");
            DestroyWindow(hwnd);
        }

        return 0;
    }

    case WM_SET_STATE: {
        if (is_shutting_down())
            return 0;

        RecvState new_state = (RecvState)(int)wp;
        int new_vol = (int)lp;

        if (new_state == g_state && (new_state != STATE_ONLINE || new_vol == g_vol_raw))
            return 0;

        RecvState old_state = g_state;

        g_state = new_state;

        if (new_state == STATE_ONLINE && new_vol >= 0)
            g_vol_raw = new_vol;
        else
            g_vol_raw = -1;

        if (new_state == STATE_OFFLINE ||
            new_state == STATE_STANDBY ||
            (new_state == STATE_CONNECTING && old_state != STATE_CONNECTING)) {
            g_src = -1;
        }

        if (g_state == STATE_ONLINE && g_vol_raw >= 0)
            popup_net_update(VOL_RAW_TO_DV(g_vol_raw));

        update_tray();

        return 0;
    }

    case WM_SET_SOURCE: {
        if (is_shutting_down())
            return 0;

        int src = (int)wp;

        if (g_state == STATE_ONLINE || g_state == STATE_CONNECTING) {
            if (src != g_src)
                g_src = src;
        }

        return 0;
    }

    case WM_HOTKEY:
        if (!is_shutting_down() && g_state == STATE_ONLINE) {
            if (wp == HOTKEY_VOL_DN)
                enqueue_cmd("!1MVLDOWN");

            if (wp == HOTKEY_VOL_UP)
                enqueue_cmd("!1MVLUP");
        }
        return 0;

    case WM_TRAY_ICON:
        if (LOWORD(lp) == WM_LBUTTONUP) {
            if (!is_shutting_down() && g_state == STATE_ONLINE)
                create_volume_popup();
        }

        if (LOWORD(lp) == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);

            HMENU m = CreatePopupMenu();

            if (g_state == STATE_STANDBY) {
                AppendMenuA(m, MF_STRING, IDM_POWER, "Power On");
            } else if (g_state == STATE_ONLINE || g_state == STATE_CONNECTING) {
                AppendMenuA(m, MF_STRING, IDM_POWER, "Standby");
            } else {
                AppendMenuA(m, MF_GRAYED | MF_STRING, IDM_POWER, "Power");
            }

            if (g_state == STATE_ONLINE) {
                HMENU srcMenu = CreatePopupMenu();

                append_src_item(srcMenu, IDM_SRC_HDMI5, SRC_HDMI5, "HDMI 5");
                append_src_item(srcMenu, IDM_SRC_HDMI6, SRC_HDMI6, "HDMI 6");

                AppendMenuA(srcMenu, MF_SEPARATOR, 0, NULL);

                append_src_item(srcMenu, IDM_SRC_BD,    SRC_BD,    "BD/DVD");
                append_src_item(srcMenu, IDM_SRC_CBL,   SRC_CBL,   "CBL/SAT");
                append_src_item(srcMenu, IDM_SRC_STRM,  SRC_STRM,  "STRM BOX");
                append_src_item(srcMenu, IDM_SRC_TV,    SRC_TV,    "TV");
                append_src_item(srcMenu, IDM_SRC_GAME,  SRC_GAME,  "GAME");
                append_src_item(srcMenu, IDM_SRC_CD,    SRC_CD,    "CD");
                append_src_item(srcMenu, IDM_SRC_USB,   SRC_USB,   "USB");
                append_src_item(srcMenu, IDM_SRC_BT,    SRC_BT,    "BLUETOOTH");
                append_src_item(srcMenu, IDM_SRC_NET,   SRC_NET,   "NET");

                char srcLabel[64];
                const char *cur = src_name(g_src);

                if (cur)
                    _snprintf_s(srcLabel, sizeof(srcLabel), _TRUNCATE, "Source (%s)", cur);
                else
                    strcpy_s(srcLabel, sizeof(srcLabel), "Source");

                AppendMenuA(m, MF_POPUP, (UINT_PTR)srcMenu, srcLabel);
            } else {
                AppendMenuA(m, MF_GRAYED | MF_STRING, 0, "Source");
            }

            AppendMenuA(m, MF_SEPARATOR, 0, NULL);
            AppendMenuA(m, MF_STRING, IDM_EXIT, "Exit");

            SetForegroundWindow(hwnd);

            TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);

            DestroyMenu(m);
        }

        return 0;

    case WM_COMMAND:
        if (!is_shutting_down()) {
            if (LOWORD(wp) == IDM_POWER) {
                if (g_state == STATE_STANDBY)
                    enqueue_cmd("!1PWR01");
                else if (g_state == STATE_ONLINE || g_state == STATE_CONNECTING)
                    enqueue_cmd("!1PWR00");
            }

            if (g_state == STATE_ONLINE) {
                if (LOWORD(wp) == IDM_SRC_HDMI5) select_source(SRC_HDMI5);
                if (LOWORD(wp) == IDM_SRC_HDMI6) select_source(SRC_HDMI6);
                if (LOWORD(wp) == IDM_SRC_BD)    select_source(SRC_BD);
                if (LOWORD(wp) == IDM_SRC_CBL)   select_source(SRC_CBL);
                if (LOWORD(wp) == IDM_SRC_STRM)  select_source(SRC_STRM);
                if (LOWORD(wp) == IDM_SRC_TV)    select_source(SRC_TV);
                if (LOWORD(wp) == IDM_SRC_GAME)  select_source(SRC_GAME);
                if (LOWORD(wp) == IDM_SRC_CD)    select_source(SRC_CD);
                if (LOWORD(wp) == IDM_SRC_USB)   select_source(SRC_USB);
                if (LOWORD(wp) == IDM_SRC_BT)    select_source(SRC_BT);
                if (LOWORD(wp) == IDM_SRC_NET)   select_source(SRC_NET);
            }

            if (LOWORD(wp) == IDM_EXIT)
                DestroyWindow(hwnd);
        }

        return 0;

    case WM_DESTROY:
        begin_shutdown();

        if (g_popup_hwnd) {
            DestroyWindow(g_popup_hwnd);
            g_popup_hwnd = NULL;
        }

        UnregisterHotKey(hwnd, HOTKEY_VOL_DN);
        UnregisterHotKey(hwnd, HOTKEY_VOL_UP);

        Shell_NotifyIconA(NIM_DELETE, &g_nid);

        if (g_icon) {
            DestroyIcon(g_icon);
            g_icon = NULL;
        }

        SetEvent(g_stop_event);
        sock_shutdown();

        if (g_thread) {
            DWORD wr = WaitForSingleObject(g_thread, 5000);

            if (wr == WAIT_OBJECT_0) {
                CloseHandle(g_thread);
                g_thread = NULL;

                CloseHandle(g_stop_event);

                DeleteCriticalSection(&g_cmd_cs);
                DeleteCriticalSection(&g_shutdown_sock_cs);

                WSACleanup();
            } else {
                OutputDebugStringA("pioneer_tray: reader thread didn't stop in time, leaking resources\n");
            }
        } else {
            CloseHandle(g_stop_event);

            DeleteCriticalSection(&g_cmd_cs);
            DeleteCriticalSection(&g_shutdown_sock_cs);

            WSACleanup();
        }

        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* MAIN                                                               */
/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    (void)hPrev;
    (void)lpCmd;
    (void)nShow;

    WNDCLASSA wc = {0};

    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "ISCPT";

    if (!RegisterClassA(&wc)) {
        OutputDebugStringA("pioneer_tray: RegisterClassA failed\n");
        return 0;
    }

    g_hwnd = CreateWindowA(
        "ISCPT",
        "",
        0,
        0, 0, 0, 0,
        HWND_MESSAGE,
        NULL,
        hInst,
        NULL
    );

    if (!g_hwnd) {
        OutputDebugStringA("pioneer_tray: CreateWindowA failed\n");
        return 0;
    }

    MSG msg;

    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return 0;
}
