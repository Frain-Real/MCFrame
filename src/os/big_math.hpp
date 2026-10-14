#pragma once
#include "common.hpp"
#include <stdint.h>
#include <stddef.h>
#include <type_traits>

namespace embed {

template <size_t N>
struct BigInt {
    static_assert(N > 0, "BigInt requires N >= 1");

    uint32_t d[N];
    size_t   len;   // число значащих слов (0 ⇒ значение 0)
    bool     neg;   // знак (для нуля всегда false)

    /// Статус последней значащей операции (Overflow/InvalidArg).
    /// mutable — операторы арифметики константны по сигнатуре.
    mutable Status status_ = Status::Ok;

    constexpr BigInt() : d{}, len(0), neg(false) {}

    constexpr BigInt(int32_t v) : d{}, len(0), neg(false) {
        if (v < 0) {
            neg = true;
            d[0] = (uint32_t)(-(v + 1)) + 1u;   // безопасно для INT32_MIN
            len  = 1;
        } else if (v > 0) {
            d[0] = (uint32_t)v;
            len  = 1;
        }
    }

    constexpr BigInt(uint32_t v) : d{}, len(0), neg(false) {
        if (v) { d[0] = v; len = 1; }
    }

    /// Целое ЛЮБОГО другого целочисленного типа.
    ///
    /// Зачем: на 32-битном ARM `uint32_t` — это `unsigned long`, поэтому
    /// литерал `BigInt<8>(7u)` (`unsigned int`) оказывался неоднозначен между
    /// конструкторами int32_t и uint32_t и не компилировался. На x86-64 таких
    /// проблем нет (`uint32_t` == `unsigned int`), поэтому дефект проявлялся
    /// только при сборке прошивки.
    template <typename T,
              typename std::enable_if_t<
                  std::is_integral_v<T> &&
                  !std::is_same_v<T, int32_t> &&
                  !std::is_same_v<T, uint32_t> &&
                  !std::is_same_v<T, bool>, int> = 0>
    constexpr BigInt(T v) : d{}, len(0), neg(false) {
        if constexpr (std::is_signed_v<T>) {
            if (v < T(0)) {
                neg = true;
                // -(v + 1) + 1 — безопасно и для минимального значения типа.
                const uint64_t magnitude =
                    static_cast<uint64_t>(-(static_cast<int64_t>(v) + 1)) + 1u;
                assign_u64(magnitude);
                return;
            }
        }
        assign_u64(static_cast<uint64_t>(v));
    }

    template    <typename... Args,
                typename = std::enable_if_t<
                (std::is_convertible_v<Args, uint32_t> && ...) && 
                (sizeof...(Args) == N) &&
                (sizeof...(Args) > 1)
                >>
    constexpr BigInt(Args... args) 
        : d{static_cast<uint32_t>(args)...}, len(N), neg(false) 
    {
        normalize();
    }

    constexpr BigInt(const uint32_t (&arr)[N], bool is_negative = false)
        : d{}, len(0), neg(is_negative)
    {
        for (size_t i = 0; i < N; ++i) d[i] = arr[i];
        len = N;
        normalize();
        if (is_zero()) neg = false;
    }

// ── Нормализация (убрать ведущие нули) ───

    constexpr void normalize() {
        while (len > 0 && d[len - 1] == 0) --len;
        if (len == 0) neg = false;
    }

    /// Записать неотрицательное 64-битное значение (вспомогательное для
    /// конструктора из произвольного целого типа).
    constexpr void assign_u64(uint64_t v) {
        size_t i = 0;
        while (v != 0u && i < N) {
            d[i++] = static_cast<uint32_t>(v & 0xFFFFFFFFu);
            v >>= 32;
        }
        len = i;
        normalize();
        if (len == 0) neg = false;
    }

    constexpr bool is_zero() const { return len == 0; }

    //  Внутренние операции над модулями

    // Сравнение модулей: -1 / 0 / +1
    static constexpr int cmp_abs(const BigInt& a, const BigInt& b) {
        if (a.len != b.len) return a.len > b.len ? 1 : -1;
        for (size_t i = a.len; i > 0; --i) {
            if (a.d[i - 1] != b.d[i - 1])
                return a.d[i - 1] > b.d[i - 1] ? 1 : -1;
        }
        return 0;
    }

