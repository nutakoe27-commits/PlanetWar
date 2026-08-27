#version 450

// Вершинный шейдер сеток.
//
// Матрица модели приезжает ТРЕМЯ ОСЯМИ И ПЕРЕНОСОМ, а не полной 4x4:
// нижняя строка аффинного преобразования всегда (0,0,0,1), и возить её
// через шину для каждой из тысяч построек — чистые потери.

layout(push_constant) uniform Push {
    mat4 viewProjection;
    vec3 lightPosition;
    float ambient;
    vec3 eyePosition;
    float lightPadding;
    vec3 lightColor;
    float reserved;
} push;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;

layout(location = 3) in vec3 axisX;
layout(location = 4) in vec3 axisY;
layout(location = 5) in vec3 axisZ;
layout(location = 6) in vec3 origin;
layout(location = 7) in vec4 tint;
layout(location = 8) in vec4 params;   // самосвечение, контур, блеск, запас

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec4 vTint;
layout(location = 4) out vec4 vParams;

void main() {
    mat3 model = mat3(axisX, axisY, axisZ);
    vec3 world = model * inPosition + origin;

    vWorld = world;
    // Нормаль преобразуется той же матрицей. Это законно ровно потому, что
    // масштаб у нас равномерный: обратная транспонированная отличается
    // от неё только длиной, а её мы всё равно нормируем.
    vNormal = normalize(model * inNormal);
    vUv = inUv;
    vTint = tint;
    vParams = params;

    gl_Position = push.viewProjection * vec4(world, 1.0);
}
