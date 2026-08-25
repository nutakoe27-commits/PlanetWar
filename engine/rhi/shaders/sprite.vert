#version 450

// Спрайты одним вызовом отрисовки.
//
// Квадрат строится в вершинном шейдере по индексу, а всё, что отличает
// один спрайт от другого, приходит поэкземплярными атрибутами. Поэтому
// вся карта — звёзды, флоты, значки — рисуется за один draw, независимо
// от того, сотня их или сто тысяч.
//
// Поворот считается здесь же: корабль на карте смотрит туда, куда летит,
// а держать для каждого угла свой спрайт значило бы раздуть атлас
// в тридцать шесть раз.

layout(push_constant) uniform Camera {
    vec2 center;   // куда смотрит камера, в мировых координатах
    vec2 scale;    // мир -> клип, с учётом зума и соотношения сторон
} camera;

// Поэкземплярные атрибуты.
layout(location = 0) in vec2 iCenter;
layout(location = 1) in vec2 iHalfSize;
layout(location = 2) in vec4 iUvRect;      // u0, v0, u1, v1
layout(location = 3) in vec4 iTint;
layout(location = 4) in float iRotationTurns;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vTint;

// Углы квадрата и их координаты в атласе. Шесть вершин, два треугольника:
// полоса потребовала бы отдельного вызова на спрайт и убила бы весь смысл.
const vec2 kCorners[6] = vec2[](
    vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
    vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0)
);
const vec2 kUv[6] = vec2[](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
);

void main() {
    vec2 corner = kCorners[gl_VertexIndex] * iHalfSize;

    // Угол в оборотах, а не в радианах — как и во всей симуляции.
    float angle = iRotationTurns * 6.28318530718;
    float s = sin(angle);
    float c = cos(angle);
    vec2 rotated = vec2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);

    vec2 world = iCenter + rotated;
    gl_Position = vec4((world - camera.center) * camera.scale, 0.0, 1.0);

    vec2 uv = kUv[gl_VertexIndex];
    vUv = mix(iUvRect.xy, iUvRect.zw, uv);
    vTint = iTint;
}
