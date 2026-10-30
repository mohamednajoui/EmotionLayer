#include "sif_iop_hle.hpp"
#include "../memory_arena.hpp"
#include "../ee_kernel/kernel_runtime.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace {

// Estructuras de XInput cargadas dinámicamente desde xinput1_4.dll para evitar dependencias rígidas de lib
#pragma pack(push, 1)
struct HostXInputGamepad {
    emotion::u16 wButtons;
    emotion::u8  bLeftTrigger;
    emotion::u8  bRightTrigger;
    emotion::s16 sThumbLX;
    emotion::s16 sThumbLY;
    emotion::s16 sThumbRX;
    emotion::s16 sThumbRY;
};

struct HostXInputState {
    emotion::u32      dwPacketNumber;
    HostXInputGamepad Gamepad;
};
#pragma pack(pop)

using PFN_XInputGetState = DWORD(WINAPI*)(DWORD dwUserIndex, HostXInputState* pState);

PFN_XInputGetState get_xinput_fn() {
    static bool initialized = false;
    static PFN_XInputGetState fn = nullptr;
    if (!initialized) {
        initialized = true;
        HMODULE hMod = LoadLibraryW(L"xinput1_4.dll");
        if (!hMod) hMod = LoadLibraryW(L"xinput9_1_0.dll");
        if (hMod) {
            fn = reinterpret_cast<PFN_XInputGetState>(GetProcAddress(hMod, "XInputGetState"));
        }
    }
    return fn;
}

} // namespace

