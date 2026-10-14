// crypto/ecc.hpp
// ---------------------------------------------------------------------------
// ECDH на кривых secp256k1 и secp256r1 (P-256).
//
// ЧТО БЫЛО ИСПРАВЛЕНО:
//   1. Модуль не компилировался вообще. Он обращался к несуществующим членам
//      BigInt (IsZero(), GetBit(), поле .data вместо .d) и к несуществующей
//      функции utilities::rand(); шифрование/расшифрование были объявлены с
//      параметрами-значениями там, где нужны ссылки на результат.
//   2. Константы кривых. У secp256k1 координата Gy была выдумана (и одно из
//      «слов» 0xA68554199 не влезало в 32 бита — это ошибка компиляции),
//      у secp256r1 было испорчено слово в коэффициенте B. Публичные ключи
//      не соответствовали ни одной стандартной кривой.
//   3. Арифметика по модулю была неверной по построению: BigInt<8> хранит
//      256 бит, а произведение двух 256-битных чисел требует 512 — mul_abs
//      возвращала Overflow, и FieldMul молча давала 0. Сложение тоже могло
//      переполниться (a + b до 2^257). Теперь произведение считается
//      умножением Монтгомери (CIOS) на 32-битных словах, без 512-битных
//      промежуточных значений и без деления.
//   4. Проверка точки на кривой пропускала точку бесконечности, а публичный
//      ключ вообще не проверялся перед использованием в ECDH (атака
//      подстановкой точки малого порядка).
//   5. «Скалярное умножение» возвращало результат в проективных
//      координатах как аффинный (Z не сокращался).
//   6. Общий секрет выдавался как последовательность слов хоста — порядок
//      байт зависел от архитектуры. Теперь это x-координата big-endian,
//      детерминированная и одинаковая на любом МКУ.
// ---------------------------------------------------------------------------
#pragma once

#include "../big_math.hpp"
#include "crypto_utils.hpp"
#include "random.hpp"

#include <cstddef>
#include <cstdint>

namespace embed {
namespace crypto {

enum class EccCurve {
    Secp256k1,
    Secp256r1
};

/// Размер скаляра/координаты в байтах (обе кривые — 256-битные).
constexpr Size EccScalarBytes = 32u;
constexpr Size EccWords       = 8u;

namespace detail {

/// Параметры кривой: все большие числа — массивы 32-битных слов,
/// МЛАДШЕЕ слово первым (little-endian), как в арифметике Монтгомери.
struct CurveParams {
    uint32_t p[8];      // модуль поля
    uint32_t n[8];      // порядок группы точек
    uint32_t a[8];      // коэффициент a (обычная, НЕ монтгомери-форма)
    uint32_t b[8];      // коэффициент b
    uint32_t gx[8];     // x базовой точки
    uint32_t gy[8];     // y базовой точки
    uint32_t r2[8];     // 2^512 mod p — для перехода в форму Монтгомери
};

} // namespace detail

template <EccCurve Curve>
struct EccCurveTraits;

/// secp256k1 (SEC 2): y² = x³ + 7.
template <>
struct EccCurveTraits<EccCurve::Secp256k1> {
    static constexpr detail::CurveParams params = {
        {   // p
            0xFFFFFC2Fu, 0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFFu,
            0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu
        },
        {   // n
            0xD0364141u, 0xBFD25E8Cu, 0xAF48A03Bu, 0xBAAEDCE6u,
            0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu
        },
        {   // a = 0
            0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
        },
        {   // b = 7
            0x00000007u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
        },
        {   // Gx
            0x16F81798u, 0x59F2815Bu, 0x2DCE28D9u, 0x029BFCDBu,
            0xCE870B07u, 0x55A06295u, 0xF9DCBBACu, 0x79BE667Eu
        },
        {   // Gy
            0xFB10D4B8u, 0x9C47D08Fu, 0xA6855419u, 0xFD17B448u,
            0x0E1108A8u, 0x5DA4FBFCu, 0x26A3C465u, 0x483ADA77u
        },
        {   // R² mod p
            0x000E90A1u, 0x000007A2u, 0x00000001u, 0u,
            0u, 0u, 0u, 0u
        }
    };
};

/// secp256r1 / NIST P-256: y² = x³ - 3x + b.
template <>
struct EccCurveTraits<EccCurve::Secp256r1> {
    static constexpr detail::CurveParams params = {
        {   // p
            0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u,
            0x00000000u, 0x00000000u, 0x00000001u, 0xFFFFFFFFu
        },
        {   // n
            0xFC632551u, 0xF3B9CAC2u, 0xA7179E84u, 0xBCE6FAADu,
            0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu
        },
        {   // a = -3 mod p
            0xFFFFFFFCu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u,
            0x00000000u, 0x00000000u, 0x00000001u, 0xFFFFFFFFu
        },
        {   // b
            0x27D2604Bu, 0x3BCE3C3Eu, 0xCC53B0F6u, 0x651D06B0u,
            0x769886BCu, 0xB3EBBD55u, 0xAA3A93E7u, 0x5AC635D8u
        },
        {   // Gx
            0xD898C296u, 0xF4A13945u, 0x2DEB33A0u, 0x77037D81u,
            0x63A440F2u, 0xF8BCE6E5u, 0xE12C4247u, 0x6B17D1F2u
        },
        {   // Gy
            0x37BF51F5u, 0xCBB64068u, 0x6B315ECEu, 0x2BCE3357u,
            0x7C0F9E16u, 0x8EE7EB4Au, 0xFE1A7F9Bu, 0x4FE342E2u
        },
        {   // R² mod p
            0x00000003u, 0x00000000u, 0xFFFFFFFFu, 0xFFFFFFFBu,
            0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFDu, 0x00000004u
        }
    };
};

/// Аффинная точка кривой.
struct EccPoint {
    BigInt<EccWords> x;
    BigInt<EccWords> y;
    bool infinity;

