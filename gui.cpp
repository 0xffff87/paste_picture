// GUI 窗口进程 - 显示摘要、收集用户反馈
#include <windows.h>
#include <objidl.h>
#include <shellapi.h>
#include <gdiplus.h>
#include "common.h"

// ============================================================
// 全局状态
// ============================================================

struct GuiState {
    std::wstring summary;
    fs::path     result_file;

    HWND  hwnd_main          = nullptr;
    HWND  hwnd_summary_label = nullptr;
    HWND  hwnd_summary       = nullptr;
    HWND  hwnd_input_label   = nullptr;
    HWND  hwnd_edit          = nullptr;
    HWND  hwnd_preview       = nullptr; // 图片预览区域
    HWND  hwnd_submit        = nullptr;
    HFONT font               = nullptr;
    bool  expired            = false;

    std::vector<fs::path> images; // 粘贴的图片临时文件路径
};

static GuiState g;
static WNDPROC g_orig_edit_proc = nullptr;

// ============================================================
// GDI+ 辅助
// ============================================================

static int get_encoder_clsid(const WCHAR* format, CLSID* pClsid)
{
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;

    auto* info = (Gdiplus::ImageCodecInfo*)malloc(size);
    if (!info) return -1;

    Gdiplus::GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(info[i].MimeType, format) == 0) {
            *pClsid = info[i].Clsid;
            free(info);
            return (int)i;
        }
    }
    free(info);
    return -1;
}

// 从剪贴板保存位图为 PNG 临时文件，返回路径（失败返回空）
static fs::path save_clipboard_bitmap_as_png(HBITMAP hBitmap)
{
    if (!hBitmap) return {};

    Gdiplus::Bitmap bmp(hBitmap, nullptr);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return {};

    CLSID pngClsid;
    if (get_encoder_clsid(L"image/png", &pngClsid) < 0) return {};

    fs::path img_path = make_temp_path("feedback_img");
    img_path.replace_extension(L".png");

    if (bmp.Save(img_path.wstring().c_str(), &pngClsid, nullptr) != Gdiplus::Ok)
        return {};

    return img_path;
}

// ============================================================
// 工具函数
// ============================================================

static int scale(int value)
{
    static int dpi = 0;
    if (dpi == 0) {
        HDC hdc = GetDC(nullptr);
        dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(nullptr, hdc);
    }
    return MulDiv(value, dpi, 96);
}

static void insert_file_paths(HDROP hdrop, HWND target)
{
    UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
        std::wstring path(len, L'\0');
        DragQueryFileW(hdrop, i, &path[0], len + 1);
        if (i > 0)
            SendMessageW(target, EM_REPLACESEL, TRUE, (LPARAM)L"\r\n");
        SendMessageW(target, EM_REPLACESEL, TRUE, (LPARAM)path.c_str());
    }
}

static void submit_and_close()
{
    int len = GetWindowTextLengthW(g.hwnd_edit);
    std::wstring text(len, L'\0');
    if (len > 0)
        GetWindowTextW(g.hwnd_edit, &text[0], len + 1);

    std::string utf8 = wide_to_utf8(text);
    std::ofstream out(g.result_file, std::ios::binary | std::ios::trunc);
    if (out)
        out.write(utf8.c_str(), (std::streamsize)utf8.size());

    // 如果有粘贴的图片，写入 .images 文件（每行一个路径）
    if (!g.images.empty()) {
        fs::path images_file = g.result_file;
        images_file += ".images";
        std::ofstream img_out(images_file, std::ios::binary | std::ios::trunc);
        if (img_out) {
            for (auto& p : g.images)
                img_out << p.u8string() << "\n";
        }
    }

    DestroyWindow(g.hwnd_main);
}

// 前向声明
static void layout(int client_w, int client_h);

// ============================================================
// 图片预览区域
// ============================================================

static const int THUMB_SIZE = 80;  // 缩略图尺寸（像素，缩放前）
static const int THUMB_GAP  = 6;
static const int CLOSE_BTN  = 16; // 关闭按钮尺寸

// 存储每个缩略图的命中区域
struct ThumbRect {
    RECT img_rect;   // 整个缩略图区域
    RECT close_rect; // 右上角 × 按钮区域
};
static std::vector<ThumbRect> g_thumb_rects;

