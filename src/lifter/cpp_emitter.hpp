#pragma once

#include "../common/types.hpp"
#include "../extractor/elf_analyzer.hpp"
#include "r5900_decoder.hpp"
#include <string>

namespace emotion {

struct RecompileOptions {
    std::string input_elf_path;
    std::string output_cpp_path;
    std::string output_exe_path;
    bool        compile_native_exe = true;
};

class CppEmitter {
public:
    // Genera el código fuente C++20 (con intrínsecos SSE4.1/AVX2) a partir de una imagen ELF de PS2
    static bool emit_cpp_translation(
        const Ps2ElfImage& img,
        const std::string& output_cpp_path,
        std::string& out_error
    );

    // Invoca el compilador nativo de Windows (MSVC cl.exe) para generar el ejecutable .exe nativo
    static bool build_native_windows_exe(
        const std::string& generated_cpp_path,
        const std::string& project_root_dir,
        const std::string& output_exe_path,
        std::string& out_log
    );

private:
    static std::string emit_single_insn(const DecodedInsn& insn);
    static std::string emit_branch_condition(const DecodedInsn& insn);
};

} // namespace emotion
