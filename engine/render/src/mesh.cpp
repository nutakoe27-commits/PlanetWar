#include "pw/render/mesh.h"

#include <cmath>
#include <cstring>
#include <fstream>

namespace pw::render {

namespace {

constexpr char kMagic[4] = {'P', 'W', 'M', '1'};

uint32_t readU32(const uint8_t* data) {
    // Явный порядок байтов, а не memcpy структуры: файл коммитится
    // и читается на пяти платформах, и молча зависеть от порядка
    // байтов машины — это ошибка, которая проявится один раз
    // и на чужом железе.
    return uint32_t(data[0]) | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16) |
           (uint32_t(data[3]) << 24);
}

float readFloat(const uint8_t* data) {
    const uint32_t bits = readU32(data);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

}  // namespace

float MeshData::extent() const {
    float largest = 0.0f;
    for (const rhi::MeshVertex& vertex : vertices) {
        largest = std::max(largest, std::abs(vertex.x));
        largest = std::max(largest, std::abs(vertex.y));
        largest = std::max(largest, std::abs(vertex.z));
    }
    return largest;
}

bool loadMesh(const std::string& path, MeshData& out, std::string* error) {
    auto fail = [&](const char* what) {
        if (error != nullptr) *error = std::string(what) + ": " + path;
        return false;
    };

    std::ifstream file(path, std::ios::binary);
    if (!file) return fail("не удалось открыть сетку");

    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    if (bytes.size() < 12) return fail("файл сетки слишком короткий");
    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
        return fail("это не сетка PlanetWar");
    }

    const uint32_t vertexCount = readU32(bytes.data() + 4);
    const uint32_t indexCount = readU32(bytes.data() + 8);
    if (vertexCount == 0 || indexCount == 0 || indexCount % 3 != 0) {
        return fail("в сетке нет треугольников");
    }

    const size_t needed = 12 + size_t(vertexCount) * 32 + size_t(indexCount) * 4;
    if (bytes.size() < needed) return fail("файл сетки обрезан");

    out.vertices.resize(vertexCount);
    const uint8_t* cursor = bytes.data() + 12;
    for (uint32_t i = 0; i < vertexCount; ++i) {
        rhi::MeshVertex& vertex = out.vertices[i];
        vertex.x = readFloat(cursor + 0);
        vertex.y = readFloat(cursor + 4);
        vertex.z = readFloat(cursor + 8);
        vertex.nx = readFloat(cursor + 12);
        vertex.ny = readFloat(cursor + 16);
        vertex.nz = readFloat(cursor + 20);
        vertex.u = readFloat(cursor + 24);
        vertex.v = readFloat(cursor + 28);
        cursor += 32;
    }

    out.indices.resize(indexCount);
    for (uint32_t i = 0; i < indexCount; ++i) {
        const uint32_t index = readU32(cursor);
        // Индекс за границей — это не «немного битый файл», это зависший
        // драйвер: видеокарта на выход за буфер отвечает не исключением.
        if (index >= vertexCount) return fail("индекс сетки за границей массива вершин");
        out.indices[i] = index;
        cursor += 4;
    }
    return true;
}

MeshData makeOrbitRing(uint32_t segments, float thickness) {
    MeshData mesh;
    if (segments < 8) segments = 8;

    const float inner = 1.0f - thickness;
    const float outer = 1.0f + thickness;

    mesh.vertices.reserve(size_t(segments) * 2);
    for (uint32_t step = 0; step < segments; ++step) {
        const float angle = 6.28318530718f * float(step) / float(segments);
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        const float along = float(step) / float(segments);

        rhi::MeshVertex innerVertex{};
        innerVertex.x = inner * cosine;
        innerVertex.y = inner * sine;
        innerVertex.nz = 1.0f;
        innerVertex.u = along;
        innerVertex.v = 0.0f;
        mesh.vertices.push_back(innerVertex);

        rhi::MeshVertex outerVertex = innerVertex;
        outerVertex.x = outer * cosine;
        outerVertex.y = outer * sine;
        outerVertex.v = 1.0f;
        mesh.vertices.push_back(outerVertex);
    }

    // Обход против часовой стрелки: конвейер сеток отсекает задние грани,
    // и намотанное наоборот кольцо просто не появится в кадре.
    mesh.indices.reserve(size_t(segments) * 6);
    for (uint32_t step = 0; step < segments; ++step) {
        const uint32_t a = step * 2;
        const uint32_t b = step * 2 + 1;
        const uint32_t c = (step * 2 + 3) % (segments * 2);
        const uint32_t d = (step * 2 + 2) % (segments * 2);
        mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, d});
    }
    return mesh;
}

}  // namespace pw::render
