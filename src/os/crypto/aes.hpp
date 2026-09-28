#pragma once

#include "../common.hpp"

namespace embed {

class AES {
public:
    enum class AESKeySize {
        AES_128 = 128,
        AES_192 = 192,
        AES_256 = 256
    };

    enum class AESMode {
        ECB,
        CBC,
        CTR
    };
    // Конструктор
    // key: указатель на массив байт ключа
    // keySize: размер ключа (128, 192, 256 бит)
    // mode: режим работы (ECB, CBC, CTR)
    // iv: вектор инициализации для режимов CBC и CTR (должен быть 16 байт)
    AES(const uint8_t* key, AESKeySize keySize, AESMode mode, const uint8_t* iv = nullptr);

    // Шифрование данных
    // input: указатель на входные данные
    // input_len: длина входных данных в байтах
    // output: указатель на буфер для зашифрованных данных (должен быть достаточно большим)
    // output_len: указатель на переменную, куда будет записана фактическая длина зашифрованных данных (после padding'а)
    bool Encrypt(const uint8_t* input, Size input_len, uint8_t* output, Size* output_len);

    // Дешифрование данных
    // input: указатель на входные данные (зашифрованные)
    // input_len: длина входных данных в байтах (зашифрованных)
    // output: указатель на буфер для дешифрованных данных
    // output_len: указатель на переменную, куда будет записана фактическая длина дешифрованных данных (после удаления padding'а)
    bool Decrypt(const uint8_t* input, Size input_len, uint8_t* output, Size* output_len);

    // Статический метод для расчета размера буфера с padding'ом
    static Size GetPaddedSize(Size data_len);

private:
    uint8_t _Nk; // Количество 32-битных слов в ключе (4, 6, 8)
    uint8_t _Nr; // Количество раундов (10, 12, 14)
    uint8_t _keySizeBits; // Размер ключа в битах
    AESMode _mode; // Режим шифрования
    uint8_t _expandedKey[240]; // Максимальный размер расширенного ключа (15 раундов * 16 байт/раунд)
    uint8_t _iv[16]; // Вектор инициализации для CBC/CTR

    // Внутренние функции AES
    void KeyExpansion(const uint8_t* key);
    void AddRoundKey(uint8_t* state, const uint8_t* roundKey);
    void SubBytes(uint8_t* state);
    void InvSubBytes(uint8_t* state);
    void ShiftRows(uint8_t* state);
    void InvShiftRows(uint8_t* state);
    void MixColumns(uint8_t* state);
    void InvMixColumns(uint8_t* state);

    // Основная функция шифрования одного блока (16 байт)
    void Cipher(uint8_t* state);

    // Основная функция дешифрования одного блока (16 байт)
    void InvCipher(uint8_t* state);

    // Функции для различных режимов
    void EncryptECB(const uint8_t* input, uint8_t* output, Size length);
    void DecryptECB(const uint8_t* input, uint8_t* output, Size length);
    void EncryptCBC(const uint8_t* input, uint8_t* output, Size length);
    void DecryptCBC(const uint8_t* input, uint8_t* output, Size length);
    void EncryptCTR(const uint8_t* input, uint8_t* output, Size length);
    void DecryptCTR(const uint8_t* input, uint8_t* output, Size length); // CTR одинаков для шифрования и дешифрования
    
    // Функции для PKCS7 Padding
    // Добавляет PKCS7 padding к данным. Возвращает новую длину.
    // data: указатель на буфер с данными.
    // data_len: текущая длина данных.
    // max_len: максимальный размер буфера (чтобы убедиться, что есть место для padding'а).
    static Size AddPKCS7Padding(uint8_t* data, Size data_len, Size max_len);

    // Удаляет PKCS7 padding из данных. Возвращает фактическую длину данных без padding'а.
    // data: указатель на буфер с данными.
    // data_len: текущая длина данных (с padding'ом).
    static Size RemovePKCS7Padding(const uint8_t* data, Size data_len);
};

}