namespace emotion {

std::vector<SifIopHle*> SifIopHle::instances_;
SifIopHle* SifIopHle::active_instance_ = nullptr;

void SifIopHle::register_instance(SifIopHle* inst) noexcept {
    if (!inst) return;
    auto it = std::find(instances_.begin(), instances_.end(), inst);
    if (it == instances_.end()) {
        instances_.push_back(inst);
    }
    active_instance_ = inst;
}

void SifIopHle::unregister_instance(SifIopHle* inst) noexcept {
    if (!inst) return;
    auto it = std::find(instances_.begin(), instances_.end(), inst);
    if (it != instances_.end()) {
        instances_.erase(it);
    }
    if (active_instance_ == inst) {
        active_instance_ = instances_.empty() ? nullptr : instances_.back();
    }
}

bool SifIopHle::mount_iso(const std::string& iso_path, std::string& out_error) {
    if (!iso_.open(iso_path, out_error)) {
        iso_mounted_ = false;
        return false;
    }
    iso_mounted_ = true;
    return true;
}

s32 SifIopHle::sif_load_module(u32 guest_path_vaddr) {
    std::string raw_path;
    if (!MemoryArena::safe_read_string(fastmem_base_, guest_path_vaddr, raw_path, 260)) {
        return -1;
    }
    std::string norm = IsoReader::normalize_ps2_path(raw_path);

    std::string hle_driver = "iop_custom_r3000_hle.dll";
    if (norm.find("SIO2MAN") != std::string::npos) {
        hle_driver = "win_sio2man.builtin";
    } else if (norm.find("PADMAN") != std::string::npos || norm.find("XPADMAN") != std::string::npos) {
        hle_driver = "win_padman_xinput.builtin";
    } else if (norm.find("CDVD") != std::string::npos) {
        hle_driver = "win_cdvd_iso_vfs.builtin";
    } else if (norm.find("MCMAN") != std::string::npos || norm.find("MCSERV") != std::string::npos) {
        hle_driver = "win_mcman_folder.builtin";
    } else if (norm.find("LIBSD") != std::string::npos || norm.find("SDRDRV") != std::string::npos) {
        hle_driver = "win_spu2_wasapi.builtin";
    }

    const s32 mod_id = static_cast<s32>(loaded_modules_.size() + 1);
    loaded_modules_.push_back(LoadedIrxModule{mod_id, raw_path, hle_driver});
    return mod_id;
}

s32 SifIopHle::cdvd_init(s32 mode) {
    (void)mode;
    return 1;
}

s32 SifIopHle::cdvd_search_file(u32 cdl_file_vaddr, u32 path_vaddr) {
    if (!iso_mounted_ || !fastmem_base_) return 0;
    if (!MemoryArena::is_valid_guest_range(cdl_file_vaddr, sizeof(Ps2CdlFile))) return 0;

    std::string guest_path;
    if (!MemoryArena::safe_read_string(fastmem_base_, path_vaddr, guest_path, 260)) {
        return 0;
    }

    Ps2CdlFile cdl{};
    if (guest_path.empty() || guest_path == "\\" || guest_path == "/") {
        cdl.lsn  = 23;
        cdl.size = 2048;
        cdl.name[0] = '\0';
        MemoryArena::fast_write<Ps2CdlFile>(fastmem_base_, cdl_file_vaddr, cdl);
        std::printf("[CDVD SearchFile] Root directory found -> LSN=23 Size=2048\n");
        std::fflush(stdout);
        return 1;
    }
    IsoFileEntry entry{};
    if (!iso_.find_file(guest_path, entry)) {
        std::printf("[CDVD SearchFile] FAILED: '%s'\n", guest_path.c_str());
        std::fflush(stdout);
        return 0;
    }

    cdl.lsn  = entry.lba;
    cdl.size = entry.size;
    std::string name_only = entry.path;
    const auto slash = name_only.find_last_of("\\/");
    if (slash != std::string::npos) {
        name_only = name_only.substr(slash + 1);
    }
    std::strncpy(cdl.name, name_only.c_str(), sizeof(cdl.name) - 1);
    cdl.name[sizeof(cdl.name) - 1] = '\0';
    MemoryArena::fast_write<Ps2CdlFile>(fastmem_base_, cdl_file_vaddr, cdl);
    std::printf("[CDVD SearchFile] '%s' -> LSN=%u Size=%u Name='%s'\n", guest_path.c_str(), cdl.lsn, cdl.size, cdl.name);
    std::fflush(stdout);
    return 1;
}

s32 SifIopHle::cdvd_read(u32 lsn, u32 sectors, u32 dst_vaddr) {
    if (!iso_mounted_ || !fastmem_base_) return 0;
    const u32 total_bytes = sectors * 2048;
    if (!MemoryArena::is_valid_guest_range(dst_vaddr, total_bytes)) return 0;

    u8* dst = fastmem_base_ + dst_vaddr;
    const bool ok = iso_.read_sectors(lsn, sectors, dst);
    std::printf("[CDVD Read] LSN=%u Sectors=%u Dst=0x%08X (bytes=%u) -> %s\n",
        lsn, sectors, dst_vaddr, total_bytes, ok ? "OK" : "ERROR");
    std::fflush(stdout);
    return ok ? 1 : 0;
}

bool SifIopHle::queue_cdvd_read(u32 lsn, u32 sectors, u32 dst_vaddr,
                                u32 client_addr, u32 client_pkt_addr, u32 sema_id,
                                u32 recv_vaddr, u32 recv_size, u32 delay_ticks) {
    if (has_pending_command()) {
        std::printf("[CDVD Async Read REJECTED] Comando previo todavia en vuelo (cmd=%u)\n",
            static_cast<u32>(pending_cmd_.cmd));
        std::fflush(stdout);
        return false;
    }

    pending_cmd_.cmd = CdvdPendingCmd::Read;
    pending_cmd_.lsn = lsn;
    pending_cmd_.sectors = sectors;
    pending_cmd_.dst_vaddr = dst_vaddr;
    pending_cmd_.ticks_remaining = (delay_ticks > 0) ? delay_ticks : 1;
    pending_cmd_.client_addr = client_addr;
    pending_cmd_.client_pkt_addr = client_pkt_addr;
    pending_cmd_.sema_id = sema_id;
    pending_cmd_.recv_vaddr = recv_vaddr;
    pending_cmd_.recv_size = recv_size;

    // Marcamos ocupado el paquete del cliente si esta mapeado
    if (client_pkt_addr != 0 && fastmem_base_ != nullptr) {
        const u32 phys_cp = client_pkt_addr & 0x1FFFFFFFU;
        if (phys_cp + 28 <= 32 * 1024 * 1024) {
            u32* cp = reinterpret_cast<u32*>(fastmem_base_ + phys_cp);
            cp[4] |= 1U; // Bit de busy activado
        }
    }
    std::printf("[CDVD Async Read Enqueued] LSN=%u Sectors=%u Dst=0x%08X Ticks=%u Sema=%u\n",
        lsn, sectors, dst_vaddr, pending_cmd_.ticks_remaining, sema_id);
    std::fflush(stdout);
    return true;
}

void SifIopHle::queue_ndiskready(u32 client_addr, u32 client_pkt_addr, u32 sema_id,
                                 u32 recv_vaddr, u32 recv_size, u32 delay_ticks) {
    pending_cmd_.cmd = CdvdPendingCmd::NDiskReady;
    pending_cmd_.ticks_remaining = (delay_ticks > 0) ? delay_ticks : 1;
    pending_cmd_.client_addr = client_addr;
    pending_cmd_.client_pkt_addr = client_pkt_addr;
    pending_cmd_.sema_id = sema_id;
    pending_cmd_.recv_vaddr = recv_vaddr;
    pending_cmd_.recv_size = recv_size;
}

void SifIopHle::count_ticks(u32 ticks, EEKernelRuntime* krnl) {
    if (pending_cmd_.cmd == CdvdPendingCmd::None) return;
    if (pending_cmd_.ticks_remaining > ticks) {
        pending_cmd_.ticks_remaining -= ticks;
    } else {
        pending_cmd_.ticks_remaining = 0;
        finish_pending_command(krnl);
    }
}

void SifIopHle::finish_pending_command(EEKernelRuntime* krnl) {
    if (pending_cmd_.cmd == CdvdPendingCmd::None) return;

    const auto cmd = pending_cmd_.cmd;
    const u32 client_pkt_addr = pending_cmd_.client_pkt_addr;
    const u32 sema_id = pending_cmd_.sema_id;
    const u32 recv_vaddr = pending_cmd_.recv_vaddr;
    const u32 recv_size = pending_cmd_.recv_size;

    if (cmd == CdvdPendingCmd::Read) {
        bool ok = false;
        if (iso_mounted_ && fastmem_base_ != nullptr) {
            const u32 phys_dst = pending_cmd_.dst_vaddr & 0x1FFFFFFFU;
            if (phys_dst + pending_cmd_.sectors * 2048 <= 32 * 1024 * 1024) {
                ok = iso_.read_sectors(pending_cmd_.lsn, pending_cmd_.sectors, fastmem_base_ + phys_dst);
            }
        }
        if (recv_vaddr != 0 && recv_size >= 4 && fastmem_base_ != nullptr) {
            const u32 phys_recv = recv_vaddr & 0x1FFFFFFFU;
            if (phys_recv + 4 <= 32 * 1024 * 1024) {
                *reinterpret_cast<u32*>(fastmem_base_ + phys_recv) = ok ? 0U : 0xFFFFFFFFU;
            }
        }
        if (ok) {
            std::printf("[CDVD Async Read Finished] LSN=%u Sectors=%u -> Data delivered to EE RAM 0x%08X\n",
                pending_cmd_.lsn, pending_cmd_.sectors, pending_cmd_.dst_vaddr);
        } else {
            std::printf("[CDVD Async Read FAILED] LSN=%u Sectors=%u -> Read error reported to EE (status=0xFFFFFFFF)\n",
                pending_cmd_.lsn, pending_cmd_.sectors);
        }
        std::fflush(stdout);
    } else if (cmd == CdvdPendingCmd::NDiskReady) {
        if (recv_vaddr != 0 && recv_size >= 4 && fastmem_base_ != nullptr) {
            const u32 phys_recv = recv_vaddr & 0x1FFFFFFFU;
            if (phys_recv + 4 <= 32 * 1024 * 1024) {
                *reinterpret_cast<u32*>(fastmem_base_ + phys_recv) = 2U; // 2 = CDVD_DRV_READY
            }
        }
    }

    // Limpiar busy flag en el paquete del cliente
    if (client_pkt_addr != 0 && fastmem_base_ != nullptr) {
        const u32 phys_cp = client_pkt_addr & 0x1FFFFFFFU;
        if (phys_cp + 28 <= 32 * 1024 * 1024) {
            u32* cp = reinterpret_cast<u32*>(fastmem_base_ + phys_cp);
            cp[4] &= ~1U; // Fin de busy
        }
    }

    // Señalizar el semaforo de finalizacion del cliente EE
    if (sema_id > 0 && krnl != nullptr) {
        krnl->sys_signal_sema(static_cast<s32>(sema_id));
    }

    // Reset de estado pendiente
    pending_cmd_ = CdvdPendingState{};
}

u32 SifIopHle::get_ndiskready_status() const noexcept {
    // Si hay comando pendiente (lectura en vuelo), el lector reporta ocupado (6)
    if (pending_cmd_.cmd != CdvdPendingCmd::None) {
        return 6U; // CDVD_DRV_NOT_READY
    }
    return 2U;     // CDVD_DRV_READY
}

s32 SifIopHle::cdvd_sync(s32 mode, EEKernelRuntime* krnl) {
    if (mode == 0 || mode == 0x10) {
        // Bloqueante: esperar a que finalice cualquier comando encolado
        if (has_pending_command()) {
            finish_pending_command(krnl);
        }
        return 0; // Finalizado
    }
    // No bloqueante (poll): si hay comando pendiente, avanza 1 tick y devuelve 1 (busy) o 0 (listo)
    if (has_pending_command()) {
        count_ticks(1, krnl);
        return has_pending_command() ? 1 : 0;
    }
    return 0;
}

s32 SifIopHle::pad_init(s32 mode) {
    (void)mode;
    return 1;
}

s32 SifIopHle::pad_port_open(s32 port, s32 slot, u32 pad_area_vaddr) {
    (void)slot;
    if (port < 0 || port >= 2) return 0;
    if (pad_area_vaddr != 0 && !MemoryArena::is_valid_guest_range(pad_area_vaddr, 32)) {
        return 0; // Rechazar buffer guest fuera de límites
    }
    pad_dma_buf_[port] = pad_area_vaddr;
    return 1;
}

s32 SifIopHle::pad_read(s32 port, s32 slot, u32 dst_data_32b_vaddr) {
    (void)slot;
    if (!fastmem_base_ || port < 0 || port >= 2) return 0;

    u32 target_vaddr = dst_data_32b_vaddr;
    if (target_vaddr == 0) {
        target_vaddr = pad_dma_buf_[port];
    }
    // Verificación estricta de límites: nunca escribir en memoria guest inválida
    if (target_vaddr == 0 || !MemoryArena::is_valid_guest_range(target_vaddr, 32)) {
        return 0;
    }

    u8 data[32];
    std::memset(data, 0, sizeof(data));

    // Formato nativo DualShock 2 (ID = 0x79: Modo Analógico + 12 sensores de presión)
    data[0] = 0x00; // Status OK (0x00)
    data[1] = 0x79; // Mode ID: DualShock 2 (0x79)
    data[2] = 0xFF; // Botones digitales byte 0 (active-low: 0xFF = ninguno pulsado)
    data[3] = 0xFF; // Botones digitales byte 1 (active-low: 0xFF = ninguno pulsado)
    data[4] = 0x80; // Right Stick X (centro = 0x80)
    data[5] = 0x80; // Right Stick Y (centro = 0x80)
    data[6] = 0x80; // Left Stick X  (centro = 0x80)
    data[7] = 0x80; // Left Stick Y  (centro = 0x80)

    // Consultar mando real en Windows mediante XInput si hay uno conectado
    if (auto pXInputGetState = get_xinput_fn()) {
        HostXInputState xstate{};
        if (pXInputGetState(static_cast<DWORD>(port), &xstate) == ERROR_SUCCESS) {
            u16 btns = 0xFFFFU;
            const u16 xb = xstate.Gamepad.wButtons;
            if (xb & 0x0010) btns &= ~(1U << 3);  // Start
            if (xb & 0x0020) btns &= ~(1U << 0);  // Select / Back
            if (xb & 0x0001) btns &= ~(1U << 4);  // D-Pad Up
            if (xb & 0x0008) btns &= ~(1U << 5);  // D-Pad Right
            if (xb & 0x0002) btns &= ~(1U << 6);  // D-Pad Down
            if (xb & 0x0004) btns &= ~(1U << 7);  // D-Pad Left
            if (xb & 0x1000) btns &= ~(1U << 14); // A -> Cross (X)
            if (xb & 0x2000) btns &= ~(1U << 13); // B -> Circle
            if (xb & 0x4000) btns &= ~(1U << 15); // X -> Square
            if (xb & 0x8000) btns &= ~(1U << 12); // Y -> Triangle

            data[2] = static_cast<u8>(btns & 0xFFU);
            data[3] = static_cast<u8>((btns >> 8) & 0xFFU);
            data[4] = static_cast<u8>((static_cast<s32>(xstate.Gamepad.sThumbRX) + 32768) >> 8);
            data[5] = static_cast<u8>((32767 - static_cast<s32>(xstate.Gamepad.sThumbRY)) >> 8);
            data[6] = static_cast<u8>((static_cast<s32>(xstate.Gamepad.sThumbLX) + 32768) >> 8);
            data[7] = static_cast<u8>((32767 - static_cast<s32>(xstate.Gamepad.sThumbLY)) >> 8);
        }
    }

    std::memcpy(fastmem_base_ + target_vaddr, data, sizeof(data));
    return 32;
}

std::string SifIopHle::resolve_mc_host_path(const std::string& guest_mc_path) const {
    if (guest_mc_path.empty()) return "";

    std::string rel = guest_mc_path;
    const auto colon = rel.find(':');
    std::string slot_folder = "mc0";
    if (colon != std::string::npos) {
        const std::string slot_prefix = rel.substr(0, colon);
        rel = rel.substr(colon + 1);
        if (slot_prefix == "mc0" || slot_prefix == "0") {
            slot_folder = "mc0";
        } else if (slot_prefix == "mc1" || slot_prefix == "1") {
            slot_folder = "mc1";
        } else {
            // Rechazar prefijos o ranuras inválidas
            return "";
        }
    }

    // Normalizar separadores
    for (char& c : rel) {
        if (c == '/') c = '\\';
    }

    // Sanitizar componentes relativos y rechazar terminantemente '..' (Path Traversal)
    std::filesystem::path rel_path(rel);
    std::filesystem::path sanitized_rel;

    for (const auto& part : rel_path) {
        const std::string s = part.string();
        if (s.empty() || s == "." || s == "\\") continue;
        if (s == "..") {
            return ""; // Intento de Directory Traversal detectado y bloqueado
        }
        for (char ch : s) {
            if (static_cast<unsigned char>(ch) < 32 || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') {
                return ""; // Caracteres inválidos para el sistema de archivos
            }
        }
        sanitized_rel /= part;
    }

    if (sanitized_rel.empty()) {
        return "";
    }

    std::filesystem::path root(mc_root_dir_);
    std::filesystem::path full_path = root / slot_folder / sanitized_rel;
    full_path = full_path.lexically_normal();

    // Verificación estricta de prefijo: full_path debe residir bajo root.lexically_normal()
    const auto norm_root = root.lexically_normal();
    auto it_root = norm_root.begin();
    auto it_full = full_path.begin();
    while (it_root != norm_root.end()) {
        if (it_full == full_path.end() || *it_full != *it_root) {
            return "";
        }
        ++it_root;
        ++it_full;
    }

    return full_path.string();
}

s32 SifIopHle::mc_get_info(s32 port, s32 slot, u32 type_vaddr, u32 free_vaddr, u32 format_vaddr) {
    (void)port; (void)slot;
    if (!fastmem_base_) return -1;
    if (type_vaddr && MemoryArena::is_valid_guest_range(type_vaddr, sizeof(s32)))
        MemoryArena::fast_write<s32>(fastmem_base_, type_vaddr, 2);        // 2 = PS2 8MB Card
    if (free_vaddr && MemoryArena::is_valid_guest_range(free_vaddr, sizeof(s32)))
        MemoryArena::fast_write<s32>(fastmem_base_, free_vaddr, 999999);   // Espacio virtualmente ilimitado en PC
    if (format_vaddr && MemoryArena::is_valid_guest_range(format_vaddr, sizeof(s32)))
        MemoryArena::fast_write<s32>(fastmem_base_, format_vaddr, 1);      // 1 = Formateada
    return 0;
}

s32 SifIopHle::mc_write_file(u32 guest_mc_path_vaddr, u32 src_data_vaddr, u32 byte_count) {
    if (!fastmem_base_) return -1;
    std::string guest_path;
    if (!MemoryArena::safe_read_string(fastmem_base_, guest_mc_path_vaddr, guest_path, 260)) {
        return -1;
    }
    const std::string host_path = resolve_mc_host_path(guest_path);
    if (host_path.empty()) {
        return -1;
    }

    // Límite de seguridad: archivos de Memory Card no deben exceder 8 MB ni desbordar la memoria del guest
    if (byte_count > 8 * 1024 * 1024 || !MemoryArena::is_valid_guest_range(src_data_vaddr, byte_count)) {
        return -1;
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(host_path).parent_path(), ec);

    std::ofstream out(host_path, std::ios::binary);
    if (!out) return -1;
    out.write(reinterpret_cast<const char*>(fastmem_base_ + src_data_vaddr), byte_count);
    return static_cast<s32>(byte_count);
}

s32 SifIopHle::mc_read_file(u32 guest_mc_path_vaddr, u32 dst_data_vaddr, u32 max_bytes) {
    if (!fastmem_base_) return -1;
    std::string guest_path;
    if (!MemoryArena::safe_read_string(fastmem_base_, guest_mc_path_vaddr, guest_path, 260)) {
        return -1;
    }
    const std::string host_path = resolve_mc_host_path(guest_path);
    if (host_path.empty()) {
        return -1;
    }

    // Límite de seguridad para buffer de destino
    if (max_bytes > 8 * 1024 * 1024 || !MemoryArena::is_valid_guest_range(dst_data_vaddr, max_bytes)) {
        return -1;
    }

    std::ifstream in(host_path, std::ios::binary | std::ios::ate);
    if (!in) return -1;
    const auto sz = static_cast<u32>(in.tellg());
    const u32 to_read = (sz < max_bytes) ? sz : max_bytes;
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(fastmem_base_ + dst_data_vaddr), to_read);
    return static_cast<s32>(to_read);
}

