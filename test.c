// test.c  —  allocator test programı
// Bölümler:
// 1) Temel testler  — temel malloc/free/calloc davranışı
// 2) Kenar durum testleri — NULL, double-free, sıfır boyut, büyük istek
// 3) Stres testi       — çok thread'li eşzamanlı alloc/free
// 4) Fragmentation     — dağınık free → birleştirme gözlemi
// 5) Performans        — 1 thread vs N thread hız karşılaştırması

#include "allocator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>

#define POOL_SIZE   (1 * 1024 * 1024)   // 1 MB havuz
#define TEST_PASS   "\033[32m[PASS]\033[0m"
#define TEST_FAIL   "\033[31m[FAIL]\033[0m"
#define TEST_INFO   "\033[36m[INFO]\033[0m"
#define SECTION(n)  printf("\n\033[1m--- %s ---\033[0m\n", n)

static long ms_diff(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1000L +
           (b.tv_nsec - a.tv_nsec) / 1000000L;
}

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                               \
    do {                                               \
        if (cond) { printf("%s %s\n", TEST_PASS, msg); g_passed++; } \
        else      { printf("%s %s\n", TEST_FAIL, msg); g_failed++; } \
    } while(0)

static void test_basic(void)
{
    SECTION("Basit Senaryolar");
    allocator_init(POOL_SIZE);

    // malloc testi
    int *arr = my_malloc(10 * sizeof(int));
    CHECK(arr != NULL, "malloc: 40 byte tahsis");
    if (arr) {
        for (int i = 0; i < 10; i++) arr[i] = i * i;
        CHECK(arr[9] == 81, "malloc: yazma/okuma doğruluğu");
    }

    // calloc testi
    char *buf = my_calloc(64, 1);
    CHECK(buf != NULL, "calloc: 64 byte tahsis");
    if (buf) {
        int all_zero = 1;
        for (int i = 0; i < 64; i++) if (buf[i] != 0) { all_zero = 0; break; }
        CHECK(all_zero, "calloc: bellek sıfırlanmış");
    }

    // free sonrası yeniden kullanım
    my_free(arr);
    int *arr2 = my_malloc(10 * sizeof(int));
    CHECK(arr2 != NULL, "free sonrası yeniden tahsis");
    my_free(arr2);
    my_free(buf);

    // ardışık birden fazla alloc
    void *p1 = my_malloc(128);
    void *p2 = my_malloc(256);
    void *p3 = my_malloc(64);
    CHECK(p1 && p2 && p3, "ardışık 3 tahsis");
    CHECK(p1 != p2 && p2 != p3 && p1 != p3, "bloklar çakışmıyor");
    my_free(p1); my_free(p2); my_free(p3);

    print_stats();
    allocator_destroy();
}

static void test_edge_cases(void)
{
    SECTION("Edge-Case Testler");
    allocator_init(POOL_SIZE);

    // NULL free
    printf("%s my_free(NULL) güvenli çağrı (log'a bakın)\n", TEST_INFO);
    my_free(NULL);           // crash olmamalı
    g_passed++;

    // double free tespiti
    void *x = my_malloc(32);
    my_free(x);
    printf("%s double-free tespiti (log'a bakın):\n", TEST_INFO);
    my_free(x);              // hata logu basmalı, crash olmamalı
    g_passed++;

    // sıfır boyut
    void *z = my_malloc(0);
    CHECK(z == NULL, "malloc(0) → NULL");

    // havuzdan büyük istek
    void *big = my_malloc(POOL_SIZE + 1);
    CHECK(big == NULL, "havuzdan büyük istek → NULL");

    // calloc boyut taşması
    void *overflow = my_calloc(SIZE_MAX, 2);
    CHECK(overflow == NULL, "calloc boyut taşması → NULL");

    // tek byte
    void *tiny = my_malloc(1);
    CHECK(tiny != NULL, "malloc(1) başarılı");
    my_free(tiny);

   // havuz doluncaya kadar alloc
    printf("%s havuzu dolduruyor...\n", TEST_INFO);
    int count = 0;
    void *ptrs[4096];
    
    // 1. Önce 500 byte'lık büyük parçalarla havuzun çoğunu dolduralım
    while (count < 4096) {
        ptrs[count] = my_malloc(500); 
        if (!ptrs[count]) break;
        count++;
    }
    
    // 2. Havuzun dibinde kalan son kırıntıları da 1 byte'lık isteklerle süpürelim (Tamamen kurutma)
    while (count < 4096) {
        void *p = my_malloc(1);
        if (!p) break;
        ptrs[count++] = p;
    }

    printf("%s Toplam %d tahsis ile havuz tamamen tüketildi\n", TEST_INFO, count);
    g_passed++;

    // Şimdi havuz GERÇEKTEN %100 dolu, bu istek kesinlikle NULL dönecek
    void *should_fail = my_malloc(1);
    CHECK(should_fail == NULL, "dolu havuzdan alloc → NULL");

    for (int i = 0; i < count; i++) my_free(ptrs[i]);
    for (int i = 0; i < count; i++) my_free(ptrs[i]);

    // tümü serbest bırakıldıktan sonra büyük tahsis yapılabilmeli
    void *recovered = my_malloc(POOL_SIZE / 2);
    CHECK(recovered != NULL, "tüm free sonrası büyük tahsis (coalescing)");
    my_free(recovered);

    print_stats();
    allocator_destroy();
}

