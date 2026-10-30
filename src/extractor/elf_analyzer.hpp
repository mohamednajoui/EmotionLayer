#pragma once

#include "../common/types.hpp"
#include "../runtime_wine/memory_arena.hpp"
#include <string>
#include <vector>
#include <map>

namespace emotion {

#pragma pack(push, 1)
struct Elf32_Ehdr {
    u8  e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u32 e_entry;
    u32 e_phoff;
    u32 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
};

struct Elf32_Phdr {
    u32 p_type;
    u32 p_offset;
    u32 p_vaddr;
    u32 p_paddr;
    u32 p_filesz;
    u32 p_memsz;
    u32 p_flags;
    u32 p_align;
};

struct Elf32_Shdr {
    u32 sh_name;
    u32 sh_type;
    u32 sh_flags;
    u32 sh_addr;
    u32 sh_offset;
    u32 sh_size;
    u32 sh_link;
    u32 sh_info;
    u32 sh_addralign;
    u32 sh_entsize;
};

struct Elf32_Sym {
    u32 st_name;
    u32 st_value;
    u32 st_size;
    u8  st_info;
    u8  st_other;
    u16 st_shndx;
};
#pragma pack(pop)

struct LoadSegment {
    u32 vaddr;
    u32 memsz;
    u32 flags; // PF_X = 1, PF_W = 2, PF_R = 4
    std::vector<u8> data;
};

struct FunctionRange {
    u32 start_vaddr;
    u32 end_vaddr; // exclusivo
    std::string name;
    bool is_heuristic = false;
};

struct Ps2ElfImage {
    std::string filepath;
    u32 entry_point = 0;
    u32 elf_flags   = 0;
    u32 gp_value    = 0;
    bool has_explicit_function_map = false;
    std::vector<LoadSegment> segments;
    std::vector<FunctionRange> functions;
    std::map<u32, std::string> symbols;

    // Lee una instrucción de 32 bits desde los segmentos cargados en la imagen ELF
    bool read_insn(u32 vaddr, u32& out_word) const noexcept {
        for (const auto& seg : segments) {
            if (vaddr >= seg.vaddr && vaddr + 4 <= seg.vaddr + seg.data.size()) {
                const u32 off = vaddr - seg.vaddr;
                std::memcpy(&out_word, seg.data.data() + off, sizeof(u32));
                return true;
            }
        }
        return false;
    }

    // Carga todos los segmentos PT_LOAD en la Arena de Memoria de 4 GB en Windows
    bool load_into_arena(u8* fastmem_base) const noexcept {
        if (!fastmem_base) return false;
        for (const auto& seg : segments) {
            if (seg.memsz == 0) continue;
            u8* dst = MemoryArena::get_guest_ptr<u8>(fastmem_base, seg.vaddr, seg.memsz);
            if (!dst) {
                return false;
            }
            std::memset(dst, 0, seg.memsz);
            if (!seg.data.empty() && seg.data.size() <= seg.memsz) {
                std::memcpy(dst, seg.data.data(), seg.data.size());
            }
        }
        return true;
    }
};

class ElfAnalyzer {
public:
    static constexpr u16 kEM_MIPS   = 8;
    static constexpr u16 kET_EXEC   = 2;
    static constexpr u16 kET_DYN    = 3;
    static constexpr u32 kPT_LOAD   = 1;
    static constexpr u32 kPF_X      = 1;
    static constexpr u32 kSHT_SYMTAB = 2;
    static constexpr u32 kSHT_STRTAB = 3;

    // Analiza un ejecutable .ELF de PS2 desde disco o desde memoria y descubre sus funciones
    static bool parse_file(const std::string& path, Ps2ElfImage& out_image, std::string& out_error);
    static bool parse_bytes(const std::vector<u8>& bytes, const std::string& label, Ps2ElfImage& out_image, std::string& out_error);

    // Carga un mapa explícito de funciones (CSV: 'address,size,name' o 'address,name')
    // evitando el escaneo heurístico ciego sobre segmentos de datos/padding.
    static bool load_function_map_csv(const std::string& path_or_content, Ps2ElfImage& out_image, std::string& out_error);

private:
    static void discover_functions_cfg(Ps2ElfImage& img);
};

} // namespace emotion
