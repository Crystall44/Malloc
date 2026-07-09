#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "malloc.h"

// Конфигурация
#define ALIGNMENT 16 // Выравнивание блоков: 16 байт
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

#define HEADER_SIZE ALIGN(sizeof(BlockHeader)) // Размер заголовка блока
#define MIN_BLOCK_SIZE (HEADER_SIZE + 16)  // Минимальный блок: заголовок + 16 байт данных

#define MAGIC_COOKIE 0xDEADBEEF  // Сигнатура для проверки целостности блоков

// Заголовок блока памяти
// Каждый блок (и свободный, и занятый) начинается с этого заголовка
// Свободные блоки объединены в двусвязный список для быстрого поиска
typedef struct BlockHeader {
    size_t size; // Размер всего блока (заголовок + данные)
    int    free; // // Флаг: 1 - свободен, 0 - занят
    struct BlockHeader* next; // Следующий свободный блок в списке
    struct BlockHeader* prev; // Предыдущий свободный блок в списке
    uint32_t cookie;
} BlockHeader;

// Глобальное состояние аллокатора
static BlockHeader* free_list = NULL; // Голова списка свободных блоков
static void* heap_start = NULL; // Начало выделенной памяти
static void* heap_end = NULL; // Конец выделенной памяти
static HANDLE heap = NULL; // Хендл кучи Windows (для HeapAlloc/HeapFree)

// Статистика
static size_t total_allocated = 0; // Общий объём выделенной памяти

// Вспомогательные функции

// По указателю на заголовок блока возвращает указатель на пользовательские данные
// Пользовательские данные начинаются сразу после заголовка
static void* get_user_ptr(BlockHeader* block) {
    return (void*)((char*)block + HEADER_SIZE);
}

// По пользовательскому указателю возвращает указатель на заголовок блока
// Заголовок находится непосредственно перед данными
static BlockHeader* get_block(void* ptr) {
    return (BlockHeader*)((char*)ptr - HEADER_SIZE);
}

// Инициализирует кучу при первом вызове my_malloc
// Использует Windows API для выделения большого непрерывного блока (50 MB), внутри которого аллокатор будет управлять памятью
// Это избавляет от необходимости дробить память системными вызовами
static int init_heap(void) {
    // Создаём отдельную кучу Windows
    heap = HeapCreate(0, 50 * 1024 * 1024, 0);

    if (!heap) {
        printf(stderr, "Ошибка: не удалось создать кучу");
        return -1;
    }

    // Выделяем в ней 50 MB непрерывной памяти
    void* mem = HeapAlloc(heap, HEAP_ZERO_MEMORY, 50 * 1024 * 1024);

    if (!mem) {
        printf(stderr, "Ошибка: не удалось выделить память из кучи\n");
        HeapDestroy(heap);
        return -1;
    }

    heap_start = mem;
    heap_end = (char*)mem + 50 * 1024 * 1024;

    // Оформляем всю память как один большой свободный блок
    BlockHeader* block = (BlockHeader*)mem;
    block->size = 50 * 1024 * 1024;
    block->free = 1;
    block->next = NULL;
    block->prev = NULL;
    block->cookie = MAGIC_COOKIE;
    free_list = block;

    total_allocated = 50 * 1024 * 1024;
    return 0;
}

// Поиск свободного блока по алгоритму Best Fit
// Best Fit ищет наименьший блок, достаточный для размещения данных
// Это уменьшает фрагментацию, но требует полного обхода списка
static BlockHeader* find_free_block(size_t total_size) {
    BlockHeader* cur = free_list;
    BlockHeader* best = NULL;
    size_t best_size = SIZE_MAX;

    while (cur) {
        if (cur->free && cur->size >= total_size) {
            // Нашли подходящий блок
            if (cur->size < best_size) {
                best = cur;
                best_size = cur->size;

                // Точное совпадение - идеальный вариант, дальше не ищем
                if (cur->size == total_size)
                    return cur;
            }
        }
        cur = cur->next;
    }

    return best;
}

// Разделяет блок на два, если остаток после выделения >= MIN_BLOCK_SIZE
// Это предотвращает расточительство: не отдаём 4KB под запрос в 16 байт
// Остаток оформляется как новый свободный блок и остаётся в списке
static BlockHeader* split_block(BlockHeader* block, size_t data_size) {
    size_t needed = HEADER_SIZE + data_size;  // data_size уже выровнен

    // Если остаток слишком мал - не разделяем
    if (block->size - needed < MIN_BLOCK_SIZE) return block;

    // Создаём новый свободный блок из остатка
    BlockHeader* new_block = (BlockHeader*)((char*)block + needed);
    new_block->size = block->size - needed;
    new_block->free = 1;
    new_block->next = block->next;
    new_block->prev = block;
    new_block->cookie = MAGIC_COOKIE;

    // Урезаем текущий блок до нужного размера
    block->size = needed;
    block->next = new_block;

    // Поправляем указатели соседа в двусвязном списке
    if (new_block->next) new_block->next->prev = new_block;

    if (free_list == block) free_list = new_block;

    return block;
}

