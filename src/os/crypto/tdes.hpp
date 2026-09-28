#pragma once

#include "../utilities.hpp"
#include <cstdint>
namespace embed {
// Класс для TDES шифрования
class TDES {
public:
    static const uint32_t BLOCK_SIZE = 64;
    static const uint32_t MAX_DATA_SIZE = 1024;
    // Конструктор: принимает три 64-битных ключа (56 эффективных бит каждый)
    TDES(const uint8_t key1[8], const uint8_t key2[8], const uint8_t key3[8]);

    // Перечисление для режимов работы
    enum class Mode {
        ECB, // Electronic Codebook
        CBC, // Cipher Block Chaining
        CTR  // Counter Mode
    };

    // Шифрование данных с padding'ом
    // input: указатель на входные данные
    // inputLen: длина входных данных в байтах
    // output: буфер для зашифрованных данных (должен быть достаточно большим)
    //         Максимальный размер: inputLen + BLOCK_SIZE
    // mode: режим работы
    // ivOrNonce: вектор инициализации (для CBC) или начальный счетчик (для CTR).
    //            Для ECB не используется (можно передать nullptr).
    //            Должен быть BLOCK_SIZE байт.
    // Возвращает фактическую длину зашифрованных данных.
    Size encrypt(const uint8_t* input, Size inputLen, uint8_t* output, Mode mode, const uint8_t ivOrNonce[8] = nullptr);

    // Дешифрование данных
    // input: указатель на зашифрованные данные
    // inputLen: длина зашифрованных данных в байтах (должна быть кратна BLOCK_SIZE)
    // output: буфер для дешифрованных данных (должен быть достаточно большим)
    //         Максимальный размер: inputLen
    // mode: режим работы (должен совпадать с тем, что использовался при шифровании)
    // ivOrNonce: вектор инициализации (для CBC) или начальный счетчик (для CTR).
    //            Для ECB не используется (можно передать nullptr).
    //            Должен быть BLOCK_SIZE байт.
    // Возвращает фактическую длину дешифрованных данных (без padding'а).
    Size decrypt(const uint8_t* input, Size inputLen, uint8_t* output, Mode mode, const uint8_t ivOrNonce[8] = nullptr);

private:
    // --- Внутренний класс для DES шифрования (без изменений) ---
    class DES {
    public:
        DES(const uint8_t key[8]);
        void encryptBlock(uint8_t data[8]);
        void decryptBlock(uint8_t data[8]);
    private:
        uint64_t subkeys[16];
        void generateSubkeys(const uint8_t key[8]);
        uint32_t feistelFunction(uint32_t rightHalf, uint64_t subkey);
        uint64_t permute(uint64_t input, const uint8_t* permutationTable, int inputBits, int outputBits);
        void rotateLeft(uint32_t* value, int bits);

        // DES-таблицы (как статические константы)
        static const uint8_t PC1[56];
        static const uint8_t PC2[48];
        static const uint8_t ShiftBits[16];
        static const uint8_t IP[64];
        static const uint8_t FP[64];
        static const uint8_t E_Permutation[48];
        static const uint8_t P_Permutation[32];
        static const uint8_t S_Boxes[8][64];
    };

    DES des1; // Первый DES
    DES des2; // Второй DES
    DES des3; // Третий DES

    // Вспомогательные методы для TDES (блочные операции)
    void tdesEncryptBlock(uint8_t data[8]);
    void tdesDecryptBlock(uint8_t data[8]);

    // Функции padding'а
    // Добавляет PKCS#7 padding к буферу.
    // data: буфер для данных.
    // currentLen: текущая длина данных в буфере.
    // Возвращает новую длину данных после добавления padding'а.
    Size addPKCS7Padding(uint8_t* data, Size currentLen);

    // Удаляет PKCS#7 padding из буфера.
    // data: буфер для данных.
    // currentLen: текущая длина данных в буфере (должна быть кратна BLOCK_SIZE).
    // Возвращает новую длину данных после удаления padding'а.
    Size removePKCS7Padding(uint8_t* data, Size currentLen);

