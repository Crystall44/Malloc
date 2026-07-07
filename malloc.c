#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "malloc.h"

// Конфигурация аллокатора

#define ALIGNMENT 16 // Выравнивание: 16 байт как в glibc

// Округление размера вверх до кратного ALIGNMENT
// ример для 16: ALIGN(1)=16, ALIGN(17)=32, ALIGN(16)=16
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

// Размер заголовка с учётом выравнивания
#define HEADER_SIZE ALIGN(sizeof(BlockHeader))

// Размер footer
#define FOOTER_SIZE sizeof(BlockFooter)

// Суммарный объём метаданных на один блок
#define METADATA_SIZE (HEADER_SIZE + FOOTER_SIZE)

// Минимальный размер блока: метаданные + 16 байт на данные
// Блоки меньшего размера бессмысленны - некуда класть полезные данные
#define MIN_BLOCK_SIZE (METADATA_SIZE + 16)

// Сегрегация: 4 списка для блоков разного размера
// Ускоряет поиск и уменьшает фрагментацию
#define NUM_LISTS 4
#define LIST_0_MAX 64  // Мелкие блоки
#define LIST_1_MAX 256  // Средние
#define LIST_2_MAX 1024 // Крупные
#define LIST_3_MAX SIZE_MAX // Огромные

#define MAGIC_COOKIE 0xDEADBEEF // Сигнатура для проверки целостности блоков

// Структуры метаданных

// Каждый блок памяти (и свободный, и занятый) имеет заголовок и footer
// Footer хранит копию размера для O(1) доступа к предыдущему блоку при слиянии
// Оба содержат magic cookie для обнаружения повреждений памяти
typedef struct BlockHeader {
	size_t size; // Размер всего блока (header + данные + footer)
	int free; // 1 - свободен. 0 - занят
	struct BlockHeader* next; // Указатели для двусвязного списка свободных
	struct BlockHeader* prev;
	uint32_t cookie; // Сигнатура MAGIC_COOKIE
} BlockHeader;

typedef struct BlockFooter {
	size_t size; // Дублирует размер из header
	uint32_t cookie; // Сигнатура MAGIC_COOKIE
} BlockFooter;

// Глобальное состояние аллокатора

static BlockHeader* free_lists[NUM_LISTS] = { NULL }; // Сегрегированные списки
static void* heap_start = NULL; // Начало кучи
static void* heap_end = NULL; // Текущий конец кучи

// Статистика для отладки и анализа
static size_t total_allocated = 0; // Сколько всего запрошено у ОС
static size_t total_freed = 0; // Сколько освобождено (сумма размеров)
static size_t peak_memory = 0; // Пиковое использование
static size_t allocation_count = 0; // Счётчик вызовов my_malloc
static size_t free_count = 0; // Счётчик вызовов my_free

// Внутренние вспомогательные функции

// Получает footer блока по его header
static inline BlockFooter* get_footer(BlockHeader* header) {
	// Footer находится в конце блока: header + size - FOOTER_SIZE
	return (BlockFooter*)((char*)header + header->size - FOOTER_SIZE);
}

// Получает следующий физический блок (не по списку, а по адресу в памяти)
// Используется для слияния соседних блоков и обхода кучи
static inline BlockHeader* get_next_block(BlockHeader* block) {
	if ((char*)block + block->size >= (char*)heap_end) {
		return NULL; // Достигли конца кучи
	}
	return (BlockHeader*)((char*)block + block->size);
}

// Получает предыдущий физический блок за O(1)
// Ключевая оптимизация: footer предыдущего блока лежит прямо перед нашим header
// Без footer пришлось бы проходить всю кучу от начала
static inline BlockHeader* get_prev_block(BlockHeader* block) {
	if ((char*)block == (char*)heap_start) {
		return NULL; // Это первый блок в куче
	}

	// Footer предыдущего блока находится прямо перед нашим header
	BlockFooter* prev_footer = (BlockFooter*)((char*)block - FOOTER_SIZE);

	if (prev_footer->cookie != MAGIC_COOKIE) {
		return NULL; // Целостность нарушена
	}

	// Адрес предыдущего блока = наш адрес - его размер
	return (BlockHeader*)((char*)block - prev_footer->size);
}


