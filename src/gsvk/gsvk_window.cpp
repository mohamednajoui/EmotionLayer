#include "gsvk_window.hpp"
#include <vector>

namespace emotion {

namespace {
const wchar_t* kClassName = L"EmotionLayer_Win32_Host";
bool g_class_registered = false;
}

LRESULT CALLBACK GsvkWindow::window_proc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<GsvkWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (uMsg) {
        case WM_CLOSE:
            if (self) self->request_close();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_SIZE:
            if (self) {
                self->client_w_ = LOWORD(lParam);
                self->client_h_ = HIWORD(lParam);
            }
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_F11) {
                // Toggle Pantalla Completa
                if (self) {
                    if (!self->is_fullscreen_) {
                        GetWindowRect(hwnd, &self->windowed_rect_);
                        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                        SetWindowPos(hwnd, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_FRAMECHANGED);
                        self->is_fullscreen_ = true;
                    } else {
                        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
                        SetWindowPos(hwnd, HWND_NOTOPMOST,
                            self->windowed_rect_.left, self->windowed_rect_.top,
                            self->windowed_rect_.right - self->windowed_rect_.left,
                            self->windowed_rect_.bottom - self->windowed_rect_.top,
                            SWP_FRAMECHANGED);
                        self->is_fullscreen_ = false;
                    }
                }
                return 0;
            }
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

bool GsvkWindow::create(const std::string& title, u32 width, u32 height) {
    close();

    HINSTANCE hInstance = GetModuleHandleW(nullptr);

    if (!g_class_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(WNDCLASSEXW);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = window_proc;
        wc.hInstance     = hInstance;
        wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return false;
        g_class_registered = true;
    }

    client_w_ = width;
    client_h_ = height;

    RECT wr = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    const int win_w = wr.right - wr.left;
    const int win_h = wr.bottom - wr.top;
    const int posX  = (GetSystemMetrics(SM_CXSCREEN) - win_w) / 2;
    const int posY  = (GetSystemMetrics(SM_CYSCREEN) - win_h) / 2;

    int wide_len = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wide_title(wide_len, 0);
    MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, wide_title.data(), wide_len);

    hwnd_ = CreateWindowExW(
        0,
        kClassName,
        wide_title.data(),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        posX > 0 ? posX : CW_USEDEFAULT,
        posY > 0 ? posY : CW_USEDEFAULT,
        win_w,
        win_h,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!hwnd_) return false;

    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    hdc_ = GetDC(hwnd_);
    should_close_ = false;

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

void GsvkWindow::present_buffer(const u32* rgba_pixels, u32 src_w, u32 src_h) {
    if (!hwnd_ || !hdc_ || !rgba_pixels || should_close_) return;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = static_cast<LONG>(src_w);
    bmi.bmiHeader.biHeight      = -static_cast<LONG>(src_h); // Top-down DIB
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    StretchDIBits(
        hdc_,
        0, 0, client_w_, client_h_,
        0, 0, src_w, src_h,
        rgba_pixels,
        &bmi,
        DIB_RGB_COLORS,
        SRCCOPY
    );
}

bool GsvkWindow::process_events() {
    if (!hwnd_ || should_close_) return false;

    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            should_close_ = true;
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !should_close_;
}

void GsvkWindow::close() {
    if (hdc_ && hwnd_) {
        ReleaseDC(hwnd_, hdc_);
        hdc_ = nullptr;
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    should_close_ = false;
}

} // namespace emotion
