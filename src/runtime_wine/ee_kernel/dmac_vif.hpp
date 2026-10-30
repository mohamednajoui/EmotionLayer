#pragma once

#include "../../common/types.hpp"
#include "../../lifter/vu_compiler.hpp"
#include "../../gsvk/gsvk_renderer.hpp"
#include <vector>

namespace emotion {

// Canales del controlador DMA (DMAC) del Emotion Engine
enum class DmaChannel : u8 {
    VIF0   = 0,
    VIF1   = 1,
    GIF    = 2,
    fromIPU = 3,
    toIPU  = 4,
    SIF0   = 5,
    SIF1   = 6,
    SIF2   = 7,
    fromSPR = 8,
    toSPR  = 9
};

// IDs de DMAtag de 64 bits en el modo Source Chain del Emotion Engine
enum class DmaTagId : u8 {
    REFE = 0, // Transfiere desde ADDR y termina la cadena
    CNT  = 1, // Transfiere QWC posteriores al tag y continúa al siguiente tag
    NEXT = 2, // Transfiere QWC posteriores al tag y salta a ADDR
    REF  = 3, // Transfiere QWC desde ADDR y continúa al tag siguiente
    REFS = 4,
    CALL = 5,
    RET  = 6,
    END  = 7  // Transfiere QWC posteriores al tag y termina la cadena
};

// Controlador DMAC y procesador de paquetes VIF1 de la PlayStation 2
class DmacVifController {
public:
    DmacVifController() noexcept {
        register_instance(this);
    }
    ~DmacVifController() noexcept {
        unregister_instance(this);
    }

    void bind(u8* fastmem_base, Vu1Engine* vu1, GsvkRenderer* gs) noexcept {
        fastmem_base_ = fastmem_base;
        vu1_          = vu1;
        gs_           = gs;
    }

    struct DmaChainResult {
        u32 total_qwc     = 0;
        u32 last_tag_bits = 0; // Bits 16..31 del último DMAtag (incluye tag ID e IRQ)
        u32 final_tadr    = 0;
    };

    // Transferencia DMA en Modo Normal (madr + qwc quadwords de 128 bits)
    u32 transfer_normal(DmaChannel ch, u32 madr_vaddr, u32 qwc);

    // Transferencia DMA en Modo Source Chain (tadr apunta a lista enlazada de DMAtags de 128 bits)
    DmaChainResult transfer_source_chain(DmaChannel ch, u32 tadr_vaddr);

    // Procesa un buffer de comandos VIF1 (MPG, UNPACK, MSCAL, DIRECT)
    void process_vif1_stream(const u8* stream_ptr, u32 size_bytes, u32 start_offset = 0);

    static DmacVifController* active_instance() noexcept { return active_instance_; }
    static void set_active_instance(DmacVifController* inst) noexcept { active_instance_ = inst; }
    static void register_instance(DmacVifController* inst) noexcept;
    static void unregister_instance(DmacVifController* inst) noexcept;

private:
    void dispatch_payload(DmaChannel ch, const u8* payload_ptr, u32 qwc);
    void dispatch_vif1_tag_and_payload(u64 tag_hi, const u8* payload_ptr, u32 qwc, bool is_contiguous);

    u8*           fastmem_base_ = nullptr;
    Vu1Engine*    vu1_          = nullptr;
    GsvkRenderer* gs_           = nullptr;

    static std::vector<DmacVifController*> instances_;
    static DmacVifController* active_instance_;
};

} // namespace emotion
