#define _CRT_SECURE_NO_WARNINGS  // (fopen: plain C files are all we need)
#include "app/debug_log.h"

#include <cinttypes>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

#include "net/protocol.h"

// (after our own headers: windows.h's macros would get into them)
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// clang-format off
#include <dbghelp.h>
// clang-format on
#endif

namespace app::debug {
namespace {

std::FILE* g_log = nullptr;
std::FILE* g_replay = nullptr;
std::string g_dir;    // logs/ beside the game
std::string g_stamp;  // this run's date and time, in the files' names
std::string g_args;
const char* g_phase = "starting";
engine::Tick g_tick = 0;
std::clock_t g_start = 0;

std::string exe_dir() {
#ifdef _WIN32
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    const std::string s(path);
    const size_t slash = s.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : s.substr(0, slash);
#else
    return ".";
#endif
}

std::string now_stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", &tm);
    return buf;
}

void ensure_dir() {
    if (!g_dir.empty()) return;
    g_dir = exe_dir() + "/logs";
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);
}

const char* type_name(engine::CommandType t) {
    using engine::CommandType;
    switch (t) {
        case CommandType::Move: return "Move";
        case CommandType::Stop: return "Stop";
        case CommandType::Attack: return "Attack";
        case CommandType::AttackMove: return "AttackMove";
        case CommandType::AttackGround: return "AttackGround";
        case CommandType::Garrison: return "Garrison";
        case CommandType::Gather: return "Gather";
        case CommandType::Train: return "Train";
        case CommandType::Retrain: return "Retrain";
        case CommandType::Build: return "Build";
        case CommandType::Haul: return "Haul";
        case CommandType::Observe: return "Observe";
        case CommandType::Ability: return "Ability";
        case CommandType::Upgrade: return "Upgrade";
        case CommandType::Unload: return "Unload";
        case CommandType::Research: return "Research";
        case CommandType::Supply: return "Supply";
        case CommandType::Collect: return "Collect";
        case CommandType::Rally: return "Rally";
        case CommandType::LoadShell: return "LoadShell";
        case CommandType::Fortify: return "Fortify";
        case CommandType::ManWorks: return "ManWorks";
        case CommandType::TakeCover: return "TakeCover";
    }
    return "?";
}

double tiles(engine::Fixed f) { return static_cast<double>(f.raw) / 65536.0; }

std::string hex(const std::vector<uint8_t>& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (const uint8_t b : bytes) {
        s.push_back(kDigits[b >> 4]);
        s.push_back(kDigits[b & 15]);
    }
    return s;
}

std::optional<std::vector<uint8_t>> unhex(std::string_view s) {
    if (s.size() % 2 != 0) return std::nullopt;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::vector<uint8_t> out;
    for (size_t i = 0; i < s.size(); i += 2) {
        const int hi = digit(s[i]);
        const int lo = digit(s[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<uint8_t>(hi * 16 + lo));
    }
    return out;
}

void write_line(std::FILE* f, const char* text) {
    if (!f) return;
    const double seconds = static_cast<double>(std::clock() - g_start) / CLOCKS_PER_SEC;
    std::fprintf(f, "[%8.3f] %6u  %s\n", seconds, g_tick, text);
    std::fflush(f);
}

#ifdef _WIN32

// The call stack from `ctx` on: each frame's module and offset in it, and
// with the game's .pdb beside it, the function and the source line.
std::string stack_of(CONTEXT ctx) {
    std::string out;
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    const std::string search = exe_dir();
    SymInitialize(process, search.c_str(), TRUE);
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 64; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr)) {
            break;
        }
        const DWORD64 pc = frame.AddrPC.Offset;
        if (pc == 0) break;
        char module[MAX_PATH] = "?";
        DWORD64 base = 0;
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(pc), &mod)) {
            char path[MAX_PATH] = {};
            GetModuleFileNameA(mod, path, MAX_PATH);
            const char* name = std::strrchr(path, '\\');
            std::snprintf(module, sizeof module, "%s", name ? name + 1 : path);
            base = reinterpret_cast<DWORD64>(mod);
        }
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        const bool named = SymFromAddr(process, pc, &displacement, symbol);
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD column = 0;
        const bool lined = SymGetLineFromAddr64(process, pc, &column, &line);
        char text[1024];
        std::snprintf(text, sizeof text, "  #%02d %s+0x%llx  %s  %s:%lu\n", i, module,
                      static_cast<unsigned long long>(pc - base), named ? symbol->Name : "?",
                      lined ? line.FileName : "", lined ? line.LineNumber : 0ul);
        out += text;
    }
    SymCleanup(process);
    return out;
}