    constexpr EccPoint() noexcept : x(), y(), infinity(true) {}
    constexpr EccPoint(const BigInt<EccWords>& x_val,
                       const BigInt<EccWords>& y_val) noexcept
        : x(x_val), y(y_val), infinity(false) {}

    static constexpr EccPoint infinity_point() noexcept { return EccPoint(); }

    constexpr bool operator==(const EccPoint& other) const noexcept {
        if (infinity || other.infinity) {
            return infinity == other.infinity;
        }
        return x == other.x && y == other.y;
    }
    constexpr bool operator!=(const EccPoint& other) const noexcept {
        return !(*this == other);
    }
};

namespace detail {

/**
 * @brief Арифметика поля GF(p) и точки в проективных координатах.
 *
 * Все элементы поля — 8 слов по 32 бита в ФОРМЕ МОНТГОМЕРИ (a·R mod p,
 * R = 2^256). Умножение — CIOS-Монтгомери: 64 умножения 32×32 с
 * накоплением в uint64_t, без 512-битных промежуточных значений, без
 * деления вообще. Это в разы дешевле школьного умножения с последующим
 * делением «в столбик» и одинаково работает на любом МКУ.
 */
template <EccCurve Curve>
class EccField {
public:
    static constexpr const CurveParams& k = EccCurveTraits<Curve>::params;
    static constexpr Size Words = 8u;

    /// -p⁻¹ mod 2^32 (метод Ньютона для обратного по модулю 2^k).
    static constexpr uint32_t n0inv() noexcept {
        uint32_t inv = 1u;
        for (int i = 0; i < 5; ++i) {
            inv = inv * (2u - k.p[0] * inv);
        }
        return static_cast<uint32_t>(0u - inv);
    }

    // ───────────────────────── Базовые операции над словами ─────────────────

    static void zero(uint32_t r[Words]) noexcept {
        for (Size i = 0; i < Words; ++i) r[i] = 0u;
    }

    static void copy(uint32_t r[Words], const uint32_t a[Words]) noexcept {
        for (Size i = 0; i < Words; ++i) r[i] = a[i];
    }

    static bool is_zero(const uint32_t a[Words]) noexcept {
        uint32_t acc = 0u;
        for (Size i = 0; i < Words; ++i) acc |= a[i];
        return acc == 0u;
    }

    /// Сравнение с модулем p: <0, 0, >0.
    static int cmp_p(const uint32_t a[Words]) noexcept {
        for (Size i = Words; i > 0u; --i) {
            if (a[i - 1u] != k.p[i - 1u]) {
                return (a[i - 1u] > k.p[i - 1u]) ? 1 : -1;
            }
        }
        return 0;
    }

