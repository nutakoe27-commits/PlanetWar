#include "pw/sim/world.h"

#include <algorithm>

namespace pw::sim {

World::World() {
    // Архетип 0 — пустой набор. Сюда попадают только что созданные сущности,
    // у которых ещё нет ни одного компонента.
    archetypes_.push_back(Archetype{});
    archetypeByMask_[0] = 0;
}

// ---------------------------------------------------------------------------
// Сущности
// ---------------------------------------------------------------------------

Entity World::create() {
    uint32_t index;
    if (!freeIndices_.empty()) {
        index = freeIndices_.back();
        freeIndices_.pop_back();
    } else {
        index = uint32_t(records_.size());
        records_.push_back(Record{});
    }

    Record& record = records_[index];
    record.alive = true;
    record.archetype = 0;

    const Entity entity{index, record.generation};
    record.row = appendRow(0, entity);
    ++liveCount_;
    return entity;
}

void World::destroy(Entity entity) {
    if (!alive(entity)) return;

    Record& record = records_[entity.index];
    eraseRow(record.archetype, record.row);
    record.alive = false;
    // Поколение растёт, поэтому старые дескрипторы на переиспользованный
    // индекс перестают проходить проверку alive.
    ++record.generation;
    freeIndices_.push_back(entity.index);
    --liveCount_;
}

bool World::alive(Entity entity) const {
    if (entity.index >= records_.size()) return false;
    const Record& record = records_[entity.index];
    return record.alive && record.generation == entity.generation;
}

// ---------------------------------------------------------------------------
// Таблицы
// ---------------------------------------------------------------------------

uint32_t World::archetypeFor(ComponentMask mask) {
    const auto found = archetypeByMask_.find(mask);
    if (found != archetypeByMask_.end()) return found->second;

    Archetype archetype;
    archetype.mask = mask;
    // Идентификаторы по возрастанию — канонический порядок столбцов.
    // На него опирается хеш мира.
    for (ComponentId id = 0; id < components_.size(); ++id) {
        if (mask & bit(id)) archetype.ids.push_back(id);
    }
    archetype.columns.resize(archetype.ids.size());

    const uint32_t index = uint32_t(archetypes_.size());
    archetypes_.push_back(std::move(archetype));
    archetypeByMask_[mask] = index;
    return index;
}

uint32_t World::appendRow(uint32_t archetypeIndex, Entity entity) {
    Archetype& archetype = archetypes_[archetypeIndex];
    const uint32_t row = archetype.rows;

    for (size_t column = 0; column < archetype.ids.size(); ++column) {
        const uint32_t size = components_[archetype.ids[column]].size;
        // Новая строка обнуляется. has_unique_object_representations уже
        // запретил байты выравнивания, но обнуление делает содержимое
        // предсказуемым и без явной инициализации на стороне игры.
        archetype.columns[column].resize(size_t(row + 1) * size, 0);
    }
    archetype.entities.push_back(entity);
    archetype.rows = row + 1;
    return row;
}

void World::eraseRow(uint32_t archetypeIndex, uint32_t row) {
    Archetype& archetype = archetypes_[archetypeIndex];
    const uint32_t last = archetype.rows - 1;

    // Удаление обменом с последней строкой: O(1) вместо сдвига хвоста.
    // Порядок строк при этом теряется — поэтому хеш мира считается по
    // индексам сущностей, а не по раскладке таблиц.
    if (row != last) {
        for (size_t column = 0; column < archetype.ids.size(); ++column) {
            const uint32_t size = components_[archetype.ids[column]].size;
            uint8_t* data = archetype.columns[column].data();
            std::memcpy(data + size_t(row) * size, data + size_t(last) * size, size);
        }
        const Entity moved = archetype.entities[last];
        archetype.entities[row] = moved;
        records_[moved.index].row = row;
    }

    for (size_t column = 0; column < archetype.ids.size(); ++column) {
        const uint32_t size = components_[archetype.ids[column]].size;
        archetype.columns[column].resize(size_t(last) * size);
    }
    archetype.entities.pop_back();
    archetype.rows = last;
}

void World::migrate(Entity entity, ComponentMask target) {
    Record& record = records_[entity.index];
    const uint32_t fromIndex = record.archetype;
    if (archetypes_[fromIndex].mask == target) return;

    const uint32_t toIndex = archetypeFor(target);
    const uint32_t fromRow = record.row;
    const uint32_t toRow = appendRow(toIndex, entity);

    // Переносим значения общих компонентов. Оба набора идентификаторов
    // отсортированы, поэтому идём слиянием, без поиска.
    {
        const Archetype& from = archetypes_[fromIndex];
        Archetype& to = archetypes_[toIndex];
        size_t a = 0, b = 0;
        while (a < from.ids.size() && b < to.ids.size()) {
            if (from.ids[a] < to.ids[b]) {
                ++a;
            } else if (to.ids[b] < from.ids[a]) {
                ++b;
            } else {
                const uint32_t size = components_[from.ids[a]].size;
                std::memcpy(to.columns[b].data() + size_t(toRow) * size,
                            from.columns[a].data() + size_t(fromRow) * size, size);
                ++a;
                ++b;
            }
        }
    }

    record.archetype = toIndex;
    record.row = toRow;
    eraseRow(fromIndex, fromRow);
}

// ---------------------------------------------------------------------------
// Компоненты
// ---------------------------------------------------------------------------

void World::attach(Entity entity, ComponentId id) {
    if (!alive(entity) || id >= components_.size()) return;
    const ComponentMask current = archetypes_[records_[entity.index].archetype].mask;
    migrate(entity, current | bit(id));
}

void World::detach(Entity entity, ComponentId id) {
    if (!alive(entity) || id >= components_.size()) return;
    const ComponentMask current = archetypes_[records_[entity.index].archetype].mask;
    if ((current & bit(id)) == 0) return;
    migrate(entity, current & ~bit(id));
}

void* World::columnData(Archetype& archetype, ComponentId id) {
    const auto found = std::lower_bound(archetype.ids.begin(), archetype.ids.end(), id);
    if (found == archetype.ids.end() || *found != id) return nullptr;
    return archetype.columns[size_t(found - archetype.ids.begin())].data();
}

void* World::componentPtr(Entity entity, ComponentId id) {
    if (!alive(entity) || id >= components_.size()) return nullptr;
    const Record& record = records_[entity.index];
    Archetype& archetype = archetypes_[record.archetype];
    if ((archetype.mask & bit(id)) == 0) return nullptr;

    uint8_t* column = static_cast<uint8_t*>(columnData(archetype, id));
    return column + size_t(record.row) * components_[id].size;
}

// ---------------------------------------------------------------------------
// Хеш состояния
// ---------------------------------------------------------------------------

uint64_t World::hash() const {
    Hasher hasher;
    hasher.u64(liveCount_);

    // Обход по индексу сущности. Именно так хеш перестаёт зависеть от истории
    // перемещений строк: два мира с одинаковым содержимым, но разной историей
    // создания и удаления, обязаны давать одинаковый хеш.
    for (uint32_t index = 0; index < records_.size(); ++index) {
        const Record& record = records_[index];
        if (!record.alive) continue;

        hasher.u32(index).u32(record.generation);

        const Archetype& archetype = archetypes_[record.archetype];
        for (size_t column = 0; column < archetype.ids.size(); ++column) {
            const ComponentId id = archetype.ids[column];
            const uint32_t size = components_[id].size;
            hasher.u32(id);
            hasher.bytes(archetype.columns[column].data() + size_t(record.row) * size, size);
        }
    }
    return hasher.value();
}

}  // namespace pw::sim
