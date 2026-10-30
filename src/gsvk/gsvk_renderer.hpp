#pragma once

#include "../common/types.hpp"
#include "gsvk_enhancements.hpp"
#include "gsvk_window.hpp"
#include <string>
#include <vector>

namespace emotion {

// Registros internos del Graphics Synthesizer (GS) de PlayStation 2
enum class GsReg : u8 {
    PRIM       = 0x00,
    RGBAQ      = 0x01,
    ST         = 0x02,
    UV         = 0x03,
    XYZF2      = 0x04,
    XYZ2       = 0x05,
    TEX0_1     = 0x06,
    TEX0_2     = 0x07,
    CLAMP_1    = 0x08,
    CLAMP_2    = 0x09,
    FOG        = 0x0A,
    XYZF3      = 0x0C,
    XYZ3       = 0x0D,
    XYOFFSET_1 = 0x18,
    XYOFFSET_2 = 0x19,
    PRMODECONT = 0x1A,
    PRMODE     = 0x1B,
    TEXCLUT    = 0x1C,
    SCANMSK    = 0x22,
    MIPTBP1_1  = 0x34,
    MIPTBP1_2  = 0x35,
    MIPTBP2_1  = 0x36,
    MIPTBP2_2  = 0x37,
    TEXFLUSH   = 0x3F,
    SCISSOR_1  = 0x40,
    SCISSOR_2  = 0x41,
    ALPHA_1    = 0x42,
    ALPHA_2    = 0x43,
    DIMX       = 0x44,
    DTHE       = 0x45,
    COLCLAMP   = 0x46,
    TEST_1     = 0x47,
    TEST_2     = 0x48,
    PABE       = 0x49,
    FBA_1      = 0x4A,
    FBA_2      = 0x4B,
    FRAME_1    = 0x4C,
    FRAME_2    = 0x4D,
    ZBUF_1     = 0x4E,
    ZBUF_2     = 0x4F,
    BITBLTBUF  = 0x50,
    TRXPOS     = 0x51,
    TRXREG     = 0x52,
    TRXDIR     = 0x53,
    HWREG      = 0x54
};

// Tipos de primitivas geométricas del Graphics Synthesizer
enum class GsPrimType : u8 {
    Point         = 0,
    Line          = 1,
    LineStrip     = 2,
    Triangle      = 3,
    TriangleStrip = 4,
    TriangleFan   = 5,
    Sprite        = 6
};

struct GsVertex {
    f32 x = 0.0f; // Coordenada X en píxeles de pantalla
    f32 y = 0.0f; // Coordenada Y en píxeles de pantalla
    u32 z = 0;    // Profundidad Z de 32 bits
    u8  r = 0, g = 0, b = 0, a = 0x80;
    f32 s = 0.0f, t = 0.0f, q = 1.0f;
    u16 u = 0, v = 0;
};

struct GsStats {
    u32 gif_packets_processed = 0;
    u32 primitives_drawn      = 0;
    u32 vertices_kicked       = 0;
    u32 vram_uploads          = 0;
    u32 lit_pixels_count      = 0;
    u32 frame_checksum        = 0;
};

// Motor Gráfico "GSVK" (GS-to-Vulkan / Rasterizador Acelerado de los 4 MB de eDRAM de PS2)
class GsvkRenderer {
public:
    static constexpr u32 kEdramSize   = 4 * 1024 * 1024; // 4 MB de eDRAM del Graphics Synthesizer
    static constexpr u32 kDefaultW    = 640;
    static constexpr u32 kDefaultH    = 480;

    GsvkRenderer() noexcept;
    ~GsvkRenderer() noexcept;

    // Inicializa el subsistema gráfico, detecta GPU vía Vulkan 1.3 (vulkan-1.dll) y reserva los 4 MB de eDRAM
    bool initialize();

    // Procesa una lista de paquetes GIF de 128 bits (PATH1 desde VU1 XGKICK, PATH2 desde VIF1 DIRECT, PATH3 desde GIF DMA)
    void submit_gif_packet(const u8* packet_ptr, u32 size_bytes, u8 path_index);

    // Escribe directamente un registro interno del GS (AD mode o registros mapeados)
    void write_gs_register(u8 reg_addr, u64 value) noexcept;