    /// Сравнение произвольных значений (по модулю не ограничено).
    static int cmp(const uint32_t a[Words], const uint32_t b[Words]) noexcept {
        for (Size i = Words; i > 0u; --i) {
            if (a[i - 1u] != b[i - 1u]) {
                return (a[i - 1u] > b[i - 1u]) ? 1 : -1;
            }
        }
        return 0;
    }

    // ─────────────────────────── Сложение/вычитание ─────────────────────────

    /// r = (a + b) mod p. a, b < p ⇒ a + b < 2p < 2^257 (нужно 9-е слово).
    static void add(uint32_t r[Words], const uint32_t a[Words],
                    const uint32_t b[Words]) noexcept {
        uint32_t t[Words + 1u];
        uint64_t carry = 0u;
        for (Size i = 0; i < Words; ++i) {
            const uint64_t s = static_cast<uint64_t>(a[i]) + b[i] + carry;
            t[i] = static_cast<uint32_t>(s);
            carry = s >> 32;
        }
        t[Words] = static_cast<uint32_t>(carry);

        if (t[Words] != 0u || cmp_p(t) >= 0) {
            subtract_p(t);          // t >= p ⇒ вычитаем p один раз
        }
        copy(r, t);
    }

    /// r = (a - b) mod p.
    static void sub(uint32_t r[Words], const uint32_t a[Words],
                    const uint32_t b[Words]) noexcept {
        if (cmp(a, b) >= 0) {
            // a >= b: обычное вычитание с заёмом.
            uint32_t t[Words + 1u] = {};
            uint64_t borrow = 0u;
            for (Size i = 0; i < Words; ++i) {
                const uint64_t d = static_cast<uint64_t>(a[i]) - b[i] - borrow;
                t[i] = static_cast<uint32_t>(d);
                borrow = (d >> 32) & 1u;
            }
            copy(r, t);
            return;
        }
        // a < b: (a + p) - b. Сначала складываем, затем вычитаем — так
        // переносы и заёмы не смешиваются в одном выражении.
        uint32_t t[Words + 1u];
        uint64_t carry = 0u;
        for (Size i = 0; i < Words; ++i) {
            const uint64_t s = static_cast<uint64_t>(a[i]) + k.p[i] + carry;
            t[i] = static_cast<uint32_t>(s);
            carry = s >> 32;
        }
        t[Words] = static_cast<uint32_t>(carry);

        uint64_t borrow = 0u;
        for (Size i = 0; i < Words; ++i) {
            const uint64_t d = static_cast<uint64_t>(t[i]) - b[i] - borrow;
            t[i] = static_cast<uint32_t>(d);
            borrow = (d >> 32) & 1u;
        }
        // Результат (a - b + p) < p, поэтому заём «съедает» перенос.
        t[Words] = static_cast<uint32_t>(t[Words] - static_cast<uint32_t>(borrow));
        copy(r, t);
    }

    // ──────────────────────────── Умножение (CIOS) ──────────────────────────

    /// t -= p (t — массив из Words+1 слова, старшее слово — перенос).
    static void subtract_p(uint32_t t[Words + 1u]) noexcept {
        uint64_t borrow = 0u;
        for (Size i = 0; i < Words; ++i) {
            const uint64_t d = static_cast<uint64_t>(t[i]) - k.p[i] - borrow;
            t[i] = static_cast<uint32_t>(d);
            borrow = (d >> 32) & 1u;
        }
        t[Words] = static_cast<uint32_t>(t[Words] - static_cast<uint32_t>(borrow));
    }

