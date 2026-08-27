#version 450

// Фрагментный шейдер сеток: свет одной звезды, мягкий терминатор,
// контурная подсветка и самосвечение.
//
// Источник света ОДИН и это осознанно: в звёздной системе светит звезда,
// второму источнику взяться неоткуда. Модель освещения, выведенная из
// предмета, а не из общности, экономит и шейдер, и мысли.

layout(push_constant) uniform Push {
    mat4 viewProjection;
    vec3 lightPosition;
    float ambient;
    vec3 eyePosition;
    float lightPadding;
    vec3 lightColor;
    float reserved;
} push;

layout(set = 0, binding = 0) uniform sampler2D albedo;

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) in vec4 vTint;
layout(location = 4) in vec4 vParams;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 texel = texture(albedo, vUv);
    vec3 base = texel.rgb * vTint.rgb;

    float emissive = vParams.x;
    float rimStrength = vParams.y;
    float gloss = vParams.z;

    vec3 normal = normalize(vNormal);
    vec3 toLight = push.lightPosition - vWorld;
    float distance = length(toLight);
    vec3 lightDir = distance > 0.0001 ? toLight / distance : vec3(0.0, 0.0, 1.0);
    vec3 viewDir = normalize(push.eyePosition - vWorld);

    float facing = dot(normal, lightDir);
    // Мягкий терминатор. Резкая граница света и тени на планете читается
    // как трафарет, наклеенный на шар, а не как сам шар: у настоящего
    // тела свет заходит за край из-за рассеяния в атмосфере и на пыли.
    float wrapped = clamp((facing + 0.3) / 1.3, 0.0, 1.0);
    wrapped *= wrapped;

    vec3 halfway = normalize(lightDir + viewDir);
    float specular = pow(max(dot(normal, halfway), 0.0), 48.0) * gloss *
                     max(facing, 0.0);

    // Контур. Им же рисуется атмосфера: у планеты с воздухом край всегда
    // светлее середины, и один этот эффект отличает шар от круга.
    float rim = pow(1.0 - max(dot(normal, viewDir), 0.0), 3.0) * rimStrength;

    vec3 lit = base * (push.ambient + wrapped) * push.lightColor +
               push.lightColor * specular + base * rim * 2.0;

    // Самосвечение выводит тело из-под освещения целиком: звезда светит
    // сама, и падающий на неё свет — это она же.
    vec3 color = mix(lit, base * 1.15, clamp(emissive, 0.0, 1.0));

    outColor = vec4(color, texel.a * vTint.a);
}
