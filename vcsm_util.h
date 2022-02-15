#ifndef VCSM_UTIL_H
#define VCSM_UTIL_H

#include <stdbool.h>

#define MAX_NUM_CODE_WORDS  (8192*16)
#define MAX_NUM_UNIFORMS    16
#define MAX_NUM_QPUS        12

typedef struct {
    unsigned int size;
    unsigned int handle;
    unsigned int vc_mem_addr;
    void *usr_mem_ptr;
} vcsm_util_buffer_t;

void vcsm_util_buffer_create(vcsm_util_buffer_t *buffer, size_t size);
void vcsm_util_buffer_destroy(vcsm_util_buffer_t *buffer);
bool vcsm_util_buffer_load_from_file(vcsm_util_buffer_t *buffer, FILE *fp, size_t size);

typedef struct {
    unsigned int code[MAX_NUM_CODE_WORDS];
    unsigned int uniforms[MAX_NUM_UNIFORMS * MAX_NUM_QPUS];
    unsigned int msg[2 * MAX_NUM_QPUS];
} vcsm_util_program_mmap_t;

typedef struct {
    int num_qpus;
    vcsm_util_buffer_t buffer;
    vcsm_util_program_mmap_t *mmap;
    unsigned int vc_msg;
} vcsm_util_program_t;

void vcsm_util_program_create(vcsm_util_program_t *program, int num_qpus);
void vcsm_util_program_load_from_memory(vcsm_util_program_t *program, void *ptr, size_t size);
void vcsm_util_program_destroy(vcsm_util_program_t *program);

#endif
