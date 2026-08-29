#include "pw/game/protocol.h"

namespace pw::game {

namespace {

/// Параметры галактики целиком.
///
/// Их тридцать штук, и все обязаны совпасть бит в бит: клиент строит
/// галактику той же функцией, что и сервер. Разойдись хоть один — игроки
/// увидят разные карты, и это проявится не сразу, а когда флот «промахнётся»
/// мимо системы, которой у соседа нет.
void writeParams(ByteWriter& writer, const sim::GalaxyParams& params) {
    writer.u64(params.seed);
    writer.varint(params.systemCount);
    writer.fixed(params.radius);
    writer.varint(params.arms);
    writer.fixed(params.minSpacing);
    writer.fixed(params.maxLaneLength);
    writer.varint(params.lanesPerSystem);
    writer.varint(params.armFractionPercent);
    writer.fixed(params.armSpread);
    writer.fixed(params.radialJitter);
    writer.fixed(params.twist);
    writer.varint(params.shortcutRangePercent);
    writer.varint(params.shortcutHopThreshold);
    writer.varint(params.shortcutRounds);
}

bool readParams(ByteReader& reader, sim::GalaxyParams& params) {
    params.seed = reader.u64();
    params.systemCount = uint32_t(reader.varint());
    params.radius = reader.fixed();
    params.arms = uint32_t(reader.varint());
    params.minSpacing = reader.fixed();
    params.maxLaneLength = reader.fixed();
    params.lanesPerSystem = uint32_t(reader.varint());
    params.armFractionPercent = uint32_t(reader.varint());
    params.armSpread = reader.fixed();
    params.radialJitter = reader.fixed();
    params.twist = reader.fixed();
    params.shortcutRangePercent = uint32_t(reader.varint());
    params.shortcutHopThreshold = uint32_t(reader.varint());
    params.shortcutRounds = uint32_t(reader.varint());
    if (reader.failed()) return false;

    // Здравый смысл: параметры приходят по сети, а генератор по ним будет
    // выделять память и крутить циклы. Клиент, доверившийся числу из
    // пакета, повесил бы себя сам.
    if (params.systemCount == 0 || params.systemCount > 1000000) return false;
    if (params.arms == 0 || params.arms > 64) return false;
    if (params.lanesPerSystem == 0 || params.lanesPerSystem > 32) return false;
    if (params.armFractionPercent > 100) return false;
    if (params.shortcutRounds > 16) return false;
    if (params.radius <= fx::zero()) return false;
    if (params.minSpacing <= fx::zero()) return false;
    if (params.maxLaneLength <= fx::zero()) return false;
    return true;
}

}  // namespace

void writeMessageType(ByteWriter& writer, MessageType type) { writer.u8(uint8_t(type)); }

bool readMessageType(ByteReader& reader, MessageType& type) {
    const uint8_t raw = reader.u8();
    if (reader.failed()) return false;
    switch (raw) {
        case uint8_t(MessageType::Join):
        case uint8_t(MessageType::Welcome):
        case uint8_t(MessageType::MoveFleet):
        case uint8_t(MessageType::BuildShip):
        case uint8_t(MessageType::Colonize):
        case uint8_t(MessageType::SplitFleet):
        case uint8_t(MessageType::BuildBuilding):
        case uint8_t(MessageType::Notice):
            type = MessageType(raw);
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------

void writeJoin(ByteWriter& writer, const JoinMessage& message) {
    writeMessageType(writer, MessageType::Join);
    writer.string(message.name);
}

bool readJoin(ByteReader& reader, JoinMessage& message) {
    message.name = reader.string();
    return !reader.failed();
}

void writeWelcome(ByteWriter& writer, const WelcomeMessage& message) {
    writeMessageType(writer, MessageType::Welcome);
    writeParams(writer, message.params);
    writer.varint(message.empire);
    writer.varint(message.capitalSystem);
    writer.varint(message.tick);
}

bool readWelcome(ByteReader& reader, WelcomeMessage& message) {
    if (!readParams(reader, message.params)) return false;
    message.empire = uint32_t(reader.varint());
    message.capitalSystem = uint32_t(reader.varint());
    message.tick = reader.varint();
    if (reader.failed()) return false;
    if (message.capitalSystem >= message.params.systemCount) return false;
    return true;
}

void writeMoveFleet(ByteWriter& writer, const MoveFleetMessage& message) {
    writeMessageType(writer, MessageType::MoveFleet);
    writer.varint(message.fleet);
    writer.varint(message.target);
}

bool readMoveFleet(ByteReader& reader, MoveFleetMessage& message) {
    message.fleet = uint32_t(reader.varint());
    message.target = uint32_t(reader.varint());
    return !reader.failed();
}

void writeBuildShip(ByteWriter& writer, const BuildShipMessage& message) {
    writeMessageType(writer, MessageType::BuildShip);
    writer.varint(message.system);
    writer.u8(message.hull);
    writer.u8(message.count);
}

bool readBuildShip(ByteReader& reader, BuildShipMessage& message) {
    message.system = uint32_t(reader.varint());
    message.hull = reader.u8();
    message.count = reader.u8();
    if (reader.failed()) return false;
    // Проверка здесь, а не у вызывающего: любой, кто забудет её сделать,
    // получит выход за границу таблицы корпусов.
    if (message.hull == 0 || message.hull >= uint8_t(sim::Hull::Count)) return false;
    if (message.count == 0) return false;
    return true;
}

void writeBuildBuilding(ByteWriter& writer, const BuildBuildingMessage& message) {
    writeMessageType(writer, MessageType::BuildBuilding);
    writer.varint(message.planet);
    writer.u8(message.slot);
    writer.u8(message.building);
}

bool readBuildBuilding(ByteReader& reader, BuildBuildingMessage& message) {
    message.planet = uint32_t(reader.varint());
    message.slot = reader.u8();
    message.building = reader.u8();
    if (reader.failed()) return false;
    if (message.slot >= sim::kMaxSlots) return false;
    if (message.building >= uint8_t(sim::Building::Count)) return false;
    return true;
}

void writeColonize(ByteWriter& writer, const ColonizeMessage& message) {
    writeMessageType(writer, MessageType::Colonize);
    writer.varint(message.fleet);
    writer.varint(message.planet);
}

bool readColonize(ByteReader& reader, ColonizeMessage& message) {
    message.fleet = uint32_t(reader.varint());
    message.planet = uint32_t(reader.varint());
    return !reader.failed();
}

void writeSplitFleet(ByteWriter& writer, const SplitFleetMessage& message) {
    writeMessageType(writer, MessageType::SplitFleet);
    writer.varint(message.fleet);
    // ЦИКЛОМ ПО ВСЕМ КЛАССАМ, а не перечислением полей. Перечисление
    // однажды уже отстало от таблицы корпусов: четыре класса из восьми
    // не ездили по сети, и флот из титанов выглядел пустым.
    for (size_t index = 0; index < sim::kHullClasses; ++index) {
        writer.varint(message.take.ships[index]);
    }
}

bool readSplitFleet(ByteReader& reader, SplitFleetMessage& message) {
    message.fleet = uint32_t(reader.varint());
    uint64_t total = 0;
    for (size_t index = 0; index < sim::kHullClasses; ++index) {
        const uint64_t count = reader.varint();
        // Потолок на класс: число из сети — это то, что прислал чужой код
        // на чужой машине, и «миллиард корветов» обязан отвалиться здесь,
        // а не переполнить счётчик где-то в симуляции.
        if (count > 0xFFFFu) return false;
        message.take.ships[index] = uint32_t(count);
        total += count;
    }
    if (reader.failed()) return false;
    // Пустое выделение — не приказ, а шум. Отказ здесь избавляет сервер
    // от разбора заведомо бессмысленного намерения.
    return total > 0;
}

void writeFleetCommand(ByteWriter& writer, const FleetCommandMessage& message) {
    writeMessageType(writer, MessageType::FleetCommand);
    writer.varint(message.fleet);
    writer.u8(uint8_t(message.command));
    writer.varint(message.value);
}

bool readFleetCommand(ByteReader& reader, FleetCommandMessage& message) {
    message.fleet = uint32_t(reader.varint());
    const uint8_t command = reader.u8();
    const uint64_t value = reader.varint();
    if (reader.failed()) return false;
    // Неизвестная команда — либо другая версия клиента, либо подделка.
    // И то и другое разбирать нельзя: дальше по коду стоит switch,
    // и значение вне перечисления провалилось бы мимо всех веток.
    if (command >= uint8_t(FleetCommand::Count)) return false;
    if (value > 0xFFFFFFFFull) return false;
    message.command = FleetCommand(command);
    message.value = uint32_t(value);
    return true;
}

void writeNotice(ByteWriter& writer, const NoticeMessage& message) {
    writeMessageType(writer, MessageType::Notice);
    writer.u8(uint8_t(message.kind));
    writer.varint(message.system);
}

bool readNotice(ByteReader& reader, NoticeMessage& message) {
    const uint8_t raw = reader.u8();
    message.system = uint32_t(reader.varint());
    if (reader.failed()) return false;
    if (raw >= uint8_t(NoticeKind::Count)) return false;
    message.kind = NoticeKind(raw);
    return true;
}

}  // namespace pw::game
