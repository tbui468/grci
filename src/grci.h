#ifndef GRCI_H
#define GRCI_H

#include <stdbool.h>
#include <stddef.h>

#if defined(_WIN32)
#define GRCI_API __declspec(dllexport)
#else
#define GRCI_API
#endif

struct grci;
struct grci_sim;
struct grci_module {
    int input_count;
    int output_count;
    bool *inputs;
    bool *outputs;

    struct grci_sim *sim;
};
struct grci_submodule {
    int state_count;
    bool *states;
};

GRCI_API struct grci *grci_init(void* (*malloc)(size_t), void* (*realloc)(void*, size_t), void (*free)(void*));
GRCI_API struct grci *grci_easy_init(void);
GRCI_API bool grci_compile_src(struct grci *g, const char *buf, size_t len);
GRCI_API struct grci_module *grci_init_module(struct grci *g, const char *module_name, size_t len);
GRCI_API struct grci_submodule *grci_submodule(struct grci_module *m, const char *submodule_name, size_t len);
GRCI_API bool grci_step_module(struct grci_module *m);
GRCI_API void grci_destroy_module(struct grci_module *m);
GRCI_API void grci_cleanup(struct grci *g);
GRCI_API const char *grci_err(void);

#endif
