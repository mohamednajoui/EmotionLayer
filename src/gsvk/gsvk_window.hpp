#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../common/types.hpp"
#include <string>
#include <windows.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace emotion {

// Ventana Interactiva Nativa de Windows para Presentación en Tiempo Real
class GsvkWindow {
public:
    GsvkWindow() noexcept = default;
    ~GsvkWindow() { close(); }

    // Crea la ventana interactiva Win32
    bool create(const std::string& title, u32 width, u32 height);

    // Presenta el búfer de píxeles RGBA8 de alta resolución en la ventana
    void present_buffer(const u32* rgba_pixels, u32 src_w, u32 src_h);

    // Procesa eventos de Windows (teclado, ratón, redimensionado, cierre)
    // Retorna false si el usuario solicitó cerrar la ventana
    bool process_events();

    void close();

    bool is_open() const noexcept { return hwnd_ != nullptr && !should_close_; }
    HWND hwnd() const noexcept { return hwnd_; }
    u32 client_width() const noexcept { return client_w_; }
    u32 client_height() const noexcept { return client_h_; }

    void request_close() noexcept { should_close_ = true; }

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    HWND hwnd_          = nullptr;
    HDC  hdc_           = nullptr;
    u32  client_w_      = 0;
    u32  client_h_      = 0;
    bool should_close_  = false;
    bool is_fullscreen_ = false;
    RECT windowed_rect_{};
};

} // namespace emotion
