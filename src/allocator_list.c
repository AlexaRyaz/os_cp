#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "allocator.h"


// ============================================================
// АЛГОРИТМ 1: СПИСКИ СВОБОДНЫХ БЛОКОВ (FIRST FIT)
// ============================================================
// ПОДРОБНОЕ ОПИСАНИЕ РАБОТЫ:
//
// 1. Структура памяти:
//    Вся память, которую нам дали, рассматривается как один большой "пирог".
//    Мы нарезаем этот пирог на кусочки (блоки).
//    У каждого кусочка есть "этикетка" (BlockHeader), которая лежит прямо перед
//    ним.
//
//    [Header | Данные пользователя] [Header | Данные пользователя] [Header |
//    Свободное место...]
//
// 2. Заголовок (BlockHeader):
//    В нем мы храним:
//    - size: размер данных.
//    - is_free: занят этот кусок или свободен.
//    - next: стрелочка на следующий кусок в памяти.
//
// 3. Выделение памяти (alloc):
//    Мы используем стратегию "First Fit" (Первый подходящий).
//    Мы идем по списку блоков с самого начала:
//    - Этот свободен? Да.
//    - Его размер подходит? Нет, мал. Идем дальше.
//    - Этот свободен? Да.
//    - Его размер подходит? Да, он огромный!
//    - Тогда мы его "расщепляем" (Split):
//      Отрезаем от него нужный кусок, помечаем как занятый.
//      Оставшийся хвост превращаем в новый свободный блок.
//
// 4. Освобождение (free):
//    Мы просто находим заголовок блока и ставим флажок is_free = true.
//    (В продвинутой версии соседние свободные блоки надо склеивать, чтобы не
//    было "дырок").

// Минимальный размер блока (заголовок + данные)
#define MIN_BLOCK_SIZE 16

// Заголовок блока памяти.
// Он лежит в памяти ПРЯМО ПЕРЕД указателем, который мы отдаем пользователю.
typedef struct BlockHeader {
    size_t size;  // Размер полезных данных в этом блоке
    bool is_free;  // Флаг: true - блок свободен, false - занят
    struct BlockHeader*
        next;  // Указатель на следующий блок в физической памяти
} BlockHeader;

// Структура самого аллокатора.
// Она лежит в самом начале выделенной нам памяти.
struct Allocator {
    void* memory_start;  // Адрес начала всей памяти
    size_t total_size;   // Общий размер памяти
    BlockHeader* head;  // Указатель на первый блок в списке
};

// --- Реализация интерфейса ---

// Функция создания аллокатора.
// Превращает сырой кусок памяти в структурированный список.
Allocator* allocator_create(void* realMemory, size_t memory_size) {
    // Проверяем, хватит ли места хотя бы для служебной структуры
    if (memory_size < sizeof(Allocator) + sizeof(BlockHeader)) {
        return NULL;
    }

    // 1. Кладем структуру Allocator в самое начало памяти.
    Allocator* allocator = (Allocator*)realMemory;
    allocator->memory_start = realMemory;
    allocator->total_size = memory_size;

    // 2. Сразу за структурой Allocator создаем ПЕРВЫЙ блок.
    // Изначально это один гигантский свободный блок, занимающий всё остальное
    // место.

    // Используем арифметику указателей: (uint8_t*) позволяет прибавлять байты.
    void* first_block_addr = (uint8_t*)realMemory + sizeof(Allocator);

    BlockHeader* first_block = (BlockHeader*)first_block_addr;

    // Вычисляем размер: Всё место минус размер самой структуры Allocator
    first_block->size = memory_size - sizeof(Allocator) - sizeof(BlockHeader);
    first_block->is_free = true;  // Он свободен
    first_block->next = NULL;     // Дальше блоков пока нет

    // Сохраняем указатель на этот блок как на "голову" списка
    allocator->head = first_block;

    return allocator;
}

