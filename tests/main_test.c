// ============================================================
// ТЕСТИРОВАНИЕ АЛЛОКАТОРОВ
// ============================================================
// Этот файл содержит стратегию тестирования для сравнения двух алгоритмов.
// Мы измеряем три ключевые характеристики:
// 1. Скорость выделения (Allocation Speed)
// 2. Скорость освобождения (Free Speed)
// 3. Фактор использования памяти (Fragmentation / Efficiency)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "allocator.h"

#define MEMORY_SIZE (10 * 1024 * 1024)

// Функция для получения точного времени (в секундах).
// Используем CLOCK_MONOTONIC, так как оно не скачет при смене времени на
// компьютере.
double get_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void run_tests(const char* name, Allocator* (*create_func)(void*, size_t)) {
    printf("================================================\n");
    printf("Testing Allocator: %s\n", name);
    printf("================================================\n");

   
    void* real_memory = malloc(MEMORY_SIZE);

    Allocator* alloc = create_func(real_memory, MEMORY_SIZE);

    if (!alloc) {
        printf("Failed to create allocator (not enough memory?)\n");
        free(real_memory);
        return;
    }

    // ------------------------------------------------------------
    // ТЕСТ 1: Скорость массового выделения (Allocation Speed)
    // ------------------------------------------------------------
    // Мы выделяем 10 000 блоков по 64 байта.
    // Это имитирует создание множества мелких объектов (например, узлов
    // списка).

    double start = get_time();
    int N = 10000;
    void** ptrs = malloc(
        N * sizeof(void*));  // Массив для хранения указателей, чтобы потом освободить

    for (int i = 0; i < N; i++) {
        ptrs[i] = allocator_alloc(alloc, 64);
    }
    double end = get_time();
    printf("[TEST 1] Allocation Speed:\n");
    printf("  Allocated %d blocks of 64 bytes.\n", N);
    printf("  Time taken: %.6f seconds\n", end - start);

    // ------------------------------------------------------------
    // ТЕСТ 2: Скорость массового освобождения (Free Speed)
    // ------------------------------------------------------------

    start = get_time();
    for (int i = 0; i < N; i++) {
        allocator_free(alloc, ptrs[i]);
    }
    end = get_time();
    printf("[TEST 2] Free Speed:\n");
    printf("  Freed %d blocks.\n", N);
    printf("  Time taken: %.6f seconds\n", end - start);

    // ------------------------------------------------------------
    // ТЕСТ 3: Фрагментация и эффективность (Fragmentation)
    // ------------------------------------------------------------
    // Попробуем выделить блоки случайного размера вперемешку.
    // Это самый сложный сценарий для аллокаторов.

    printf("[TEST 3] Fragmentation Stress Test:\n");
    int M = 5000;
    size_t allocated_bytes = 0;
    int success_count = 0;

    for (int i = 0; i < M; i++) {
        size_t sz = (rand() % 128) + 16;  // Случайный размер от 16 до 144 байт
        ptrs[i] = allocator_alloc(alloc, sz);
        if (ptrs[i]) {
            allocated_bytes += sz;
            success_count++;
        }
    }
    printf("  Requested allocation of %d random blocks.\n", M);
    printf("  Successfully allocated: %d blocks\n", success_count);
    printf("  Total user bytes: %zu\n", allocated_bytes);

    // Попытка выделить ОЧЕНЬ большой кусок (1 МБ) после "мусора".
    // Если память сильно фрагментирована (много дырок), это не получится,
    // даже если суммарно свободного места хватает.
    void* big_chunk = allocator_alloc(alloc, 1024 * 1024);
    if (big_chunk) {
        printf("  Big chunk (1MB) allocation: SUCCESS (Low fragmentation)\n");
        allocator_free(alloc, big_chunk);
    } else {
        printf("  Big chunk (1MB) allocation: FAILED (High fragmentation)\n");
    }

    // Чистка
    free(ptrs);
    allocator_destroy(alloc);
    free(real_memory);
    printf("------------------------------------------------\n\n");
}

// Точка входа.
// В зависимости от флага компиляции (USE_BUDDY или USE_MK)
int main() {
#ifdef USE_BUDDY
    printf("=== Running Tests for BUDDY Allocator ===\n");
#elif defined(USE_MK)
    printf("=== Running Tests for MK Allocator ===\n");
#else
    printf("=== Running Tests for LIST Allocator (First Fit) ===\n");
#endif

    // Запускаем тесты, передавая функцию создания аллокатора
    run_tests("Current Implementation", allocator_create);

    return 0;
}