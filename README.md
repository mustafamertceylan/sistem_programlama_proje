# Custom Memory Allocator

Bu projede malloc, calloc ve free mantığını anlamak için basit bir memory allocator geliştirildi.

## Kullanılan yapı

- Bellek başlangıçta tek parça olarak oluşturulur
- İstek geldikçe uygun blok bulunur
- Gerekirse blok bölünür
- Free edilen bloklar tekrar listeye eklenir
- Uygun durumlarda komşu bloklar birleştirilir

## Kullanılan yöntemler

- Best-fit allocation
- Block splitting
- Block coalescing
- Mutex ile thread safety

## Çalıştırma

```bash
make
./test_allocator
```

Valgrind:

```bash
make valgrind
```

Temizleme:

```bash
make clean
```

## Testler

- malloc/free testleri
- calloc testleri
- fragmentation testleri
- thread testleri
- performans testleri
