// allocator.c  —  Kullanıcı alanı bellek yöneticisi
// Tasarım kararları:
// - Tek bir büyük bellek havuzu (pool) başta sbrk/mmap yerine malloc ile
// alınır; bu sayede valgrind altında da rahat test edilebilir.
// - Serbest bloklar çift-yönlü bağlı liste ile tutulur.
// - Yerleştirme stratejisi: best-fit (en küçük yeterli boşluk).
// - Bloklar birleştirilir (coalescing): my_free çağrısında komşu serbest
// bloklar varsa tek bloğa indirilir; bu fragmentation'ı azaltır.
// - Çok thread'li erişimi korumak için her kritik bölüm mutex ile sarılır.
// - Çifte free ve geçersiz pointer tespiti için basit bir magic-number
// mekanizması kullanılır.

#include "allocator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>

#define MAGIC_FREE  0xDEADBEEF   // serbest blok işaretçisi
#define MAGIC_USED  0xCAFEBABE   // dolu  blok işaretçisi
#define MIN_SPLIT   32           // bölme için gereken minimum artık (byte)

typedef struct BlockHeader {
    size_t              size;
    uint32_t            magic;
    struct BlockHeader *prev;
    struct BlockHeader *next;
} BlockHeader;

#define HEADER_SIZE  (sizeof(BlockHeader))

static void        *g_pool       = NULL;   // ham bellek havuzu
static size_t       g_pool_size  = 0;      // havuz boyutu
static BlockHeader *g_free_list  = NULL;   // serbest blok listesinin başı
static pthread_mutex_t g_lock    = PTHREAD_MUTEX_INITIALIZER;

// istatistik sayaçları
static size_t g_total_allocs   = 0;
static size_t g_total_frees    = 0;
static size_t g_total_req_bytes= 0;
static size_t g_current_used   = 0;

static void log_msg(const char *level, const char *fmt, ...)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", t);

    fprintf(stderr, "[%s][%s] ", tbuf, level);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fputc('\n', stderr);
}

static int block_is_valid(BlockHeader *b)
{
    if (b == NULL) return 0;
    // havuzun içinde mi?
    uintptr_t start = (uintptr_t)g_pool;
    uintptr_t end   = start + g_pool_size;
    uintptr_t addr  = (uintptr_t)b;
    if (addr < start || addr + HEADER_SIZE > end) return 0;
    // magic tutarlı mı?
    if (b->magic != MAGIC_FREE && b->magic != MAGIC_USED) return 0;
    return 1;
}

static void freelist_insert(BlockHeader *b)
{
    b->magic = MAGIC_FREE;
    b->prev  = NULL;
    b->next  = NULL;

    if (g_free_list == NULL) {
        g_free_list = b;
        return;
    }

    // Adres sırasıyla ekle — coalescing için şart
    BlockHeader *cur = g_free_list;
    BlockHeader *prev_node = NULL;

    while (cur && cur < b) {
        prev_node = cur;
        cur = cur->next;
    }

    b->next = cur;
    b->prev = prev_node;

    if (prev_node)  prev_node->next = b;
    else            g_free_list = b;

    if (cur) cur->prev = b;
}

static void freelist_remove(BlockHeader *b)
{
    if (b->prev) b->prev->next = b->next;
    else         g_free_list   = b->next;

    if (b->next) b->next->prev = b->prev;

    b->prev = NULL;
    b->next = NULL;
}

static BlockHeader *coalesce(BlockHeader *b)
{
    // Sağdaki komşunun adresi
    uintptr_t right_addr = (uintptr_t)b + HEADER_SIZE + b->size;
    BlockHeader *right   = (BlockHeader *)right_addr;

    // Sağ komşu serbest mi?
    if (block_is_valid(right) && right->magic == MAGIC_FREE) {
        b->size += HEADER_SIZE + right->size;   // boyutu genişlet
        freelist_remove(right);                 // sağı listeden çıkar
        log_msg("DEBUG", "coalesce: sağ komşu birleştirildi, yeni boyut=%zu", b->size);
    }

    // Sol komşuyu listede ara (prev zaten sıralı)
    if (b->prev != NULL) {
        uintptr_t expected_right = (uintptr_t)b->prev + HEADER_SIZE + b->prev->size;
        if (expected_right == (uintptr_t)b) {
            BlockHeader *left = b->prev;
            left->size += HEADER_SIZE + b->size;
            freelist_remove(b);
            log_msg("DEBUG", "coalesce: sol komşu birleştirildi, yeni boyut=%zu", left->size);
            return left;
        }
    }

    return b;
}

