#include "gsvk_renderer.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

// Definiciones mínimas ABI de Vulkan 1.3 para detectar e inicializar la GPU física real desde vulkan-1.dll
// sin requerir el instalador externo del LunarG Vulkan SDK.
namespace {

using VkResult = int;
using VkStructureType = int;
using VkFlags = emotion::u32;
using VkInstance = void*;
using VkPhysicalDevice = void*;

#pragma pack(push, 4)
struct VkApplicationInfo {
    VkStructureType sType;
    const void*     pNext;
    const char*     pApplicationName;
    emotion::u32    applicationVersion;
    const char*     pEngineName;
    emotion::u32    engineVersion;
    emotion::u32    apiVersion;
};

struct VkInstanceCreateInfo {
    VkStructureType          sType;
    const void*              pNext;
    VkFlags                  flags;
    const VkApplicationInfo* pApplicationInfo;
    emotion::u32             enabledLayerCount;
    const char* const*       ppEnabledLayerNames;
    emotion::u32             enabledExtensionCount;
    const char* const*       ppEnabledExtensionNames;
};

struct VkPhysicalDeviceLimits {
    emotion::u8 opaque[504];
};

struct VkPhysicalDeviceSparseProperties {
    emotion::u8 opaque[20];
};

struct VkPhysicalDeviceProperties {
    emotion::u32                     apiVersion;
    emotion::u32                     driverVersion;
    emotion::u32                     vendorID;
    emotion::u32                     deviceID;
    emotion::u32                     deviceType;
    char                             deviceName[256];
    emotion::u8                      pipelineCacheUUID[16];
    VkPhysicalDeviceLimits           limits;
    VkPhysicalDeviceSparseProperties sparseProperties;
};
#pragma pack(pop)

using PFN_vkCreateInstance = VkResult(WINAPI*)(const VkInstanceCreateInfo*, const void*, VkInstance*);
using PFN_vkDestroyInstance = void(WINAPI*)(VkInstance, const void*);
using PFN_vkEnumeratePhysicalDevices = VkResult(WINAPI*)(VkInstance, emotion::u32*, VkPhysicalDevice*);
using PFN_vkGetPhysicalDeviceProperties = void(WINAPI*)(VkPhysicalDevice, VkPhysicalDeviceProperties*);

} // namespace

