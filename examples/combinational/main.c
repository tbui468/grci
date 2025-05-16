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

int main(int argc, char **argv) {
    struct grci *g = grci_init(malloc, realloc, free);
    size_t size;
    char *src = read_file("modules.hdl", &size);
    grci_compile_src(g, src, size);
    const char *not_module = "Add8";
    struct grci_module *m = grci_init_module(g, not_module, strlen(not_module));

    bool a[] = { 0, 0, 0, 0, 0, 1, 0, 0 }; //32
    bool b[] = { 0, 1, 0, 1, 0, 0, 0, 0 }; //10

    for (int i = 0; i < 8; i++) {
        m->inputs[i] = a[i];
        m->inputs[i + 8] = b[i];
    }


    grci_step_module(m);
    printf("expecting 42: ");
    for (int i = 7; i >= 0; i--) {
        printf("%d", m->outputs[i]);
    }
    printf("\n");

    grci_destroy_module(m);
    grci_cleanup(g);
    free(src);
    return 0;
}
