#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include "grci.h"

#define GRCI_DEFAULT_CHUNK_SIZE 4096
#define GRCI_RAM64K_STATE_COUNT 65536 * 8

#define GRCI_MAX_PARTS 64
#define GRCI_MAX_WIRES 32
#define GRCI_MAX_INPUTS 160
#define GRCI_MAX_OUTPUTS 128
#define GRCI_MAX_MODULES 64

#define GRCI_OUTPUT_NONE -1
#define GRCI_OUTPUT_0 -2
#define GRCI_OUTPUT_1 -3

#define GRCI_UNKNOWN_WIDTH 0

#define GRCI_OK true
#define GRCI_ERR false

//TODO: bug where adding a logic gate to Controller causes a node to be NULL, and it crashes
//  this is a major bug when IGCSE Computer output NOT being used, and memory being overwritten somewhere?
//  grci should give an error when module output is NOT set.

//TODO: Use enums to keep all error messages in same place
/*
    Make a enum to specify error type: too many inputs, not enough inputs, too many outputs, not enough outputs, etc
    Use this rather than messages so that we can keep all messages in same place

*/

//TODO: Allow grabbing submodules within submodules
/*
    Need this in order to read state of deeply embedded modules
*/

typedef bool grci_status;
static char grci_err_buf[128];

enum grci_err_type {
    GRCI_ERR_COMP = 0,
    GRCI_ERR_SIM,
    GRCI_ERR_MEM,
    GRCI_ERR_INTERN
};

static int grci_err_prefix(enum grci_err_type type, int line) {
    switch (type) {
    case GRCI_ERR_COMP:
        sprintf(grci_err_buf, "GRCI compilation error [near line %d]: ", line);
        break;
    case GRCI_ERR_SIM:
        sprintf(grci_err_buf, "GRCI simulation error: ");
        break;
    case GRCI_ERR_MEM:
        sprintf(grci_err_buf, "GRCI memory allocation error: ");
        break;
    case GRCI_ERR_INTERN:
        sprintf(grci_err_buf, "GRCI internal error: ");
        break;
    default:
        sprintf(grci_err_buf, "invalid error code\n");
        break;
    }
    return (int) strlen(grci_err_buf);
}

