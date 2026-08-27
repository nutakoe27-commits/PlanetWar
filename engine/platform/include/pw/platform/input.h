// pw_platform — состояние ввода.
//
// Свой набор кодов клавиш, а не чужие константы: иначе смена платформенной
// библиотеки потянула бы за собой правку всего игрового кода.
//
// Мышь и тач живут рядом намеренно. Игра обязана одинаково работать на ПК
// и на телефоне, поэтому интерфейс должен думать в терминах «указателей»,
// а не «мыши», с самого первого дня — переделывать это потом дороже.
#pragma once

#include <cstdint>

namespace pw {

enum class Key : uint16_t {
    Unknown = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Escape, Space, Enter, Tab, Backspace, Delete,
    Left, Right, Up, Down,
    LeftShift, LeftControl, LeftAlt, RightShift, RightControl, RightAlt,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Plus, Minus,
    Count
};

enum class MouseButton : uint8_t { Left = 0, Right, Middle, Count };

/// Один активный указатель: палец на экране или курсор мыши.
struct Pointer {
    int64_t id = -1;      // -1 — мышь; иначе идентификатор касания
    float x = 0.0f;       // в пикселях, начало в левом верхнем углу
    float y = 0.0f;
    bool down = false;
};

class Input {
public:
    static constexpr int kMaxPointers = 8;

    // --- клавиатура ---
    bool isDown(Key key) const { return keys_[index(key)]; }
    /// Нажата именно в этом кадре. Нужно для действий, которые не должны
    /// повторяться при удержании.
    bool wasPressed(Key key) const { return keys_[index(key)] && !prevKeys_[index(key)]; }
    bool wasReleased(Key key) const { return !keys_[index(key)] && prevKeys_[index(key)]; }

    // --- мышь ---
    bool isDown(MouseButton b) const { return buttons_[uint8_t(b)]; }
    bool wasPressed(MouseButton b) const {
        return buttons_[uint8_t(b)] && !prevButtons_[uint8_t(b)];
    }
    /// Отпущена именно в этом кадре.
    ///
    /// Интерфейсу нужно именно отпускание: кнопка обязана срабатывать
    /// по нему, а не по нажатию, иначе промахнуться после нажатия нельзя
    /// и любое случайное нажатие становится необратимым.
    bool wasReleased(MouseButton b) const {
        return !buttons_[uint8_t(b)] && prevButtons_[uint8_t(b)];
    }
    /// Положение курсора В ПИКСЕЛЯХ КАДРОВОГО БУФЕРА.
    ///
    /// Не в точках окна. На Retina это вдвое разные числа, и интерфейс,
    /// разложенный в пикселях, промахнётся мимо курсора ровно во столько
    /// же раз. Пересчёт делает Window::pumpEvents — здесь координаты уже
    /// в тех единицах, в которых работает всё остальное.
    float mouseX() const { return mouseX_; }
    float mouseY() const { return mouseY_; }
    float mouseDeltaX() const { return mouseX_ - prevMouseX_; }
    float mouseDeltaY() const { return mouseY_ - prevMouseY_; }
    float wheel() const { return wheel_; }

    // --- указатели: мышь и касания единым списком ---
    int pointerCount() const { return pointerCount_; }
    const Pointer& pointer(int i) const { return pointers_[i]; }

    /// Вызывается окном перед разбором событий кадра.
    void beginFrame();

    // Заполняется реализацией платформы.
    void setKey(Key key, bool down);
    void setButton(MouseButton b, bool down);
    void setMouse(float x, float y);
    void addWheel(float delta);
    void setPointer(int64_t id, float x, float y, bool down);
    void clearPointers();

private:
    static int index(Key key) { return int(key); }

    bool keys_[int(Key::Count)] = {};
    bool prevKeys_[int(Key::Count)] = {};
    bool buttons_[int(MouseButton::Count)] = {};
    bool prevButtons_[int(MouseButton::Count)] = {};

    float mouseX_ = 0.0f, mouseY_ = 0.0f;
    float prevMouseX_ = 0.0f, prevMouseY_ = 0.0f;
    float wheel_ = 0.0f;

    Pointer pointers_[kMaxPointers] = {};
    int pointerCount_ = 0;
};

}  // namespace pw
