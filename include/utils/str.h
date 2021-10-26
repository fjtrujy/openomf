#ifndef _UTIL_STRING_H
#define _UTIL_STRING_H

#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

typedef struct {
    size_t len;
    char* data;
} str;

#define str_free(ptr) _str_free((&(ptr)));

/**
 * @brief Create an empty string object.
 * @details Creates a new, empty string object. After creating the object, it must be freed
 *          using the str_free. 
 * 
 * @param string Target string to initialize
 */
void str_create(str *string);

/**
 * @brief Free string object
 * @details Frees up any memory used by the string object. Usage of the string after freeing it
 *          is undefined behaviour.
 * 
 * @param string String to free.
 */
void _str_free(str **string);

// After creation, these can be used to set the data
// Destination string is always cleared (if already used)
void str_copy_c(str *dst, const char *cstr);
void str_copy_buf(str *dst, const char *buf, size_t len);
void str_copy_str(str *dst, const str *src);
void str_format(str *dst, const char *format, ...);
void str_toupper(str *dst, str *src);
void str_tolower(str *dst, str *src);
void str_rstrip(str *dst, str *src);
void str_lstrip(str *dst, str *src);
void str_strip(str *dst, str *src);
void str_slice(str *dst, const str *src, size_t start, size_t end);

// Information getters
size_t str_size(const str *string);
int str_first_of(const str *string, char find, size_t *pos);
int str_last_of(const str *string, char find, size_t *pos);
bool str_equal(const str *string, const str *string_b);
bool str_equal_c(const str *string, const char *string_b);
char str_at(const str *string, size_t pos);

// Allows joining multiple str's or C strings to target string
// Replaces append, prepend, etc.
void str_join(str *dst, ...):

// Data type conversion
bool str_to_int(const str *string, int *m);
bool str_to_long(const str *string, long *m);
bool str_to_float(const str *string, float *m);
bool str_to_bool(const str *string, bool *m);

// Compatibility with C strings
const char* str_c(const str *src);
const char* str_c_alloc(const str *src);

#endif // _UTIL_STRING_H