    /// r = a·b·R⁻¹ mod p (умножение Монтгомери).
    static void mont_mul(uint32_t r[Words], const uint32_t a[Words],
                         const uint32_t b[Words]) noexcept {
        constexpr uint32_t n0 = n0inv();
        uint32_t t[Words + 2u];
        for (Size i = 0; i < Words + 2u; ++i) t[i] = 0u;

        for (Size i = 0; i < Words; ++i) {
            // t += a[i] · b
            uint64_t carry = 0u;
            for (Size j = 0; j < Words; ++j) {
                const uint64_t s = static_cast<uint64_t>(t[j]) +
                                   static_cast<uint64_t>(a[i]) * b[j] + carry;
                t[j] = static_cast<uint32_t>(s);
                carry = s >> 32;
            }
            uint64_t s = static_cast<uint64_t>(t[Words]) + carry;
            t[Words] = static_cast<uint32_t>(s);
            t[Words + 1u] = static_cast<uint32_t>(s >> 32);

            // Приведение: делаем t[0] нулевым, добавляя m·p и сдвигая на слово.
            const uint32_t m = static_cast<uint32_t>(
                static_cast<uint64_t>(t[0]) * n0);

            carry = 0u;
            for (Size j = 0; j < Words; ++j) {
                const uint64_t s2 = static_cast<uint64_t>(t[j]) +
                                    static_cast<uint64_t>(m) * k.p[j] + carry;
                if (j > 0u) {
                    t[j - 1u] = static_cast<uint32_t>(s2);
                }
                carry = s2 >> 32;
            }
            s = static_cast<uint64_t>(t[Words]) + carry;
            t[Words - 1u] = static_cast<uint32_t>(s);
            t[Words] = t[Words + 1u] + static_cast<uint32_t>(s >> 32);
            t[Words + 1u] = 0u;
        }

        // Результат < 2p: вычитаем p, пока не станет меньше.
        while (t[Words] != 0u || cmp_p(t) >= 0) {
            subtract_p(t);
        }
        copy(r, t);
    }

    /// r = a·b mod p (оба аргумента — в форме Монтгомери).
    static void mul(uint32_t r[Words], const uint32_t a[Words],
                    const uint32_t b[Words]) noexcept {
        mont_mul(r, a, b);
    }

    static void sqr(uint32_t r[Words], const uint32_t a[Words]) noexcept {
        mont_mul(r, a, a);
    }

    /// Переход в форму Монтгомери: r = a·R mod p.
    static void to_mont(uint32_t r[Words], const uint32_t a[Words]) noexcept {
        mont_mul(r, a, k.r2);
    }

    /// Обратный переход: r = a·R⁻¹ mod p.
    static void from_mont(uint32_t r[Words], const uint32_t a[Words]) noexcept {
        uint32_t one[Words];
        zero(one);
        one[0] = 1u;
        mont_mul(r, a, one);
    }

    /// Элемент поля, равный небольшому целому (в форме Монтгомери).
    static void from_small(uint32_t r[Words], uint32_t value) noexcept {
        uint32_t t[Words];
        zero(t);
        t[0] = value;
        to_mont(r, t);
    }

    /// Обратный элемент: a^(p-2) (p — простое, малая теорема Ферма).
    static void inverse(uint32_t r[Words], const uint32_t a[Words]) noexcept {
        // Показатель p-2: у обеих кривых младшее слово p не меньше 2,
        // поэтому заёма при вычитании двойки не возникает.
        uint32_t exp[Words];
        copy(exp, k.p);
        exp[0] -= 2u;

        uint32_t result[Words];
        from_small(result, 1u);

        for (Size i = Words * 32u; i > 0u; --i) {
            sqr(result, result);
            const Size bit = i - 1u;
            if (((exp[bit / 32u] >> (bit % 32u)) & 1u) != 0u) {
                mont_mul(result, result, a);
            }
        }
        copy(r, result);
    }

    // ─────────────────────────── Байты ↔ слова ──────────────────────────────

    /// 32 байта big-endian → слова (младшее первым).
    static void from_bytes_be(uint32_t r[Words], const uint8_t in[32]) noexcept {
        for (Size i = 0; i < Words; ++i) {
            const uint8_t* p = in + (Words - 1u - i) * 4u;
            r[i] = load_be32(p);
        }
    }

    /// Слова → 32 байта big-endian.
    static void to_bytes_be(uint8_t out[32], const uint32_t a[Words]) noexcept {
        for (Size i = 0; i < Words; ++i) {
            store_be32(a[Words - 1u - i], out + i * 4u);
        }
    }

    /// Сравнение с порядком группы n.
    static int cmp_n(const uint32_t a[Words]) noexcept {
        for (Size i = Words; i > 0u; --i) {
            if (a[i - 1u] != k.n[i - 1u]) {
                return (a[i - 1u] > k.n[i - 1u]) ? 1 : -1;
            }
        }
        return 0;
    }

    // ──────────────────── Точки в проективных координатах ───────────────────

    struct JacPoint {
        uint32_t X[Words];
        uint32_t Y[Words];
        uint32_t Z[Words];   // Z == 0 ⇒ точка на бесконечности
    };

    static void jac_set_infinity(JacPoint& p) noexcept {
        zero(p.X);
        from_small(p.Y, 1u);   // ненулевое Y у бесконечности удобнее для отладки
        zero(p.Z);
    }