void write_dump(EXCEPTION_POINTERS* ep, const std::string& path) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = ep;
    info.ClientPointers = FALSE;
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
                                                 MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, ep ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);
}

// What happened, where, and how we got there: into crash_<time>.txt (and the log).
void report(const char* what, EXCEPTION_POINTERS* ep, const CONTEXT& ctx) {
    static bool reported = false;
    if (reported) return;
    reported = true;
    ensure_dir();
    const std::string base = g_dir + "/crash_" + (g_stamp.empty() ? now_stamp() : g_stamp);
    std::string text = std::string("Anchor crashed: ") + what + "\n";
    char line[512];
    if (ep) {
        const auto address = reinterpret_cast<DWORD64>(ep->ExceptionRecord->ExceptionAddress);
        std::snprintf(line, sizeof line, "exception 0x%08lX at 0x%llx", ep->ExceptionRecord->ExceptionCode,
                      static_cast<unsigned long long>(address));
        text += line;
        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
            std::snprintf(line, sizeof line, " (%s 0x%llx)", ep->ExceptionRecord->ExceptionInformation[0] == 0   ? "reading"
                                                             : ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "writing"
                                                                                                                  : "executing",
                          static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1]));
            text += line;
        }
        text += "\n";
    }
    std::snprintf(line, sizeof line, "tick %u, while: %s\nprotocol %u, args:%s\n", g_tick, g_phase,
                  static_cast<unsigned>(net::kProtocolVersion), g_args.c_str());
    text += line;
    text += "stack:\n" + stack_of(ctx);
    if (std::FILE* f = std::fopen((base + ".txt").c_str(), "w")) {
        std::fputs(text.c_str(), f);
        std::fclose(f);
    }
    if (g_log) {
        std::fputs("\n", g_log);
        std::fputs(text.c_str(), g_log);
        std::fflush(g_log);
    }
    if (g_replay) std::fflush(g_replay);
    write_dump(ep, base + ".dmp");
    std::fprintf(stderr, "%s(report: %s.txt)\n", text.c_str(), base.c_str());
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* ep) {
    report("an exception", ep, *ep->ContextRecord);
    return EXCEPTION_CONTINUE_SEARCH;
}

void report_here(const char* what) {
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    report(what, nullptr, ctx);
}

#else
void report_here(const char*) {}
#endif

}  // namespace

void start(int argc, char** argv, bool log) {
    g_start = std::clock();
    g_stamp = now_stamp();
    for (int i = 1; i < argc; ++i) g_args += std::string(" ") + argv[i];
#ifdef _WIN32
    SetUnhandledExceptionFilter(on_exception);
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
        report_here("an invalid argument to a C library function");
        std::abort();
    });
    _set_purecall_handler([] {
        report_here("a pure virtual function called");
        std::abort();
    });
#endif
    std::set_terminate([] {
        report_here("std::terminate (an exception nobody caught)");
        std::abort();
    });
    std::signal(SIGABRT, [](int) { report_here("abort()"); });
    if (!log) return;
    ensure_dir();
    const std::string base = g_dir + "/anchor_" + g_stamp;
    g_log = std::fopen((base + ".log").c_str(), "w");
    g_replay = std::fopen((base + ".replay").c_str(), "w");
    logf("Anchor, protocol %u, args:%s", static_cast<unsigned>(net::kProtocolVersion), g_args.c_str());
    logf("log: %s.log, replay: %s.replay", base.c_str(), base.c_str());
    std::fprintf(stderr, "debug log: %s.log\n", base.c_str());
}

bool logging() { return g_log != nullptr; }

