#pragma once

typedef unsigned int size_t;

int          atoi(const char *s);
unsigned int atou(const char *s);
int          abs(int n);

void *malloc(size_t size);
void  free(void *ptr);
void *realloc(void *ptr, size_t size);

/* Return pointer to value for name in the task's environment, or NULL.
 * Uses a static 128-byte buffer — not thread-safe (no threads in Luminary). */
char *getenv(const char *name);
