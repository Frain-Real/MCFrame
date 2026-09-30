#pragma once

#include "../common.hpp"
#include "../utilities.hpp"
#include "../big_math.hpp"

namespace embed {

enum class EccCurve {
    SECP256K1,
    SECP256R1
};

template <EccCurve Curve>
struct EccTraits;

template <>
struct EccTraits<EccCurve::SECP256K1> {
    static constexpr size_t Words = 8;
    static constexpr BigInt<Words> P = { 0xFFFFFC2F, 0xFFFFFFFE, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
    static constexpr BigInt<Words> A = { 0, 0, 0, 0, 0, 0, 0, 0 };
    static constexpr BigInt<Words> B = { 7, 0, 0, 0, 0, 0, 0, 0 };
    static constexpr BigInt<Words> N = { 0xD0364141, 0xBFD25E8C, 0xAF48A03B, 0xBAAEDCE6, 0xFFFFFFFE, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
    static constexpr BigInt<Words> Gx = { 0x16F81798, 0x59F2815B, 0x2DCE28D9, 0x029BFCDB, 0xCE870B07, 0x55A06295, 0xF9DCBBAC, 0x79BE667E };
    static constexpr BigInt<Words> Gy = { 0xFB10D4B8, 0x9C47D08F, 0xA68554199, 0xFD557D33, 0xC1711CFE, 0x4A3B9373, 0x5C393111, 0x483ADA77 };
};
template <>
struct EccTraits<EccCurve::SECP256R1> {
    static constexpr size_t Words = 8;
    static constexpr BigInt<Words> P = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0xFFFFFFFF};
    static constexpr BigInt<Words> A = {0xFFFFFFFC, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0xFFFFFFFF};
    static constexpr BigInt<Words> B = {0x27D2604B, 0x3BCE3C3E, 0xCC53B0F6, 0x651D06B0, 0x769886BC, 0xB3EBBD55, 0x8EE7EB4A, 0x5AC635D8};
    static constexpr BigInt<Words> N = {0xFC632551, 0xF3B9CAC2, 0xA7179E84, 0xBCE6FAAD, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF};
    static constexpr BigInt<Words> Gx = {0xD898C296, 0xF4A13945, 0x2DEB33A0, 0x77037D81, 0x63A440F2, 0xF8BCE6E5, 0xE12C4247, 0x6B17D1F2};
    static constexpr BigInt<Words> Gy = {0x37BF51F5, 0xCBB64068, 0x6B315ECE, 0x2BCE3357, 0x7C0F9E16, 0x8EE7EB4A, 0xFE1A7F9B, 0x4FE342E2};
};

template <size_t Words>
struct EccPoint {
    BigInt<Words> x;
    BigInt<Words> y;
    bool is_infinity;

    constexpr EccPoint() : x(0), y(0), is_infinity(true) {}
    constexpr EccPoint(const BigInt<Words>& x_val, const BigInt<Words>& y_val) : x(x_val), y(y_val), is_infinity(false) {}
    
    static constexpr EccPoint Infinity() { return EccPoint(); }
    
    bool operator==(const EccPoint& other) const {
        if (is_infinity && other.is_infinity) return true;
        if (is_infinity != other.is_infinity) return false;
        return (x == other.x && y == other.y);
    }
};

template <typename Impl, EccCurve Curve, bool EnableDecrypt = false>
class ECC {
public:
    bool GenerateKeyPair(const BigInt<EccTraits<Curve>::Words>& out_private, const EccPoint<EccTraits<Curve>::Words>& out_public) { 
        return static_cast<Impl*>(this)->GenerateKeyPair(out_private, out_public); 
    }
    bool ComputePublicKey(BigInt<EccTraits<Curve>::Words>& private_key, const EccPoint<EccTraits<Curve>::Words>& out_public) { 
        return static_cast<Impl*>(this)->ComputePublicKey(private_key, out_public); 
    }
    bool DeriveSharedSecret(BigInt<EccTraits<Curve>::Words>& private_key, const EccPoint<EccTraits<Curve>::Words>& remote_public, const EccPoint<EccTraits<Curve>::Words>& out_secret) { 
        return static_cast<Impl*>(this)->DeriveSharedSecret(private_key, remote_public, out_secret); 
    }
    
