#ifndef STRING_H
#define STRING_H

#include <stddef.h>

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
char  *strcpy(char *dst, const char *src);
int    memcmp(const void *a, const void *b, size_t n);
void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
const char *strstr_ci(const char *hay, const char *needle);   /* bez rozrozniania wielkosci liter */

/* Formatowanie do bufora - jak kprintf (%s %c %u %d %x, np. %02u). Zawsze konczy '\0'. */
int ksnprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#include <stdarg.h>
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#endif
