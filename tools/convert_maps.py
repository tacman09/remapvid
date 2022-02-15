import sys
import numpy as np
from struct import unpack
import argparse

num_elements = 16
num_threads = 12

def next_pow2(x):
    return 1<<(x-1).bit_length()

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument("-mw", "--map-width", type=int, help="width of map", required=True)
    parser.add_argument("-mh", "--map-height", type=int, help="height of map", required=True)
    parser.add_argument("-iw", "--image-width", type=int, help="width of image to be remapped")
    parser.add_argument("-ih", "--image-height", type=int, help="height of image to be remapped")
    parser.add_argument("-x", "--map-x", type=str, help="input filename of x-coord map", required=True)
    parser.add_argument("-y", "--map-y", type=str, help="input filename of y-coord map", required=True)
    parser.add_argument("-o", "--output", type=str, help="output filename", required=True)
    args = parser.parse_args()

    map_width = args.map_width
    map_height = args.map_height
    image_width = args.image_width if args.image_width is not None else map_width
    image_height = args.image_height if args.image_height is not None else map_height
    src_x = (np.fromfile(args.map_x, dtype='float32') / (next_pow2(image_width) - 1) - 0.5) * 65535
    src_x = src_x.astype('int16').reshape((map_height, map_width))
    src_y = (np.fromfile(args.map_y, dtype='float32') / (image_height - 1) - 0.5) * 65535
    src_y = src_y.astype('int16').reshape((map_height, map_width))
    dst = np.empty(map_height * map_width, dtype='uint32')
    nx = map_width // num_elements
    ny = map_height // num_threads

    i = 0
    for ty in range(ny):
        sys.stdout.write(f'\r{ty+1}/{ny}')
        sys.stdout.flush()
        for tx in range(nx):
            x = tx * 16
            for th in range(num_threads):
                y = ty * num_threads + th
                su16 = np.array(unpack('16H', src_x[y, x:x+num_elements]), dtype='uint32')
                sv16 = np.array(unpack('16H', src_y[y, x:x+num_elements]), dtype='uint32')
                dst[i:i+num_elements] = (sv16<<16) | su16
                i += num_elements
    print("")
    header = np.array([map_width, map_height, image_width, image_height], dtype=np.int32)
    with open(args.output, 'wb') as f:
        header.tofile(f)
        dst.tofile(f)