    bool Encrypt(const uint8_t* input, Size input_len, uint8_t* output, Size* output_len) { 
        return static_cast<Impl*>(this)->Encrypt(input, input_len, output, output_len); 
    }
    bool Decrypt(const uint8_t* input, Size input_len, uint8_t* output, Size* output_len) { 
        return static_cast<Impl*>(this)->Decrypt(input, input_len, output, output_len); 
    }

    Size GetEncryptedSize(Size data_len) { 
        return static_cast<Impl*>(this)->GetEncryptedSize(data_len); 
    }
};

template <EccCurve Curve, bool EnableDecrypt = false>
class SoftECC : public ECC<SoftECC<Curve, EnableDecrypt>, Curve, EnableDecrypt> {
public:
    static_assert(
        EccTraits<Curve>::P > 2,
        "Curve modulus P must be greater than 2"
    );
    static constexpr size_t Words = EccTraits<Curve>::Words;
    using Point = EccPoint<Words>;
    using Int = BigInt<Words>;

    SoftECC(const SoftECC&) = delete;
    SoftECC& operator=(const SoftECC&) = delete;

    SoftECC(const Int& private_key = Int(), const Point& remote_public_key = Point::Infinity()) 
        : m_private_key(private_key), m_remote_public_key(remote_public_key) 
    {
        if (m_private_key != 0) {
            ComputePublicKey(m_private_key, m_public_key);
        } else {
            m_public_key = Point::Infinity();
        }
    }

    bool GenerateKeyPair(Int& out_private, Point& out_public) {
        for (size_t i = 0; i < Words; ++i) {
            out_private.data[i] = utilities::rand();
        }
        out_private = out_private % Traits::N;
        if (out_private.IsZero()) out_private.data[0] = 1;
        
        return ComputePublicKey(out_private, out_public);
    }

    bool ComputePublicKey(const Int& private_key, Point& out_public) {
        if (private_key.IsZero() || private_key >= Traits::N) {
            return false;
        }
        out_public = ScalarMul(private_key, m_G);
        return true;
    }

    bool DeriveSharedSecret(const Int& private_key, const Point& remote_public, Point& out_secret) {
        if (private_key.IsZero() || private_key >= Traits::N) return false;
        if (!IsOnCurve(remote_public)) return false;
        
        out_secret = ScalarMul(private_key, remote_public);
        return !out_secret.is_infinity;
    }

    Size GetEncryptedSize(Size data_len) {
        return data_len + 64;
    }

    bool Encrypt(const uint8_t* input, Size input_len, uint8_t* output, Size* output_len) {
        if (input == nullptr || output == nullptr || output_len == nullptr) return false;
        if (m_remote_public_key.is_infinity) return false;

        Size required_size = GetEncryptedSize(input_len);
        if (*output_len < required_size && *output_len != 0) {
            return false;
        }

        Int eph_private;
        Point eph_public;
        if (!GenerateKeyPair(eph_private, eph_public)) return false;

        Point shared_secret;
        if (!DeriveSharedSecret(eph_private, m_remote_public_key, shared_secret)) return false;

        size_t offset = 0;
        for (size_t i = 0; i < Words; ++i, offset += 4) {
            utilities::fast_memcpy(output + offset, &eph_public.x.data[i], 4);
        }
        for (size_t i = 0; i < Words; ++i, offset += 4) {
            utilities::fast_memcpy(output + offset, &eph_public.y.data[i], 4);
        }

        uint8_t sym_key[32];
        for (size_t i = 0; i < Words && i < 8; ++i) {
            utilities::fast_memcpy(sym_key + i * 4, &shared_secret.x.data[i], 4);
        }
        
        for (Size i = 0; i < input_len; ++i) {
            output[64 + i] = input[i] ^ sym_key[i % 32];
        }
        *output_len = required_size;
        return true;
    }

    bool Decrypt(const uint8_t* input, Size input_len, uint8_t* output, Size* output_len) {
        static_assert(EnableDecrypt, "ECC decryption is disabled");
        if (input == nullptr || output == nullptr || output_len == nullptr) return false;
        if (m_private_key.IsZero()) return false;
        if (input_len < 64) return false;

        Size payload_len = input_len - 64;
        if (*output_len < payload_len && *output_len != 0) return false;

        Point eph_public;
        eph_public.is_infinity = false;
        size_t offset = 0;
        for (size_t i = 0; i < Words; ++i, offset += 4) {
            utilities::fast_memcpy(&eph_public.x.data[i], input + offset, 4);
        }
        for (size_t i = 0; i < Words; ++i, offset += 4) {
            utilities::fast_memcpy(&eph_public.y.data[i], input + offset, 4);
        }
        if (!IsOnCurve(eph_public)) return false;

        Point shared_secret;
        if (!DeriveSharedSecret(m_private_key, eph_public, shared_secret)) return false;

        uint8_t sym_key[32];
        for (size_t i = 0; i < Words && i < 8; ++i) {
            utilities::fast_memcpy(sym_key + i * 4, &shared_secret.x.data[i], 4);
        }

        for (Size i = 0; i < payload_len; ++i) {
            output[i] = input[64 + i] ^ sym_key[i % 32];
        }

        *output_len = payload_len;
        return true;
    }

private:
    using Traits = EccTraits<Curve>;
    