// Удаляет блок из списка свободных
//  Используется, когда блок выделяется пользователю или поглощается при слиянии
static void remove_from_free_list(BlockHeader* block) {
    // Если удаляем голову списка - сдвигаем голову
    if (free_list == block) free_list = block->next;

    // Перелинковываем соседей
    if (block->prev) block->prev->next = block->next;
    if (block->next) block->next->prev = block->prev;

    block->next = block->prev = NULL;
}

// Сливает освобождённый блок с физическими соседями
// Это ключевой механизм борьбы с внешней фрагментацией
// Возвращает указатель на объединённый блок
static BlockHeader* merge_blocks(BlockHeader* block) {
    // 1. Слияние со следующим физическим блоком
    // Следующий блок находится сразу после текущего
    BlockHeader* next_phys = (BlockHeader*)((char*)block + block->size);

    if ((char*)next_phys < (char*)heap_end && next_phys->free) {
        remove_from_free_list(next_phys); // Убираем соседа из списка
        block->size += next_phys->size;  // Поглощаем его память
    }

    // 2. Слияние с предыдущим физическим блоком
    // Ищем в списке свободных блок, который заканчивается ровно там, где начинается текущий
    BlockHeader* cur = free_list;

    while (cur) {
        if ((char*)cur + cur->size == (char*)block && cur->free) {
            // Убираем текущий из списка
            remove_from_free_list(block);

            // Предыдущий поглощает текущий
            cur->size += block->size;

            // Возвращаем объединённый блок
            return cur;
        }

        cur = cur->next;
    }

    return block; // Слияния не произошло
}

// Основные функции

// Выделяет блок памяти размером size байт
// Память не инициализируется
void* my_malloc(size_t size) {
    if (size == 0) return NULL;

    // Ленивая инициализация: при первом вызове выделяем 50 MB у ОС
    if (!heap_start) {
        if (init_heap() != 0) return NULL;
    }

    // Вычисляем полный размер с заголовком и выравниванием
    size_t aligned = ALIGN(size);
    size_t total = HEADER_SIZE + aligned;

    // Ищем подходящий свободный блок
    BlockHeader* block = find_free_block(total);
    if (!block) {
        printf("Ошибка: недостаточно памяти (запрошено %zu байт)\n", size);
        return NULL;
    }

    // Разделяем блок, если он слишком большой
    block = split_block(block, aligned);
    block->free = 0;

    // Убираем из списка свободных - теперь он занят
    remove_from_free_list(block);

    if (block->cookie != MAGIC_COOKIE) {
        printf("ОШИБКА: повреждён заголовок блока\n");
        return NULL;
    }

    return get_user_ptr(block);
}

// Освобождает ранее выделенный блок памяти
// Автоматически сливается с соседними свободными блоками
void my_free(void* ptr) {
    if (!ptr) return;

    // Защита от двойного освобождения
    BlockHeader* block = get_block(ptr);

    if (block->cookie != MAGIC_COOKIE) {
        printf("ОШИБКА: повреждён заголовок при освобождении\n");
        return;
    }

    if (block->free) {
        return;
    }

    block->free = 1;

    // Вставляем в начало списка свободных
    block->next = free_list;
    block->prev = NULL;
    if (free_list) free_list->prev = block;
    free_list = block;

    // Объединяем с соседями для борьбы с фрагментацией
    merge_blocks(block);
}

// Выделяет и обнуляет память под массив из nmemb элементов по size байт
void* my_calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return NULL;

    // Проверка на переполнение умножения
    size_t total = nmemb * size;
    if (total / nmemb != size) return NULL;

    void* ptr = my_malloc(total);
    if (ptr) memset(ptr, 0, total); // Обнуляем выделенную память
    return ptr;
}

// Изменяет размер ранее выделенного блока
// Если новый размер меньше или равен старому - возвращает тот же указатель
// Иначе выделяет новый блок, копирует данные и освобождает старый
void* my_realloc(void* ptr, size_t size) {
    if (!ptr) return my_malloc(size); // realloc(NULL, size) = malloc(size)

    if (size == 0) { my_free(ptr); return NULL; } // realloc(ptr, 0) = free(ptr)

    BlockHeader* block = get_block(ptr);
    size_t old_data_size = block->size - HEADER_SIZE;

    // Если данные помещаются в текущий блок - ничего не делаем
    if (size <= old_data_size) return ptr;

    // Выделяем новый блок и копируем данные
    void* new_ptr = my_malloc(size);

    if (new_ptr) {
        memcpy(new_ptr, ptr, old_data_size);
        my_free(ptr);
    }

    return new_ptr;
}

// Отладочные функции

