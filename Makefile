CC      = gcc
CFLAGS  = -Wall -Wextra -g -pthread
TARGET  = test_allocator
SRCS    = allocator.c test.c
 
all: $(TARGET)
 
$(TARGET): $(SRCS) allocator.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)
 
# Valgrind ile sızıntı kontrolü
valgrind: $(TARGET)
	valgrind --leak-check=full --track-origins=yes ./$(TARGET)
 
clean:
	rm -f $(TARGET)
 
.PHONY: all clean valgrind
 