    Int m_private_key;
    Point m_public_key;
    Point m_remote_public_key;
    
    static constexpr Point m_G = Point(Traits::Gx, Traits::Gy);

    static Int FieldAdd(const Int& a, const Int& b) {
        Int sum = a + b;
        if (sum >= Traits::P) {
            sum = sum - Traits::P;
        }
        return sum;
    }

    static Int FieldSub(const Int& a, const Int& b) {
        if (a >= b) {
            return a - b;
        } else {
            return Traits::P - (b - a);
        }
    }

    static Int FieldMul(const Int& a, const Int& b) {
        Int product = a * b;
        return product % Traits::P;
    }

    static Int FieldInv(const Int& a) {
        Int res = Int(1);
        Int base = a % Traits::P;
        Int exp = Traits::P - Int(2);
        
        for (size_t i = 0; i < Words * 32; ++i) {
            if (exp.GetBit(i)) {
                res = FieldMul(res, base);
            }
            base = FieldMul(base, base);
        }
        return res;
    }

    static bool IsOnCurve(const Point& p) {
        if (p.is_infinity) return true;
        
        Int lhs = FieldMul(p.y, p.y);
        Int x2 = FieldMul(p.x, p.x);
        Int x3 = FieldMul(x2, p.x);
        Int ax = FieldMul(Traits::A, p.x);
        Int rhs = FieldAdd(FieldAdd(x3, ax), Traits::B);
        
        return lhs == rhs;
    }

    static Point PointDouble(const Point& p) {
        if (p.is_infinity || p.y.IsZero()) return Point::Infinity();
        
        Int x2 = FieldMul(p.x, p.x);
        Int three_x2 = FieldAdd(FieldAdd(x2, x2), x2);
        Int num = FieldAdd(three_x2, Traits::A);
        Int den = FieldAdd(p.y, p.y);
        Int lambda = FieldMul(num, FieldInv(den));

        Int lambda2 = FieldMul(lambda, lambda);
        Int x3 = FieldSub(FieldSub(lambda2, p.x), p.x);
        Int y3 = FieldSub(FieldMul(lambda, FieldSub(p.x, x3)), p.y);
        
        return Point(x3, y3);
    }

    static Point PointAdd(const Point& p1, const Point& p2) {
        if (p1.is_infinity) return p2;
        if (p2.is_infinity) return p1;
        
        if (p1.x == p2.x) {
            if (p1.y == p2.y) return PointDouble(p1);
            return Point::Infinity();
        }

        Int num = FieldSub(p2.y, p1.y);
        Int den = FieldSub(p2.x, p1.x);
        Int lambda = FieldMul(num, FieldInv(den));

        Int lambda2 = FieldMul(lambda, lambda);
        Int x3 = FieldSub(FieldSub(lambda2, p1.x), p2.x);
        Int y3 = FieldSub(FieldMul(lambda, FieldSub(p1.x, x3)), p1.y);
        
        return Point(x3, y3);
    }

    static Point ScalarMul(const Int& k, const Point& p) {
        Point r0 = Point::Infinity();
        Point r1 = p;
        for (size_t i = Words * 32; i > 0; --i) {
            bool bit = k.GetBit(i - 1);
            if (bit) {
                Int temp_x = r0.x;
                Int temp_y = r0.y;
                bool temp_inf = r0.is_infinity;
            
                r0.x = r1.x;
                r0.y = r1.y;
                r0.is_infinity = r1.is_infinity;
                
                r1.x = temp_x;
                r1.y = temp_y;
                r1.is_infinity = temp_inf;
            }

            Point sum = PointAdd(r0, r1);
            Point dbl = PointDouble(r0);
            r0 = dbl;
            r1 = sum;
        }
        return r0;
    }
};

}