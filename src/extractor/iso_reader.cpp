#include "iso_reader.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace emotion {

IsoReader::IsoReader() noexcept
    : file_handle_(INVALID_HANDLE_VALUE)
    , mapping_handle_(nullptr)
    , mapped_data_(nullptr)
    , file_size_(0)
    , sector_size_(2048)
    , header_offset_(0) {}

IsoReader::~IsoReader() {
    close();
}

IsoReader::IsoReader(IsoReader&& other) noexcept
    : iso_path_(std::move(other.iso_path_))
    , file_handle_(other.file_handle_)
    , mapping_handle_(other.mapping_handle_)
    , mapped_data_(other.mapped_data_)
    , file_size_(other.file_size_)
    , sector_size_(other.sector_size_)
    , header_offset_(other.header_offset_)
    , fallback_buffer_(std::move(other.fallback_buffer_))
    , entries_(std::move(other.entries_))
    , lookup_map_(std::move(other.lookup_map_)) {
    other.file_handle_    = INVALID_HANDLE_VALUE;
    other.mapping_handle_ = nullptr;
    other.mapped_data_   = nullptr;
    other.file_size_      = 0;
    other.sector_size_    = 2048;
    other.header_offset_  = 0;
}

IsoReader& IsoReader::operator=(IsoReader&& other) noexcept {
    if (this != &other) {
        close();
        iso_path_        = std::move(other.iso_path_);
        file_handle_    = other.file_handle_;
        mapping_handle_ = other.mapping_handle_;
        mapped_data_   = other.mapped_data_;
        file_size_      = other.file_size_;
        sector_size_    = other.sector_size_;
        header_offset_  = other.header_offset_;
        fallback_buffer_ = std::move(other.fallback_buffer_);
        entries_        = std::move(other.entries_);
        lookup_map_     = std::move(other.lookup_map_);

        other.file_handle_    = INVALID_HANDLE_VALUE;
        other.mapping_handle_ = nullptr;
        other.mapped_data_   = nullptr;
        other.file_size_      = 0;
        other.sector_size_    = 2048;
        other.header_offset_  = 0;
    }
    return *this;
}

void IsoReader::close() noexcept {
    if (mapped_data_ && mapped_data_ != fallback_buffer_.data()) {
        UnmapViewOfFile(mapped_data_);
        mapped_data_ = nullptr;
    }
    if (mapping_handle_) {
        CloseHandle(static_cast<HANDLE>(mapping_handle_));
        mapping_handle_ = nullptr;
    }
    if (file_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(file_handle_));
        file_handle_ = INVALID_HANDLE_VALUE;
    }
    fallback_buffer_.clear();
    entries_.clear();
    lookup_map_.clear();
    file_size_     = 0;
    sector_size_   = 2048;
    header_offset_ = 0;
    iso_path_.clear();
}

