#include "image.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>


void color_blend_absorb (const float *t, const color *x, const color *y, color *z)
{
    const int components = 4;
    int c;
    /* It is assumed that the input is 0.0f to 1.0f.
       So that the maximum sum is then 2.0f.
     */
    float const x_max = 2.0f;
    float const y_max = 2.0f;
    float const z_max = 2.0f;
    float x_gray = fminf(fminf(x->values[0], x->values[1]), x->values[2]);
    float y_gray = fminf(fminf(x->values[0], x->values[1]), x->values[2]);
#pragma omp simd
    for (c = 0; c < components; c++)
      {
        float value = x_max - x->values[c] + x_gray;
        z->values[c] = value + (y_max - y->values[c] + y_gray - value) * t[c];
      }
#pragma omp simd
    for (c = 0; c < components; c++)
      {
        z->values[c] = z_max - z->values[c] + (y_gray - x_gray) * t[c] + x_gray;
      }
}


void color_blend_absorb_single (const float t, const color *x, const color *y, color *z)
{
    const int components = 4;
    int c;
    /* It is assumed that the input is 0.0f to 1.0f.
       So that the maximum sum is then 2.0f.
     */
    float const x_max = 2.0f;
    float const y_max = 2.0f;
    float const z_max = 2.0f;
    float x_gray = fminf(fminf(x->values[0], x->values[1]), x->values[2]);
    float y_gray = fminf(fminf(x->values[0], x->values[1]), x->values[2]);
#pragma omp simd
    for (c = 0; c < components; c++)
      {
        float value = x_max - x->values[c] + x_gray;
        z->values[c] = value + (y_max - y->values[c] + y_gray - value) * t;
      }
#pragma omp simd
    for (c = 0; c < components; c++)
      {
        z->values[c] = z_max - z->values[c] + (y_gray - x_gray) * t + x_gray;
      }
}


image_t *
image_new (unsigned int width, unsigned int height)
{
    image_t *image = malloc (sizeof (image_t));
    image->width = width;
    image->height = height;
    image->data = aligned_alloc (32, sizeof (color) * image->width * image->height);
    unsigned char *image_data_chars = (unsigned char *) image->data;
    memset (image_data_chars, 0, sizeof (color) * image->width * image->height);
    return image;
}

void
image_del (image_t *image) {
    free (image->data);
    free (image);
}

inline void color_add (color *x, color *y, color *z)
{
    asm volatile
        ("vmovaps (%[rdi]), %%xmm0;\
          vmovaps (%[rsi]), %%xmm1;\
          vaddps %%xmm0, %%xmm1, %%xmm0;\
          vmovaps %%xmm0, (%[rdx]);\
         " : : [rdi] "r" (x), [rsi] "r" (y), [rdx] "r" (z) : "xmm0", "xmm1", "memory");
}

inline void color_add_struct (colorvector x, colorvector y, color *z)
{
    asm volatile
       ("vaddps %[xmm0], %[xmm1], %[xmm0];\
          vmovaps %[xmm0], (%[r]);\
         " : [xmm0] "+v" (x), [xmm1] "+v" (y) : [r] "r" (z) : "memory");
}

inline void color_blend (float const *t, color const *x, color const *y, color *z)
{
    asm volatile
        ("vmovaps (%[rdi]), %%xmm0;\
          vmovaps (%[rsi]), %%xmm1;\
          vmovaps ones, %%xmm2;\
          vsubps %%xmm0, %%xmm2, %%xmm2;\
          vmulps %%xmm2, %%xmm1, %%xmm3;\
          vmovaps (%[rdx]), %%xmm1;\
          vmulps %%xmm0, %%xmm1, %%xmm1;\
          vaddps %%xmm1, %%xmm3, %%xmm3;\
          vmovaps %%xmm3, (%[rcx]);\
         " : : [rdi] "r" (t), [rsi] "r" (x), [rdx] "r" (y), [rcx] "r" (z) : "xmm0", "xmm1", "xmm2", "xmm3", "memory");
}

inline void color_blend_single (float t, color const *x, color const *y, color *z)
{
    asm volatile
        ("vbroadcastss %[xmm0], %[xmm0];\
          vmovaps (%[rdi]), %%xmm1;\
          vmovaps ones, %%xmm2;\
          vsubps %[xmm0], %%xmm2, %%xmm2;\
          vmulps %%xmm2, %%xmm1, %%xmm3;\
          vmovaps (%[rsi]), %%xmm1;\
          vmulps %[xmm0], %%xmm1, %%xmm1;\
          vaddps %%xmm1, %%xmm3, %%xmm3;\
          vmovaps %%xmm3, (%[rdx]);\
         " : [xmm0] "+v" (t) : [rdi] "r" (x), [rsi] "r" (y), [rdx] "r" (z) : "xmm1", "xmm2", "xmm3", "memory");
}

inline void color_blend_single_struct (float t, colorvector x, colorvector y, color *z)
{
    asm volatile
        ("vbroadcastss %[xmm0], %[xmm0];\
          vmovaps ones, %%xmm3;\
          vsubps %[xmm0], %%xmm3, %%xmm3;\
          vmulps %%xmm3, %[xmm1], %[xmm1];\
          vmulps %[xmm2], %[xmm0], %[xmm2];\
          vaddps %[xmm2], %[xmm1], %[xmm1];\
          vmovaps %[xmm1], (%[rdi]);\
         " : [xmm0] "+v" (t), [xmm1] "+v" (x), [xmm2] "+v" (y) : [rdi] "r" (z) : "xmm3", "memory");
}

inline void color_blend_struct (colorvector t, colorvector x, colorvector y, color *z)
{
    asm volatile
        ("vmovaps ones, %%xmm3;\
          vsubps %[xmm0], %%xmm3, %%xmm3;\
          vmulps %%xmm3, %[xmm1], %[xmm1];\
          vmulps %[xmm2], %[xmm0], %[xmm2];\
          vaddps %[xmm2], %[xmm1], %[xmm1];\
          vmovaps %[xmm1], (%[rdi]);\
         " : [xmm0] "+v" (t), [xmm1] "+v" (x), [xmm2] "+v" (y) : [rdi] "r" (z) : "xmm3", "memory");
}

inline void color_multiply_single_struct (float t, colorvector x, color *z)
{
    asm volatile
        ("vbroadcastss %[xmm0], %[xmm0];\
          vmulps %[xmm0], %[xmm1], %[xmm1];\
          vmovaps %[xmm1], (%[rdi]);\
         " : [xmm0] "+v" (t), [xmm1] "+v" (x) : [rdi] "r" (z) : "memory");
}

inline void color_multiply_struct (colorvector t, colorvector x, color *z)
{
    asm volatile
        ("vmulps %[xmm0], %[xmm1], %[xmm1];\
          vmovaps %[xmm1], (%[rdi]);\
         " : [xmm0] "+v" (t), [xmm1] "+v" (x) : [rdi] "r" (z) : "memory");
}

asm (".section .rodata;\
      .align 16;\
       ones: .rept 4;\
      .float 1.0;\
      .endr;\
      .text;");