    // Registros Privilegiados del Graphics Synthesizer (0x12000000..0x120000A0: PMODE, DISPFB1/2, DISPLAY1/2)
    void write_privileged_register(u32 addr, u64 value) noexcept;
    u64  pmode() const noexcept { return pmode_reg_; }
    u64  dispfb1() const noexcept { return dispfb1_reg_; }
    u64  display1() const noexcept { return display1_reg_; }
    u64  dispfb2() const noexcept { return dispfb2_reg_; }
    u64  display2() const noexcept { return display2_reg_; }

    // Resuelve el framebuffer activo desde eDRAM según DISPFB1/DISPFB2 (formato PSMCT32/24/16/16S y tamaño)
    bool resolve_display_framebuffer(std::vector<u32>& out_pixels, u32& out_w, u32& out_h) const noexcept;

    // Presenta el frame actual: exporta a BMP nativo y opcionalmente abre una ventana Win32
    bool present_frame(const std::string& dump_bmp_path, bool show_win32_window = false, bool throttle = false);

    // --- Mejoras Nativas de Calidad de Vida (Fase 5) ---
    void set_resolution_scale(u32 scale);
    u32  resolution_scale() const noexcept { return res_scale_; }
    void set_aspect_ratio(AspectRatio ar) noexcept { aspect_ratio_ = ar; }
    AspectRatio aspect_ratio() const noexcept { return aspect_ratio_; }
    void set_target_framerate(u32 fps) noexcept { frame_limiter_.set_target_fps(fps); }
    u32  target_framerate() const noexcept { return frame_limiter_.target_fps(); }
    u32  render_width() const noexcept { return width_; }
    u32  render_height() const noexcept { return height_; }
    HdTextureManager& hd_textures() noexcept { return hd_textures_; }
    const HdTextureManager& hd_textures() const noexcept { return hd_textures_; }
    HighPrecisionFrameLimiter& frame_limiter() noexcept { return frame_limiter_; }
    const HighPrecisionFrameLimiter& frame_limiter() const noexcept { return frame_limiter_; }
    GsvkWindow& window() noexcept { return window_; }
    const GsvkWindow& window() const noexcept { return window_; }
    void enable_realtime_window(bool enable = true) noexcept { window_enabled_ = enable; }
    bool is_realtime_window_enabled() const noexcept { return window_enabled_; }

    const std::string& gpu_device_name() const noexcept { return gpu_name_; }
    bool is_vulkan_available() const noexcept { return vulkan_available_; }
    const GsStats& stats() const noexcept { return stats_; }
    const std::vector<u8>& edram() const noexcept { return edram_; }
    std::vector<u8>& edram() noexcept { return edram_; }
    u32  sample_texture(f32 u_norm, f32 v_norm) const noexcept;

    static GsvkRenderer* active_instance() noexcept { return active_instance_; }
    static void set_active_instance(GsvkRenderer* inst) noexcept { active_instance_ = inst; }
    static void register_instance(GsvkRenderer* inst) noexcept;
    static void unregister_instance(GsvkRenderer* inst) noexcept;

private:
    void probe_vulkan_1_3_device();
    void vertex_kick(u64 xyz_raw, bool drawing_kick);
    void rasterize_triangle(const GsVertex& v0, const GsVertex& v1, const GsVertex& v2);
    void rasterize_sprite(const GsVertex& v0, const GsVertex& v1);
    void upload_image_qword(const r128& qword) noexcept;
    u32  blend_ps2_alpha(u32 src_rgba, u32 dst_rgba) const noexcept;
    void update_prim_attributes() noexcept;

    enum class GifState : u8 {
        Tag,
        Packed,
        Reglist,
        Image
    };

    struct GifPathState {
        GifState state      = GifState::Tag;
        u32      nloop      = 0;
        u32      loop_count = 0;
        u8       flg        = 0;
        u32      nregs      = 0;
        u32      reg_idx    = 0;
        u64      tag_hi     = 0;
        bool     eop        = false;
    };

    GifPathState path_state_[4]{};

    std::vector<u8>  edram_;         // 4 MB de memoria de vídeo dedicada del GS
    std::vector<u32> color_buffer_;  // Framebuffer activo de alta velocidad (640x480 RGBA8)
    std::vector<u32> depth_buffer_;  // Z-Buffer de 32 bits (640x480)

