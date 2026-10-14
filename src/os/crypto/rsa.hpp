// crypto/rsa.hpp
// ---------------------------------------------------------------------------
// Учебно-прикладной RSA на 16/32-битных целых (SoftRSA).
//
// ОБЛАСТЬ ПРИМЕНИМОСТИ: этот класс работает с модулем, помещающимся в
// uint32_t, то есть n < 2^32. Такой RSA НЕ обеспечивает стойкости (модуль
// разлагается на множители за секунды) и годится только для отладки,
// протоколов с уже скомпрометированной стойкостью и учебных задач.
// Для реальных задач нужен модуль от 2048 бит — это BigInt<64> и отдельная
// реализация (Монтгомери + вероятностная генерация простых), см. модуль ECC
// как образец аккуратной арифметики по модулю.
//
// ЧТО БЫЛО ИСПРАВЛЕНО:
//   1. _n = p * q считалось без проверки переполнения: при p, q > 65535
//      произведение молча «сворачивалось» по модулю 2^32, и ключ становился
//      нерабочим (расшифрование не возвращало исходное сообщение).
//   2. modInverse() не сообщал об отсутствии обратного элемента: при
//      gcd(e, phi) != 1 возвращалось произвольное число, и класс молча
//      выдавал неверные ключи. Теперь инверсия проверяется (e*d ≡ 1 mod phi).
//   3. e = 65537 подставлялось безусловно, даже когда 65537 >= phi
//      (для малых простых это норма), и обнулялось при приведении к Uint.
//   4. Генерация простых зависела от внешнего rng() с % — появился modulo
//      bias; теперь используется Rng::next_below (отбраковка хвоста).
//   5. Отсутствовала возможность задать готовый ключ (n, e)/(n, d):
//      обмен ключами с внешним миром был невозможен.
//   6. Сообщение не проверялось на m < n: результат молча «сворачивался».
// ---------------------------------------------------------------------------
#pragma once

#include "crypto_utils.hpp"
#include "random.hpp"

#include <cstdint>
#include <type_traits>

namespace embed {
namespace crypto {

template <typename Uint = uint32_t>
class Rsa {
    static_assert(std::is_unsigned<Uint>::value,
                  "Rsa: нужен беззнаковый целый тип");
    static_assert(sizeof(Uint) <= 4,
                  "Rsa: без 128-битных произведений поддерживается Uint до 32 бит");

public:
    using WideUint = uint64_t;

    Rsa() noexcept : n_(0), e_(0), d_(0) {}

    /// Задать открытый ключ (n, e) — для шифрования/проверки.
    bool set_public_key(Uint n, Uint e) noexcept {
        if (n < 3u || e < 2u || e >= n) {
            return false;
        }
        n_ = n;
        e_ = e;
        d_ = 0;
        return true;
    }

    /// Задать закрытый ключ (n, d) — для расшифрования.
    bool set_private_key(Uint n, Uint d) noexcept {
        if (n < 3u || d < 2u || d >= n) {
            return false;
        }
        n_ = n;
        d_ = d;
        e_ = 0;
        return true;
    }

    /// Задать полную пару (n, e, d).
    bool set_key_pair(Uint n, Uint e, Uint d) noexcept {
        if (!set_public_key(n, e)) {
            return false;
        }
        if (d < 2u || d >= n) {
            n_ = 0;
            e_ = 0;
            return false;
        }
        d_ = d;
        return true;
    }

