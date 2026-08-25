// pw_platform — окно и поверхность для отрисовки.
//
// Головной режим: настоящее окно. Безголовый: окна нет вообще.
// Безголовый режим — не заглушка ради галочки, а рабочий путь: он нужен
// серверу, тестам рендера и CI, где нет ни дисплея, ни видеокарты.
// Именно он позволяет проверять графику автоматически, сравнивая результат
// с эталонным кадром, а не «посмотрев глазами один раз».
#pragma once

#include <cstdint>

#include "pw/platform/input.h"

namespace pw {

struct WindowDesc {
    const char* title = "PlanetWar";
    int width = 1280;
    int height = 720;
    bool resizable = true;
    /// Без окна: сервер, автотесты рендера, CI.
    bool headless = false;
};

class Window {
public:
    explicit Window(const WindowDesc& desc);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool valid() const { return handle_ != nullptr || headless_; }
    bool headless() const { return headless_; }

    /// Размер в пикселях кадрового буфера. На Retina и на телефонах он
    /// отличается от логического размера окна, и путать их нельзя:
    /// область отрисовки задаётся именно в пикселях.
    void framebufferSize(int& width, int& height) const;

    /// Разобрать события кадра. false — пользователь закрывает окно.
    bool pumpEvents(Input& input);

    void setTitle(const char* title);

    // --- стыковка с pw_rhi ---
    //
    // Vulkan специально НЕ подключается в этот заголовок. Модуль отдаёт
    // список нужных расширений и умеет создать поверхность по дескриптору
    // экземпляра — этого хватает, чтобы pw_rhi ничего не знал про SDL,
    // а pw_platform ничего не знал про устройство Vulkan.

    /// Расширения экземпляра, нужные платформе. Возвращает их количество.
    static int vulkanInstanceExtensions(const char** out, int capacity);

    /// Создать поверхность. instance и outSurface — VkInstance и VkSurfaceKHR,
    /// передаются как непрозрачные указатели.
    bool createVulkanSurface(void* instance, void** outSurface) const;

private:
    void* handle_ = nullptr;  // SDL_Window*, наружу не показывается
    bool headless_ = false;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace pw
