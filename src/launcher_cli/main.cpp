#include "../common/types.hpp"
#include "../common/game_profile.hpp"
#include "../extractor/elf_analyzer.hpp"
#include "../extractor/iso_reader.hpp"
#include "../lifter/r5900_decoder.hpp"
#include "../lifter/cpp_emitter.hpp"
#include "../lifter/vu_compiler.hpp"
#include "../gsvk/gsvk_renderer.hpp"
#include "../runtime_wine/memory_arena.hpp"
#include "../../tests/ps2_elf_builder.hpp"
#include "../../tests/ps2_iso_builder.hpp"
#include "../../tests/ps2_phase3_3d_iso_builder.hpp"
#include "../../tests/ps2_phase4_iso_builder.hpp"
#include "../../tests/ps2_phase5_enhancements_builder.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace emotion;

namespace {

void print_usage() {
    std::printf(
        "EmotionLayer CLI (prototipo experimental de análisis y traducción parcial de PS2)\n"
        "Uso:\n"
        "  emotionlayer selftest                                       Ejecuta escenarios sinteticos incluidos (no valida compatibilidad general)\n"
        "  emotionlayer package <juego.iso> [salida.exe]               Compila el ELF de arranque de una ISO; no empaqueta todos sus datos\n"
        "  emotionlayer run-iso <juego.iso>                            Extrae y compila el ELF de arranque, monta la ISO e intenta ejecutarlo\n"
        "  emotionlayer run-elf <entrada.elf> [--map <funciones.csv>]  Compila el ELF nativamente y lo ejecuta en Windows\n"
        "  emotionlayer inspect <archivo.elf> [--map <funciones.csv>]  Analiza cabeceras, segmentos PT_LOAD, CFG y desensambla instrucciones R5900/MMI/COP1\n"
        "  emotionlayer build <entrada.elf> <salida.exe> [--map <csv>] Traduce un ELF a C++ y compila el ejecutable nativo para Windows\n"
    );
}

int cmd_inspect_elf(const std::string& elf_path, const std::string& map_csv = "") {
    Ps2ElfImage img;
    std::string err;
    if (!ElfAnalyzer::parse_file(elf_path, img, err)) {
        std::fprintf(stderr, "[Error] %s\n", err.c_str());
        return 1;
    }

    if (!map_csv.empty()) {
        if (!ElfAnalyzer::load_function_map_csv(map_csv, img, err)) {
            std::fprintf(stderr, "[Advertencia Mapa CSV] %s\n", err.c_str());
        } else {
            std::printf("  [OK] Mapa de funciones CSV cargado: %s (%zu funciones)\n",
                map_csv.c_str(), img.functions.size());
        }
    }

    std::printf("=== Analisis de Binario ELF de PlayStation 2 ===\n");
    std::printf("  Archivo:      %s\n", img.filepath.c_str());
    std::printf("  Entry Point:  0x%08X\n", img.entry_point);
    std::printf("  Flags MIPS:   0x%08X (R5900 Emotion Engine)\n", img.elf_flags);
    std::printf("  Segmentos PT_LOAD (%zu):\n", img.segments.size());
    for (std::size_t i = 0; i < img.segments.size(); ++i) {
        const auto& s = img.segments[i];
        std::printf("    [%zu] VAddr: 0x%08X | FileSz: 0x%06zX | MemSz: 0x%06X | Flags: %c%c%c\n",
            i, s.vaddr, s.data.size(), s.memsz,
            (s.flags & 4) ? 'R' : '-',
            (s.flags & 2) ? 'W' : '-',
            (s.flags & 1) ? 'X' : '-');
    }

    std::printf("  Funciones descubiertas por CFG (%zu)%s:\n",
        img.functions.size(),
        img.has_explicit_function_map ? " (Mapa Explicito CSV)" : " (Barrido Heuristico)");
    for (const auto& fn : img.functions) {
        std::printf("    - %s (0x%08X -> 0x%08X)%s\n",
            fn.name.c_str(), fn.start_vaddr, fn.end_vaddr,
            fn.is_heuristic ? " [HEURISTICA]" : " [EXPLICITA]");
        for (u32 pc = fn.start_vaddr; pc < fn.end_vaddr; pc += 4) {
            u32 raw = 0;
            if (img.read_insn(pc, raw)) {
                const auto d = R5900Decoder::decode(pc, raw);
                std::printf("        0x%08X: [%08X] %s\n", pc, raw, d.disassemble().c_str());
            }
        }
    }
    return 0;
}

int cmd_inspect_iso(const std::string& iso_path) {
    IsoReader iso;
    std::string err;
    if (!iso.open(iso_path, err)) {
        std::fprintf(stderr, "[Error ISO] %s\n", err.c_str());
        return 1;
    }

    std::string boot_elf_path;
    std::string version;
    if (!iso.parse_system_cnf(boot_elf_path, version, err)) {
        std::fprintf(stderr, "[Error SYSTEM.CNF] %s\n", err.c_str());
        return 1;
    }

    const double mb_size = static_cast<double>(iso.file_size()) / (1024.0 * 1024.0);
    const double gb_size = mb_size / 1024.0;
    std::printf("======================================================================\n");
    std::printf("  EMOTIONLAYER - INSPECCION PRELIMINAR DE IMAGEN PS2\n");
    std::printf("======================================================================\n");
    std::printf("  Imagen ISO:         %s\n", iso_path.c_str());
    if (gb_size >= 1.0) {
        std::printf("  Tamano:             %.2f GB (DVD-ROM de PlayStation 2)\n", gb_size);
    } else {
        std::printf("  Tamano:             %.2f MB (CD-ROM de PlayStation 2)\n", mb_size);
    }
    std::printf("  Ejecutable BOOT2:   %s (Version %s)\n", boot_elf_path.c_str(), version.c_str());
    std::printf("  Total de archivos:  %zu archivos/carpetas en ISO9660\n\n", iso.entries().size());

    // Detectar módulos IRX del IOP
    std::vector<std::string> irx_modules;
    for (const auto& entry : iso.entries()) {
        if (!entry.is_dir) {
            std::string upper = entry.path;
            for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (upper.find(".IRX") != std::string::npos) {
                irx_modules.push_back(entry.path);
            }
        }
    }

    std::printf("--- Modulos IOP (.IRX) Detectados (%zu) ---\n", irx_modules.size());
    for (const auto& mod : irx_modules) {
        std::string upper = mod;
        for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        bool is_hle = (upper.find("CDVD") != std::string::npos ||
                       upper.find("PAD")  != std::string::npos ||
                       upper.find("MC")   != std::string::npos ||
                       upper.find("SIO")  != std::string::npos ||
                       upper.find("LIBSD") != std::string::npos ||
                       upper.find("SDRV")  != std::string::npos);
        std::printf("  [IRX] %-30s -> %s\n", mod.c_str(), 
            is_hle ? "Coincide con un nombre que el prototipo intenta interceptar" : "Sin ruta HLE identificada (lifter experimental)");
    }
    if (irx_modules.empty()) {
        std::printf("  (Ningun modulo .IRX externo encontrado, drivers embebidos en el ELF)\n");
    }
    std::printf("\n");

    // Extraer y analizar el ejecutable BOOT2
    std::vector<u8> elf_bytes;
    if (!iso.extract_file(boot_elf_path, elf_bytes)) {
        std::fprintf(stderr, "[Error] No se pudo extraer el ELF de arranque (%s).\n", boot_elf_path.c_str());
        return 1;
    }

    Ps2ElfImage img;
    if (!ElfAnalyzer::parse_bytes(elf_bytes, boot_elf_path, img, err)) {
        std::fprintf(stderr, "[Error ELF] %s\n", err.c_str());
        return 1;
    }

    std::printf("--- Analisis del Binario ELF (%s) ---\n", boot_elf_path.c_str());
    std::printf("  Entry Point:        0x%08X\n", img.entry_point);
    std::printf("  Segmentos PT_LOAD:  %zu segmentos\n", img.segments.size());
    std::printf("  Funciones (CFG):    %zu funciones identificadas\n\n", img.functions.size());

    // Analizar instrucciones de todo el binario
    u64 total_insns   = 0;
    u64 decoded_ok    = 0;
    u64 unknown_insns = 0;
    u64 alu_insns     = 0;
    u64 branch_insns  = 0;
    u64 mem_insns     = 0;
    u64 mmi_insns     = 0;
    u64 fpu_insns     = 0;
    u64 syscall_insns = 0;
    std::map<u8, u32> unknown_opcodes; // opcode -> count

    for (const auto& seg : img.segments) {
        if ((seg.flags & 1) == 0) continue; // Solo segmentos ejecutables
        for (std::size_t off = 0; off + 4 <= seg.data.size(); off += 4) {
            u32 raw = 0;
            std::memcpy(&raw, seg.data.data() + off, 4);
            const u32 pc = seg.vaddr + static_cast<u32>(off);
            total_insns++;

            const auto d = R5900Decoder::decode(pc, raw);
            if (d.op != R5900Op::UNKNOWN) {
                decoded_ok++;
                if (d.is_branch) branch_insns++;
                if (d.op == R5900Op::SYSCALL) syscall_insns++;
                else if (d.op >= R5900Op::PADDB && d.op <= R5900Op::PEXTUW) mmi_insns++;
                else if (d.op >= R5900Op::LWC1 && d.op <= R5900Op::BC1TL) fpu_insns++;
                else if (d.op >= R5900Op::LB && d.op <= R5900Op::SQ) mem_insns++;
                else alu_insns++;
            } else {
                unknown_insns++;
                const u8 op = static_cast<u8>((raw >> 26) & 0x3F);
                unknown_opcodes[op]++;
            }
        }
    }

    const double coverage = total_insns > 0 ? (100.0 * decoded_ok / total_insns) : 0.0;
    std::printf("--- Reconocimiento del decodificador (no es medida de compatibilidad) ---\n");
    std::printf("  Total Instrucciones: %llu\n", total_insns);
    std::printf("  Palabras reconocidas: %llu (%.2f%% del barrido lineal)\n", decoded_ok, coverage);
    std::printf("  Desconocidas:        %llu\n", unknown_insns);
    std::printf("    - ALU / Logica:    %llu\n", alu_insns);
    std::printf("    - Saltos y Ramas:  %llu\n", branch_insns);
    std::printf("    - Memoria Fastmem: %llu\n", mem_insns);
    std::printf("    - MMI SIMD 128bit: %llu\n", mmi_insns);
    std::printf("    - FPU COP1:        %llu\n", fpu_insns);
    std::printf("    - Syscalls Kernel: %llu\n", syscall_insns);
    std::printf("\n");

    if (!unknown_opcodes.empty()) {
        std::printf("--- Opcodes no reconocidos ---\n");
        for (const auto& [op, count] : unknown_opcodes) {
            std::printf("  Opcode 0x%02X: %u ocurrencias\n", static_cast<unsigned>(op), count);
        }
        std::printf("\n");
    }

    std::printf("--- Limites del analisis ---\n");
    std::printf("  Este porcentaje solo cuenta palabras que el decodificador reconoce en segmentos marcados ejecutables.\n");
    std::printf("  No confirma que sean instrucciones, que estén implementadas ni que el juego sea ejecutable.\n");
    std::printf("  Para un intento experimental: emotionlayer run-iso \"%s\"\n", iso_path.c_str());
    std::printf("======================================================================\n");
    return 0;
}

int cmd_inspect(const std::string& target_path, const std::string& map_csv = "") {
    std::string lower = target_path;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.ends_with(".iso") || lower.ends_with(".bin")) {
        return cmd_inspect_iso(target_path);
    }
    return cmd_inspect_elf(target_path, map_csv);
}