    static bool jac_is_infinity(const JacPoint& p) noexcept {
        return is_zero(p.Z);
    }

    static void jac_from_affine(JacPoint& r, const uint32_t x[Words],
                                const uint32_t y[Words]) noexcept {
        copy(r.X, x);
        copy(r.Y, y);
        from_small(r.Z, 1u);
    }

    /// Удвоение точки (формулы dbl-2007-bl, годятся для любого a).
    static void jac_double(JacPoint& r, const JacPoint& p) noexcept {
        if (jac_is_infinity(p) || is_zero(p.Y)) {
            jac_set_infinity(r);
            return;
        }

        uint32_t y2[Words], s[Words], z2[Words], z4[Words];
        uint32_t m[Words], t[Words];

        sqr(y2, p.Y);                       // Y²
        mul(s, p.X, y2);                    // X·Y²
        add(s, s, s);                       // 2·X·Y²
        add(s, s, s);                       // 4·X·Y²  (S)

        sqr(z2, p.Z);
        sqr(z4, z2);                        // Z⁴

        sqr(m, p.X);                        // X²
        add(t, m, m);                       // 2X²
        add(m, t, m);                       // 3X²

        uint32_t a_mont[Words];
        to_mont(a_mont, k.a);
        mul(t, a_mont, z4);                 // a·Z⁴
        add(m, m, t);                       // M = 3X² + a·Z⁴

        uint32_t x3[Words];
        sqr(x3, m);
        sub(x3, x3, s);
        sub(x3, x3, s);                     // X' = M² - 2S

        uint32_t y4[Words], y3[Words];
        sqr(y4, y2);                        // Y⁴
        sub(t, s, x3);
        mul(y3, m, t);                      // M·(S - X')

        add(t, y4, y4);
        add(t, t, t);
        add(t, t, t);                       // 8Y⁴
        sub(y3, y3, t);                     // Y' = M(S-X') - 8Y⁴

        uint32_t z3[Words];
        mul(z3, p.Y, p.Z);
        add(z3, z3, z3);                    // Z' = 2YZ

        copy(r.X, x3);
        copy(r.Y, y3);
        copy(r.Z, z3);
    }

    /// Сложение точек (формулы add-2007-bl).
    static void jac_add(JacPoint& r, const JacPoint& p, const JacPoint& q) noexcept {
        if (jac_is_infinity(p)) { r = q; return; }
        if (jac_is_infinity(q)) { r = p; return; }

        uint32_t z1z1[Words], z2z2[Words];
        sqr(z1z1, p.Z);
        sqr(z2z2, q.Z);

        uint32_t u1[Words], u2[Words];
        mul(u1, p.X, z2z2);
        mul(u2, q.X, z1z1);

        uint32_t s1[Words], s2[Words];
        mul(s1, p.Y, q.Z);
        mul(s1, s1, z2z2);                  // Y1·Z2³
        mul(s2, q.Y, p.Z);
        mul(s2, s2, z1z1);                  // Y2·Z1³

        uint32_t h[Words], rr[Words];
        sub(h, u2, u1);
        sub(rr, s2, s1);

        if (is_zero(h)) {
            if (is_zero(rr)) {
                jac_double(r, p);           // P == Q
            } else {
                jac_set_infinity(r);        // P == -Q
            }
            return;
        }

        uint32_t hh[Words], hhh[Words], v[Words];
        sqr(hh, h);
        mul(hhh, hh, h);
        mul(v, u1, hh);

        uint32_t x3[Words], y3[Words], z3[Words], t[Words];
        sqr(x3, rr);
        sub(x3, x3, hhh);
        sub(x3, x3, v);
        sub(x3, x3, v);                     // X' = R² - H³ - 2V

        sub(t, v, x3);
        mul(y3, rr, t);
        mul(t, s1, hhh);
        sub(y3, y3, t);                     // Y' = R(V - X') - S1·H³

        mul(z3, p.Z, q.Z);
        mul(z3, z3, h);                     // Z' = Z1·Z2·H

        copy(r.X, x3);
        copy(r.Y, y3);
        copy(r.Z, z3);
    }

