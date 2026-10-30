#pragma once

#include "../common/types.hpp"
#include <string>
#include <vector>
#include <map>

namespace emotion {

struct IsoFileEntry {
    std::string path;      // Ruta normalizada en mayúsculas, ej. "\SCUS_999.99;1" o "\MODULES\PADMAN.IRX;1"
    u32         lba  = 0;  // Logical Block Address (sector de 2048 bytes)
    u32         size = 0;  // Tamaño en bytes
    bool        is_dir = false;
};

// Lector nativo de imágenes ópticas ISO9660 de PlayStation 2
class IsoReader {
public:
    static constexpr u32 kSectorSize = 2048;

    IsoReader() noexcept;
    ~IsoReader();

    IsoReader(const IsoReader&) = delete;
    IsoReader& operator=(const IsoReader&) = delete;

    IsoReader(IsoReader&& other) noexcept;
    IsoReader& operator=(IsoReader&& other) noexcept;

    void close() noexcept;

    // Abre una imagen .ISO de PS2 mediante Memory-Mapped Files (cero copia de RAM, soporte hasta 8.5 GB DVD9)
    bool open(const std::string& iso_path, std::string& out_error);

    // Parsea SYSTEM.CNF dentro de la ISO y devuelve la ruta interna del ejecutable BOOT2 (ej. "\SCUS_999.99;1")
    bool parse_system_cnf(std::string& out_boot_elf_path, std::string& out_version, std::string& out_error) const;

    // Busca un archivo dentro de la ISO (insensible a mayúsculas, ignora prefijos "cdrom0:" y sufijos ";1")
    bool find_file(const std::string& guest_path, IsoFileEntry& out_entry) const;

    // Extrae los bytes completos de un archivo dentro de la ISO
    bool extract_file(const std::string& guest_path, std::vector<u8>& out_bytes) const;

    // Lee sectores crudos de 2048 bytes desde la ISO (usado por sceCdRead en CDVDMAN HLE)
    bool read_sectors(u32 lba, u32 sector_count, u8* dst_buffer) const;

    const std::string& iso_path() const noexcept { return iso_path_; }
    const std::vector<IsoFileEntry>& entries() const noexcept { return entries_; }
    u64 file_size() const noexcept { return file_size_; }

    static std::string normalize_ps2_path(const std::string& raw_path);

private:
    void parse_directory_record(u32 dir_lba, u32 dir_size, const std::string& parent_prefix);
    const u8* get_sector_data(u32 lba) const noexcept;

    std::string iso_path_;
    void* file_handle_    = reinterpret_cast<void*>(-1); // INVALID_HANDLE_VALUE
    void* mapping_handle_ = nullptr;
    const u8* mapped_data_ = nullptr;
    u64 file_size_        = 0;
    u32 sector_size_      = 2048; // 2048 (ISO estandar) o 2352 (RAW CD MODE2/2352)
    u32 header_offset_    = 0;    // 0 (ISO) o 24 (Mode 2 Form 1) o 16 (Mode 1)

    std::vector<u8> fallback_buffer_; // Buffer de respaldo en caso de que MapViewOfFile no esté disponible
    std::vector<IsoFileEntry> entries_;
    std::map<std::string, std::size_t> lookup_map_;
};

} // namespace emotion