    /**
     * @brief Сгенерировать ключи: простые p, q из [min_prime, max_prime].
     *
     * @param rng Источник случайности. Обычный xorshift (utilities::os_rand)
     *            НЕ подходит: по нескольким значениям восстанавливается
     *            состояние, а значит и ключ. Используйте crypto::Rng.
     * @return false, если простые не найдены, n не помещается в Uint или
     *         не удалось подобрать пару (e, d).
     */
    bool generate_keys(Uint min_prime, Uint max_prime, Rng& rng) noexcept {
        if (min_prime < 3u || max_prime < min_prime || !rng.ready()) {
            return false;
        }

        Uint p = 0u;
        Uint q = 0u;

        if (!find_prime(min_prime, max_prime, rng, p)) return false;
        // q должно отличаться от p, иначе n = p^2 разлагается мгновенно.
        for (int attempt = 0; attempt < 32; ++attempt) {
            Uint candidate = 0u;
            if (!find_prime(min_prime, max_prime, rng, candidate)) break;
            if (candidate != p) {
                q = candidate;
                break;
            }
        }
        if (q == 0u) return false;

        const WideUint n_wide = static_cast<WideUint>(p) * static_cast<WideUint>(q);
        if (n_wide > static_cast<WideUint>(~static_cast<Uint>(0))) {
            return false;   // переполнение модуля: ключ был бы нерабочим
        }
        const WideUint phi = static_cast<WideUint>(p - 1u) *
                             static_cast<WideUint>(q - 1u);
        if (phi < 4u) {
            return false;
        }

        WideUint e = 0u;
        if (!choose_exponent(phi, rng, e)) {
            return false;
        }

        const WideUint d = mod_inverse(e, phi);
        if (d == 0u) {
            return false;   // обратного элемента нет (не должно случиться)
        }

        n_ = static_cast<Uint>(n_wide);
        e_ = static_cast<Uint>(e);
        d_ = static_cast<Uint>(d);
        return true;
    }

    /// Зашифровать сообщение (m < n).
    bool encrypt(Uint message, Uint* output) const noexcept {
        if (output == nullptr || n_ == 0u || e_ == 0u) {
            return false;
        }
        if (message >= n_) {
            return false;   // иначе результат «свернётся» и не восстановится
        }
        *output = power_mod(message, e_, n_);
        return true;
    }

    /// Расшифровать сообщение.
    bool decrypt(Uint ciphertext, Uint* output) const noexcept {
        if (output == nullptr || n_ == 0u || d_ == 0u) {
            return false;
        }
        if (ciphertext >= n_) {
            return false;
        }
        *output = power_mod(ciphertext, d_, n_);
        return true;
    }

    bool has_public_key()  const noexcept { return n_ != 0u && e_ != 0u; }
    bool has_private_key() const noexcept { return n_ != 0u && d_ != 0u; }

    Uint modulus()          const noexcept { return n_; }
    Uint public_exponent()  const noexcept { return e_; }
    Uint private_exponent() const noexcept { return d_; }

    /// Затереть ключевой материал.
    void clear() noexcept {
        wipe_object(n_);
        wipe_object(e_);
        wipe_object(d_);
    }

    ~Rsa() noexcept { clear(); }

    Rsa(const Rsa&)            = delete;
    Rsa& operator=(const Rsa&) = delete;

private:
    Uint n_;
    Uint e_;
    Uint d_;

    static constexpr WideUint gcd(WideUint a, WideUint b) noexcept {
        while (b != 0u) {
            const WideUint t = b;
            b = a % b;
            a = t;
        }
        return a;
    }

    /// Модульное возведение в степень (square-and-multiply).
    static constexpr Uint power_mod(Uint base, Uint exp, Uint mod) noexcept {
        if (mod == 1u) {
            return 0u;
        }
        WideUint result = 1u;
        WideUint b = static_cast<WideUint>(base) % mod;
        WideUint e = exp;
        while (e > 0u) {
            if ((e & 1u) != 0u) {
                result = (result * b) % mod;
            }
            b = (b * b) % mod;
            e >>= 1u;
        }
        return static_cast<Uint>(result);
    }

