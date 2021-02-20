// Copyright (c) 2017-2021 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "include/fetch.cl"
#include "include/common.cl"
#include "include/data_types.cl"

#define unroll_for __attribute__((opencl_unroll_hint)) for
#define CEIL_DIV(A, B) (((A) + (B) - 1) / (B))
#define INPUT0_GET_TILED_INDEX(ORDER) INPUT0_GET_INDEX(ORDER)
#define OUTPUT_GET_TILED_INDEX(ORDER) OUTPUT_GET_INDEX(ORDER)
KERNEL (permute_tile_8x8_4x4)(
    const __global INPUT0_TYPE* input,
    __global OUTPUT_TYPE* output
#if HAS_FUSED_OPS_DECLS
    , FUSED_OPS_DECLS
#endif
    )
{
    const uint x = get_global_id(0);
    const uint f = (uint)get_global_id(2) % NFEATURE_TILES;
    const uint b = (uint)get_global_id(2) / NFEATURE_TILES;

#if INPUT0_DIMS == 4
    //|dim2:bf|dim1:y|dim0:x
    const uint y = get_global_id(1);
#elif INPUT0_DIMS == 5
    //|dim2:bf|dim1:yz|dim0:x
    const uint z = get_global_id(1) / INPUT0_SIZE_Y;
    const uint y = get_global_id(1) % INPUT0_SIZE_Y;   
#elif INPUT0_DIMS == 6
    //|dim2:bf|dim1:wyz|dim0:x
    const uint y = get_global_id(1) % INPUT0_SIZE_Y;
    const uint z = get_global_id(1) / INPUT0_SIZE_Y % INPUT0_SIZE_Z;
    const uint w = get_global_id(1) / (INPUT0_SIZE_Y * INPUT0_SIZE_Z) % INPUT0_SIZE_W;
#endif
    __local OUTPUTVTYPE transpose_buf[TRANS_BUF_SIZE];

    int local_id = get_local_id(0) * get_local_size(2) * get_local_size(1)
                    + get_local_id(1) * get_local_size(2)
                    + get_local_id(2);

    int local_buf_offset = local_id * LOCAL_BUF_STRIDE;

    if (NORMAL_TILE_CONDITION) {
        for (int lh = 0; lh < TILE_SIZE; ++lh) {
#if VEC_WIDTH_SAME_AS_TILE_SIZE
            // vectorwidth == tilesize
            // read
            unsigned int input_idx = INPUT0_GET_TILED_INDEX(INPUT0_TILED_ORDER);
            INPUTVTYPE read_data = AS_INPUTVTYPE(VLOAD(0, input + input_idx));
            // transpose
            unsigned int dst_w = lh / TILE_SIZE;
            unroll_for (int i = 0; i < TILE_SIZE; ++i) {
                unsigned int dst = local_buf_offset + i;
#if HAS_FUSED_OPS
                INPUT0_TYPE input_var = read_data[i];
                FUSED_OPS;
                transpose_buf[dst][lh] = FUSED_OPS_RESULT;
#else
                transpose_buf[dst][lh] = ACTIVATION(read_data[i], ACTIVATION_PARAMS);
#endif
            }
#else
            // vectorwidth != tilesize
            for (int lw = 0; lw < N_VECTORS_IN_TILE; ++lw) {
                // read
                unsigned int input_idx = INPUT0_GET_TILED_INDEX(INPUT0_TILED_ORDER);
                INPUTVTYPE read_data = AS_INPUTVTYPE(VLOAD(0, input + input_idx + lw));
                // transpose
                unsigned int dst_h = lw * VEC_WIDTH;
                unsigned int dst_w = lh / VEC_WIDTH;
                unsigned int dst_element = lh % VEC_WIDTH;
                unsigned int dst_h_pitch = N_VECTORS_IN_TILE;
                unroll_for (int i = 0; i < VEC_WIDTH; ++i) {
                    unsigned int dst = local_buf_offset + (dst_h + i) * dst_h_pitch + dst_w;
#if HAS_FUSED_OPS
                    INPUT0_TYPE input_var = read_data[i];
                    FUSED_OPS;
                    transpose_buf[dst][dst_element] = FUSED_OPS_RESULT;
#else
                    transpose_buf[dst][dst_element] = ACTIVATION(read_data[i], ACTIVATION_PARAMS);
#endif
                }
            }
#endif
        }
        // write to ddr
        for(int lh = 0; lh < TILE_SIZE; ++lh) {
#if VEC_WIDTH_SAME_AS_TILE_SIZE
            unsigned int output_idx = OUTPUT_GET_TILED_INDEX(OUTPUT_TILED_ORDER);
            VSTORE(transpose_buf[local_buf_offset + lh], 0, output + output_idx);
#else
            for(int lw = 0; lw < N_VECTORS_IN_TILE; ++lw) {
                // b, f, z, x, y
                unsigned int output_idx = OUTPUT_GET_TILED_INDEX(OUTPUT_TILED_ORDER);
                VSTORE(transpose_buf[local_buf_offset + lh * N_VECTORS_IN_TILE + lw], 0, output + output_idx);
            }
#endif
        }
    }
#ifdef F_REMAINDER_ITEM
    else if (F_REMAINDER_CONDITION) {
        for (int lh = 0; lh < F_REMAINDER_SIZE; ++lh) {
#if VEC_WIDTH_SAME_AS_TILE_SIZE
            // read
            unsigned int input_idx = INPUT0_GET_TILED_INDEX(INPUT0_TILED_ORDER);
            INPUTVTYPE read_data = AS_INPUTVTYPE(VLOAD(0, input + input_idx));
            // transpose
            unroll_for (int i = 0; i < TILE_SIZE; ++i) {
#if HAS_FUSED_OPS
                INPUT0_TYPE input_var = read_data[i];
                FUSED_OPS;
                transpose_buf[local_buf_offset + i][lh] = FUSED_OPS_RESULT;
#else
                transpose_buf[local_buf_offset + i][lh] = ACTIVATION(read_data[i], ACTIVATION_PARAMS);
#endif
            }
#else
            for (int lw = 0; lw < N_VECTORS_IN_TILE; ++lw) {
                // read
                unsigned int input_idx = INPUT0_GET_TILED_INDEX(INPUT0_TILED_ORDER);
                INPUTVTYPE read_data = AS_INPUTVTYPE(VLOAD(0, input + input_idx + lw));
                // transpose
                unsigned int dst_h = lw * VEC_WIDTH;
                unsigned int dst_w = lh / VEC_WIDTH;
                unsigned int dst_element = lh % VEC_WIDTH;
                unsigned int dst_h_pitch = CEIL_DIV(F_REMAINDER_SIZE, VEC_WIDTH);
                unroll_for (int i = 0; i < VEC_WIDTH; ++i) {
                    unsigned int dst = local_buf_offset + (dst_h + i) * dst_h_pitch + dst_w;
#if HAS_FUSED_OPS
                    INPUT0_TYPE input_var = read_data[i];
                    FUSED_OPS;
                    transpose_buf[dst][dst_element] = FUSED_OPS_RESULT;
#else
                    transpose_buf[dst][dst_element] = ACTIVATION(read_data[i], ACTIVATION_PARAMS);
#endif
                }
            }
#endif
        }
        // write to ddr
        for (int lh = 0; lh < TILE_SIZE; ++lh) {
#if VEC_WIDTH_SAME_AS_TILE_SIZE
            int lw = 0;
            unsigned int output_idx = OUTPUT_GET_TILED_INDEX(OUTPUT_TILED_ORDER);
            for ( int i = 0; i < F_REMAINDER_SIZE; ++i) {
                output[output_idx + i] = transpose_buf[local_buf_offset + lh][i];
            }
#else
            for (int lw = 0; lw < CEIL_DIV(F_REMAINDER_SIZE, VEC_WIDTH); ++lw) {
                unsigned int output_idx = OUTPUT_GET_TILED_INDEX(OUTPUT_TILED_ORDER);
                if (lw == (CEIL_DIV(F_REMAINDER_SIZE, VEC_WIDTH) - 1)) {
                    for ( int i = 0; i < F_REMAINDER_SIZE % VEC_WIDTH; ++i) {
                        output[output_idx + i] = transpose_buf[local_buf_offset + lh * CEIL_DIV(F_REMAINDER_SIZE, VEC_WIDTH) + lw][i];
                    }
                } else {
                    // still vector
                    VSTORE(transpose_buf[local_buf_offset + lh * CEIL_DIV(F_REMAINDER_SIZE, VEC_WIDTH) + lw], 0, output + output_idx);
                }
            }
#endif
        }
    }
#endif
#ifdef X_REMAINDER_ITEM
    else if (X_REMAINDER_CONDITION) {
        // read
        for (int lh = 0; lh < TILE_SIZE; ++lh) {
#if VEC_WIDTH_SAME_AS_TILE_SIZE
            // read
            unsigned int input_idx = INPUT0_GET_TILED_INDEX(INPUT0_TILED_ORDER);
            INPUTVTYPE read_data = AS_INPUTVTYPE(VLOAD(0, input + input_idx));
            // transpose
            unroll_for (int i = 0; i < X_REMAINDER_SIZE; ++i) {
#if HAS_FUSED_OPS
                INPUT0_TYPE input_var = read_data[i];
                FUSED_OPS;
                transpose_buf[local_buf_offset + i][lh] = FUSED_OPS_RESULT;
#else
                transpose_buf[local_buf_offset + i][lh] = ACTIVATION(read_data[i], ACTIVATION_PARAMS);
#endif
            }
#else
            for (int lw = 0; lw < X_REMAINDER_SIZE_AS_VECTOR; ++lw) {
                // read
                unsigned int input_idx = INPUT0_GET_TILED_INDEX(INPUT0_TILED_ORDER);
                INPUTVTYPE read_data = AS_INPUTVTYPE(VLOAD(0, input + input_idx + lw));
                // transpose
                unsigned int dst_h = lw * VEC_WIDTH;
                unsigned int dst_w = lh / VEC_WIDTH;
                unsigned int dst_element = lh % VEC_WIDTH;
                unsigned int dst_h_pitch = N_VECTORS_IN_TILE;
                int read_fragment_width = (lw == (X_REMAINDER_SIZE_AS_VECTOR - 1)) ? X_REMAINDER_SIZE % VEC_WIDTH : VEC_WIDTH;
                unroll_for (int i = 0; i < read_fragment_width; ++i) {
                    unsigned int dst = local_buf_offset + (dst_h + i) * dst_h_pitch + dst_w;
#if HAS_FUSED_OPS
                    INPUT0_TYPE input_var = read_data[i];
                    FUSED_OPS;
                    transpose_buf[dst][dst_element] = FUSED_OPS_RESULT;
#else
                    transpose_buf[dst][dst_element] = ACTIVATION(read_data[i], ACTIVATION_PARAMS);
#endif
                }
            }
#endif
        }
        // write to ddr
        for (int lh = 0; lh < X_REMAINDER_SIZE; ++lh) {
#if VEC_WIDTH_SAME_AS_TILE_SIZE
            unsigned int output_idx = OUTPUT_GET_TILED_INDEX(OUTPUT_TILED_ORDER);
            VSTORE(transpose_buf[local_buf_offset + lh], 0, output + output_idx);
#else
            unroll_for (int lw = 0; lw < N_VECTORS_IN_TILE; ++lw) {
                unsigned int output_idx = OUTPUT_GET_TILED_INDEX(OUTPUT_TILED_ORDER);
                VSTORE(transpose_buf[local_buf_offset + lh * N_VECTORS_IN_TILE + lw], 0, output + output_idx);
            }
#endif
        }
    }
#endif
#if defined(X_REMAINDER_ITEM) && defined(F_REMAINDER_ITEM)
     else if (f == F_REMAINDER_ITEM && x == X_REMAINDER_ITEM) { 
        // point by point
        for (int lh = 0; lh < F_REMAINDER_SIZE; ++lh) {
#if VEC_WIDTH_SAME_AS_TILE_SIZE
            // read
            unsigned int input_idx = INPUT0_GET_TILED_INDEX(INPUT0_TILED_ORDER);
            INPUTVTYPE read_data = AS_INPUTVTYPE(VLOAD(0, input + input_idx));
            // transpose
            for (int i = 0; i < X_REMAINDER_SIZE; ++i) {
                unsigned int dst = local_buf_offset + i;
#if HAS_FUSED_OPS
                INPUT0_TYPE input_var = read_data[i];
                FUSED_OPS;
                transpose_buf[local_buf_offset + i][lh] = FUSED_OPS_RESULT;
#else
                transpose_buf[local_buf_offset + i][lh] = ACTIVATION(read_data[i], ACTIVATION_PARAMS);
#endif
            }
#else
            for(int lw = 0; lw < X_REMAINDER_SIZE_AS_VECTOR; ++lw) {
                // read
                unsigned int input_idx = INPUT0_GET_TILED_INDEX(INPUT0_TILED_ORDER);
                INPUTVTYPE read_data = AS_INPUTVTYPE(VLOAD(0, input + input_idx + lw));
                // transpose
                unsigned int dst_h = lw * VEC_WIDTH;
                unsigned int dst_w = lh / VEC_WIDTH;
                unsigned int dst_element = lh % VEC_WIDTH;
                unsigned int dst_h_pitch = CEIL_DIV(F_REMAINDER_SIZE, VEC_WIDTH);
                int read_fragment_width = (lw == (X_REMAINDER_SIZE_AS_VECTOR - 1)) ? X_REMAINDER_SIZE % VEC_WIDTH : VEC_WIDTH;
                for (int i = 0; i < read_fragment_width; ++i) {
                    unsigned int dst = local_buf_offset + (dst_h + i) * dst_h_pitch + dst_w;
#if HAS_FUSED_OPS
                    INPUT0_TYPE input_var = read_data[i];
                    FUSED_OPS;
                    transpose_buf[dst][dst_element] = FUSED_OPS_RESULT;
#else
                    transpose_buf[dst][dst_element] = ACTIVATION(read_data[i], ACTIVATION_PARAMS);
#endif
                }
            }
#endif
        }
        // write to ddr
        for(int lh = 0; lh < X_REMAINDER_SIZE; ++lh) {
#if VEC_WIDTH_SAME_AS_TILE_SIZE
            unsigned int output_idx = OUTPUT_GET_TILED_INDEX(OUTPUT_TILED_ORDER);
            for ( int i = 0; i < F_REMAINDER_SIZE; ++i) {
                output[output_idx + i] = transpose_buf[local_buf_offset + lh][i];
            }
#else
            for(int lw = 0; lw < F_REMAINDER_SIZE_AS_VECTOR; ++lw) {
                unsigned int output_idx = OUTPUT_GET_TILED_INDEX(OUTPUT_TILED_ORDER);
                int read_fragment_width = (lw == (F_REMAINDER_SIZE_AS_VECTOR - 1)) ? (F_REMAINDER_SIZE % VEC_WIDTH) : VEC_WIDTH;
                for ( int i = 0; i < read_fragment_width; ++i) {
                    output[output_idx + i] = transpose_buf[local_buf_offset + lh * F_REMAINDER_SIZE_AS_VECTOR + lw][i];
                }
            }
#endif
        }
    }
#endif
}
