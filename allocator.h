#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stddef.h>

// allocator fonksiyonları
void  allocator_init(size_t pool_size);
void  allocator_destroy(void);

void *my_malloc(size_t size);
void *my_calloc(size_t nmemb, size_t size);
void  my_free(void *ptr);

// debug fonksiyonları
void  print_stats(void);
void  print_free_list(void);

#endif
