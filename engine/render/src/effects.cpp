#include "pw/render/effects.h"

#include <algorithm>
#include <cmath>

namespace pw::render {

namespace {

constexpr float kTwoPi = 6.28318530718f;

/// Сколько отрезков в кольце.
///
/// Тридцать два. Меньше — на общем плане видно многоугольник, и кольцо
/// читается как «шестерёнка»; больше — счёт вершин растёт, а разницы
/// на экране уже нет. Число одно для всех колец: кольцо с другим числом
/// граней выглядит как эффект другого вида, хотя означает то же самое.
constexpr int kRingSegments = 32;

/// Плавное затухание к концу жизни. Не линейное: линейное гаснет
/// равномерно и последнюю треть выглядит грязным следом, а квадратичное
/// уходит быстро и чисто.
float fadeOut(float phase) {
    const float left = std::clamp(1.0f - phase, 0.0f, 1.0f);
    return left * left;
}

void ring(std::vector<rhi::LineVertex>& out, float cx, float cy, float radius,
          float r, float g, float b, float alpha) {
    if (radius <= 0.0f || alpha <= 0.003f) return;
    for (int i = 0; i < kRingSegments; ++i) {
        const float a0 = float(i) / float(kRingSegments) * kTwoPi;
        const float a1 = float(i + 1) / float(kRingSegments) * kTwoPi;
        out.push_back(rhi::LineVertex{cx + std::cos(a0) * radius,
                                      cy + std::sin(a0) * radius, r, g, b, alpha});
        out.push_back(rhi::LineVertex{cx + std::cos(a1) * radius,
                                      cy + std::sin(a1) * radius, r, g, b, alpha});
    }
}

void spoke(std::vector<rhi::LineVertex>& out, float cx, float cy, float angle,
           float from, float to, float r, float g, float b, float alpha) {
    if (alpha <= 0.003f) return;
    const float dx = std::cos(angle), dy = std::sin(angle);
    out.push_back(rhi::LineVertex{cx + dx * from, cy + dy * from, r, g, b, alpha});
    out.push_back(rhi::LineVertex{cx + dx * to, cy + dy * to, r, g, b, alpha * 0.15f});
}

/// Крест: две черты через центр. Спокойный знак «готово», в отличие
/// от лучей сражения.
void cross(std::vector<rhi::LineVertex>& out, float cx, float cy, float size,
           float r, float g, float b, float alpha) {
    if (alpha <= 0.003f) return;
    out.push_back(rhi::LineVertex{cx - size, cy, r, g, b, alpha});
    out.push_back(rhi::LineVertex{cx + size, cy, r, g, b, alpha});
    out.push_back(rhi::LineVertex{cx, cy - size, r, g, b, alpha});
    out.push_back(rhi::LineVertex{cx, cy + size, r, g, b, alpha});
}

/// Дешёвая перемешка для разброса лучей. Тот же приём, что и в мире,
/// но здесь он ни на что не влияет и потому может быть любым.
uint32_t scramble(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    value ^= value >> 16;
    return value;
}

}  // namespace

float effectLifetime(EffectKind kind) {
    switch (kind) {
        // Колонизация и захват — новости, ради которых играют. Полторы
        // секунды: достаточно, чтобы заметить боковым зрением и успеть
        // перевести взгляд.
        case EffectKind::Colonized: return 1.5f;
        case EffectKind::Captured:  return 1.5f;
        // Потеря своей планеты — худшая новость в игре. Держится вдвое
        // дольше именно поэтому.
        case EffectKind::Lost:      return 3.0f;
        // Осада идёт часами, но кольцо тревоги — не индикатор осады,
        // а сообщение о её НАЧАЛЕ. Индикатор рисует карта.
        case EffectKind::Sieged:    return 2.2f;
        case EffectKind::Battle:    return 1.1f;
        // Достроенное здание — тихое событие: игрок его ждал и знает,
        // что оно будет. Ему хватает полсекунды подтверждения.
        case EffectKind::Built:     return 0.7f;
        // Отклик на щелчок. Короче всех: он подтверждает, что приказ
        // принят, а не сообщает новость.
        case EffectKind::Order:     return 0.45f;
        default:                    return 1.0f;
    }
}

void Effects::spawn(EffectKind kind, float worldX, float worldY, float radius,
                    const EmpireColor& color, uint32_t seed) {
    if (kind >= EffectKind::Count) return;

    Effect effect;
    effect.kind = kind;
    effect.x = worldX;
    effect.y = worldY;
    effect.radius = radius > 0.0f ? radius : 1.0f;
    effect.life = effectLifetime(kind);
    effect.color = color;
    effect.seed = scramble(seed + uint32_t(kind) * 2654435761u);
    live_.push_back(effect);

    // Вытесняем СТАРШИЕ, а не отказываем новому: свежее событие важнее
    // позавчерашнего, а отказ означал бы, что при волне кризиса игрок
    // не увидит именно последние — то есть самые нужные — события.
    while (live_.size() > kMaxEffects) live_.erase(live_.begin());
}

void Effects::update(float deltaSeconds) {
    if (deltaSeconds <= 0.0f) return;
    // Шаг НЕ ограничивается здесь, хотя соблазн есть: после свёрнутого
    // окна между кадрами проходит минута, и все эффекты разом умирают.
    //
    // Но «состарить на секунду» обязано означать ровно секунду, иначе
    // это уже не update. Ограничение стоит там, где шаг вычисляется, —
    // в клиенте, один раз и для всех, кто им пользуется. Иначе оно было бы
    // спрятано в трёх местах с тремя разными числами.
    const float step = deltaSeconds;
    for (Effect& effect : live_) effect.age += step;
    live_.erase(std::remove_if(live_.begin(), live_.end(),
                               [](const Effect& e) { return e.dead(); }),
                live_.end());
}

void Effects::build(MapFrame& out) const {
    for (const Effect& effect : live_) {
        const float phase = effect.phase();
        const float fade = fadeOut(phase);
        const float r = effect.color.r, g = effect.color.g, b = effect.color.b;

        switch (effect.kind) {
            case EffectKind::Colonized: {
                // Расходится наружу: империя выросла. Два кольца с
                // задержкой — одно кольцо читается как «что-то мигнуло»,
                // два подряд читаются как направление.
                ring(out.lines, effect.x, effect.y,
                     effect.radius * (1.0f + phase * 3.0f), r, g, b, fade * 0.9f);
                if (phase > 0.25f) {
                    ring(out.lines, effect.x, effect.y,
                         effect.radius * (1.0f + (phase - 0.25f) * 3.0f), r, g, b,
                         fade * 0.45f);
                }
                cross(out.lines, effect.x, effect.y, effect.radius * 0.8f, r, g, b,
                      fade);
                break;
            }
            case EffectKind::Captured:
            case EffectKind::Lost: {
                // Сходится внутрь: пришли извне и сдавили. Направление
                // противоположно колонизации, и это единственное, что
                // отличает две новости на карте без чтения текста.
                const float shrink = 4.0f - phase * 3.0f;
                ring(out.lines, effect.x, effect.y, effect.radius * shrink, r, g, b,
                     fade * 0.9f);
                if (phase > 0.2f) {
                    ring(out.lines, effect.x, effect.y,
                         effect.radius * (4.0f - (phase - 0.2f) * 3.0f), r, g, b,
                         fade * 0.5f);
                }
                break;
            }
            case EffectKind::Sieged: {
                // Пульс на месте: не расходится и не сходится. Осада —
                // это состояние, а не событие, и движение обязано это
                // передавать.
                const float beat = 0.5f + 0.5f * std::sin(phase * kTwoPi * 3.0f);
                ring(out.lines, effect.x, effect.y,
                     effect.radius * (1.6f + beat * 0.9f), r, g, b, fade);
                break;
            }
            case EffectKind::Battle: {
                // Лучи в стороны. Разброс — от сида: два боя подряд
                // в одной системе не должны выглядеть одним и тем же
                // узором, иначе второй теряется в первом.
                const int spokes = 7;
                for (int i = 0; i < spokes; ++i) {
                    const uint32_t mixed = scramble(effect.seed + uint32_t(i) * 977u);
                    const float angle =
                        float(mixed % 3600u) / 3600.0f * kTwoPi;
                    const float length =
                        effect.radius * (1.6f + float((mixed >> 12) % 100u) / 100.0f);
                    spoke(out.lines, effect.x, effect.y, angle,
                          effect.radius * 0.6f + phase * length * 0.5f,
                          effect.radius * 0.6f + phase * length, r, g, b, fade);
                }
                ring(out.lines, effect.x, effect.y, effect.radius * (0.8f + phase * 0.6f),
                     r, g, b, fade * 0.6f);
                break;
            }
            case EffectKind::Built: {
                ring(out.lines, effect.x, effect.y,
                     effect.radius * (0.6f + phase * 1.2f), r, g, b, fade * 0.8f);
                break;
            }
            case EffectKind::Order: {
                // Кольцо сжимается к цели: «приказ принят, идём сюда».
                ring(out.lines, effect.x, effect.y,
                     effect.radius * (3.0f - phase * 2.0f), r, g, b, fade);
                break;
            }
            default:
                break;
        }
    }
}

}  // namespace pw::render