int cmd_build(const std::string& elf_path, const std::string& out_exe_path, const std::string& map_csv = "") {
    Ps2ElfImage img;
    std::string err;
    if (!ElfAnalyzer::parse_file(elf_path, img, err)) {
        std::fprintf(stderr, "[Error ELF] %s\n", err.c_str());
        return 1;
    }

    if (!map_csv.empty()) {
        if (!ElfAnalyzer::load_function_map_csv(map_csv, img, err)) {
            std::fprintf(stderr, "[Advertencia Mapa CSV] %s\n", err.c_str());
        } else {
            std::printf("  [OK] Mapa de funciones CSV cargado: %s (%zu funciones)\n",
                map_csv.c_str(), img.functions.size());
        }
    }

    const std::string cpp_path = out_exe_path + ".recompiled.cpp";
    std::printf("[1/2] Levantando codigo MIPS R5900/MMI/COP1 -> C++20/AVX2 (%s)...\n", cpp_path.c_str());
    if (!CppEmitter::emit_cpp_translation(img, cpp_path, err)) {
        std::fprintf(stderr, "[Error Emitter] %s\n", err.c_str());
        return 1;
    }

    std::printf("[2/2] Compilando ejecutable nativo de Windows x86_64 (%s)...\n", out_exe_path.c_str());
    std::string build_log;
    if (!CppEmitter::build_native_windows_exe(cpp_path, ".", out_exe_path, build_log)) {
        std::fprintf(stderr, "[Error Build] %s\n", build_log.c_str());
        return 1;
    }
    std::printf("[OK] %s\n", build_log.c_str());
    return 0;
}

