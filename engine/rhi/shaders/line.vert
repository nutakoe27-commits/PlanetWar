#version 450

// Гиперлинии и всё остальное, что рисуется отрезками.
//
// Отдельный конвейер, а не спрайты: линия связывает две произвольные
// точки, и растягивать под неё квадрат означало бы считать поворот и
// длину на процессоре для каждого ребра графа. Их тысячи.

layout(push_constant) uniform Camera {
    vec2 center;
    vec2 scale;
} camera;

layout(location = 0) in vec2 iPosition;
layout(location = 1) in vec4 iColor;

layout(location = 0) out vec4 vColor;

void main() {
    gl_Position = vec4((iPosition - camera.center) * camera.scale, 0.0, 1.0);
    vColor = iColor;
}
