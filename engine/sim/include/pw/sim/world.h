// pw_sim — мир: хранилище сущностей и компонентов.
//
// Раскладка архетипная, данные по столбцам (SoA). Для нашего профиля это
// заметно выигрывает у разреженных множеств: тик проходит по всем сущностям
// с одинаковым набором компонентов, и такой проход линеен по памяти.
//
// Сущности с одинаковым набором компонентов лежат в одной таблице:
//
//   архетип {Position, Fleet, Owner}
//     столбец Position  [p0][p1][p2]...
//     столбец Fleet     [f0][f1][f2]...
//     столбец Owner     [o0][o1][o2]...
//
// Добавление компонента переносит сущность в другую таблицу. Операция не
// бесплатная, поэтому набор компонентов задают при создании, а не меняют
// каждый тик.
#pragma once

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pw/core/hash.h"
#include "pw/sim/entity.h"

namespace pw::sim {

class World {
public:
    World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // -----------------------------------------------------------------------
    // Регистрация компонентов
    // -----------------------------------------------------------------------

    /// Зарегистрировать тип компонента. Порядок вызовов задаёт идентификаторы,
    /// поэтому регистрация обязана быть в одном месте и в фиксированном
    /// порядке — от него зависит хеш состояния мира.
    template <typename T>
    ComponentId registerComponent(const char* name) {
        static_assert(kIsValidComponent<T>,
                      "компонент обязан быть тривиально копируемым, без байтов "
                      "выравнивания и без плавающей точки — иначе хеш мира "
                      "перестанет быть воспроизводимым");
        static_assert(sizeof(T) > 0, "пустые компоненты пока не поддерживаются");

        const ComponentId id = uint32_t(components_.size());
        components_.push_back(ComponentInfo{uint32_t(sizeof(T)), uint32_t(alignof(T)), name});
        detail::ComponentSlot<T>::id = id;
        return id;
    }

    // -----------------------------------------------------------------------
    // Сущности
    // -----------------------------------------------------------------------

    Entity create();
    void destroy(Entity entity);
    bool alive(Entity entity) const;
    uint32_t liveCount() const { return liveCount_; }

    // -----------------------------------------------------------------------
    // Компоненты
    // -----------------------------------------------------------------------

    template <typename T>
    T& add(Entity entity, const T& value = T{}) {
        const ComponentId id = componentIdOf<T>();
        attach(entity, id);
        T* slot = static_cast<T*>(componentPtr(entity, id));
        *slot = value;
        return *slot;
    }

    template <typename T>
    void remove(Entity entity) {
        detach(entity, componentIdOf<T>());
    }

    template <typename T>
    bool has(Entity entity) const {
        if (!alive(entity)) return false;
        const ComponentId id = componentIdOf<T>();
        return (archetypes_[records_[entity.index].archetype].mask & bit(id)) != 0;
    }

    template <typename T>
    T* get(Entity entity) {
        return static_cast<T*>(componentPtr(entity, componentIdOf<T>()));
    }

    template <typename T>
    const T* get(Entity entity) const {
        return static_cast<const T*>(
            const_cast<World*>(this)->componentPtr(entity, componentIdOf<T>()));
    }

    // -----------------------------------------------------------------------
    // Обход
    // -----------------------------------------------------------------------

    /// Обойти все сущности, у которых есть все перечисленные компоненты.
    /// Вызывает fn(Entity, T1&, T2&, ...).
    ///
    /// Порядок обхода стабилен: архетипы в порядке создания, строки по
    /// возрастанию. Он не зависит ни от адресов, ни от хеш-таблиц.
    ///
    /// ВНУТРИ ОБХОДА МИР МЕНЯТЬ НЕЛЬЗЯ — создание и удаление сущностей
    /// перекладывает строки прямо под ногами. Изменения копятся в буфер
    /// и применяются после. Это то же правило, что и для параллельных задач.
    template <typename... Ts, typename Fn>
    void each(Fn&& fn) {
        constexpr size_t kCount = sizeof...(Ts);
        static_assert(kCount > 0, "обход без компонентов не имеет смысла");

        const ComponentId ids[kCount] = {componentIdOf<Ts>()...};
        ComponentMask required = 0;
        for (size_t i = 0; i < kCount; ++i) required |= bit(ids[i]);

        for (size_t index = 0; index < archetypes_.size(); ++index) {
            Archetype& archetype = archetypes_[index];
            if ((archetype.mask & required) != required) continue;
            if (archetype.rows == 0) continue;

            void* columns[kCount];
            for (size_t i = 0; i < kCount; ++i) columns[i] = columnData(archetype, ids[i]);

            for (uint32_t row = 0; row < archetype.rows; ++row) {
                invoke<Ts...>(fn, archetype.entities[row], columns, row,
                              std::index_sequence_for<Ts...>{});
            }
        }
    }

