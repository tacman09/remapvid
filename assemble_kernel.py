from videocore.assembler import qpu, sema_down, sema_up, wait_dma_store
from videocore.driver import Driver
import sys

# register usage

# u0 : uniform base address
# u1 : texture config 0
# u2 : texture config 1
# u3 : texture config 2
# u4 : texture config 3
# u5 : qpu id
# u6 : remap base address
# u7 : frame buffer base address
# u8 : vpm write y config
# u9 : vpm write uv config
# u12: x tile count
# u13: y tile count
# u10: frame buffer width
# u11: frame buffer height
# u14: -
# u15: -

# r0: temp
# r1: temp
# r2: temp
# r3: temp
# r4: tmu
# r5: broadcast

# ra0 : -
# ra1 : thread index
# ra2 : rgba texture config uniform base address
# ra3 : remap address
# ra4 : y address
# ra5 : u address
# ra6 : v address
# ra7 : x tile index
# ra8 : y tile index
# ra9 : st coord
# ra10: y register
# ra11: u register
# ra12: v register
# ra13: s coord
# ra14: t coord
# ra15: yuvx register
# ra16: vpm write y(0,0) setup register
# ra17: vpm write uv(0,0) setup register
# ra18: 1/65535
# ra19: -
# ra20: -
# ra21: -
# ra22: -
# ra23: -
# ra24: -
# ra25: -
# ra26: -
# ra27: -
# ra28: -
# ra29: -
# ra30: -
# ra31: -

# rb0 : -
# rb1 : -
# rb2 : -
# rb3 : -
# rb4 : -
# rb5 : -
# rb6 : -
# rb7 : x tile count
# rb8 : y tile count
# rb9 : remap address increment
# rb10: y address increment (column)
# rb11: u,v address increment (column)
# rb12 : y address increment (row)
# rb13 : u,v address increment (row)
# rb14 : dma store stride for y
# rb15 : dma store stride for u,v
# rb16 : -
# rb17 : -
# rb18 : -
# rb19 : -
# rb20 : -
# rb21 : -
# rb22 : -
# rb23 : -
# rb24 : -
# rb25 : -
# rb26 : -
# rb27 : -
# rb28 : -
# rb29 : -
# rb30 : -
# rb31 : -

# Semaphore index
COMPLETED           = 0
HALF_TILE_SYNC_PRE  = 1
HALF_TILE_SYNC_POST = 2

def mask(idx):
    idxs = idx if isinstance(idx, list) else [idx]
    return [0 if i in idxs else 1 for i in range(16)]

def texture_config_0(base, flipy, ttype):
    cswiz = 0
    cmmode = 0
    miplvls = 0
    return (((base>>12)&0xFFFFF)<<12|cswiz<<10|cmmode<<9|flipy<<8|(ttype&0xf)<<4|miplvls)

def texture_config_1(height, width, magfilt, minfilt, wrap_t, wrap_s, ttype):
    ttype4 = (ttype & 0x10) >> 4
    etcflip = 0
    return (ttype4<<31|height<<20|etcflip<<19|width<<8|magfilt<<7|minfilt<<4|wrap_t<<2|wrap_s)

def vpm_write_y_config(thread):
    stride = 1 # B += 1
    size = 0 # 8-bit
    laned = 0 # packed
    horizontal = 1 # horizontal
    Y = thread
    B = 0
    addr = ((Y & 0x3f) << 2) | (B & 0x3)
    return (stride<<12|horizontal<<11|laned<<10|size<<8|addr)

def vpm_write_uv_config(thread):
    stride = 24 if thread % 2 == 0 else 1 # Y += 6
    size = 0 # 8-bit
    laned = 0 # packed
    horizontal = 1 # horizontal
    Y = 24 + thread // 2 if thread % 2 == 0 else 36
    B = 0
    addr = ((Y & 0x3f) << 2) | (B & 0x3)
    return (stride<<12|horizontal<<11|laned<<10|size<<8|addr)

@qpu
def remap(asm, n_threads):
    init(asm, n_threads)
    main_loop(asm, n_threads)
    finalize(asm, n_threads)

