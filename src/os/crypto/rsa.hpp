#pragma once
#include <random>
#include <chrono>
class RSA {
public:
    // Конструктор
    RSA() : n(0), e(0), d(0) {
        // Инициализация генератора случайных чисел один раз
        rng_prime.seed(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        rng_e.seed(std::chrono::high_resolution_clock::now().time_since_epoch().count() + 1); // Добавляем смещение для другого сида
    }

    // Метод для генерации ключей
    void generateKeys(long long min_prime_val, long long max_prime_val) {
        long long p, q;
        do {
            p = generatePrime(min_prime_val, max_prime_val);
            q = generatePrime(min_prime_val, max_prime_val);
        } while (p == q); // Убеждаемся, что p и q различны

        n = p * q;
        long long phi = (p - 1) * (q - 1);

        e = 65537; // Общепринятое значение для e.
        // Проверяем, что e взаимно просто с phi и 1 < e < phi
        while (e >= phi || gcd(e, phi) != 1) {
            // Если 65537 не подходит, генерируем другое e в диапазоне [2, phi-1]
            std::uniform_int_distribution<long long> dist_e(2, phi - 1);
            e = dist_e(rng_e);
        }
        
        d = modInverse(e, phi);
    }

    // Метод для шифрования сообщения
    long long encrypt(long long message) const {
        return power(message, e, n);
    }

    // Метод для дешифрования сообщения
    long long decrypt(long long encrypted_message) const {
        return power(encrypted_message, d, n);
    }

    // Геттеры для ключей (для вывода и отладки)
    long long getN() const { return n; }
    long long getE() const { return e; }
    long long getD() const { return d; }

private:
    long long n; // Модуль (общая часть для открытого и закрытого ключа)
    long long e; // Открытая экспонента
    long long d; // Закрытая экспонента

    // Генераторы случайных чисел, члены класса для инициализации один раз
    std::mt19937_64 rng_prime; // Для генерации простых чисел
    std::mt19937_64 rng_e;     // Для генерации e

    // 1. Наибольший общий делитель (GCD)
    long long gcd(long long a, long long b) const {
        while (b) {
            a %= b;
            std::swap(a, b);
        }
        return a;
    }

    // 2. Модульное возведение в степень (Modular Exponentiation)
    long long power(long long base, long long exp, long long mod) const {
        long long res = 1;
        base %= mod;
        while (exp > 0) {
            if (exp % 2 == 1) res = (res * base) % mod;
            base = (base * base) % mod;
            exp /= 2;
        }
        return res;
    }

    // 3. Расширенный алгоритм Евклида для нахождения модульного обратного
    long long modInverse(long long a, long long m) const {
        long long m0 = m;
        long long y = 0, x = 1;

        if (m == 1) return 0;

        while (a > 1) {
            long long q = a / m;
            long long t = m;

            m = a % m, a = t;
            t = y;

            y = x - q * y;
            x = t;
        }

        if (x < 0) x += m0;
        return x;
    }

    // 4. Тест Миллера-Рабина для проверки на простоту
    bool isPrime(long long n, int k = 5) const {
        if (n <= 1 || n == 4) return false;
        if (n <= 3) return true;

        long long d = n - 1;
        while (d % 2 == 0) {
            d /= 2;
        }

        std::uniform_int_distribution<long long> dist(2, n - 2);

        for (int i = 0; i < k; ++i) {
            long long a = dist(rng_prime); 
            long long x = power(a, d, n);

            if (x == 1 || x == n - 1) continue;

            while (d != n - 1) {
                x = (x * x) % n;
                d *= 2;

                if (x == 1) return false;
                if (x == n - 1) break;
            }
            if (x != n - 1) return false;
        }
        return true;
    }

    // 5. Генерация случайного простого числа
    long long generatePrime(long long min_val, long long max_val) {
        std::uniform_int_distribution<long long> dist(min_val, max_val);
        long long p;
        do {
            p = dist(rng_prime);
            if (p % 2 == 0 && p != 2) p++; // Убедимся, что число нечетное, если не 2
        } while (!isPrime(p));
        return p;
    }
};