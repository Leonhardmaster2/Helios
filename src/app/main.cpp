// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 the Helios authors and contributors
//
// Helios M0 entry point: headless smoke suite or windowed explorer.
#include "helios/app/cli.hpp"
#include "helios/app/headless.hpp"
#include "helios/core/log.hpp"
#include "helios/version.hpp"

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
// windows.h must precede dbghelp.h (MSVC C3646 otherwise); clang-format
// wants them alphabetically, so this block is exempt.
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
// clang-format on

// Crash diagnostic: exception code/address plus a symbolized stack trace
// (dbghelp reads the PDB next to the executable).
LONG WINAPI heliosCrashHandler(EXCEPTION_POINTERS* ep) {
    std::fprintf(stderr, "\nHELIOS CRASH: code=0x%08lx address=%p\n",
                 ep->ExceptionRecord != nullptr ? ep->ExceptionRecord->ExceptionCode : 0,
                 ep->ExceptionRecord != nullptr ? ep->ExceptionRecord->ExceptionAddress : nullptr);
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    if (SymInitialize(GetCurrentProcess(), nullptr, TRUE)) {
        void* stack[32] = {};
        const USHORT frames = RtlCaptureStackBackTrace(0, 32, stack, nullptr);
        for (USHORT i = 0; i < frames; ++i) {
            char symbol_buf[sizeof(SYMBOL_INFO) + 256] = {};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buf);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 256;
            DWORD64 displacement = 0;
            if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(stack[i]), &displacement,
                            symbol)) {
                std::fprintf(stderr, "  #%u %p %s+0x%llx\n", i, stack[i], symbol->Name,
                             static_cast<unsigned long long>(displacement));
            } else {
                std::fprintf(stderr, "  #%u %p\n", i, stack[i]);
            }
        }
        SymCleanup(GetCurrentProcess());
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(heliosCrashHandler);
#endif
    // --sdl-probe: diagnose SDL initialization in restricted environments.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--sdl-probe") {
#ifdef _WIN32
            std::fprintf(stderr, "sdl-probe: SDL_GetVersion...\n");
#endif
            const int r1 = SDL_Init(SDL_INIT_EVENTS);
            std::fprintf(stderr, "sdl-probe: SDL_Init(EVENTS) = %d, err=[%s]\n", r1, SDL_GetError());
            SDL_Quit();
            SDL_ClearError();
            const int r2 = SDL_Init(SDL_INIT_VIDEO);
            std::fprintf(stderr, "sdl-probe: SDL_Init(VIDEO) = %d, err=[%s]\n", r2, SDL_GetError());
            if (r2 == 0) {
                SDL_Window* w = SDL_CreateWindow("helios-probe", 64, 64, 0);
                std::fprintf(stderr, "sdl-probe: CreateWindow = %p, err=[%s]\n", (void*)w, SDL_GetError());
                if (w)
                    SDL_DestroyWindow(w);
            }
            SDL_Quit();
            return 0;
        }
    }
    const helios::app::CliOptions options = helios::app::parseCli(argc, argv);
    if (options.show_help) {
        helios::app::printUsage(argv[0]);
        return 0;
    }

    helios::core::Log::instance().setLevel(helios::core::LogLevel::Info);
    helios::core::logInfo("Helios {} starting", helios::heliosVersionString());

    if (options.headless_smoke) {
        return helios::app::runHeadlessSmoke(options);
    }
    return helios::app::runWindowed(options);
}
