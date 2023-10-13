#pragma once

#include <stdbool.h>

struct ring {
  size_t size;
  char* data;
  char* ptr_read, *ptr_write;
};

int ring_alloc(struct ring** ret, size_t size);
void ring_free(struct ring* ring);


size_t ring_available(struct ring* ring);
size_t ring_available_contig(struct ring* ring);
size_t ring_free_space(struct ring* ring);
size_t ring_free_space_contig(struct ring* ring);

int ring_peek(struct ring* ring, char* data, size_t len);
int ring_read(struct ring* ring, char* data, size_t len);
int ring_write(struct ring* ring, char* data, size_t len);
void ring_advance_read(struct ring* ring, off_t offset);
void ring_advance_write(struct ring* ring, off_t offset);

// Special zero-copy optimizations
int ring_memcmp(struct ring*, char* ref, unsigned int len, char** next_pos);

// Optimized inline funnctions
bool ring_any_available(struct ring* ring);
// Take a small peek into the buffer
char ring_peek_one(struct ring* ring);

// Pointer to next byte to read from ringbuffer
char* ring_next(struct ring* ring, char* ptr);

// Read one byte from the buffer
char ring_read_one(struct ring* ring);

void ring_inc_read(struct ring* ring);