s32 SifIopHle::spu_upload_sample(u32 sram_addr, u32 guest_src_vaddr, u32 byte_count) {
    if (!fastmem_base_) return -1;
    if (!MemoryArena::is_valid_guest_range(guest_src_vaddr, byte_count)) return -1;
    spu2_.upload_sample_data(sram_addr, fastmem_base_ + guest_src_vaddr, byte_count);
    return static_cast<s32>(byte_count);
}

s32 SifIopHle::spu_voice_key_on(u32 voice, u32 sram_addr, u16 pitch, s16 vol_l, s16 vol_r) {
    spu2_.voice_key_on(voice, sram_addr, pitch, vol_l, vol_r);
    return 0;
}

s32 SifIopHle::spu_export_wav(u32 guest_filepath_vaddr, u32 frames) {
    if (!fastmem_base_) return -1;
    std::string filename;
    if (!MemoryArena::safe_read_string(fastmem_base_, guest_filepath_vaddr, filename, 260)) {
        return -1;
    }
    // Confinar: aislar solo el nombre de archivo sin rutas relativas ni absolutas del host
    std::filesystem::path p(filename);
    std::string clean_name = p.filename().string();
    if (clean_name.empty()) clean_name = "spu2_dump.wav";
    if (!clean_name.ends_with(".wav")) clean_name += ".wav";

    const std::string host_path = "build\\" + clean_name;
    return spu2_.export_to_wav(host_path, frames) ? 0 : -1;
}

s32 SifIopHle::run_custom_irx_module(u32 guest_irx_path_vaddr) {
    if (!fastmem_base_) return -1;
    std::string guest_path;
    if (!MemoryArena::safe_read_string(fastmem_base_, guest_irx_path_vaddr, guest_path, 260)) {
        return -1;
    }

    std::vector<u8> irx_bytes;
    if (iso_mounted_) {
        iso_.extract_file(guest_path, irx_bytes);
    }

    // P0 Seguridad: El guest nunca puede abrir archivos locales del host fuera de la ISO montada.
    if (irx_bytes.empty()) return -1;

    u32 entry = 0;
    std::string err;
    if (!iop_lifter_.load_irx(irx_bytes, 0x00010000U, entry, err)) {
        return -2;
    }
    return iop_lifter_.execute(entry, 0, 0);
}

} // namespace emotion
