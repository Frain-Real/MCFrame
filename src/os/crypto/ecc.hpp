#pragma once

#include "../common.hpp"
#include "../utilities.hpp"

namespace embed {
// Максимальный размер для больших чисел (например, для 32-битного поля, это может быть 1)
// Если вы используете, например, 256-битное поле, то это будет 256/32 = 8
#define BIGINT_WORD_SIZE 8

// Класс для работы с арифметикой в конечном поле GF(p)
class FiniteFieldArithmetic {
public:
    // Модуль поля P
    uint32_t P;

    FiniteFieldArithmetic(uint32_t modulus) : P(modulus) {}

    // Сложение по модулю
    uint32_t add(uint32_t a, uint32_t b) {
        uint64_t sum = (uint64_t)a + b;
        return sum % P;
    }

    // Вычитание по модулю
    uint32_t sub(uint32_t a, uint32_t b) {
        if (a >= b) {
            return (a - b) % P;
        } else {
            return (P - (b - a)) % P;
        }
    }

    // Умножение по модулю
    uint32_t mul(uint32_t a, uint32_t b) {
        uint64_t product = (uint64_t)a * b;
        return product % P;
    }

    // Быстрое возведение в степень по модулю (a^exp % P)
    uint32_t power(uint32_t base, uint32_t exp) {
        uint32_t res = 1;
        base %= P;
        while (exp > 0) {
            if (exp % 2 == 1) { // Если exp нечетное
                res = mul(res, base);
            }
            base = mul(base, base);
            exp /= 2;
        }
        return res;
    }

    // Расширенный алгоритм Евклида для нахождения мультипликативной инверсии
    // Возвращает x такое, что (a * x) % P == 1
    uint32_t inverse(uint32_t a) {
        int32_t m = P;
        int32_t n = a;
        int32_t t = 0, q = 1;
        int32_t r = m;
        int32_t s = n;

        while (s != 0) {
            int32_t quotient = r / s;
            int32_t temp_r = r;
            int32_t temp_t = t;
            r = s;
            t = q;
            s = temp_r - quotient * s;
            q = temp_t - quotient * q;
        }
        if (r > 1) return 0; // Инверсия не существует
        if (t < 0) t = t + m; // Приводим к положительному значению

        return (uint32_t)t;
    }
};
    // Структура для представления точки на эллиптической кривой
struct Point {
    uint32_t x;
    uint32_t y;
    bool isInfinity; // Флаг, указывающий, является ли точка "точкой на бесконечности"

    // Конструктор для обычной точки
    Point(uint32_t val_x = 0, uint32_t val_y = 0) : x(val_x), y(val_y), isInfinity(false) {}

    // Конструктор для точки на бесконечности
    static Point infinity() {
        Point p;
        p.isInfinity = true;
        return p;
    }

    bool operator==(const Point& other) const {
        if (isInfinity && other.isInfinity) return true;
        if (isInfinity != other.isInfinity) return false;
        return (x == other.x && y == other.y);
    }

    bool operator!=(const Point& other) const {
        return !(*this == other);
    }
};
// Класс для работы с эллиптическими кривыми
class ECC {
public:
    uint32_t P; // Модуль поля
    uint32_t A; // Коэффициент A кривой y^2 = x^3 + Ax + B (mod P)
    uint32_t B; // Коэффициент B кривой y^2 = x^3 + Ax + B (mod P)
    Point G;    // Базовая точка (генератор) кривой
    uint32_t N; // Порядок базовой точки G (наименьшее целое N > 0 такое, что N*G = O (точка на бесконечности))

    FiniteFieldArithmetic ff;

