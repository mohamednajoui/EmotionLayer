#pragma once

#include "../../common/types.hpp"
#include "../../extractor/iso_reader.hpp"
#include "win_spu2_audio.hpp"
#include "../../lifter/iop_r3000_lifter.hpp"
#include <string>
#include <vector>

namespace emotion {

class EEKernelRuntime;

// Estado de comandos asíncronos diferidos de CDVD (semántica fiel de retardo y polling)
enum class CdvdPendingCmd : u32 {
    None = 0,
    Read = 1,
    NDiskReady = 2,
    Pause = 3
};

struct CdvdPendingState {
    CdvdPendingCmd cmd = CdvdPendingCmd::None;
    u32 lsn = 0;
    u32 sectors = 0;
    u32 dst_vaddr = 0;
    u32 ticks_remaining = 0;
    u32 client_addr = 0;
    u32 client_pkt_addr = 0;
    u32 sema_id = 0;
    u32 recv_vaddr = 0;
    u32 recv_size = 0;
};

// Estructura estándar sceCdlFILE de <libcdvd.h> en el SDK de PlayStation 2
#pragma pack(push, 1)
struct Ps2CdlFile {
    u32  lsn;       // Logical Sector Number (LBA)
    u32  size;      // Tamaño del archivo en bytes
    char name[16];  // Nombre corto ISO9660
    u8   date[8];   // Timestamp
    u32  flag;
};
#pragma pack(pop)

struct LoadedIrxModule {
    s32         mod_id = 0;
    std::string guest_path;
    std::string hle_driver_name;
};

// Capa "PS2-Wine" para el bus SIF RPC y reemplazo nativo (HLE) de módulos .IRX del IOP en Windows
class SifIopHle {
public:
    SifIopHle() noexcept {
        register_instance(this);
    }
    ~SifIopHle() noexcept {
        unregister_instance(this);
    }

    // Monta una imagen .ISO de PS2 para que CDVDMAN.IRX y SifLoadModule lean archivos de ella
    bool mount_iso(const std::string& iso_path, std::string& out_error);
    IsoReader& iso_reader() noexcept { return iso_; }
    const IsoReader& iso_reader() const noexcept { return iso_; }
    bool is_iso_mounted() const noexcept { return iso_mounted_; }

    void set_fastmem_base(u8* base) noexcept { fastmem_base_ = base; }
    void set_mc_root_dir(const std::string& dir) { mc_root_dir_ = dir; }

    // --- SIF Module Loader (sceSifLoadModule) ---
    s32 sif_load_module(u32 guest_path_vaddr);

    // --- CDVDMAN.IRX / libcdvd Native HLE ---
    s32 cdvd_init(s32 mode);
    s32 cdvd_search_file(u32 cdl_file_vaddr, u32 path_vaddr);
    s32 cdvd_read(u32 lsn, u32 sectors, u32 dst_vaddr);
    s32 cdvd_sync(s32 mode, EEKernelRuntime* krnl = nullptr);

    // --- Comandos Asíncronos Diferidos CDVDFSV (Semántica de timing real) ---
    bool has_pending_command() const noexcept { return pending_cmd_.cmd != CdvdPendingCmd::None; }
    CdvdPendingCmd pending_command_type() const noexcept { return pending_cmd_.cmd; }

    bool queue_cdvd_read(u32 lsn, u32 sectors, u32 dst_vaddr,
                         u32 client_addr, u32 client_pkt_addr, u32 sema_id,
                         u32 recv_vaddr = 0, u32 recv_size = 0, u32 delay_ticks = 4);
    void queue_ndiskready(u32 client_addr, u32 client_pkt_addr, u32 sema_id,
                          u32 recv_vaddr = 0, u32 recv_size = 0, u32 delay_ticks = 1);

    void count_ticks(u32 ticks, EEKernelRuntime* krnl = nullptr);
    void finish_pending_command(EEKernelRuntime* krnl = nullptr);
    u32  get_ndiskready_status() const noexcept;

    // --- PADMAN.IRX / libpad Native HLE (Windows XInput + Win32 Keyboard -> DualShock 2) ---
    s32 pad_init(s32 mode);
    s32 pad_port_open(s32 port, s32 slot, u32 pad_area_vaddr);
    s32 pad_read(s32 port, s32 slot, u32 dst_data_32b_vaddr);

    // --- MCMAN.IRX / libmc Native HLE (Guardado directo en carpetas de Windows sin límite de 8 MB) ---
    s32 mc_get_info(s32 port, s32 slot, u32 type_vaddr, u32 free_vaddr, u32 format_vaddr);
    s32 mc_write_file(u32 guest_mc_path_vaddr, u32 src_data_vaddr, u32 byte_count);
    s32 mc_read_file(u32 guest_mc_path_vaddr, u32 dst_data_vaddr, u32 max_bytes);
    std::string resolve_mc_host_path(const std::string& guest_mc_path) const;

    // --- LIBSD.IRX / SPU2 Audio Native HLE (Fase 4) ---
    s32 spu_upload_sample(u32 sram_addr, u32 guest_src_vaddr, u32 byte_count);
    s32 spu_voice_key_on(u32 voice, u32 sram_addr, u16 pitch, s16 vol_l, s16 vol_r);
    s32 spu_export_wav(u32 guest_filepath_vaddr, u32 frames);

    // --- Ejecutor de módulos .IRX personalizados MIPS R3000A (Fase 4) ---
    s32 run_custom_irx_module(u32 guest_irx_path_vaddr);

    Spu2AudioEngine& spu2() noexcept { return spu2_; }
    IopR3000Lifter&  iop_lifter() noexcept { return iop_lifter_; }

    static SifIopHle* active_instance() noexcept { return active_instance_; }
    static void set_active_instance(SifIopHle* inst) noexcept { active_instance_ = inst; }
    static void register_instance(SifIopHle* inst) noexcept;
    static void unregister_instance(SifIopHle* inst) noexcept;

private:
    u8*                          fastmem_base_ = nullptr;
    IsoReader                    iso_;
    bool                         iso_mounted_  = false;
    std::string                  mc_root_dir_  = "build\\saves";
    std::vector<LoadedIrxModule> loaded_modules_;
    u32                          pad_dma_buf_[2] = {0, 0};
    Spu2AudioEngine              spu2_;
    IopR3000Lifter               iop_lifter_;
    CdvdPendingState             pending_cmd_{};

    static std::vector<SifIopHle*> instances_;
    static SifIopHle* active_instance_;
};

} // namespace emotion