int cmd_run_elf(const std::string& elf_path, const std::string& map_csv = "") {
    Ps2ElfImage img;
    std::string err;
    if (!ElfAnalyzer::parse_file(elf_path, img, err)) {
        std::fprintf(stderr, "[Error ELF] %s\n", err.c_str());
        return 1;
    }

    if (!map_csv.empty()) {
        if (!ElfAnalyzer::load_function_map_csv(map_csv, img, err)) {
            std::fprintf(stderr, "[Advertencia Mapa CSV] %s\n", err.c_str());
        } else {
            std::printf("  [OK] Mapa de funciones CSV cargado: %s (%zu funciones)\n",
                map_csv.c_str(), img.functions.size());
        }
    }

    std::string clean_name = elf_path;
    for (char& c : clean_name) {
        if (c == '\\' || c == '/' || c == '.' || c == ':') c = '_';
    }
    const std::string out_exe = "build\\" + clean_name + "_native.exe";
    const std::string out_cpp = out_exe + ".recompiled.cpp";

    std::printf("[1/3] Levantando codigo MIPS R5900 -> C++20/AVX2 (%s)...\n", out_cpp.c_str());
    if (!CppEmitter::emit_cpp_translation(img, out_cpp, err)) {
        std::fprintf(stderr, "[Error Emitter] %s\n", err.c_str());
        return 1;
    }

    std::printf("[2/3] Compilando ejecutable nativo de Windows x86_64 (%s)...\n", out_exe.c_str());
    std::string build_log;
    if (!CppEmitter::build_native_windows_exe(out_cpp, ".", out_exe, build_log)) {
        std::fprintf(stderr, "[Error Build] %s\n", build_log.c_str());
        return 1;
    }
    std::printf("  [OK] %s\n\n", build_log.c_str());

    std::printf("[3/3] Ejecutando binario nativo recompilado...\n");
    return std::system(out_exe.c_str());
}