namespace emotion {

namespace gs_layout {

static constexpr int kBlockSwizzle32[4][8] = {
    {  0,  1,  4,  5, 16, 17, 20, 21 },
    {  2,  3,  6,  7, 18, 19, 22, 23 },
    {  8,  9, 12, 13, 24, 25, 28, 29 },
    { 10, 11, 14, 15, 26, 27, 30, 31 }
};

static constexpr int kColumnSwizzle32[2][8] = {
    { 0, 1, 4, 5,  8,  9, 12, 13 },
    { 2, 3, 6, 7, 10, 11, 14, 15 }
};

static constexpr int kBlockSwizzle16[8][4] = {
    {  0,  2,  8, 10 },
    {  1,  3,  9, 11 },
    {  4,  6, 12, 14 },
    {  5,  7, 13, 15 },
    { 16, 18, 24, 26 },
    { 17, 19, 25, 27 },
    { 20, 22, 28, 30 },
    { 21, 23, 29, 31 }
};

static constexpr int kColumnSwizzle16[2][16] = {
    { 0, 2,  8, 10, 16, 18, 24, 26, 1, 3,  9, 11, 17, 19, 25, 27 },
    { 4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31 }
};

// Calcula el offset en bytes dentro de un buffer en eDRAM organizado en páginas/bloques (PSMCT32 / PSMCT24)
static inline emotion::u32 calc_edram_offset_psmct32(emotion::u32 x, emotion::u32 y, emotion::u32 buf_width_pixels) noexcept {
    const emotion::u32 pages_per_row = (buf_width_pixels > 0) ? (buf_width_pixels / 64U) : 10U;
    const emotion::u32 page_x = x / 64U;
    const emotion::u32 page_y = y / 32U;
    const emotion::u32 page_num = page_x + page_y * (pages_per_row > 0 ? pages_per_row : 1U);

    const emotion::u32 local_x = x % 64U;
    const emotion::u32 local_y = y % 32U;

    const emotion::u32 block_x = local_x / 8U;
    const emotion::u32 block_y = local_y / 8U;
    const emotion::u32 block_num = static_cast<emotion::u32>(kBlockSwizzle32[block_y][block_x]);

    const emotion::u32 rem_x = local_x % 8U;
    const emotion::u32 rem_y = local_y % 8U;
    const emotion::u32 col_num = rem_y / 2U;
    const emotion::u32 col_y = rem_y % 2U;
    const emotion::u32 col_x = rem_x;
    const emotion::u32 word_in_col = static_cast<emotion::u32>(kColumnSwizzle32[col_y][col_x]);

    return (page_num * 8192U) + (block_num * 256U) + (col_num * 64U) + (word_in_col * 4U);
}

// Calcula el offset en bytes dentro de un buffer en eDRAM organizado en páginas/bloques (PSMCT16)
static inline emotion::u32 calc_edram_offset_psmct16(emotion::u32 x, emotion::u32 y, emotion::u32 buf_width_pixels) noexcept {
    const emotion::u32 pages_per_row = (buf_width_pixels > 0) ? (buf_width_pixels / 64U) : 10U;
    const emotion::u32 page_x = x / 64U;
    const emotion::u32 page_y = y / 64U;
    const emotion::u32 page_num = page_x + page_y * (pages_per_row > 0 ? pages_per_row : 1U);

    const emotion::u32 local_x = x % 64U;
    const emotion::u32 local_y = y % 64U;

    const emotion::u32 block_x = local_x / 16U;
    const emotion::u32 block_y = local_y / 8U;
    const emotion::u32 block_num = static_cast<emotion::u32>(kBlockSwizzle16[block_y][block_x]);

    const emotion::u32 rem_x = local_x % 16U;
    const emotion::u32 rem_y = local_y % 8U;
    const emotion::u32 col_num = rem_y / 2U;
    const emotion::u32 col_y = rem_y % 2U;
    const emotion::u32 col_x = rem_x;
    const emotion::u32 hword_in_col = static_cast<emotion::u32>(kColumnSwizzle16[col_y][col_x]);

    return (page_num * 8192U) + (block_num * 256U) + (col_num * 64U) + (hword_in_col * 2U);
}

static constexpr int kBlockSwizzle16S[8][4] = {
    {  0,  2, 16, 18 },
    {  1,  3, 17, 19 },
    {  8, 10, 24, 26 },
    {  9, 11, 25, 27 },
    {  4,  6, 20, 22 },
    {  5,  7, 21, 23 },
    { 12, 14, 28, 30 },
    { 13, 15, 29, 31 }
};

// Calcula el offset en bytes dentro de un buffer en eDRAM organizado en páginas/bloques (PSMCT16S)
static inline emotion::u32 calc_edram_offset_psmct16s(emotion::u32 x, emotion::u32 y, emotion::u32 buf_width_pixels) noexcept {
    const emotion::u32 pages_per_row = (buf_width_pixels > 0) ? (buf_width_pixels / 64U) : 10U;
    const emotion::u32 page_x = x / 64U;
    const emotion::u32 page_y = y / 64U;
    const emotion::u32 page_num = page_x + page_y * (pages_per_row > 0 ? pages_per_row : 1U);

    const emotion::u32 local_x = x % 64U;
    const emotion::u32 local_y = y % 64U;

    const emotion::u32 block_x = local_x / 16U;
    const emotion::u32 block_y = local_y / 8U;
    const emotion::u32 block_num = static_cast<emotion::u32>(kBlockSwizzle16S[block_y][block_x]);

    const emotion::u32 rem_x = local_x % 16U;
    const emotion::u32 rem_y = local_y % 8U;
    const emotion::u32 col_num = rem_y / 2U;
    const emotion::u32 col_y = rem_y % 2U;
    const emotion::u32 col_x = rem_x;
    const emotion::u32 hword_in_col = static_cast<emotion::u32>(kColumnSwizzle16[col_y][col_x]);

    return (page_num * 8192U) + (block_num * 256U) + (col_num * 64U) + (hword_in_col * 2U);
}
} // namespace gs_layout

std::vector<GsvkRenderer*> GsvkRenderer::instances_;
GsvkRenderer* GsvkRenderer::active_instance_ = nullptr;

void GsvkRenderer::register_instance(GsvkRenderer* inst) noexcept {
    if (!inst) return;
    auto it = std::find(instances_.begin(), instances_.end(), inst);
    if (it == instances_.end()) {
        instances_.push_back(inst);
    }
    active_instance_ = inst;
}

void GsvkRenderer::unregister_instance(GsvkRenderer* inst) noexcept {
    if (!inst) return;
    auto it = std::find(instances_.begin(), instances_.end(), inst);
    if (it != instances_.end()) {
        instances_.erase(it);
    }
    if (active_instance_ == inst) {
        active_instance_ = instances_.empty() ? nullptr : instances_.back();
    }
}

GsvkRenderer::GsvkRenderer() noexcept {
    register_instance(this);
}

GsvkRenderer::~GsvkRenderer() noexcept {
    unregister_instance(this);
}

bool GsvkRenderer::initialize() {
    edram_.assign(kEdramSize, 0);
    color_buffer_.assign(width_ * height_, 0xFF140C1CU); // Fondo azul oscuro característico de la BIOS de PS2
    depth_buffer_.assign(width_ * height_, 0U);
    stats_ = GsStats{};

    probe_vulkan_1_3_device();
    return true;
}

void GsvkRenderer::set_resolution_scale(u32 scale) {
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    res_scale_ = scale;
    width_ = kDefaultW * res_scale_;
    height_ = kDefaultH * res_scale_;
    color_buffer_.assign(width_ * height_, 0xFF140C1CU);
    depth_buffer_.assign(width_ * height_, 0U);
    scissor_x0_ = 0;
    scissor_x1_ = width_ - 1;
    scissor_y0_ = 0;
    scissor_y1_ = height_ - 1;
}

void GsvkRenderer::probe_vulkan_1_3_device() {
    HMODULE hVk = LoadLibraryW(L"vulkan-1.dll");
    if (!hVk) return;

    auto pCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(GetProcAddress(hVk, "vkCreateInstance"));
    auto pDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(GetProcAddress(hVk, "vkDestroyInstance"));
    auto pEnumDevs = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(GetProcAddress(hVk, "vkEnumeratePhysicalDevices"));
    auto pGetProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(GetProcAddress(hVk, "vkGetPhysicalDeviceProperties"));

    if (!pCreateInstance || !pDestroyInstance || !pEnumDevs || !pGetProps) return;

    VkApplicationInfo app{};
    app.sType              = 0; // VK_STRUCTURE_TYPE_APPLICATION_INFO
    app.pApplicationName   = "EmotionLayer PS2 Native Port";
    app.applicationVersion = (1U << 22);
    app.pEngineName        = "GSVK";
    app.engineVersion      = (1U << 22);
    app.apiVersion         = (1U << 22) | (3U << 12); // VK_API_VERSION_1_3

    VkInstanceCreateInfo ci{};
    ci.sType            = 1; // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
    ci.pApplicationInfo = &app;

    VkInstance instance = nullptr;
    if (pCreateInstance(&ci, nullptr, &instance) != 0) {
        // Fallback a Vulkan 1.1 si el driver no soporta 1.3
        app.apiVersion = (1U << 22) | (1U << 12);
        if (pCreateInstance(&ci, nullptr, &instance) != 0) return;
    }

    u32 dev_count = 0;
    if (pEnumDevs(instance, &dev_count, nullptr) == 0 && dev_count > 0) {
        std::vector<VkPhysicalDevice> devs(dev_count);
        if (pEnumDevs(instance, &dev_count, devs.data()) == 0) {
            VkPhysicalDeviceProperties props{};
            pGetProps(devs[0], &props);
            vulkan_available_ = true;
            gpu_name_ = std::string(props.deviceName) + " (Vulkan "
                + std::to_string((props.apiVersion >> 22) & 0x7FU) + "."
                + std::to_string((props.apiVersion >> 12) & 0x3FFU) + ")";
        }
    }
    pDestroyInstance(instance, nullptr);
}

void GsvkRenderer::update_prim_attributes() noexcept {
    const u64 attr = (prmodecont_ac_ != 0) ? prmode_reg_ : prim_reg_;
    iip_gouraud_  = ((attr >> 3) & 0x01U) != 0;
    tme_textured_ = ((attr >> 4) & 0x01U) != 0;
    fge_fog_      = ((attr >> 5) & 0x01U) != 0;
    abe_alpha_    = ((attr >> 6) & 0x01U) != 0;
    aa1_          = ((attr >> 7) & 0x01U) != 0;
    fst_uv_mode_  = ((attr >> 8) & 0x01U) != 0;
    ctxt_         = static_cast<u8>((attr >> 9) & 0x01U);
}

void GsvkRenderer::write_gs_register(u8 reg_addr, u64 value) noexcept {
    switch (static_cast<GsReg>(reg_addr)) {
        case GsReg::PRIM: {
            prim_reg_     = value;
            prim_type_    = static_cast<GsPrimType>(value & 0x07U);
            update_prim_attributes();
            vtx_count_    = 0;
            static u32 log_prim = 0;
            if (log_prim++ < 20) {
                std::printf("[GS PRIM] type=%u tex=%d gouraud=%d alpha=%d fst=%d\n",
                    static_cast<u32>(prim_type_), tme_textured_, iip_gouraud_, abe_alpha_, fst_uv_mode_);
                std::fflush(stdout);
            }
            break;
        }
        case GsReg::PRMODECONT: {
            prmodecont_ac_ = static_cast<u8>(value & 0x01U);
            update_prim_attributes();
            break;
        }
        case GsReg::PRMODE: {
            prmode_reg_ = value;
            update_prim_attributes();
            break;
        }
        case GsReg::RGBAQ: {
            cur_vtx_.r = static_cast<u8>(value & 0xFFU);
            cur_vtx_.g = static_cast<u8>((value >> 8)  & 0xFFU);
            cur_vtx_.b = static_cast<u8>((value >> 16) & 0xFFU);
            cur_vtx_.a = static_cast<u8>((value >> 24) & 0xFFU);
            u32 q_bits = static_cast<u32>((value >> 32) & 0xFFFFFFFFULL);
            if (q_bits != 0) {
                std::memcpy(&cur_vtx_.q, &q_bits, sizeof(f32));
            } else {
                cur_vtx_.q = 1.0f;
            }
            break;
        }
        case GsReg::ST: {
            const u32 s_bits = static_cast<u32>(value & 0xFFFFFFFFULL);
            const u32 t_bits = static_cast<u32>((value >> 32) & 0xFFFFFFFFULL);
            std::memcpy(&cur_vtx_.s, &s_bits, sizeof(f32));
            std::memcpy(&cur_vtx_.t, &t_bits, sizeof(f32));
            break;
        }
        case GsReg::UV: {
            cur_vtx_.u = static_cast<u16>(value & 0x3FFFU);
            cur_vtx_.v = static_cast<u16>((value >> 16) & 0x3FFFU);
            cur_vtx_.s = static_cast<f32>(cur_vtx_.u) / 16.0f;
            cur_vtx_.t = static_cast<f32>(cur_vtx_.v) / 16.0f;
            break;
        }
        case GsReg::XYZ2:
        case GsReg::XYZF2: {
            vertex_kick(value, true);
            break;
        }
        case GsReg::XYZ3:
        case GsReg::XYZF3: {
            vertex_kick(value, false);
            break;
        }
        case GsReg::XYOFFSET_1:
        case GsReg::XYOFFSET_2: {
            const u16 raw_x = static_cast<u16>(value & 0xFFFFU);
            const u16 raw_y = static_cast<u16>((value >> 32) & 0xFFFFU);
            ofx_ = static_cast<f32>(raw_x) / 16.0f;
            ofy_ = static_cast<f32>(raw_y) / 16.0f;
            break;
        }
        case GsReg::SCISSOR_1:
        case GsReg::SCISSOR_2: {
            scissor_x0_ = static_cast<u32>(value & 0x7FFU) * res_scale_;
            scissor_x1_ = (static_cast<u32>((value >> 16) & 0x7FFU) + 1U) * res_scale_ - 1U;
            scissor_y0_ = static_cast<u32>((value >> 32) & 0x7FFU) * res_scale_;
            scissor_y1_ = (static_cast<u32>((value >> 48) & 0x7FFU) + 1U) * res_scale_ - 1U;
            break;
        }
        case GsReg::ALPHA_1: {
            alpha_1_reg_ = value;
            break;
        }
        case GsReg::ALPHA_2: {
            alpha_2_reg_ = value;
            break;
        }
        case GsReg::TEST_1:
        case GsReg::TEST_2: {
            if (static_cast<GsReg>(reg_addr) == GsReg::TEST_1) test_1_reg_ = value;
            else test_2_reg_ = value;
            test_ate_   = (value & 0x01U) != 0;
            test_atst_  = static_cast<u8>((value >> 1) & 0x07U);
            test_aref_  = static_cast<u8>((value >> 4) & 0xFFU);
            test_afail_ = static_cast<u8>((value >> 12) & 0x03U);
            test_zte_   = ((value >> 16) & 0x01U) != 0;
            test_ztst_  = static_cast<u8>((value >> 17) & 0x03U);
            break;
        }
        case GsReg::FRAME_1: {
            frame_1_reg_ = value;
            static u32 log_f1 = 0;
            if (log_f1++ < 5) {
                std::printf("[FRAME_1] fbp=%llu fbw=%llu psm=%llu mask=0x%08llX\n",
                    (unsigned long long)(value & 0x1FFULL), (unsigned long long)((value >> 16) & 0x3FULL),
                    (unsigned long long)((value >> 24) & 0x3FULL), (unsigned long long)(value >> 32));
                std::fflush(stdout);
            }
            break;
        }
        case GsReg::FRAME_2: {
            frame_2_reg_ = value;
            static u32 log_f2 = 0;
            if (log_f2++ < 5) {
                std::printf("[FRAME_2] fbp=%llu fbw=%llu psm=%llu mask=0x%08llX\n",
                    (unsigned long long)(value & 0x1FFULL), (unsigned long long)((value >> 16) & 0x3FULL),
                    (unsigned long long)((value >> 24) & 0x3FULL), (unsigned long long)(value >> 32));
                std::fflush(stdout);
            }
            break;
        }
        case GsReg::ZBUF_1: {
            zbuf_1_reg_ = value;
            break;
        }
        case GsReg::ZBUF_2: {
            zbuf_2_reg_ = value;
            break;
        }
        case GsReg::TEX0_1: {
            tex0_1_reg_ = value;
            static u32 log_t1 = 0;
            if (log_t1++ < 10) {
                std::printf("[TEX0_1] tbp=%llu tbw=%llu psm=%llu %llux%llu cbp=%llu cpsm=%llu\n",
                    (unsigned long long)(value & 0x3FFFULL), (unsigned long long)((value >> 14) & 0x3FULL),
                    (unsigned long long)((value >> 20) & 0x3FULL), (unsigned long long)(1ULL << ((value >> 26) & 0xFULL)),
                    (unsigned long long)(1ULL << ((value >> 30) & 0xFULL)), (unsigned long long)((value >> 37) & 0x3FFFULL),
                    (unsigned long long)((value >> 51) & 0xFULL));
                std::fflush(stdout);
            }
            break;
        }
        case GsReg::TEX0_2: {
            tex0_2_reg_ = value;
            static u32 log_t2 = 0;
            if (log_t2++ < 10) {
                std::printf("[TEX0_2] tbp=%llu tbw=%llu psm=%llu %llux%llu cbp=%llu cpsm=%llu\n",
                    (unsigned long long)(value & 0x3FFFULL), (unsigned long long)((value >> 14) & 0x3FULL),
                    (unsigned long long)((value >> 20) & 0x3FULL), (unsigned long long)(1ULL << ((value >> 26) & 0xFULL)),
                    (unsigned long long)(1ULL << ((value >> 30) & 0xFULL)), (unsigned long long)((value >> 37) & 0x3FFFULL),
                    (unsigned long long)((value >> 51) & 0xFULL));
                std::fflush(stdout);
            }
            break;
        }
        case GsReg::CLAMP_1: {
            clamp_1_reg_ = value;
            break;
        }
        case GsReg::CLAMP_2: {
            clamp_2_reg_ = value;
            break;
        }
        case GsReg::BITBLTBUF: {
            trx_sbp_  = static_cast<u32>(value & 0x3FFFU);
            trx_sbw_  = static_cast<u32>((value >> 16) & 0x3FU);
            trx_spsm_ = static_cast<u32>((value >> 24) & 0x3FU);
            trx_dbp_  = static_cast<u32>((value >> 32) & 0x3FFFU);
            trx_dbw_  = static_cast<u32>((value >> 48) & 0x3FU);
            trx_dpsm_ = static_cast<u32>((value >> 56) & 0x3FU);
            break;
        }
        case GsReg::TRXPOS: {
            trx_ssax_ = static_cast<u32>(value & 0x7FFU);
            trx_ssay_ = static_cast<u32>((value >> 16) & 0x7FFU);
            trx_dsax_ = static_cast<u32>((value >> 32) & 0x7FFU);
            trx_dsay_ = static_cast<u32>((value >> 48) & 0x7FFU);
            trx_x_    = trx_dsax_;
            trx_y_    = trx_dsay_;
            break;
        }
        case GsReg::TRXREG: {
            trx_rrw_ = static_cast<u32>(value & 0xFFFU);
            trx_rrh_ = static_cast<u32>((value >> 32) & 0xFFFU);
            trx_x_   = trx_dsax_;
            trx_y_   = trx_dsay_;
            break;
        }
        case GsReg::TRXDIR: {
            trx_dir_ = static_cast<u32>(value & 0x03U);
            if (trx_dir_ == 0) {
                stats_.vram_uploads++;
                trx_x_ = trx_dsax_;
                trx_y_ = trx_dsay_;
                static u32 log_trx = 0;
                if (log_trx++ < 20) {
                    std::printf("[TRXDIR Host->Local] dbp=%u dbw=%u dpsm=%u dsax=%u dsay=%u rrw=%u rrh=%u\n",
                        trx_dbp_, trx_dbw_, trx_dpsm_, trx_dsax_, trx_dsay_, trx_rrw_, trx_rrh_);
                    std::fflush(stdout);
                }
            }
            break;
        }
        default:
            break;
    }
}

void GsvkRenderer::write_privileged_register(u32 addr, u64 value) noexcept {
    switch (addr) {
        case 0x12000000U: // PMODE
            pmode_reg_ = value;
            break;
        case 0x12000020U: // SMODE2
            smode2_reg_ = value;
            break;
        case 0x12000070U: // DISPFB1
            dispfb1_reg_ = value;
            break;
        case 0x12000080U: // DISPLAY1
            display1_reg_ = value;
            break;
        case 0x12000090U: // DISPFB2
            dispfb2_reg_ = value;
            break;
        case 0x120000A0U: // DISPLAY2
            display2_reg_ = value;
            break;
        default:
            break;
    }
}

bool GsvkRenderer::resolve_display_framebuffer(std::vector<u32>& out_pixels, u32& out_w, u32& out_h) const noexcept {
    if (dispfb1_reg_ == 0 && dispfb2_reg_ == 0) {
        out_w = width_;
        out_h = height_;
        out_pixels = color_buffer_;
        return true;
    }

    const u32 rc_mode = static_cast<u32>(pmode_reg_ & 0x03U);
    u64 dispfb = 0;
    u64 display = 0;

    if (rc_mode == 1) {
        dispfb  = dispfb1_reg_;
        display = display1_reg_;
    } else if (rc_mode == 2) {
        dispfb  = dispfb2_reg_;
        display = display2_reg_;
    } else if (rc_mode == 3) {
        if (dispfb1_reg_ != 0 && dispfb2_reg_ == 0) {
            dispfb  = dispfb1_reg_;
            display = display1_reg_;
        } else if (dispfb1_reg_ == 0 && dispfb2_reg_ != 0) {
            dispfb  = dispfb2_reg_;
            display = display2_reg_;
        } else {
            dispfb  = dispfb1_reg_;
            display = display1_reg_;
        }
    } else {
        if (dispfb1_reg_ != 0) {
            dispfb  = dispfb1_reg_;
            display = display1_reg_;
        } else if (dispfb2_reg_ != 0) {
            dispfb  = dispfb2_reg_;
            display = display2_reg_;
        } else {
            out_w = width_;
            out_h = height_;
            out_pixels = color_buffer_;
            return true;
        }
    }

    if (dispfb == 0) {
        out_w = width_;
        out_h = height_;
        out_pixels = color_buffer_;
        return true;
    }

    const u32 fbp = static_cast<u32>(dispfb & 0x1FFULL);
    const u32 fbw = static_cast<u32>((dispfb >> 9) & 0x3FULL);
    const u32 psm = static_cast<u32>((dispfb >> 15) & 0x1FULL);
    const u32 dbx = static_cast<u32>((dispfb >> 32) & 0x7FFULL);
    const u32 dby = static_cast<u32>((dispfb >> 43) & 0x7FFULL);

    u32 disp_w = 0;
    u32 disp_h = 0;
    if (display != 0) {
        const u32 mag_x = static_cast<u32>((display >> 23) & 0x0FULL);
        const u32 mag_y = static_cast<u32>((display >> 27) & 0x03ULL);
        const u32 nw    = static_cast<u32>((display >> 32) & 0xFFFULL);
        const u32 nh    = static_cast<u32>((display >> 44) & 0xFFFULL);
        disp_w = (nw + 1U) / (mag_x + 1U);
        disp_h = (nh + 1U) / (mag_y + 1U);
        if (disp_h > 640U) {
            disp_h /= 2U;
        }
    }

    if (disp_w == 0 || disp_w > 2048U) {
        disp_w = (fbw > 0) ? (fbw * 64U) : kDefaultW;
    }
    if (disp_h == 0 || disp_h > 2048U) {
        disp_h = kDefaultH;
    }

    const u32 pitch = (fbw > 0) ? (fbw * 64U) : disp_w;
    const u32 base_byte = (fbp * 8192U) & (kEdramSize - 1U);

    out_w = disp_w;
    out_h = disp_h;
    out_pixels.resize(static_cast<size_t>(disp_w) * disp_h);

    for (u32 y = 0; y < disp_h; ++y) {
        const u32 src_y = dby + y;
        for (u32 x = 0; x < disp_w; ++x) {
            const u32 src_x = dbx + x;
            const u32 out_idx = y * disp_w + x;

            if (psm == 0) {
                const u32 byte_off = gs_layout::calc_edram_offset_psmct32(src_x, src_y, pitch);
                const u32 addr = (base_byte + byte_off) & (kEdramSize - 4U);
                u32 px = 0;
                std::memcpy(&px, edram_.data() + addr, sizeof(u32));
                out_pixels[out_idx] = px;
            } else if (psm == 1) {
                const u32 byte_off = gs_layout::calc_edram_offset_psmct32(src_x, src_y, pitch);
                const u32 addr = (base_byte + byte_off) & (kEdramSize - 4U);
                u32 px = 0;
                std::memcpy(&px, edram_.data() + addr, sizeof(u32));
                out_pixels[out_idx] = px | 0xFF000000U;
            } else if (psm == 2) {
                const u32 byte_off = gs_layout::calc_edram_offset_psmct16(src_x, src_y, pitch);
                const u32 addr = (base_byte + byte_off) & (kEdramSize - 2U);
                u16 c16 = 0;
                std::memcpy(&c16, edram_.data() + addr, sizeof(u16));
                const u32 r5 = (c16 & 0x001FU);
                const u32 g5 = ((c16 >> 5) & 0x001FU);
                const u32 b5 = ((c16 >> 10) & 0x001FU);
                const u32 r = (r5 << 3) | (r5 >> 2);
                const u32 g = (g5 << 3) | (g5 >> 2);
                const u32 b = (b5 << 3) | (b5 >> 2);
                const u32 a = (c16 & 0x8000U) ? 0xFFU : 0x00U;
                out_pixels[out_idx] = r | (g << 8) | (b << 16) | (a << 24);
            } else if (psm == 10) { // PSMCT16S con tabla de bloques propia
                const u32 byte_off = gs_layout::calc_edram_offset_psmct16s(src_x, src_y, pitch);
                const u32 addr = (base_byte + byte_off) & (kEdramSize - 2U);
                u16 c16 = 0;
                std::memcpy(&c16, edram_.data() + addr, sizeof(u16));
                const u32 r5 = (c16 & 0x001FU);
                const u32 g5 = ((c16 >> 5) & 0x001FU);
                const u32 b5 = ((c16 >> 10) & 0x001FU);
                const u32 r = (r5 << 3) | (r5 >> 2);
                const u32 g = (g5 << 3) | (g5 >> 2);
                const u32 b = (b5 << 3) | (b5 >> 2);
                const u32 a = (c16 & 0x8000U) ? 0xFFU : 0x00U;
                out_pixels[out_idx] = r | (g << 8) | (b << 16) | (a << 24);
            } else {
                out_pixels[out_idx] = 0xFF000000U;
            }
        }
    }
    return true;
}

void GsvkRenderer::submit_gif_packet(const u8* packet_ptr, u32 size_bytes, u8 path_index) {
    if (!packet_ptr || size_bytes < 16) return;
    stats_.gif_packets_processed++;

    const u8 p = (path_index < 4) ? path_index : 3;
    auto& st = path_state_[p];

    u32 offset = 0;
    while (offset + 16 <= size_bytes) {
        u64 q_lo = 0, q_hi = 0;
        std::memcpy(&q_lo, packet_ptr + offset,     8);
        std::memcpy(&q_hi, packet_ptr + offset + 8, 8);
        offset += 16;

        if (st.state == GifState::Tag) {
            const u64 tag_lo = q_lo;
            const u64 tag_hi = q_hi;

            st.nloop       = static_cast<u32>(tag_lo & 0x7FFFULL);
            st.eop         = ((tag_lo >> 15) & 1ULL) != 0;
            const bool pre = ((tag_lo >> 46) & 1ULL) != 0;
            const u64 prim = (tag_lo >> 47) & 0x7FFULL;
            st.flg         = static_cast<u8>((tag_lo >> 58) & 0x03ULL);
            st.nregs       = static_cast<u32>((tag_lo >> 60) & 0x0FULL);
            if (st.nregs == 0) st.nregs = 16;
            st.tag_hi      = tag_hi;
            st.loop_count  = 0;
            st.reg_idx     = 0;

            if (pre) {
                write_gs_register(static_cast<u8>(GsReg::PRIM), prim);
            }
            static u32 log_gif = 0;
            if (log_gif++ < 30) {
                std::printf("[GIF Pkt] path=%u nloop=%u flg=%u nregs=%u pre=%d prim=%llu eop=%d\n",
                    path_index, st.nloop, st.flg, st.nregs, pre, (unsigned long long)prim, st.eop);
                std::fflush(stdout);
            }

            if (st.nloop == 0) {
                if (st.eop) st.state = GifState::Tag;
                continue;
            }

            if (st.flg == 0) {
                st.state = GifState::Packed;
            } else if (st.flg == 1) {
                st.state = GifState::Reglist;
            } else if (st.flg == 2) {
                st.state = GifState::Image;
            }
        }
        else if (st.state == GifState::Image) {
            r128 qw;
            qw.ud[0] = q_lo;
            qw.ud[1] = q_hi;
            upload_image_qword(qw);
            st.loop_count++;
            if (st.loop_count >= st.nloop) {
                st.state = GifState::Tag;
            }
        }
        else if (st.state == GifState::Packed) {
            const u8 reg_desc = static_cast<u8>((st.tag_hi >> (st.reg_idx * 4)) & 0x0FULL);

            if (reg_desc == 0x0E) { // A+D (Address + Data)
                write_gs_register(static_cast<u8>(q_hi & 0xFFULL), q_lo);
            } else if (reg_desc == 0x01) { // RGBAQ packed format from VU1
                cur_vtx_.r = static_cast<u8>(q_lo & 0xFFULL);
                cur_vtx_.g = static_cast<u8>((q_lo >> 32) & 0xFFULL);
                cur_vtx_.b = static_cast<u8>(q_hi & 0xFFULL);
                cur_vtx_.a = static_cast<u8>((q_hi >> 32) & 0xFFULL);
            } else if (reg_desc == 0x02) { // STQ packed format from VU1
                const u32 s_bits = static_cast<u32>(q_lo & 0xFFFFFFFFULL);
                const u32 t_bits = static_cast<u32>((q_lo >> 32) & 0xFFFFFFFFULL);
                const u32 q_bits = static_cast<u32>(q_hi & 0xFFFFFFFFULL);
                std::memcpy(&cur_vtx_.s, &s_bits, 4);
                std::memcpy(&cur_vtx_.t, &t_bits, 4);
                if (q_bits != 0) std::memcpy(&cur_vtx_.q, &q_bits, 4);
            } else if (reg_desc == 0x03) { // UV packed format
                const u16 u_val = static_cast<u16>(q_lo & 0x3FFFULL);
                const u16 v_val = static_cast<u16>((q_lo >> 32) & 0x3FFFULL);
                cur_vtx_.u = u_val;
                cur_vtx_.v = v_val;
                cur_vtx_.s = static_cast<f32>(u_val) / 16.0f;
                cur_vtx_.t = static_cast<f32>(v_val) / 16.0f;
            } else if (reg_desc == 0x04) { // XYZF2 packed format from VU1
                const u16 x_fp = static_cast<u16>(q_lo & 0xFFFFULL);
                const u16 y_fp = static_cast<u16>((q_lo >> 32) & 0xFFFFULL);
                const u32 z_val = static_cast<u32>(q_hi & 0x00FFFFFFULL);
                const bool adc = ((q_hi >> 47) & 1ULL) != 0;
                const u64 packed_xyz = static_cast<u64>(x_fp) | (static_cast<u64>(y_fp) << 16) | (static_cast<u64>(z_val) << 32);
                vertex_kick(packed_xyz, !adc);
            } else if (reg_desc == 0x05) { // XYZ2 packed format from VU1
                const u16 x_fp = static_cast<u16>(q_lo & 0xFFFFULL);
                const u16 y_fp = static_cast<u16>((q_lo >> 32) & 0xFFFFULL);
                const u32 z_val = static_cast<u32>(q_hi & 0xFFFFFFFFULL);
                const u64 packed_xyz = static_cast<u64>(x_fp) | (static_cast<u64>(y_fp) << 16) | (static_cast<u64>(z_val) << 32);
                vertex_kick(packed_xyz, true);
            } else if (reg_desc == 0x0C) { // XYZF3 packed format
                const u16 x_fp = static_cast<u16>(q_lo & 0xFFFFULL);
                const u16 y_fp = static_cast<u16>((q_lo >> 32) & 0xFFFFULL);
                const u32 z_val = static_cast<u32>(q_hi & 0x00FFFFFFULL);
                const u64 packed_xyz = static_cast<u64>(x_fp) | (static_cast<u64>(y_fp) << 16) | (static_cast<u64>(z_val) << 32);
                vertex_kick(packed_xyz, false);
            } else if (reg_desc == 0x0C) { // XYZF3 packed format
                const u16 x_fp = static_cast<u16>(q_lo & 0xFFFFULL);
                const u16 y_fp = static_cast<u16>((q_lo >> 32) & 0xFFFFULL);
                const u32 z_val = static_cast<u32>(q_hi & 0x00FFFFFFULL);
                const u64 packed_xyz = static_cast<u64>(x_fp) | (static_cast<u64>(y_fp) << 16) | (static_cast<u64>(z_val) << 32);
                vertex_kick(packed_xyz, false);
            } else if (reg_desc == 0x0D) { // XYZ3 packed format
                const u16 x_fp = static_cast<u16>(q_lo & 0xFFFFULL);
                const u16 y_fp = static_cast<u16>((q_lo >> 32) & 0xFFFFULL);
                const u32 z_val = static_cast<u32>(q_hi & 0xFFFFFFFFULL);
                const u64 packed_xyz = static_cast<u64>(x_fp) | (static_cast<u64>(y_fp) << 16) | (static_cast<u64>(z_val) << 32);
                vertex_kick(packed_xyz, false);
            } else {
                write_gs_register(reg_desc, q_lo);
            }

            st.reg_idx++;
            if (st.reg_idx >= st.nregs) {
                st.reg_idx = 0;
                st.loop_count++;
                if (st.loop_count >= st.nloop) {
                    st.state = GifState::Tag;
                }
            }
        }
        else if (st.state == GifState::Reglist) {
            u64 halves[2] = { q_lo, q_hi };
            for (int h = 0; h < 2; ++h) {
                const u8 reg_desc = static_cast<u8>((st.tag_hi >> (st.reg_idx * 4)) & 0x0FULL);
                write_gs_register(reg_desc, halves[h]);
                st.reg_idx++;
                if (st.reg_idx >= st.nregs) {
                    st.reg_idx = 0;
                    st.loop_count++;
                    if (st.loop_count >= st.nloop) {
                        st.state = GifState::Tag;
                        break;
                    }
                }
            }
        }
    }
}

void GsvkRenderer::upload_image_qword(const r128& qword) noexcept {
    static u32 log_up = 0;
    if (trx_x_ == trx_dsax_ && trx_y_ == trx_dsay_ && log_up++ < 15) {
        std::printf("[GsUpload] dbp=%u dbw=%u dpsm=%u dsax=%u dsay=%u rrw=%u rrh=%u\n",
            trx_dbp_, trx_dbw_, trx_dpsm_, trx_dsax_, trx_dsay_, trx_rrw_, trx_rrh_);
        std::fflush(stdout);
    }
    const u32 pitch_pixels = (trx_dbw_ > 0) ? (trx_dbw_ * 64U) : (trx_rrw_ > 0 ? trx_rrw_ : 64U);
    const u32 base_byte = (trx_dbp_ * 256U) & (kEdramSize - 1U);
    const u32 end_x = trx_dsax_ + (trx_rrw_ > 0 ? trx_rrw_ : 1U);
    const u32 end_y = trx_dsay_ + (trx_rrh_ > 0 ? trx_rrh_ : 1U);

    if (trx_dpsm_ == 0 || trx_dpsm_ == 1) { // PSMCT32 / PSMCT24 con mapeo de páginas/bloques GS
        const u32* px32 = reinterpret_cast<const u32*>(&qword);
        for (u32 i = 0; i < 4; ++i) {
            if (trx_y_ >= end_y) break;
            const u32 byte_off = gs_layout::calc_edram_offset_psmct32(trx_x_, trx_y_, pitch_pixels);
            const u32 addr = (base_byte + byte_off) & (kEdramSize - 4U);
            *reinterpret_cast<u32*>(edram_.data() + addr) = px32[i];
            trx_x_++;
            if (trx_x_ >= end_x) {
                trx_x_ = trx_dsax_;
                trx_y_++;
            }
        }
    } else if (trx_dpsm_ == 2) { // PSMCT16 con mapeo de páginas/bloques GS
        const u16* px16 = reinterpret_cast<const u16*>(&qword);
        for (u32 i = 0; i < 8; ++i) {
            if (trx_y_ >= end_y) break;
            const u32 byte_off = gs_layout::calc_edram_offset_psmct16(trx_x_, trx_y_, pitch_pixels);
            const u32 addr = (base_byte + byte_off) & (kEdramSize - 2U);
            *reinterpret_cast<u16*>(edram_.data() + addr) = px16[i];
            trx_x_++;
            if (trx_x_ >= end_x) {
                trx_x_ = trx_dsax_;
                trx_y_++;
            }
        }
    } else if (trx_dpsm_ == 10) { // PSMCT16S con mapeo de páginas/bloques propio
        const u16* px16 = reinterpret_cast<const u16*>(&qword);
        for (u32 i = 0; i < 8; ++i) {
            if (trx_y_ >= end_y) break;
            const u32 byte_off = gs_layout::calc_edram_offset_psmct16s(trx_x_, trx_y_, pitch_pixels);
            const u32 addr = (base_byte + byte_off) & (kEdramSize - 2U);
            *reinterpret_cast<u16*>(edram_.data() + addr) = px16[i];
            trx_x_++;
            if (trx_x_ >= end_x) {
                trx_x_ = trx_dsax_;
                trx_y_++;
            }
        }
    } else if (trx_dpsm_ == 19) { // PSMT8: 8 bits por píxel (16 píxeles por QWORD)
        const u8* px8 = reinterpret_cast<const u8*>(&qword);
        for (u32 i = 0; i < 16; ++i) {
            if (trx_y_ >= end_y) break;
            const u32 byte_off = (trx_y_ * pitch_pixels + trx_x_) * 1U;
            const u32 addr = (base_byte + byte_off) & (kEdramSize - 1U);
            edram_[addr] = px8[i];
            trx_x_++;
            if (trx_x_ >= end_x) {
                trx_x_ = trx_dsax_;
                trx_y_++;
            }
        }
    } else if (trx_dpsm_ == 20) { // PSMT4: 4 bits por píxel (32 píxeles por QWORD)
        const u8* px8 = reinterpret_cast<const u8*>(&qword);
        for (u32 i = 0; i < 32; ++i) {
            if (trx_y_ >= end_y) break;
            const u8 nibble = (i & 1) ? (px8[i / 2] >> 4) : (px8[i / 2] & 0x0FU);
            const u32 bit_idx = trx_y_ * pitch_pixels + trx_x_;
            const u32 addr = (base_byte + (bit_idx / 2U)) & (kEdramSize - 1U);
            if (bit_idx & 1) {
                edram_[addr] = (edram_[addr] & 0x0FU) | (nibble << 4);
            } else {
                edram_[addr] = (edram_[addr] & 0xF0U) | nibble;
            }
            trx_x_++;
            if (trx_x_ >= end_x) {
                trx_x_ = trx_dsax_;
                trx_y_++;
            }
        }
    } else { // Fallback raw copy
        const u32 byte_off = (trx_y_ * pitch_pixels + trx_x_) * 4U;
        const u32 addr = (base_byte + byte_off) & (kEdramSize - 16U);
        std::memcpy(edram_.data() + addr, &qword, 16);
        trx_x_ += 4;
        if (trx_x_ >= end_x) {
            trx_x_ = trx_dsax_;
            trx_y_++;
        }
    }
}

void GsvkRenderer::vertex_kick(u64 xyz_raw, bool drawing_kick) {
    const u16 raw_x = static_cast<u16>(xyz_raw & 0xFFFFULL);
    const u16 raw_y = static_cast<u16>((xyz_raw >> 16) & 0xFFFFULL);
    const u32 raw_z = static_cast<u32>((xyz_raw >> 32) & 0xFFFFFFFFULL);

    const f32 base_x = (static_cast<f32>(raw_x) / 16.0f) - ofx_;
    const f32 base_y = (static_cast<f32>(raw_y) / 16.0f) - ofy_;

    f32 sx = base_x * static_cast<f32>(res_scale_);
    f32 sy = base_y * static_cast<f32>(res_scale_);

    if (aspect_ratio_ == AspectRatio::Widescreen_16_9) {
        const f32 center_x = static_cast<f32>(width_) * 0.5f;
        sx = center_x + (sx - center_x) * (3.0f / 4.0f * 16.0f / 9.0f);
    } else if (aspect_ratio_ == AspectRatio::Ultrawide_21_9) {
        const f32 center_x = static_cast<f32>(width_) * 0.5f;
        sx = center_x + (sx - center_x) * (3.0f / 4.0f * 21.0f / 9.0f);
    }

    cur_vtx_.x = sx;
    cur_vtx_.y = sy;
    cur_vtx_.z = raw_z;
    stats_.vertices_kicked++;

    switch (prim_type_) {
        case GsPrimType::Triangle: {
            vtx_queue_[vtx_count_++] = cur_vtx_;
            if (vtx_count_ == 3) {
                if (drawing_kick) {
                    rasterize_triangle(vtx_queue_[0], vtx_queue_[1], vtx_queue_[2]);
                    stats_.primitives_drawn++;
                }
                vtx_count_ = 0;
            }
            break;
        }
        case GsPrimType::TriangleStrip: {
            if (vtx_count_ < 3) {
                vtx_queue_[vtx_count_++] = cur_vtx_;
            } else {
                vtx_queue_[0] = vtx_queue_[1];
                vtx_queue_[1] = vtx_queue_[2];
                vtx_queue_[2] = cur_vtx_;
            }
            if (vtx_count_ == 3 && drawing_kick) {
                rasterize_triangle(vtx_queue_[0], vtx_queue_[1], vtx_queue_[2]);
                stats_.primitives_drawn++;
            }
            break;
        }
        case GsPrimType::TriangleFan: {
            if (vtx_count_ < 2) {
                vtx_queue_[vtx_count_++] = cur_vtx_;
            } else if (vtx_count_ == 2) {
                vtx_queue_[2] = cur_vtx_;
                vtx_count_ = 3;
                if (drawing_kick) {
                    rasterize_triangle(vtx_queue_[0], vtx_queue_[1], vtx_queue_[2]);
                    stats_.primitives_drawn++;
                }
            } else {
                vtx_queue_[1] = vtx_queue_[2];
                vtx_queue_[2] = cur_vtx_;
                if (drawing_kick) {
                    rasterize_triangle(vtx_queue_[0], vtx_queue_[1], vtx_queue_[2]);
                    stats_.primitives_drawn++;
                }
            }
            break;
        }
        case GsPrimType::Sprite: {
            vtx_queue_[vtx_count_++] = cur_vtx_;
            if (vtx_count_ == 2) {
                if (drawing_kick) {
                    rasterize_sprite(vtx_queue_[0], vtx_queue_[1]);
                    stats_.primitives_drawn++;
                }
                vtx_count_ = 0;
            }
            break;
        }
        default:
            break;
    }
}

u32 GsvkRenderer::sample_texture(f32 u_norm, f32 v_norm) const noexcept {
    const u64 tex0 = (ctxt_ == 0) ? tex0_1_reg_ : tex0_2_reg_;
    if (tex0 == 0) return 0xFFFFFFFFU;

    const u32 tbp0 = static_cast<u32>(tex0 & 0x3FFFULL);
    const u32 tbw  = static_cast<u32>((tex0 >> 14) & 0x3FULL);
    const u32 psm  = static_cast<u32>((tex0 >> 20) & 0x3FULL);
    const u32 tw   = 1U << ((tex0 >> 26) & 0x0FULL);
    const u32 th   = 1U << ((tex0 >> 30) & 0x0FULL);
    const u32 tcc  = static_cast<u32>((tex0 >> 34) & 0x01ULL);
    const u32 tfx  = static_cast<u32>((tex0 >> 35) & 0x03ULL);
    const u32 cbp  = static_cast<u32>((tex0 >> 37) & 0x3FFFULL);
    const u32 cpsm = static_cast<u32>((tex0 >> 51) & 0x0FULL);
    const u32 csm  = static_cast<u32>((tex0 >> 55) & 0x01ULL);
    const u32 csa  = static_cast<u32>((tex0 >> 56) & 0x1FULL);

    const u64 tex_key = (static_cast<u64>(tbp0) << 32) | (static_cast<u64>(tw) << 16) | th;
    if (hd_textures_.has_texture(tex_key)) {
        return hd_textures_.sample(tex_key, u_norm, v_norm);
    }

    u32 tx = 0, ty = 0;
    if (fst_uv_mode_) {
        tx = static_cast<u32>(std::clamp(u_norm, 0.0f, static_cast<f32>(tw > 0 ? tw - 1 : 0))) & (tw - 1);
        ty = static_cast<u32>(std::clamp(v_norm, 0.0f, static_cast<f32>(th > 0 ? th - 1 : 0))) & (th - 1);
    } else {
        tx = static_cast<u32>(std::clamp(u_norm, 0.0f, 1.0f) * static_cast<f32>(tw > 0 ? tw - 1 : 0)) & (tw - 1);
        ty = static_cast<u32>(std::clamp(v_norm, 0.0f, 1.0f) * static_cast<f32>(th > 0 ? th - 1 : 0)) & (th - 1);
    }

    const u32 pitch = (tbw > 0) ? (tbw * 64U) : tw;
    const u32 tex_base = (tbp0 * 256U) & (kEdramSize - 1U);

    if (psm == 0 || psm == 1) { // PSMCT32 / PSMCT24 con direccionamiento swizzled GS
        const u32 byte_off = gs_layout::calc_edram_offset_psmct32(tx, ty, pitch);
        const u32 byte_addr = (tex_base + byte_off) & (kEdramSize - 4U);
        u32 texel = 0;
        std::memcpy(&texel, edram_.data() + byte_addr, sizeof(u32));
        if (psm == 1 || tcc == 0) texel |= 0xFF000000U;
        return texel;
    } else if (psm == 2) { // PSMCT16 con direccionamiento swizzled GS y expansion completa 8-bit
        const u32 byte_off = gs_layout::calc_edram_offset_psmct16(tx, ty, pitch);
        const u32 byte_addr = (tex_base + byte_off) & (kEdramSize - 2U);
        u16 c16 = 0;
        std::memcpy(&c16, edram_.data() + byte_addr, sizeof(u16));
        const u32 r5 = c16 & 0x001FU;
        const u32 g5 = (c16 >> 5) & 0x001FU;
        const u32 b5 = (c16 >> 10) & 0x001FU;
        const u32 r = (r5 << 3) | (r5 >> 2);
        const u32 g = (g5 << 3) | (g5 >> 2);
        const u32 b = (b5 << 3) | (b5 >> 2);
        const u32 a = (c16 & 0x8000U) ? 0xFFU : 0x00U;
        return r | (g << 8) | (b << 16) | (((tcc == 0) ? 0xFFU : a) << 24);
    } else if (psm == 10) { // PSMCT16S con tabla de bloques propia (sub-buffer GS) y expansion completa 8-bit
        const u32 byte_off = gs_layout::calc_edram_offset_psmct16s(tx, ty, pitch);
        const u32 byte_addr = (tex_base + byte_off) & (kEdramSize - 2U);
        u16 c16 = 0;
        std::memcpy(&c16, edram_.data() + byte_addr, sizeof(u16));
        const u32 r5 = c16 & 0x001FU;
        const u32 g5 = (c16 >> 5) & 0x001FU;
        const u32 b5 = (c16 >> 10) & 0x001FU;
        const u32 r = (r5 << 3) | (r5 >> 2);
        const u32 g = (g5 << 3) | (g5 >> 2);
        const u32 b = (b5 << 3) | (b5 >> 2);
        const u32 a = (c16 & 0x8000U) ? 0xFFU : 0x00U;
        return r | (g << 8) | (b << 16) | (((tcc == 0) ? 0xFFU : a) << 24);
    } else if (psm == 19) { // PSMT8 (8-bit con CLUT - aproximacion lineal/unswizzled en eDRAM)
        const u32 byte_addr = (tex_base + (ty * pitch + tx)) & (kEdramSize - 1U);
        const u8 idx = edram_[byte_addr];
        u32 clut_idx = idx;
        if (csm == 0) { // CSM1 (PS2 CLUT swizzle para modo de 8 bits)
            if ((idx & 0x18) == 0x08) clut_idx += 8;
            else if ((idx & 0x18) == 0x10) clut_idx -= 8;
        }
        clut_idx += csa * 16U;
        const u32 clut_base = (cbp * 256U) & (kEdramSize - 1U);
        if (cpsm == 2) { // CLUT en formato 16-bit con expansion completa
            const u32 clut_addr = (clut_base + clut_idx * 2U) & (kEdramSize - 2U);
            u16 c16 = 0;
            std::memcpy(&c16, edram_.data() + clut_addr, sizeof(u16));
            const u32 r5 = c16 & 0x001FU;
            const u32 g5 = (c16 >> 5) & 0x001FU;
            const u32 b5 = (c16 >> 10) & 0x001FU;
            const u32 r = (r5 << 3) | (r5 >> 2);
            const u32 g = (g5 << 3) | (g5 >> 2);
            const u32 b = (b5 << 3) | (b5 >> 2);
            const u32 a = (c16 & 0x8000U) ? 0xFFU : 0x00U;
            return r | (g << 8) | (b << 16) | (((tcc == 0) ? 0xFFU : a) << 24);
        } else { // CLUT en formato 32-bit
            const u32 clut_addr = (clut_base + clut_idx * 4U) & (kEdramSize - 4U);
            u32 texel = 0;
            std::memcpy(&texel, edram_.data() + clut_addr, sizeof(u32));
            if (tcc == 0) texel |= 0xFF000000U;
            return texel;
        }
    } else if (psm == 20) { // PSMT4 (4-bit con CLUT - aproximacion lineal/unswizzled en eDRAM)
        const u32 bit_idx = ty * pitch + tx;
        const u32 byte_addr = (tex_base + (bit_idx / 2U)) & (kEdramSize - 1U);
        const u8 byte_val = edram_[byte_addr];
        const u8 idx = (bit_idx & 1) ? (byte_val >> 4) : (byte_val & 0x0FU);
        const u32 clut_idx = (idx & 0x0FU) + (csa * 16U);
        const u32 clut_base = (cbp * 256U) & (kEdramSize - 1U);
        if (cpsm == 2) { // CLUT en formato 16-bit con expansion completa
            const u32 clut_addr = (clut_base + clut_idx * 2U) & (kEdramSize - 2U);
            u16 c16 = 0;
            std::memcpy(&c16, edram_.data() + clut_addr, sizeof(u16));
            const u32 r5 = c16 & 0x001FU;
            const u32 g5 = (c16 >> 5) & 0x001FU;
            const u32 b5 = (c16 >> 10) & 0x001FU;
            const u32 r = (r5 << 3) | (r5 >> 2);
            const u32 g = (g5 << 3) | (g5 >> 2);
            const u32 b = (b5 << 3) | (b5 >> 2);
            const u32 a = (c16 & 0x8000U) ? 0xFFU : 0x00U;
            return r | (g << 8) | (b << 16) | (((tcc == 0) ? 0xFFU : a) << 24);
        } else {
            const u32 clut_addr = (clut_base + clut_idx * 4U) & (kEdramSize - 4U);
            u32 texel = 0;
            std::memcpy(&texel, edram_.data() + clut_addr, sizeof(u32));
            if (tcc == 0) texel |= 0xFF000000U;
            return texel;
        }
    }

    // Default fallback
    const u32 byte_addr = (tex_base + (ty * pitch + tx) * 4U) & (kEdramSize - 4U);
    u32 texel = 0;
    std::memcpy(&texel, edram_.data() + byte_addr, sizeof(u32));
    return texel;
}

u32 GsvkRenderer::blend_ps2_alpha(u32 src_rgba, u32 dst_rgba) const noexcept {
    // Fórmula hardware del Graphics Synthesizer: Cv = ((A - B) * C >> 7) + D
    // Por defecto (Cs - Cd) * As >> 7 + Cd
    const s32 sr = static_cast<s32>(src_rgba & 0xFFU);
    const s32 sg = static_cast<s32>((src_rgba >> 8) & 0xFFU);
    const s32 sb = static_cast<s32>((src_rgba >> 16) & 0xFFU);
    const s32 sa = static_cast<s32>((src_rgba >> 24) & 0xFFU);

    const s32 dr = static_cast<s32>(dst_rgba & 0xFFU);
    const s32 dg = static_cast<s32>((dst_rgba >> 8) & 0xFFU);
    const s32 db = static_cast<s32>((dst_rgba >> 16) & 0xFFU);

    const s32 out_r = std::clamp(((sr - dr) * sa >> 7) + dr, 0, 255);
    const s32 out_g = std::clamp(((sg - dg) * sa >> 7) + dg, 0, 255);
    const s32 out_b = std::clamp(((sb - db) * sa >> 7) + db, 0, 255);
    return static_cast<u32>(out_r) | (static_cast<u32>(out_g) << 8) | (static_cast<u32>(out_b) << 16) | 0xFF000000U;
}

void GsvkRenderer::rasterize_triangle(const GsVertex& v0, const GsVertex& v1, const GsVertex& v2) {
    static u32 log_tri = 0;
    if ((tme_textured_ || v0.r != 0 || v0.g != 0 || v0.b != 0) && log_tri++ < 30) {
        const u64 t0 = (ctxt_ == 0) ? tex0_1_reg_ : tex0_2_reg_;
        std::printf("[GsTri] tme=%d tex0=0x%016llX v0=(%.1f,%.1f,st=%.2f,%.2f,rgba=%u,%u,%u,%u) v1=(%.1f,%.1f) v2=(%.1f,%.1f)\n",
            tme_textured_, (unsigned long long)t0,
            v0.x, v0.y, v0.s, v0.t, v0.r, v0.g, v0.b, v0.a,
            v1.x, v1.y, v2.x, v2.y);
        std::fflush(stdout);
    }
    const f32 min_xf = std::min({v0.x, v1.x, v2.x});
    const f32 max_xf = std::max({v0.x, v1.x, v2.x});
    const f32 min_yf = std::min({v0.y, v1.y, v2.y});
    const f32 max_yf = std::max({v0.y, v1.y, v2.y});

    const s32 x0 = std::max(static_cast<s32>(scissor_x0_), static_cast<s32>(std::floor(min_xf)));
    const s32 x1 = std::min(static_cast<s32>(std::min(width_ - 1, scissor_x1_)), static_cast<s32>(std::ceil(max_xf)));
    const s32 y0 = std::max(static_cast<s32>(scissor_y0_), static_cast<s32>(std::floor(min_yf)));
    const s32 y1 = std::min(static_cast<s32>(std::min(height_ - 1, scissor_y1_)), static_cast<s32>(std::ceil(max_yf)));

    auto edge = [](f32 ax, f32 ay, f32 bx, f32 by, f32 px, f32 py) -> f32 {
        return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
    };

    f32 area = edge(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);
    if (std::fabs(area) < 1e-4f) return;

    const f32 inv_area = 1.0f / area;

    for (s32 y = y0; y <= y1; ++y) {
        for (s32 x = x0; x <= x1; ++x) {
            const f32 px = static_cast<f32>(x) + 0.5f;
            const f32 py = static_cast<f32>(y) + 0.5f;

            const f32 w0 = edge(v1.x, v1.y, v2.x, v2.y, px, py) * inv_area;
            const f32 w1 = edge(v2.x, v2.y, v0.x, v0.y, px, py) * inv_area;
            const f32 w2 = edge(v0.x, v0.y, v1.x, v1.y, px, py) * inv_area;

            if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                const u32 idx = static_cast<u32>(y) * width_ + static_cast<u32>(x);
                const u32 z   = static_cast<u32>(w0 * static_cast<f32>(v0.z) + w1 * static_cast<f32>(v1.z) + w2 * static_cast<f32>(v2.z));

                // Depth test (ZTE y ZTST)
                bool pass_depth = true;
                if (test_zte_) {
                    switch (test_ztst_) {
                        case 0: pass_depth = false; break;                 // NEVER
                        case 1: pass_depth = true; break;                  // ALWAYS
                        case 2: pass_depth = (z >= depth_buffer_[idx]); break; // GEQUAL (GS default)
                        case 3: pass_depth = (z > depth_buffer_[idx]); break;  // GREATER
                    }
                }
                if (!pass_depth) continue;

                u8 r = v2.r, g = v2.g, b = v2.b, a = v2.a;
                if (iip_gouraud_) {
                    r = static_cast<u8>(std::clamp(w0 * v0.r + w1 * v1.r + w2 * v2.r, 0.0f, 255.0f));
                    g = static_cast<u8>(std::clamp(w0 * v0.g + w1 * v1.g + w2 * v2.g, 0.0f, 255.0f));
                    b = static_cast<u8>(std::clamp(w0 * v0.b + w1 * v1.b + w2 * v2.b, 0.0f, 255.0f));
                    a = static_cast<u8>(std::clamp(w0 * v0.a + w1 * v1.a + w2 * v2.a, 0.0f, 255.0f));
                }

                u32 rgba = static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) | (static_cast<u32>(a) << 24);
                if (tme_textured_) {
                    const f32 u_interp = w0 * v0.s + w1 * v1.s + w2 * v2.s;
                    const f32 v_interp = w0 * v0.t + w1 * v1.t + w2 * v2.t;
                    const u32 texel = sample_texture(u_interp, v_interp);

                    const u64 tex0 = (ctxt_ == 0) ? tex0_1_reg_ : tex0_2_reg_;
                    const u32 tcc  = static_cast<u32>((tex0 >> 34) & 0x01ULL);
                    const u32 tfx  = static_cast<u32>((tex0 >> 35) & 0x03ULL);

                    const u32 tr = texel & 0xFFU;
                    const u32 tg = (texel >> 8) & 0xFFU;
                    const u32 tb = (texel >> 16) & 0xFFU;
                    const u32 ta = (texel >> 24) & 0xFFU;

                    u32 out_r = r, out_g = g, out_b = b, out_a = a;
                    if (tfx == 0) { // MODULATE
                        out_r = (tr * r) >> 7;
                        out_g = (tg * g) >> 7;
                        out_b = (tb * b) >> 7;
                        out_a = (tcc == 1) ? ((ta * a) >> 7) : a;
                    } else if (tfx == 1) { // DECAL
                        out_r = tr;
                        out_g = tg;
                        out_b = tb;
                        out_a = (tcc == 1) ? ta : a;
                    } else if (tfx == 2) { // HIGHLIGHT
                        out_r = ((tr * r) >> 7) + a;
                        out_g = ((tg * g) >> 7) + a;
                        out_b = ((tb * b) >> 7) + a;
                        out_a = (tcc == 1) ? (ta + a) : a;
                    } else { // HIGHLIGHT2
                        out_r = ((tr * r) >> 7) + a;
                        out_g = ((tg * g) >> 7) + a;
                        out_b = ((tb * b) >> 7) + a;
                        out_a = a;
                    }

                    r = static_cast<u8>(std::min(out_r, 255U));
                    g = static_cast<u8>(std::min(out_g, 255U));
                    b = static_cast<u8>(std::min(out_b, 255U));
                    a = static_cast<u8>(std::min(out_a, 255U));
                    rgba = static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) | (static_cast<u32>(a) << 24);
                }

                // Alpha Test (ATE)
                if (test_ate_) {
                    bool pass_alpha = true;
                    switch (test_atst_) {
                        case 0: pass_alpha = false; break;                 // NEVER
                        case 1: pass_alpha = true; break;                  // ALWAYS
                        case 2: pass_alpha = (a < test_aref_); break;      // LESS
                        case 3: pass_alpha = (a <= test_aref_); break;     // LEQUAL
                        case 4: pass_alpha = (a == test_aref_); break;     // EQUAL
                        case 5: pass_alpha = (a >= test_aref_); break;     // GEQUAL
                        case 6: pass_alpha = (a > test_aref_); break;      // GREATER
                        case 7: pass_alpha = (a != test_aref_); break;     // NOTEQUAL
                    }
                    if (!pass_alpha) continue;
                }

                depth_buffer_[idx] = z;

                if (abe_alpha_) {
                    rgba = blend_ps2_alpha(rgba, color_buffer_[idx]);
                } else {
                    rgba |= 0xFF000000U;
                }
                color_buffer_[idx] = rgba;

                const u64 frame_reg = (ctxt_ == 0) ? frame_1_reg_ : frame_2_reg_;
                const u32 fbp = static_cast<u32>(frame_reg & 0x1FFULL);
                const u32 fbw = static_cast<u32>((frame_reg >> 16) & 0x3FULL);
                const u32 psm = static_cast<u32>((frame_reg >> 24) & 0x3FULL);
                const u32 fb_base = (fbp * 8192U) & (kEdramSize - 1U);
                const u32 pitch = (fbw > 0) ? (fbw * 64U) : kDefaultW;

                const u32 nx = (res_scale_ > 1) ? (static_cast<u32>(x) / res_scale_) : static_cast<u32>(x);
                const u32 ny = (res_scale_ > 1) ? (static_cast<u32>(y) / res_scale_) : static_cast<u32>(y);

                if (nx < pitch && ny < 1024U) {
                    if (psm == 0 || psm == 1) {
                        const u32 byte_off = gs_layout::calc_edram_offset_psmct32(nx, ny, pitch);
                        const u32 byte_addr = (fb_base + byte_off) & (kEdramSize - 4U);
                        *reinterpret_cast<u32*>(edram_.data() + byte_addr) = rgba;
                    } else if (psm == 2) {
                        const u16 c16 = static_cast<u16>(
                            ((r >> 3) & 0x1FU) |
                            (((g >> 3) & 0x1FU) << 5) |
                            (((b >> 3) & 0x1FU) << 10) |
                            ((a >= 128U) ? 0x8000U : 0x0000U)
                        );
                        const u32 byte_off = gs_layout::calc_edram_offset_psmct16(nx, ny, pitch);
                        const u32 byte_addr = (fb_base + byte_off) & (kEdramSize - 2U);
                        *reinterpret_cast<u16*>(edram_.data() + byte_addr) = c16;
                    } else if (psm == 10) {
                        const u16 c16 = static_cast<u16>(
                            ((r >> 3) & 0x1FU) |
                            (((g >> 3) & 0x1FU) << 5) |
                            (((b >> 3) & 0x1FU) << 10) |
                            ((a >= 128U) ? 0x8000U : 0x0000U)
                        );
                        const u32 byte_off = gs_layout::calc_edram_offset_psmct16s(nx, ny, pitch);
                        const u32 byte_addr = (fb_base + byte_off) & (kEdramSize - 2U);
                        *reinterpret_cast<u16*>(edram_.data() + byte_addr) = c16;
                    }
                }
            }
        }
    }
}

