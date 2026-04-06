#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "allocator.h"

#define PAGE_SIZE 4096
#define MAX_ORDER 12  // 2^12 = 4096 (размер страницы)
#define MIN_ORDER 4   // 2^4 = 16 байт
#define LARGE_BLOCK_ORDER 255 // Специальный маркер для больших блоков

typedef struct PageDescriptor {
    struct PageDescriptor* next;
    struct PageDescriptor* prev; // Для двусвязного списка свободных страниц
    
    void* page_start;
    uint8_t order;       // Размер блоков (степень двойки) или LARGE_BLOCK_ORDER
    bool is_page_free;   // Флаг: находится ли страница в глобальном списке free_pages
    
    uint16_t free_count; // Для мелких блоков: кол-во свободных. Для больших: кол-во страниц.
    void* free_list_head; // Голова списка свободных блоков внутри страницы
} PageDescriptor;

struct Allocator {
    void* memory_start;
    size_t total_size;

    // buckets[i] хранит список страниц, нарезанных на блоки размера 2^i.
    // Это односвязные списки (используем next).
    PageDescriptor* buckets[MAX_ORDER + 1];

    // Список полностью пустых страниц. Двусвязный.
    PageDescriptor* free_pages_head;

    PageDescriptor* descriptors_array;
    size_t descriptors_count;
};

static uint8_t get_order(size_t size) {
    uint8_t order = 0;
    size_t s = 1;
    while (s < size) {
        s <<= 1;
        order++;
    }
    if (order < MIN_ORDER) order = MIN_ORDER;
    return order;
}

// Добавление страницы в двусвязный список свободных страниц
static void list_add_free_page(Allocator* allocator, PageDescriptor* desc) {
    desc->is_page_free = true;
    desc->order = 0;
    desc->free_count = 0;
    desc->free_list_head = NULL;
    
    desc->next = allocator->free_pages_head;
    desc->prev = NULL;
    
    if (allocator->free_pages_head != NULL) {
        allocator->free_pages_head->prev = desc;
    }
    allocator->free_pages_head = desc;
}

// Удаление страницы из двусвязного списка свободных страниц
static void list_remove_free_page(Allocator* allocator, PageDescriptor* desc) {
    if (!desc->is_page_free) return;
    
    if (desc->prev != NULL) {
        desc->prev->next = desc->next;
    } else {
        allocator->free_pages_head = desc->next;
    }
    
    if (desc->next != NULL) {
        desc->next->prev = desc->prev;
    }
    
    desc->next = NULL;
    desc->prev = NULL;
    desc->is_page_free = false;
}

Allocator* allocator_create(void* realMemory, size_t memory_size) {
    if (memory_size < PAGE_SIZE * 2) return NULL;

    Allocator* allocator = (Allocator*)realMemory;
    allocator->memory_start = realMemory;
    allocator->total_size = memory_size;

    for (int i = 0; i <= MAX_ORDER; i++) {
        allocator->buckets[i] = NULL;
    }

    uint8_t* data_start = (uint8_t*)realMemory + sizeof(Allocator);
    size_t num_pages = (memory_size - sizeof(Allocator)) / (sizeof(PageDescriptor) + PAGE_SIZE);

    allocator->descriptors_count = num_pages;
    allocator->descriptors_array = (PageDescriptor*)data_start;

    uint8_t* pages_base = (uint8_t*)(allocator->descriptors_array + num_pages);
    
    // Выравнивание на границу страницы
    size_t align_offset = (uintptr_t)pages_base % PAGE_SIZE;
    if (align_offset != 0) {
        pages_base += (PAGE_SIZE - align_offset);
    }

    allocator->free_pages_head = NULL;

    for (size_t i = 0; i < num_pages; i++) {
        PageDescriptor* desc = &allocator->descriptors_array[i];
        desc->page_start = pages_base + i * PAGE_SIZE;

        if ((uint8_t*)desc->page_start + PAGE_SIZE > (uint8_t*)realMemory + memory_size) {
            allocator->descriptors_count = i;
            break;
        }

        // Инициализируем и добавляем в список свободных
        list_add_free_page(allocator, desc);
    }

    return allocator;
}