int cmd_run_iso(const std::string& iso_path) {
    IsoReader iso;
    std::string err;
    if (!iso.open(iso_path, err)) {
        std::fprintf(stderr, "[Error ISO] %s\n", err.c_str());
        return 1;
    }

    std::string boot_elf_path;
    std::string version;
    if (!iso.parse_system_cnf(boot_elf_path, version, err)) {
        std::fprintf(stderr, "[Error SYSTEM.CNF] %s\n", err.c_str());
        return 1;
    }

    std::printf("=== Imagen ISO9660 de PlayStation 2 Montada ===\n");
    std::printf("  Archivo ISO: %s\n", iso_path.c_str());
    std::printf("  BOOT2 ELF:   %s (Version %s)\n", boot_elf_path.c_str(), version.c_str());
    std::printf("  Archivos en el volumen ISO (%zu):\n", iso.entries().size());
    for (const auto& e : iso.entries()) {
        std::printf("    - %-20s | LBA: %4u | Size: %u bytes\n", e.path.c_str(), e.lba, e.size);
    }

    std::vector<u8> elf_bytes;
    if (!iso.extract_file(boot_elf_path, elf_bytes)) {
        std::fprintf(stderr, "[Error] No se pudo extraer el ejecutable BOOT2 (%s) de la ISO.\n", boot_elf_path.c_str());
        return 1;
    }
    GameProfileRegistry::detect_and_set_profile(elf_bytes.data(), elf_bytes.size());

    Ps2ElfImage img;
    if (!ElfAnalyzer::parse_bytes(elf_bytes, boot_elf_path, img, err)) {
        std::fprintf(stderr, "[Error ELF] %s\n", err.c_str());
        return 1;
    }

    std::string clean_name = IsoReader::normalize_ps2_path(boot_elf_path);
    for (char& c : clean_name) {
        if (c == '\\' || c == '/' || c == '.' || c == ':') c = '_';
    }
    const std::string out_exe = "build\\" + clean_name + "_native.exe";
    const std::string out_cpp = out_exe + ".recompiled.cpp";
    if (!CppEmitter::emit_cpp_translation(img, out_cpp, err)) {
        std::fprintf(stderr, "[Error Emitter] %s\n", err.c_str());
        return 1;
    }

    std::string build_log;
    if (!CppEmitter::build_native_windows_exe(out_cpp, ".", out_exe, build_log)) {
        std::fprintf(stderr, "[Error Build] %s\n", build_log.c_str());
        return 1;
    }
    std::printf("  [OK] %s\n\n", build_log.c_str());

    const std::string run_cmd = out_exe + " --iso \"" + iso_path + "\"";
    return std::system(run_cmd.c_str());
}

