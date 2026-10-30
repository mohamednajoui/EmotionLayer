#pragma once
// Herramienta de diagnóstico de EmotionLayer (activa con EMOTION_DIAG=1).
// Genera build/diag_report.txt cada ~3 s: hot PCs por hilo, opcodes no implementados,
// syscalls, direcciones de polling (lecturas repetidas sin cambio) y estado de hilos.
#include "../common/types.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>
#include <algorithm>
#include <chrono>
#include <string>

namespace emotion {

struct DiagPoll { u64 reads = 0; u64 changes = 0; u32 last = 0; u32 pc = 0; };

class Diag {
public:
    static bool enabled() {
        static const bool e = (std::getenv("EMOTION_DIAG") != nullptr);
        return e;
    }
    static Diag& get() { static Diag d; return d; }

    void on_block(s32 thid, u32 pc) {
        ++pc_hist_[thid][pc];
        if ((++blocks_ & 0xFFFFF) == 0) maybe_report();
    }
    void on_unimpl(u32 pc, u32 op) { ++unimpl_[(static_cast<u64>(op) << 32) | pc]; }
    void on_syscall(u32 num, u32 pc) { ++sys_[num]; last_sys_.push_back((static_cast<u64>(num) << 32) | pc); if (last_sys_.size() > 50) last_sys_.erase(last_sys_.begin()); }
    void on_load(u32 addr, u32 value, u32 pc) {
        auto& p = poll_[addr];
        ++p.reads;
        if (p.reads > 1 && p.last != value) ++p.changes;
        p.last = value; p.pc = pc;
    }
    void event(const char* name) { ++events_[name]; }
    void on_gs_prim(bool textured) { ++(textured ? gs_tex_ : gs_untex_); }

    void report(void (*dump_threads)() = nullptr) {
        FILE* f = std::fopen("build/diag_report.txt", "w");
        if (!f) return;
        std::fprintf(f, "=== EMOTION DIAG === bloques=%llu\n\n", (unsigned long long)blocks_);
        for (auto& [thid, h] : pc_hist_) {
            std::vector<std::pair<u32, u64>> v(h.begin(), h.end());
            std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
            std::fprintf(f, "[Hilo %d] top PCs (bloques):\n", thid);
            for (size_t i = 0; i < v.size() && i < 12; ++i) std::fprintf(f, "  0x%08X  %llu\n", v[i].first, (unsigned long long)v[i].second);
        }
        std::fprintf(f, "\n[Opcodes NO implementados] (op, pc) -> veces\n");
        for (auto& [k, n] : unimpl_) std::fprintf(f, "  op=%u pc=0x%08X  x%llu\n", (u32)(k >> 32), (u32)k, (unsigned long long)n);
        std::fprintf(f, "\n[Syscalls] num -> veces\n");
        for (auto& [k, n] : sys_) std::fprintf(f, "  0x%02X  x%llu\n", k, (unsigned long long)n);
        std::fprintf(f, "  ultimas: ");
        for (u64 s : last_sys_) std::fprintf(f, "0x%X@%08X ", (u32)(s >> 32), (u32)s);
        std::fprintf(f, "\n\n[Polling] top direcciones leidas (reads, cambios, ultimo valor, pc)\n");
        std::vector<std::pair<u32, DiagPoll>> pv(poll_.begin(), poll_.end());
        std::sort(pv.begin(), pv.end(), [](auto& a, auto& b) { return a.second.reads > b.second.reads; });
        for (size_t i = 0; i < pv.size() && i < 15; ++i)
            std::fprintf(f, "  addr=0x%08X reads=%llu cambios=%llu valor=0x%08X pc=0x%08X\n", pv[i].first,
                (unsigned long long)pv[i].second.reads, (unsigned long long)pv[i].second.changes, pv[i].second.last, pv[i].second.pc);
        std::fprintf(f, "\n[Eventos]\n");
        for (auto& [k, n] : events_) std::fprintf(f, "  %s x%llu\n", k.c_str(), (unsigned long long)n);
        std::fprintf(f, "\n[GS] prims con textura=%llu sin textura=%llu\n", (unsigned long long)gs_tex_, (unsigned long long)gs_untex_);
        std::fclose(f);
        (void)dump_threads;
    }

private:
    void maybe_report() {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report_ > std::chrono::seconds(3)) { last_report_ = now; report(); }
    }
    std::map<s32, std::map<u32, u64>> pc_hist_;
    std::map<u64, u64> unimpl_;
    std::map<u32, u64> sys_;
    std::vector<u64> last_sys_;
    std::map<u32, DiagPoll> poll_;
    std::map<std::string, u64> events_;
    u64 blocks_ = 0, gs_tex_ = 0, gs_untex_ = 0;
    std::chrono::steady_clock::time_point last_report_ = std::chrono::steady_clock::now();
};

} // namespace emotion