void* allocator_alloc(Allocator* allocator, size_t size) {
    if (!allocator || size == 0) return NULL;

    // --- БОЛЬШИЕ АЛЛОКАЦИИ (> PAGE_SIZE) ---
    if (size > PAGE_SIZE) {
        size_t needed_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        
        // Ищем последовательность свободных страниц
        size_t consecutive_free = 0;
        size_t start_index = 0;
        
        for (size_t i = 0; i < allocator->descriptors_count; i++) {
            if (allocator->descriptors_array[i].is_page_free) {
                if (consecutive_free == 0) start_index = i;
                consecutive_free++;
            } else {
                consecutive_free = 0;
            }
            
            if (consecutive_free == needed_pages) {
                // Нашли! Забираем страницы.
                for (size_t k = 0; k < needed_pages; k++) {
                    PageDescriptor* d = &allocator->descriptors_array[start_index + k];
                    list_remove_free_page(allocator, d);
                }
                
                // Настраиваем первый дескриптор
                PageDescriptor* first = &allocator->descriptors_array[start_index];
                first->order = LARGE_BLOCK_ORDER;
                first->free_count = (uint16_t)needed_pages; // Храним кол-во страниц
                
                return first->page_start;
            }
        }
        return NULL; // Не нашли непрерывного куска
    }

    // --- МЕЛКИЕ АЛЛОКАЦИИ (<= PAGE_SIZE) ---
    uint8_t order = get_order(size);
    if (order > MAX_ORDER) return NULL; // На всякий случай

    PageDescriptor* desc = allocator->buckets[order];

    // Ищем страницу с местом
    while (desc != NULL && desc->free_count == 0) {
        desc = desc->next;
    }

    // Если нет, берем новую из резерва
    if (desc == NULL) {
        if (allocator->free_pages_head == NULL) return NULL;

        desc = allocator->free_pages_head;
        list_remove_free_page(allocator, desc);

        // Настраиваем страницу
        desc->order = order;
        desc->next = allocator->buckets[order]; // Вставляем в начало списка бакета
        allocator->buckets[order] = desc;

        // Нарезка
        size_t block_size = (1 << order);
        size_t blocks_per_page = PAGE_SIZE / block_size;
        desc->free_count = blocks_per_page;

        uint8_t* ptr = (uint8_t*)desc->page_start;
        for (size_t i = 0; i < blocks_per_page - 1; i++) {
            *(void**)ptr = (ptr + block_size);
            ptr += block_size;
        }
        *(void**)ptr = NULL;
        desc->free_list_head = desc->page_start;
    }

    // Выделяем блок
    void* block = desc->free_list_head;
    if (block) {
        desc->free_list_head = *(void**)block;
        desc->free_count--;
    }
    return block;
}

void allocator_free(Allocator* allocator, void* block) {
    if (!allocator || !block) return;

    // Определяем страницу по адресу
    // Т.к. страницы выровнены по PAGE_SIZE, можно попробовать найти базу.
    // Но у нас есть descriptors_array, который мапит страницы линейно.
    // Лучше найти индекс страницы.
    
    // Предполагаем, что block находится внутри управляемой памяти.
    // Но для Large Alloc block == page_start.
    // Для Small Alloc block внутри page.
    
    // Найдем дескриптор.
    // Быстрый поиск: (addr - first_page_addr) / PAGE_SIZE
    // Нам нужно знать адрес первой страницы.
    // Он хранится в descriptors_array[0].page_start
    
    if (allocator->descriptors_count == 0) return;
    
    uintptr_t base_addr = (uintptr_t)allocator->descriptors_array[0].page_start;
    uintptr_t block_addr = (uintptr_t)block;
    
    if (block_addr < base_addr) return; // Чужой указатель
    
    size_t page_idx = (block_addr - base_addr) / PAGE_SIZE;
    if (page_idx >= allocator->descriptors_count) return;
    
    PageDescriptor* desc = &allocator->descriptors_array[page_idx];

    // --- ОСВОБОЖДЕНИЕ БОЛЬШОГО БЛОКА ---
    if (desc->order == LARGE_BLOCK_ORDER) {
        size_t num_pages = desc->free_count;
        for (size_t i = 0; i < num_pages; i++) {
            if (page_idx + i >= allocator->descriptors_count) break;
            list_add_free_page(allocator, &allocator->descriptors_array[page_idx + i]);
        }
        return;
    }

    // --- ОСВОБОЖДЕНИЕ МЕЛКОГО БЛОКА ---
    // Возвращаем блок в список
    *(void**)block = desc->free_list_head;
    desc->free_list_head = block;
    desc->free_count++;

    // Если страница полностью свободна, возвращаем её в глобальный резерв
    size_t block_size = (1 << desc->order);
    size_t max_blocks = PAGE_SIZE / block_size;

    if (desc->free_count == max_blocks) {
        // Удаляем из списка bucket
        // Это O(N) для односвязного списка bucket, но списки bucket обычно короткие,
        // либо мы можем сделать bucket двусвязным.
        // Пока оставим односвязным.
        
        PageDescriptor** prev_ptr = &allocator->buckets[desc->order];
        PageDescriptor* curr = *prev_ptr;
        
        while (curr != NULL) {
            if (curr == desc) {
                *prev_ptr = curr->next; // Удаляем
                break;
            }
            prev_ptr = &curr->next;
            curr = curr->next;
        }
        
        // Возвращаем в free_pages
        list_add_free_page(allocator, desc);
    }
}

void allocator_destroy(Allocator* allocator) {}