@qpu
def init(asm, n_threads):
    # disable tmu swap because we use tmu0: remap, tmu1: rgba
    mov(tmu_noswap, 1)

    # texture config uniforms base address
    iadd(ra2, uniform, 4)
    
    # texture config 0-3 (discard here)
    mov(null, uniform)
    mov(null, uniform)
    mov(null, uniform)
    mov(null, uniform)

    # thread index
    mov(ra1, uniform)

    # add rows to remap address
    ldi(r0, 64)
    imul24(r0, r0, ra1) # 1-row size (64byte) is multiplied by thread index
    iadd(ra3, uniform, r0)

    # make slope to remap address register
    for i in range(16):
        ldi(r0, i*4)
        ldi(null, mask(i), set_flags=True)
        iadd(ra3, ra3, r0, cond='zs')

    # y address
    mov(ra4, uniform)

    # vpm write y(0,0) setup register
    mov(ra16, uniform)

    # vpm write uv(0,0) setup register
    mov(ra17, uniform)

    # x tile count
    mov(rb7, uniform)

    # y tile count
    mov(rb8, uniform)

    # frame width
    mov(r0, uniform)

    # frame height
    mov(r1, uniform)

    # u address
    imul24(r2, r0, r1)
    iadd(ra5, ra4, r2)

    # v address
    shr(r2, r2, 2)
    iadd(ra6, ra5, r2)

    # y address increment (row)
    imul24(rb12, r0, (n_threads - 1))

    # u,v address increment (row)
    shr(r2, r0, 1)
    imul24(rb13, r2, (n_threads//2 - 1))

    # dma store stride for y
    ldi(r2, 64)
    isub(r2, r0, r2)
    mov(rb14, r2)

    # dma store stride for u,v
    shr(rb15, r2, 1)

    ldi(ra18, 0x37800080) # 1/65535

    # init remap address increment
    ldi(rb9, 64 * n_threads)
    # init y address increment (column)
    ldi(rb10, 64)
    # init uv address increment (column)
    ldi(rb11, 32)

    # if main thread
    mov(null, ra1, set_flags=True)  # thread index
    jzc(L.init_end)
    nop(); nop(); nop()

    # setup dma store stride for v(1:0-3) (dummy)
    setup_dma_store_stride(rb15)
    # setup dma store v(1,0:3) (dummy)
    setup_dma_store(mode='32bit horizontal', nrows=n_threads//2, ncols=8, X=8, Y=30)
    # start store v(1,0:3) (dummy)
    start_dma_store(ra6)

    # endif
    L.init_end

    # write remap addr to tmu0_s
    mov(tmu0_s, ra3)
    # increment remap addr
    iadd(ra3, ra3, rb9)
    # receive coord from tmu0
    nop(sig='load tmu0')
    # move coord to A-reg to unpack
    mov(ra9, r4)   

    # set uniform_address to texture config base address
    mov(uniforms_address, ra2) # uniforms can be read after 2 instructions

    itof(r3, ra9.unpack('16b')) # t coord
    itof(r2, ra9.unpack('16a')) # s coord

    fmul(r3, r3, ra18) # t/=65535
    fadd(tmu1_t, r3, 0.5).fmul(r2, r2, ra18) # t+=0.5, s/=65535
    fadd(tmu1_s, r2, 0.5) # s+=0.5

    half_tile(asm, n_threads, store_index=1, increment_address=False)

@qpu
def main_loop(asm, n_threads):
    # init tile y loop counter
    mov(r0, rb8)
    mov(ra8, r0)

    L.tile_y_loop

    # init tile x loop counter
    mov(r0, rb7)
    mov(ra7, r0)

    L.tile_x_loop

    # tile
    tile(asm, n_threads)

    # decrement tile x loop counter
    isub(r0, ra7, 1)
    jzc(L.tile_x_loop)
    mov(ra7, r0)
    nop()
    nop()

    # --- end of tile x loop ---

    # increment y address (row)
    mov(r0, rb12)
    iadd(ra4, ra4, r0)
    # increment u address (row)
    mov(r0, rb13)
    iadd(ra5, ra5, r0)
    # increment v address (row)
    iadd(ra6, ra6, r0)

    # decrement tile y loop counter
    isub(r0, ra8, 1)
    jzc(L.tile_y_loop)
    mov(ra8, r0)
    nop()
    nop()

    # --- end of tile y loop ---

@qpu
def finalize(asm, n_threads):
    # wait tmu1
    nop(sig='load tmu1')

    # wait dma store
    wait_dma_store()

    sema_up(COMPLETED)

    # if main thread
    mov(null, ra1, set_flags=True)  # thread index
    jzc(L.fin_end)
    nop(); nop(); nop()

    for i in range(n_threads):
        sema_down(COMPLETED)

    interrupt()

    # endif
    L.fin_end

    nop()
    nop()
    nop()

    # Finish the thread
    exit(interrupt=False)

@qpu
def tile(asm, n_threads):
    for store_index in range(2):
        half_tile(asm, n_threads, store_index)

@qpu
def half_tile(asm, n_threads, store_index, increment_address=True):
    si = store_index
    wi = 1 - store_index
    for t in range(4):
        label = 's{}t{}i{}'.format(si,t,increment_address)

        # wait yuvx from tmu1
        nop(sig='load tmu1')
        # move yuvx to A-reg to unpack
        mov(ra15, r4)
        # write remap addr to tmu0_s
        mov(tmu0_s, ra3)

        fmul(r0, ra15.unpack('8a'), 1.0) # Y
        mov(ra10, r0).fmul(r0, ra15.unpack('8b'), 1.0) # U
        mov(r2, r0).fmul(r0, ra15.unpack('8c'), 1.0) # V
        iadd(ra3, ra3, rb9).mov(r3, r0) # increment remap addr

        mov(r0, ra11)
        mov(r1, ra12)
        for f in range(8):
            # set flag f+8*(t%2)
            ldi(null, mask(f+8*(t%2)), set_flags=True)
            rotate(r0, r2, 16-(f+8*(t%2)), cond='zs')
            rotate(r1, r3, 16-(f+8*(t%2)), cond='zs')

        # receive coord from tmu0
        nop(sig='load tmu0').mov(ra11, r0)
        # move coord to A-reg to unpack
        mov(ra9, r4)

        # set uniform_address to texture config base address
        mov(uniforms_address, ra2) # uniforms can be read after 2 instructions

        itof(r3, ra9.unpack('16b')) # t coord
        itof(r2, ra9.unpack('16a')) # s coord

        ldi(r0, wi*n_threads*4 + t)
        iadd(vpmvcd_wr_setup, ra16, r0)

        mov(ra12, r1).fmul(r3, r3, ra18) # t/=65535
        fadd(tmu1_t, r3, 0.5).fmul(r2, r2, ra18) # t+=0.5, s/=65535
        fadd(tmu1_s, r2, 0.5) # s+=0.5

        # pack y to vpm
        fmul(vpm, ra10, 1.0, pack='8a') # pack y

        if t % 2 == 1:
            ldi(r2, wi*2 + t//2)
            iadd(vpmvcd_wr_setup, ra17, r2)
            # pack u to vpm
            fmul(vpm, ra11, 1.0, pack='8a') # pack u
            # pack v to vpm
            fmul(vpm, ra12, 1.0, pack='8a') # pack v

        if t == 0:
            # if main thread
            mov(null, ra1, set_flags=True)  # thread index
            jzc(L[label])
            nop(); nop(); nop()

            # wait store
            wait_dma_store()

            # setup dma store stride for y(si:0-3)
            setup_dma_store_stride(rb14)
            # setup dma store y(si,0:3)
            setup_dma_store(mode='32bit horizontal', nrows=n_threads, ncols=16, Y=si*12)
            # start store y(si,0:3)
            start_dma_store(ra4)

            if increment_address:
                # increment y address
                iadd(ra4, ra4, rb10) # += 64

            # endif
            L[label]
        elif t == 1:
            # if main thread
            mov(null, ra1, set_flags=True)  # thread index
            jzc(L[label])
            nop(); nop(); nop()

            # wait store
            wait_dma_store()

            # setup dma store stride for u(si:0-3)
            setup_dma_store_stride(rb15)
            # setup dma store u(si,0:3)
            setup_dma_store(mode='32bit horizontal', nrows=n_threads//2, ncols=8, X=si*8, Y=24)
            # start store u(si,0:3)
            start_dma_store(ra5)

            if increment_address:
                # increment u address
                iadd(ra5, ra5, rb11) # += 32

            # endif
            L[label]
        elif t == 2:
            # if main thread
            mov(null, ra1, set_flags=True)  # thread index
            jzc(L[label])
            nop(); nop(); nop()

            # wait store
            wait_dma_store()

            # setup dma store stride for v(si:0-3)
            setup_dma_store_stride(rb15)
            # setup dma store v(si,0:3)
            setup_dma_store(mode='32bit horizontal', nrows=n_threads//2, ncols=8, X=si*8, Y=30)
            # start store v(si,0:3)
            start_dma_store(ra6)

            if increment_address:
                # increment v address
                iadd(ra6, ra6, rb11) # += 32

            # endif
            L[label]
        elif t == 3:
            sema_up(HALF_TILE_SYNC_PRE)

            # if main thread
            mov(null, ra1, set_flags=True)  # thread index
            jzc(L[label])
            nop(); nop(); nop()

            for i in range(n_threads):
                sema_down(HALF_TILE_SYNC_PRE)
            for i in range(n_threads):
                sema_up(HALF_TILE_SYNC_POST)

            # endif
            L[label]

            sema_down(HALF_TILE_SYNC_POST)

    # end of t loop

if __name__ == '__main__':
    if len(sys.argv) != 2:
        print('invalid argument')
        sys.exit(1)
    else:
        output_filepath = sys.argv[1]
        n_threads = 12

        with Driver() as drv:
            program = drv.program(remap, n_threads)

            with open(output_filepath, "wb") as f:
                f.write(program.code)
