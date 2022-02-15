#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <time.h>
#include <stdlib.h>
#include <interface/vcsm/user-vcsm.h>

#include "vcsm_util.h"
#include "mailbox.h"

void vcsm_util_buffer_create(vcsm_util_buffer_t *buffer, size_t size) {
    buffer->size = size;
    buffer->handle = vcsm_malloc(size, "vcsm_util_buffer_create");
    buffer->usr_mem_ptr = vcsm_lock(buffer->handle);
    vcsm_unlock_ptr(buffer->usr_mem_ptr);
    buffer->vc_mem_addr = vcsm_vc_addr_from_hdl(buffer->handle);
}

void vcsm_util_buffer_destroy(vcsm_util_buffer_t *buffer) {
    vcsm_free(buffer->handle);
}

bool vcsm_util_buffer_load_from_file(vcsm_util_buffer_t *buffer, FILE *fp, size_t size) {
    bool result = false;
    vcsm_lock(buffer->handle);
    size_t s = fread(buffer->usr_mem_ptr, 1, size, fp);
    if (s != size) {
        fprintf(stderr, "ERROR: failed to read map file\n");
        goto error;
    }
    result = true;

error:
    vcsm_unlock_ptr(buffer->usr_mem_ptr);
    return result;
}

void vcsm_util_program_create(vcsm_util_program_t *program, int num_qpus) {
    program->num_qpus = num_qpus;
    
    vcsm_util_buffer_t *buffer = &program->buffer;
    
    unsigned int size = sizeof(vcsm_util_program_mmap_t);
    vcsm_util_buffer_create(buffer, size);
    
    vcsm_lock(buffer->handle);

    vcsm_util_program_mmap_t *mmap = (vcsm_util_program_mmap_t *) buffer->usr_mem_ptr;
    memset(mmap, 0x0, sizeof(vcsm_util_program_mmap_t));
    program->mmap = mmap;

    unsigned int ptr = buffer->vc_mem_addr;
    unsigned vc_msg = ptr + offsetof(vcsm_util_program_mmap_t, msg);
    program->vc_msg = vc_msg;
    
    vcsm_unlock_ptr(buffer->usr_mem_ptr);
}

void vcsm_util_program_load_from_memory(vcsm_util_program_t *program, void *ptr, size_t size) {
    vcsm_lock(program->buffer.handle);
    memcpy(program->mmap->code, ptr, size);
    vcsm_unlock_ptr(program->buffer.usr_mem_ptr);
}

void vcsm_util_program_destroy(vcsm_util_program_t *program) {
    vcsm_util_buffer_destroy(&program->buffer);
}