    // Сложение модулей: out = |a| + |b|.
    // Возвращает Status::Overflow, если результат не помещается в N слов:
    // прежде перенос молча терялся (max+1 давало 0), что для криптографии
    // недопустимо.
    static constexpr Status add_abs(const BigInt& a, const BigInt& b, BigInt& out) {
        BigInt tmp{};                       // локальный: out может совпадать с a или b
        uint32_t carry = 0;
        const size_t mx = a.len > b.len ? a.len : b.len;
        size_t i = 0;
        for (; i < mx; ++i) {
            uint64_t s = carry;
            if (i < a.len) s += a.d[i];
            if (i < b.len) s += b.d[i];
            if (i >= N) return Status::Overflow;
            tmp.d[i] = static_cast<uint32_t>(s);
            carry    = static_cast<uint32_t>(s >> 32);
        }
        if (carry != 0u) {
            if (i >= N) return Status::Overflow;
            tmp.d[i] = carry;
            tmp.len  = i + 1u;
        } else {
            tmp.len = mx;
        }
        tmp.normalize();
        out = tmp;
        return Status::Ok;
    }

    // Вычитание модулей: out = |a| - |b|, при условии |a| >= |b|.
    // Возвращает Status::InvalidArg, если условие нарушено.
    // Локальная переменная устраняет самоприсваивание out == a, из-за
    // которого divmod_abs() давал неверный результат (100/7 == 8).
    static constexpr Status sub_abs(const BigInt& a, const BigInt& b, BigInt& out) {
        BigInt tmp{};
        uint32_t borrow = 0;
        for (size_t i = 0; i < a.len; ++i) {
            uint64_t diff = static_cast<uint64_t>(a.d[i]) - borrow;
            if (i < b.len) diff -= b.d[i];
            if (i >= N) return Status::Overflow;
            tmp.d[i] = static_cast<uint32_t>(diff);
            // Заём — бит 32: результат вычитания лежит в 33 битах.
            borrow   = static_cast<uint32_t>((diff >> 32) & 1u);
        }
        if (borrow != 0u) return Status::InvalidArg;   // |a| < |b|
        tmp.len = a.len;
        tmp.normalize();
        out = tmp;
        return Status::Ok;
    }

    // Сдвиг влево на 1 бит (in-place)
    static constexpr void shl1(BigInt& a) {
        uint32_t carry = 0;
        for (size_t i = 0; i < a.len; ++i) {
            uint32_t nc = a.d[i] >> 31;
            a.d[i] = (a.d[i] << 1) | carry;
            carry  = nc;
        }
        if (carry && a.len < N) { a.d[a.len++] = carry; }
    }

    // Умножение модулей (школьный O(n²)).
    // Возвращает Status::Overflow при переполнении: прежде перенос в старшее
    // слово не распространялся, и произведение молча становилось неверным.
    static constexpr Status mul_abs(const BigInt& a, const BigInt& b, BigInt& out) {
        BigInt tmp{};
        for (size_t i = 0; i < a.len; ++i) {
            uint64_t carry = 0;
            for (size_t j = 0; j < b.len; ++j) {
                const size_t k = i + j;
                if (k >= N) return Status::Overflow;
                const uint64_t p = static_cast<uint64_t>(a.d[i]) * b.d[j]
                                 + tmp.d[k] + carry;
                tmp.d[k] = static_cast<uint32_t>(p);
                carry    = p >> 32;
            }
            // Распространение переноса ЦИКЛОМ, а не однократным сложением.
            for (size_t k = i + b.len; carry != 0u; ++k) {
                if (k >= N) return Status::Overflow;
                const uint64_t sum = static_cast<uint64_t>(tmp.d[k]) + carry;
                tmp.d[k] = static_cast<uint32_t>(sum);
                carry    = sum >> 32;
            }
        }
        size_t len = a.len + b.len;
        if (len > N) len = N;
        tmp.len = len;
        tmp.normalize();
        out = tmp;
        return Status::Ok;
    }