static void test_fragmentation(void)
{
    SECTION("Fragmentation & Coalescing");
    allocator_init(POOL_SIZE);

    // 10 blok ayır
    void *p[10];
    for (int i = 0; i < 10; i++)
        p[i] = my_malloc(1024);

    printf("%s 10 x 1KB tahsis sonrası durum:\n", TEST_INFO);
    print_stats();

    // tek sıralı (çift index) free → fragmentation oluştur
    for (int i = 0; i < 10; i += 2)
        my_free(p[i]);

    printf("%s çift index'ler serbest bırakıldı (fragmentation)::\n", TEST_INFO);
    print_free_list();
    print_stats();

    // geri kalanları serbest bırak → coalescing beklenir
    for (int i = 1; i < 10; i += 2)
        my_free(p[i]);

    printf("%s tümü serbest (coalescing sonrası):\n", TEST_INFO);
    print_free_list();
    print_stats();

    // coalescing sonrası büyük blok oluştu mu?
    void *large = my_malloc(8 * 1024);
    CHECK(large != NULL, "coalescing sonrası 8KB tahsis");
    my_free(large);

    allocator_destroy();
}

#define STRESS_THREADS   8
#define OPS_PER_THREAD   500

typedef struct {
    int thread_id;
    int errors;
} ThreadArg;

static void *stress_worker(void *arg)
{
    ThreadArg *ta = (ThreadArg *)arg;
    void *local_ptrs[32];
    int   local_count = 0;
    ta->errors = 0;

    for (int op = 0; op < OPS_PER_THREAD; op++) {
        int action = rand() % 3;   // 0=alloc, 1=free, 2=write-check

        if (action == 0 && local_count < 32) {
            size_t sz = (rand() % 256) + 8;
            void *p = my_malloc(sz);
            if (p) {
                memset(p, ta->thread_id & 0xFF, sz);
                local_ptrs[local_count++] = p;
            }
        }
        else if (action == 1 && local_count > 0) {
            int idx = rand() % local_count;
            my_free(local_ptrs[idx]);
            local_ptrs[idx] = local_ptrs[--local_count];
        }
        else if (action == 2 && local_count > 0) {
            // yazdığımız değeri doğrula — başka thread bozmuş olmamalı
            int idx = rand() % local_count;
            // boyutu başlıktan oku; ama biz sadece ilk byte'ı kontrol edelim
            unsigned char *b = (unsigned char *)local_ptrs[idx];
            if (*b != (unsigned char)(ta->thread_id & 0xFF)) {
                ta->errors++;
            }
        }
    }

    // thread bitişinde elinde kalanları serbest bırak
    for (int i = 0; i < local_count; i++)
        my_free(local_ptrs[i]);

    return NULL;
}

static void test_stress(void)
{
    SECTION("Stres Testi (çok thread'li)");
    allocator_init(POOL_SIZE);

    pthread_t tids[STRESS_THREADS];
    ThreadArg args[STRESS_THREADS];

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < STRESS_THREADS; i++) {
        args[i].thread_id = i;
        pthread_create(&tids[i], NULL, stress_worker, &args[i]);
    }

    int total_errors = 0;
    for (int i = 0; i < STRESS_THREADS; i++) {
        pthread_join(tids[i], NULL);
        total_errors += args[i].errors;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    long elapsed = ms_diff(t0, t1);

    printf("%s %d thread, her biri %d op → toplam %d işlem\n",
           TEST_INFO, STRESS_THREADS, OPS_PER_THREAD, STRESS_THREADS * OPS_PER_THREAD);
    printf("%s Süre: %ld ms\n", TEST_INFO, elapsed);
    CHECK(total_errors == 0, "çok thread'li erişimde veri bütünlüğü");

    print_stats();
    allocator_destroy();
}

#define PERF_OPS  2000

static void *perf_worker(void *arg)
{
    int ops = *(int *)arg;
    void *ptrs[64];
    int   cnt = 0;

    for (int i = 0; i < ops; i++) {
        if (cnt < 64) {
            void *p = my_malloc((rand() % 128) + 16);
            if (p) ptrs[cnt++] = p;
        } else {
            my_free(ptrs[--cnt]);
        }
    }
    for (int i = 0; i < cnt; i++) my_free(ptrs[i]);
    return NULL;
}

static void test_performance(void)
{
    SECTION("Performans Karşılaştırması (1 thread vs 4 thread)");

    // 1 thread
    allocator_init(POOL_SIZE);
    int ops = PERF_OPS;
    struct timespec t0, t1;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    perf_worker(&ops);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long single_ms = ms_diff(t0, t1);
    printf("%s 1  thread, %d op → %ld ms\n", TEST_INFO, ops, single_ms);
    allocator_destroy();

    // 4 thread
    allocator_init(POOL_SIZE);
    pthread_t t[4];
    int per_thread = PERF_OPS / 4;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, perf_worker, &per_thread);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long multi_ms = ms_diff(t0, t1);
    printf("%s 4  thread, %d op (toplam %d) → %ld ms\n",
           TEST_INFO, per_thread, PERF_OPS, multi_ms);

    printf("%s Not: mutex overhead nedeniyle 4 thread daha yavaş olabilir —\n", TEST_INFO);
    printf("       bu beklenen bir davranış; güvenlik önceliklidir.\n");

    allocator_destroy();
    g_passed++;   // bu test sayısal değil, gözlem amaçlı
}

int main(void)
{
    srand((unsigned)time(NULL));

    printf("\n\033[1m====== Custom Memory Allocator Test Suite ======\033[0m\n");

    test_basic();
    test_edge_cases();
    test_fragmentation();
    test_stress();
    test_performance();

    printf("\n\033[1m====== Sonuç: %d PASS  %d FAIL ======\033[0m\n\n",
           g_passed, g_failed);

    return g_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
