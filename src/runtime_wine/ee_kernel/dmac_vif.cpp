#include "dmac_vif.hpp"
#include "../memory_arena.hpp"
#include <cstring>
#include <algorithm>

namespace emotion {

std::vector<DmacVifController*> DmacVifController::instances_;
DmacVifController* DmacVifController::active_instance_ = nullptr;

void DmacVifController::register_instance(DmacVifController* inst) noexcept {
    if (!inst) return;
    auto it = std::find(instances_.begin(), instances_.end(), inst);
    if (it == instances_.end()) {
        instances_.push_back(inst);
    }
    active_instance_ = inst;
}

void DmacVifController::unregister_instance(DmacVifController* inst) noexcept {
    if (!inst) return;
    auto it = std::find(instances_.begin(), instances_.end(), inst);
    if (it != instances_.end()) {
        instances_.erase(it);
    }
    if (active_instance_ == inst) {
        active_instance_ = instances_.empty() ? nullptr : instances_.back();
    }
}

void DmacVifController::dispatch_payload(DmaChannel ch, const u8* payload_ptr, u32 qwc) {
    if (!payload_ptr || qwc == 0) return;
    const u32 bytes = qwc * 16U;

    switch (ch) {
        case DmaChannel::VIF1:
            process_vif1_stream(payload_ptr, bytes, 0);
            break;
        case DmaChannel::GIF:
            if (gs_) {
                gs_->submit_gif_packet(payload_ptr, bytes, 3); // PATH3
            }
            break;
        default:
            break;
    }
}

void DmacVifController::dispatch_vif1_tag_and_payload(u64 tag_hi, const u8* payload_ptr, u32 qwc, bool is_contiguous) {
    if (!payload_ptr && qwc == 0 && tag_hi == 0) return;

    if (tag_hi == 0) {
        if (payload_ptr && qwc > 0) {
            process_vif1_stream(payload_ptr, qwc * 16U, 0);
        }
        return;
    }

    // Tag Transfer Enabled (TTE=1): tag_hi (8 bytes) precede al payload (qwc * 16 bytes)
    if (is_contiguous && payload_ptr) {
        const u8* contiguous_tag_base = payload_ptr - 16U;
        const u32 total_bytes = 16U + qwc * 16U;
        process_vif1_stream(contiguous_tag_base, total_bytes, 8U);
        return;
    }

    const u32 payload_bytes = qwc * 16U;
    const u32 total_bytes = 16U + payload_bytes;

    alignas(16) u8 small_buf[1024];
    std::vector<u8> dynamic_buf;
    u8* buf_ptr = small_buf;
    if (total_bytes > sizeof(small_buf)) {
        dynamic_buf.resize(total_bytes);
        buf_ptr = dynamic_buf.data();
    }

    std::memset(buf_ptr, 0, 16);
    std::memcpy(buf_ptr + 8U, &tag_hi, 8U);
    if (payload_ptr && payload_bytes > 0) {
        std::memcpy(buf_ptr + 16U, payload_ptr, payload_bytes);
    }

    process_vif1_stream(buf_ptr, total_bytes, 8U);
}

namespace {

inline u32 dma_resolve_addr(u32 raw_addr, bool spr_flag) noexcept {
    if (spr_flag || (raw_addr & 0x80000000U) || ((raw_addr >> 28) == 0x7) || ((raw_addr >> 28) == 0xF) || ((raw_addr >> 28) == 0xD)) {
        return 0x70000000U | (raw_addr & 0x3FF0U);
    }
    return raw_addr & 0x01FFFFFFU;
}

inline bool dma_check_bounds(u32 addr, u32 qwc) noexcept {
    const u32 bytes = qwc * 16U;
    if (addr >= 0x70000000U && addr <= 0x70004000U) {
        return (addr + bytes <= 0x70004000U);
    }
    return (addr + bytes <= 32U * 1024U * 1024U);
}

} // namespace

u32 DmacVifController::transfer_normal(DmaChannel ch, u32 madr_vaddr, u32 qwc) {
    if (!fastmem_base_ || qwc == 0) return 0;
    const bool spr_bit = (madr_vaddr & 0x80000000U) || ((madr_vaddr >> 28) == 0x7);
    const u32 addr = dma_resolve_addr(madr_vaddr, spr_bit);
    if (!dma_check_bounds(addr, qwc)) return 0;
    dispatch_payload(ch, fastmem_base_ + addr, qwc);
    return qwc;
}

DmacVifController::DmaChainResult DmacVifController::transfer_source_chain(DmaChannel ch, u32 tadr_vaddr) {
    DmaChainResult res{};
    if (!fastmem_base_) return res;

    const bool initial_spr = (tadr_vaddr & 0x80000000U) || ((tadr_vaddr >> 28) == 0x7);
    u32 cur_tadr  = dma_resolve_addr(tadr_vaddr, initial_spr);
    u32 total_qwc = 0;
    bool end_chain = false;
    u32 last_tag_w0 = 0;
    bool had_irq = false;
    u32 asr[2] = { 0, 0 };

    for (u32 safety = 0; safety < 8192 && !end_chain; ++safety) {
        if (!dma_check_bounds(cur_tadr, 1)) break;
        u64 tag_lo = 0, tag_hi = 0;
        std::memcpy(&tag_lo, fastmem_base_ + cur_tadr,     8);
        std::memcpy(&tag_hi, fastmem_base_ + cur_tadr + 8, 8);

        const u32 tag_w0 = static_cast<u32>(tag_lo & 0xFFFFFFFFULL);
        last_tag_w0 = tag_w0;
        if (tag_w0 & 0x80000000U) {
            had_irq = true;
        }

        const u16 qwc       = static_cast<u16>(tag_lo & 0xFFFFULL);
        const auto id       = static_cast<DmaTagId>((tag_lo >> 28) & 0x07ULL);
        const bool spr_flag = ((tag_lo >> 63) & 1ULL) != 0;
        const u32 raw_addr  = static_cast<u32>((tag_lo >> 32) & 0xFFFFFFFFULL);
        const u32 addr      = dma_resolve_addr(raw_addr, spr_flag);

        if (qwc == 0 && tag_hi != 0 && ch == DmaChannel::VIF1) {
            dispatch_vif1_tag_and_payload(tag_hi, nullptr, 0, false);
        }

        switch (id) {
            case DmaTagId::REFE: { // 0: Transfiere desde ADDR y termina la cadena
                if (dma_check_bounds(addr, qwc)) {
                    if (ch == DmaChannel::VIF1) {
                        dispatch_vif1_tag_and_payload(tag_hi, fastmem_base_ + addr, qwc, false);
                    } else {
                        dispatch_payload(ch, fastmem_base_ + addr, qwc);
                    }
                }
                total_qwc += qwc;
                end_chain = true;
                break;
            }
            case DmaTagId::CNT: { // 1: Transfiere QWC contiguos al tag y avanza al siguiente
                const u32 data_addr = cur_tadr + 16U;
                if (dma_check_bounds(data_addr, qwc)) {
                    if (ch == DmaChannel::VIF1) {
                        dispatch_vif1_tag_and_payload(tag_hi, fastmem_base_ + data_addr, qwc, true);
                    } else {
                        dispatch_payload(ch, fastmem_base_ + data_addr, qwc);
                    }
                }
                total_qwc += qwc;
                cur_tadr  = data_addr + qwc * 16U;
                break;
            }
            case DmaTagId::NEXT: { // 2: Transfiere QWC contiguos y salta al tag en ADDR
                const u32 data_addr = cur_tadr + 16U;
                if (dma_check_bounds(data_addr, qwc)) {
                    if (ch == DmaChannel::VIF1) {
                        dispatch_vif1_tag_and_payload(tag_hi, fastmem_base_ + data_addr, qwc, true);
                    } else {
                        dispatch_payload(ch, fastmem_base_ + data_addr, qwc);
                    }
                }
                total_qwc += qwc;
                cur_tadr   = addr;
                break;
            }
            case DmaTagId::REF:   // 3: Transfiere QWC desde ADDR y avanza al siguiente tag contiguo
            case DmaTagId::REFS: { // 4: REFS (Stall control)
                if (dma_check_bounds(addr, qwc)) {
                    if (ch == DmaChannel::VIF1) {
                        dispatch_vif1_tag_and_payload(tag_hi, fastmem_base_ + addr, qwc, false);
                    } else {
                        dispatch_payload(ch, fastmem_base_ + addr, qwc);
                    }
                }
                total_qwc += qwc;
                cur_tadr  += 16U;
                break;
            }
            case DmaTagId::CALL: { // 5: Subrutina DMA (guarda retorno en ASR y salta a ADDR)
                const u32 data_addr = cur_tadr + 16U;
                if (dma_check_bounds(data_addr, qwc)) {
                    if (ch == DmaChannel::VIF1) {
                        dispatch_vif1_tag_and_payload(tag_hi, fastmem_base_ + data_addr, qwc, true);
                    } else {
                        dispatch_payload(ch, fastmem_base_ + data_addr, qwc);
                    }
                }
                total_qwc += qwc;
                asr[1] = asr[0];
                asr[0] = data_addr + qwc * 16U;
                cur_tadr = addr;
                break;
            }
            case DmaTagId::RET: { // 6: Retorno de subrutina DMA desde ASR
                const u32 data_addr = cur_tadr + 16U;
                if (dma_check_bounds(data_addr, qwc)) {
                    if (ch == DmaChannel::VIF1) {
                        dispatch_vif1_tag_and_payload(tag_hi, fastmem_base_ + data_addr, qwc, true);
                    } else {
                        dispatch_payload(ch, fastmem_base_ + data_addr, qwc);
                    }
                }
                total_qwc += qwc;
                cur_tadr = asr[0];
                asr[0] = asr[1];
                asr[1] = 0;
                break;
            }
            case DmaTagId::END: { // 7: Transfiere QWC contiguos y termina la cadena
                const u32 data_addr = cur_tadr + 16U;
                if (dma_check_bounds(data_addr, qwc)) {
                    if (ch == DmaChannel::VIF1) {
                        dispatch_vif1_tag_and_payload(tag_hi, fastmem_base_ + data_addr, qwc, true);
                    } else {
                        dispatch_payload(ch, fastmem_base_ + data_addr, qwc);
                    }
                }
                total_qwc += qwc;
                end_chain = true;
                break;
            }
            default:
                end_chain = true;
                break;
        }
    }

    res.total_qwc     = total_qwc;
    res.last_tag_bits = (last_tag_w0 & 0xFFFF0000U) | (had_irq ? 0x80000000U : 0U);
    res.final_tadr    = cur_tadr;
    return res;
}

void DmacVifController::process_vif1_stream(const u8* stream_ptr, u32 size_bytes, u32 start_offset) {
    if (!stream_ptr || size_bytes < 4) return;

    u32 offset = start_offset;
    while (offset + 4 <= size_bytes) {
        u32 vifcode = 0;
        std::memcpy(&vifcode, stream_ptr + offset, 4);
        offset += 4;

        const u16 imm = static_cast<u16>(vifcode & 0xFFFFU);
        const u8  num = static_cast<u8>((vifcode >> 16) & 0xFFU);
        const u8  cmd = static_cast<u8>((vifcode >> 24) & 0x7FU);
        static u32 log_vif = 0;
        if (cmd != 0 && log_vif++ < 35) {
            std::printf("[VIF1 Code] cmd=0x%02X num=%u imm=%u off=%u/%u\n", cmd, num, imm, offset, size_bytes);
            std::fflush(stdout);
        }

        switch (cmd) {
            case 0x00: // NOP
            case 0x01: // STCYCL
            case 0x02: // OFFSET
            case 0x03: // BASE
            case 0x04: // ITOP
            case 0x05: // STMOD
            case 0x10: // FLUSHE
            case 0x11: // FLUSH
            case 0x13: // FLUSHA
                break;

            case 0x14:   // MSCAL (Ejecuta microprograma VU1 en dirección imm * 8)
            case 0x17: { // MSCNT
                if (vu1_) {
                    vu1_->execute_mscal(static_cast<u32>(imm) * 8U);
                }
                break;
            }

            case 0x4A: { // MPG (Sube num instrucciones de 64 bits a VU1_MICRO en dirección imm * 8)
                const u32 count = (num == 0) ? 256U : static_cast<u32>(num);
                const u32 byte_len = count * 8U;
                if (offset + byte_len <= size_bytes && vu1_) {
                    vu1_->upload_microcode(static_cast<u32>(imm) * 8U, stream_ptr + offset, byte_len);
                }
                offset += byte_len;
                break;
            }

            case 0x50:   // DIRECT (Envía imm quadwords de 128 bits directamente al GS por PATH2)
            case 0x51: { // DIRECTHL
                const u32 qwords = (imm == 0) ? 65536U : static_cast<u32>(imm);
                // Alinear offset a 16 bytes
                offset = (offset + 15U) & ~15U;
                const u32 byte_len = qwords * 16U;
                if (offset + byte_len > size_bytes) {
                    static u32 log_dir_ovf = 0;
                    if (log_dir_ovf++ < 10) {
                        std::printf("[VIF1 DIRECT OVERFLOW] qwords=%u byte_len=%u off=%u size=%u\n",
                            qwords, byte_len, offset, size_bytes);
                        std::fflush(stdout);
                    }
                }
                if (offset + byte_len <= size_bytes && gs_) {
                    gs_->submit_gif_packet(stream_ptr + offset, byte_len, 2); // PATH2
                }
                offset += byte_len;
                break;
            }

            default: {
                // Comandos UNPACK (0x60..0x7F)
                if ((cmd & 0x60) == 0x60) {
                    const u32 dst_qw = static_cast<u32>(imm & 0x3FFU);
                    const u32 qwords = (num == 0) ? 256U : static_cast<u32>(num);
                    const u32 vn = (cmd & 0x03U) + 1U; // 1, 2, 3, o 4 componentes
                    const u32 vl = (cmd >> 2) & 0x03U; // 0=32b, 1=16b, 2=8b, 3=5-5-5-1
                    u32 vec_bytes = 16U;
                    if (vl == 0) vec_bytes = vn * 4U;
                    else if (vl == 1) vec_bytes = vn * 2U;
                    else if (vl == 2) vec_bytes = vn * 1U;
                    else if (vl == 3) vec_bytes = 2U; // V4-5

                    for (u32 q = 0; q < qwords; ++q) {
                        if (offset + vec_bytes > size_bytes) break;
                        r128 val = r128::zero();
                        if (vl == 0) {
                            std::memcpy(&val, stream_ptr + offset, (vec_bytes < 16U) ? vec_bytes : 16U);
                            if (vn == 3) {
                                *reinterpret_cast<f32*>(&val.uw[3]) = 1.0f;
                            }
                        } else if (vl == 1) {
                            const u16* s16 = reinterpret_cast<const u16*>(stream_ptr + offset);
                            for (u32 c = 0; c < vn && c < 4; ++c) val.uw[c] = s16[c];
                            if (vn < 4) val.uw[3] = 1;
                        } else if (vl == 2) {
                            const u8* s8 = stream_ptr + offset;
                            for (u32 c = 0; c < vn && c < 4; ++c) val.uw[c] = s8[c];
                            if (vn < 4) val.uw[3] = 1;
                        } else if (vl == 3) {
                            const u16 c16 = *reinterpret_cast<const u16*>(stream_ptr + offset);
                            val.uw[0] = (c16 & 0x1F) << 3;
                            val.uw[1] = ((c16 >> 5) & 0x1F) << 3;
                            val.uw[2] = ((c16 >> 10) & 0x1F) << 3;
                            val.uw[3] = ((c16 >> 15) & 0x01) ? 255 : 0;
                        }
                        offset += vec_bytes;
                        if (vu1_) {
                            vu1_->write_data_qword(dst_qw + q, val);
                        }
                    }
                    offset = (offset + 3U) & ~3U; // Alinear fin de UNPACK a 4 bytes
                }
                break;
            }
        }
    }
}

} // namespace emotion