    /// Перевод в аффинные координаты (одна инверсия).
    static void jac_to_affine(uint32_t x[Words], uint32_t y[Words],
                              bool& infinity, const JacPoint& p) noexcept {
        if (jac_is_infinity(p)) {
            zero(x);
            zero(y);
            infinity = true;
            return;
        }
        uint32_t zinv[Words], zinv2[Words], zinv3[Words];
        inverse(zinv, p.Z);
        sqr(zinv2, zinv);
        mul(zinv3, zinv2, zinv);
        mul(x, p.X, zinv2);
        mul(y, p.Y, zinv3);
        infinity = false;
    }

    /**
     * @brief Скалярное умножение: r = k·P (лестница Монтгомери).
     *
     * Лестница выполняет одинаковый набор операций для бита 0 и 1 —
     * это не защита от мощного анализа, но убирает зависимость «время ∝
     * число единичных бит» из наивного double-and-add.
     */
    static void scalar_mul(JacPoint& r, const uint32_t k[Words],
                           const JacPoint& p) noexcept {
        JacPoint r0, r1;
        jac_set_infinity(r0);
        r1 = p;

        for (Size i = Words * 32u; i > 0u; --i) {
            const Size bit = i - 1u;
            const bool set = ((k[bit / 32u] >> (bit % 32u)) & 1u) != 0u;

            if (set) {
                JacPoint sum;
                jac_add(sum, r0, r1);
                jac_double(r1, r1);
                r0 = sum;
            } else {
                JacPoint sum;
                jac_add(sum, r0, r1);
                jac_double(r0, r0);
                r1 = sum;
            }
        }
        r = r0;
    }
};

} // namespace detail

/**
 * @brief ECDH (обмен ключами по Диффи—Хеллману на эллиптических кривых).
 *
 * @code
 *   using D = embed::crypto::Ecdh<embed::crypto::EccCurve::Secp256r1>;
 *   D::Scalar priv; D::Point pub;
 *   D::generate_key_pair(rng, priv, pub);          // своя пара
 *   uint8_t secret[32];
 *   D::derive_shared_secret_bytes(priv, peer_pub, secret);
 * @endcode
 *
 * Проверки: приватный скаляр 0 < d < n; публичный ключ лежит на кривой и не
 * является точкой бесконечности (у обеих кривых кофактор 1, поэтому этого
 * достаточно для защиты от подстановки точки малого порядка).
 */
template <EccCurve Curve>
class Ecdh {
    using Field = detail::EccField<Curve>;
    using Jac   = typename detail::EccField<Curve>::JacPoint;

    static_assert(EccCurveTraits<Curve>::params.p[0] != 0u,
                  "Ecdh: параметры кривой не заданы");

public:
    using Scalar = BigInt<EccWords>;
    using Point  = EccPoint;

    static constexpr Size ScalarBytes       = EccScalarBytes;
    static constexpr Size SharedSecretBytes = EccScalarBytes;

    /// Базовая точка кривой G.
    static Point generator() noexcept {
        return Point(bigint_from_words(EccCurveTraits<Curve>::params.gx),
                     bigint_from_words(EccCurveTraits<Curve>::params.gy));
    }

    /// 0 < d < n.
    static bool is_valid_private_key(const Scalar& priv) noexcept {
        if (priv.neg) {
            return false;
        }
        uint32_t words[EccWords];
        bigint_to_words(priv, words);
        if (Field::is_zero(words)) {
            return false;
        }
        return Field::cmp_n(words) < 0;
    }

    /// Точка лежит на кривой (и не бесконечность).
    static bool is_on_curve(const Point& point) noexcept {
        if (point.infinity) {
            return false;
        }
        uint32_t x[EccWords], y[EccWords];
        bigint_to_words(point.x, x);
        bigint_to_words(point.y, y);

        // Координаты обязаны быть приведены по модулю p.
        if (Field::cmp_p(x) >= 0 || Field::cmp_p(y) >= 0) {
            return false;
        }

        uint32_t xm[EccWords], ym[EccWords];
        Field::to_mont(xm, x);
        Field::to_mont(ym, y);

        uint32_t lhs[EccWords], x2[EccWords], x3[EccWords];
        uint32_t a_mont[EccWords], t[EccWords], b_mont[EccWords];
        uint32_t rhs[EccWords];

        Field::sqr(lhs, ym);                        // y²
        Field::sqr(x2, xm);
        Field::mul(x3, x2, xm);                     // x³
        Field::to_mont(a_mont, EccCurveTraits<Curve>::params.a);
        Field::mul(t, a_mont, xm);                  // a·x
        Field::add(rhs, x3, t);
        Field::to_mont(b_mont, EccCurveTraits<Curve>::params.b);
        Field::add(rhs, rhs, b_mont);               // x³ + ax + b

        return Field::cmp(lhs, rhs) == 0;
    }

