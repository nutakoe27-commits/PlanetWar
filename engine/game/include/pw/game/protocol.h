#pragma once

// Протокол игры: что именно ездит между клиентом и сервером.
//
// Живёт отдельным модулем, потому что знает и правила игры (pw_sim), и
// транспорт (pw_net). Ни тот, ни другой о нём не знают — и это важно:
// симуляция обязана прогоняться без сети (в CI, в реплее, в ночном
// прогоне), а транспорт обязан ничего не знать про флоты, чтобы его
// можно было заменить.
//
// ГАЛАКТИКА ПО СЕТИ НЕ ЕЗДИТ. Она выводится из сида детерминированно,
// поэтому клиент строит её у себя сам, а по проводу идёт только сид
// и параметры формы — три десятка байт вместо сотен килобайт. Это то же
// решение, что даёт «бесконечность планет» (docs/03).
//
// ЧИСЕЛ С ПЛАВАЮЩЕЙ ТОЧКОЙ ЗДЕСЬ НЕТ. Клиент строит галактику той же
// функцией, что и сервер, и обязан получить бит в бит то же самое.

#include <cstdint>
#include <string>
#include <vector>

#include "pw/net/bitstream.h"
#include "pw/sim/economy.h"
#include "pw/sim/fleet.h"
#include "pw/sim/galaxy.h"

namespace pw::game {

using namespace pw::net;

/// Надёжные сообщения. Едут каналом с гарантией порядка и однократности.
enum class MessageType : uint8_t {
    /// Клиент представляется после установления соединения.
    Join = 1,
    /// Сервер отдаёт сид галактики и номер империи игрока.
    Welcome = 2,
    /// Приказ флоту идти в систему.
    MoveFleet = 10,
    /// Заказ корабля на верфи системы.
    BuildShip = 11,
    /// Постройка здания в слоте планеты.
    BuildBuilding = 12,
    /// Высадка колонии на ничью планету.
    Colonize = 13,
    /// Выделить корабли в отдельный флот. Составом, а не по одному классу.
    SplitFleet = 14,
    /// Всё остальное управление отрядом: маршрут, стойка, приписка, слияние.
    FleetCommand = 15,
    /// Сервер сообщает о событии, которое игрок обязан заметить.
    Notice = 20,
};

/// О чём сервер уведомляет игрока.
enum class NoticeKind : uint8_t {
    None = 0,
    SystemCaptured = 1,
    SystemLost = 2,
    BattleWon = 3,
    BattleLost = 4,
    FleetDestroyed = 5,
    /// Приказ отвергнут: не ваш флот, нет пути, не хватает ресурсов.
    OrderRejected = 6,
    /// Бой без победителя. Приходит ОБЕИМ сторонам.
    ///
    /// Взаимное истребление — тоже исход, и молчать о нём нельзя: игрок
    /// иначе узнаёт о гибели флота только по его пропаже с карты.
    BattleDraw = 7,

    /// Вашу планету начали осаждать.
    ///
    /// Без этой новости обещание из control.h — «владелец успевает
    /// получить уведомление, союзники увидят осаду и придут» — было
    /// пустым: сервер молчал, и узнать об осаде можно было, только
    /// глядя в нужную часть карты в нужную минуту.
    PlanetSieged = 8,
    /// Планета потеряна.
    PlanetLost = 9,
    /// Планета взята.
    PlanetCaptured = 10,
    /// Колония основана. Отдельно от «планета взята»: это разные события
    /// с разной ценой, и в журнале они обязаны выглядеть по-разному.
    ColonyFounded = 11,