    /// Сколько сущностей подходит под набор компонентов. Без обхода строк.
    template <typename... Ts>
    uint32_t count() const {
        const ComponentId ids[sizeof...(Ts)] = {componentIdOf<Ts>()...};
        ComponentMask required = 0;
        for (size_t i = 0; i < sizeof...(Ts); ++i) required |= bit(ids[i]);

        uint32_t total = 0;
        for (const Archetype& archetype : archetypes_) {
            if ((archetype.mask & required) == required) total += archetype.rows;
        }
        return total;
    }

    // -----------------------------------------------------------------------
    // Ресурсы: то, что существует в мире в единственном экземпляре
    // -----------------------------------------------------------------------
    //
    // Граф галактики, таблицы баланса, журнал событий. Компонентами их делать
    // неправильно: они не принадлежат сущностям и не должны попадать в хеш
    // состояния. Мир хранит только указатель — владение остаётся снаружи.

    template <typename T>
    void setResource(T* value) {
        const uint32_t id = detail::resourceIdOf<T>();
        if (id >= resources_.size()) resources_.resize(id + 1, nullptr);
        resources_[id] = value;
    }

    template <typename T>
    T* resource() const {
        const uint32_t id = detail::resourceIdOf<T>();
        return id < resources_.size() ? static_cast<T*>(resources_[id]) : nullptr;
    }

    // -----------------------------------------------------------------------
    // Детерминизм
    // -----------------------------------------------------------------------

    /// Хеш состояния мира.
    ///
    /// Считается обходом по индексу сущности, а НЕ по внутренней раскладке
    /// таблиц. Иначе хеш зависел бы от истории перемещений строк, и два мира
    /// с одинаковым содержимым, но разной историей, дали бы разные хеши.
    uint64_t hash() const;

    /// Сколько типов компонентов зарегистрировано.
    uint32_t componentCount() const { return uint32_t(components_.size()); }
    /// Сколько таблиц создано. Полезно для профилирования: много редких
    /// архетипов — признак того, что компоненты навешиваются вразнобой.
    uint32_t archetypeCount() const { return uint32_t(archetypes_.size()); }

private:
    struct ComponentInfo {
        uint32_t size = 0;
        uint32_t align = 0;
        const char* name = "";
    };

    struct Archetype {
        ComponentMask mask = 0;
        std::vector<ComponentId> ids;                 // по возрастанию — канонический порядок
        std::vector<std::vector<uint8_t>> columns;    // столбец на компонент
        std::vector<Entity> entities;                 // строка -> сущность
        uint32_t rows = 0;
    };

    struct Record {
        uint32_t archetype = 0;
        uint32_t row = 0;
        uint32_t generation = 1;
        bool alive = false;
    };

    static constexpr ComponentMask bit(ComponentId id) { return ComponentMask(1) << id; }

    template <typename... Ts, typename Fn, size_t... I>
    static void invoke(Fn& fn, Entity entity, void* const* columns, uint32_t row,
                       std::index_sequence<I...>) {
        fn(entity, static_cast<Ts*>(columns[I])[row]...);
    }

    void* columnData(Archetype& archetype, ComponentId id);
    void* componentPtr(Entity entity, ComponentId id);

    uint32_t archetypeFor(ComponentMask mask);
    uint32_t appendRow(uint32_t archetypeIndex, Entity entity);
    void eraseRow(uint32_t archetypeIndex, uint32_t row);
    void migrate(Entity entity, ComponentMask target);

    void attach(Entity entity, ComponentId id);
    void detach(Entity entity, ComponentId id);

    std::vector<ComponentInfo> components_;
    std::vector<Archetype> archetypes_;
    // Только для поиска, обход по ней НИКОГДА не идёт: порядок в хеш-таблице
    // зависит от адресов и раскладки, а мир от них зависеть не имеет права.
    std::unordered_map<ComponentMask, uint32_t> archetypeByMask_;

    // Ресурсы в хеш не входят и на состояние не влияют — только поиск по типу.
    std::vector<void*> resources_;

    std::vector<Record> records_;
    std::vector<uint32_t> freeIndices_;
    uint32_t liveCount_ = 0;
};

}  // namespace pw::sim