// Определяет, в какой из сегрегированных списков попадёт блок данного размера
// Мелкие блоки ищутся быстрее (короткие списки), крупные - в отдельных списках
static inline int get_list_index(size_t size) {
	size_t data_size = size - METADATA_SIZE;

	if (data_size <= LIST_0_MAX) return 0;
	if (data_size <= LIST_1_MAX) return 1;
	if (data_size <= LIST_2_MAX) return 2;
	return 3;
}


// Инициализирует блок: заполняет header и footer, прописывает размер и cookie
static void init_block(BlockHeader* block, size_t size, int free_flag) {
	block->size = size;
	block->free = free_flag;
	block->next = NULL;
	block->prev = NULL;
	block->cookie = MAGIC_COOKIE;

	BlockFooter* footer = get_footer(block);
	footer->size = size;
	footer->cookie = MAGIC_COOKIE;
}

// Вставляет блок в начало соответствующего сегрегированного списка
// LIFO (в начало) - быстрее, чем вставка с сортировкой
static void insert_free_block(BlockHeader* block) {
	int index = get_list_index(block->size);
	BlockHeader** list = &free_lists[index];

	block->prev = *list;
	block->next = NULL;

	if (*list != NULL) {
		(*list)->prev = block;
	}

	*list = block;
	block->free = 1;
}


// Удаляет блок из списка свободных (двусвязный список - удаление за O(1))
static void remove_free_block(BlockHeader* block) {
	int index = get_list_index(block->size);
	BlockHeader** list = &free_lists[index];

	if (*list == block) {
		*list = block->next;
		if (block->next) {
			block->next->prev = NULL;
		}
	}
	else {
		if (block->prev) {
			block->prev->next = block->next;
		}
		if (block->next) {
			block->next->prev = block->prev;
		}
	}

	block->next = NULL;
	block->prev = NULL;
}

// Сливает освобождённый блок с соседними свободными блоками
// Это ключевой механизм борьбы с внешней фрагментацией
// Возвращает указатель на объединённый блок
static BlockHeader* merge_blocks(BlockHeader* block) {
	// Пробуем слить со следующим
	BlockHeader* next = get_next_block(block);

	if (next != NULL && next->free) {
		remove_free_block(next);
		block->size += next->size;
	}

	// Пробуем слить с предыдущим
	BlockHeader* prev = get_prev_block(block);

	if (prev != NULL && prev->free) {
		remove_free_block(prev);
		prev->size += block->size;

		// Обновляем footer обьединенного блока
		BlockFooter* footer = get_footer(prev);
		footer->size = prev->size;
		footer->cookie = MAGIC_COOKIE;

		return prev; // Позвращаем более ранний блок
	}

	// Обновляем footer текущего блока
	BlockFooter* footer = get_footer(block);
	footer->size = block->size;
	footer->cookie = MAGIC_COOKIE;

	return block;
}


// Разделяет блок, если он значительно больше запрошенного размера
// Остаток оформляется как новый свободный блок
// Предотвращает расточительство: не отдаём 4KB под запрос в 16 байт
static void split_block(BlockHeader* block, size_t requested_size) {
	size_t remaining = block->size - requested_size;

	if (remaining >= MIN_BLOCK_SIZE) {
		// Создаем новый блок из остатка
		BlockHeader* new_block = (BlockHeader*)((char*)block + requested_size);
		init_block(new_block, remaining, 1);

		// Урезаем текущий блок
		block->size = requested_size;
		BlockFooter* footer = get_footer(block);
		footer->size = requested_size;
		footer->cookie = MAGIC_COOKIE;

		// Остаток - в список свободных
		insert_free_block(new_block);
	}
}


// Запрашивает новую память у ОС через VirtualAlloc (Windows API)
// Всегда запрашивает минимум 4KB (размер страницы) для эффективности
static BlockHeader* request_memory(size_t size) {
	size_t request_size = (size < 4096) ? 4096 : ALIGN(size);

	// VirtualAlloc резервирует и выделяет страницы виртуальной памяти
	void* mem = VirtualAlloc(
		NULL, // Система сама выберет адрес
		request_size, // Размер региона
		MEM_COMMIT | MEM_RESERVE, // Зарезервировать и выделить
		PAGE_READWRITE // Доступ на чтение и запись
	);

	if (mem == NULL) {
		return NULL; // ОС отказала в памяти
	}

	if (heap_start == NULL) {
		heap_start = mem; // Первый запрос - запоминаем начало кучи
	}

	heap_end = (char*)mem + request_size;

	BlockHeader* block = (BlockHeader*)mem;
	init_block(block, request_size, 0);

	total_allocated += request_size;
	if (total_allocated > peak_memory) {
		peak_memory = total_allocated;
	}

	return block;
}