void allocator_init(size_t pool_size)
{
    pthread_mutex_lock(&g_lock);

    if (g_pool != NULL) {
        log_msg("WARN", "allocator zaten başlatılmış, yeniden başlatılıyor");
        free(g_pool);
    }

    g_pool = malloc(pool_size);
    if (g_pool == NULL) {
        log_msg("ERROR", "havuz oluşturulamadı: %zu byte talep edildi", pool_size);
        pthread_mutex_unlock(&g_lock);
        return;
    }

    memset(g_pool, 0, pool_size);
    g_pool_size = pool_size;

    // Havuzun tamamını tek bir serbest blok yap
    BlockHeader *initial = (BlockHeader *)g_pool;
    initial->size  = pool_size - HEADER_SIZE;
    initial->magic = MAGIC_FREE;
    initial->prev  = NULL;
    initial->next  = NULL;
    g_free_list    = initial;

    g_total_allocs    = 0;
    g_total_frees     = 0;
    g_total_req_bytes = 0;
    g_current_used    = 0;

    log_msg("INFO", "allocator başlatıldı: pool=%zu byte, header=%zu byte",
            pool_size, HEADER_SIZE);

    pthread_mutex_unlock(&g_lock);
}

void allocator_destroy(void)
{
    pthread_mutex_lock(&g_lock);

    if (g_pool) {
        free(g_pool);
        g_pool      = NULL;
        g_pool_size = 0;
        g_free_list = NULL;
        log_msg("INFO", "allocator kapatıldı");
    }

    pthread_mutex_unlock(&g_lock);
}

