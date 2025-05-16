#include "grci.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>


int main(int argc, char **argv) {
    struct grci *g = grci_init(malloc, realloc, free);
    //Nand modules are built-in
    const char *src = "module Not(in) -> out { Nand(in, in) -> out } module "
                      "And(a, b) -> out { Nand(a, b) -> temp Not(temp) -> out }";
    grci_compile_src(g, src, strlen(src));
    const char *not_module = "And";
    struct grci_module *m = grci_init_module(g, not_module, strlen(not_module));

    m->inputs[0] = 0;
    m->inputs[1] = 0;
    grci_step_module(m);
    printf("expecting 0: %d\n", m->outputs[0]);

    m->inputs[0] = 0;
    m->inputs[1] = 1;
    grci_step_module(m);
    printf("expecting 0: %d\n", m->outputs[0]);

    m->inputs[0] = 1;
    m->inputs[1] = 0;
    grci_step_module(m);
    printf("expecting 0: %d\n", m->outputs[0]);

    m->inputs[0] = 1;
    m->inputs[1] = 1;
    grci_step_module(m);
    printf("expecting 1: %d\n", m->outputs[0]);

    grci_destroy_module(m);
    grci_cleanup(g);
    return 0;
}
