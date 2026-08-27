#include "pw/platform/window.h"

#include <vulkan/vulkan.h>
// Строго после vulkan.h: SDL_vulkan.h пользуется его типами, но сам их
// не подключает — так задумано, чтобы не навязывать Vulkan тем, кому он не нужен.
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "pw/core/log.h"
#include "pw/platform/platform.h"

namespace pw {
namespace {

/// Коды клавиш платформы переводятся в наши. Без этого смена платформенной
/// библиотеки потянула бы правку всего игрового кода.
Key translateKey(SDL_Scancode code) {
    if (code >= SDL_SCANCODE_A && code <= SDL_SCANCODE_Z) {
        return Key(int(Key::A) + (code - SDL_SCANCODE_A));
    }
    // Раскладка цифр в SDL идёт 1..9, затем 0 — поэтому не диапазоном.
    if (code >= SDL_SCANCODE_1 && code <= SDL_SCANCODE_9) {
        return Key(int(Key::Num1) + (code - SDL_SCANCODE_1));
    }
    if (code >= SDL_SCANCODE_F1 && code <= SDL_SCANCODE_F12) {
        return Key(int(Key::F1) + (code - SDL_SCANCODE_F1));
    }

    switch (code) {
        case SDL_SCANCODE_0:          return Key::Num0;
        case SDL_SCANCODE_ESCAPE:     return Key::Escape;
        case SDL_SCANCODE_SPACE:      return Key::Space;
        case SDL_SCANCODE_RETURN:     return Key::Enter;
        case SDL_SCANCODE_TAB:        return Key::Tab;
        case SDL_SCANCODE_BACKSPACE:  return Key::Backspace;
        case SDL_SCANCODE_DELETE:     return Key::Delete;
        case SDL_SCANCODE_LEFT:       return Key::Left;
        case SDL_SCANCODE_RIGHT:      return Key::Right;
        case SDL_SCANCODE_UP:         return Key::Up;
        case SDL_SCANCODE_DOWN:       return Key::Down;
        case SDL_SCANCODE_LSHIFT:     return Key::LeftShift;
        case SDL_SCANCODE_LCTRL:      return Key::LeftControl;
        case SDL_SCANCODE_LALT:       return Key::LeftAlt;
        case SDL_SCANCODE_RSHIFT:     return Key::RightShift;
        case SDL_SCANCODE_RCTRL:      return Key::RightControl;
        case SDL_SCANCODE_RALT:       return Key::RightAlt;
        case SDL_SCANCODE_EQUALS:     return Key::Plus;
        case SDL_SCANCODE_MINUS:      return Key::Minus;
        default:                      return Key::Unknown;
    }
}

MouseButton translateButton(Uint8 button) {
    switch (button) {
        case SDL_BUTTON_LEFT:   return MouseButton::Left;
        case SDL_BUTTON_RIGHT:  return MouseButton::Right;
        case SDL_BUTTON_MIDDLE: return MouseButton::Middle;
        default:                return MouseButton::Count;
    }
}

}  // namespace

Window::Window(const WindowDesc& desc)
    : headless_(desc.headless), width_(desc.width), height_(desc.height) {
    if (headless_) {
        PW_LOG_INFO("window", "безголовый режим, %dx%d", width_, height_);
        return;
    }

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
    if (desc.resizable) flags |= SDL_WINDOW_RESIZABLE;
    // Без этого на Retina и на телефонах картинка выйдет мыльной: система
    // растянет буфер логического размера на физический экран.
    flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;

    handle_ = SDL_CreateWindow(desc.title, desc.width, desc.height, flags);
    if (!handle_) {
        PW_LOG_ERROR("window", "SDL_CreateWindow: %s", SDL_GetError());
        return;
    }

    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(handle_), &pw, &ph);
    // Масштаб печатается намеренно: если координаты мыши однажды снова
    // разъедутся с интерфейсом, первое, что надо увидеть, — это число.
    PW_LOG_INFO("window", "окно %dx%d, кадровый буфер %dx%d, масштаб %.2f",
                desc.width, desc.height, pw, ph,
                desc.width > 0 ? double(pw) / double(desc.width) : 1.0);
}

Window::~Window() {
    if (handle_) SDL_DestroyWindow(static_cast<SDL_Window*>(handle_));
}