std::string IsoReader::normalize_ps2_path(const std::string& raw_path) {
    std::string s = raw_path;
    const auto colon = s.find(':');
    if (colon != std::string::npos) {
        s = s.substr(colon + 1);
    }
    for (char& c : s) {
        if (c == '/') c = '\\';
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (s.size() >= 2 && s.substr(s.size() - 2) == ";1") {
        s.resize(s.size() - 2);
    }
    if (s.empty() || s[0] != '\\') {
        s = "\\" + s;
    }
    return s;
}

const u8* IsoReader::get_sector_data(u32 lba) const noexcept {
    const u64 off = static_cast<u64>(lba) * sector_size_ + header_offset_;
    if (off + 2048 > file_size_ || !mapped_data_) return nullptr;
    return mapped_data_ + off;
}

bool IsoReader::open(const std::string& iso_path, std::string& out_error) {
    close();
    iso_path_ = iso_path;

    // Apertura eficiente por Win32 Memory-Mapped File
    HANDLE hFile = CreateFileA(
        iso_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER liSize;
        if (GetFileSizeEx(hFile, &liSize)) {
            file_size_ = static_cast<u64>(liSize.QuadPart);
            if (file_size_ >= static_cast<u64>(17 * 2048)) {
                HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
                if (hMap != nullptr) {
                    const void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
                    if (view != nullptr) {
                        file_handle_    = hFile;
                        mapping_handle_ = hMap;
                        mapped_data_   = static_cast<const u8*>(view);
                    } else {
                        CloseHandle(hMap);
                    }
                }
            }
        }
        if (!mapped_data_) {
            CloseHandle(hFile);
            file_handle_ = INVALID_HANDLE_VALUE;
        }
    }

    if (!mapped_data_) {
        std::ifstream f(iso_path, std::ios::binary | std::ios::ate);
        if (!f) {
            out_error = "No se pudo abrir la imagen ISO/BIN: " + iso_path;
            return false;
        }
        const auto total_sz = f.tellg();
        if (total_sz < static_cast<std::streamoff>(17 * 2048)) {
            out_error = "Archivo ISO/BIN demasiado pequeno.";
            return false;
        }
        file_size_ = static_cast<u64>(total_sz);
        fallback_buffer_.resize(static_cast<std::size_t>(total_sz));
        f.seekg(0, std::ios::beg);
        f.read(reinterpret_cast<char*>(fallback_buffer_.data()), total_sz);
        mapped_data_ = fallback_buffer_.data();
    }

    // Auto-detección del tipo de imagen óptica:
    // 1. ISO9660 estándar de DVD o CD normalizado (2048 bytes/sector, header=0)
    // 2. CD-ROM RAW MODE2/2352 (Form 1: 2352 bytes/sector, header=24)
    // 3. CD-ROM RAW MODE1/2352 (2352 bytes/sector, header=16)
    const u8* pvd = nullptr;

    if (file_size_ >= static_cast<u64>(17 * 2048)) {
        const u8* test_pvd = mapped_data_ + 16 * 2048;
        if (test_pvd[0] == 0x01 && std::memcmp(test_pvd + 1, "CD001", 5) == 0) {
            sector_size_   = 2048;
            header_offset_ = 0;
            pvd = test_pvd;
        }
    }

    if (!pvd && file_size_ >= static_cast<u64>(17 * 2352)) {
        const u8* test_pvd = mapped_data_ + 16 * 2352 + 24;
        if (test_pvd[0] == 0x01 && std::memcmp(test_pvd + 1, "CD001", 5) == 0) {
            sector_size_   = 2352;
            header_offset_ = 24;
            pvd = test_pvd;
        }
    }

    if (!pvd && file_size_ >= static_cast<u64>(17 * 2352)) {
        const u8* test_pvd = mapped_data_ + 16 * 2352 + 16;
        if (test_pvd[0] == 0x01 && std::memcmp(test_pvd + 1, "CD001", 5) == 0) {
            sector_size_   = 2352;
            header_offset_ = 16;
            pvd = test_pvd;
        }
    }

    if (!pvd) {
        out_error = "Descriptor de volumen ISO9660 (CD001) invalido en el sector 16 (verificado en ISO y RAW BIN).";
        close();
        return false;
    }

    // Root Directory Record comienza en el byte 156 del PVD
    const u8* root_rec = pvd + 156;
    u32 root_lba  = 0;
    u32 root_size = 0;
    std::memcpy(&root_lba,  root_rec + 2,  sizeof(u32));
    std::memcpy(&root_size, root_rec + 10, sizeof(u32));

    entries_.clear();
    lookup_map_.clear();
    parse_directory_record(root_lba, root_size, "");
    return true;
}

void IsoReader::parse_directory_record(u32 dir_lba, u32 dir_size, const std::string& parent_prefix) {
    if (dir_size == 0) return;
    std::vector<u8> dir_bytes(dir_size);
    const u32 sector_count = (dir_size + 2047) / 2048;
    for (u32 s = 0; s < sector_count; ++s) {
        const u8* src = get_sector_data(dir_lba + s);
        if (!src) return;
        const u32 chunk = std::min(2048u, dir_size - s * 2048);
        std::memcpy(dir_bytes.data() + s * 2048, src, chunk);
    }

    u32 pos = 0;
    while (pos < dir_size) {
        const u8 rec_len = dir_bytes[pos];
        if (rec_len == 0) {
            const u32 next_sector = ((pos / 2048) + 1) * 2048;
            if (next_sector <= pos) break;
            pos = next_sector;
            continue;
        }
        if (pos + rec_len > dir_size) break;

        const u8* rec = dir_bytes.data() + pos;
        u32 extent_lba = 0;
        u32 data_len   = 0;
        std::memcpy(&extent_lba, rec + 2,  sizeof(u32));
        std::memcpy(&data_len,   rec + 10, sizeof(u32));
        const u8 flags    = rec[25];
        const u8 name_len = rec[32];

        if (name_len > 0 && 33 + name_len <= rec_len) {
            const char* raw_name = reinterpret_cast<const char*>(rec + 33);
            if (!(name_len == 1 && (raw_name[0] == 0x00 || raw_name[0] == 0x01))) {
                std::string name(raw_name, name_len);
                const bool is_dir = (flags & 0x02) != 0;
                std::string full_path = parent_prefix + "\\" + name;

                IsoFileEntry entry{};
                entry.path   = full_path;
                entry.lba    = extent_lba;
                entry.size   = data_len;
                entry.is_dir = is_dir;

                const std::string norm_key = normalize_ps2_path(full_path);
                lookup_map_[norm_key] = entries_.size();
                entries_.push_back(entry);

                if (is_dir && extent_lba != dir_lba) {
                    parse_directory_record(extent_lba, data_len, norm_key);
                }
            }
        }
        pos += rec_len;
    }
}

bool IsoReader::find_file(const std::string& guest_path, IsoFileEntry& out_entry) const {
    const std::string key = normalize_ps2_path(guest_path);
    auto it = lookup_map_.find(key);
    if (it == lookup_map_.end()) return false;
    out_entry = entries_[it->second];
    return true;
}

bool IsoReader::extract_file(const std::string& guest_path, std::vector<u8>& out_bytes) const {
    IsoFileEntry entry{};
    if (!find_file(guest_path, entry) || entry.is_dir) return false;

    out_bytes.resize(entry.size);
    u32 remaining = entry.size;
    u32 current_lba = entry.lba;
    u32 dst_off = 0;
    while (remaining > 0) {
        const u8* src = get_sector_data(current_lba);
        if (!src) return false;
        const u32 chunk = std::min(remaining, 2048u);
        std::memcpy(out_bytes.data() + dst_off, src, chunk);
        dst_off += chunk;
        remaining -= chunk;
        current_lba++;
    }
    return true;
}

bool IsoReader::read_sectors(u32 lba, u32 sector_count, u8* dst_buffer) const {
    for (u32 s = 0; s < sector_count; ++s) {
        const u8* src = get_sector_data(lba + s);
        if (!src) return false;
        std::memcpy(dst_buffer + s * 2048, src, 2048);
    }
    return true;
}

bool IsoReader::parse_system_cnf(std::string& out_boot_elf_path, std::string& out_version, std::string& out_error) const {
    std::vector<u8> cnf_bytes;
    if (!extract_file("\\SYSTEM.CNF;1", cnf_bytes)) {
        out_error = "No se encontro SYSTEM.CNF en la raiz de la imagen ISO/BIN.";
        return false;
    }

    std::string content(reinterpret_cast<const char*>(cnf_bytes.data()), cnf_bytes.size());
    out_boot_elf_path.clear();
    out_version = "1.00";

    std::size_t pos = 0;
    while (pos < content.size()) {
        std::size_t end = content.find_first_of("\r\n", pos);
        if (end == std::string::npos) end = content.size();
        std::string line = content.substr(pos, end - pos);
        pos = end + 1;

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        auto trim = [](std::string& str) {
            while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front()))) str.erase(str.begin());
            while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back())))  str.pop_back();
        };
        trim(key);
        trim(val);

        if (key == "BOOT2") {
            out_boot_elf_path = val;
        } else if (key == "VER") {
            out_version = val;
        }
    }

    if (out_boot_elf_path.empty()) {
        out_error = "SYSTEM.CNF no contiene una entrada BOOT2 valida.";
        return false;
    }
    return true;
}

} // namespace emotion