// Выводит сводную статистику: размер кучи, занято/свободно, количество блоков, фрагментацию, целостность
void print_heap_stats(void) {
    printf("\nСтатистика кучи\n");
    printf("Начало кучи: %p\n", heap_start);
    printf("Конец кучи: %p\n", heap_end);
    printf("Размер кучи: %zu байт (%.2f MB)\n", (size_t)((char*)heap_end - (char*)heap_start), (float)((char*)heap_end - (char*)heap_start) / (1024 * 1024));

    // Обходим все блоки физически и собираем статистику
    int total_blocks = 0, free_blocks = 0;
    size_t total_memory = 0, free_memory = 0;

    BlockHeader* cur = (BlockHeader*)heap_start;
    while ((char*)cur < (char*)heap_end) {
        total_blocks++;
        total_memory += cur->size;

        if (cur->free) {
            free_blocks++;
            free_memory += cur->size;
        }

        cur = (BlockHeader*)((char*)cur + cur->size);
    }

    printf("Всего блоков: %d\n", total_blocks);
    printf("Свободных блоков: %d\n", free_blocks);
    printf("Занятая память: %zu байт (%.2f KB)\n", total_memory - free_memory, (float)(total_memory - free_memory) / 1024);
    printf("Свободная память: %zu байт (%.2f KB)\n", free_memory, (float)free_memory / 1024);
    printf("Использование памяти: %.1f%%\n", (float)(total_memory - free_memory) / total_memory * 100);

    if (validate_heap())
        printf("Целостность кучи: OK\n");
    else
        printf("Целостность кучи: ПОВРЕЖДЕНА\n");
}

// Выводит содержимое списка свободных блоков
void print_free_lists(void) {
    printf("\nСписок свободных блоков\n");
    BlockHeader* cur = free_list;
    int count = 0;
    size_t total_free = 0;

    while (cur) {
        printf("Блок %d: адрес=%p, размер=%zu байт (%.2f KB), свободен=%d\n", count, (void*)cur, cur->size, (float)cur->size / 1024, cur->free);
        total_free += cur->size;
        cur = cur->next;
        count++;
    }

    if (count == 0)
        printf("Нет свободных блоков\n");
    else
        printf("Всего свободно: %zu байт (%.2f KB)\n", total_free, (float)total_free / 1024);
}

// Суммарный объём свободной памяти (в байтах)
size_t get_total_free_memory(void) {
    size_t total = 0;
    BlockHeader* cur = free_list;
    while (cur) { total += cur->size; cur = cur->next; }
    return total;
}

// Количество свободных блоков
size_t get_free_block_count(void) {
    size_t count = 0;
    BlockHeader* cur = free_list;
    while (cur) { count++; cur = cur->next; }
    return count;
}

// Общий объём памяти, выделенной у ОС
size_t get_total_allocated_memory(void) {
    return total_allocated;
}

// Коэффициент внешней фрагментации [0; 1]
// 0 - вся свободная память в одном блоке (идеально)
// 1 - память раздроблена на мельчайшие кусочки
double get_fragmentation_ratio(void) {
    size_t free_mem = get_total_free_memory();
    if (free_mem == 0) return 0.0;

    // Находим крупнейший свободный блок
    size_t largest = 0;
    BlockHeader* cur = free_list;

    while (cur) {
        if (cur->size > largest) largest = cur->size;
        cur = cur->next;
    }

    return 1.0 - ((double)largest / (double)free_mem);
}

// Проверяет, указывает ли ptr на валидный выделенный блок
int is_block_valid(void* ptr) {
    if (!ptr) return 0;
    BlockHeader* block = get_block(ptr);
    if (block->cookie != MAGIC_COOKIE) return 0;

    if (block->free) return 0; // Блок уже освобождён

    if (block->size < MIN_BLOCK_SIZE || block->size > 1024 * 1024 * 1024) return 0;
    return 1;
}

// Проверяет целостность всей кучи:
// Размеры блоков корректны и выровнены
// Блоки не выходят за границы кучи
// Нет блоков меньше минимального размера
int validate_heap(void) {
    if (!heap_start) return 1; // Куча ещё не инициализирована

    BlockHeader* cur = (BlockHeader*)heap_start;
    while ((char*)cur < (char*)heap_end) {
        if (cur->cookie != MAGIC_COOKIE) {
            printf("ОШИБКА: повреждён cookie блока %p\n", (void*)cur);
            return 0;
        }

        // Проверка на нулевой размер
        if (cur->size == 0) {
            printf("ОШИБКА: нулевой размер блока\n");
            return 0;
        }

        // Проверка выхода за границы кучи
        if ((char*)cur + cur->size > (char*)heap_end) {
            printf("ОШИБКА: блок %p выходит за границу кучи\n", (void*)cur);
            return 0;
        }

        // Проверка минимального размера
        if (cur->size < MIN_BLOCK_SIZE) {
            printf("ОШИБКА: блок %p меньше минимального\n", (void*)cur);
            return 0;
        }

        // Переходим к следующему физическому блоку
        cur = (BlockHeader*)((char*)cur + cur->size);
    }
    return 1;
}