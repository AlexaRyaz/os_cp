#include "allocator.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

// Минимальный порядок блока. 2^5 = 32 байта.
// Этого достаточно для заголовка (8 байт) + 2 указателя (16 байт) = 24 байта.
#define MIN_ORDER 5
#define MAX_ORDER 63

// Заголовок блока. Выравниваем до 8 байт.
typedef struct BlockHeader {
    uint8_t order;
    uint8_t is_free;
    uint8_t padding[6]; 
} BlockHeader;

// Структура свободного блока.
// Накладывается на память блока, включая заголовок.
typedef struct FreeBlock {
    BlockHeader header;
    struct FreeBlock* next;
    struct FreeBlock* prev;
} FreeBlock;

// Структура аллокатора
struct Allocator {
    void* memory_start;
    size_t memory_size;
    FreeBlock* free_lists[MAX_ORDER + 1];
};

// Вспомогательная функция для добавления блока в список свободных
static void list_add(Allocator* allocator, FreeBlock* block, int order) {
    block->header.is_free = 1;
    block->header.order = order;
    
    block->next = allocator->free_lists[order];
    block->prev = NULL;
    
    if (allocator->free_lists[order] != NULL) {
        allocator->free_lists[order]->prev = block;
    }
    
    allocator->free_lists[order] = block;
}

// Вспомогательная функция для удаления блока из списка свободных
static void list_remove(Allocator* allocator, FreeBlock* block) {
    int order = block->header.order;
    
    if (block->prev != NULL) {
        block->prev->next = block->next;
    } else {
        allocator->free_lists[order] = block->next;
    }
    
    if (block->next != NULL) {
        block->next->prev = block->prev;
    }
    
    block->next = NULL;
    block->prev = NULL;
}

// Вычисление логарифма по основанию 2 (округление вверх)
static int get_order(size_t size) {
    int order = 0;
    size_t s = 1;
    while (s < size) {
        s <<= 1;
        order++;
    }
    return order;
}

Allocator* allocator_create(void* realMemory, size_t memory_size) {
    if (memory_size < sizeof(Allocator) + (1 << MIN_ORDER)) {
        return NULL;
    }

    Allocator* allocator = (Allocator*)realMemory;
    allocator->memory_start = realMemory;
    allocator->memory_size = memory_size;
    
    for (int i = 0; i <= MAX_ORDER; i++) {
        allocator->free_lists[i] = NULL;
    }

    // Вычисляем начало управляемой кучи
    uint8_t* heap_start = (uint8_t*)realMemory + sizeof(Allocator);
    
    // Выравниваем начало кучи (хотя бы до 8 байт, но для buddy лучше иметь смещение 0 относительно начала блока)
    // В простой реализации будем считать смещения относительно heap_start.
    // Но нам нужно разбить доступную память на блоки степеней двойки.
    
    size_t available_size = memory_size - sizeof(Allocator);
    uint8_t* current_addr = heap_start;
    
    // Жадным образом откусываем максимально возможные куски степени 2
    while (available_size >= (1 << MIN_ORDER)) {
        int order = MAX_ORDER;
        // Ищем максимальный порядок, который помещается и выровнен
        // Для корректной работы buddy system адреса блоков должны быть кратны их размеру.
        // Относительно heap_start?
        // Если мы используем относительную адресацию для поиска двойников:
        // buddy_offset = offset ^ (1 << order)
        // То offset должен быть относительно heap_start.
        
        size_t offset = current_addr - heap_start;
        
        // Находим максимальный порядок, который влезает в available_size
        // И при этом offset кратен 2^order (для выравнивания)
        while (order > MIN_ORDER) {
            size_t block_size = (size_t)1 << order;
            if (block_size <= available_size && (offset % block_size == 0)) {
                break;
            }
            order--;
        }
        
        // Добавляем блок в список
        FreeBlock* block = (FreeBlock*)current_addr;
        list_add(allocator, block, order);
        
        size_t block_size = (size_t)1 << order;
        current_addr += block_size;
        available_size -= block_size;
    }

    return allocator;
}

void* allocator_alloc(Allocator* allocator, size_t size) {
    if (allocator == NULL || size == 0) return NULL;

    size_t needed_size = size + sizeof(BlockHeader);
    int order = get_order(needed_size);
    if (order < MIN_ORDER) order = MIN_ORDER;

    // Ищем свободный блок подходящего или большего размера
    int current_order = order;
    while (current_order <= MAX_ORDER && allocator->free_lists[current_order] == NULL) {
        current_order++;
    }

    if (current_order > MAX_ORDER) {
        return NULL; // Нет памяти
    }

    // Если нашли блок большего размера, разбиваем его
    FreeBlock* block = allocator->free_lists[current_order];
    list_remove(allocator, block);

    while (current_order > order) {
        current_order--;
        // Разбиваем на два блока порядка current_order
        FreeBlock* buddy = (FreeBlock*)((uint8_t*)block + ((size_t)1 << current_order));
        
        // Добавляем buddy в список свободных
        list_add(allocator, buddy, current_order);
        
        // block остается у нас для следующей итерации (или возврата)
        // Мы как бы спускаемся вниз по дереву, держа левую половинку
    }

    // Помечаем как занятый
    block->header.is_free = 0;
    block->header.order = order;

    return (void*)((uint8_t*)block + sizeof(BlockHeader));
}

void allocator_free(Allocator* allocator, void* ptr) {
    if (allocator == NULL || ptr == NULL) return;

    FreeBlock* block = (FreeBlock*)((uint8_t*)ptr - sizeof(BlockHeader));
    if (block->header.is_free) return; // Уже свободен?

    int order = block->header.order;
    block->header.is_free = 1;

    // Пытаемся склеить с двойниками
    uint8_t* heap_start = (uint8_t*)allocator->memory_start + sizeof(Allocator);
    
    while (order < MAX_ORDER) {
        size_t offset = (uint8_t*)block - heap_start;
        size_t buddy_offset = offset ^ ((size_t)1 << order);
        FreeBlock* buddy = (FreeBlock*)(heap_start + buddy_offset);

        // Проверяем, что buddy находится в пределах памяти
        if ((uint8_t*)buddy < heap_start || (uint8_t*)buddy >= (uint8_t*)allocator->memory_start + allocator->memory_size) {
            break;
        }

        // Проверяем, свободен ли buddy и имеет ли он тот же порядок
        if (!buddy->header.is_free || buddy->header.order != order) {
            break;
        }

        // Склеиваем
        list_remove(allocator, buddy);
        
        // Новый адрес блока - минимальный из двух (block и buddy)
        if (buddy < block) {
            block = buddy;
        }
        
        order++;
        block->header.order = order; // Обновляем порядок объединенного блока
    }

    list_add(allocator, block, order);
}

void allocator_destroy(Allocator* allocator) {
    // Ничего особенного делать не нужно, так как вся память внутри realMemory
}