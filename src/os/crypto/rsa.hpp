#pragma once
#include "../common.hpp"
#include <type_traits>

namespace embed {

template <typename Uint = uint32_t>
class SoftRSA {
public:
    static_assert(std::is_unsigned_v<Uint>, "SoftRSA requires an unsigned integer type");
    static_assert(sizeof(Uint) <= 4, "For safe modular exponentiation without 128-bit integers, Uint must be 32-bit or smaller.");
    using WideUint = uint64_t;

    SoftRSA(const SoftRSA&) = delete;
    SoftRSA& operator=(const SoftRSA&) = delete;

    SoftRSA() : _n(0), _e(0), _d(0) {}

    template <typename Rng>
    bool GenerateKeys(Uint min_prime_val, Uint max_prime_val, Rng& rng) {
        if (min_prime_val < 3 || max_prime_val < min_prime_val) {
            return false;
        }

        Uint p = 0, q = 0;
        
        // Генерация p с ограничением попыток для предотвращения зависания
        for (int attempts = 0; attempts < 50; ++attempts) {
            p = GeneratePrime(min_prime_val, max_prime_val, rng);
            if (p != 0) break;
        }
        if (p == 0) return false;

        // Генерация q (должно отличаться от p)
        for (int attempts = 0; attempts < 50; ++attempts) {
            q = GeneratePrime(min_prime_val, max_prime_val, rng);
            if (q != 0 && q != p) break;
        }
        if (q == 0 || q == p) return false;

        _n = p * q;
        WideUint phi = static_cast<WideUint>(p - 1) * (q - 1);
        
        _e = 65537; // Стандартная открытая экспонента
        
        // Проверка применимости e = 65537
        if (_e >= phi || GCD(_e, static_cast<Uint>(phi)) != 1) {
            bool e_found = false;
            for (int attempts = 0; attempts < 50; ++attempts) {
                Uint range = (phi > 3) ? static_cast<Uint>(phi - 3) : 1;
                Uint candidate = 3 + (rng() % range);
                
                if ((candidate & 1) == 0) candidate++; // Делаем нечётным
                if (candidate >= phi) candidate = static_cast<Uint>((phi - 1) | 1);
                if (candidate < 3) candidate = 3;

                if (GCD(candidate, static_cast<Uint>(phi)) == 1) {
                    _e = candidate;
                    e_found = true;
                    break;
                }
            }
            if (!e_found) return false;
        }

        _d = ModInverse(_e, static_cast<Uint>(phi));
        if (_d == 0) {
            return false;
        }

        return true;
    }

    bool Encrypt(Uint message, Uint* output) const {
        if (output == nullptr || _n == 0) {
            return false;
        }
        *output = Power(message, _e, _n);
        return true;
    }

    bool Decrypt(Uint encrypted_message, Uint* output) const {
        if (output == nullptr || _n == 0) {
            return false;
        }
        *output = Power(encrypted_message, _d, _n);
        return true;
    }

    void Clear() {
        // Явное затирание ключей из памяти (важно для безопасности в embedded)
        _n = 0;
        _e = 0;
        _d = 0;
    }

    Uint GetN() const { return _n; }
    Uint GetE() const { return _e; }
    Uint GetD() const { return _d; }

private:
    Uint _n; // Модуль
    Uint _e; // Открытая экспонента
    Uint _d; // Закрытая экспонента
    static constexpr Uint GCD(Uint a, Uint b) {
        while (b != 0) {
            Uint temp = b;
            b = a % b;
            a = temp;
        }
        return a;
    }
    static constexpr Uint Power(Uint base, Uint exp, Uint mod) {
        if (mod == 1) return 0;
        Uint res = 1;
        base = base % mod;
        while (exp > 0) {
            if ((exp & 1) == 1) {
                res = static_cast<Uint>((static_cast<WideUint>(res) * base) % mod);
            }
            base = static_cast<Uint>((static_cast<WideUint>(base) * base) % mod);
            exp >>= 1;
        }
        return res;
    }
    static constexpr Uint ModInverse(Uint a, Uint m) {
        if (m == 1) return 0;
        
        int64_t m0 = m;
        int64_t y = 0, x = 1;
        int64_t a_signed = a;

        while (a_signed > 1) {
            int64_t q = a_signed / m0;
            int64_t t = m0;

            m0 = a_signed % m0;
            a_signed = t;
            t = y;

            y = x - q * y;
            x = t;
        }

        if (x < 0) {
            x += m;
        }
        return static_cast<Uint>(x);
    }
    template <typename Rng>
    static bool IsPrime(Uint n, int k, Rng& rng) {
        if (n <= 1 || n == 4) return false;
        if (n <= 3) return true;

        Uint d = n - 1;
        while ((d & 1) == 0) {
            d >>= 1;
        }

        for (int i = 0; i < k; ++i) {
            Uint range = n - 3;
            if (range == 0) return false;
            Uint a = 2 + (rng() % range); 
            Uint x = Power(a, d, n);

            if (x == 1 || x == n - 1) continue;

            Uint current_d = d;
            bool composite = true;
            while (current_d != n - 1) {
                x = static_cast<Uint>((static_cast<WideUint>(x) * x) % n);
                current_d <<= 1;

                if (x == 1) return false;
                if (x == n - 1) {
                    composite = false;
                    break;
                }
            }
            if (composite) return false;
        }
        return true;
    }
    template <typename Rng>
    static Uint GeneratePrime(Uint min_val, Uint max_val, Rng& rng) {
        if (min_val > max_val) return 0;
        
        for (int attempts = 0; attempts < 50; ++attempts) {
            Uint range = max_val - min_val;
            Uint p = min_val + (rng() % (range + 1));
            
            // Оптимизация: пропускаем чётные числа
            if (p > 2 && (p & 1) == 0) {
                p++;
                if (p > max_val) {
                    p = (min_val & 1) ? min_val : min_val + 1;
                }
            }
            
            if (IsPrime(p, 5, rng)) {
                return p;
            }
        }
        return 0; 
    }
};

}