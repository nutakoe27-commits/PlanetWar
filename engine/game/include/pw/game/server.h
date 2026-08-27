#pragma once

// Сервер: авторитетная симуляция плюс приём игроков.
//
// АВТОРИТЕТ ЗДЕСЬ И ТОЛЬКО ЗДЕСЬ. Клиент не применяет игровых правил
// вообще — он рисует то, что прислали, и отправляет намерения. Это стоит
// немного отзывчивости и покупает полную невозможность клиентских читов
// (docs/03): подделать пакет можно, но сервер всё равно проверит, ваш ли
// это флот, есть ли путь и хватает ли ресурсов.
//
// СЕРВЕР НЕ ЗНАЕТ ПРО СОКЕТЫ. Он принимает пришедшие датаграммы и отдаёт
// готовые к отправке — кто их носит, дело вызывающего. Поэтому весь
// сервер целиком, вместе с рукопожатием, приказами и снапшотами,
// проверяется в памяти на симуляторе плохой сети, без единого системного
// вызова и без единой настоящей паузы.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "pw/game/protocol.h"
#include "pw/game/snapshot.h"
#include "pw/net/connection.h"
#include "pw/sim/commands.h"
#include "pw/sim/control.h"
#include "pw/sim/economy.h"
#include "pw/sim/galaxy.h"
#include "pw/sim/season.h"
#include "pw/sim/production.h"
#include "pw/sim/schedule.h"
#include "pw/sim/world.h"

namespace pw::game {

/// Настройки запуска сезона.
struct ServerConfig {
    sim::GalaxyParams galaxy;
    /// Сколько игроков сервер согласен принять.
    uint32_t maxPlayers = 16;
    /// Стартовые ресурсы каждой империи.
    int64_t startingEnergy = 200;
    int64_t startingMinerals = 300;
    int64_t startingAlloys = 500;
    /// Стартовый флот. ОДИН КОЛОНИЗАТОР В НЁМ ОБЯЗАТЕЛЕН.
    ///
    /// Империя начинается с одной планеты, и без колонизатора расширяться
    /// было бы нечем: чтобы построить первого, нужна верфь, чтобы верфь —
    /// минералы и время, а всё это добывается ровно с той одной планеты.
    /// Игрок первые двадцать минут смотрел бы на застройку единственного
    /// мира, не имея ни одного решения, кроме порядка зданий.
    ///
    /// С колонизатором на борту первое решение принимается на второй
    /// минуте и оно настоящее: КУДА его послать. Обратно этот корабль
    /// не вернётся — он тратится, — и выбор соседа определяет, с кем
    /// игрок будет граничить весь сезон.
    sim::Fleet startingFleet = sim::makeFleet(
        {{sim::Hull::Corvette, 8}, {sim::Hull::Destroyer, 2}, {sim::Hull::Colonizer, 1}});

    /// Длительность стадий сезона.
    ///
    /// По умолчанию сезон длится два часа игрового времени — столько же,
    /// сколько ночной прогон в CI. Живой сервер ставит `scale` в несколько
    /// сотен и получает те самые 8–12 недель из docs/01.
    sim::SeasonConfig season;

    /// Во сколько раз игровое время быстрее реального.
    ///
    /// Существует для оценки и отладки, а не для игры: сезон рассчитан
    /// на 8–12 недель, и растянутость там осмысленна. Но чтобы ПОСМОТРЕТЬ,
    /// как разворачивается партия, шесть минут до первого захвата — это
    /// шесть минут ожидания за каждую проверку.
    ///
    /// На детерминизм не влияет: меняется частота тиков в реальном
    /// времени, а не их содержание. Мир, прокрученный на тысячу тиков,
    /// одинаков при любой скорости.
    uint32_t speed = 1;
};

/// Пакет, который сервер просит отправить.
struct OutgoingPacket {
    net::Address to;
    std::vector<uint8_t> data;
};

/// Один подключённый игрок.
struct Player {
    net::Connection connection;
    SnapshotWriter snapshots;
    sim::Entity empireEntity;
    uint32_t empire = 0;
    uint32_t home = 0;
    /// Орбита столицы в родной системе. Империя начинается с ОДНОЙ
    /// планеты, и надо помнить, с какой именно: на неё встаёт стартовый
    /// флот, и от неё игрок расширяется.
    uint32_t capitalOrbit = 0;
    std::string name;
    bool joined = false;
    /// Номер снапшота, который игрок подтвердил последним.
    uint16_t acknowledged = 0;
};

class Server {
public:
    /// Поднять сезон. Галактика генерируется здесь и живёт до конца.
    void start(const ServerConfig& config);