int cmd_package(const std::string& iso_path, const std::string& out_exe_path) {
    IsoReader iso;
    std::string err;
    if (!iso.open(iso_path, err)) {
        std::fprintf(stderr, "[Error ISO] %s\n", err.c_str());
        return 1;
    }

    std::string boot_elf_path;
    std::string version;
    if (!iso.parse_system_cnf(boot_elf_path, version, err)) {
        std::fprintf(stderr, "[Error SYSTEM.CNF] %s\n", err.c_str());
        return 1;
    }

    std::printf("=== Compilando ELF de arranque extraido de la ISO ===\n");
    std::printf("  ISO Fuente:    %s\n", iso_path.c_str());
    std::printf("  BOOT2 ELF:     %s (Version %s)\n", boot_elf_path.c_str(), version.c_str());
    std::printf("  Destino EXE:   %s\n", out_exe_path.c_str());

    std::vector<u8> elf_bytes;
    if (!iso.extract_file(boot_elf_path, elf_bytes)) {
        std::fprintf(stderr, "[Error] No se pudo extraer el ELF de la ISO.\n");
        return 1;
    }
    GameProfileRegistry::detect_and_set_profile(elf_bytes.data(), elf_bytes.size());

    Ps2ElfImage img;
    if (!ElfAnalyzer::parse_bytes(elf_bytes, boot_elf_path, img, err)) {
        std::fprintf(stderr, "[Error ELF] %s\n", err.c_str());
        return 1;
    }

    const std::string out_cpp = out_exe_path + ".recompiled.cpp";
    if (!CppEmitter::emit_cpp_translation(img, out_cpp, err)) {
        std::fprintf(stderr, "[Error Emitter] %s\n", err.c_str());
        return 1;
    }

    std::string build_log;
    if (!CppEmitter::build_native_windows_exe(out_cpp, ".", out_exe_path, build_log)) {
        std::fprintf(stderr, "[Error Build] %s\n", build_log.c_str());
        return 1;
    }

    std::printf("  [OK] %s\n", build_log.c_str());
    std::printf("  [OK] Se compilo el ELF de arranque. La ISO y sus archivos no se han incluido en el EXE.\n\n");
    return 0;
}

