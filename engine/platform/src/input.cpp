#include "pw/platform/input.h"

#include <cstring>

namespace pw {

void Input::beginFrame() {
    // Прошлый кадр запоминается целиком: на нём строятся wasPressed
    // и wasReleased, без которых действие срабатывало бы каждый кадр
    // удержания клавиши.
    std::memcpy(prevKeys_, keys_, sizeof(keys_));
    std::memcpy(prevButtons_, buttons_, sizeof(buttons_));
    prevMouseX_ = mouseX_;
    prevMouseY_ = mouseY_;
    wheel_ = 0.0f;  // колесо — событие, а не состояние
}

void Input::setKey(Key key, bool down) {
    if (key != Key::Unknown && key < Key::Count) keys_[index(key)] = down;
}

void Input::setButton(MouseButton b, bool down) {
    if (b < MouseButton::Count) buttons_[uint8_t(b)] = down;
}

void Input::setMouse(float x, float y) {
    mouseX_ = x;
    mouseY_ = y;
}

void Input::addWheel(float delta) { wheel_ += delta; }

void Input::setPointer(int64_t id, float x, float y, bool down) {
    for (int i = 0; i < pointerCount_; ++i) {
        if (pointers_[i].id == id) {
            pointers_[i].x = x;
            pointers_[i].y = y;
            pointers_[i].down = down;
            if (!down) {  // отпущенный указатель убираем, порядок не важен
                pointers_[i] = pointers_[pointerCount_ - 1];
                --pointerCount_;
            }
            return;
        }
    }
    if (down && pointerCount_ < kMaxPointers) {
        pointers_[pointerCount_++] = Pointer{id, x, y, true};
    }
}

void Input::clearPointers() { pointerCount_ = 0; }

}  // namespace pw
