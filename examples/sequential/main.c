#include "grci.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char* read_file(const char* path, size_t *size) {
    FILE *file = fopen(path, "rb");
    fseek(file, 0L, SEEK_END);
    size_t s = ftell(file);
    rewind(file);
    char *buf = malloc(s + 1);
    size_t read = fread(buf, sizeof(char), s, file);
    buf[read] = '\0';
    fclose(file);
    *size = s;
    return buf;
}

static void print_register(struct grci_module *m) {
    for (int i = 0; i < 8; i++) {
        printf("%d", m->outputs[i]);
    }
    printf("\n");
}

//module Register(in[8], load) -> out[8] {
int main(int argc, char **argv) {
    struct grci *g = grci_init(malloc, realloc, free);

    size_t size;
    char *src = read_file("modules.hdl", &size);
    grci_compile_src(g, src, size);
    const char *not_module = "Register";
    struct grci_module *m = grci_init_module(g, not_module, strlen(not_module));


    static bool b[] = { 1, 0, 1, 0, 0, 0, 0, 0 }; //5
    for (int i = 0; i < 8; i++) {
        m->inputs[i] = b[i];
    }
    bool load[] = { 0, 0, 0, 0, 1, 1, 0, 0, 0, 0 };

    for (int i = 0; i < 10; i++) {
        m->inputs[8] = load[i];
        int clock = grci_step_module(m);
        printf("clock level: %d    load: %d    ", clock, m->inputs[8]);
        print_register(m);
    }

    grci_destroy_module(m);
    grci_cleanup(g);
    free(src);
    return 0;
}