int cmd_selftest() {
    std::printf("=== [EmotionLayer] Verificacion Integral de Fases 1, 2, 3, 4 y 5 en Windows ===\n");

    MemoryArena arena;
    if (!arena.initialize()) {
        std::fprintf(stderr, "[FAIL] No se pudo inicializar MemoryArena.\n");
        return 1;
    }
    GsvkRenderer gs_probe;
    gs_probe.initialize();
    std::printf("  [OK] Arena de 4 GB reservada en: %p (Hardware Mirroring VirtualAlloc2: %s)\n",
        static_cast<void*>(arena.base()),
        arena.is_hardware_mirrored() ? "ACTIVO" : "FALLBACK");
        std::printf("  [OK] Renderer inicializado | Dispositivo reportado: %s\n\n",
        gs_probe.gpu_device_name().c_str());

    // --- FASE 1: Binario .ELF de pruebas de CPU R5900 / MMI / COP1 ---
    const std::string test_elf = "build\\ps2_hw_autotest.elf";
    const std::string test_exe = "build\\ps2_hw_autotest_native.exe";
    if (!test::Ps2ElfBuilder::generate_autotest_elf(test_elf)) return 1;
    if (cmd_build(test_elf, test_exe) != 0) return 1;
    if (std::system(test_exe.c_str()) != 0) return 1;

    std::printf("\n");

    // --- FASE 2: Imagen .ISO con SYSTEM.CNF, Win32 Fibers Kernel, Semáforos y SIF/IOP HLE ---
    const std::string test_iso2 = "build\\ps2_phase2_game.iso";
    if (!test::Ps2IsoBuilder::generate_phase2_iso(test_iso2)) return 1;
    if (cmd_run_iso(test_iso2) != 0) return 1;

    std::printf("\n");

    // --- FASE 3: Imagen .ISO 3D con DMAC Source Chain, VIF1, Microcódigo VU1 + XGKICK y GSVK 4MB eDRAM ---
    const std::string test_iso3 = "build\\ps2_phase3_3d_demo.iso";
    if (!test::Ps2Phase3IsoBuilder::generate_phase3_iso(test_iso3)) return 1;
    if (cmd_run_iso(test_iso3) != 0) return 1;

    std::printf("\n");

    // --- Escenarios sintéticos adicionales: audio, IOP y código dinámico ---
    const std::string test_iso4 = "build\\ps2_phase4_demo.iso";
    if (!test::Ps2Phase4IsoBuilder::generate_phase4_iso(test_iso4)) return 1;
    if (cmd_run_iso(test_iso4) != 0) return 1;

    std::printf("\n");

    // --- Escenarios sintéticos adicionales: opciones del renderer y compilación ---
    const std::string test_iso5 = "build\\ps2_phase5_demo.iso";
    if (!test::Ps2Phase5EnhancementsBuilder::generate_phase5_iso(test_iso5)) return 1;
    if (cmd_run_iso(test_iso5) != 0) return 1;

    // Compilar el ELF de arranque de la ISO sintética; no se prueba un paquete autónomo.
    const std::string package_probe_exe = "build\\EmotionLayer_Package_Probe.exe";
    if (cmd_package(test_iso5, package_probe_exe) != 0) return 1;

    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 0;
    }
    const std::string cmd = argv[1];
    if (cmd == "selftest") {
        return cmd_selftest();
    } else if (cmd == "package" && argc >= 3) {
        const std::string out_exe = (argc >= 4) ? argv[3] : "build\\game_recompiled.exe";
        return cmd_package(argv[2], out_exe);
    } else if (cmd == "run-iso" && argc >= 3) {
        return cmd_run_iso(argv[2]);
    } else if (cmd == "run-elf" && argc >= 3) {
        std::string map_csv;
        for (int i = 3; i < argc; ++i) {
            if (std::string(argv[i]) == "--map" && i + 1 < argc) {
                map_csv = argv[++i];
            }
        }
        return cmd_run_elf(argv[2], map_csv);
    } else if (cmd == "inspect" && argc >= 3) {
        std::string map_csv;
        for (int i = 3; i < argc; ++i) {
            if (std::string(argv[i]) == "--map" && i + 1 < argc) {
                map_csv = argv[++i];
            }
        }
        return cmd_inspect(argv[2], map_csv);
    } else if (cmd == "build" && argc >= 4) {
        std::string map_csv;
        for (int i = 4; i < argc; ++i) {
            if (std::string(argv[i]) == "--map" && i + 1 < argc) {
                map_csv = argv[++i];
            }
        }
        return cmd_build(argv[2], argv[3], map_csv);
    }
    print_usage();
    return 1;
}
