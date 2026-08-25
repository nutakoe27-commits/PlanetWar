#pragma once

// Запись и чтение пакетов.
//
// Три требования, из которых выведено всё остальное:
//
//   1. ФОРМАТ ОДИНАКОВ НА ВСЕХ ПЛАТФОРМАХ. Игрок на ARM-маке и игрок на
//      x86-Windows читают один и тот же байт одинаково. Поэтому порядок байт
//      задан явно (little-endian на проводе), а не берётся у машины, и в
//      протоколе нет ни одного числа с плавающей точкой — как и в pw_sim.
//
//   2. БИТЫЙ ПАКЕТ НЕ ЛОМАЕТ СЕРВЕР. Пакет приходит из сети, то есть от
//      кого угодно. Читатель никогда не выходит за границу буфера: при
//      попытке он поднимает флаг и дальше отдаёт нули. Одна проверка
//      failed() в конце разбора закрывает весь пакет целиком — не нужно
//      проверять каждое поле по отдельности и невозможно забыть проверку.
//
//   3. РАЗМЕР ИМЕЕТ ЗНАЧЕНИЕ. Бюджет — 20 КБ/с на игрока в бою
//      (docs/03-NETWORK-AND-SERVER.md). Поэтому целые пишутся varint'ом:
//      номер системы 42 занимает один байт, а не четыре.
//
// Исключений нет намеренно: разбор пакета — горячий путь, и он обязан
// стоить одинаково для честного пакета и для мусора.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "pw/core/fixed.h"

namespace pw::net {

/// Максимальная длина строки в протоколе.
///
/// Ограничение существует, чтобы отправитель не мог одним полем заставить
/// получателя выделить память произвольного размера.
inline constexpr uint32_t kMaxStringLength = 256;

/// Сколько байт займёт значение в varint.
///
/// Нужна, чтобы решать «влезет ли» ДО записи. У ByteWriter намеренно нет
/// отката: он умеет только вперёд, и это делает его невозможно использовать
/// неправильно. Значит, тот, кто укладывает данные в пакет по бюджету,
/// обязан считать размер заранее.
constexpr size_t varintSize(uint64_t value) {
    size_t count = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Запись
// ---------------------------------------------------------------------------

/// Пишет в чужой буфер фиксированного размера.
///
/// Не владеет памятью и не выделяет её: буфер приходит снаружи, обычно это
/// кусок пакета на стеке. Переполнение не аварийно — поднимается флаг,
/// и дальнейшая запись игнорируется.
class ByteWriter {
public:
    ByteWriter(uint8_t* data, size_t capacity) : data_(data), capacity_(capacity) {}

    void u8(uint8_t value);
    void u16(uint16_t value);
    void u32(uint32_t value);
    void u64(uint64_t value);

    /// Целое переменной длины (LEB128): маленькие числа занимают мало места.
    void varint(uint64_t value);
    /// Знаковое переменной длины: zigzag, чтобы -1 занимал байт, а не десять.
    void svarint(int64_t value);

    void fixed(fx value);
    void boolean(bool value);
    void bytes(const void* source, size_t size);
    void string(const std::string& value);

    size_t size() const { return size_; }
    size_t capacity() const { return capacity_; }
    /// Не хватило места. Пакет собран не полностью и отправлять его нельзя.
    bool overflowed() const { return overflowed_; }
    const uint8_t* data() const { return data_; }

private:
    uint8_t* data_;
    size_t capacity_;
    size_t size_ = 0;
    bool overflowed_ = false;

    bool reserve(size_t bytes);
};

// ---------------------------------------------------------------------------
// Чтение
// ---------------------------------------------------------------------------

/// Читает из чужого буфера, никогда не выходя за его границу.
///
/// После первой же неудачи поток считается испорченным: failed() остаётся
/// поднятым до конца разбора, а все чтения отдают нули. Это нарочно —
/// частично разобранный пакет опаснее отброшенного.
class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();

    uint64_t varint();
    int64_t svarint();

    fx fixed();
    bool boolean();
    /// Прочитать ровно `size` байт. При нехватке — флаг и нули в приёмнике.
    void bytes(void* destination, size_t size);
    std::string string();

    size_t position() const { return position_; }
    size_t remaining() const { return failed_ ? 0 : size_ - position_; }
    /// Поток испорчен: пакет обрезан, повреждён или намеренно подделан.
    bool failed() const { return failed_; }
    /// Разбор корректен И пакет прочитан целиком.
    ///
    /// Хвост — не мелочь: он означает, что отправитель и получатель
    /// понимают схему по-разному, а значит дальше разойдётся и состояние.
    bool complete() const { return !failed_ && position_ == size_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t position_ = 0;
    bool failed_ = false;

    bool require(size_t bytes);
};

}  // namespace pw::net