void logf(const char* fmt, ...) {
    if (!g_log) return;
    char text[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    write_line(g_log, text);
}

void session(uint64_t seed, int32_t map_size, engine::PlayerId local_player, int player_count) {
    logf("session: seed %" PRIu64 ", map %d, we are player %d of %d", seed, map_size, local_player, player_count);
    if (!g_replay) return;
    std::fprintf(g_replay, "anchor-replay 1\nprotocol %u\nseed %" PRIu64 "\nmap %d\nplayer %d\nplayers %d\n",
                 static_cast<unsigned>(net::kProtocolVersion), seed, map_size, local_player, player_count);
    std::fflush(g_replay);
}

void step(const engine::World& world, const std::vector<engine::Command>& commands) {
    g_tick = world.tick();
    if (!g_log) return;
    for (const engine::Command& cmd : commands) {
        logf("P%d %s", cmd.player, describe(cmd).c_str());
        if (g_replay) {
            net::TickInput one;
            one.tick = world.tick();
            one.commands = {cmd};
            std::fprintf(g_replay, "T %u %d %s\n", world.tick(), cmd.player, hex(net::encode(one)).c_str());
        }
    }
    if (world.tick() % engine::kTicksPerSecond == 0) {
        const uint64_t sum = world.checksum();
        if (g_replay) std::fprintf(g_replay, "C %u %016" PRIx64 "\n", world.tick(), sum);
        if (world.tick() % (10 * engine::kTicksPerSecond) == 0) {
            int units[2] = {0, 0};
            for (const engine::Unit& u : world.units()) ++units[u.owner % 2];
            logf("checksum %016" PRIx64 ", units %d / %d, structures %zu", sum, units[0], units[1], world.structures().size());
        }
    }
    if (g_replay && !commands.empty()) std::fflush(g_replay);
}

void phase(const char* what) { g_phase = what; }

std::string describe(const engine::Command& cmd) {
    std::ostringstream s;
    s << type_name(cmd.type);
    if (!cmd.units.empty()) {
        s << " units=[";
        for (size_t i = 0; i < cmd.units.size(); ++i) s << (i ? "," : "") << cmd.units[i];
        s << "]";
    }
    char buf[96];
    if (cmd.target.x.raw != 0 || cmd.target.y.raw != 0) {
        std::snprintf(buf, sizeof buf, " target=(%.2f,%.2f)", tiles(cmd.target.x), tiles(cmd.target.y));
        s << buf;
    }
    if (cmd.target_end.x.raw != 0 || cmd.target_end.y.raw != 0) {
        std::snprintf(buf, sizeof buf, " end=(%.2f,%.2f)", tiles(cmd.target_end.x), tiles(cmd.target_end.y));
        s << buf;
    }
    if (cmd.target_unit) s << " target_unit=" << cmd.target_unit;
    if (cmd.unit_type) s << " unit_type=" << static_cast<int>(cmd.unit_type);
    if (cmd.structure_type) s << " structure_type=" << static_cast<int>(cmd.structure_type);
    if (cmd.ability) s << " ability=" << static_cast<int>(cmd.ability);
    if (cmd.upgrade) s << " upgrade=" << static_cast<int>(cmd.upgrade);
    if (cmd.cargo) s << " cargo=" << static_cast<int>(cmd.cargo);
    if (cmd.queued) s << " queued";
    return s.str();
}

std::optional<Replay> load_replay(const std::string& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    Replay r;
    bool header = false;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        std::string key;
        s >> key;
        if (key == "anchor-replay") {
            header = true;
        } else if (key == "protocol") {
            unsigned v = 0;
            s >> v;
            if (v != net::kProtocolVersion) {
                std::fprintf(stderr, "replay: made with protocol %u, this game is %u: it may play out differently\n", v,
                             static_cast<unsigned>(net::kProtocolVersion));
            }
        } else if (key == "seed") {
            s >> r.seed;
        } else if (key == "map") {
            s >> r.map_size;
        } else if (key == "player") {
            int p = 0;
            s >> p;
            r.local_player = static_cast<engine::PlayerId>(p);
        } else if (key == "players") {
            s >> r.player_count;
        } else if (key == "T") {
            engine::Tick tick = 0;
            int player = 0;
            std::string data;
            s >> tick >> player >> data;
            const auto bytes = unhex(data);
            if (!bytes) continue;
            const auto input = net::decode_tick_input(*bytes);
            if (!input) continue;
            for (engine::Command cmd : input->commands) {
                cmd.player = static_cast<engine::PlayerId>(player);
                r.commands[tick].push_back(std::move(cmd));
            }
        } else if (key == "C") {
            engine::Tick tick = 0;
            std::string sum;
            s >> tick >> sum;
            r.checksums[tick] = std::strtoull(sum.c_str(), nullptr, 16);
        }
    }
    if (!header || r.map_size <= 0) return std::nullopt;
    return r;
}

}  // namespace app::debug