// Пытается вернуть память ОС, если последний блок в куче свободен
// Использует VirtualFree для освобождения целых регионов
static void try_return_memory(void) {
	if (heap_start == NULL) return;

	// Находим последний физический блок
	BlockHeader* current = (BlockHeader*)heap_start;
	BlockHeader* last_block = NULL;

	while (current != NULL && (void*)current < heap_end) {
		last_block = current;
		current = get_next_block(current);
	}

	if (last_block != NULL && last_block->free) {
		// Проверяем, что это действительно последний блок
		if ((char*)last_block + last_block->size == heap_end) {
			remove_free_block(last_block);

			size_t shrink = last_block->size;

			// Освобождаем регион виртуальной памяти
			VirtualFree(last_block, 0, MEM_RELEASE);

			heap_end = (char*)heap_end - shrink;
			total_allocated -= shrink;

			if (heap_start == heap_end) {
				heap_start = NULL; // Куча полностью пуста
			}
		}
	}
}

// Поиск свободного блока по алгоритму best-fit
// Ищет наименьший подходящий блок во всех списках, начиная с подходящего по размеру
// При точном совпадении размера сразу возвращает блок
static BlockHeader* find_bect_fit(size_t size) {
	int start_index = get_list_index(size);
	BlockHeader* best = NULL;
	size_t best_size = SIZE_MAX;

	// Проходим по спискам от подходящего до огромных
	for (int i = start_index; i < NUM_LISTS; i++) {
		BlockHeader* current = free_lists[i];

		while (current != NULL) {
			if (current->size >= size && current->size < best_size) {
				best = current;
				best_size = current->size;

				// Точное совпадение - идеальный вариант
				if (current->size == size) {
					return current;
				}
			}

			current = current->next;
		}

		// Если нашли хороший блок в текущем списке - возвращаем
		if (best != NULL) {
			return best;
		}
	}

	return NULL;
}

// Выделяет блок памяти размером size байт
void* my_malloc(size_t size) {
	if (size == 0) {
		return NULL;
	}

	// Вычисляем полный размер с метаданными и выравниванием
	size_t total_size = ALIGN(size) + METADATA_SIZE;
	if (total_size < MIN_BLOCK_SIZE) {
		total_size = MIN_BLOCK_SIZE;
	}

	// Ищем подходящий свободный блок
	BlockHeader* block = find_bect_fit(total_size);

	if (block != NULL) {
		remove_free_block(block);

		// Проверяем целостность перед использованием
		if (block->cookie != MAGIC_COOKIE) {
			return NULL;
		}

		split_block(block, total_size); // Отрезаем лишнее
		block->size = 0;
	}
	else {
		// Нет подходящего блока - запрашиваем у ОС
		block = request_memory(total_size);
		if (block == NULL) {
			return NULL;
		}
		split_block(block, total_size);
	}
	allocation_count++;

	// Возвращаем указатель на пользовательские данные (после header)
	return (void*)((char*)block + HEADER_SIZE);
}


// Освобождает ранее выделенный блок памяти
void my_free(void* ptr) {
	if (ptr == NULL) {
		return;
	}

	// Получаем header по пользовательскому указателю
	BlockHeader* block = (BlockHeader*)((char*)ptr - HEADER_SIZE);

	// Проверка целостности
	if (block->cookie != MAGIC_COOKIE) {
		return; // Повреждение памяти - тихо игнорируем
	}

	if (block->free) {
		return; // Двойное освобождение - игнорируем
	}

	block->free = 1;
	free_count++;
	total_freed += block->size;

	// Шаг 1: вставляем в список свободных
	insert_free_block(block);

	// Шаг 2: сливаем с соседями (если они тоже свободны)
	BlockHeader* merged = merge_blocks(block);

	// Если merged != block - нас поглотил предыдущий блок, он уже в списке
	// Если merged == block - мы уже вставили блок в список ранее (insert_free_block)

	// Шаг 3: пробуем вернуть память ОС

	try_return_memory();
}