    /// Публичный ключ: pub = d·G.
    static bool compute_public_key(const Scalar& priv, Point& pub) noexcept {
        if (!is_valid_private_key(priv)) {
            return false;
        }
        uint32_t k[EccWords];
        bigint_to_words(priv, k);

        uint32_t gxm[EccWords], gym[EccWords];
        Field::to_mont(gxm, EccCurveTraits<Curve>::params.gx);
        Field::to_mont(gym, EccCurveTraits<Curve>::params.gy);

        Jac base;
        Jac result;
        Field::jac_from_affine(base, gxm, gym);
        Field::scalar_mul(result, k, base);

        uint32_t rx[EccWords], ry[EccWords];
        bool inf = false;
        Field::jac_to_affine(rx, ry, inf, result);
        if (inf) {
            return false;
        }
        Field::from_mont(rx, rx);
        Field::from_mont(ry, ry);

        pub = Point(bigint_from_words(rx), bigint_from_words(ry));
        return true;
    }

    /// Общий секрет: shared = d·Q (Q — публичный ключ партнёра).
    static bool derive_shared_secret(const Scalar& priv, const Point& peer,
                                     Point& shared) noexcept {
        if (!is_valid_private_key(priv)) {
            return false;
        }
        if (!is_on_curve(peer)) {
            return false;
        }

        uint32_t k[EccWords], qx[EccWords], qy[EccWords];
        bigint_to_words(priv, k);
        bigint_to_words(peer.x, qx);
        bigint_to_words(peer.y, qy);

        Jac q;
        Jac result;

        uint32_t qxm[EccWords], qym[EccWords];
        Field::to_mont(qxm, qx);
        Field::to_mont(qym, qy);
        Field::jac_from_affine(q, qxm, qym);

        Field::scalar_mul(result, k, q);

        uint32_t rx[EccWords], ry[EccWords];
        bool inf = false;
        Field::jac_to_affine(rx, ry, inf, result);
        if (inf) {
            return false;
        }
        Field::from_mont(rx, rx);
        Field::from_mont(ry, ry);

        shared = Point(bigint_from_words(rx), bigint_from_words(ry));
        return true;
    }

    /**
     * @brief Общий секрет как 32 байта — это x-координата точки d·Q
     *        в big-endian. Именно такое представление обязано попадать в KDF:
     *        порядок байт не зависит от архитектуры.
     */
    static bool derive_shared_secret_bytes(const Scalar& priv, const Point& peer,
                                           uint8_t out[SharedSecretBytes]) noexcept {
        Point shared;
        if (!derive_shared_secret(priv, peer, shared)) {
            return false;
        }
        uint32_t x[EccWords];
        bigint_to_words(shared.x, x);
        Field::to_bytes_be(out, x);
        return true;
    }

    /// Ключевая пара: скаляр из CSPRNG (отбраковка значений >= n).
    static bool generate_key_pair(Rng& rng, Scalar& priv, Point& pub) noexcept {
        if (!rng.ready()) {
            return false;
        }
        uint8_t buf[ScalarBytes];
        uint32_t words[EccWords];

        for (int attempt = 0; attempt < 128; ++attempt) {
            rng.fill(buf, sizeof(buf));
            Field::from_bytes_be(words, buf);
            if (Field::is_zero(words) || Field::cmp_n(words) >= 0) {
                continue;   // вне [1, n-1] — берём следующее
            }
            priv = bigint_from_words(words);
            return compute_public_key(priv, pub);
        }
        wipe(buf, sizeof(buf));
        return false;
    }

private:
    static void bigint_to_words(const Scalar& v, uint32_t out[EccWords]) noexcept {
        for (Size i = 0; i < EccWords; ++i) {
            out[i] = (i < v.len) ? v.d[i] : 0u;
        }
    }

    static Scalar bigint_from_words(const uint32_t in[EccWords]) noexcept {
        Scalar v;
        for (Size i = 0; i < EccWords; ++i) {
            v.d[i] = in[i];
        }
        v.len = EccWords;
        v.neg = false;
        v.normalize();
        return v;
    }
};

} // namespace crypto
} // namespace embed
