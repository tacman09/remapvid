#ifndef QPU_UTIL_H
#define QPU_UTIL_H

unsigned int texture_config_0(unsigned int base, unsigned int flipy, unsigned int ttype) {
    unsigned int cswiz = 0;
    unsigned int cmmode = 0;
    unsigned int miplvls = 0;
    return (((base>>12)&0xFFFFF)<<12|cswiz<<10|cmmode<<9|flipy<<8|(ttype&0xf)<<4|miplvls);
}

unsigned int texture_config_1(unsigned int height, unsigned int width, unsigned int magfilt, unsigned int minfilt, unsigned int wrap_t, unsigned int wrap_s, unsigned int ttype) {
    unsigned int ttype4 = (ttype & 0x10) >> 4;
    unsigned int etcflip = 0;
    return (ttype4<<31|height<<20|etcflip<<19|width<<8|magfilt<<7|minfilt<<4|wrap_t<<2|wrap_s);
}

unsigned int vpm_write_y_config(unsigned int thread) {
    unsigned int stride = 1; // B += 1
    unsigned int size = 0; // 8-bit
    unsigned int laned = 0; // packed
    unsigned int horizontal = 1; // horizontal
    unsigned int Y = thread;
    unsigned int B = 0;
    unsigned int addr = ((Y & 0x3f) << 2) | (B & 0x3);
    return (stride<<12|horizontal<<11|laned<<10|size<<8|addr);
}

unsigned int vpm_write_uv_config(unsigned int thread) {
    unsigned int stride = thread % 2 == 0 ? 24 : 1; // Y += 6
    unsigned int size = 0; // 8-bit
    unsigned int laned = 0; // packed
    unsigned int horizontal = 1; // horizontal
    unsigned int Y = thread % 2 == 0 ? (24 + thread / 2) : 36;
    unsigned int B = 0;
    unsigned int addr = ((Y & 0x3f) << 2) | (B & 0x3);
    return (stride<<12|horizontal<<11|laned<<10|size<<8|addr);
}

#endif