    /// Граница допустимых значений. Существует ради разбора пакета.
    ///
    /// Проверка «не больше последнего вида» жила в readNotice константой,
    /// и добавленный вид молча не проходил разбор: сервер слал новость,
    /// клиент её выбрасывал, и найти это можно было только отладкой.
    /// Со счётчиком добавление вида не требует помнить о втором месте.
    Count,
};

// ---------------------------------------------------------------------------
// Сообщения
// ---------------------------------------------------------------------------

struct JoinMessage {
    std::string name;
};

/// Всё, что нужно, чтобы клиент построил ту же галактику, что и сервер.
struct WelcomeMessage {
    sim::GalaxyParams params;
    uint32_t empire = 0;
    uint32_t capitalSystem = 0;
    /// Тик сервера на момент отправки — клиент понимает, насколько отстал.
    uint64_t tick = 0;
};

struct MoveFleetMessage {
    uint32_t fleet = 0;
    uint32_t target = 0;
};

struct BuildShipMessage {
    uint32_t system = 0;
    uint8_t hull = 0;
    uint8_t count = 1;
};

struct BuildBuildingMessage {
    uint32_t planet = 0;
    uint8_t slot = 0;
    uint8_t building = 0;
};

struct ColonizeMessage {
    uint32_t fleet = 0;
    /// Номер сущности планеты. Не «орбита в системе»: планета —
    /// самостоятельная сущность, и адресовать её парой чисел значило бы
    /// заставить сервер повторять поиск, который клиент уже сделал.
    uint32_t planet = 0;
};

struct SplitFleetMessage {
    uint32_t fleet = 0;
    /// ПОЛНЫЙ СОСТАВ, а не пара «класс — сколько».
    ///
    /// Собрать «шесть корветов, два эсминца и тендер» тремя приказами
    /// нельзя: три приказа дают три отряда, и собрать их обратно в один —
    /// ещё три действия. Один приказ — один отряд.
    sim::Fleet take{};
};

/// Что сделать с отрядом.
///
/// Одно сообщение на всё управление, а не восемь сообщений по одному
/// полю. У всех этих команд общая форма — «отряду такому-то сделать
/// то-то с числом таким-то», — и восемь почти одинаковых пар write/read
/// были бы восемью местами, где можно забыть проверку.
enum class FleetCommand : uint8_t {
    /// Заменить маршрут одной точкой. `value` — система.
    RouteSet = 0,
    /// Добавить точку в конец маршрута. `value` — система.
    RouteAppend,
    /// Снять маршрут. `value` не используется.
    RouteClear,
    /// Сменить стойку. `value` — sim::Stance.
    SetStance,
    /// Включить или выключить уклонение. `value` — 0 или 1.
    SetEvade,
    /// Приписать к системе целиком. `value` — номер системы.
    AnchorSystem,
    /// Приписать к планете. `value` — номер СУЩНОСТИ планеты.
    ///
    /// Сущности, а не пары «система, орбита»: игрок тычет в планету,
    /// и заставлять сервер повторять поиск, который клиент уже сделал,
    /// незачем. Так же адресуется планета и в приказе на колонизацию.
    AnchorPlanet,
    /// Влить в этот отряд другой. `value` — номер сущности источника.
    Merge,
    Count
};

struct FleetCommandMessage {
    uint32_t fleet = 0;
    /// Система, орбита, стойка, номер другого отряда — по смыслу команды.
    uint32_t value = 0;
    FleetCommand command = FleetCommand::RouteClear;
};

struct NoticeMessage {
    NoticeKind kind = NoticeKind::None;
    uint32_t system = 0;
};

// ---------------------------------------------------------------------------
// Запись и разбор
//
// Каждая функция парная: write*/read*. Тесты гоняют их туда-обратно на
// случайных значениях — это ловит расхождение записи и чтения, которое
// иначе проявилось бы как необъяснимый рассинхрон у игроков.
// ---------------------------------------------------------------------------

void writeJoin(ByteWriter& writer, const JoinMessage& message);
bool readJoin(ByteReader& reader, JoinMessage& message);

void writeWelcome(ByteWriter& writer, const WelcomeMessage& message);
bool readWelcome(ByteReader& reader, WelcomeMessage& message);

void writeMoveFleet(ByteWriter& writer, const MoveFleetMessage& message);
bool readMoveFleet(ByteReader& reader, MoveFleetMessage& message);

void writeBuildShip(ByteWriter& writer, const BuildShipMessage& message);
bool readBuildShip(ByteReader& reader, BuildShipMessage& message);

void writeBuildBuilding(ByteWriter& writer, const BuildBuildingMessage& message);
bool readBuildBuilding(ByteReader& reader, BuildBuildingMessage& message);

void writeColonize(ByteWriter& writer, const ColonizeMessage& message);
bool readColonize(ByteReader& reader, ColonizeMessage& message);

void writeSplitFleet(ByteWriter& writer, const SplitFleetMessage& message);
bool readSplitFleet(ByteReader& reader, SplitFleetMessage& message);

void writeFleetCommand(ByteWriter& writer, const FleetCommandMessage& message);
bool readFleetCommand(ByteReader& reader, FleetCommandMessage& message);

void writeNotice(ByteWriter& writer, const NoticeMessage& message);
bool readNotice(ByteReader& reader, NoticeMessage& message);

/// Прочитать тип сообщения. Пишется первым байтом каждого сообщения.
bool readMessageType(ByteReader& reader, MessageType& type);
void writeMessageType(ByteWriter& writer, MessageType type);

}  // namespace pw::game