void *my_malloc(size_t size)
{
    if (size == 0) {
        log_msg("WARN", "my_malloc(0) çağrıldı, NULL döndürülüyor");
        return NULL;
    }

    // Hizalama: 8 byte'ın katına yuvarla
    size = (size + 7) & ~(size_t)7;

    pthread_mutex_lock(&g_lock);

    // Best-fit: tüm serbest blokları tara, en küçük yeterlisini bul
    BlockHeader *best = NULL;
    BlockHeader *cur  = g_free_list;

    while (cur != NULL) {
        if (cur->size >= size) {
            if (best == NULL || cur->size < best->size) {
                best = cur;
                // tam eşleşme — daha iyisini bulamayız
                if (best->size == size) break;
            }
        }
        cur = cur->next;
    }

    if (best == NULL) {
        log_msg("ERROR", "my_malloc: yeterli alan yok (%zu byte istendi)", size);
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    // Bloğu böl — artık MIN_SPLIT'ten büyükse
    if (best->size >= size + HEADER_SIZE + MIN_SPLIT) {
        BlockHeader *remainder   = (BlockHeader *)((uintptr_t)best + HEADER_SIZE + size);
        remainder->size  = best->size - size - HEADER_SIZE;
        remainder->magic = MAGIC_FREE;
        remainder->prev  = NULL;
        remainder->next  = NULL;

        freelist_remove(best);
        freelist_insert(remainder);   // artığı listeye ekle
        best->size = size;
        log_msg("DEBUG", "blok bölündü: kullanılan=%zu, artık=%zu", size, remainder->size);
    } else {
        freelist_remove(best);
    }

    best->magic = MAGIC_USED;
    best->prev  = NULL;
    best->next  = NULL;

    g_total_allocs++;
    g_total_req_bytes += size;
    g_current_used    += size;

    log_msg("INFO", "my_malloc(%zu) → %p", size, (void *)(best + 1));

    pthread_mutex_unlock(&g_lock);
    return (void *)(best + 1);   // başlığın hemen ardındaki adres
}

void *my_calloc(size_t nmemb, size_t size)
{
    // taşma kontrolü
    if (nmemb != 0 && size > SIZE_MAX / nmemb) {
        log_msg("ERROR", "my_calloc: boyut taşması (%zu * %zu)", nmemb, size);
        return NULL;
    }

    size_t total = nmemb * size;
    void  *ptr   = my_malloc(total);

    if (ptr) {
        memset(ptr, 0, total);   // sıfırla
        log_msg("INFO", "my_calloc(%zu, %zu) → %p", nmemb, size, ptr);
    }

    return ptr;
}

void my_free(void *ptr)
{
    if (ptr == NULL) {
        log_msg("WARN", "my_free(NULL) çağrıldı, görmezden geliniyor");
        return;
    }

    BlockHeader *b = (BlockHeader *)ptr - 1;

    pthread_mutex_lock(&g_lock);

    // Geçersiz pointer mu?
    if (!block_is_valid(b)) {
        log_msg("ERROR", "my_free: geçersiz pointer %p (havuz dışı?)", ptr);
        pthread_mutex_unlock(&g_lock);
        return;
    }

    // Çifte free mi?
    if (b->magic == MAGIC_FREE) {
        log_msg("ERROR", "my_free: çifte serbest bırakma tespit edildi! ptr=%p", ptr);
        pthread_mutex_unlock(&g_lock);
        return;
    }

    if (b->magic != MAGIC_USED) {
        log_msg("ERROR", "my_free: bozuk magic number (0x%X) ptr=%p", b->magic, ptr);
        pthread_mutex_unlock(&g_lock);
        return;
    }

    g_total_frees++;
    if (g_current_used >= b->size)
        g_current_used -= b->size;
    else
        g_current_used = 0;

    log_msg("INFO", "my_free(%p), boyut=%zu", ptr, b->size);

    freelist_insert(b);
    coalesce(b);   // komşuları birleştirmeyi dene

    pthread_mutex_unlock(&g_lock);
}

void print_stats(void)
{
    pthread_mutex_lock(&g_lock);

    size_t free_bytes  = 0;
    size_t free_blocks = 0;
    size_t largest_free = 0;

    BlockHeader *cur = g_free_list;
    while (cur) {
        free_bytes += cur->size;
        free_blocks++;
        if (cur->size > largest_free) largest_free = cur->size;
        cur = cur->next;
    }

    size_t used_bytes = g_current_used;
    double frag = 0.0;

    // Harici fragmentation oranı:
// 1 - (en_büyük_serbest_blok / toplam_serbest_alan)
// 0 = hiç fragmentation yok, 1 = tamamen parçalanmış
    if (free_bytes > 0)
        frag = 1.0 - ((double)largest_free / (double)free_bytes);

    printf("\n========== Allocator İstatistikleri ==========\n");
    printf("  Havuz boyutu       : %zu byte\n", g_pool_size);
    printf("  Kullanılan         : %zu byte\n", used_bytes);
    printf("  Serbest            : %zu byte\n", free_bytes);
    printf("  Serbest blok sayısı: %zu\n", free_blocks);
    printf("  En büyük serbest   : %zu byte\n", largest_free);
    printf("  Toplam malloc çağrısı: %zu\n", g_total_allocs);
    printf("  Toplam free çağrısı  : %zu\n", g_total_frees);
    printf("  Harici fragmentation : %.2f%%\n", frag * 100.0);
    printf("==============================================\n\n");

    pthread_mutex_unlock(&g_lock);
}

void print_free_list(void)
{
    pthread_mutex_lock(&g_lock);

    printf("\n--- Serbest Blok Listesi ---\n");
    BlockHeader *cur = g_free_list;
    int i = 0;
    while (cur) {
        printf("  [%d] adres=%p  boyut=%-8zu  sonraki=%p\n",
               i++, (void *)cur, cur->size, (void *)cur->next);
        cur = cur->next;
    }
    if (i == 0) printf("  (liste boş)\n");
    printf("----------------------------\n\n");

    pthread_mutex_unlock(&g_lock);
}
