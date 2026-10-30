#include "elf_analyzer.hpp"
#include "../runtime_wine/memory_arena.hpp"
#include <fstream>
#include <sstream>
#include <set>
#include <algorithm>
#include <cstdio>

namespace emotion {

bool ElfAnalyzer::parse_file(const std::string& path, Ps2ElfImage& out_image, std::string& out_error) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        out_error = "No se pudo abrir el archivo ELF: " + path;
        return false;
    }
    const auto sz = f.tellg();
    if (sz < static_cast<std::streamoff>(sizeof(Elf32_Ehdr))) {
        out_error = "Archivo demasiado pequeno para ser un ELF32 valido.";
        return false;
    }
    std::vector<u8> buf(static_cast<std::size_t>(sz));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    return parse_bytes(buf, path, out_image, out_error);
}

bool ElfAnalyzer::parse_bytes(const std::vector<u8>& bytes, const std::string& label, Ps2ElfImage& out_image, std::string& out_error) {
    if (bytes.size() < sizeof(Elf32_Ehdr)) {
        out_error = "Buffer menor que Elf32_Ehdr.";
        return false;
    }

    Elf32_Ehdr ehdr{};
    std::memcpy(&ehdr, bytes.data(), sizeof(Elf32_Ehdr));

    // 1. Verificación de firma ELF (\x7F E L F), 32-bit (1), Little-Endian (1), Versión (1)
    if (ehdr.e_ident[0] != 0x7F || ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L'  || ehdr.e_ident[3] != 'F') {
        out_error = "Firma magica ELF invalida.";
        return false;
    }
    if (ehdr.e_ident[4] != 1) { // ELFCLASS32
        out_error = "El ELF no es de 32 bits (ELFCLASS32 requerido).";
        return false;
    }
    if (ehdr.e_ident[5] != 1) { // ELFDATA2LSB
        out_error = "El ELF no es Little-Endian (ELFDATA2LSB requerido por el Emotion Engine).";
        return false;
    }
    if (ehdr.e_ident[6] != 1) { // EV_CURRENT
        out_error = "Version ELF invalida en e_ident[6] (se esperaba EV_CURRENT=1).";
        return false;
    }

    // 2. Tipo y Arquitectura
    if (ehdr.e_type != kET_EXEC && ehdr.e_type != kET_DYN) {
        out_error = "Tipo de ELF no ejecutable (e_type no es ET_EXEC ni ET_DYN).";
        return false;
    }
    if (ehdr.e_machine != kEM_MIPS) {
        out_error = "El ELF no corresponde a arquitectura MIPS (e_machine != 8).";
        return false;
    }
    if (ehdr.e_ehsize < sizeof(Elf32_Ehdr)) {
        out_error = "Tamano de cabecera ELF (e_ehsize) menor que sizeof(Elf32_Ehdr).";
        return false;
    }

    out_image = Ps2ElfImage{};
    out_image.filepath    = label;
    out_image.entry_point = ehdr.e_entry;
    out_image.elf_flags   = ehdr.e_flags;

    // 3. Parsear Program Headers (PT_LOAD) con comprobaciones de límites y overflow
    if (ehdr.e_phnum == 0 || ehdr.e_phoff == 0) {
        out_error = "El ELF no contiene tabla de Program Headers.";
        return false;
    }
    if (ehdr.e_phentsize < sizeof(Elf32_Phdr)) {
        out_error = "Tamano de entrada de Program Header (e_phentsize) invalido.";
        return false;
    }
    const u64 ph_end = static_cast<u64>(ehdr.e_phoff) + static_cast<u64>(ehdr.e_phnum) * ehdr.e_phentsize;
    if (ph_end > bytes.size()) {
        out_error = "Tabla de Program Headers excede el tamano del archivo ELF.";
        return false;
    }

    for (u16 i = 0; i < ehdr.e_phnum; ++i) {
        const std::size_t off = ehdr.e_phoff + static_cast<std::size_t>(i) * ehdr.e_phentsize;
        Elf32_Phdr phdr{};
        std::memcpy(&phdr, bytes.data() + off, sizeof(Elf32_Phdr));
        if (phdr.p_type != kPT_LOAD) continue;

        if (phdr.p_filesz > phdr.p_memsz) {
            out_error = "Segmento PT_LOAD corrupto: p_filesz mayor que p_memsz.";
            return false;
        }

        const u64 file_end = static_cast<u64>(phdr.p_offset) + phdr.p_filesz;
        if (file_end > bytes.size()) {
            out_error = "Segmento PT_LOAD truncado en el archivo ELF.";
            return false;
        }

        const u64 mem_end = static_cast<u64>(phdr.p_vaddr) + phdr.p_memsz;
        if (mem_end > 0x100000000ULL) {
            out_error = "Segmento PT_LOAD desborda el espacio virtual de 32 bits.";
            return false;
        }

        LoadSegment seg{};
        seg.vaddr = phdr.p_vaddr;
        seg.memsz = phdr.p_memsz;
        seg.flags = phdr.p_flags;
        seg.data.resize(phdr.p_filesz);
        if (phdr.p_filesz > 0) {
            std::memcpy(seg.data.data(), bytes.data() + phdr.p_offset, phdr.p_filesz);
        }
        out_image.segments.push_back(std::move(seg));
    }

    if (out_image.segments.empty()) {
        out_error = "El ELF no contiene ningun segmento cargable (PT_LOAD).";
        return false;
    }

    // 4. Parsear Section Headers y Tabla de Símbolos con lectura acotada de strings
    if (ehdr.e_shoff > 0 && ehdr.e_shnum > 0 && ehdr.e_shentsize >= sizeof(Elf32_Shdr)) {
        const u64 sh_end = static_cast<u64>(ehdr.e_shoff) + static_cast<u64>(ehdr.e_shnum) * ehdr.e_shentsize;
        if (sh_end <= bytes.size()) {
            std::vector<Elf32_Shdr> shdrs(ehdr.e_shnum);
            for (u16 i = 0; i < ehdr.e_shnum; ++i) {
                const std::size_t off = ehdr.e_shoff + static_cast<std::size_t>(i) * ehdr.e_shentsize;
                std::memcpy(&shdrs[i], bytes.data() + off, sizeof(Elf32_Shdr));
            }

            for (u16 i = 0; i < ehdr.e_shnum; ++i) {
                if (shdrs[i].sh_type == kSHT_SYMTAB && shdrs[i].sh_link < ehdr.e_shnum) {
                    const auto& sym_sec = shdrs[i];
                    const auto& str_sec = shdrs[sym_sec.sh_link];
                    const u64 sym_end = static_cast<u64>(sym_sec.sh_offset) + sym_sec.sh_size;
                    const u64 str_end = static_cast<u64>(str_sec.sh_offset) + str_sec.sh_size;
                    if (sym_end > bytes.size() || str_end > bytes.size()) continue;

                    const char* strtab = reinterpret_cast<const char*>(bytes.data() + str_sec.sh_offset);
                    const std::size_t count = sym_sec.sh_size / sizeof(Elf32_Sym);
                    for (std::size_t s = 0; s < count; ++s) {
                        Elf32_Sym sym{};
                        std::memcpy(&sym, bytes.data() + sym_sec.sh_offset + s * sizeof(Elf32_Sym), sizeof(Elf32_Sym));
                        const u8 st_type = sym.st_info & 0x0F;
                        // STT_FUNC == 2
                        if (st_type == 2 && sym.st_value != 0 && sym.st_name < str_sec.sh_size) {
                            // Lectura acotada segura: garantiza NUL antes de fin de buffer
                            std::string name;
                            for (std::size_t c = sym.st_name; c < str_sec.sh_size; ++c) {
                                if (strtab[c] == '\0') break;
                                name.push_back(strtab[c]);
                            }
                            if (!name.empty()) {
                                out_image.symbols[sym.st_value] = name;
                            }
                        }
                    }
                }
            }
        }
    }

    // 5. Descubrir funciones (por mapa previo si existe o heurística delimitada)
    discover_functions_cfg(out_image);
    return true;
}

