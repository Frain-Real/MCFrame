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

    // Сложение модулей: out = |a| + |b|
    static constexpr void add_abs(const BigInt& a, const BigInt& b, BigInt& out) {
        out = BigInt();
        uint32_t carry = 0;
        size_t mx = a.len > b.len ? a.len : b.len;
        for (size_t i = 0; i < mx || carry; ++i) {
            uint64_t s = carry;
            if (i < a.len) s += a.d[i];
            if (i < b.len) s += b.d[i];
            if (i < N) out.d[i] = (uint32_t)s;
            carry = (uint32_t)(s >> 32);
        }
        out.len = mx;
        if (carry && mx < N) { out.d[mx] = carry; out.len = mx + 1; }
        out.normalize();
    }

    // Вычитание модулей: out = |a| - |b|,  гарантировано |a| >= |b|
    static constexpr void sub_abs(const BigInt& a, const BigInt& b, BigInt& out) {
        out = BigInt();
        uint32_t borrow = 0;
        for (size_t i = 0; i < a.len; ++i) {
            uint64_t diff = (uint64_t)a.d[i] - borrow;
            if (i < b.len) diff -= b.d[i];
            out.d[i] = (uint32_t)diff;
            borrow   = (uint32_t)(diff >> 63);   // 1 если underflow
        }
        out.len = a.len;
        out.normalize();
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

    // Умножение модулей (школьный O(n²))
    static constexpr void mul_abs(const BigInt& a, const BigInt& b, BigInt& out) {
        out = BigInt();
        for (size_t i = 0; i < a.len; ++i) {
            uint32_t carry = 0;
            for (size_t j = 0; j < b.len; ++j) {
                size_t k = i + j;
                if (k >= N) break;
                uint64_t p = (uint64_t)a.d[i] * b.d[j] + out.d[k] + carry;
                out.d[k] = (uint32_t)p;
                carry    = (uint32_t)(p >> 32);
            }
            size_t k = i + b.len;
            if (k < N && carry) out.d[k] += carry;
        }
        out.len = a.len + b.len;
        if (out.len > N) out.len = N;
        out.normalize();
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
                sub_abs(r, b, r);
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

    constexpr BigInt operator+(const BigInt& b) const {
        BigInt r;
        if (neg == b.neg) {
            add_abs(*this, b, r);
            r.neg = neg;
        } else {
            int c = cmp_abs(*this, b);
            if (c >= 0) { sub_abs(*this, b, r); r.neg = neg; }
            else        { sub_abs(b, *this, r); r.neg = b.neg; }
        }
        r.normalize();
        return r;
    }

    constexpr BigInt operator-(const BigInt& b) const {
        BigInt nb = b;
        if (!nb.is_zero()) nb.neg = !nb.neg;
        return *this + nb;
    }

    constexpr BigInt operator*(const BigInt& b) const {
        BigInt r;
        mul_abs(*this, b, r);
        r.neg = (neg != b.neg) && !r.is_zero();
        return r;
    }

    constexpr BigInt operator/(const BigInt& b) const {
        BigInt q, r;
        divmod_abs(*this, b, q, r);
        q.neg = (neg != b.neg) && !q.is_zero();
        return q;
    }

    constexpr BigInt operator%(const BigInt& b) const {
        BigInt q, r;
        divmod_abs(*this, b, q, r);
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