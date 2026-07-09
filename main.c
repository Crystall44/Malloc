#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>
#include <locale.h>
#include <stdint.h>
#include <time.h>
#include "malloc.h"
#define STRESS_COUNT 1000
#define POOL_SIZE 200

static int passed = 0;
static int failed = 0;

static void check(const char* description, int condition) {
    if (condition) {
        printf("Успешно %s\n", description);
        passed++;
    }
    else {
        printf("Ошибка %s\n", description);
        failed++;
    }
}

// Тест 1: базовые операции
static void test_basic_operations(void) {
    printf("\nТест 1: базовые операции\n\n");

    // 1.1 Выделение и освобождение
    printf("1.1 Выделение и освобождение\n");

    void* p1 = my_malloc(100);
    check("malloc(100) != NULL", p1 != NULL);
    check("валидный указатель", is_block_valid(p1));

    my_free(p1);
    check("free не падает", 1);

    // 1.2 Несколько выделений
    printf("\n1.2 Несколько выделений\n");

    void* a = my_malloc(64);
    void* b = my_malloc(128);
    void* c = my_malloc(256);

    check("три блока != NULL", a != NULL && b != NULL && c != NULL);
    check("блоки разные", a != b && b != c && a != c);
    check("все валидны", is_block_valid(a) && is_block_valid(b) && is_block_valid(c));

    my_free(b);
    check("после free(b) a и c валидны", is_block_valid(a) && is_block_valid(c));

    my_free(a);
    my_free(c);
    check("после освобождения всех память не повреждена", validate_heap());

    // 1.3 calloc
    printf("\n1.3 calloc\n");

    int* arr = (int*)my_calloc(10, sizeof(int));
    check("calloc(10, sizeof(int)) != NULL", arr != NULL);

    int all_zero = 1;
    for (int i = 0; i < 10; i++) {
        if (arr[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    check("все 10 элементов == 0", all_zero);

    // Пишем данные
    for (int i = 0; i < 10; i++) {
        arr[i] = i * 10;
    }

    int correct = 1;
    for (int i = 0; i < 10; i++) {
        if (arr[i] != i * 10) correct = 0;
    }
    check("данные читаются корректно", correct);

    my_free(arr);

    // 1.4 realloc
    printf("\n1.4 realloc\n");

    char* str = (char*)my_malloc(10);
    strcpy(str, "Hello");
    check("исходная строка 'Hello'", strcmp(str, "Hello") == 0);

    str = (char*)my_realloc(str, 20);
    check("realloc до 20 байт", str != NULL);
    check("данные сохранились", strcmp(str, "Hello") == 0);
    strcat(str, " World!");
    check("строка расширена", strcmp(str, "Hello World!") == 0);

    // Уменьшение
    str = (char*)my_realloc(str, 6);
    check("realloc до 6 байт", str != NULL);
    check("данные при сжатии", strncmp(str, "Hello", 5) == 0);

    my_free(str);

    // 1.5 Граничные случаи
    printf("\n1.5 Граничные случаи\n");

    check("malloc(0) == NULL", my_malloc(0) == NULL);
    check("calloc(0, 10) == NULL", my_calloc(0, 10) == NULL);
    check("calloc(10, 0) == NULL", my_calloc(10, 0) == NULL);

    void* r = my_malloc(32);
    void* r2 = my_realloc(r, 0);
    check("realloc(ptr, 0) возвращает NULL", r2 == NULL);

    my_free(NULL);  // Не должно падать
    check("free(NULL) не падает", 1);

    check("realloc(NULL, 64) работает как malloc", my_realloc(NULL, 64) != NULL);
}


// Тест 2: выравнивание и метаданные
static void test_alignment(void) {
    printf("\nТест 2: выравнивание и метаданные\n\n");

    printf("2.1 Проверка выравнивания\n");

    int align_ok = 1;
    void* ptrs[10];

    for (int i = 0; i < 10; i++) {
        ptrs[i] = my_malloc((i + 1) * 7);  // Разные нечётные размеры

        if (((uintptr_t)ptrs[i] & 15) != 0) {  
            // Должно быть кратно 16
            printf("Ошибка: ptr %p не выровнен по 16\n", ptrs[i]);
            align_ok = 0;
        }
    }

    check("все 10 указателей выровнены по 16 байт", align_ok);

    for (int i = 0; i < 10; i++) {
        my_free(ptrs[i]);
    }

    printf("\n2.2 Множественные мелкие выделения\n");

    void* small[50];
    int all_valid = 1;
    for (int i = 0; i < 50; i++) {
        small[i] = my_malloc(8);

        if (small[i] == NULL || !is_block_valid(small[i])) {
            all_valid = 0;
        }
    }
    check("50 мелких выделений валидны", all_valid);

    for (int i = 0; i < 50; i++) {
        my_free(small[i]);
    }

    check("куча цела после освобождения", validate_heap());

    printf("\n2.3 Выделение ровно 4096 байт\n");

    void* page = my_malloc(4096);

    check("malloc(4096) != NULL", page != NULL);
    check("валиден", is_block_valid(page));

    // Пишем в начало и конец
    memset(page, 0xAB, 4096);

    check("запись в начало", ((unsigned char*)page)[0] == 0xAB);
    check("запись в конец", ((unsigned char*)page)[4095] == 0xAB);

    my_free(page);
}


// Тест 3: слияние блоков и дефрагментация
static void test_merging(void) {
    printf("\nТест 3: слияние блоков и дефрагментация\n\n");

    printf("3.1 Последовательное освобождение - слияние\n");

    void* a = my_malloc(100);
    void* b = my_malloc(200);
    void* c = my_malloc(300);

    size_t free_before = get_total_free_memory();
    size_t blocks_before = get_free_block_count();

    my_free(a);
    my_free(b);
    my_free(c);

    size_t free_after = get_total_free_memory();
    size_t blocks_after = get_free_block_count();

    printf("Свободных блоков: %zu -> %zu\n", blocks_before, blocks_after);
    printf("Свободной памяти: %zu -> %zu\n", free_before, free_after);

    // Три соседних блока должны слиться в один
    check("блоки слились (свободных блоков не больше 2)", blocks_after <= blocks_before + 2);
    check("память вернулась", free_after > free_before);
    check("куча цела", validate_heap());

    printf("\n3.2 Освобождение через один (проверка фрагментации)\n");

    void* x = my_malloc(64);
    void* y = my_malloc(64);
    void* z = my_malloc(64);

    my_free(x);
    my_free(z); // y всё ещё занят - слияния не будет

    double frag_before = get_fragmentation_ratio();
    printf("Фрагментация (y занят): %.2f%%\n", frag_before * 100);

    my_free(y); // Теперь все три свободны - должны слиться

    double frag_after = get_fragmentation_ratio();
    printf("Фрагментация (все свободны): %.2f%%\n", frag_after * 100);

    check("фрагментация уменьшилась после полного освобождения", frag_after <= frag_before);
    check("куча цела", validate_heap());

    printf("\n3.3 Переиспользование освобождённых блоков\n");

    void* p1 = my_malloc(128);
    memset(p1, 0x11, 128);

    void* addr1 = p1;
    my_free(p1);

    void* p2 = my_malloc(128);
    void* addr2 = p2;

    printf("Адрес первого: %p\n", addr1);
    printf("Адрес второго: %p\n", addr2);

    // Best-fit должен выдать тот же блок (точное совпадение размера)
    check("переиспользован тот же адрес (best-fit)", addr1 == addr2);

    my_free(p2);
}


// Тест 4: стресс-тест
static void test_stress(void) {
    printf("\nТест 4: стресс-тест\n\n");

    printf("4.1 1000 случайных выделений и освобождений\n");

    srand(12345); // Фиксированный seed для воспроизводимости

    void* ptrs[STRESS_COUNT] = { NULL };
    int allocated = 0;
    int max_allocated = 0;

    for (int i = 0; i < STRESS_COUNT; i++) {
        int action = rand() % 100;

        if (action < 60 && allocated < 500) {
            // Выделяем (60% вероятность)
            size_t size = (rand() % 500) + 1;
            ptrs[i] = my_malloc(size);

            if (ptrs[i] != NULL) {
                allocated++;
                if (allocated > max_allocated) max_allocated = allocated;

                // Пишем в память для проверки
                memset(ptrs[i], i & 0xFF, size > 0 ? 1 : 0);
            }
        }
        else if (action < 100 && allocated > 0) {
            // Освобождаем случайный блок
            int idx = rand() % i;

            if (ptrs[idx] != NULL) {
                my_free(ptrs[idx]);

                ptrs[idx] = NULL;
                allocated--;
            }
        }
    }

    // Освобождаем оставшиеся
    for (int i = 0; i < STRESS_COUNT; i++) {
        if (ptrs[i] != NULL) {
            my_free(ptrs[i]);

            ptrs[i] = NULL;
            allocated--;
        }
    }

    printf("Максимально выделено одновременно: %d\n", max_allocated);
    check("всё освобождено", allocated == 0);
    check("куча цела после стресс-теста", validate_heap());

    printf("\n4.2 Выделение большого блока (1 MB)\n");

    void* big = my_malloc(1024 * 1024);
    check("malloc(1 MB) != NULL", big != NULL);
    check("блок валиден", is_block_valid(big));

    // Пишем в разные части мегабайта
    memset(big, 0, 1024 * 1024);
    ((char*)big)[0] = 'A';
    ((char*)big)[512 * 1024] = 'B';
    ((char*)big)[1024 * 1024 - 1] = 'C';

    check("запись в начало", ((char*)big)[0] == 'A');
    check("запись в середину", ((char*)big)[512 * 1024] == 'B');
    check("запись в конец", ((char*)big)[1024 * 1024 - 1] == 'C');

    my_free(big);
    check("куча цела после большого блока", validate_heap());

    printf("\n4.3 realloc до большого размера\n");

    char* growing = (char*)my_malloc(16);
    strcpy(growing, "start");
    check("начальный размер 16", growing != NULL);

    // Постепенно увеличиваем
    for (int size = 32; size <= 8192; size *= 2) {
        growing = (char*)my_realloc(growing, size);

        if (growing == NULL) break;

        snprintf(growing + strlen(growing), size - strlen(growing), "+");
    }

    check("realloc до 8KB", growing != NULL);
    check("данные сохранились", strncmp(growing, "start", 5) == 0);

    my_free(growing);
}


// Тест 5: реальные сценарии
static void test_real_scenarios(void) {
    printf("\nТест 5: реальные сценарии\n\n");

    // 5.1 Динамический массив (как std::vector)
    printf("5.1 Динамический массив (паттерн vector)\n");

    int* vec = NULL;
    size_t vec_size = 0;
    size_t vec_capacity = 0;

    for (int i = 0; i < 1000; i++) {
        if (vec_size == vec_capacity) {
            size_t new_cap = vec_capacity == 0 ? 4 : vec_capacity * 2;
            vec = (int*)my_realloc(vec, new_cap * sizeof(int));

            if (vec == NULL) break;
            vec_capacity = new_cap;
        }
        vec[vec_size++] = i * i;
    }

    check("добавлено 1000 элементов", vec_size == 1000);
    check("корректные данные", vec[0] == 0 && vec[999] == 999 * 999);
    check("куча цела", validate_heap());

    my_free(vec);

    // 5.2 Пул объектов фиксированного размера
    printf("\n5.2 Пул объектов (мелкие выделения по 32 байта)\n");

    void* pool[POOL_SIZE];
    int pool_ok = 1;

    for (int i = 0; i < POOL_SIZE; i++) {
        pool[i] = my_malloc(32);
        if (pool[i] == NULL) {
            pool_ok = 0;
            break;
        }
        memset(pool[i], i, 32);
    }

    check("200 блоков по 32 байта выделены", pool_ok);

    // Проверяем данные
    int data_ok = 1;
    for (int i = 0; i < POOL_SIZE && data_ok; i++) {
        if (pool[i] != NULL) {
            unsigned char* data = (unsigned char*)pool[i];

            if (data[0] != (i & 0xFF) || data[31] != (i & 0xFF)) {
                data_ok = 0;
            }
        }
    }
    check("данные не повреждены", data_ok);

    // Освобождаем через один
    for (int i = 0; i < POOL_SIZE; i += 2) {
        my_free(pool[i]);
        pool[i] = NULL;
    }

    // Выделяем заново (должны переиспользовать освобождённые)
    for (int i = 0; i < POOL_SIZE; i += 2) {
        pool[i] = my_malloc(32);

        if (pool[i] != NULL) {
            memset(pool[i], 0xFF, 32);
        }
    }

    check("переиспользование освобождённых слотов", pool_ok);

    for (int i = 0; i < POOL_SIZE; i++) {
        my_free(pool[i]);
    }

    check("куча цела после пула", validate_heap());

    // 5.3 Парсинг строк (короткоживущие выделения)
    printf("\n5.3 Обработка текста (короткоживущие строки)\n");

    const char* input = "The quick brown fox jumps over the lazy dog";
    char* words[50];
    int word_count = 0;

    // Разбиваем на слова
    char* temp = (char*)my_malloc(strlen(input) + 1);
    strcpy(temp, input);

    char* token = strtok(temp, " ");
    while (token != NULL && word_count < 50) {
        words[word_count] = (char*)my_malloc(strlen(token) + 1);

        strcpy(words[word_count], token);

        word_count++;
        token = strtok(NULL, " ");
    }

    check("найдено 9 слов", word_count == 9);
    check("первое слово 'The'", strcmp(words[0], "The") == 0);
    check("последнее 'dog'", strcmp(words[word_count - 1], "dog") == 0);

    // Освобождаем временную строку
    my_free(temp);

    // Собираем предложение обратно
    char* result = (char*)my_malloc(100);
    result[0] = '\0';

    for (int i = 0; i < word_count; i++) {
        strcat(result, words[i]);
        if (i < word_count - 1) strcat(result, " ");
    }

    check("собрано обратно", strcmp(result, input) == 0);

    // Освобождаем все слова
    for (int i = 0; i < word_count; i++) {
        my_free(words[i]);
    }

    my_free(result);
    check("куча цела", validate_heap());

    // 5.4 Буфер для сетевого пакета (вариативные размеры)
    printf("\n5.4 Сетевой буфер (разные размеры пакетов)\n");

    void* packets[100];
    size_t packet_sizes[] = { 64, 128, 256, 512, 1024, 1500, 64, 128, 256, 512 };
    int num_sizes = sizeof(packet_sizes) / sizeof(packet_sizes[0]);

    // 10 циклов приёма пакетов
    for (int cycle = 0; cycle < 10; cycle++) {
        for (int i = 0; i < 100; i++) {
            size_t sz = packet_sizes[(i + cycle) % num_sizes];
            packets[i] = my_malloc(sz);

            if (packets[i] != NULL) {
                memset(packets[i], cycle, sz > 0 ? 1 : 0);
            }
        }

        // "Обрабатываем" и освобождаем все
        for (int i = 0; i < 100; i++) {
            my_free(packets[i]);
            packets[i] = NULL;
        }
    }

    check("10 циклов по 100 пакетов без ошибок", 1);
    check("куча цела после пакетов", validate_heap());
}


int main(void) {
    setlocale(LC_ALL, "Russian");

    printf("Тесты собственного аллокатора памяти (malloc)\n");

    test_basic_operations();
    test_alignment();
    test_merging();
    test_stress();
    test_real_scenarios();

    printf("Итоги:\n");
    printf("Пройдено проверок: %d\n", passed);
    printf("Провалено: %d\n", failed);
    printf("Всего: %d\n", passed + failed);

    if (failed == 0) {
        printf("\nВсе тесты пройдены успешно!\n");
    }
    else {
        printf("\nВнимание: есть проваленные проверки.\n");
    }

    // Финальный отчёт
    printf("Финальная статистика кучи:\n");
    print_heap_stats();
    print_free_lists();

    return 0;
}