void GsvkRenderer::rasterize_sprite(const GsVertex& v0, const GsVertex& v1) {
    
    GsVertex a = v0, b = v1;
    GsVertex c = v1; c.x = v0.x; c.y = v1.y; c.s = v0.s; c.t = v1.t; c.u = v0.u; c.v = v1.v;
    GsVertex d = v1; d.x = v1.x; d.y = v0.y; d.s = v1.s; d.t = v0.t; d.u = v1.u; d.v = v0.v;
    rasterize_triangle(a, d, b);
    rasterize_triangle(a, b, c);
}

bool GsvkRenderer::present_frame(const std::string& dump_bmp_path, bool show_win32_window, bool throttle) {
    if (throttle) {
        frame_limiter_.end_frame();
    }

    u32 disp_w = width_;
    u32 disp_h = height_;
    const u32* pixels_to_present = color_buffer_.data();

    if (dispfb1_reg_ != 0 || dispfb2_reg_ != 0) {
        if (resolve_display_framebuffer(presentation_buffer_, disp_w, disp_h) && !presentation_buffer_.empty()) {
            pixels_to_present = presentation_buffer_.data();
        }
    }

    if (show_win32_window || window_enabled_) {
        if (!window_.is_open()) {
            std::string win_title = "EmotionLayer - PlayStation 2 Native Port [Vulkan/AVX2 | " +
                                    std::to_string(disp_w) + "x" + std::to_string(disp_h) + "]";
            window_.create(win_title, disp_w, disp_h);
        }
        window_.present_buffer(pixels_to_present, disp_w, disp_h);
        window_.process_events();
    }

    const u32 total_px = disp_w * disp_h;
    u32 lit = 0;
    u32 hash = 2166136261U;
    for (u32 i = 0; i < total_px; ++i) {
        const u32 px = pixels_to_present[i];
        if (px != 0xFF140C1CU && px != 0 && px != 0xFF000000U) lit++;
        hash = (hash ^ px) * 16777619U;
    }
    stats_.lit_pixels_count = lit;
    stats_.frame_checksum   = hash;

    u32 edram_nonzero = 0;
    for (u8 b : edram_) if (b != 0) edram_nonzero++;

    static u32 present_count = 0;
    if (++present_count % 60 == 0) {
        std::printf("[GSVK Present] Frame %u (%ux%u): GIF pkts=%llu, Prims=%llu, LitPx=%u, EdramNZ=%u, Chk=0x%08X\n",
            present_count, disp_w, disp_h, (unsigned long long)stats_.gif_packets_processed, (unsigned long long)stats_.primitives_drawn, stats_.lit_pixels_count, edram_nonzero, stats_.frame_checksum);
        std::fflush(stdout);
    }

    if (!dump_bmp_path.empty()) {
        #pragma pack(push, 1)
        struct BmpHeader {
            u16 magic      = 0x4D42; // 'BM'
            u32 file_size  = 0;
            u32 reserved   = 0;
            u32 data_off   = 54;
            u32 info_size  = 40;
            s32 width      = 0;
            s32 height     = 0;
            u16 planes     = 1;
            u16 bpp        = 32;
            u32 comp       = 0;
            u32 img_size   = 0;
            s32 x_ppm      = 2835;
            s32 y_ppm      = 2835;
            u32 clr_used   = 0;
            u32 clr_imp    = 0;
        };
        #pragma pack(pop)

        const u32 img_bytes = disp_w * disp_h * 4U;
        BmpHeader hdr{};
        hdr.file_size = 54U + img_bytes;
        hdr.width     = static_cast<s32>(disp_w);
        hdr.height    = -static_cast<s32>(disp_h); // Top-down DIB
        hdr.img_size  = img_bytes;

        std::ofstream out(dump_bmp_path, std::ios::binary);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(&hdr), sizeof(BmpHeader));

        // Convertir RGBA a BGRA para formato BMP estándar de Windows
        std::vector<u32> bgra(total_px);
        for (std::size_t i = 0; i < total_px; ++i) {
            const u32 c = pixels_to_present[i];
            const u32 r = c & 0xFFU;
            const u32 g = (c >> 8) & 0xFFU;
            const u32 b = (c >> 16) & 0xFFU;
            const u32 a = (c >> 24) & 0xFFU;
            bgra[i] = b | (g << 8) | (r << 16) | (a << 24);
        }
        out.write(reinterpret_cast<const char*>(bgra.data()), img_bytes);
    }
    return true;
}

} // namespace emotion