    // Деление модулей (побитовый алгоритм): |a| = q·|b| + r
    static constexpr Status divmod_abs(const BigInt& a, const BigInt& b,
                                BigInt& q, BigInt& r)
    {
        if (b.is_zero()) return Status::NotSupported;
        q = BigInt();
        r = BigInt();

        size_t bits = a.len * 32;
        for (size_t i = bits; i > 0; --i) {
            shl1(r);
            size_t wi = (i - 1) / 32;
            size_t bi = (i - 1) % 32;
            if (wi < a.len && (a.d[wi] & (1u << bi))) {
                r.d[0] |= 1u;
                if (r.len == 0) r.len = 1;
            }
            r.normalize();

            if (cmp_abs(r, b) >= 0) {
                if (sub_abs(r, b, r) != Status::Ok) return Status::Fault;
                size_t qw = (i - 1) / 32;
                size_t qb = (i - 1) % 32;
                if (qw < N) {
                    q.d[qw] |= (1u << qb);
                    if (qw >= q.len) q.len = qw + 1;
                }
            }
        }
        q.normalize();
        return Status::Ok;
    }

    //  Арифметические операторы

    constexpr Status last_status() const { return status_; }

    constexpr BigInt operator+(const BigInt& b) const {
        BigInt r;
        status_ = Status::Ok;
        if (neg == b.neg) {
            status_ = add_abs(*this, b, r);
            r.neg = neg;
        } else {
            const int c = cmp_abs(*this, b);
            if (c >= 0) { status_ = sub_abs(*this, b, r); r.neg = neg; }
            else        { status_ = sub_abs(b, *this, r); r.neg = b.neg; }
        }
        r.normalize();
        if (r.is_zero()) r.neg = false;
        return r;
    }

    constexpr BigInt operator-(const BigInt& b) const {
        BigInt nb = b;
        if (!nb.is_zero()) nb.neg = !nb.neg;
        return *this + nb;
    }

    constexpr BigInt operator*(const BigInt& b) const {
        BigInt r;
        status_ = mul_abs(*this, b, r);
        r.neg = (neg != b.neg) && !r.is_zero();
        return r;
    }

    constexpr BigInt operator/(const BigInt& b) const {
        BigInt q, r;
        status_ = divmod_abs(*this, b, q, r);
        q.neg = (neg != b.neg) && !q.is_zero();
        return q;
    }

    constexpr BigInt operator%(const BigInt& b) const {
        BigInt q, r;
        status_ = divmod_abs(*this, b, q, r);
        r.neg = neg && !r.is_zero();
        return r;
    }

    //  Битовые операторы (над модулями)

    constexpr BigInt operator|(const BigInt& b) const {
        BigInt r;
        size_t mx = len > b.len ? len : b.len;
        for (size_t i = 0; i < mx; ++i) {
            uint32_t av = i < len   ? d[i]   : 0;
            uint32_t bv = i < b.len ? b.d[i] : 0;
            r.d[i] = av | bv;
        }
        r.len = mx;
        r.neg = false;
        r.normalize();
        return r;
    }

    constexpr BigInt operator&(const BigInt& b) const {
        BigInt r;
        size_t mn = len < b.len ? len : b.len;
        for (size_t i = 0; i < mn; ++i)
            r.d[i] = d[i] & b.d[i];
        r.len = mn;
        r.neg = false;
        r.normalize();
        return r;
    }

    constexpr BigInt operator^(const BigInt& b) const {
        BigInt r;
        size_t mx = len > b.len ? len : b.len;
        for (size_t i = 0; i < mx; ++i) {
            uint32_t av = i < len   ? d[i]   : 0;
            uint32_t bv = i < b.len ? b.d[i] : 0;
            r.d[i] = av ^ bv;
        }
        r.len = mx;
        r.neg = false;
        r.normalize();
        return r;
    }

    //  Операторы сравнения

    constexpr bool operator==(const BigInt& b) const {
        if (neg != b.neg || len != b.len) return false;
        for (size_t i = 0; i < len; ++i)
            if (d[i] != b.d[i]) return false;
        return true;
    }

    constexpr bool operator<(const BigInt& b) const {
        if (neg != b.neg) return neg;          // отриц. < полож.
        int c = cmp_abs(*this, b);
        return neg ? (c > 0) : (c < 0);        // инверсия для отриц.
    }

    constexpr bool operator>(const BigInt& b)  const { return b < *this;       }
    constexpr bool operator<=(const BigInt& b) const { return !(b < *this);    }
    constexpr bool operator>=(const BigInt& b) const { return !(*this < b);    }
};

}