    /// Обратный элемент по модулю (расширенный алгоритм Евклида).
    /// @return 0, если обратного элемента не существует (gcd(a, m) != 1).
    static constexpr WideUint mod_inverse(WideUint a_in, WideUint m) noexcept {
        if (m <= 1u) {
            return 0u;
        }
        const int64_t m0 = static_cast<int64_t>(m);   // m < 2^32: int64 хватает
        int64_t r0 = m0;
        int64_t r1 = static_cast<int64_t>(a_in);
        int64_t t0 = 0;
        int64_t t1 = 1;

        while (r1 != 0) {
            const int64_t q  = r0 / r1;
            const int64_t r2 = r0 - q * r1;
            r0 = r1;
            r1 = r2;
            const int64_t t2 = t0 - q * t1;
            t0 = t1;
            t1 = t2;
        }

        if (r0 != 1) {
            return 0u;                     // gcd(a, m) != 1
        }
        if (t0 < 0) {
            t0 += m0;
        }
        return static_cast<WideUint>(t0);
    }

    /// Вероятностная проверка простоты (Миллер–Рабин, k раундов).
    static bool is_prime(WideUint n, int rounds, Rng& rng) noexcept {
        if (n < 2u) return false;
        if (n == 2u || n == 3u) return true;
        if ((n & 1u) == 0u) return false;

        WideUint d = n - 1u;
        int s = 0;
        while ((d & 1u) == 0u) {
            d >>= 1u;
            ++s;
        }

        for (int i = 0; i < rounds; ++i) {
            // Свидетель из [2, n-2].
            uint32_t witness = 0u;
            if (!rng.next_range(2u, static_cast<uint32_t>(n - 2u), &witness)) {
                return false;
            }
            WideUint x = 1u;
            {
                WideUint base = witness % n;
                WideUint exp  = d;
                while (exp > 0u) {
                    if ((exp & 1u) != 0u) x = (x * base) % n;
                    base = (base * base) % n;
                    exp >>= 1u;
                }
            }
            if (x == 1u || x == n - 1u) {
                continue;
            }
            bool composite = true;
            for (int r = 1; r < s; ++r) {
                x = (x * x) % n;
                if (x == n - 1u) {
                    composite = false;
                    break;
                }
                if (x == 1u) {
                    return false;   // нетривиальный корень из 1 — составное
                }
            }
            if (composite) {
                return false;
            }
        }
        return true;
    }

    /// Найти простое в [min_val, max_val].
    static bool find_prime(Uint min_val, Uint max_val, Rng& rng, Uint& out) noexcept {
        if (min_val > max_val) {
            return false;
        }

        for (int attempt = 0; attempt < 128; ++attempt) {
            uint32_t picked = 0u;
            if (!rng.next_range(static_cast<uint32_t>(min_val),
                                static_cast<uint32_t>(max_val), &picked)) {
                return false;
            }
            WideUint candidate = picked;

            // Чётные (кроме 2) не бывают простыми — берём следующее нечётное
            // в диапазоне.
            if (candidate > 2u && (candidate & 1u) == 0u) {
                candidate += 1u;
                if (candidate > max_val) {
                    candidate = (min_val & 1u) ? min_val : (min_val + 1u);
                }
            }
            if (candidate < min_val || candidate > max_val) {
                continue;
            }
            if (is_prime(candidate, 8, rng)) {
                out = static_cast<Uint>(candidate);
                return true;
            }
        }
        return false;
    }

    /// Подобрать открытую экспоненту: 65537, если она применима, иначе
    /// случайную нечётную, взаимно простую с phi.
    static bool choose_exponent(WideUint phi, Rng& rng, WideUint& out) noexcept {
        constexpr WideUint preferred = 65537u;
        if (preferred < phi && gcd(preferred, phi) == 1u) {
            out = preferred;
            return true;
        }
        if (phi < 4u) {
            return false;
        }
        for (int attempt = 0; attempt < 64; ++attempt) {
            uint32_t candidate = 0u;
            if (!rng.next_range(3u, static_cast<uint32_t>(phi - 1u), &candidate)) {
                return false;
            }
            if ((candidate & 1u) == 0u) {
                ++candidate;
            }
            if (static_cast<WideUint>(candidate) >= phi) {
                candidate = static_cast<uint32_t>((phi - 1u) | 1u);
            }
            if (candidate >= 3u && gcd(candidate, phi) == 1u) {
                out = candidate;
                return true;
            }
        }
        return false;
    }
};

} // namespace crypto
} // namespace embed
