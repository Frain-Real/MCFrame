// crypto/tdes.hpp
// ---------------------------------------------------------------------------
// DES и Triple-DES (3DES/EDE) в режимах ECB, CBC, CTR.
//
// ЧТО БЫЛО ИСПРАВЛЕНО:
//   1. ГЛАВНОЕ: BLOCK_SIZE = 64 объявлялся как «размер блока в битах», но
//      использовался как размер в БАЙТАХ. Из-за этого:
//        — encrypt()/decrypt() делили данные на 64-байтные «блоки» и
//          зашифровывали только первые 8 байт каждого, остальные 56 байт
//          уходили в шифротекст ОТКРЫТЫМ текстом;
//        — длина шифротекста считалась по 64-байтной сетке;
//        — циклы XOR (CBC/CTR) проходили по 64 байтам вместо 8.
//      Теперь блок — 8 байт, и это зафиксировано статической проверкой.
//   2. Буфер под открытый текст выделялся на СТЕКЕ размером
//      MAX_DATA_SIZE + BLOCK_SIZE (1088 байт) — на задача с 512 байтами
//      стека это переполнение стека. Шифрование идёт потоково, блок за
//      блоком, без промежуточной копии всего сообщения.
//   3. Размер выходного буфера не проверялся вообще, а признаком ошибки был
//      возврат 0 — то есть расшифрование пустого сообщения было неотличимо
//      от ошибки. Теперь Status + отдельный out_len и явная ёмкость буфера.
//   4. CBC и CTR использовали ОДИН буфер ivBuffer: в CTR это ломало
//      начальный счётчик (он перезаписывался цепочкой от CBC-ветки).
//      Теперь состояние режима локально для операции.
//   5. Мёртвый код: paddedLen считался дважды, ветка `paddingBytes == 0`
//      была недостижима, Magic-числа 64/8 путались.
// ---------------------------------------------------------------------------
#pragma once

#include "crypto_utils.hpp"

#include <cstddef>
#include <cstdint>

namespace embed {
namespace crypto {

/**
 * @brief Одиночный DES (FIPS 46-3).
 *
 * Оставлен публичным сознательно: часть legacy-протоколов работает именно
 * с одиночным DES. Для новых задач использовать нельзя — 56-битный ключ
 * перебирается за сутки на бытовой видеокарте.
 */
class Des {
public:
    static constexpr Size BlockSize = 8u;
    static constexpr Size KeyBytes  = 8u;

    /// Ключ копируется; nullptr ⇒ объект невалиден (encrypt_block — no-op).
    explicit Des(const uint8_t key[KeyBytes]) noexcept;

    Des(const Des&)            = delete;
    Des& operator=(const Des&) = delete;

    bool valid() const noexcept { return valid_; }

    /// Зашифровать один блок (8 байт) на месте.
    void encrypt_block(uint8_t block[BlockSize]) const noexcept;

    /// Расшифровать один блок (8 байт) на месте.
    void decrypt_block(uint8_t block[BlockSize]) const noexcept;

    /// Затереть раундовые ключи.
    void clear() noexcept;

private:
    uint64_t subkeys_[16];
    bool     valid_;
};

/**
 * @brief Triple-DES (EDE: шифр — расшифрование — шифр).
 *
 * Размер блока — 8 байт, ключ — 24 байта (или 16 байт для двухключевого
 * режима, тогда K3 = K1; или 8 байт, тогда это одиночный DES).
 */
class Tdes {
public:
    static constexpr Size BlockSize = 8u;
    static constexpr Size KeyBytes  = 8u;

    enum class Mode {
        Ecb,    ///< Electronic Codebook
        Cbc,    ///< Cipher Block Chaining (нужен IV)
        Ctr     ///< Counter (нужен начальный счётчик)
    };

    Tdes(const uint8_t key1[KeyBytes],
         const uint8_t key2[KeyBytes],
         const uint8_t key3[KeyBytes]) noexcept;

    /// Двухключевой 3DES: K3 = K1.
    Tdes(const uint8_t key1[KeyBytes],
         const uint8_t key2[KeyBytes]) noexcept;

    /// Один ключ: EDE с одинаковыми ключами сводится к одиночному DES.
    explicit Tdes(const uint8_t key[KeyBytes]) noexcept;

    ~Tdes() noexcept { clear(); }

    Tdes(const Tdes&)            = delete;
    Tdes& operator=(const Tdes&) = delete;

    bool valid() const noexcept { return valid_; }

    /// Размер шифротекста для data_len байт открытого текста (PKCS#7 всегда
    /// добавляет минимум один байт).
    static constexpr Size encrypted_size(Size data_len) noexcept {
        return pkcs7_padded_size(data_len, BlockSize);
    }

    /**
     * @brief Зашифровать буфер (с PKCS#7).
     * @param ivOrNonce IV для CBC или начальный счётчик для CTR (8 байт).
     *                  Для ECB игнорируется.
     * @return Status::InvalidArg — нулевые указатели, отсутствующий IV,
     *         невалидный ключ; Status::NoMemory — мало места в output.
     */
    Status encrypt(const uint8_t* input, Size input_len,
                   uint8_t* output, Size output_cap,
                   Size* output_len,
                   Mode mode, const uint8_t* ivOrNonce = nullptr) const noexcept;

    /**
     * @brief Расшифровать буфер и снять padding.
     * @return Status::Corruption — padding некорректен (неверный ключ/IV или
     *         данные повреждены).
     */
    Status decrypt(const uint8_t* input, Size input_len,
                   uint8_t* output, Size output_cap,
                   Size* output_len,
                   Mode mode, const uint8_t* ivOrNonce = nullptr) const noexcept;

    /// «Сырой» TDES-EDE без режима и padding'а (один блок 8 байт, на месте).
    void encrypt_block(uint8_t block[BlockSize]) const noexcept;
    void decrypt_block(uint8_t block[BlockSize]) const noexcept;

    void clear() noexcept;

private:
    Des des1_;
    Des des2_;
    Des des3_;
    bool valid_;
};

} // namespace crypto
} // namespace embed
