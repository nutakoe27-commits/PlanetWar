#version 450

// Атлас плюс тон.
//
// Тон умножается, а не подмешивается: спрайты кораблей испечены серыми
// (см. docs/07-ASSET-PIPELINE.md), и цвет империи берётся отсюда. Один
// набор моделей на всех игроков вместо копии атласа под каждый цвет.

layout(set = 0, binding = 0) uniform sampler2D uAtlas;

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vTint;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 texel = texture(uAtlas, vUv);
    outColor = texel * vTint;
    // Полностью прозрачное отбрасываем: иначе прозрачные края спрайтов
    // писали бы в буфер глубины и резали то, что за ними.
    if (outColor.a < 0.004) discard;
}
