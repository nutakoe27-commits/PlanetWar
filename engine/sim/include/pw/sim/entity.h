// pw_sim — сущности и компоненты.
//
// ПРАВИЛА ЭТОГО МОДУЛЯ, из которых следует всё остальное в проекте:
//   1. Зависит только от pw_core. Ни платформы, ни рендера, ни сети.
//      Отсюда headless-сервер, прогон симуляции в CI без видеокарты и общий
//      код правил у клиента и сервера.
//   2. Никакой плавающей точки. Только fixed-point из pw_core.
//   3. Обход сущностей — всегда в стабильном порядке, никогда по адресам
//      или по хеш-таблице: раскладка памяти не имеет права влиять на мир.
//
// Оба первых правила проверяются машиной (tools/check_layering.sh), а не
// держатся на дисциплине.
#pragma once

#include <cstdint>
#include <type_traits>

#include "pw/core/hash.h"

namespace pw::sim {

/// Дескриптор сущности: плотный индекс плюс поколение.
///
/// Поколение решает задачу висячих ссылок. Индексы переиспользуются: удалили
/// флот, создали новый — он получит тот же индекс. Без поколения старая
/// ссылка молча указала бы на чужую сущность, и приказ ушёл бы не тому.
struct Entity {
    uint32_t index = kInvalidIndex;
    uint32_t generation = 0;

    static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

    constexpr bool valid() const { return index != kInvalidIndex; }
    constexpr bool operator==(const Entity&) const = default;
};

inline constexpr Entity kNoEntity{};

using ComponentId = uint32_t;
using ComponentMask = uint64_t;

inline constexpr ComponentId kInvalidComponent = 0xFFFFFFFFu;
/// Разрядность маски. Шестидесяти четырёх типов компонентов хватает
/// с большим запасом: у нас их порядка полутора десятков.
inline constexpr int kMaxComponents = 64;

namespace detail {

/// Идентификатор типа компонента. Присваивается ЯВНО при регистрации,
/// а не автоматическим счётчиком.
///
/// Это принципиально. Автоматический счётчик раздаёт номера в порядке
/// статической инициализации, а тот зависит от порядка компоновки объектных
/// файлов. Мир, хеш которого зависит от того, как компоновщик разложил
/// объектники, — это не детерминированный мир.
template <typename T>
struct ComponentSlot {
    static inline ComponentId id = kInvalidComponent;
};

}  // namespace detail

template <typename T>
inline ComponentId componentIdOf() {
    return detail::ComponentSlot<T>::id;
}

/// Требования к типу компонента.
///
/// has_unique_object_representations ловит сразу две вещи, и обе для нас
/// критичны: наличие байтов выравнивания (их содержимое не определено, и хеш
/// мира начал бы плясать) и наличие плавающей точки (у неё два представления
/// нуля и множество представлений NaN, поэтому байтового равенства нет).
///
/// То есть один этот статический контроль механически запрещает float внутри
/// компонентов симуляции — ровно то правило, на котором стоит детерминизм.
template <typename T>
inline constexpr bool kIsValidComponent =
    std::is_trivially_copyable_v<T> &&
    std::has_unique_object_representations_v<T>;

}  // namespace pw::sim