    ECC(uint32_t modulus, uint32_t coeffA, uint32_t coeffB,
        Point generator, uint32_t orderOfG)
        : P(modulus), A(coeffA), B(coeffB), G(generator), N(orderOfG), ff(modulus)
    {
        // Проверка условия 4A^3 + 27B^2 != 0 (mod P)
        uint32_t term1 = ff.mul(ff.mul(ff.power(A, 3), 4), 1); // 4 * A^3
        uint32_t term2 = ff.mul(ff.mul(ff.power(B, 2), 27), 1); // 27 * B^2
        uint32_t discriminant = ff.add(term1, term2);

        if (discriminant == 0) {
            // В реальном приложении здесь нужно выдать ошибку или исключение
            // Для микроконтроллера можно просто зависнуть или войти в бесконечный цикл
            // или использовать assert
            // Для простоты примера, мы просто выведем сообщение, но это не безопасно.
            // Вместо этого нужно убедиться, что используемые параметры кривой корректны.
            // printf("Error: Curve parameters are invalid (4A^3 + 27B^2 == 0 mod P)\n");
        }
    }

    // Проверка, лежит ли точка на кривой
    bool isOnCurve(const Point& p) {
        if (p.isInfinity) return true;
        uint32_t lhs = ff.mul(p.y, p.y); // y^2
        uint32_t rhs = ff.add(ff.add(ff.mul(ff.mul(p.x, p.x), p.x), ff.mul(A, p.x)), B); // x^3 + Ax + B
        return lhs == rhs;
    }

    // Сложение двух точек P1 и P2
    Point addPoints(const Point& p1, const Point& p2) {
        if (p1.isInfinity) return p2;
        if (p2.isInfinity) return p1;

        // Если P1 == -P2, то P1 + P2 = O (точка на бесконечности)
        // То есть P1.x == P2.x и P1.y == P - P2.y
        if (p1.x == p2.x && ff.add(p1.y, p2.y) == 0) {
            return Point::infinity();
        }

        uint32_t lambda;
        if (p1.x == p2.x && p1.y == p2.y) { // Удвоение точки P1 == P2
            // lambda = (3x^2 + A) * (2y)^-1 (mod P)
            uint32_t three_x_squared = ff.mul(ff.mul(p1.x, p1.x), 3);
            uint32_t numerator = ff.add(three_x_squared, A);
            uint32_t two_y_inv = ff.inverse(ff.mul(2, p1.y));
            lambda = ff.mul(numerator, two_y_inv);
        } else { // Сложение разных точек
            // lambda = (y2 - y1) * (x2 - x1)^-1 (mod P)
            uint32_t numerator = ff.sub(p2.y, p1.y);
            uint32_t denominator_inv = ff.inverse(ff.sub(p2.x, p1.x));
            lambda = ff.mul(numerator, denominator_inv);
        }

        uint32_t x3 = ff.sub(ff.sub(ff.mul(lambda, lambda), p1.x), p2.x);
        uint32_t y3 = ff.sub(ff.mul(lambda, ff.sub(p1.x, x3)), p1.y);

        return Point(x3, y3);
    }

    // Умножение точки на скаляр (k * P)
    Point scalarMultiply(uint32_t k, const Point& p) {
        Point result = Point::infinity(); // Инициализируем результатом как точка на бесконечности
        Point current = p;

        while (k > 0) {
            if (k & 1) { // Если младший бит k равен 1
                result = addPoints(result, current);
            }
            current = addPoints(current, current); // Удваиваем current (current = 2 * current)
            k >>= 1; // Сдвигаем k вправо на 1 бит (делим на 2)
        }
        return result;
    }

    // Генерация случайного закрытого ключа (для примера, используем rand(), в реальной системе нужен CSPRNG)
    uint32_t generatePrivateKey() {
        // Закрытый ключ должен быть в диапазоне [1, N-1]
        // Для микроконтроллера, где нет хорошего RNG, это проблема.
        // Здесь используется rand() из cstdlib, что НЕБЕЗОПАСНО для криптографии.
        // Вам нужно будет реализовать или использовать аппаратный генератор случайных чисел.
        return (utilities::rand() % (N - 1)) + 1;
    }

    // Вычисление открытого ключа из закрытого
    Point getPublicKey(uint32_t privateKey) {
        return scalarMultiply(privateKey, G);
    }

    // Вычисление общего секрета ECDH
    Point calculateSharedSecret(uint32_t privateKey, const Point& publicKeyOther) {
        // S = privateKey * publicKeyOther
        return scalarMultiply(privateKey, publicKeyOther);
    }
};

}