// Функция выделения памяти.
void* allocator_alloc(Allocator* allocator, size_t size) {
    if (!allocator || size == 0) return NULL;

    // 1. Выравнивание (Alignment).
    // Процессоры работают быстрее, если данные лежат по адресам, кратным 4
    // или 8. Если пользователь просит 1 байт, мы все равно выделим 8 (или
    // больше), чтобы не ломать выравнивание.
    size_t aligned_size = size;
    if (aligned_size % 8 != 0) {
        aligned_size += 8 - (aligned_size % 8);
    }

    // 2. Поиск свободного блока (First Fit).
    BlockHeader* current = allocator->head;
    while (current != NULL) {
        // Нам нужен блок, который:
        // а) Свободен (is_free == true)
        // б) Вмещает наши данные (current->size >= aligned_size)
        if (current->is_free && current->size >= aligned_size) {
            // УРА! НАШЛИ!

            // 3. Попытка расщепления (Splitting).
            // Если блок слишком большой (например, просили 10 байт, а блок 1000
            // байт), глупо отдавать весь блок. Отрежем от него кусочек.

            // Проверяем, останется ли место для еще одного блока (заголовок +
            // хотя бы чуть-чуть данных)
            if (current->size >= aligned_size + sizeof(BlockHeader) + 8) {
                // Вычисляем адрес, где начнется новый (отрезанный) блок
                // Адрес текущего + размер заголовка + размер данных, которые мы
                // забираем
                BlockHeader* new_block =
                    (BlockHeader*)((uint8_t*)current + sizeof(BlockHeader) +
                                   aligned_size);

                // Настраиваем новый блок (это будет "хвост", он свободный)
                new_block->size =
                    current->size - aligned_size - sizeof(BlockHeader);
                new_block->is_free = true;
                new_block->next = current->next;  // Вставляем его в цепочку

                // Обновляем текущий блок (теперь он стал короче и указывает на
                // новый)
                current->size = aligned_size;
                current->next = new_block;
            }

            // 4. Помечаем блок как занятый
            current->is_free = false;

            // 5. Возвращаем пользователю указатель на ДАННЫЕ.
            // Данные начинаются сразу после заголовка.
            return (void*)((uint8_t*)current + sizeof(BlockHeader));
        }
        // Если блок не подошел, идем к следующему
        current = current->next;
    }

    return NULL;  // Прошли весь список и ничего не нашли (память кончилась или
                  // сильно фрагментирована)
}

// Функция освобождения памяти.
void allocator_free(Allocator* allocator, void* block) {
    if (!allocator || !block) return;

    // 1. Получаем доступ к заголовку.
    // Пользователь дал нам указатель на данные. Заголовок лежит ПЕРЕД данными.
    // Отступаем назад на размер заголовка.
    BlockHeader* header = (BlockHeader*)((uint8_t*)block - sizeof(BlockHeader));

    // 2. Просто помечаем как свободный.
    header->is_free = true;

    // 3. Склейка (Coalescing) - ОПЦИОНАЛЬНО, НО ВАЖНО.
    // Если мы освободили блок, а следующий за ним ТОЖЕ свободен, их надо
    // объединить в один большой. Иначе у нас будет куча мелких свободных
    // кусочков, в которые не влезет большой объект.

    BlockHeader* current = allocator->head;
    while (current != NULL && current->next != NULL) {
        if (current->is_free && current->next->is_free) {
            // Склеиваем current и current->next
            // Размер увеличивается на размер следующего блока + размер его
            // заголовка
            current->size += sizeof(BlockHeader) + current->next->size;

            // Перекидываем стрелку через один блок
            current->next = current->next->next;

            // ВАЖНО: Не переходим к следующему блоку (current = current->next),
            // потому что новый объединенный блок может склеиться с еще одним
            // следующим!
        } else {
            current = current->next;
        }
    }
}

void allocator_destroy(Allocator* allocator) {
    // Ничего особенного делать не надо, память очистится снаружи
}