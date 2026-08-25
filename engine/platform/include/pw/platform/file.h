// pw_platform — файлы.
//
// Тонкий слой поверх стандартных потоков плюс поиск каталогов, которые на
// каждой платформе лежат в своём месте. На Android ассеты вообще не файлы,
// а записи внутри apk, поэтому чтение ассетов обязано идти через этот
// интерфейс, а не через прямой fopen где попало по коду.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pw {

/// Прочитать файл целиком. Пустой результат при ошибке — проверяйте ok.
std::vector<uint8_t> readFile(const std::string& path, bool* ok = nullptr);

/// Прочитать текстовый файл целиком.
std::string readTextFile(const std::string& path, bool* ok = nullptr);

bool writeFile(const std::string& path, const void* data, size_t bytes);

/// Каталог рядом с исполняемым файлом: ассеты, шейдеры.
std::string basePath();

/// Каталог для записи: сохранения, настройки, кэш. На каждой платформе свой.
std::string prefPath(const std::string& app = "PlanetWar");

bool fileExists(const std::string& path);

}  // namespace pw