    u32 width_         = kDefaultW;
    u32 height_        = kDefaultH;
    f32 ofx_           = 0.0f;       // Offset X en subpíxeles (XYOFFSET_1)
    f32 ofy_           = 0.0f;       // Offset Y en subpíxeles (XYOFFSET_1)
    u32 scissor_x0_    = 0, scissor_x1_ = kDefaultW - 1;
    u32 scissor_y0_    = 0, scissor_y1_ = kDefaultH - 1;

    // Estado actual de primitiva y vértice en construcción
    u64        prim_reg_      = 0;
    u64        prmode_reg_    = 0;
    u8         prmodecont_ac_ = 0;
    GsPrimType prim_type_   = GsPrimType::Triangle;
    bool       iip_gouraud_ = true;
    bool       tme_textured_ = false;
    bool       fge_fog_     = false;
    bool       abe_alpha_   = false;
    bool       aa1_         = false;
    bool       fst_uv_mode_ = false;
    u8         ctxt_        = 0;

    // Registros de Contexto 1 y 2
    u64        tex0_1_reg_  = 0;
    u64        tex0_2_reg_  = 0;
    u64        clamp_1_reg_ = 0;
    u64        clamp_2_reg_ = 0;
    u64        alpha_1_reg_ = 0;
    u64        alpha_2_reg_ = 0;
    u64        frame_1_reg_ = 0;
    u64        frame_2_reg_ = 0;
    u64        zbuf_1_reg_  = 0;
    u64        zbuf_2_reg_  = 0;


    // Registros Privilegiados de Display del GS
    u64        pmode_reg_    = 0;
    u64        smode2_reg_   = 0;
    u64        dispfb1_reg_  = 0;
    u64        display1_reg_ = 0;
    u64        dispfb2_reg_  = 0;
    u64        display2_reg_ = 0;
    mutable std::vector<u32> presentation_buffer_;

    // Tests (ATE, ATST, AREF, AFAIL, ZTE, ZTST)
    u64        test_1_reg_  = 0;
    u64        test_2_reg_  = 0;
    bool       test_ate_    = false;
    u8         test_atst_   = 1; // 1 = ALWAYS
    u8         test_aref_   = 0;
    u8         test_afail_  = 0;
    bool       test_zte_    = true;
    u8         test_ztst_   = 2; // 2 = GEQUAL

    GsVertex   cur_vtx_{};
    GsVertex   vtx_queue_[3]{};
    u32        vtx_count_ = 0;

    // Estado de transferencia Host-to-Local / Local-to-Local (BITBLTBUF / TRXPOS / TRXREG / TRXDIR)
    u32 trx_sbp_  = 0;
    u32 trx_sbw_  = 0;
    u32 trx_spsm_ = 0;
    u32 trx_dbp_  = 0; // Destination Base Pointer (en bloques de 64 words = 256 bytes)
    u32 trx_dbw_  = 0; // Buffer width en unidades de 64 píxeles
    u32 trx_dpsm_ = 0; // Destination Pixel Storage Mode (0=PSMCT32, 1=PSMCT24, 2=PSMCT16, 19=PSMT8, 20=PSMT4)
    u32 trx_ssax_ = 0;
    u32 trx_ssay_ = 0;
    u32 trx_dsax_ = 0; // Destination start X
    u32 trx_dsay_ = 0; // Destination start Y
    u32 trx_rrw_  = 0; // Ancho de transferencia en píxeles
    u32 trx_rrh_  = 0; // Alto de transferencia en píxeles
    u32 trx_dir_  = 0; // Dirección de transferencia (0=Host->Local)
    u32 trx_x_    = 0;
    u32 trx_y_    = 0;

    bool        vulkan_available_ = false;
    std::string gpu_name_         = "Software AVX2 GS Rasterizer";
    GsStats     stats_{};

    // Mejoras de Fase 5
    u32                       res_scale_    = 1;
    AspectRatio               aspect_ratio_ = AspectRatio::Standard_4_3;
    HdTextureManager          hd_textures_;
    HighPrecisionFrameLimiter frame_limiter_;
    GsvkWindow                window_;
    bool                      window_enabled_ = false;

    static std::vector<GsvkRenderer*> instances_;
    static GsvkRenderer* active_instance_;
};

} // namespace emotion
