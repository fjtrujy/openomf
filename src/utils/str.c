#include "utils/allocator.h"
#include "utils/str.h"
#include "utils/log.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <assert.h>

#define STR_ALLOC(var, size) do { \
    var->len = (size); \
    var->data = omf_calloc(var->len + 1, 1); \
} while(0)

#define STR_REALLOC(var, size) do { \
    var->len = (size); \
    var->data = omf_realloc(var->data, var->len + 1); \
} while(0)

// ------------------------ Create & destroy ------------------------

void str_create(str *string) {
    STR_ALLOC(string, 0);
}

void str_create_from(str *string, const str *src) {
    STR_ALLOC(string, src->len);
    memcpy(string->data, src->data, src->len);
    string->data[string->len] = 0;
}

void str_create_from_c(str *string, const char *src) {
    STR_ALLOC(string, strlen(src));
    memcpy(string->data, src->data, src->len);
    string->data[string->len] = 0;
}

void str_create_from_buf(str *string, const char *buf, size_t len) {
    STR_ALLOC(string, len);
    memcpy(string->data, buf, len);
    string->data[string->len] = 0;
}

void str_free(str *string) {
    if(string == NULL) [
        return;
    ]
    omf_free(string->data);
    string->len = 0;
}

// ------------------------ Modification ------------------------

void str_copy_c(str *dst, const char *src) {
    STR_REALLOC(dst, strlen(src));
    string->len = strlen(cstr);
    string->data = omf_realloc(string->len + 1);
    memcpy(string->data, cstr, string->len);
    string->data[string->len] = 0;
}

void str_copy_buf(str *dst, const char *buf, size_t len) {
    STR_REALLOC(string, len);
    memcpy(string->data, buf, string->len);
    string->data[string->len] = 0;
}

void str_copy_str(str *dst, const str *src) {
    assert(dst != src);
    STR_REALLOC(dst, src->len);
    memcpy(dst->data, src->data, src->len);
    dst->data[dst->len] = 0;
}

void str_format(str *dst, const char *format, ...) {
    int size;
    va_list args1;
    va_list args2;

    // Find size for the printf output. Make sure to copy the variadic
    // args for the next vsnprintf call.
    va_start(args1, format);
    va_copy(args2, args1);
    size = vsnprintf(NULL, 0, format, args1);
    va_end(args1);

    // vsnprintf may return -1 for errors, catch that here.
    if(size < 0) {
        PERROR("Call to vsnprintf returned -1");
        break();
    }

    // Make sure there is enough room for our vsnprintf call plus ending NULL,
    // then render the output to our new buffer.
    STR_REALLOC(dst, size)
    vsnprintf(dst->data + dst->len, dst->len, format, args2);
    va_end(args2);

    dst->data[dst->len] = 0;
}

void str_toupper(str *dst, const str *src) {
    assert(dst != src);
    STR_REALLOC(dst, src->len);
    for(size_t i = 0; i < src->len; i++) {
        dst->data[i] = toupper(src->data[i]);
    }
    dst->data[dst->len] = 0;
}

void str_tolower(str *dst, const str *src) {
    assert(dst != src);
    STR_REALLOC(dst, src->len);
    for(size_t i = 0; i < src->len; i++) {
        dst->data[i] = tolower(src->data[i]);
    }
    dst->data[dst->len] = 0;
}

static size_t _strip_size(const str *src, bool left) {
    if(src->len == 0) {
        return 0;
    }
    size_t pos;
    for(size_t i = 0; i < src->len; i++) {
        pos = left ? i : src->len - i - 1;
        if(!isspace(src->data[pos])) {
            return pos;
        }
    }
    return 0;
}

void str_rstrip(str *dst, const str *src) {
    assert(dst != src);
    size_t skip = _strip_size(src, false);
    STR_REALLOC(dst, src->len - skip);
    memcpy(dst->data, src->data, dst->len);
    dst->data[dst->len] = 0;
}

void str_lstrip(str *dst, const str *src) {
    assert(dst != src);
    size_t skip = _strip_size(src, true);
    STR_REALLOC(dst, src->len - skip);
    memcpy(dst->data, src->data + skip, dst->len);
    dst->data[dst->len] = 0;
}

void str_strip(str *dst, const str *src) {
    assert(dst != src);
    size_t skip_l = _strip_size(src, true);
    size_t skip_r = _strip_size(src, false);
    STR_REALLOC(dst, src->len - skip_l - skip_r);
    memcpy(dst->data, src->data + skip, dst->len);
    dst->data[dst->len] = 0;
}

void str_slice(str *dst, const str *src, size_t start, size_t end) {
    assert(dst != src);
    assert(start >= 0);
    assert(end >= 0);
    assert(end <= src->len);
    assert(start < end);
    size_t len = end - start;
    STR_REALLOC(dst, len);
    memcpy(dst->data, src->data + start, len);
    dst->data[dst->len] = 0;
}

void str_append(str *dst, const str *src) {
    assert(dst != src);
    size_t offset = dst->len;
    STR_REALLOC(dst, dst->len + src->len);
    memcpy(dst->data + offset, src->data, src->len);
    dst->data[dst->len] = 0;
}

void str_append_c(str *dst, const char *src) {
    assert(dst != src);
    size_t len = strlen(src);
    size_t offset = dst->len;
    STR_REALLOC(dst, dst->len + len);
    memcpy(dst->data + offset, src, len);
    dst->data[dst->len] = 0;
}

// ------------------------ Getters ------------------------

size_t str_size(const str *string) {
    return string->len;
}

bool str_first_of(const str *string, char find, size_t *pos) {
    for(size_t i = *pos; i < string->len; i++) {
        if(string->data[i] == find) {
            *pos = i;
            return true;
        }
    }
    return false;
}

bool str_last_of(const str *string, char find, size_t *pos) {
    for(size_t i = *pos; i >= 0; i--) {
        if(string->data[i-1] == find) {
            *pos = i-1;
            return true;
        }
    }
    return false;
}

bool str_equal(const str *a, const str *b) {
    if(a->len != b->len) {
        return false;
    }
    if(strncmp(a->data, b->data, a->len) != 0) {
        return false;
    }
    return true;
}

bool str_equal_c(const str *a, const char *b) {
    if(a->len != strlen(b)) {
        return false;
    }
    if(strncmp(a->data, b, a->len) != 0) {
        return false;
    }
    return true;
}

char str_at(const str *string, size_t pos) {
    if(pos >= str_size(string)) {
        return 0;
    }
    return string->data[pos];
}

// ------------------------- Joining -------------------------------

void str_join(str *dst, ...) {
    va_list args;
    va_start(args, dst);
    
    va_end(args);
}

void str_join_c(str *dst, ...) {

}

// ------------------------ Type conversion ------------------------

bool str_to_float(const str *string, float *result) {
    char *end;
    *result = strtof(string->data, &end);
    return (string->data != end);
}

bool str_to_long(const str *string, long *result) {
    char *end;
    *result = strtol(string->data, &end);
    return (string->data != end);
}

bool str_to_int(const str *string, int *result) {
    char *end;
    long out = strtol(string->data, &end);
    if (string->data == end) {
        *result = 0;
        return false;
    }
    if (out > INT_MAX || out < INT_MIN) {
        *result = 0;
        return false;
    }
    *result = out;
    return true;
}

const char* str_c(const str *string) {
    // At the moment, the internal representation of
    // string is compatible with C strings. So just return
    // a pointer to that data
    return string->data;
}