static void calc_thumb_rects(const RECT& client)
{
    g_thumb_rects.clear();
    int thumb = scale(THUMB_SIZE);
    int gap   = scale(THUMB_GAP);
    int btn   = scale(CLOSE_BTN);
    int x = gap;

    for (auto& img_path : g.images) {
        Gdiplus::Image img(img_path.wstring().c_str());
        if (img.GetLastStatus() != Gdiplus::Ok)
            continue;

        int w = (int)img.GetWidth(), h = (int)img.GetHeight();
        float ratio = (float)thumb / (float)(w > h ? w : h);
        int dw = (int)(w * ratio), dh = (int)(h * ratio);
        int dy = (client.bottom - dh) / 2;

        ThumbRect tr;
        tr.img_rect   = { x, dy, x + dw, dy + dh };
        tr.close_rect = { x + dw - btn, dy, x + dw, dy + btn };
        g_thumb_rects.push_back(tr);

        x += dw + gap;
    }
}

static LRESULT CALLBACK preview_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));

        calc_thumb_rects(rc);

        Gdiplus::Graphics gfx(hdc);
        gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

        for (size_t i = 0; i < g.images.size() && i < g_thumb_rects.size(); ++i) {
            auto& tr = g_thumb_rects[i];
            Gdiplus::Image img(g.images[i].wstring().c_str());
            if (img.GetLastStatus() != Gdiplus::Ok)
                continue;

            int x = tr.img_rect.left, y = tr.img_rect.top;
            int dw = tr.img_rect.right - x, dh = tr.img_rect.bottom - y;

            gfx.DrawImage(&img, x, y, dw, dh);

            // 边框
            Gdiplus::Pen pen(Gdiplus::Color(180, 180, 180));
            gfx.DrawRectangle(&pen, x, y, dw - 1, dh - 1);

            // 右上角 × 关闭按钮
            int bx = tr.close_rect.left, by = tr.close_rect.top;
            int bw = tr.close_rect.right - bx, bh = tr.close_rect.bottom - by;
            Gdiplus::SolidBrush bg(Gdiplus::Color(180, 0, 0, 0));
            gfx.FillRectangle(&bg, bx, by, bw, bh);
            Gdiplus::Pen xpen(Gdiplus::Color(255, 255, 255), 1.5f);
            int m = scale(3);
            gfx.DrawLine(&xpen, bx + m, by + m, bx + bw - m, by + bh - m);
            gfx.DrawLine(&xpen, bx + bw - m, by + m, bx + m, by + bh - m);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    if (msg == WM_LBUTTONDOWN) {
        int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
        POINT pt = { mx, my };

        for (size_t i = 0; i < g_thumb_rects.size() && i < g.images.size(); ++i) {
            // 点击 × 按钮：删除图片
            if (PtInRect(&g_thumb_rects[i].close_rect, pt)) {
                std::error_code ec;
                fs::remove(g.images[i], ec);
                g.images.erase(g.images.begin() + i);

                // 更新布局和预览
                RECT rc;
                GetClientRect(g.hwnd_main, &rc);
                layout(rc.right, rc.bottom);
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            // 点击缩略图：用默认程序打开图片
            if (PtInRect(&g_thumb_rects[i].img_rect, pt)) {
                ShellExecuteW(nullptr, L"open",
                    g.images[i].wstring().c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
        }
        return 0;
    }

    if (msg == WM_SETCURSOR) {
        // 鼠标在缩略图上时显示手型光标
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        for (auto& tr : g_thumb_rects) {
            if (PtInRect(&tr.img_rect, pt)) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    if (msg == WM_ERASEBKGND)
        return 1;

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================
// 布局：固定元素扣除后，剩余空间平分给摘要和输入框
// ============================================================

static void layout(int client_w, int client_h)
{
    const int margin   = scale(12);
    const int gap      = scale(4);
    const int label_h  = scale(20);
    const int button_h = scale(32);
    const int button_w = scale(160);
    const int preview_h = g.images.empty() ? 0 : scale(THUMB_SIZE + THUMB_GAP * 2);

    int content_w = client_w - margin * 2;

    // 固定高度：两个标签 + 标签间距 + 按钮 + 间距 + 预览
    int fixed_h = label_h * 2 + gap * 2 + button_h + margin * 4 + preview_h;
    if (preview_h > 0) fixed_h += gap;
    int flex_h = client_h - fixed_h;

    int summary_h = flex_h / 4;
    int edit_h    = flex_h - summary_h;

    int y = margin;

    MoveWindow(g.hwnd_summary_label, margin, y, content_w, label_h, TRUE);
    y += label_h + gap;

    MoveWindow(g.hwnd_summary, margin, y, content_w, summary_h, TRUE);
    y += summary_h + margin;

    MoveWindow(g.hwnd_input_label, margin, y, content_w, label_h, TRUE);
    y += label_h + gap;

    MoveWindow(g.hwnd_edit, margin, y, content_w, edit_h, TRUE);
    y += edit_h;

    // 图片预览区域（有图片时显示）
    if (preview_h > 0) {
        y += gap;
        MoveWindow(g.hwnd_preview, margin, y, content_w, preview_h, TRUE);
        ShowWindow(g.hwnd_preview, SW_SHOW);
        y += preview_h;
    } else {
        ShowWindow(g.hwnd_preview, SW_HIDE);
    }

    y += margin;
    MoveWindow(g.hwnd_submit, client_w - margin - button_w, y, button_w, button_h, TRUE);
}

// ============================================================
// 编辑框子类 - Ctrl+Enter 提交、文件粘贴
// ============================================================

static LRESULT CALLBACK edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN &&
        (GetKeyState(VK_CONTROL) & 0x8000) && !g.expired) {
        submit_and_close();
        return 0;
    }
    if (msg == WM_PASTE) {
        if (IsClipboardFormatAvailable(CF_HDROP)) {
            if (!OpenClipboard(hwnd))
                return 0;
            HDROP hDrop = (HDROP)GetClipboardData(CF_HDROP);
            if (hDrop)
                insert_file_paths(hDrop, hwnd);
            CloseClipboard();
            return 0;
        }
        // 粘贴位图：保存为 PNG，在编辑框插入占位符，并更新预览
        if (IsClipboardFormatAvailable(CF_BITMAP)) {
            if (!OpenClipboard(hwnd))
                return 0;
            HBITMAP hBitmap = (HBITMAP)GetClipboardData(CF_BITMAP);
            if (hBitmap) {
                fs::path img_path = save_clipboard_bitmap_as_png(hBitmap);
                if (!img_path.empty()) {
                    g.images.push_back(img_path);

                    // 触发布局更新以显示/扩展预览区域
                    RECT rc;
                    GetClientRect(g.hwnd_main, &rc);
                    layout(rc.right, rc.bottom);
                    InvalidateRect(g.hwnd_preview, nullptr, TRUE);
                }
            }
            CloseClipboard();
            return 0;
        }
    }
    return CallWindowProcW(g_orig_edit_proc, hwnd, msg, wp, lp);
}

// ============================================================
// 窗口过程
// ============================================================

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_CREATE: {
        g.hwnd_main = hwnd;

        NONCLIENTMETRICSW ncm = {};
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        g.font = CreateFontIndirectW(&ncm.lfMessageFont);

        auto create_ctrl = [&](const wchar_t* cls, const wchar_t* text,
                               DWORD style, DWORD ex_style = 0) {
            HWND h = CreateWindowExW(
                ex_style, cls, text, WS_CHILD | WS_VISIBLE | style,
                0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr
            );
            SendMessageW(h, WM_SETFONT, (WPARAM)g.font, TRUE);
            return h;
        };

        g.hwnd_summary_label = create_ctrl(L"STATIC", L"Summary:", 0);
        g.hwnd_summary = create_ctrl(L"EDIT", g.summary.c_str(),
            WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            WS_EX_CLIENTEDGE);

        g.hwnd_input_label = create_ctrl(L"STATIC", L"Feedback:", 0);
        g.hwnd_edit = create_ctrl(L"EDIT", L"",
            WS_VSCROLL | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            WS_EX_CLIENTEDGE);

        g_orig_edit_proc = (WNDPROC)SetWindowLongPtrW(
            g.hwnd_edit, GWLP_WNDPROC, (LONG_PTR)edit_proc
        );

        g.hwnd_submit = create_ctrl(L"BUTTON", L"Send (Ctrl+Enter)",
            WS_TABSTOP | BS_DEFPUSHBUTTON);

        // 图片预览区域（初始隐藏）
        g.hwnd_preview = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"FeedbackPreviewClass", L"",
            WS_CHILD, // 不加 WS_VISIBLE，初始隐藏
            0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr
        );

        DragAcceptFiles(hwnd, TRUE);

        RECT rc;
        GetClientRect(hwnd, &rc);
        layout(rc.right, rc.bottom);
        return 0;
    }

    case WM_SIZE:
        layout(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED && (HWND)lp == g.hwnd_submit)
            submit_and_close();
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wp;
        insert_file_paths(hDrop, g.hwnd_edit);
        DragFinish(hDrop);
        return 0;
    }

    case WM_COPYDATA: {
        auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
        if (!cds || cds->dwData != COPYDATA_UPDATE_SUMMARY || !cds->lpData || cds->cbData == 0)
            break;

        // 数据格式：summary\0result_file\0
        const char* data = static_cast<const char*>(cds->lpData);
        const char* end  = data + cds->cbData;

        const char* nul1 = static_cast<const char*>(memchr(data, '\0', cds->cbData));
        if (!nul1 || nul1 + 1 >= end)
            break;

        std::string summary_utf8(data, nul1 - data);
        const char* second = nul1 + 1;
        const char* nul2 = static_cast<const char*>(memchr(second, '\0', end - second));
        std::string result_utf8(second, nul2 ? (nul2 - second) : (end - second));

        g.summary     = utf8_to_wide(summary_utf8);
        g.result_file = result_utf8;
        g.expired     = false;
        g.images.clear();

        SetWindowTextW(g.hwnd_summary, g.summary.c_str());
        SetWindowTextW(g.hwnd_main, L"Interactive Feedback MCP");
        SetWindowTextW(g.hwnd_submit, L"Send (Ctrl+Enter)");

        // 更新布局以隐藏预览区域
        RECT rc;
        GetClientRect(g.hwnd_main, &rc);
        layout(rc.right, rc.bottom);

        // 闪烁任务栏，提示用户有新内容
        FLASHWINFO fi = {};
        fi.cbSize  = sizeof(fi);
        fi.hwnd    = g.hwnd_main;
        fi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
        fi.uCount  = 3;
        FlashWindowEx(&fi);
        return TRUE;
    }

    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE)
            SetFocus(g.hwnd_edit);
        return 0;

    case WM_FEEDBACK_CANCELLED:
    case WM_FEEDBACK_TIMEOUT:
        if (GetWindowTextLengthW(g.hwnd_edit) == 0) {
            DestroyWindow(hwnd);
        } else {
            g.expired = true;
            if (msg == WM_FEEDBACK_CANCELLED)
                SetWindowTextW(hwnd, L"Interactive Feedback MCP [Cancelled]");
            else
                SetWindowTextW(hwnd, L"Interactive Feedback MCP [Timed Out]");
            SetWindowTextW(g.hwnd_submit, L"Close");
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        // 清理未提交的临时图片文件
        for (auto& p : g.images) {
            std::error_code ec;
            fs::remove(p, ec);
        }
        g.images.clear();
        if (g.font)
            DeleteObject(g.font);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================
// 入口
// ============================================================

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow)
{
    // 初始化 GDI+（用于图片粘贴保存为 PNG）
    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 3) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 1;
    }
    g.summary     = argv[1];
    g.result_file = argv[2];
    LocalFree(argv);

    // 注册图片预览窗口类
    WNDCLASSEXW pcls = {};
    pcls.cbSize        = sizeof(pcls);
    pcls.lpfnWndProc   = preview_proc;
    pcls.hInstance      = hInst;
    pcls.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    pcls.hbrBackground  = (HBRUSH)(COLOR_WINDOW + 1);
    pcls.lpszClassName  = L"FeedbackPreviewClass";
    RegisterClassExW(&pcls);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"FeedbackGuiClass";
    if (!RegisterClassExW(&wc)) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 1;
    }

    int win_w = scale(580);
    int win_h = scale(380);
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);

    CreateWindowExW(
        WS_EX_TOPMOST,
        L"FeedbackGuiClass", L"Interactive Feedback MCP",
        WS_OVERLAPPEDWINDOW,
        (screen_w - win_w) / 2, (screen_h - win_h) / 2, win_w, win_h,
        nullptr, nullptr, hInst, nullptr
    );
    if (!g.hwnd_main) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 1;
    }

    // 不抢焦点地显示窗口（仍保持置顶）
    ShowWindow(g.hwnd_main, SW_SHOWNOACTIVATE);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Gdiplus::GdiplusShutdown(gdiplusToken);
    return (int)msg.wParam;
}