    // Вспомогательный буфер для операций с блоками
    uint8_t blockBuffer[BLOCK_SIZE];
    uint8_t ivBuffer[BLOCK_SIZE]; // Для хранения IV/Counter
    uint8_t counterBuffer[BLOCK_SIZE]; // Для CTR режима
};

// ====================================================================================================
// Реализация DES-таблиц (размещаем их вне класса, чтобы они были статическими)
// ====================================================================================================

// Initial Permutation (IP) table
const uint8_t TDES::DES::IP[64] = {
    58, 50, 42, 34, 26, 18, 10, 2,
    60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6,
    64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9,  1,
    59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5,
    63, 55, 47, 39, 31, 23, 15, 7
};

// Final Permutation (FP) table (inverse of IP)
const uint8_t TDES::DES::FP[64] = {
    40, 8, 48, 16, 56, 24, 64, 32,
    39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30,
    37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28,
    35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26,
    33, 1, 41, 9, 49, 17, 57, 25
};

// Permuted Choice 1 (PC1) table
const uint8_t TDES::DES::PC1[56] = {
    57, 49, 41, 33, 25, 17, 9,
    1,  58, 50, 42, 34, 26, 18,
    10, 2,  59, 51, 43, 35, 27,
    19, 11, 3,  60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15,
    7,  62, 54, 46, 38, 30, 22,
    14, 6,  61, 53, 45, 37, 29,
    21, 13, 5,  28, 20, 12, 4
};

// Permuted Choice 2 (PC2) table
const uint8_t TDES::DES::PC2[48] = {
    14, 17, 11, 24, 1,  5,
    3,  28, 15, 6,  21, 10,
    23, 19, 12, 4,  26, 8,
    16, 7,  27, 20, 13, 2,
    41, 52, 31, 37, 47, 55,
    30, 40, 51, 45, 33, 48,
    44, 49, 39, 56, 34, 53,
    46, 42, 50, 36, 29, 32
};

// Left shifts for key schedule
const uint8_t TDES::DES::ShiftBits[16] = {
    1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1
};

// Expansion Permutation (E-Permutation) table
const uint8_t TDES::DES::E_Permutation[48] = {
    32, 1,  2,  3,  4,  5,
    4,  5,  6,  7,  8,  9,
    8,  9,  10, 11, 12, 13,
    12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21,
    20, 21, 22, 23, 24, 25,
    24, 25, 26, 27, 28, 29,
    28, 29, 30, 31, 32, 1
};

// P-Box Permutation (P-Permutation) table
const uint8_t TDES::DES::P_Permutation[32] = {
    16, 7,  20, 21,
    29, 12, 28, 17,
    1,  15, 23, 26,
    5,  18, 31, 10,
    2,  8,  24, 14,
    32, 27, 3,  9,
    19, 13, 30, 6,
    22, 11, 4,  25
};

// S-Boxes (8 S-Boxes, each 6x4 to 4 bits)
const uint8_t TDES::DES::S_Boxes[8][64] = {
    // S1
    {14, 4,  13, 1,  2,  15, 11, 8,  3,  10, 6,  12, 5,  9,  0,  7,
     0,  15, 7,  4,  14, 2,  13, 1,  10, 6,  12, 11, 9,  5,  3,  8,
     4,  1,  14, 8,  13, 6,  2,  11, 15, 12, 9,  7,  3,  10, 5,  0,
     15, 12, 8,  2,  4,  9,  1,  7,  5,  11, 3,  14, 10, 0,  6,  13},
    // S2
    {15, 1,  8,  14, 6,  11, 3,  4,  9,  7,  2,  13, 12, 0,  5,  10,
     3,  13, 4,  7,  15, 2,  8,  14, 12, 0,  1,  10, 6,  9,  11, 5,
     0,  14, 7,  11, 10, 4,  13, 1,  5,  8,  12, 6,  9,  3,  2,  15,
     13, 8,  10, 1,  3,  15, 4,  2,  11, 6,  7,  12, 0,  5,  14, 9},
    // S3
    {10, 0,  9,  14, 6,  3,  15, 5,  1,  13, 12, 7,  11, 4,  2,  8,
     13, 7,  0,  9,  3,  4,  6,  10, 2,  8,  5,  14, 12, 11, 15, 1,
     13, 6,  4,  9,  8,  15, 3,  0,  11, 1,  2,  12, 5,  10, 14, 7,
     1,  10, 13, 0,  6,  9,  8,  7,  4,  15, 14, 3,  11, 5,  2,  12},
    // S4
    {7,  13, 14, 3,  0,  6,  9,  10, 1,  2,  8,  5,  11, 12, 4,  15,
     13, 8,  11, 5,  6,  15, 0,  3,  4,  7,  2,  12, 1,  10, 14, 9,
     10, 6,  9,  0,  12, 11, 7,  13, 15, 1,  3,  14, 5,  2,  8,  4,
     3,  15, 0,  6,  10, 1,  13, 8,  9,  4,  5,  11, 12, 7,  2,  14},
    // S5
    {2,  12, 4,  1,  7,  10, 11, 6,  8,  5,  3,  15, 13, 0,  14, 9,
     14, 11, 2,  12, 4,  7,  13, 1,  5,  0,  15, 10, 3,  9,  8,  6,
     4,  2,  1,  11, 10, 13, 7,  8,  15, 9,  12, 5,  6,  3,  0,  14,
     11, 8,  12, 7,  1,  14, 2,  13, 6,  15, 0,  9,  10, 4,  5,  3},
    // S6
    {12, 1,  10, 15, 9,  2,  6,  8,  0,  13, 3,  4,  14, 7,  5,  11,
     10, 15, 4,  2,  7,  12, 9,  5,  6,  1,  13, 14, 0,  11, 3,  8,
     9,  14, 15, 5,  2,  8,  12, 3,  7,  0,  4,  10, 1,  13, 11, 6,
     4,  3,  2,  12, 9,  5,  15, 10, 11, 14, 1,  7,  6,  0,  8,  13},
    // S7
    {4,  11, 2,  14, 15, 0,  8,  13, 3,  12, 9,  7,  5,  10, 6,  1,
     13, 0,  11, 7,  4,  9,  1,  10, 14, 3,  5,  12, 2,  15, 8,  6,
     1,  4,  11, 13, 12, 3,  7,  14, 10, 15, 6,  8,  0,  5,  9,  2,
     6,  11, 13, 8,  1,  4,  10, 7,  9,  5,  0,  15, 14, 2,  3,  12},
    // S8
    {13, 2,  8,  4,  6,  15, 11, 1,  10, 9,  3,  14, 5,  0,  12, 7,
     1,  15, 13, 8,  10, 3,  7,  4,  12, 5,  6,  11, 0,  14, 9,  2,
     7,  11, 4,  1,  9,  12, 14, 2,  0,  6,  10, 13, 15, 3,  5,  8,
     2,  1,  14, 7,  4,  10, 8,  13, 15, 12, 9,  0,  3,  5,  6,  11}
};

// ====================================================================================================
// Реализация методов класса DES (вспомогательного для TDES)
// ====================================================================================================

// Вспомогательная функция для перестановки битов
// input: входное 64-битное значение
// permutationTable: таблица перестановок
// inputBits: количество бит во входном значении (например, 64 для данных, 56 для ключа)
// outputBits: количество бит в выходном значении
uint64_t TDES::DES::permute(uint64_t input, const uint8_t* permutationTable, int inputBits, int outputBits) {
    uint64_t output = 0;
    for (int i = 0; i < outputBits; ++i) {
        // Получаем позицию бита из таблицы перестановок (таблицы основаны на 1, поэтому -1)
        int bitPos = permutationTable[i] - 1;
        // Проверяем, установлен ли бит во входном значении
        if ((input >> (inputBits - 1 - bitPos)) & 1) {
            // Устанавливаем бит в выходном значении
            output |= (1ULL << (outputBits - 1 - i));
        }
    }
    return output;
}

// Вспомогательная функция для циклического сдвига влево (для генерации ключей)
void TDES::DES::rotateLeft(uint32_t* value, int bits) {
    *value = (*value << bits) | (*value >> (28 - bits)); // 28 - т.к. 56-битный ключ делится на две 28-битные части
    *value &= 0x0FFFFFFF; // Обрезаем до 28 бит
}

// Генерация 16 раундовых ключей
void TDES::DES::generateSubkeys(const uint8_t key[8]) {
    uint64_t initialKey = 0;
    // Преобразуем 8 байт ключа в 64-битное значение
    for (int i = 0; i < 8; ++i) {
        initialKey = (initialKey << 8) | key[i];
    }

    // Применяем PC1 для получения 56-битного ключа
    uint64_t permutedKey = permute(initialKey, PC1, 64, 56);

    // Разделяем на две 28-битные половины C0 и D0
    uint32_t C = (uint32_t)((permutedKey >> 28) & 0x0FFFFFFF);
    uint32_t D = (uint32_t)(permutedKey & 0x0FFFFFFF);

    for (int i = 0; i < 16; ++i) {
        // Выполняем циклический сдвиг
        rotateLeft(&C, ShiftBits[i]);
        rotateLeft(&D, ShiftBits[i]);

        // Объединяем C и D, затем применяем PC2 для получения 48-битного раундового ключа
        uint64_t CD_combined = ((uint64_t)C << 28) | D;
        subkeys[i] = permute(CD_combined, PC2, 56, 48);
    }
}

// Функция Feistel (F-функция)
uint32_t TDES::DES::feistelFunction(uint32_t rightHalf, uint64_t subkey) {
    // 1. Расширение R до 48 бит (E-Permutation)
    uint64_t expandedR = permute(rightHalf, E_Permutation, 32, 48);

    // 2. XOR с раундовым ключом
    uint64_t xorResult = expandedR ^ subkey;

    // 3. Пропускаем через S-блоки
    uint32_t sboxOutput = 0;
    for (int i = 0; i < 8; ++i) {
        // Извлекаем 6 бит для текущего S-блока
        uint8_t sixBits = (uint8_t)((xorResult >> (48 - (i + 1) * 6)) & 0x3F); // 0x3F = 111111b

        // Определяем строку и столбец для S-блока
        uint8_t row = (sixBits & 0x20) >> 4 | (sixBits & 0x01); // Бит 0 и Бит 5
        uint8_t col = (sixBits >> 1) & 0x0F; // Биты 1-4

        // Получаем значение из S-блока
        uint8_t sboxValue = S_Boxes[i][row * 16 + col]; // S-блоки 4x16

        // Добавляем значение в выход
        sboxOutput = (sboxOutput << 4) | sboxValue;
    }

    // 4. Пропускаем через P-перестановку
    return (uint32_t)permute(sboxOutput, P_Permutation, 32, 32);
}

// Конструктор DES
TDES::DES::DES(const uint8_t key[8]) {
    generateSubkeys(key);
}

// Шифрование DES-блока
void TDES::DES::encryptBlock(uint8_t data[8]) {
    uint64_t block = 0;
    for (int i = 0; i < 8; ++i) {
        block = (block << 8) | data[i];
    }

    // Начальная перестановка
    block = permute(block, IP, 64, 64);

    // Разделяем на левую и правую половины
    uint32_t L = (uint32_t)(block >> 32);
    uint32_t R = (uint32_t)(block & 0xFFFFFFFF);

    // 16 раундов
    for (int i = 0; i < 16; ++i) {
        uint32_t tempL = R;
        R = L ^ feistelFunction(R, subkeys[i]);
        L = tempL;
    }

    // Объединяем L и R (поменяем местами L и R после последнего раунда)
    uint64_t combined = ((uint64_t)R << 32) | L; // Обрати внимание, L и R меняются местами на выходе

    // Конечная перестановка
    block = permute(combined, FP, 64, 64);

    // Копируем результат обратно в массив байт
    for (int i = 7; i >= 0; --i) {
        data[i] = (uint8_t)(block & 0xFF);
        block >>= 8;
    }
}

// Дешифрование DES-блока
void TDES::DES::decryptBlock(uint8_t data[8]) {
    uint64_t block = 0;
    for (int i = 0; i < 8; ++i) {
        block = (block << 8) | data[i];
    }

    // Начальная перестановка
    block = permute(block, IP, 64, 64);

    // Разделяем на левую и правую половины
    uint32_t L = (uint32_t)(block >> 32);
    uint32_t R = (uint32_t)(block & 0xFFFFFFFF);

    // 16 раундов в обратном порядке (используем ключи с конца к началу)
    for (int i = 15; i >= 0; --i) {
        uint32_t tempL = R;
        R = L ^ feistelFunction(R, subkeys[i]);
        L = tempL;
    }

    // Объединяем L и R (поменяем местами L и R после последнего раунда)
    uint64_t combined = ((uint64_t)R << 32) | L; // Обрати внимание, L и R меняются местами на выходе

    // Конечная перестановка
    block = permute(combined, FP, 64, 64);

    // Копируем результат обратно в массив байт
    for (int i = 7; i >= 0; --i) {
        data[i] = (uint8_t)(block & 0xFF);
        block >>= 8;
    }
}

// ====================================================================================================
// Реализация методов класса TDES (обновлено для режимов работы и padding'а)
// ====================================================================================================

// Конструктор TDES
TDES::TDES(const uint8_t key1[8], const uint8_t key2[8], const uint8_t key3[8])
    : des1(key1), des2(key2), des3(key3) {
    // Инициализация буферов нулями
    utilities::fast_memset(blockBuffer, 0, BLOCK_SIZE);
    utilities::fast_memset(ivBuffer, 0, BLOCK_SIZE);
    utilities::fast_memset(counterBuffer, 0, BLOCK_SIZE);
}

// Вспомогательная функция TDES шифрования одного блока (EDE)
void TDES::tdesEncryptBlock(uint8_t data[8]) {
    des1.encryptBlock(data);
    des2.decryptBlock(data);
    des3.encryptBlock(data);
}

// Вспомогательная функция TDES дешифрования одного блока (DED)
void TDES::tdesDecryptBlock(uint8_t data[8]) {
    des3.decryptBlock(data);
    des2.encryptBlock(data);
    des1.decryptBlock(data);
}

// --- Padding Functions ---

Size TDES::addPKCS7Padding(uint8_t* data, Size currentLen) {
    Size paddingBytes = BLOCK_SIZE - (currentLen % BLOCK_SIZE);
    if (paddingBytes == 0) { // Если данные уже кратны BLOCK_SIZE, добавляем целый блок
        paddingBytes = BLOCK_SIZE;
    }
    for (Size i = 0; i < paddingBytes; ++i) {
        data[currentLen + i] = (uint8_t)paddingBytes;
    }
    return currentLen + paddingBytes;
}

Size TDES::removePKCS7Padding(uint8_t* data, Size currentLen) {
    if (currentLen == 0 || currentLen % BLOCK_SIZE != 0) {
        // Ошибка: неверная длина данных для удаления padding'а
        return 0; // Или бросить исключение
    }
    uint8_t paddingBytes = data[currentLen - 1];
    if (paddingBytes == 0 || paddingBytes > BLOCK_SIZE) {
        // Ошибка: неверное значение padding'а
        return 0; // Или бросить исключение
    }
    // Проверяем, что все padding-байты имеют правильное значение
    for (Size i = 0; i < paddingBytes; ++i) {
        if (data[currentLen - 1 - i] != paddingBytes) {
            // Ошибка: невалидный padding
            return 0; // Или бросить исключение
        }
    }
    return currentLen - paddingBytes;
}

// --- Main Encryption/Decryption Methods ---

Size TDES::encrypt(const uint8_t* input, Size inputLen, uint8_t* output, Mode mode, const uint8_t ivOrNonce[8]) {
    Size paddedLen = inputLen + (BLOCK_SIZE - (inputLen % BLOCK_SIZE));
    if (inputLen % BLOCK_SIZE == 0) {
        paddedLen = inputLen + BLOCK_SIZE; // PKCS#7: если кратно, добавляем целый блок
    }

    // Проверка на размер выходного буфера, если MAX_DATA_SIZE не определен
    // Для микроконтроллеров обычно предполагается, что буферы заданы статически
    // и достаточно велики, или же передаются явно вызывающей стороной.
    // Если MAX_DATA_SIZE определен, убедимся, что inputLen не превышает лимит.
    if (inputLen > MAX_DATA_SIZE) {
        // Ошибка: входные данные слишком велики для буфера
        return 0;
    }

    // Копируем входные данные во временный буфер и добавляем padding
    uint8_t tempBuffer[MAX_DATA_SIZE + BLOCK_SIZE]; // Достаточно большой для данных + padding
    utilities::fast_memcpy(tempBuffer, input, inputLen);
    paddedLen = addPKCS7Padding(tempBuffer, inputLen);

    Size numBlocks = paddedLen / BLOCK_SIZE;
    Size currentOffset = 0;

    if (ivOrNonce) {
        utilities::fast_memcpy(ivBuffer, ivOrNonce, BLOCK_SIZE); // Копируем IV/Nonce
        utilities::fast_memcpy(counterBuffer, ivOrNonce, BLOCK_SIZE); // Для CTR, копируем начальный счетчик
    } else if (mode != Mode::ECB) {
        // Если IV/Nonce не передан, а режим требует его, это ошибка
        return 0; // Или можно сгенерировать случайный IV, но это не для МК обычно
    }

    for (Size i = 0; i < numBlocks; ++i) {
        utilities::fast_memcpy(blockBuffer, tempBuffer + currentOffset, BLOCK_SIZE); // Копируем текущий блок

        switch (mode) {
            case Mode::ECB:
                tdesEncryptBlock(blockBuffer);
                break;
            case Mode::CBC:
                // XOR с предыдущим зашифрованным блоком (или IV для первого)
                for (int j = 0; j < BLOCK_SIZE; ++j) {
                    blockBuffer[j] ^= ivBuffer[j];
                }
                tdesEncryptBlock(blockBuffer);
                utilities::fast_memcpy(ivBuffer, blockBuffer, BLOCK_SIZE); // Обновляем IV
                break;
            case Mode::CTR:
                // Шифруем счетчик
                tdesEncryptBlock(counterBuffer);
                // XOR с открытым текстом
                for (int j = 0; j < BLOCK_SIZE; ++j) {
                    blockBuffer[j] ^= counterBuffer[j];
                }
                // Увеличиваем счетчик
                // Простая инкрементация (возможны более сложные для больших счетчиков)
                for (int j = BLOCK_SIZE - 1; j >= 0; --j) {
                    if (++ivBuffer[j] != 0) break;
                }
                utilities::fast_memcpy(counterBuffer, ivBuffer, BLOCK_SIZE); // Обновляем счетчик для следующего раунда
                break;
        }
        utilities::fast_memcpy(output + currentOffset, blockBuffer, BLOCK_SIZE); // Копируем результат
        currentOffset += BLOCK_SIZE;
    }
    return paddedLen;
}

Size TDES::decrypt(const uint8_t* input, Size inputLen, uint8_t* output, Mode mode, const uint8_t ivOrNonce[8]) {
    if (inputLen == 0 || inputLen % BLOCK_SIZE != 0) {
        // Ошибка: длина входных данных должна быть кратна размеру блока
        return 0;
    }

    // Проверка на размер входного буфера, если MAX_DATA_SIZE не определен
    if (inputLen > MAX_DATA_SIZE + BLOCK_SIZE) { // MAX_DATA_SIZE + BLOCK_SIZE для учёта padding'а
        // Ошибка: входные данные слишком велики для буфера
        return 0;
    }

    Size numBlocks = inputLen / BLOCK_SIZE;
    Size currentOffset = 0;

    // Временный буфер для дешифрованных данных (перед удалением padding'а)
    uint8_t tempDecryptedBuffer[MAX_DATA_SIZE + BLOCK_SIZE];

    if (ivOrNonce) {
        utilities::fast_memcpy(ivBuffer, ivOrNonce, BLOCK_SIZE); // Копируем IV/Nonce
        utilities::fast_memcpy(counterBuffer, ivOrNonce, BLOCK_SIZE); // Для CTR, копируем начальный счетчик
    } else if (mode != Mode::ECB) {
        return 0; // Ошибка: IV/Nonce не передан
    }

    for (Size i = 0; i < numBlocks; ++i) {
        utilities::fast_memcpy(blockBuffer, input + currentOffset, BLOCK_SIZE); // Копируем текущий блок

        switch (mode) {
            case Mode::ECB:
                tdesDecryptBlock(blockBuffer);
                break;
            case Mode::CBC: {
                uint8_t tempBlock[BLOCK_SIZE]; // Сохраняем текущий зашифрованный блок
                utilities::fast_memcpy(tempBlock, blockBuffer, BLOCK_SIZE);
                tdesDecryptBlock(blockBuffer);
                // XOR с предыдущим зашифрованным блоком (или IV для первого)
                for (int j = 0; j < BLOCK_SIZE; ++j) {
                    blockBuffer[j] ^= ivBuffer[j];
                }
                utilities::fast_memcpy(ivBuffer, tempBlock, BLOCK_SIZE); // Обновляем IV
                break;
            }
            case Mode::CTR:
                // Шифруем счетчик (то же, что и при шифровании)
                tdesEncryptBlock(counterBuffer);
                // XOR с зашифрованным текстом
                for (int j = 0; j < BLOCK_SIZE; ++j) {
                    blockBuffer[j] ^= counterBuffer[j];
                }
                // Увеличиваем счетчик
                for (int j = BLOCK_SIZE - 1; j >= 0; --j) {
                    if (++ivBuffer[j] != 0) break;
                }
                utilities::fast_memcpy(counterBuffer, ivBuffer, BLOCK_SIZE); // Обновляем счетчик для следующего раунда
                break;
        }
        utilities::fast_memcpy(tempDecryptedBuffer + currentOffset, blockBuffer, BLOCK_SIZE); // Копируем результат
        currentOffset += BLOCK_SIZE;
    }

    // Удаляем padding после дешифрования
    Size finalLen = removePKCS7Padding(tempDecryptedBuffer, currentOffset);
    if (finalLen == 0 && inputLen > 0) { // Если padding был некорректен
        return 0;
    }
    utilities::fast_memcpy(output, tempDecryptedBuffer, finalLen); // Копируем окончательные данные
    return finalLen;
}
}