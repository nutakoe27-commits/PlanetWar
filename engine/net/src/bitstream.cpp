#include "pw/net/bitstream.h"

namespace pw::net {

// ---------------------------------------------------------------------------
// Запись
// ---------------------------------------------------------------------------

bool ByteWriter::reserve(size_t bytes) {
    if (overflowed_) return false;
    if (size_ + bytes > capacity_) {
        overflowed_ = true;
        return false;
    }
    return true;
}

void ByteWriter::u8(uint8_t value) {
    if (!reserve(1)) return;
    data_[size_++] = value;
}

// Порядок байт задан ЯВНО, а не взят у машины: иначе игрок на ARM и игрок
// на x86 читали бы один пакет по-разному, и рассинхрон нашёлся бы не в
// тесте, а в бою.
void ByteWriter::u16(uint16_t value) {
    if (!reserve(2)) return;
    data_[size_++] = uint8_t(value & 0xFF);
    data_[size_++] = uint8_t((value >> 8) & 0xFF);
}

void ByteWriter::u32(uint32_t value) {
    if (!reserve(4)) return;
    for (int shift = 0; shift < 32; shift += 8) {
        data_[size_++] = uint8_t((value >> shift) & 0xFF);
    }
}

void ByteWriter::u64(uint64_t value) {
    if (!reserve(8)) return;
    for (int shift = 0; shift < 64; shift += 8) {
        data_[size_++] = uint8_t((value >> shift) & 0xFF);
    }
}

void ByteWriter::varint(uint64_t value) {
    // LEB128: семь бит полезных, старший бит — «есть продолжение».
    // Ноль занимает один байт, номер системы 42 — тоже один.
    uint8_t buffer[10];
    size_t count = 0;
    do {
        uint8_t byte = uint8_t(value & 0x7F);
        value >>= 7;
        if (value != 0) byte |= 0x80;
        buffer[count++] = byte;
    } while (value != 0);

    if (!reserve(count)) return;
    for (size_t i = 0; i < count; ++i) data_[size_++] = buffer[i];
}

void ByteWriter::svarint(int64_t value) {
    // Zigzag: 0 -> 0, -1 -> 1, 1 -> 2, -2 -> 3. Без него -1 в дополнительном
    // коде — это все шестьдесят четыре бита в единицах, то есть десять байт
    // на самое частое отрицательное число протокола.
    const uint64_t encoded = (uint64_t(value) << 1) ^ uint64_t(value >> 63);
    varint(encoded);
}

void ByteWriter::fixed(fx value) {
    // Fixed-point идёт как обычное знаковое целое: у него внутри и есть
    // целое. Чисел с плавающей точкой в протоколе нет — по той же причине,
    // по которой их нет в pw_sim.
    svarint(value.raw());
}

void ByteWriter::boolean(bool value) { u8(value ? 1 : 0); }

void ByteWriter::bytes(const void* source, size_t size) {
    if (!reserve(size)) return;
    if (size > 0) std::memcpy(data_ + size_, source, size);
    size_ += size;
}

void ByteWriter::string(const std::string& value) {
    const size_t length = value.size() > kMaxStringLength ? kMaxStringLength : value.size();
    varint(uint64_t(length));
    bytes(value.data(), length);
}

// ---------------------------------------------------------------------------
// Чтение
// ---------------------------------------------------------------------------

bool ByteReader::require(size_t bytes) {
    if (failed_) return false;
    if (position_ + bytes > size_) {
        failed_ = true;
        return false;
    }
    return true;
}

uint8_t ByteReader::u8() {
    if (!require(1)) return 0;
    return data_[position_++];
}

uint16_t ByteReader::u16() {
    if (!require(2)) return 0;
    const uint16_t low = data_[position_];
    const uint16_t high = data_[position_ + 1];
    position_ += 2;
    return uint16_t(low | (high << 8));
}

uint32_t ByteReader::u32() {
    if (!require(4)) return 0;
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value |= uint32_t(data_[position_ + size_t(i)]) << (i * 8);
    position_ += 4;
    return value;
}

uint64_t ByteReader::u64() {
    if (!require(8)) return 0;
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |= uint64_t(data_[position_ + size_t(i)]) << (i * 8);
    position_ += 8;
    return value;
}

uint64_t ByteReader::varint() {
    uint64_t value = 0;
    // Десять групп по семь бит перекрывают все 64. Одиннадцатая означает,
    // что отправитель либо сломан, либо пытается загнать нас в цикл.
    for (int index = 0; index < 10; ++index) {
        if (!require(1)) return 0;
        const uint8_t byte = data_[position_++];
        value |= uint64_t(byte & 0x7F) << (index * 7);
        if ((byte & 0x80) == 0) return value;
    }
    failed_ = true;
    return 0;
}

int64_t ByteReader::svarint() {
    const uint64_t encoded = varint();
    return int64_t((encoded >> 1) ^ (~(encoded & 1) + 1));
}

fx ByteReader::fixed() { return fx::fromRaw(svarint()); }

bool ByteReader::boolean() { return u8() != 0; }

void ByteReader::bytes(void* destination, size_t size) {
    if (!require(size)) {
        // Приёмник обязан остаться определённым даже на битом пакете:
        // вызывающий увидит нули, а не то, что лежало в памяти до нас.
        if (size > 0) std::memset(destination, 0, size);
        return;
    }
    if (size > 0) std::memcpy(destination, data_ + position_, size);
    position_ += size;
}

std::string ByteReader::string() {
    const uint64_t length = varint();
    if (failed_) return {};
    // Длина приходит по сети, то есть от кого угодно. Без этой проверки
    // одно поле заставляло бы нас выделить сколько угодно памяти.
    if (length > kMaxStringLength || length > remaining()) {
        failed_ = true;
        return {};
    }
    std::string out(size_t(length), '\0');
    bytes(out.data(), size_t(length));
    return out;
}

}  // namespace pw::net
