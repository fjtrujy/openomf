/*
 * Tiny host-side generator for the one-record OpenOMF extension language file.
 * It avoids requiring the desktop OpenGL/SDL_mixer dependencies just to build
 * the PSP port. The format mirrors formats/language.c and tools/languagetool.
 *
 * Usage: create_language2 ENGLISH2.TXT ENGLISH.DAT2
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void strip_eol(char *s) {
    s[strcspn(s, "\r\n")] = 0;
}

int main(int argc, char **argv) {
    if(argc != 3) {
        fprintf(stderr, "Usage: %s INPUT.TXT OUTPUT.DAT2\n", argv[0]);
        return 1;
    }
    FILE *in = fopen(argv[1], "rb");
    if(!in) {
        perror(argv[1]);
        return 1;
    }
    char id[64], title[256], data[2048];
    if(!fgets(id, sizeof(id), in) || !fgets(title, sizeof(title), in) ||
       !fgets(data, sizeof(data), in)) {
        fprintf(stderr, "Missing OpenOMF language fields in %s\n", argv[1]);
        fclose(in);
        return 1;
    }
    fclose(in);
    strip_eol(id);
    strip_eol(title);
    strip_eol(data);
    const char *id_prefix = "ID: ";
    const char *title_prefix = "Title: ";
    const char *data_prefix = "Data: ";
    if(strcmp(id, "ID: 0") != 0 || strncmp(title, title_prefix, strlen(title_prefix)) ||
       strncmp(data, data_prefix, strlen(data_prefix))) {
        fprintf(stderr, "Expected a single ID: 0 / Title / Data record\n");
        return 1;
    }
    const char *title_value = title + strlen(title_prefix);
    const char *data_value = data + strlen(data_prefix);
    const size_t title_len = strlen(title_value);
    const size_t data_len = strlen(data_value);
    if(title_len > 31 || !data_len || data_len > 255) {
        fprintf(stderr, "Invalid language record lengths\n");
        return 1;
    }
    FILE *out = fopen(argv[2], "wb");
    if(!out) {
        perror(argv[2]);
        return 1;
    }
    const uint8_t offset[4] = {36, 0, 0, 0};
    uint8_t desc[32] = {0};
    memcpy(desc, title_value, title_len);
    if(fwrite(offset, 1, sizeof(offset), out) != sizeof(offset) ||
       fwrite(desc, 1, sizeof(desc), out) != sizeof(desc)) {
        fclose(out);
        return 1;
    }
    for(size_t i = 0; i < data_len; i++) {
        const unsigned char c = (unsigned char)data_value[i];
        if(c > 127) {
            fprintf(stderr, "Non-ASCII translation; use original languagetool for CP437 conversion\n");
            fclose(out);
            remove(argv[2]);
            return 1;
        }
        const uint8_t encoded = c ^ (uint8_t)(data_len + i);
        if(fwrite(&encoded, 1, 1, out) != 1) {
            fclose(out);
            return 1;
        }
    }
    if(fclose(out)) {
        perror(argv[2]);
        return 1;
    }
    return 0;
}
