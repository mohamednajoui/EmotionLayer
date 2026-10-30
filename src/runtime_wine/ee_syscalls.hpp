#pragma once

#include "../common/types.hpp"

namespace emotion {

// Tabla de Syscalls del Kernel del Emotion Engine (PS2 BIOS / OS)
// En el ABI del Emotion Engine, el número de syscall se pasa en el registro $v1 (gpr[3]).
// Los argumentos se pasan en $a0 (gpr[4]), $a1 (gpr[5]), $a2 (gpr[6]), $a3 (gpr[7]).
class EESyscalls {
public:
    static void dispatch(EEContext& ctx) noexcept;

    // Reinicia el estado global estático de llamadas al sistema (SIF RPC, heap, registros)
    static void reset() noexcept;
};

} // namespace emotion
