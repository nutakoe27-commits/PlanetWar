#include "pw/platform/platform.h"

#include <SDL3/SDL.h>

#include "pw/core/log.h"

namespace pw {
namespace {
bool gInitialised = false;
bool gVideo = false;
}  // namespace

bool initPlatform(bool headless) {
    if (gInitialised) return true;

    // События нужны всегда. Видео — только когда есть куда рисовать: на
    // сервере и в CI дисплея нет, и попытка его открыть упала бы на пустом
    // месте.
    SDL_InitFlags flags = SDL_INIT_EVENTS;
    if (!headless) flags |= SDL_INIT_VIDEO;

    if (!SDL_Init(flags)) {
        PW_LOG_ERROR("platform", "SDL_Init: %s", SDL_GetError());
        return false;
    }

    gInitialised = true;
    gVideo = !headless;
    PW_LOG_INFO("platform", "платформа поднята: %s, логических ядер %d",
                headless ? "без видео" : "с видео", SDL_GetNumLogicalCPUCores());
    return true;
}

void shutdownPlatform() {
    if (!gInitialised) return;
    SDL_Quit();
    gInitialised = false;
    gVideo = false;
}

bool platformHasVideo() { return gVideo; }

uint64_t nowNanos() { return SDL_GetTicksNS(); }

double nowSeconds() { return double(SDL_GetTicksNS()) / 1e9; }

int cpuCount() { return SDL_GetNumLogicalCPUCores(); }

const char* platformError() { return SDL_GetError(); }

}  // namespace pw
