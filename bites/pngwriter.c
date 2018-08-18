#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define PNG_DEBUG 3
#include <png.h>

#include "pngwriter.h"

static int width, height;
static png_byte color_type = PNG_COLOR_TYPE_RGB;
static png_byte bit_depth = 8;

static png_structp png_ptr;
static png_infop info_ptr;
static png_bytep * row_pointers;

void abort_(const char * s, ...)
{
        va_list args;
        va_start(args, s);
        vfprintf(stderr, s, args);
        fprintf(stderr, "\n");
        va_end(args);
        abort();
}

void write_png_file(char* file_name)
{
        /* create file */
        FILE *fp = fopen(file_name, "wb");
        if (!fp)
                abort_("[write_png_file] File %s could not be opened for writing", file_name);


        /* initialize stuff */
        png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

        if (!png_ptr)
                abort_("[write_png_file] png_create_write_struct failed");

        info_ptr = png_create_info_struct(png_ptr);
        if (!info_ptr)
                abort_("[write_png_file] png_create_info_struct failed");

        if (setjmp(png_jmpbuf(png_ptr)))
                abort_("[write_png_file] Error during init_io");

        png_init_io(png_ptr, fp);


        /* write header */
        if (setjmp(png_jmpbuf(png_ptr)))
                abort_("[write_png_file] Error during writing header");

        png_set_IHDR(png_ptr, info_ptr, width, height,
                     bit_depth, color_type, PNG_INTERLACE_NONE,
                     PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

        png_write_info(png_ptr, info_ptr);


        /* write bytes */
        if (setjmp(png_jmpbuf(png_ptr)))
                abort_("[write_png_file] Error during writing bytes");

        png_write_image(png_ptr, row_pointers);


        /* end write */
        if (setjmp(png_jmpbuf(png_ptr)))
                abort_("[write_png_file] Error during end of write");

        png_write_end(png_ptr, NULL);

        /* cleanup heap allocation */
        for (int y=0; y<height; y++)
                free(row_pointers[y]);
        free(row_pointers);

        fclose(fp);
}


void write_greyscale_png_image(const short* pixels, char *filename, int w, int h, double color_divider)
{
    write_greyscale_png_image_with_min(pixels, filename, w, h, color_divider, 0);
}

void write_greyscale_png_image_with_min(const short* pixels, char *filename, int w, int h, 
                                        double color_divider, uint8_t color_offset)
{
    width = w;
    height = h; 
    
    row_pointers = (png_bytep*) malloc(sizeof(png_bytep) * height);
    for (int y=0; y<height; y++)
          row_pointers[y] = (png_byte*) malloc(width * 3);

    for (int row = 0; row < h; row++)
      for (int col = 0; col < w; col++)
        for (int ch = 0; ch < 3; ch++)
          row_pointers[row][col * 3 + ch] = color_offset + (png_byte)(pixels[row * w + col] / color_divider);

    write_png_file(filename);
}

void write_rgb_png_image(const short* pixels, char *filename, int w, int h)
{
    width = w;
    height = h; 
    
    row_pointers = (png_bytep*) malloc(sizeof(png_bytep) * height);
    for (int y=0; y<height; y++)
          row_pointers[y] = (png_byte*) malloc(width * 3);

    for (int row = 0; row < h; row++)
      for (int col = 0; col < w; col++)
        for (int ch = 0; ch < 3; ch++)
          row_pointers[row][col * 3 + ch] = (png_byte)(pixels[row * w * 3 + col * 3 + ch]);

    write_png_file(filename);
}