bool ElfAnalyzer::load_function_map_csv(const std::string& path_or_content, Ps2ElfImage& out_image, std::string& out_error) {
    std::string content;
    std::ifstream f(path_or_content);
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        content = ss.str();
    } else {
        content = path_or_content;
    }

    if (content.empty()) {
        out_error = "El mapa de funciones esta vacio o no se pudo abrir.";
        return false;
    }

    std::stringstream lines(content);
    std::string line;
    std::vector<FunctionRange> explicit_funcs;

    while (std::getline(lines, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        std::size_t start_idx = 0;
        while (start_idx < line.size() && (line[start_idx] == ' ' || line[start_idx] == '\t')) start_idx++;
        if (start_idx >= line.size()) continue;
        if (line[start_idx] == '#' || line[start_idx] == ';') continue;

        std::string trimmed = line.substr(start_idx);
        std::stringstream ss_tokens(trimmed);
        std::string token1, token2, token3;
        if (!std::getline(ss_tokens, token1, ',')) continue;
        if (!std::getline(ss_tokens, token2, ',')) continue;
        std::getline(ss_tokens, token3, ',');

        auto parse_u32 = [](const std::string& s, u32& out_val) -> bool {
            if (s.empty()) return false;
            try {
                if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) {
                    out_val = static_cast<u32>(std::stoul(s, nullptr, 16));
                } else {
                    out_val = static_cast<u32>(std::stoul(s, nullptr, 10));
                }
                return true;
            } catch (...) {
                return false;
            }
        };

        u32 addr = 0;
        if (!parse_u32(token1, addr)) continue;

        FunctionRange fn{};
        fn.start_vaddr = addr;
        fn.is_heuristic = false;

        if (!token3.empty()) {
            u32 val2 = 0;
            if (parse_u32(token2, val2)) {
                if (val2 > addr) {
                    fn.end_vaddr = val2;
                } else {
                    fn.end_vaddr = addr + val2;
                }
            }
            std::size_t n_start = 0;
            while (n_start < token3.size() && (token3[n_start] == ' ' || token3[n_start] == '"')) n_start++;
            std::size_t n_end = token3.size();
            while (n_end > n_start && (token3[n_end - 1] == ' ' || token3[n_end - 1] == '"')) n_end--;
            fn.name = token3.substr(n_start, n_end - n_start);
        } else {
            std::size_t n_start = 0;
            while (n_start < token2.size() && (token2[n_start] == ' ' || token2[n_start] == '"')) n_start++;
            std::size_t n_end = token2.size();
            while (n_end > n_start && (token2[n_end - 1] == ' ' || token2[n_end - 1] == '"')) n_end--;
            fn.name = token2.substr(n_start, n_end - n_start);
            fn.end_vaddr = 0;
        }

        if (fn.name.empty()) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "sub_%08X", addr);
            fn.name = buf;
        }

        explicit_funcs.push_back(fn);
    }

    if (explicit_funcs.empty()) {
        out_error = "No se encontraron funciones validas en el archivo CSV.";
        return false;
    }

    std::sort(explicit_funcs.begin(), explicit_funcs.end(), [](const FunctionRange& a, const FunctionRange& b) {
        return a.start_vaddr < b.start_vaddr;
    });

    for (std::size_t i = 0; i < explicit_funcs.size(); ++i) {
        if (explicit_funcs[i].end_vaddr == 0 || explicit_funcs[i].end_vaddr <= explicit_funcs[i].start_vaddr) {
            if (i + 1 < explicit_funcs.size()) {
                explicit_funcs[i].end_vaddr = explicit_funcs[i + 1].start_vaddr;
            } else {
                u32 seg_end = explicit_funcs[i].start_vaddr + 0x1000U;
                for (const auto& seg : out_image.segments) {
                    if (explicit_funcs[i].start_vaddr >= seg.vaddr && explicit_funcs[i].start_vaddr < seg.vaddr + seg.memsz) {
                        seg_end = seg.vaddr + static_cast<u32>(seg.data.size());
                        break;
                    }
                }
                explicit_funcs[i].end_vaddr = seg_end;
            }
        }
        out_image.symbols[explicit_funcs[i].start_vaddr] = explicit_funcs[i].name;
    }

    out_image.functions = std::move(explicit_funcs);
    out_image.has_explicit_function_map = true;
    return true;
}