#define grci_ensure(cond, type, line, fmt, ...) \
    do { \
        if (cond) break; \
        if (strlen(grci_err_buf) == 0) { \
            int off = grci_err_prefix(type, line); \
            sprintf(grci_err_buf + off, fmt, ##__VA_ARGS__); \
        } \
        return GRCI_ERR; \
    } while (0)

#define grci_ensure_retnull(cond, type, line, fmt, ...) \
    do { \
        if (cond) break; \
        if (strlen(grci_err_buf) == 0) { \
            int off = grci_err_prefix(type, line); \
            sprintf(grci_err_buf + off, fmt, ##__VA_ARGS__); \
            return NULL; \
        } \
    } while (0)


/*
 * Arena allocator
*/

union grci_align {
    int i;
    long l;
    long *lp;
    void *p;
    void (*fp)(void);
    float f;
    double d;
    long double ld;
};

static size_t grci_aligned_size(size_t size) {
    return ((size + sizeof(union grci_align) - 1) / sizeof(union grci_align)) * sizeof(union grci_align);
}

struct grci_chunk {
    void *base;
    size_t off;
    size_t size;
};

static grci_status grci_chunk_init(struct grci_chunk *chunk, void* (*malloc)(size_t), size_t size) {
    chunk->base = malloc(size);
    grci_ensure(chunk->base, GRCI_ERR_MEM, 0, "malloc failed");
    chunk->off = 0;
    chunk->size = size;

    return GRCI_OK;
}

static void *grci_chunk_alloc(struct grci_chunk *chunk, size_t size) {
    //round up to strictest alignment requirements
    size_t alsize = grci_aligned_size(size);

    if (chunk->off + alsize > chunk->size) {
        return NULL;
    }

    void *ptr = (char*) chunk->base + chunk->off;
    chunk->off += alsize;
    return ptr;
}

struct grci_arena {
    struct grci_chunk *chunks;
    int chunk_count;
    int chunk_cap;
    int idx;
    void* (*malloc)(size_t);
    void* (*realloc)(void*, size_t);
    void (*free)(void*);
};

static grci_status grci_arena_init(struct grci_arena *arena, 
                            void* (*malloc)(size_t), 
                            void* (*realloc)(void*, size_t), 
                            void (*free)(void*)) {
    arena->malloc = malloc;
    arena->realloc = realloc;
    arena->free = free;
    arena->chunk_cap = 8;
    arena->chunks = arena->malloc(sizeof(struct grci_chunk) * arena->chunk_cap);
    grci_ensure(arena->chunks, GRCI_ERR_MEM, 0, "malloc failed");

    arena->chunk_count = 1;
    grci_chunk_init(&arena->chunks[0], arena->malloc, GRCI_DEFAULT_CHUNK_SIZE);
    arena->idx = 0;

    return GRCI_OK;
}

static void grci_arena_cleanup(struct grci_arena *arena) {
    for (int i = 0; i < arena->chunk_count; i++) {
        struct grci_chunk *c = &arena->chunks[i];
        arena->free(c->base);
    }
    arena->free(arena->chunks);
}

static void grci_arena_dealloc_all(struct grci_arena *arena) {
    for (int i = 0; i < arena->chunk_count; i++) {
        struct grci_chunk *c = &arena->chunks[i];
        c->off = 0;
    }
    arena->idx = 0;
}

static grci_status grci_append_chunk(struct grci_arena *arena, const struct grci_chunk *chunk) {
    if (arena->chunk_count >= arena->chunk_cap) {
        arena->chunk_cap *= 2;
        arena->chunks = arena->realloc(arena->chunks, sizeof(struct grci_chunk) * arena->chunk_cap); 
        assert(arena->chunks);
        grci_ensure(arena->chunks, GRCI_ERR_MEM, 0, "realloc failed");
    }

    arena->chunks[arena->chunk_count] = *chunk;
    arena->chunk_count++;

    return GRCI_OK;
}

static grci_status grci_arena_malloc(struct grci_arena *arena, size_t size, void **result) {
    //round up to strictest alignment requirements
    size_t alsize = grci_aligned_size(size);

    void *ptr = NULL;
    while (arena->idx < arena->chunk_count) {
        struct grci_chunk *cur = &arena->chunks[arena->idx];
        ptr = grci_chunk_alloc(cur, alsize);
        if (!ptr) {
            arena->idx++;
        } else {
            *result = ptr;
            return GRCI_OK;
        }
    }

    if (!ptr) {
        size_t new_size = GRCI_DEFAULT_CHUNK_SIZE;
        while (new_size < alsize) {
            new_size *= 8;
        }
        struct grci_chunk new_chunk;
        grci_ensure(grci_chunk_init(&new_chunk, arena->malloc, new_size), GRCI_ERR_MEM, 0, "placeholder");
        grci_ensure(grci_append_chunk(arena, &new_chunk), GRCI_ERR_MEM, 0, "placeholder");
        ptr = grci_chunk_alloc(&arena->chunks[arena->chunk_count - 1], alsize);
        assert(ptr);
    }

    *result = ptr;

    return GRCI_OK;
}
static grci_status grci_arena_calloc(struct grci_arena *arena, size_t nmem, size_t size, void **result)  {
    //round up to strictest alignment requirements
    size_t alsize = grci_aligned_size(nmem * size);

    void *ptr;
    grci_ensure(grci_arena_malloc(arena, alsize, &ptr), 
                GRCI_ERR_MEM, 0, "placeholder");
    memset(ptr, 0, alsize);
    *result = ptr;
    return GRCI_OK;
}
static grci_status grci_arena_realloc(struct grci_arena *arena, void *ptr, size_t size, void **result)  {
    //round up to strictest alignment requirements
    size_t alsize = grci_aligned_size(size);

    size_t copy_len = 0;
    bool found = false; //used to test assert
    if (ptr) {
        for (int i = 0; i < arena->chunk_count; i++) {
            struct grci_chunk *c = &arena->chunks[i];
            //found chunk of first allocation
            if (ptr >= c->base && ptr < (void*) ((char*) c->base + c->off)) {
                size_t ptr_off = (char*) ptr - (char*) c->base;
                assert(c->off > ptr_off);
                copy_len = c->off - ptr_off;
                copy_len = copy_len > alsize ? alsize : copy_len; //copy length will be smaller size
                //copy_len = alsize > copy_len ? copy_len : alsize; //copy length will be smaller size
                found = true;
                break;
            }
        }
    }
    grci_ensure(!ptr || found, GRCI_ERR_MEM, 0, "realloc failed");
    assert(!ptr || found);

    void *new_ptr;
    grci_ensure(grci_arena_malloc(arena, alsize, &new_ptr),
                GRCI_ERR_MEM, 0, "placeholder");
    if (ptr) {
        memcpy(new_ptr, ptr, copy_len);
    }

    *result = new_ptr;
    return GRCI_OK;
}

/*
    String
*/
struct grci_string {
    const char *ptr;
    int len;
};

static inline bool grci_string_matches(const struct grci_string *s, const char *c, size_t len) {
    return s->len == len && memcmp(s->ptr, c, len) == 0;
}
static inline bool grci_strings_equal(const struct grci_string *s1, const struct grci_string *s2) {
    return s1->len == s2->len && memcmp(s1->ptr, s2->ptr, s1->len) == 0;
}

static inline grci_status grci_string_alloc(struct grci_arena *arena, const char *ptr, size_t len, struct grci_string *s) {
    char *c;
    grci_ensure(grci_arena_malloc(arena, len, (void**) &c), 
                GRCI_ERR_MEM, 0, "placeholder");

    memcpy(c, ptr, len);
    s->ptr = c;
    s->len = len;

    return GRCI_OK;
}

/*
    Tokenizer
*/

enum grci_token_type {
    GRCI_TT_KEYWORD = 1,
    GRCI_TT_SYMBOL = 2,
    GRCI_TT_IDENTIFIER = 4,
    GRCI_TT_INT_LITERAL = 8,
    GRCI_TT_BYTE = 16,
    GRCI_TT_WORD = 32,
    GRCI_TT_EOF = 64
};

struct grci_token {
    enum grci_token_type type;
    struct grci_string literal;
    int line;
};

static const char *grci_keywords[] = {
    "module", "test", "clock"
};

static const char grci_symbols[] = {
    '{', '}', '(', ')', '[', ']', ',', '.',
    '-', '>', ':'
};

static const char grci_delimiters[] = {
    '{', '}', '(', ')', '[', ']', ',', '.',
    '-', '>', ':',
    ' ', '\n', '\t', '\0', '\r'
};


struct grci_tokenizer {
    const char *buf;
    int idx;
    int line;
};

static void grci_tokenizer_skip_whitespace_and_comments(struct grci_tokenizer *tokenizer) {
    while (true) {
        char c = tokenizer->buf[tokenizer->idx];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            tokenizer->idx++;
            if (c == '\n') tokenizer->line++;
        } else if (c == '/' && (tokenizer->buf[tokenizer->idx + 1] == '/' || tokenizer->buf[tokenizer->idx + 1] == '*')) { //comment
            if (tokenizer->buf[tokenizer->idx + 1] == '/') { //single line comment
                while (tokenizer->buf[tokenizer->idx] != '\n') {
                    tokenizer->idx++;
                }
                tokenizer->idx++; //skip \n
                tokenizer->line++;
            } else { //multiline comment
                while (!(tokenizer->buf[tokenizer->idx] == '*' && tokenizer->buf[tokenizer->idx + 1] == '/')) {
                    if (tokenizer->buf[tokenizer->idx] == '\n') tokenizer->line++;
                    tokenizer->idx++;
                }
                tokenizer->idx++; //skip *
                tokenizer->idx++; //skip /
            }
        } else {
            //eof
            return;
        }
    }
}

static inline void grci_tokenizer_init(struct grci_tokenizer *tokenizer) {
    tokenizer->buf = NULL;
    tokenizer->idx = 0;
    tokenizer->line = 1;
}

static inline bool grci_is_digit(char c) {
    return c >= '0' && c <= '9';
}

static inline bool grci_is_symbol(char c) {
    for (int i = 0; i < sizeof(grci_symbols) / sizeof(char); i++) {
        if (grci_symbols[i] == c)
            return true;
    }

    return false;
}

static inline bool grci_is_delimiter(char c) {
    for (int i = 0; i < sizeof(grci_delimiters) / sizeof(char); i++) {
        if (grci_delimiters[i] == c)
            return true;
    }

    return false;
}

static inline bool grci_is_keyword(const char* ptr, size_t len) {
    for (int i = 0; i < sizeof(grci_keywords) / sizeof(char*); i++) {
        const char* keyword = grci_keywords[i];
        if (len == strlen(keyword) && !strncmp(keyword, ptr, len)) {
            return true;
        }
    }

    return false;
}

static inline char grci_tokenizer_peek_one(struct grci_tokenizer *tokenizer) {
    return tokenizer->buf[tokenizer->idx];
}
static inline char grci_tokenizer_grci_peek_two(struct grci_tokenizer *tokenizer) {
    return tokenizer->buf[tokenizer->idx + 1];
}

static struct grci_token grci_tokenize_bit_or_idx(struct grci_tokenizer *tokenizer) {
    const char *start = &tokenizer->buf[tokenizer->idx];
    while (grci_is_digit(grci_tokenizer_peek_one(tokenizer))) {
        tokenizer->idx++;
    }
    const char *end = &tokenizer->buf[tokenizer->idx];

    return (struct grci_token) { .type=GRCI_TT_INT_LITERAL, 
                                 .literal.ptr=start, 
                                 .literal.len=(int) (end - start), 
                                 .line=tokenizer->line };
}

static struct grci_token grci_tokenize_multidigit(struct grci_tokenizer *tokenizer, enum grci_token_type type) {
    const char *start = &tokenizer->buf[tokenizer->idx];
    while (grci_is_digit(grci_tokenizer_peek_one(tokenizer))) {
        tokenizer->idx++;
    }
    const char *end = &tokenizer->buf[tokenizer->idx];
    assert((int) (end - start) > 0);
    return (struct grci_token) { .type=type, 
                                 .literal.ptr=start, 
                                 .literal.len=(int) (end - start), 
                                 .line=tokenizer->line };
}

static struct grci_token grci_tokenize_number(struct grci_tokenizer *tokenizer) {
    if (grci_tokenizer_peek_one(tokenizer) == '0' && grci_tokenizer_grci_peek_two(tokenizer) == 'b') {
        tokenizer->idx += 2;
        return grci_tokenize_multidigit(tokenizer, GRCI_TT_BYTE); 
    } else if (grci_tokenizer_peek_one(tokenizer) == '0' && grci_tokenizer_grci_peek_two(tokenizer) == 'w') {
        tokenizer->idx += 2;
        return grci_tokenize_multidigit(tokenizer, GRCI_TT_WORD); 
    } else {
        return grci_tokenize_bit_or_idx(tokenizer);
    }
}

static struct grci_token grci_tokenize_symbols(struct grci_tokenizer *tokenizer) {
    const char *start = &tokenizer->buf[tokenizer->idx];
    tokenizer->idx++;
    const char *end = &tokenizer->buf[tokenizer->idx];
    return (struct grci_token) { .type = GRCI_TT_SYMBOL, 
                                 .literal.ptr = start, 
                                 .literal.len = (int) (end - start), 
                                 .line=tokenizer->line };
}

static struct grci_token grci_tokenize_keyword_or_identifier(struct grci_tokenizer *tokenizer) {
    const char *start = &tokenizer->buf[tokenizer->idx];
    while (!grci_is_delimiter(grci_tokenizer_peek_one(tokenizer))) {
        tokenizer->idx++;
    }
    const char *end = &tokenizer->buf[tokenizer->idx];

    enum grci_token_type type = grci_is_keyword(start, (int) (end - start)) ? GRCI_TT_KEYWORD : GRCI_TT_IDENTIFIER;
    return (struct grci_token) { .type = type, 
                                 .literal.ptr = start, 
                                 .literal.len = (int) (end - start), 
                                 .line=tokenizer->line };
}

static struct grci_token grci_tokenizer_next(struct grci_tokenizer *tokenizer) {
    grci_tokenizer_skip_whitespace_and_comments(tokenizer);
    char c = grci_tokenizer_peek_one(tokenizer);
    switch (c) {
    case '\0':
        return (struct grci_token) { .type = GRCI_TT_EOF, 
                                     .literal.ptr = NULL, 
                                     .literal.len = 0, 
                                     .line=tokenizer->line };
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
        return grci_tokenize_number(tokenizer);
    default:
        if (grci_is_symbol(c)) {
            return grci_tokenize_symbols(tokenizer);
        } else {
            return grci_tokenize_keyword_or_identifier(tokenizer);
        }
    }

    assert(false);
    return (struct grci_token) { .type = GRCI_TT_EOF, 
                                 .literal.ptr = NULL, 
                                 .literal.len = 0, 
                                 .line=tokenizer->line };
}


static inline bool grci_token_matches(struct grci_token token, const char *target) {
    return grci_string_matches(&token.literal, target, strlen(target));
}


/*
 * grci_compiler
*/

enum grci_input_type {
    GRCI_IT_INTERNAL,
    GRCI_IT_EXTERNAL,
    GRCI_IT_CONSTANT,
    GRCI_IT_CLOCK //unused?
};

struct grci_element {
    int idx;
    int output_idx;
};

static inline struct grci_element output_part(int idx, int output_idx) {
    return (struct grci_element) { .idx=idx, 
                                   .output_idx=output_idx };
}

struct grci_connection {
    enum grci_input_type type;
    union {
        int parameter_idx;
        struct grci_element part;
        bool constant;
    } get;
};

static inline struct grci_connection intern_conn(int part_idx, int output_idx) {
    return (struct grci_connection) { .type=GRCI_IT_INTERNAL, 
                                      .get.part.idx=part_idx, 
                                      .get.part.output_idx=output_idx };
}

static inline struct grci_connection extern_conn(int parameter_idx) {
    return (struct grci_connection) { .type=GRCI_IT_EXTERNAL, 
                                      .get.parameter_idx=parameter_idx };
}

static inline struct grci_connection const_conn(bool constant) {
    return (struct grci_connection) { .type=GRCI_IT_CONSTANT, 
                                      .get.constant=constant };
}

static inline struct grci_connection clock_conn(void) {
    return (struct grci_connection) { .type=GRCI_IT_CLOCK };
}

struct grci_connection_list {
    struct grci_connection *values;
    int capacity;
    int count;
    struct grci_arena *arena;
};

static void grci_connection_list_init(struct grci_connection_list *l, struct grci_arena *arena) {
    l->values = NULL;
    l->capacity = 0;
    l->count = 0;
    l->arena = arena;
}

//Order of connections matters here - the first connection to a given part will be assigned to parameters[0],
//  the second connection to the same part will be assigned to parameters[1], etc
static grci_status grci_connection_list_append(struct grci_connection_list *l, struct grci_connection pc) {
    if (l->count == l->capacity) {
        l->capacity = l->capacity == 0 ? 8 : l->capacity * 2;
        grci_ensure(grci_arena_realloc(l->arena, l->values, sizeof(struct grci_connection) * l->capacity, (void**) &l->values),
                    GRCI_ERR_MEM, 0, "placeholder"); 
        assert(l->values);
    }

    l->values[l->count] = pc;
    l->count++;
    return GRCI_OK;
}

struct grci_module_desc {
    struct grci_string name;
    const struct grci_module_desc *parts[GRCI_MAX_PARTS];
    struct grci_string part_names[GRCI_MAX_PARTS];
    struct grci_connection_list part_connections[GRCI_MAX_PARTS];
    int part_count;
    int input_count;
    int input_param_count; //if a parameter is a bus, it will be associate with many inputs
    struct grci_element outputs[GRCI_MAX_OUTPUTS];
    int output_count;
    int output_param_count;
    int input_widths[GRCI_MAX_INPUTS]; //TODO: rename to input_param_widths
    int output_widths[GRCI_MAX_OUTPUTS]; //TODO: rename to output_param_widths
    bool is_nand;
    bool is_dff;
    bool is_ram64K;
    int sink_counts[GRCI_MAX_INPUTS]; //cumulative sink counts for each input (eg, including sinks for internal elements that use this input)

    int node_count;
    int dff_count;
};

static void grci_module_desc_init(struct grci_module_desc *decl, struct grci_arena *arena) {
    for (int i = 0; i < GRCI_MAX_PARTS; i++) {
        grci_connection_list_init(&decl->part_connections[i], arena);
        decl->part_names[i].ptr = NULL;
        decl->part_names[i].len = 0;
    }
    decl->part_count = 0;
    decl->input_count = 0;
    decl->input_param_count = 0;
    decl->output_count = 0;
    decl->output_param_count = 0;
    decl->is_nand = false;
    decl->is_dff = false;
    decl->is_ram64K = false;

    decl->node_count = 0;
    decl->dff_count = 0;
}

struct grci_module_desc_list {
    struct grci_module_desc entries[GRCI_MAX_MODULES];
    int count;
};

static void grci_module_desc_list_init(struct grci_module_desc_list *list) {
    list->count = 0;
}

static void grci_module_desc_list_append(struct grci_module_desc_list *list, const struct grci_module_desc *module_def) {
    assert(list->count < GRCI_MAX_MODULES);
    list->entries[list->count] = *module_def;
    list->count++; 
}

static const struct grci_module_desc* grci_module_desc_list_get(const struct grci_module_desc_list *list, const struct grci_string *s) {
    for (int i = 0; i < list->count; i++) {
        const struct grci_module_desc *cur = &list->entries[i];
        if (grci_strings_equal(&cur->name, s)) {
            return cur;
        }
    }

    return NULL;
}

struct grci_symbol_entry {
    struct grci_token token;
    //offset of within a symbol for slicing buses.
    int offset;
    int width;
};

struct grci_symbol_list {
    struct grci_symbol_entry entries[128];
    int count;
};

static void grci_symbol_list_init(struct grci_symbol_list *list) {
    list->count = 0;
}

static void grci_symbol_list_append(struct grci_symbol_list *list, struct grci_symbol_entry sym) {
    assert(list->count < 128);

    list->entries[list->count] = sym;
    list->count++;
}

static int grci_symbol_list_idx(struct grci_symbol_list *list, struct grci_token t) {
    for (int i = 0; i < list->count; i++) {
        struct grci_token token = list->entries[i].token;
        if (grci_strings_equal(&t.literal, &token.literal)) {
            return i;
        }
    }
    return -1;
}

static void grci_symbol_list_print(struct grci_symbol_list *list, const char *title) {
    printf("%s (name, offset, width)\n", title);
    for (int i = 0; i < list->count; i++) {
        struct grci_symbol_entry *s = &list->entries[i];
        printf("%.*s  %d  %d\n", s->token.literal.len, s->token.literal.ptr, s->offset, s->width);
    }
}

struct grci_symbol_table {
    struct {
        struct grci_symbol_list inputs;
        struct grci_symbol_list outputs;
    } interface;

    struct {
        struct grci_symbol_list inputs[GRCI_MAX_PARTS];
        struct grci_symbol_list outputs[GRCI_MAX_PARTS];
        int count;
    } parts;

    struct {
        struct grci_symbol_list inputs[GRCI_MAX_WIRES];
        struct grci_symbol_list outputs; //wires can only output one symbol
        int count;
    } wires;
};

static void grci_symbol_table_init(struct grci_symbol_table *table) {
    grci_symbol_list_init(&table->interface.inputs);
    grci_symbol_list_init(&table->interface.outputs);

    for (int i = 0; i < GRCI_MAX_PARTS; i++) {
        grci_symbol_list_init(&table->parts.inputs[i]);
        grci_symbol_list_init(&table->parts.outputs[i]);
    }
    table->parts.count = 0;

    for (int i = 0; i < GRCI_MAX_WIRES; i++) {
        grci_symbol_list_init(&table->wires.inputs[i]);
    }
    grci_symbol_list_init(&table->wires.outputs);
    table->wires.count = 0;
}

static void grci_symbol_table_print(struct grci_symbol_table *symbols) {
    printf("  Input grci_symbol_entrys:\n");
    for (int i = 0; i < symbols->interface.inputs.count; i++) {
        struct grci_symbol_entry *s = &symbols->interface.inputs.entries[i];
        printf("    name: %.*s, offset: %d, width: %d\n", s->token.literal.len, s->token.literal.ptr, s->offset, s->width);
    }
    printf("  Output grci_symbol_entrys:\n");
    for (int i = 0; i < symbols->interface.outputs.count; i++) {
        struct grci_symbol_entry *s = &symbols->interface.outputs.entries[i];
        printf("    name: %.*s, offset: %d, width: %d\n", s->token.literal.len, s->token.literal.ptr, s->offset, s->width);
    }
    printf("  Part grci_symbol_entrys:\n");
    for (int i = 0; i < symbols->parts.count; i++) {
        printf("    Input grci_symbol_entrys[%d]:\n", i);
        for (int j = 0; j < symbols->parts.inputs[i].count; j++) {
            struct grci_symbol_entry *s = &symbols->parts.inputs[i].entries[j];
            printf("      name: %.*s, offset: %d, width: %d\n", s->token.literal.len, s->token.literal.ptr, s->offset, s->width);
        }
        printf("    Output grci_symbol_entrys[%d]:\n", i);
        for (int j = 0; j < symbols->parts.outputs[i].count; j++) {
            struct grci_symbol_entry *s = &symbols->parts.outputs[i].entries[j];
            printf("      name: %.*s, offset: %d, width: %d\n", s->token.literal.len, s->token.literal.ptr, s->offset, s->width);
        }
    }
    printf("  Wire grci_symbol_entrys:\n");
    for (int i = 0; i < symbols->wires.count; i++) {
        printf("    Input grci_symbol_entrys[%d]:\n", i);
        for (int j = 0; j < symbols->wires.inputs[i].count; j++) {
            struct grci_symbol_entry *s = &symbols->wires.inputs[i].entries[j];
            printf("      name: %.*s, offset: %d, width: %d\n", s->token.literal.len, s->token.literal.ptr, s->offset, s->width);
        }
        printf("    Output grci_symbol_entrys[%d]:\n", i);
        struct grci_symbol_entry *s = &symbols->wires.outputs.entries[i];
        printf("      name: %.*s, offset: %d, width: %d\n", s->token.literal.len, s->token.literal.ptr, s->offset, s->width);
    }
}

struct grci_compiler {
    struct grci_tokenizer tokenizer;
    struct grci_token cur; //peek looks here
    struct grci_token next; //peek two looks here
    struct grci_token *current_module;
    const char *current_file;

    struct grci_module_desc_list module_defs;
    struct grci_symbol_list inputs;
    struct grci_symbol_list outputs;
    struct grci_symbol_list locals;

    int id_counter;

    struct grci_arena arena;
};

static const struct grci_module_desc *nand_decl(void) {
    static struct grci_module_desc nand_data = { .name.ptr = "Nand", 
                                                 .name.len = 4,
                                                 .part_count = 0,
                                                 .input_param_count = 2,
                                                 .input_count = 2,
                                                 .input_widths[0] = 1,
                                                 .input_widths[1] = 1,
                                                 .output_count = 1,
                                                 .output_param_count = 1,
                                                 .output_widths[0] = 1,
                                                 .is_nand = true };

    nand_data.outputs[0] = output_part(GRCI_OUTPUT_NONE, GRCI_OUTPUT_NONE); //unused output part since this is hard coded
    nand_data.sink_counts[0] = 1;
    nand_data.sink_counts[1] = 1;
    //No connections since no parts in nand gate
    nand_data.node_count = 1;
    nand_data.dff_count = 0;

    return &nand_data;
}

static const struct grci_module_desc *dff_decl(void) {
    static struct grci_module_desc dff_data = { .name.ptr = "Dff", 
                                                .name.len = 3,
                                                .part_count = 0,
                                                .input_param_count = 1,
                                                .input_count = 1,
                                                .input_widths[0] = 1,
                                                .output_count = 1,
                                                .output_param_count = 1,
                                                .output_widths[0] = 1,
                                                .is_dff = true }; //output_part not used in atomic nand gate

    dff_data.outputs[0] = output_part(GRCI_OUTPUT_NONE, GRCI_OUTPUT_NONE);
    dff_data.sink_counts[0] = 1;
    //No connections since no parts in dff gate
    dff_data.node_count = 1;
    dff_data.dff_count = 1;

    return &dff_data;
}


static const struct grci_module_desc *ram64K_decl(void) {
    static struct grci_module_desc ram;
    ram.name.ptr = "Ram64K";
    ram.name.len = 6;
    ram.part_count = 0;
    ram.input_param_count = 3;
    ram.input_count = 33;
    ram.input_widths[0] = 16; //input
    ram.input_widths[1] = 1; //load
    ram.input_widths[2] = 16; //address
    ram.output_count = 16;
    ram.output_param_count = 1;
    ram.output_widths[0] = 16;
    ram.is_ram64K = true;

    for (int i = 0; i < 16; i++) {
        ram.outputs[i] = output_part(GRCI_OUTPUT_NONE, GRCI_OUTPUT_NONE);
    }
    for (int i = 0; i < 33; i++) {
        ram.sink_counts[i] = 1;
    }

    ram.node_count = 16;
    ram.dff_count = 16;

    return &ram;
}

static void grci_compiler_init(struct grci_compiler *compiler, 
                                           void* (*malloc)(size_t), 
                                           void* (*realloc)(void*, size_t), 
                                           void(*free)(void*)) {
    grci_arena_init(&compiler->arena, malloc, realloc, free);

    grci_module_desc_list_init(&compiler->module_defs);
    grci_module_desc_list_append(&compiler->module_defs, nand_decl());
    grci_module_desc_list_append(&compiler->module_defs, dff_decl());
    grci_module_desc_list_append(&compiler->module_defs, ram64K_decl());

    grci_tokenizer_init(&compiler->tokenizer);
    compiler->id_counter = 0;
}

static void grci_compiler_cleanup(struct grci_compiler *compiler) {
    grci_arena_cleanup(&compiler->arena);
}

static inline int grci_compiler_new_id(struct grci_compiler *compiler) {
    compiler->id_counter++;
    return compiler->id_counter;
}

static inline struct grci_token grci_peek_next(struct grci_compiler *compiler) {
    return compiler->cur; 
}

static inline struct grci_token grci_peek_two(struct grci_compiler *compiler) {
    return compiler->next; 
}

static inline int grci_line_number(struct grci_compiler *compiler) {
    return grci_peek_next(compiler).line;
}

static struct grci_token grci_next_token(struct grci_compiler *compiler) {
    struct grci_token result = compiler->cur; 
    compiler->cur = compiler->next;
    compiler->next = grci_tokenizer_next(&compiler->tokenizer);
    return result;
}

#define grci_eat_token(compiler, expected) \
    do { \
        struct grci_token t = grci_next_token(compiler); \
        grci_ensure(grci_token_matches(t, expected), GRCI_ERR_COMP, t.line, \
                    "Expected '%s' but got '%.*s'",  \
                    expected, t.literal.len, t.literal.ptr); \
    } while (0) 

static inline bool grci_consume_if_next_matches(struct grci_compiler *compiler, const char *target) {
    bool matches;
    if ((matches = grci_token_matches(grci_peek_next(compiler), target))) {
        grci_next_token(compiler);
    }

    return matches;
}

static grci_status grci_compiler_parse_slice(struct grci_compiler *compiler, int *offset, int* width) {
    grci_eat_token(compiler, "[");
    struct grci_token idx = grci_next_token(compiler);
    grci_ensure(idx.type == GRCI_TT_INT_LITERAL, GRCI_ERR_COMP, idx.line,
                "Invalid slice index '%.*s'.  Slicing must be of the format [n] or [n..m] where n and m are integers", 
                idx.literal.len, idx.literal.ptr);

    *offset = atoi(idx.literal.ptr);
    *width = 1;

    if (grci_consume_if_next_matches(compiler, ".")) {
        grci_eat_token(compiler, ".");

        struct grci_token idx = grci_next_token(compiler);
        grci_ensure(idx.type == GRCI_TT_INT_LITERAL, GRCI_ERR_COMP, idx.line, 
                    "Invalid slice index '%.*s'.  Slicing must be of the format [n] or [n..m] where n and m are integers", 
                    idx.literal.len, 
                    idx.literal.ptr);

        int end = atoi(idx.literal.ptr);
        grci_ensure(end >= *offset, GRCI_ERR_COMP, idx.line, "Slice ending index must be larger than starting index");
        *width = end - *offset + 1; //inclusive of 'end'
    }

    grci_eat_token(compiler, "]");

    return GRCI_OK;
}

static grci_status grci_compiler_compile_parameter_list(struct grci_compiler *compiler, struct grci_symbol_list *list) {

    while (!grci_token_matches(grci_peek_next(compiler), ")")) {
        int width = 1;
        int offset = 0;
        struct grci_token identifier = grci_next_token(compiler);
        if (grci_token_matches(grci_peek_next(compiler), "[")) {
            grci_compiler_parse_slice(compiler, &offset, &width);
            grci_ensure(width == 1, GRCI_ERR_COMP, identifier.line, "Parameter declaration must be a single input or bus[n]");
        } else {
            offset = 1; //offset is used as width (confusingly), so setting it to 1 if not a bus
        }
        grci_symbol_list_append(list, (struct grci_symbol_entry) { .token=identifier, .offset=0, .width=offset }); //'index offset' is the bus width

        if (grci_consume_if_next_matches(compiler, ","))
            ;
    }
    return GRCI_OK;
}

static grci_status grci_compiler_compile_output_list(struct grci_compiler *compiler, struct grci_symbol_list *list) {
    grci_ensure(grci_peek_next(compiler).type == GRCI_TT_IDENTIFIER, GRCI_ERR_COMP, grci_line_number(compiler), "Output must be an identifier");
    bool more;
    do {
        int width = 1;
        int offset = 0;
        more = false;
        struct grci_token identifier = grci_next_token(compiler);
        if (grci_token_matches(grci_peek_next(compiler), "[")) {
            grci_compiler_parse_slice(compiler, &offset, &width);
            grci_ensure(width == 1, GRCI_ERR_COMP, identifier.line, "Output declaration must be a single input or bus[n]");
        } else {
            offset = 1; //offset is used as width (confusingly), so setting it to 1 if not a bus
        }
        grci_symbol_list_append(list, (struct grci_symbol_entry) { .token=identifier, .offset=0, .width=offset }); //'index offset' is the bus width

        if (grci_consume_if_next_matches(compiler, ",")) {
            more = true;
        }
    } while (more);

    return GRCI_OK;
}


static grci_status compile_expr(struct grci_compiler *compiler, struct grci_symbol_entry *s) {
    int width = GRCI_UNKNOWN_WIDTH;
    int offset = 0;
    struct grci_token identifier = grci_next_token(compiler);
    if (grci_token_matches(grci_peek_next(compiler), "[")) {
        grci_ensure(identifier.type != GRCI_TT_INT_LITERAL, GRCI_ERR_COMP, identifier.line, "Can't [] slice a constant");
        grci_compiler_parse_slice(compiler, &offset, &width);
    }

    *s = (struct grci_symbol_entry) { .token=identifier, .offset=offset, .width=width };

    return GRCI_OK;
}

static int grci_absolute_offset(const struct grci_symbol_list *list, int end_idx) {
    int offset = 0;
    for (int i = 0; i < end_idx; i++) {
        offset += list->entries[i].width;
    }
    return offset;
}

//part or wire inputs/outputs
static grci_status compile_expr_list(struct grci_compiler *compiler, struct grci_symbol_list *list, struct grci_symbol_table *symbols) {
    grci_ensure(grci_peek_next(compiler).type == GRCI_TT_IDENTIFIER || 
                grci_peek_next(compiler).type == GRCI_TT_INT_LITERAL || 
                grci_token_matches(grci_peek_next(compiler), "{"), 
                GRCI_ERR_COMP,
                grci_line_number(compiler),
                "Internal parts must be other modules or wires");

    int count = 0;
    bool more;
    do {
        more = false;

        if (!grci_token_matches(grci_peek_next(compiler), "{")) { //const or identifier
            struct grci_symbol_entry s;
            compile_expr(compiler, &s);
            grci_symbol_list_append(list, s);
        } else { //anonymous wire
            int wire_idx = symbols->wires.count;
            symbols->wires.count++;

            grci_eat_token(compiler, "{"); 
            compile_expr_list(compiler, &symbols->wires.inputs[wire_idx], symbols);
            grci_eat_token(compiler, "}");

            //make a temporary id for this anonymous wire
            int unique_id = grci_compiler_new_id(compiler);
            char *buf;
            grci_ensure(grci_arena_malloc(&compiler->arena, sizeof(char) * 16, (void**) &buf),
                        GRCI_ERR_MEM, 0, "placeholder");
            sprintf(buf, "_%d_", unique_id);
            struct grci_token identifier = { .type=GRCI_TT_IDENTIFIER, 
                                             .literal.ptr=buf, 
                                             .literal.len=strlen(buf), 
                                             .line=compiler->tokenizer.line };
            struct grci_symbol_entry entry = { .token=identifier, 
                                               .offset=0, 
                                               .width=GRCI_UNKNOWN_WIDTH };
            grci_symbol_list_append(&symbols->wires.outputs, entry);
            grci_symbol_list_append(list, entry);
        }

        if (grci_consume_if_next_matches(compiler, ",")) {
            more = true;
        }

        count++;
    } while (more);

    return GRCI_OK;
}

static bool get_part_by_name(struct grci_string *part_names, struct grci_token t, int *idx) {
    for (int i = 0; i < GRCI_MAX_PARTS; i++) {
        if (!part_names[i].ptr) continue;
        if (grci_string_matches(&part_names[i], t.literal.ptr, t.literal.len)) {
            *idx = i;
            return true;
        }
    } 
    return false;
}

static grci_status grci_compile_part_list(struct grci_compiler *compiler, 
                                   const struct grci_module_desc **parts, 
                                   struct grci_string *part_names, 
                                   struct grci_symbol_table *symbols) {

    while (grci_peek_next(compiler).type == GRCI_TT_IDENTIFIER || 
           grci_peek_next(compiler).type == GRCI_TT_INT_LITERAL || 
           grci_token_matches(grci_peek_next(compiler), "{")) {

        grci_ensure(symbols->parts.count < GRCI_MAX_PARTS, GRCI_ERR_COMP, grci_line_number(compiler),
                    "Internal parts exceeding GRCI_MAX_PARTS %d", 
                    GRCI_MAX_PARTS);

        if (grci_token_matches(grci_peek_two(compiler), "(") || grci_token_matches(grci_peek_two(compiler), ":")) { //compile part
            int part_idx = symbols->parts.count;
            symbols->parts.count++;

            //save part name if supplied
            if (grci_token_matches(grci_peek_two(compiler), ":")) {
                struct grci_token t = grci_next_token(compiler);
                grci_eat_token(compiler, ":");
                int idx;
                grci_ensure(!get_part_by_name(part_names, t, &idx), GRCI_ERR_COMP, t.line,
                            "Part name '%.*s' already exsists in this module", 
                            t.literal.len, t.literal.ptr);
                grci_ensure(grci_string_alloc(&compiler->arena, t.literal.ptr, t.literal.len, &part_names[part_idx]), 
                            GRCI_ERR_MEM, 0, "placeholder");
            }

            struct grci_token identifier = grci_next_token(compiler);
            const struct grci_module_desc *module_def = grci_module_desc_list_get(&compiler->module_defs, &identifier.literal);
            grci_ensure(module_def, GRCI_ERR_COMP, identifier.line,
                        "Attempting to use nonexistent module '%.*s'", 
                        identifier.literal.len, identifier.literal.ptr);

            parts[part_idx] = module_def;
            grci_eat_token(compiler, "(");
            if (!grci_token_matches(grci_peek_next(compiler), ")")) {
                grci_ensure(compile_expr_list(compiler, &symbols->parts.inputs[part_idx], symbols),
                            GRCI_ERR_COMP, 0, "placeholder");
            }
            grci_ensure(symbols->parts.inputs[part_idx].count == module_def->input_param_count, GRCI_ERR_COMP, identifier.line,
                        "Argument count of '%.*s' doesn't match module declaration. Actual %d vs expected %d", 
                        identifier.literal.len, identifier.literal.ptr, 
                        symbols->parts.inputs[part_idx].count, module_def->input_param_count);

            grci_eat_token(compiler, ")");
            grci_eat_token(compiler, "-");
            grci_eat_token(compiler, ">");
            grci_ensure(compile_expr_list(compiler, &symbols->parts.outputs[part_idx], symbols),
                        GRCI_ERR_COMP, 0, "placeholder");

            grci_ensure(symbols->parts.outputs[part_idx].count == module_def->output_param_count, GRCI_ERR_COMP, identifier.line,
                        "'%.*s' output count doesn't match module declaration", 
                        identifier.literal.len, identifier.literal.ptr);
        } else { //compile wire
            int wire_idx = symbols->wires.count;
            symbols->wires.count++;
            bool binding;
            if ((binding = grci_consume_if_next_matches(compiler, "{"))) {

            }

            grci_ensure(compile_expr_list(compiler, &symbols->wires.inputs[wire_idx], symbols),
                        GRCI_ERR_MEM, 0, "placeholder");
            if (binding) { 
                grci_eat_token(compiler, "}");
            } else {
                //grci_ensure(input_count == 1, GRCI_ERR_COMP, symbols->wires.inputs[wire_idx].entries[0].token.line, 
                grci_ensure(symbols->wires.inputs[wire_idx].count == 1, GRCI_ERR_COMP, symbols->wires.inputs[wire_idx].entries[0].token.line, 
                            "Multiple wire inputs must be bound using {}");
            }

            grci_eat_token(compiler, "-");
            grci_eat_token(compiler, ">");
       
            //appending {} to ensure capacity/count is correct is ugly and confusing
            grci_symbol_list_append(&symbols->wires.outputs, (struct grci_symbol_entry) {}); //appending empty symbol so that list capacity/count is updated
            compile_expr(compiler, &symbols->wires.outputs.entries[wire_idx]);
        }
    }

    return GRCI_OK;    
}

static inline bool grci_is_module_input(struct grci_token token, struct grci_symbol_table *symbols, int *param_idx) {
    *param_idx = grci_symbol_list_idx(&symbols->interface.inputs, token);
    return *param_idx != -1;
}

static inline bool grci_is_module_output(struct grci_token token, struct grci_symbol_table *symbols, int *param_idx) {
    *param_idx = grci_symbol_list_idx(&symbols->interface.outputs, token);
    return *param_idx != -1;
}

static inline bool grci_is_element_output(struct grci_token token, struct grci_symbol_table *symbols, struct grci_element *part) {
    int idx = -1;
    for (int i = 0; i < symbols->parts.count; i++) {
        struct grci_symbol_list *list = &symbols->parts.outputs[i]; 
        idx = grci_symbol_list_idx(list, token);
        if (idx != -1) {
            part->idx = i;
            part->output_idx = idx;
            break;
        }
    }

    return idx != -1;
}

static inline bool grci_is_wire_output(struct grci_token token, struct grci_symbol_table *symbols, int *wire_idx) {
    *wire_idx = grci_symbol_list_idx(&symbols->wires.outputs, token);
    return *wire_idx != -1;
}

static grci_status grci_infer_implicit_element_conn_widths(struct grci_module_desc *module_decl, 
                                                    int part_idx, 
                                                    struct grci_symbol_table *symbols) {

    //infer element output widths
    struct grci_symbol_list *part_output_params = &symbols->parts.outputs[part_idx];
    for (int j = 0; j < part_output_params->count; j++) {
        if (part_output_params->entries[j].width != GRCI_UNKNOWN_WIDTH) {
            continue;
        }

        //infer output width from element decl 
        struct grci_symbol_entry *s = &part_output_params->entries[j];
        const struct grci_module_desc* part_decl = module_decl->parts[part_idx];
        s->width = part_decl->output_widths[j];

        //double check with module output if connected there
        int idx;
        if (grci_is_module_output(s->token, symbols, &idx)) {
            grci_ensure(module_decl->output_widths[idx] == s->width, GRCI_ERR_COMP, s->token.line,
                        "'%.*s' output does not match declared output width", module_decl->name.len, module_decl->name.ptr);
        }
    }

    //infer element input widths
    struct grci_symbol_list *part_input_params = &symbols->parts.inputs[part_idx];
    for (int j = 0; j < part_input_params->count; j++) {
        if (part_input_params->entries[j].width != GRCI_UNKNOWN_WIDTH) {
            continue;
        }

        //inferred from element input width, which will be inferred from element decl
        struct grci_symbol_entry *s = &part_input_params->entries[j];
        const struct grci_module_desc* part_decl = module_decl->parts[part_idx];
        s->width = part_decl->input_widths[j];

        int idx;
        //check if module input is an output of a different element
        struct grci_element part;
        if (grci_is_element_output(s->token, symbols, &part)) {
            const struct grci_module_desc *out_part = module_decl->parts[part.idx];
            grci_ensure(out_part->output_widths[part.output_idx] == s->width, GRCI_ERR_COMP, s->token.line,
                        "Part expects input width of %d, but '%.*s' is of width %d", s->width, 
                        s->token.literal.len, s->token.literal.ptr, out_part->output_widths[part.output_idx]);
        }

        //double check with module input if connected there
        if (grci_is_module_input(s->token, symbols, &idx)) {
            grci_ensure(module_decl->input_widths[idx] == s->width, GRCI_ERR_COMP, s->token.line,
                        "'%.*s' input '%.*s' does not match declared input width", 
                        module_decl->parts[part_idx]->name.len, module_decl->parts[part_idx]->name.ptr,
                        s->token.literal.len, s->token.literal.ptr);
        }
    }

    return GRCI_OK;
}

static grci_status grci_infer_implicit_wire_conn_widths(struct grci_module_desc *module_decl, int wire_idx, struct grci_symbol_table *symbols ) {


    //infer wire outputs since this will compute wire input widths too
    struct grci_symbol_list *wire_outputs = &symbols->wires.outputs;
    struct grci_symbol_entry *output_wire = &wire_outputs->entries[wire_idx];

    int total_input_widths = 0;
    
    //compute, cache and sum input widths
    struct grci_symbol_list *wire_inputs = &symbols->wires.inputs[wire_idx];
    grci_ensure(wire_inputs->count > 0, GRCI_ERR_COMP, output_wire->token.line, 
                "Wire input must have at least one input");
    for (int i = 0; i < wire_inputs->count; i++) {
        struct grci_symbol_entry *input_wire = &wire_inputs->entries[i];

        if (input_wire->width != GRCI_UNKNOWN_WIDTH) {
            total_input_widths += input_wire->width;
            continue;
        }

        int idx;
        struct grci_element part;
        int other_wire_idx;

        if (grci_is_module_input(input_wire->token, symbols, &idx)) { //module input
            input_wire->width = symbols->interface.inputs.entries[idx].width;
        } else if (grci_is_element_output(input_wire->token, symbols, &part)) {
            input_wire->width = module_decl->parts[part.idx]->output_widths[part.output_idx];
        } else if (grci_is_wire_output(input_wire->token, symbols, &other_wire_idx)) { //wire output
            grci_infer_implicit_wire_conn_widths(module_decl, other_wire_idx, symbols);
            grci_ensure(symbols->wires.outputs.entries[other_wire_idx].width != GRCI_UNKNOWN_WIDTH, GRCI_ERR_COMP,
                        symbols->wires.outputs.entries[other_wire_idx].token.line,
                        "grci_compiler Error: wire width not inferred");
            input_wire->width = symbols->wires.outputs.entries[other_wire_idx].width;
        } else if (input_wire->token.type == GRCI_TT_INT_LITERAL) {
            grci_ensure(grci_token_matches(input_wire->token, "0") || grci_token_matches(input_wire->token, "1"), 
                        GRCI_ERR_COMP, input_wire->token.line, "Constant inputs must be 0 or 1");
            input_wire->width = 1;
        } else {
            grci_ensure(false, GRCI_ERR_COMP, input_wire->token.line,
                        "'%.*s' not declared in module", 
                        input_wire->token.literal.len, input_wire->token.literal.ptr);
        }

        total_input_widths += input_wire->width;
    }

    if (output_wire->width != GRCI_UNKNOWN_WIDTH) {
        grci_ensure(output_wire->width == total_input_widths, GRCI_ERR_COMP, output_wire->token.line,
                    "grci_compiler Error: wire width not inferred");
    }
   
    output_wire->width = total_input_widths;

    return GRCI_OK;
}

static grci_status grci_connect_wire_to_element(int wire_idx, 
                                         int part_idx, 
                                         int relative_offset, 
                                         struct grci_symbol_table *symbols, 
                                         struct grci_module_desc *module_decl) {

    for (int i = 0; i < symbols->wires.inputs[wire_idx].count; i++) {
        struct grci_symbol_entry *s = &symbols->wires.inputs[wire_idx].entries[i];
        int idx;
        struct grci_element output_part;
        if (grci_is_wire_output(s->token, symbols, &idx)) { //wire connected to another wire
            grci_connect_wire_to_element(idx, part_idx, s->offset + relative_offset, symbols, module_decl);
        } else if (grci_is_module_input(s->token, symbols, &idx)) { //wire connected to module input
            int input_off = grci_absolute_offset(&symbols->interface.inputs, idx);
            for (int j = 0; j < s->width; j++) {
                grci_ensure(grci_connection_list_append(&module_decl->part_connections[part_idx], extern_conn(input_off + s->offset + j)),
                            GRCI_ERR_MEM, 0, "placeholder");
            }

        } else if (s->token.type == GRCI_TT_INT_LITERAL) {
            grci_ensure(grci_token_matches(s->token, "0") || grci_token_matches(s->token, "1"), 
                        GRCI_ERR_COMP, s->token.line, "Constant inputs must be 0 or 1");
            bool value = grci_string_matches(&s->token.literal, "0", 1) ? 0 : 1;
            for (int j = 0; j < s->width; j++) {
                grci_ensure(grci_connection_list_append(&module_decl->part_connections[part_idx], const_conn(value)),
                            GRCI_ERR_MEM, 0, "placeholder");
            }
        } else if (grci_is_element_output(s->token, symbols, &output_part)) {
            int output_offset = grci_absolute_offset(&symbols->parts.outputs[output_part.idx], output_part.output_idx);
            for (int k = 0; k < s->width; k++) {
                grci_ensure(grci_connection_list_append(&module_decl->part_connections[part_idx], intern_conn(output_part.idx, s->offset + output_offset + k)),
                            GRCI_ERR_MEM, 0, "placeholder");
            }
        } else {
            grci_ensure(false, GRCI_ERR_COMP, s->token.line, 
                        "'%.*s' not declared", s->token.literal.len, s->token.literal.ptr);
        }
    }

    return GRCI_OK;
}

static grci_status grci_connect_wire_to_output(int wire_idx, 
                                        int *output_offset, 
                                        int relative_offset, 
                                        struct grci_symbol_table *symbols, 
                                        struct grci_module_desc *module_decl) {

    for (int i = relative_offset; i < symbols->wires.inputs[wire_idx].count; i++) {
        struct grci_symbol_entry *s = &symbols->wires.inputs[wire_idx].entries[i];
        int idx;
        struct grci_element op;
        if (grci_is_wire_output(s->token, symbols, &idx)) { //wire connect to another wire output
            grci_connect_wire_to_output(idx, output_offset, s->offset, symbols, module_decl);
        } else if (grci_is_module_input(s->token, symbols, &idx)) { //wire connect to module output that connects to module input - not allowed
            grci_ensure(false, GRCI_ERR_COMP, s->token.line,
                        "Invalid connection. Module input '%.*s' is connected to module output", 
                        s->token.literal.len, s->token.literal.ptr);
        } else if (s->token.type == GRCI_TT_INT_LITERAL) {
            grci_ensure(grci_token_matches(s->token, "0") || grci_token_matches(s->token, "1"), 
                        GRCI_ERR_COMP, s->token.line, "Constant inputs must be 0 or 1");
            int output_idx = grci_string_matches(&s->token.literal, "0", 1) ? GRCI_OUTPUT_0 : GRCI_OUTPUT_1;
            module_decl->outputs[*output_offset] = output_part(output_idx, output_idx);
            (*output_offset)++;
        } else if (grci_is_element_output(s->token, symbols, &op)) {
            int part_output_off = grci_absolute_offset(&symbols->parts.outputs[op.idx], op.output_idx);
            for (int k = 0; k < s->width; k++) {
                module_decl->outputs[*output_offset] = output_part(op.idx, part_output_off + k + s->offset);
                (*output_offset)++;
            }
        } else {
            grci_ensure(false, GRCI_ERR_COMP, s->token.line, 
                        "'%.*s' not declared", s->token.literal.len, s->token.literal.ptr);
        }
    }

    return GRCI_OK;
}

static grci_status grci_compiler_compile_module(struct grci_compiler *compiler) {
    struct grci_module_desc module_decl;
    grci_module_desc_init(&module_decl, &compiler->arena);

    struct grci_symbol_table symbols;
    grci_symbol_table_init(&symbols);

    grci_eat_token(compiler, "module");
    struct grci_token name = grci_next_token(compiler);
    compiler->current_module = &name;
    grci_ensure(grci_string_alloc(&compiler->arena, name.literal.ptr, name.literal.len, &module_decl.name),
                GRCI_ERR_MEM, 0, "placeholder");
    grci_eat_token(compiler, "(");

    grci_compiler_compile_parameter_list(compiler, &symbols.interface.inputs);
    module_decl.input_param_count = symbols.interface.inputs.count;
    module_decl.input_count = grci_absolute_offset(&symbols.interface.inputs, symbols.interface.inputs.count);

    grci_eat_token(compiler, ")");
    grci_eat_token(compiler, "-");
    grci_eat_token(compiler, ">");

    grci_compiler_compile_output_list(compiler, &symbols.interface.outputs);
    module_decl.output_param_count = symbols.interface.outputs.count;
    module_decl.output_count = grci_absolute_offset(&symbols.interface.outputs, symbols.interface.outputs.count);

    grci_eat_token(compiler, "{");

    grci_compile_part_list(compiler, module_decl.parts, module_decl.part_names, &symbols);
    module_decl.part_count = symbols.parts.count;

    grci_ensure(symbols.parts.count > 0 || symbols.wires.count > 0, GRCI_ERR_COMP, name.line,
                "Module '%.*s' must contain at least one part or wire", 
                module_decl.name.len, module_decl.name.ptr);

    grci_eat_token(compiler, "}");

    //set width of module inputs/output parameters
    grci_ensure(module_decl.input_count <= GRCI_MAX_INPUTS, GRCI_ERR_COMP, name.line,
                "Module '%.*s' exceeds the GRCI_MAX_INPUTS of %d", 
                module_decl.name.len, module_decl.name.ptr, GRCI_MAX_INPUTS);
    grci_ensure(module_decl.output_count <= GRCI_MAX_OUTPUTS, GRCI_ERR_COMP, name.line,
                "Module '%.*s' exceeds the GRCI_MAX_OUTPUTS of %d", 
                module_decl.name.len, module_decl.name.ptr, GRCI_MAX_OUTPUTS); 

    for (int i = 0; i < module_decl.input_count; i++) {
        module_decl.input_widths[i] = symbols.interface.inputs.entries[i].width;
    }
    for (int i = 0; i < module_decl.output_count; i++) {
        module_decl.output_widths[i] = symbols.interface.outputs.entries[i].width;
    }

    for (int i = 0; i < module_decl.part_count; i++) {
        grci_infer_implicit_element_conn_widths(&module_decl, i, &symbols);
    }

    for (int i = 0; i < symbols.wires.count; i++) {
        grci_infer_implicit_wire_conn_widths(&module_decl, i, &symbols);
    }

    //connect internal parts and wires
    for (int i = 0; i < module_decl.part_count; i++) {

        //connect internal parts
        struct grci_symbol_list *input_list = &symbols.parts.inputs[i];
        for (int j = 0; j < input_list->count; j++) {
            struct grci_token part_is = input_list->entries[j].token;
            int idx;
            struct grci_element part;
            if (grci_is_module_input(part_is, &symbols, &idx)) { //input into part is module input
                //identifier input
                int off = input_list->entries[j].offset;
                int input_offset = grci_absolute_offset(&symbols.interface.inputs, idx);
                for (int k = 0; k < input_list->entries[j].width; k++) {
                    grci_ensure(grci_connection_list_append(&module_decl.part_connections[i], extern_conn(off + input_offset + k)),
                                GRCI_ERR_MEM, 0, "placeholder");
                }
                //get module decl to make sure input widths match  //@current
                const struct grci_module_desc *part = module_decl.parts[i];
                grci_ensure(input_list->entries[j].width == part->input_widths[j], 
                            GRCI_ERR_COMP, part_is.line, "Input count does not match module declaration");
            } else if (grci_is_element_output(part_is, &symbols, &part)) { //input is from sibling part
                int output_offset = grci_absolute_offset(&symbols.parts.outputs[part.idx], part.output_idx);
                for (int k = 0; k < input_list->entries[j].width; k++) {    
                    grci_ensure(grci_connection_list_append(&module_decl.part_connections[i], intern_conn(part.idx, output_offset + input_list->entries[j].offset + k)),
                                GRCI_ERR_MEM, 0, "placeholder");
                }
            } else if (grci_is_wire_output(part_is, &symbols, &idx)) { //input is from wire
                int part_input_offset = grci_absolute_offset(input_list, j);
                grci_connect_wire_to_element(idx, i, part_input_offset, &symbols, &module_decl);
            } else if (part_is.type == GRCI_TT_INT_LITERAL) {
                grci_ensure(grci_token_matches(part_is, "0") || grci_token_matches(part_is, "1"), 
                            GRCI_ERR_COMP, part_is.line, "Constant input must be 0 or 1");
                bool value = grci_string_matches(&part_is.literal, "0", 1) ? 0: 1;
                for (int k = 0; k < input_list->entries[j].width; k++) {
                    grci_ensure(grci_connection_list_append(&module_decl.part_connections[i], const_conn(value)),
                                GRCI_ERR_MEM, 0, "placeholder");
                }
            } else if (grci_string_matches(&part_is.literal, "clock", 5)) { //not currently used
                grci_ensure(false, GRCI_ERR_COMP, part_is.line, "This is not being used right?");
                for (int k = 0; k < input_list->entries[j].width; k++) {
                    grci_ensure(grci_connection_list_append(&module_decl.part_connections[i], clock_conn()),
                                GRCI_ERR_MEM, 0, "placeholder");
                }
            } else {
                grci_ensure(false, GRCI_ERR_COMP, part_is.line, "Identifier '%.*s' not declared",
                            part_is.literal.len, part_is.literal.ptr);
            }

        }
    }

    //connect parts to module outputs
    
    for (int i = 0; i < module_decl.part_count; i++) {
        struct grci_symbol_list *output_list = &symbols.parts.outputs[i];
        int input_offset = 0;
        for (int j = 0; j < output_list->count; j++) {
            struct grci_symbol_entry *s = &output_list->entries[j];

            int idx;
            if (grci_is_module_output(s->token, &symbols, &idx)) {
                int output_offset = grci_absolute_offset(&symbols.interface.outputs, idx);
                for (int k = 0; k < s->width; k++) {
                    module_decl.outputs[output_offset + k + s->offset] = output_part(i, input_offset + k);
                }

                grci_ensure(s->width <= symbols.interface.outputs.entries[idx].width, GRCI_ERR_COMP, s->token.line,
                            "Output is larger in width than module output");
            }
            input_offset += s->width;
        }
    }

    //connecting wires to module outputs
    for (int i = 0; i < symbols.wires.count; i++) {
        int idx;
        struct grci_symbol_entry *s = &symbols.wires.outputs.entries[i];
        if (grci_is_module_output(s->token, &symbols, &idx)) {
            int output_offset = grci_absolute_offset(&symbols.interface.outputs, idx) + s->offset;
            int relative_offset = 0;
            grci_connect_wire_to_output(i, &output_offset, relative_offset, &symbols, &module_decl);
        }
        //what was supposed to happen here? Anything?
    }

    for (int i = 0; i < module_decl.input_count; i++) {
        module_decl.sink_counts[i] = 0;
    }

    for (int part_idx = 0; part_idx < module_decl.part_count; part_idx++) {
        for (int i = 0; i < module_decl.part_connections[part_idx].count; i++) {
            struct grci_connection c = module_decl.part_connections[part_idx].values[i];
            if (c.type == GRCI_IT_EXTERNAL) {
                module_decl.sink_counts[c.get.parameter_idx] += module_decl.parts[part_idx]->sink_counts[i];
            }
        }
    }

    for (int part_idx = 0; part_idx < module_decl.part_count; part_idx++) {
        module_decl.node_count += module_decl.parts[part_idx]->node_count;
        module_decl.dff_count += module_decl.parts[part_idx]->dff_count;
    }

    grci_module_desc_list_append(&compiler->module_defs, &module_decl);
    compiler->current_module = NULL;
    
    return GRCI_OK;
}

/*
 * Simulator
 */

struct grc_input_sink {
    struct grci_node ***ptps;
    int count;
};

struct grci_module_instance {
    const struct grci_module_desc *desc;
    struct grc_input_sink sinks[GRCI_MAX_INPUTS];
    struct grci_node *inputs[GRCI_MAX_INPUTS];
    struct grci_node *outputs[GRCI_MAX_OUTPUTS];

    struct grci_submodule submodules[GRCI_MAX_PARTS];
    int dff_off_len[GRCI_MAX_PARTS][2];
};

enum grci_node_type {
    GRCI_NT_CONSTANT,
    GRCI_NT_NAND,
    GRCI_NT_DFF,
    GRCI_NT_RAM64KOUT
};

struct grci_nand {
    struct grci_node *a;
    struct grci_node *b;
};

struct grci_dff {
    struct grci_node *input;
    bool last_state;
};

struct grci_ram64k {
    //external nodes - these should be assigned during instantiation
    struct grci_node *inputs[16];
    struct grci_node *load;
    struct grci_node *addrs[16];

    char *data;

    //these should be made
    struct grci_node *outputs[16];
};

struct grci_ram64kOut {
    struct grci_ram64k *ram;
    bool last_state;
};


struct grci_node {
    union {
        struct grci_nand nand;
        bool constant;
        struct grci_dff dff;
        struct grci_ram64kOut ram64Kout;
    } as;
    enum grci_node_type type;
    bool visited;
    bool cached_state;
};


struct grci_simulator {
    struct grci_arena arena;

    struct grci_node *nodes;
    int node_count;

    //keeping extra list of dff references since they need to be treated
    //a little differently during simulation.  There is (usually)
    //fewer dffs compared to nand nodes, so this list can be iterated
    //more quickly if only dffs need to be updated
    struct grci_node **dff_nodes;
    int dff_node_count;

    struct grci_node *const0;
    struct grci_node *const1;
    struct grci_node *clock;
};

//this function is only ever called on a high clock signal
static bool grci_eval_dff(struct grci_node *node, bool root) {
    assert(node && "Node is NULL");
    if (node->visited) {
        if (node->type == GRCI_NT_DFF) {
            return node->as.dff.last_state;
        }
        return node->cached_state;
    }

    node->visited = true;

    switch (node->type) {
    case GRCI_NT_CONSTANT:
        node->cached_state = node->as.constant;
        break;
    case GRCI_NT_NAND:
        assert(node->as.nand.a);
        assert(node->as.nand.b);
        bool left = grci_eval_dff(node->as.nand.a, false);
        bool right = grci_eval_dff(node->as.nand.b, false);
        node->cached_state = !(left && right);
        break;
    case GRCI_NT_DFF:
        if (!root) return node->cached_state;
        assert(node->as.dff.input);
        node->cached_state = grci_eval_dff(node->as.dff.input, false);
        break;
    case GRCI_NT_RAM64KOUT: {
        struct grci_ram64k *ram = node->as.ram64Kout.ram;
        char input1 = 0;
        char input2 = 0;
        int addr = 0;
        bool load = grci_eval_dff(ram->load, false);
        for (int i = 0; i < 16; i++) {
            if (i < 8)
                input1 |= grci_eval_dff(ram->inputs[i], false) << i;
            else
                input2 |= grci_eval_dff(ram->inputs[i], false) << (i - 8);
            addr  |= grci_eval_dff(ram->addrs[i], false) << i;
        }
        if (load) {
            ram->data[addr] = input1;
            ram->data[addr + 1] = input2;
        }
        //need 16 bits, so just grabbing an int, which should be at least 16 bits
        int output = *((int*) &ram->data[addr]);
        for (int i = 0; i < 16; i++) {
            ram->outputs[i]->visited = true;
            ram->outputs[i]->cached_state = (output >> i) & 1;
        }
        break;
    }
    default:
        assert(false);
        break;
    }
    
    return node->cached_state;
}

static bool grci_eval_node(struct grci_node *node) {
    assert(node && "Node is NULL");
    if (node->visited) {
        if (node->type == GRCI_NT_DFF) {
            return node->as.dff.last_state;
        }
        return node->cached_state;
    }

    node->visited = true;

    switch (node->type) {
    case GRCI_NT_CONSTANT:
        node->cached_state = node->as.constant;
        break;
    case GRCI_NT_NAND:
        assert(node->as.nand.a);
        assert(node->as.nand.b);
        bool left = grci_eval_node(node->as.nand.a);
        bool right = grci_eval_node(node->as.nand.b);
        node->cached_state = !(left && right);
        break;
    case GRCI_NT_DFF:
        assert(node->as.dff.input);
        break;
    case GRCI_NT_RAM64KOUT: {
        struct grci_ram64k *ram = node->as.ram64Kout.ram;
        int addr = 0;
        for (int i = 0; i < 16; i++) {
            addr  |= grci_eval_dff(ram->addrs[i], false) << i;
        }
        int output = *((int*) &ram->data[addr]);
        for (int i = 0; i < 16; i++) {
            ram->outputs[i]->visited = true;
            ram->outputs[i]->cached_state = (output >> i) & 1;
        }
        break;
    }
    default:
        assert(false);
        break;
    }
    
    return node->cached_state;
}

struct grci_node *grci_constant_new(struct grci_simulator *sim, bool c) {
    struct grci_node *n = &sim->nodes[sim->node_count];
    sim->node_count++;
    n->type = GRCI_NT_CONSTANT;
    n->visited = false;
    n->cached_state = c;

    n->as.constant = c;
    return n;
}

struct grci_node *grci_nand_new(struct grci_simulator *sim, struct grci_node *a, struct grci_node *b) {
    struct grci_node *n = &sim->nodes[sim->node_count];
    sim->node_count++;
    n->type = GRCI_NT_NAND;
    n->visited = false;
    n->cached_state = false;

    n->as.nand.a = a;
    n->as.nand.b = b;
    return n;
}


struct grci_node *grci_dff_new(struct grci_simulator *sim) {
    struct grci_node *n = &sim->nodes[sim->node_count];
    sim->node_count++;
    n->type = GRCI_NT_DFF;
    n->visited = false;
    n->cached_state = false;

    n->as.dff.last_state = false;

    sim->dff_nodes[sim->dff_node_count] = n;
    sim->dff_node_count++;

    return n;
}

struct grci_node *grci_ram64kout_new(struct grci_simulator *sim, struct grci_ram64k *ram) {
    struct grci_node *n = &sim->nodes[sim->node_count];
    sim->node_count++;
    n->type = GRCI_NT_RAM64KOUT;
    n->visited = false;
    n->cached_state = false;

    n->as.ram64Kout.ram = ram;
    n->as.ram64Kout.last_state = false;

    sim->dff_nodes[sim->dff_node_count] = n;
    sim->dff_node_count++;

    return n;
}


struct grci_ram64k *grci_ram64k_new(struct grci_simulator *sim) {
    struct grci_ram64k *ram;
    grci_ensure_retnull(grci_arena_malloc(&sim->arena, sizeof(struct grci_ram64k), (void **) &ram),
                        GRCI_ERR_MEM, 0, "placeholder");

    for (int i = 0; i < 16; i++) {
        ram->outputs[i] = grci_ram64kout_new(sim, ram);
    }

    grci_ensure_retnull(grci_arena_malloc(&sim->arena, 65536, (void**) &ram->data),
                        GRCI_ERR_MEM, 0, "placeholder");
    return ram;
}

struct grci_input_data {
    enum grci_input_type type;
    union {
        struct grci_node *obj;
        struct grc_input_sink *sink;
    } as;
};

struct grci_input_list {
    struct grci_input_data *values;
    int capacity;
    int count;
};

static grci_status grci_input_list_init(struct grci_arena *arena, struct grci_input_list *l, int capacity) {
    l->capacity = capacity;
    grci_ensure(grci_arena_malloc(arena, sizeof(struct grci_input_data) * capacity, (void **) &l->values),
                GRCI_ERR_MEM, 0, "placeholder");
    l->count = 0;
    return GRCI_OK;
}

static void grci_input_list_append(struct grci_input_list *l, struct grci_input_data pc) {
    assert(l->count < l->capacity);

    l->values[l->count] = pc;
    l->count++;
}

static void grci_set_module_inputs(struct grci_module_instance *api, const struct grci_input_data *inputs) {
    for (int i = 0; i < api->desc->input_count; i++) {
        const struct grci_input_data *input = &inputs[i];
        for (int j = 0; j < api->desc->sink_counts[i]; j++) {
            switch (input->type) {
            case GRCI_IT_INTERNAL:
                *(api->sinks[i].ptps[j]) = input->as.obj;
                break;
            case GRCI_IT_EXTERNAL:
                input->as.sink->ptps[input->as.sink->count] = api->sinks[i].ptps[j];
                input->as.sink->count++;
                break;
            case GRCI_IT_CONSTANT:
            case GRCI_IT_CLOCK:
                *(api->sinks[i].ptps[j]) = input->as.obj;
                break;
            default:
                assert(false && "Unknown input type");
            }
        }
    }
}

static grci_status grci_make_module(struct grci_simulator *sim, struct grci_module_instance *api) {
    const struct grci_module_desc *data = api->desc;

    
    struct grci_module_instance *apis;
    grci_ensure(grci_arena_malloc(&sim->arena, sizeof(struct grci_module_instance) * data->part_count, (void **)&apis), 
                GRCI_ERR_MEM, 0, "placeholder");
    assert(apis);
    for (int i = 0; i < data->part_count; i++) {
        api->dff_off_len[i][0] = sim->dff_node_count;
        apis[i].desc = api->desc->parts[i];
        grci_make_module(sim, &apis[i]);
        api->dff_off_len[i][1] = sim->dff_node_count - api->dff_off_len[i][0];
    }

    for (int i = 0; i < data->input_count; i++) {
        int sink_count = api->desc->sink_counts[i];
        grci_ensure(grci_arena_malloc(&sim->arena, sizeof(struct grci_node**) * sink_count, (void**) &api->sinks[i].ptps), 
                    GRCI_ERR_MEM, 0, "placeholder");
        api->sinks[i].count = 0;
    }

    //built-in modules
    if (data->is_nand) {
        struct grci_node *node = grci_nand_new(sim, NULL, NULL);

        //inputs connect to nand
        api->sinks[0].ptps[0] = &node->as.nand.a;
        api->sinks[1].ptps[0] = &node->as.nand.b;

        //nand connects to output
        api->outputs[0] = node;

        return GRCI_OK;
    } else if (data->is_dff) {
        struct grci_node *node = grci_dff_new(sim);
        //input to connect to dff
        api->sinks[0].ptps[0] = &node->as.dff.input;

        //dff connections to output
        api->outputs[0] = node;
        return GRCI_OK;
    } else if (data->is_ram64K) {
        struct grci_ram64k *ram = grci_ram64k_new(sim);
        for (int i = 0; i < 16; i++) {
            api->sinks[i].ptps[0] = &ram->inputs[i];
        }
        api->sinks[16].ptps[0] = &ram->load;
        for (int i = 17; i < 33; i++) {
            api->sinks[i].ptps[0] = &ram->addrs[i - 17];
        }


        for (int i = 0; i < 16; i++) {
            api->outputs[i] = ram->outputs[i]; 
        }

        return GRCI_OK;
    }


    //Set inputs for internal elements.  Module inputs are set during module instantiation
    for (int part_idx = 0; part_idx < data->part_count; part_idx++) {
        struct grci_input_list inputs;
        grci_ensure(grci_input_list_init(&sim->arena, &inputs, data->part_connections[part_idx].count),
                    GRCI_ERR_MEM, 0, "placeholder");
        for (int i = 0; i < data->part_connections[part_idx].count; i++) {
            struct grci_connection c = data->part_connections[part_idx].values[i];
            struct grci_input_data input;
            switch (c.type) {
            case GRCI_IT_EXTERNAL:
                input = (struct grci_input_data) { .type=GRCI_IT_EXTERNAL, 
                                                   .as.sink=&api->sinks[c.get.parameter_idx] };
                break;
            case GRCI_IT_INTERNAL:
                input = (struct grci_input_data) { .type=GRCI_IT_INTERNAL, 
                                                   .as.obj=apis[c.get.part.idx].outputs[c.get.part.output_idx] };
                break;
            case GRCI_IT_CONSTANT:
                input = (struct grci_input_data) { .type=GRCI_IT_CONSTANT, 
                                                   .as.obj= c.get.constant ? sim->const1 : sim->const0 };
                break;
            case GRCI_IT_CLOCK:
                input = (struct grci_input_data) { .type=GRCI_IT_CLOCK, 
                                                   .as.obj=sim->clock };
                break;
            default:
                assert(false && "Uknown input type");
                break;
            }
            grci_input_list_append(&inputs, input);
        }

        grci_set_module_inputs(&apis[part_idx], inputs.values);
    }

    for (int i = 0; i < data->output_count; i++) {
        const struct grci_element *part = &data->outputs[i];
        switch (part->idx) {
        case GRCI_OUTPUT_0:
            api->outputs[i] = sim->const0;
            break;
        case GRCI_OUTPUT_1:
            api->outputs[i] = sim->const1;
            break;
        default:
            api->outputs[i] = apis[part->idx].outputs[part->output_idx];
            break;
        }
    }

    return GRCI_OK;
}


static void grci_simulator_init(struct grci_simulator *sim, 
                                void* (*malloc)(size_t), 
                                void* (*realloc)(void*, size_t), 
                                void (*free)(void*), 
                                int node_count, 
                                int dff_count) {

    node_count += 3; //for const0, const1, and clock
    grci_arena_init(&sim->arena, malloc, realloc, free);
    sim->nodes = malloc(sizeof(struct grci_node) * node_count);
    sim->node_count = 0;
    sim->dff_nodes = malloc(sizeof(struct grci_node*) * dff_count);
    sim->dff_node_count = 0;

    sim->const0 = grci_constant_new(sim, 0);
    sim->const1 = grci_constant_new(sim, 1);
    sim->clock = grci_constant_new(sim, 1); //clock signal is changed first in grci_step_module, so this allows first step to be on low clock signal
}

static void grci_simulator_cleanup(struct grci_simulator *sim) {
    grci_arena_cleanup(&sim->arena);
    free(sim->nodes);
    free(sim->dff_nodes);
}

static void grci_module_desc_print(const struct grci_module_desc *m) {
    printf("Module name: %.*s\n", m->name.len, m->name.ptr);
    printf("\tPart count: %d\n", m->part_count);
    for (int j = 0; j < m->part_count; j++) {
        printf("\t\t%.*s\n", m->parts[j]->name.len, m->parts[j]->name.ptr);
    }
    printf("\tInput Count: %d\n", m->input_count);
    printf("\tInput Param Count: %d\n", m->input_param_count);
    printf("\tInput Param Widths:\n");
    for (int i = 0; i < m->input_param_count; i++) {
        printf("\t\t%d\n", m->input_widths[i]);
    }
    printf("\tOutput Count: %d\n", m->output_count);
    printf("\tOutput Param Count: %d\n", m->output_param_count);
    printf("\tOutput Param Widths:\n");
    for (int i = 0; i < m->output_param_count; i++) {
        printf("\t\t%d\n", m->output_widths[i]);
    }
    printf("\tOutput Parts\n");
    for (int j = 0; j < m->output_count; j++) {
        printf("\t\t%d, %d\n", m->outputs[j].idx, m->outputs[j].output_idx);
    }

    printf("\tConnections\n");
    for (int j = 0; j < m->part_count; j++) {
        const struct grci_connection_list *list = &m->part_connections[j];
        for (int k = 0; k < list->count; k++) {
            struct grci_connection c = list->values[k];
            if (c.type == GRCI_IT_EXTERNAL) {
                printf("\t\tconnections[%d]: %d\n", j, c.get.parameter_idx);
            } else if (c.type == GRCI_IT_INTERNAL) {
                printf("\t\tconnections[%d]: %d, %d\n", j, c.get.part.idx, c.get.part.output_idx);
            } else {
                printf("\t\tconnections[%d]: %d\n", j, c.get.constant);
            }
        }
    }
}



/*
 * API
 */

struct grci {
    void* (*client_malloc)(size_t);
    void* (*client_realloc)(void*, size_t);
    void (*client_free)(void*);
    struct grci_compiler compiler;
};

struct grci_sim {
    struct grci_simulator sim;
    struct grci_module_instance module;
    struct grci *g;
};

struct grci* grci_init(void* (*malloc)(size_t), void* (*realloc)(void*, size_t), void (*free)(void*)) {
    struct grci *g = malloc(sizeof(struct grci));
    grci_ensure_retnull(g, GRCI_ERR_MEM, 0, "malloc failed");

    g->client_malloc = malloc;
    g->client_realloc = realloc;
    g->client_free = free;
    grci_compiler_init(&g->compiler, malloc, realloc, free);

    grci_err_buf[0] = '\0';

    return g;
}
struct grci* grci_easy_init(void) {
    return grci_init(malloc, realloc, free); 
}

grci_status grci_compile_src(struct grci *g, const char *buf, size_t len) {
    struct grci_compiler *compiler = &g->compiler;

    compiler->tokenizer.buf = buf;
    compiler->tokenizer.idx = 0;
    compiler->cur = grci_tokenizer_next(&compiler->tokenizer);
    compiler->next = grci_tokenizer_next(&compiler->tokenizer);
    compiler->current_file = "fake file name";

    while (true) {
        struct grci_token peek = grci_peek_next(compiler);
        if (peek.type == GRCI_TT_EOF) {
            break;
        } else if (grci_string_matches(&peek.literal, "module", 6)) {
            struct grci_token pp = grci_peek_two(compiler);
            //printf("compiling: %.*s\n", pp.literal.len, pp.literal.ptr);
            grci_ensure(grci_compiler_compile_module(compiler), GRCI_ERR_COMP, -1, "placeholder");
        } else {
            grci_ensure(false, GRCI_ERR_COMP, peek.line, "Use keyword 'module' to make a new module");
        }
    }
    compiler->current_file = NULL;

    return GRCI_OK;
}

struct grci_module *grci_init_module(struct grci *g, const char *module_name, size_t len) {
    struct grci_module *module = g->client_malloc(sizeof(struct grci_module));
    struct grci_string string = { .ptr = module_name, .len = len };
    //printf("\n");

    for (int i = 0; i < g->compiler.module_defs.count; i++) {
            const struct grci_module_desc *cur = &g->compiler.module_defs.entries[i];
            //printf("list item: %.*s\n", cur->name.len, cur->name.ptr);
    }
    const struct grci_module_desc* decl = grci_module_desc_list_get(&g->compiler.module_defs, &string);
    printf("\n");
    if (!decl) {
        printf("*************%.*s\n", string.len, string.ptr);
    }
    assert(decl);

    module->sim = g->client_malloc(sizeof(struct grci_sim));
    module->sim->g = g;
    //decl->input_count is added to total node count since inputs are NOT included during module compilation
    grci_simulator_init(&module->sim->sim, 
                        g->client_malloc, 
                        g->client_realloc, 
                        g->client_free, 
                        decl->node_count + decl->input_count, 
                        decl->dff_count);

    struct grci_module_instance *m = &module->sim->module;
    m->desc = decl;
    grci_make_module(&module->sim->sim, m);

    int input_count = decl->input_count;
    int output_count = decl->output_count;

    module->inputs = g->client_malloc(sizeof(bool) * input_count);
    module->input_count = input_count;
    module->outputs = g->client_malloc(sizeof(bool) * output_count);
    module->output_count = output_count;

    struct grci_input_data *inputs;
    grci_ensure_retnull(grci_arena_malloc(&module->sim->sim.arena, sizeof(struct grci_input_data) * input_count, (void**) &inputs),
                      GRCI_ERR_MEM, 0, "placeholder");
    assert(inputs);
    for (int i = 0; i < input_count; i++) {
        inputs[i] = (struct grci_input_data) { .type=GRCI_IT_INTERNAL, 
                                               .as.obj=grci_constant_new(&module->sim->sim, 0) };
        m->inputs[i] = inputs[i].as.obj;
    }

    grci_set_module_inputs(m, inputs);

    //submodules
    for (int i = 0; i < m->desc->part_count; i++) {
        m->submodules[i].state_count = m->dff_off_len[i][1];
        if (grci_string_matches(&decl->parts[i]->name, "Ram64K", 6)) {
            m->submodules[i].state_count = GRCI_RAM64K_STATE_COUNT;
        }
        grci_ensure_retnull(grci_arena_malloc(&module->sim->sim.arena, sizeof(bool) * m->submodules[i].state_count, (void **)&m->submodules[i].states), 
                          GRCI_ERR_MEM, 0, "placeholder");
        memset(m->submodules[i].states, 0, sizeof(bool) * m->submodules[i].state_count);
    }

    return module;
}

struct grci_submodule *grci_submodule(struct grci_module *m, const char *submodule_name, size_t len) {
    for (int i = 0; i < m->sim->module.desc->part_count; i++) {
        if (!m->sim->module.desc->part_names[i].ptr) continue;

        if (grci_string_matches(&m->sim->module.desc->part_names[i], submodule_name, len)) {
            return &m->sim->module.submodules[i];
        }
    }

    grci_ensure_retnull(false, GRCI_ERR_SIM, 0, "submodule %.*s does not exist", (int) len, submodule_name);
    return NULL;
}

bool grci_step_module(struct grci_module *m) {
    //set module inputs
    for (int i = 0; i < m->sim->module.desc->input_count; i++) {
        m->sim->module.inputs[i]->as.constant = m->inputs[i];
    }

    //set submodule states
    for (int i = 0; i < m->sim->module.desc->part_count; i++) {
        if (!m->sim->module.desc->part_names[i].ptr) continue;
        struct grci_submodule *sub = &m->sim->module.submodules[i];

        if (grci_string_matches(&m->sim->module.desc->parts[i]->name, "Ram64K", 6)) {
            //just grabbing first of 16 dff nodes on Ram64K outputs in order to get struct grci_ram64k*
            struct grci_node *node = m->sim->sim.dff_nodes[m->sim->module.dff_off_len[i][0]];
            assert(node->type == GRCI_NT_RAM64KOUT);
            struct grci_ram64k *ram = node->as.ram64Kout.ram;
          
            char c = 0;
            for (int i = 0; i < GRCI_RAM64K_STATE_COUNT; i++) {
                c |= sub->states[i] << (i % 8);
                if (i % 8 == 7) {
                    ram->data[i / 8] = c;
                    c = 0;
                }
            }

        } else {
            int dff_off = m->sim->module.dff_off_len[i][0];
            int dff_len = m->sim->module.dff_off_len[i][1];
            for (int j = 0; j < dff_len; j++) {
                m->sim->sim.dff_nodes[j + dff_off]->as.dff.last_state = sub->states[j];
                m->sim->sim.dff_nodes[j + dff_off]->cached_state = sub->states[j];
            }
        }
    }

    struct grci_simulator *sim = &m->sim->sim;

    sim->clock->as.constant = sim->clock->as.constant == 0 ? 1: 0;

    //not reseting nodes recursively since that's too slow
    for (int k = 0; k < sim->node_count; k++) {
        sim->nodes[k].visited = false;
    }

    //updating all flip flops first prevents bug with combinationals gates getting old data when evaluated first
    if (sim->clock->as.constant) {
        for (int k = 0; k < sim->dff_node_count; k++) {
            sim->dff_nodes[k]->visited = false;
            grci_eval_dff(sim->dff_nodes[k], true);

            for (int k = 0; k < sim->node_count; k++) {
                if (sim->nodes[k].type != GRCI_NT_DFF)
                    sim->nodes[k].visited = false;
            }
        }

        //setting state of flip flops to new value
        for (int k = 0; k < sim->dff_node_count; k++) {
            sim->dff_nodes[k]->as.dff.last_state = sim->dff_nodes[k]->cached_state;
        }
    }


    for (int k = 0; k < m->sim->module.desc->output_count; k++) {
        bool v = grci_eval_node(m->sim->module.outputs[k]);
        m->outputs[k] = v;
    }

    //set submodule states
    for (int i = 0; i < m->sim->module.desc->part_count; i++) {
        if (!m->sim->module.desc->part_names[i].ptr) continue;
        struct grci_submodule *sub = &m->sim->module.submodules[i];

        if (grci_string_matches(&m->sim->module.desc->parts[i]->name, "Ram64K", 6)) {
            //just grabbing first of 16 dff nodes on Ram64K outputs in order to get struct grci_ram64k*
            struct grci_node *node = m->sim->sim.dff_nodes[m->sim->module.dff_off_len[i][0]];
            assert(node->type == GRCI_NT_RAM64KOUT);
            struct grci_ram64k *ram = node->as.ram64Kout.ram;
            
            for (int i = 0; i < GRCI_RAM64K_STATE_COUNT; i++) {
                char c = ram->data[i / 8];
                sub->states[i] = (c >> (i % 8)) & 1;
            }
        } else {
            int dff_off = m->sim->module.dff_off_len[i][0];
            int dff_len = m->sim->module.dff_off_len[i][1];
            for (int j = 0; j < dff_len; j++) {
                sub->states[j] = m->sim->sim.dff_nodes[j + dff_off]->as.dff.last_state;
            }
        }
    }

    return sim->clock->as.constant;
}

void grci_destroy_module(struct grci_module *m) {
    grci_simulator_cleanup(&m->sim->sim);
    void (*free)(void*) = m->sim->g->client_free;
    free(m->sim);
    free(m);
}
void grci_cleanup(struct grci *g) {
    g->client_free(g);
}

const char *grci_err(void) {
    return grci_err_buf;
}

void grci_set_input(struct grci_module *m, int idx, bool value) {
    m->inputs[idx] = value;
}

bool grci_get_output(struct grci_module *m, int idx) {
    return m->outputs[idx];
}


#undef GRCI_DEFAULT_CHUNK_SIZE
#undef GRCI_RAM64K_STATE_COUNT

#undef GRCI_MAX_PARTS
#undef GRCI_MAX_WIRES
#undef GRCI_MAX_INPUTS
#undef GRCI_MAX_OUTPUTS
#undef GRCI_MAX_MODULES

#undef GRCI_OUTPUT_NONE
#undef GRCI_OUTPUT_0
#undef GRCI_OUTPUT_1

#undef GRCI_UNKNOWN_WIDTH