    /// Принять датаграмму. `now` — время в миллисекундах, приходит снаружи.
    void receive(const net::Address& from, const uint8_t* data, size_t size, int64_t now);

    /// Прокрутить симуляцию и собрать исходящие пакеты.
    ///
    /// Симуляция тикает ровно kTicksPerSecond раз в секунду по СВОИМ часам,
    /// а не по числу вызовов: иначе сервер на медленной машине играл бы
    /// в замедленную игру, а на быстрой — в ускоренную.
    void update(int64_t now, std::vector<OutgoingPacket>& outgoing);

    /// Разорвать всех: сервер выключается.
    void shutdown(std::vector<OutgoingPacket>& outgoing);

    // --- наблюдение ---

    uint64_t tick() const { return tick_; }
    uint32_t playerCount() const;
    const sim::Galaxy& galaxy() const { return galaxy_; }
    sim::World& world() { return world_; }
    const std::map<net::Address, Player>& players() const { return players_; }
    /// Сколько приказов сервер отверг: не ваш флот, нет пути, нет ресурсов.
    uint64_t rejectedOrders() const { return rejectedOrders_; }

private:
    ServerConfig config_;
    sim::World world_;
    sim::Galaxy galaxy_;
    sim::Ledger ledger_;
    sim::Commands commands_;
    sim::Presence presence_;
    sim::Season season_;

    std::map<net::Address, Player> players_;
    std::vector<bool> homeTaken_;

    uint64_t tick_ = 0;
    int64_t startedAt_ = 0;
    bool running_ = false;
    uint64_t rejectedOrders_ = 0;

    WorldView view_;

    /// Владельцы систем на прошлом тике и живые флоты на прошлом тике.
    ///
    /// Нужны, чтобы заметить событие: система сменила хозяина, флот
    /// пропал. Игрок обязан узнавать об этом сам, а не замечать
    /// изменение на карте — в MMO он часто смотрит в другую её часть.
    std::vector<uint8_t> previousOwners_;
    std::vector<std::pair<uint32_t, uint32_t>> previousFleets_;   // сущность, империя
    /// Суммарный тоннаж каждой империи на прошлом тике.
    ///
    /// Именно тоннаж, а не число отрядов: слияние уменьшает количество,
    /// не теряя ни одного корабля, и по количеству потеря неотличима
    /// от обычной перегруппировки.
    std::vector<uint32_t> previousTonnage_;
    /// Перезарядка боя в каждой системе на прошлом тике. Скачок вверх
    /// означает, что сражение только что произошло.
    std::vector<uint32_t> previousCooldown_;

    /// Состояние планет в прошлом тике: владелец и осаждающий.
    ///
    /// Ключ — номер сущности планеты. Не вектор по индексу: планеты
    /// создаются генератором вперемешку с системами, и их номера
    /// не образуют плотного диапазона.
    struct PlanetMemory {
        uint8_t owner = 0xFF;
        uint8_t besieger = 0xFF;
        uint32_t system = 0;
    };
    std::map<uint32_t, PlanetMemory> previousPlanets_;
    /// Отказы, собранные в receive: отправлять оттуда некуда, поэтому
    /// они ждут ближайшего update.
    std::vector<OutgoingPacket> pendingRejects_;

    void step();
    void handleMessage(Player& player, const uint8_t* data, size_t size,
                       std::vector<OutgoingPacket>& outgoing);
    void applyMove(Player& player, const MoveFleetMessage& message);
    void applyBuildShip(Player& player, const BuildShipMessage& message);
    void applyBuildBuilding(Player& player, const BuildBuildingMessage& message);
    void applyColonize(Player& player, const ColonizeMessage& message);
    void applySplitFleet(Player& player, const SplitFleetMessage& message);
    void sendWelcome(Player& player);
    /// Разослать уведомления о том, что изменилось за тик.
    void notifyChanges();
    void notify(uint32_t empire, NoticeKind kind, uint32_t system);
    /// Выбрать стартовую систему подальше от уже занятых.
    uint32_t pickHome();
};

}  // namespace pw::game