void Window::framebufferSize(int& width, int& height) const {
    if (!handle_) {
        width = width_;
        height = height_;
        return;
    }
    SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(handle_), &width, &height);
}

void Window::setTitle(const char* title) {
    if (handle_) SDL_SetWindowTitle(static_cast<SDL_Window*>(handle_), title);
}

bool Window::pumpEvents(Input& input) {
    input.beginFrame();
    if (headless_) return true;

    int fbWidth = 0, fbHeight = 0;
    framebufferSize(fbWidth, fbHeight);

    // ТОЧКИ ОКНА -> ПИКСЕЛИ КАДРОВОГО БУФЕРА.
    //
    // Весь игровой код работает в пикселях кадра: в них задан интерфейс,
    // в них же приходит его размер. SDL отдаёт координаты мыши в ТОЧКАХ
    // окна, а это не одно и то же: с SDL_WINDOW_HIGH_PIXEL_DENSITY
    // на Retina кадровый буфер вдвое крупнее окна.
    //
    // Без этого пересчёта курсор промахивается ровно во столько раз,
    // каков масштаб экрана: человек наводит на одну кнопку, подсвечивается
    // другая. На обычном мониторе масштаб единица, и ошибки не видно
    // вовсе — поэтому она и прожила до первого запуска на маке.
    //
    // Касания ниже переводятся в пиксели с самого начала, и это тот же
    // самый уговор: единицы у мыши и у пальца обязаны совпадать.
    int winWidth = 0, winHeight = 0;
    SDL_GetWindowSize(static_cast<SDL_Window*>(handle_), &winWidth, &winHeight);
    const float pointScaleX = winWidth > 0 ? float(fbWidth) / float(winWidth) : 1.0f;
    const float pointScaleY = winHeight > 0 ? float(fbHeight) / float(winHeight) : 1.0f;

    bool keepRunning = true;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                keepRunning = false;
                break;

            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                keepRunning = false;
                break;

            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                if (!event.key.repeat) {
                    input.setKey(translateKey(event.key.scancode), event.key.down);
                }
                break;

            case SDL_EVENT_MOUSE_MOTION: {
                const float x = event.motion.x * pointScaleX;
                const float y = event.motion.y * pointScaleY;
                input.setMouse(x, y);
                input.setPointer(-1, x, y, input.isDown(MouseButton::Left));
                break;
            }

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                const MouseButton button = translateButton(event.button.button);
                const float x = event.button.x * pointScaleX;
                const float y = event.button.y * pointScaleY;
                input.setButton(button, event.button.down);
                input.setMouse(x, y);
                if (button == MouseButton::Left) {
                    input.setPointer(-1, x, y, event.button.down);
                }
                break;
            }

            case SDL_EVENT_MOUSE_WHEEL:
                input.addWheel(event.wheel.y);
                break;

            // Касания приходят в долях экрана — переводим в пиксели, чтобы
            // игровой код работал в одних единицах и с мышью, и с пальцем.
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_MOTION:
                input.setPointer(int64_t(event.tfinger.fingerID),
                                 event.tfinger.x * float(fbWidth),
                                 event.tfinger.y * float(fbHeight), true);
                break;

            case SDL_EVENT_FINGER_UP:
                input.setPointer(int64_t(event.tfinger.fingerID),
                                 event.tfinger.x * float(fbWidth),
                                 event.tfinger.y * float(fbHeight), false);
                break;

            default:
                break;
        }
    }
    return keepRunning;
}

int Window::vulkanInstanceExtensions(const char** out, int capacity) {
    // Без видеоподсистемы поверхности не будет вовсе: безголовый рендер
    // пишет прямо в память, и расширения окна ему не нужны.
    if (!platformHasVideo()) return 0;

    Uint32 count = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!names) return 0;

    const int total = int(count);
    if (!out) return total;
    const int copied = total < capacity ? total : capacity;
    for (int i = 0; i < copied; ++i) out[i] = names[i];
    return copied;
}

bool Window::createVulkanSurface(void* instance, void** outSurface) const {
    if (!handle_ || !instance || !outSurface) return false;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(handle_),
                                  static_cast<VkInstance>(instance), nullptr, &surface)) {
        PW_LOG_ERROR("window", "SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        return false;
    }
    *outSurface = surface;
    return true;
}

}  // namespace pw