void ElfAnalyzer::discover_functions_cfg(Ps2ElfImage& img) {
    if (img.has_explicit_function_map) {
        return; // Mapa explícito verificado ya presente: evitar barrido heurístico
    }

    std::set<u32> func_starts;
    if (img.entry_point != 0) {
        func_starts.insert(img.entry_point);
        if (img.symbols.find(img.entry_point) == img.symbols.end()) {
            img.symbols[img.entry_point] = "_start";
        }
    }

    for (const auto& [addr, _] : img.symbols) {
        func_starts.insert(addr);
    }

    // Escanear todos los segmentos ejecutables buscando targets de JAL y BAL/BGEZAL/BLTZAL
    for (const auto& seg : img.segments) {
        const bool is_exec = (seg.flags & kPF_X) != 0 ||
                             ( img.entry_point >= seg.vaddr && img.entry_point < seg.vaddr + seg.memsz );
        if (!is_exec || seg.data.size() < 4) continue;

        const std::size_t word_count = seg.data.size() / 4;
        for (std::size_t i = 0; i < word_count; ++i) {
            u32 insn = 0;
            std::memcpy(&insn, seg.data.data() + i * 4, sizeof(u32));
            const u32 pc = seg.vaddr + static_cast<u32>(i * 4);
            const u8 op  = static_cast<u8>((insn >> 26) & 0x3F);

            if (op == 0x03) { // JAL target
                const u32 target = ((pc + 4) & 0xF0000000U) | ((insn & 0x03FFFFFFU) << 2);
                if (target >= seg.vaddr && target < seg.vaddr + seg.data.size()) {
                    func_starts.insert(target);
                }
            } else if (op == 0x01) { // REGIMM: BLTZAL (0x10), BGEZAL (0x11)
                const u8 rt = static_cast<u8>((insn >> 16) & 0x1F);
                if (rt == 0x10 || rt == 0x11) {
                    const s16 imm = static_cast<s16>(insn & 0xFFFF);
                    const u32 target = pc + 4 + (static_cast<s32>(imm) << 2);
                    if (target >= seg.vaddr && target < seg.vaddr + seg.data.size()) {
                        func_starts.insert(target);
                    }
                }
            }
        }
    }

    // Construir rangos de funciones contiguos dentro de cada segmento ejecutable
    for (const auto& seg : img.segments) {
        const bool is_exec = (seg.flags & kPF_X) != 0 ||
                             ( img.entry_point >= seg.vaddr && img.entry_point < seg.vaddr + seg.memsz );
        if (!is_exec || seg.data.empty()) continue;

        const u32 seg_start = seg.vaddr;
        const u32 seg_end   = seg.vaddr + static_cast<u32>(seg.data.size());

        std::vector<u32> seg_funcs;
        for (u32 addr : func_starts) {
            if (addr >= seg_start && addr < seg_end) {
                seg_funcs.push_back(addr);
            }
        }
        if (seg_funcs.empty()) {
            seg_funcs.push_back(seg_start);
        }

        std::sort(seg_funcs.begin(), seg_funcs.end());
        for (std::size_t i = 0; i < seg_funcs.size(); ++i) {
            const u32 f_start = seg_funcs[i];
            const u32 f_end   = (i + 1 < seg_funcs.size()) ? seg_funcs[i + 1] : seg_end;

            std::string name;
            auto it = img.symbols.find(f_start);
            if (it != img.symbols.end()) {
                name = it->second;
            } else {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "sub_%08X", f_start);
                name = buf;
            }
            img.functions.push_back(FunctionRange{f_start, f_end, name, /*is_heuristic=*/true});
        }
    }
}

} // namespace emotion
