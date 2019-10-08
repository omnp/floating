#pragma once
#include <immintrin.h>

typedef struct image_t image_t;
typedef union color color;
typedef __m128 colorvector;

image_t *
image_new (unsigned int width, unsigned int height);

void
image_del (image_t *image);

void color_add (color *x, color *y, color *z);
void color_add_struct (colorvector x, colorvector y, color *z);
void color_blend (float const *t, color const *x, color const *y, color *z);
void color_blend_single (float t, color const *x, color const *y, color *z);
void color_blend_single_struct (float t, colorvector x, colorvector y, color *z);
void color_blend_struct (colorvector t, colorvector x, colorvector y, color *z);
void color_multiply_single_struct (float t, colorvector x, color *z);
void color_multiply_struct (colorvector t, colorvector x, color *z);

union __attribute__ ((aligned (16))) color
{
    colorvector vector;
    float values[4] __attribute__ ((aligned (16)));
    struct __attribute__ ((aligned (16)))
    {
        float red, green, blue, alpha;
    };
};

struct image_t
{
    color *data;
    struct
    {
        unsigned int width;
        unsigned int height;
    };
};
