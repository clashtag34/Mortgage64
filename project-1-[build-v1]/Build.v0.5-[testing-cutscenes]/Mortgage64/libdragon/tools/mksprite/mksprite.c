#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include "../common/binout.c"
#include "../common/binout.h"
#include "../common/polyfill.h"
#include "exoquant.h"

#define LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS    // No need to parse PNG extra fields
#define LODEPNG_NO_COMPILE_CPP                 // No need to use C++ API
#include "../common/lodepng.h"
#include "../common/lodepng.c"

// Quantization library
#include "exoquant.h"
#include "exoquant.c"

// Compression library
#include "../common/assetcomp.h"

// Bring in tex_format_t definition
#include "surface.h"
#include "sprite.h"

#define FMT_ZBUF   (64 + 0)
#define FMT_IHQ    (64 + 1)
#define FMT_SHQ    (64 + 2)

#define SWAP(a, b) ({ typeof(a) t = a; a = b; b = t; })
#define ROUND_UP(n, d) ({ \
	typeof(n) _n = n; typeof(d) _d = d; \
	(((_n) + (_d) - 1) / (_d) * (_d)); \
})
#define MIN(a, b) ({ typeof(a) _a = a; typeof(b) _b = b; _a < _b ? _a : _b; })
#define MAX(a, b) ({ typeof(a) _a = a; typeof(b) _b = b; _a > _b ? _a : _b; })

const char* tex_format_name(tex_format_t fmt) {
    switch ((int)fmt) {
    case FMT_NONE: return "AUTO";
    case FMT_RGBA32: return "RGBA32";
    case FMT_RGBA16: return "RGBA16";
    case FMT_CI8: return "CI8";
    case FMT_CI4: return "CI4";
    case FMT_I8: return "I8";
    case FMT_I4: return "I4";
    case FMT_IA16: return "IA16";
    case FMT_IA8: return "IA8";
    case FMT_IA4: return "IA4";
    case FMT_ZBUF: return "ZBUF";
    case FMT_IHQ: return "IHQ";
    case FMT_SHQ: return "SHQ";
    default: assert(0); return ""; // should not happen
    }
}

tex_format_t tex_format_from_name(const char *name) {
    if (!strcasecmp(name, "RGBA32")) return FMT_RGBA32;
    if (!strcasecmp(name, "RGBA16")) return FMT_RGBA16;
    if (!strcasecmp(name, "IA16"))   return FMT_IA16;
    if (!strcasecmp(name, "CI8"))    return FMT_CI8;
    if (!strcasecmp(name, "I8"))     return FMT_I8;
    if (!strcasecmp(name, "IA8"))    return FMT_IA8;
    if (!strcasecmp(name, "CI4"))    return FMT_CI4;
    if (!strcasecmp(name, "I4"))     return FMT_I4;
    if (!strcasecmp(name, "IA4"))    return FMT_IA4;
    if (!strcasecmp(name, "ZBUF"))   return FMT_ZBUF;
    if (!strcasecmp(name, "IHQ"))    return FMT_IHQ;
    if (!strcasecmp(name, "SHQ"))    return FMT_SHQ;
    return FMT_NONE;
}

#define MIPMAP_ALGO_NONE  0
#define MIPMAP_ALGO_BOX   1

const char *mipmap_algo_name(int algo) {
    switch (algo) {
    case MIPMAP_ALGO_NONE: return "NONE";
    case MIPMAP_ALGO_BOX: return "BOX";
    default: assert(0); return "";
    }
}

#define DITHER_ALGO_NONE     0
#define DITHER_ALGO_RANDOM   1
#define DITHER_ALGO_ORDERED  2

const char *dither_algo_name(int algo) {
    switch (algo) {
    case DITHER_ALGO_NONE: return "NONE";
    case DITHER_ALGO_RANDOM: return "RANDOM";
    case DITHER_ALGO_ORDERED: return "ORDERED";
    default: assert(0); return "";
    }
}

typedef struct {
    struct {
        float translate;
        int scale;
        float repeats;
        int mirror;
    } s, t;
    bool defined;
} texparms_t;


typedef struct {
    tex_format_t outfmt;
    int hslices;
    int vslices;
    int tilew;
    int tileh;
    int mipmap_algo;
    int dither_algo;
    int gamma_correct;
    texparms_t texparms;
    struct{
        const char   *infn;       // Input file for detail texture
        texparms_t   texparms;
        tex_format_t outfmt;
        float        blend_factor;
        bool         use_main_tex;
        bool         enabled;
    } detail;

} parms_t;


bool flag_verbose = false;
bool flag_debug = false;

void print_supported_formats(void) {
    fprintf(stderr, "Supported formats: AUTO, RGBA32, RGBA16, IA16, CI8, I8, IA8, CI4, I4, IA4, ZBUF, IHQ\n");
}

void print_supported_mipmap(void) {
    fprintf(stderr, "Supported mipmap algorithms: NONE (disable), BOX\n");
}

void print_supported_dithers(void) {
    fprintf(stderr, "Supported dithering algorithms: NONE (disable), RANDOM, ORDERED. \nNote that dithering is only applied while quantizing an image.\n");
}

void print_args( char * name )
{
    fprintf(stderr, "Usage: %s [flags] <input files...>\n", name);
    fprintf(stderr, "\n");
    fprintf(stderr, "Command-line flags:\n");
    fprintf(stderr, "   -v/--verbose          Verbose output\n");
    fprintf(stderr, "   -o/--output <dir>     Specify output directory (default: .)\n");
    fprintf(stderr, "   -f/--format <fmt>     Specify output format (default: AUTO)\n");
    fprintf(stderr, "   -D/--dither <dither>  Dithering algorithm (default: NONE)\n");
    fprintf(stderr, "   -c/--compress <level> Compress output files (default: %d)\n", DEFAULT_COMPRESSION);
    fprintf(stderr, "   -g/--gamma            Adjust colors for when VI gamma correction is enabled on console (convert to linear colors)\n");
    fprintf(stderr, "   -d/--debug            Dump computed images (eg: mipmaps) as PNG files in output directory\n");
    fprintf(stderr, "\nSampling flags:\n");
    fprintf(stderr, "   --texparms <x,s,r,m>          Sampling parameters:\n");
    fprintf(stderr, "                                 x=translation, s=scale, r=repetitions, m=mirror\n");
    fprintf(stderr, "   --texparms <x,x,s,s,r,r,m,m>  Sampling parameters (different for S/T)\n");
    fprintf(stderr, "\nMipmapping flags:\n");
    fprintf(stderr, "   -m/--mipmap <algo>                    Calculate mipmap levels using the specified algorithm (default: NONE)\n");
    fprintf(stderr, "   --detail [<image>[,<fmt>]][,<factor>] Activate detail texture:\n");
    fprintf(stderr, "                                         <image> is the file to use as detail (default: reuse input image)\n");
    fprintf(stderr, "                                         <fmt> is the output format (default: AUTO)\n");
    fprintf(stderr, "                                         <factor> is the blend factor in range 0..1 (default: 0.5)\n");
    fprintf(stderr, "   --detail-texparms <x,x,s,s,r,r,m,m>   Sampling parameters for the detail texture\n");
    fprintf(stderr, "\n");
    print_supported_formats();
    print_supported_mipmap();
    print_supported_dithers();
}

uint16_t conv_rgb5551(uint8_t r8, uint8_t g8, uint8_t b8, uint8_t a8) {
    uint16_t r=r8>>3, g=g8>>3, b=b8>>3, a=a8 >= 128?1:0;
    return (r<<11) | (g<<6) | (b<<1) | a;
}

/*dither matrix*/
unsigned int dith[4][4] = {{0, 6, 1, 7}, {4, 2, 5, 3}, {3, 5, 2, 4}, {7, 1, 6, 0}};

int check_color(int val){
    if(val > 255) val = 255;
    if(val < 0) val = 0;
    return val;
}

uint16_t conv_rgb5551_dither(uint8_t r8, uint8_t g8, uint8_t b8, uint8_t a8, unsigned int x, unsigned int y, int dither ) {
    int value = 0;
    switch(dither){
        case DITHER_ALGO_ORDERED: value = dith[x & 0x3][y & 0x3]; break;
        case DITHER_ALGO_RANDOM:  value = rand() & 0x7; break;
        case DITHER_ALGO_NONE: return conv_rgb5551(r8, g8, b8, a8); break;
        default: fprintf(stderr, "ERROR: conv RGBA5551 unimplemented dithering mode %s\n", dither_algo_name(dither)); assert(0);
    }

    int r = r8 + value - 4;
    int g = g8 + value - 4;
    int b = b8 + value - 4;
    r = check_color(r);
    g = check_color(g);
    b = check_color(b);

    return conv_rgb5551(r,g,b,a8);
}

// Convert a 18-bit fixed point 0.15.3 into floating point 14-bit.
uint16_t conv_float14(uint32_t fx) {
    if (!(fx & 0x20000)) return (0<<11) | ((fx >> 6) & 0x7FF);
    if (!(fx & 0x10000)) return (1<<11) | ((fx >> 5) & 0x7FF);
    if (!(fx & 0x08000)) return (2<<11) | ((fx >> 4) & 0x7FF);
    if (!(fx & 0x04000)) return (3<<11) | ((fx >> 3) & 0x7FF);
    if (!(fx & 0x02000)) return (4<<11) | ((fx >> 2) & 0x7FF);
    if (!(fx & 0x01000)) return (5<<11) | ((fx >> 1) & 0x7FF);
    if (!(fx & 0x00800)) return (6<<11) | ((fx >> 0) & 0x7FF);
    if (true)            return (7<<11) | ((fx >> 0) & 0x7FF);
}

int calc_tmem_usage(tex_format_t fmt, int width, int height)
{
    int pitch_align = 8;
    if (fmt == FMT_RGBA32 || fmt == FMT_YUV16) pitch_align = 4;
    int pitch = ROUND_UP(TEX_FORMAT_PIX2BYTES(fmt, width), pitch_align);
    return pitch*height;
}

const char *colortype_to_string(LodePNGColorType ct) {
    switch (ct) {
    case LCT_GREY: return "LCT_GREY";
    case LCT_RGB: return "LCT_RGB";
    case LCT_PALETTE: return "LCT_PALETTE";
    case LCT_GREY_ALPHA: return "LCT_GREY_ALPHA";
    case LCT_RGBA: return "LCT_RGBA";
    default: assert(0); return "";
    }
}

typedef struct {
    uint8_t *image;         // Pointer to image data (pixels)
    int width, height;      // Image dimensions
    tex_format_t fmt;       // Texture format
    LodePNGColorType ct;    // PNG color type
} image_t;

typedef struct {
    int num_colors;         // Number of colors in palette
    int used_colors;        // Number of colors actually used in palette
    uint8_t colors[256][4]; // Color palette (if num_colors != 0)
} palette_t;

#define MAX_IMAGES 8

typedef struct {
    const char *infn;       // Input file name
    FILE *out;              // Output file
    image_t images[MAX_IMAGES]; // Pixel images (one per lod level).
    palette_t palette;      // Palette (if any)
    int vslices;            // Number of vertical slices (deprecated API for old rdp.c)
    int hslices;            // Number of horizontal slices (deprecated API for old rdp.c)
    texparms_t texparms;    // Texture parameters
    int out_flags;          // Flags to store into output
    struct{
        const char   *infn;         // Input file for detail texture
        texparms_t   texparms;      // Texture parameters for the detail
        float        blend_factor;  // Blend factor of the detail vs main lod
        bool         use_main_tex;  // If true, use the main texture as detail (fractal detail)
        bool         enabled;       // If true, detail texture is enabled
    } detail;
    int ditheralgo;
} spritemaker_t;


/**
 * @brief Load a PNG image from a file, performing all the required color conversions
 * 
 * @param infn      Input filename
 * @param fmt       Output format requested by the user (of FMT_NONE for autodetection)
 * @param imgout    Pointer to the image_t structure to fill
 * @return true     If the image was loaded successfully
 * @return false    If there was an error
 */
bool load_png_image(const char *infn, tex_format_t fmt, image_t *imgout, palette_t *palout) {
    LodePNGState state;
    bool autofmt = (fmt == FMT_NONE);
    unsigned char* png = 0;
    size_t pngsize;
    unsigned char* image = 0;
    unsigned width, height;
    bool inspected = false;
    int error;

    if (flag_verbose)
        fprintf(stderr, "loading image: %s\n", infn);

    // Initialize lodepng and load the input file into memory (without decoding).
    lodepng_state_init(&state);

    if (!strstr(infn, "(stdin)")) {
        error = lodepng_load_file(&png, &pngsize, infn);
        if(error) {
            fprintf(stderr, "%s: PNG reading error: %u: %s\n", infn, error, lodepng_error_text(error));
            goto error;
        }
    } else {
        // Read from stdin the whole file
        size_t bufsize = 64*1024;
        png = malloc(bufsize);
        pngsize = 0;
        while (true) {
            size_t n = fread(png+pngsize, 1, bufsize-pngsize, stdin);
            if (n == 0) break;
            pngsize += n;
            if (pngsize == bufsize) {
                bufsize *= 2;
                png = realloc(png, bufsize);
            }
        }
        fclose(stdin);
    }

    // Check if we're asked to autodetect the best possible texformat for output.
    // Try first inspecting the extension
    if (fmt == FMT_NONE) {
        // Check the filename string if it contains a texformat for output
        char *fntok = strdup(infn);
        char *sect = strtok(fntok, ".");
        while (sect) {
            fmt = tex_format_from_name(sect);
            if (fmt != FMT_NONE) break;
            sect = strtok(NULL, ".");
        }
        if (fmt != FMT_NONE) {
            if (flag_verbose)
                fprintf(stderr, "detected format from filename: %s\n", tex_format_name(fmt));
        }
        free(fntok);
    }

    // If we still don't have a format, try to autodetect it from the PNG header
    if (fmt == FMT_NONE) {
        // Parse the PNG header to get some metadata
        error = lodepng_inspect(&width, &height, &state, png, pngsize);
        if(error) {
        	fprintf(stderr, "%s: PNG reading error: %u: %s\n", infn, error, lodepng_error_text(error));
        	goto error;
        }
        inspected = true;
        // Autodetect the best output format depending on the input format
        // The rule of thumb is that we want to preserve the information on the
        // input image as much as possible.
        switch (state.info_png.color.colortype) {
        case LCT_GREY:
            fmt = (state.info_png.color.bitdepth > 4) ? FMT_I8 : FMT_I4;
            break;
        case LCT_GREY_ALPHA:
            if (state.info_png.color.bitdepth < 4) fmt = FMT_IA4;
            else if (state.info_png.color.bitdepth < 8) fmt = FMT_IA8;
            else fmt = FMT_IA16;
            break;
        case LCT_PALETTE:
            fmt = FMT_CI8; // Will check if CI4 (<= 16 colors) later
            break;
        case LCT_RGB: case LCT_RGBA:
            // Usage of 32-bit sprites/textures is extremely rare because of the
            // limited TMEM size. Default to 16-bit here, even though this might
            // cause some banding to appear.
            fmt = FMT_RGBA16;
            break;
        default:
            fprintf(stderr, "%s: unknown PNG color type: %d\n", infn, state.info_png.color.colortype);
            goto error;
        }
    }

    // We should have a format now
    assert(fmt != FMT_NONE);

    // Setup the info_raw structure with the desired pixel conversion,
    // depending on the output format.
    switch ((int)fmt) {
    case FMT_RGBA32: case FMT_RGBA16: case FMT_IHQ: case FMT_SHQ:
        // PNG does not support RGBA555 (aka RGBA16), so just convert
        // to 32-bit version we will downscale later.
        state.info_raw.colortype = LCT_RGBA;
        state.info_raw.bitdepth = 8;
        break;
    case FMT_CI8: case FMT_CI4: {
        // Inspect the PNG if we haven't already
        if (!inspected) {
            error = lodepng_inspect(&width, &height, &state, png, pngsize);
            if(error) {
                fprintf(stderr, "%s: PNG reading error: %u: %s\n", infn, error, lodepng_error_text(error));
                goto error;
            }
            inspected = true;
        }
        if (state.info_png.color.colortype != LCT_PALETTE) {
            // If the original is not a palettized format, we need to run our quantization engine.
            // Expand to RGBA for now.
            state.info_raw.colortype = LCT_RGBA;
            state.info_raw.bitdepth = 8;
        } else {
            // Keep the current palette so that we respect the existing colormap.
            // Notice lodepng does not encode to 4bit palettized, so for now just force 8bit,
            // and will later change it back to CI4 if needed/possible.
            state.info_raw.colortype = LCT_PALETTE;
            state.info_raw.bitdepth = 8;
        }
    }   break;
    case FMT_I8: case FMT_I4:
        // Lodepng LCT_GREY only works for pure greyscale input images, as only
        // the R channel is decoded. Instead, we want to attempt something more
        // sophisticated to allow for a wider range of input images.
        state.info_raw.colortype = LCT_RGBA;
        state.info_raw.bitdepth = 8;
        break;
    case FMT_ZBUF:
        state.info_raw.colortype = LCT_GREY;
        state.info_raw.bitdepth = 16;
        break;
    case FMT_IA16: case FMT_IA8: case FMT_IA4:
        state.info_raw.colortype = LCT_GREY_ALPHA;
        state.info_raw.bitdepth = 8;
        break;
    default:
        assert(0); // should not happen
    }

    // Decode the PNG and do the color conversion as requested.
    // This will error out if the conversion requires downsampling / quantization,
    // as this is not supported by lodepng.
    // TODO: maybe provide quantization algorithms here?
    error = lodepng_decode(&image, &width, &height, &state, png, pngsize);
    if(error) {
        fprintf(stderr, "PNG decoding error: %u: %s\n", error, lodepng_error_text(error));
        goto error;
    }

    if (fmt == FMT_I4 || fmt == FMT_I8) {
        assert(state.info_raw.colortype == LCT_RGBA);

        uint8_t *output = malloc(width*height);

        // Check if the image is natively greyscale, and if so, preserve it
        // as-is, just converting it to 8bpp
        bool input_greyscale = true;
        for (int i=0; i<width*height; i++) {
            uint8_t r = image[i*4+0];
            uint8_t g = image[i*4+1];
            uint8_t b = image[i*4+2];
            if (!(r == g && g == b)) {
                input_greyscale = false;
                break;
            }
            output[i] = r;
        }
        
        // If the input image is not greyscale, perform a conversion
        // and rescale the output to use the full range of the output format.
        if (!input_greyscale) {
            float minf = 1024.0f, maxf = -1024.0f;
            float* imgf = malloc(width*height*sizeof(float));
            for (int i=0; i<width*height; i++) {
                uint8_t r = image[i*4+0];
                uint8_t g = image[i*4+1];
                uint8_t b = image[i*4+2];
                uint8_t a = image[i*4+3];

                // Apply alpha channel if present, so that we preserve
                // antialiasing information.
                imgf[i] = 0.299f*r + 0.587f*g + 0.114f*b;
                imgf[i] *= a / 255.0f;
                minf = MIN(minf, imgf[i]);
                maxf = MAX(maxf, imgf[i]);
            }
            for (int i=0; i<width*height; i++) {
                output[i] = (imgf[i] - minf) / (maxf - minf) * 255.0f;
            }
            free(imgf);
        }

        free(image);
        image = output;
        state.info_raw.colortype = LCT_GREY;
    }

    // Copy the image into the output
    *imgout = (image_t){
        .image = image,
        .width = width,
        .height = height,
        .ct = state.info_raw.colortype,
    };

    if(flag_verbose)
        fprintf(stderr, "loaded %s (%dx%d, %s)\n", infn, width, height, colortype_to_string(state.info_png.color.colortype));

    // For a palettized image, copy the palette and also count the number of actually
    // used colors (aka, the highest index used in the image). This is useful later for
    // some heuristics.
    if (state.info_raw.colortype == LCT_PALETTE) {
        memcpy(palout->colors, state.info_png.color.palette, state.info_png.color.palettesize * 4);
        palout->num_colors = state.info_png.color.palettesize;
        palout->used_colors = 0;
        for (int i=0; i < width*height; i++) {
            if (image[i] >= palout->used_colors)
                palout->used_colors = image[i]+1;
        }
        if (flag_verbose)
            fprintf(stderr, "palette: %d colors (used: %d)\n", palout->num_colors, palout->used_colors);
    }
    if (state.info_raw.colortype == LCT_GREY && state.info_raw.bitdepth <= 8) {
        bool used[256] = {0};
        palout->used_colors = 0;
        for (int i=0; i < width*height; i++) {
            if (!used[image[i]]) {
                used[image[i]] = true;
                palout->used_colors++;
            }
        }
    }

    // In case we're autodetecting the output format and the PNG had a palette, and only
    // indices 0-15 are used, we can use a FMT_CI4.
    if (autofmt && state.info_raw.colortype == LCT_PALETTE && palout->used_colors <= 16)
        fmt = FMT_CI4;

    // In case we're autodetecting the output format and the PNG is a greyscale, and only
    // indices 0-15 are used, we can use a FMT_I4.
    if (autofmt && state.info_raw.colortype == LCT_GREY && palout->used_colors <= 16)
        fmt = FMT_I4;

    // Autodetection complete, log it.
    if (flag_verbose && autofmt)
        fprintf(stderr, "auto selected format: %s\n", tex_format_name(fmt));
    imgout->fmt = fmt;
    
    return true;

error:
    lodepng_state_cleanup(&state);
    if (png) lodepng_free(png);
    return false;
}

bool spritemaker_load_png(spritemaker_t *spr, tex_format_t outfmt)
{
    return load_png_image(spr->infn, outfmt, &spr->images[0], &spr->palette);
}

bool spritemaker_load_detail_png(spritemaker_t *spr, tex_format_t outfmt)
{
    // Load the detail texture into images[7], as last lod.
    palette_t pal;
    bool ok = load_png_image(spr->detail.infn, outfmt, &spr->images[7], &pal);

    // For now, abort if the detail texture is palettized
    if (ok && (spr->images[7].fmt == FMT_CI4 || spr->images[7].fmt == FMT_CI8)) {
        fprintf(stderr, "ERROR: detail textures with palettes are not yet supported.\n");
        return false;
    }
    
    return ok;
}

bool spritemaker_fit_tmem(spritemaker_t *spr, int *out_tmem_usage)
{
    bool has_palette = false;
    int tmem_usage = 0;

    // Calculate TMEM size for the image
    for (int i=0; i<MAX_IMAGES; i++) {
        if (!spr->images[i].image) continue;
        if (spr->images[i].fmt == FMT_CI8) has_palette = true;
        if (spr->images[i].fmt == FMT_CI4) has_palette = true;
        tmem_usage += calc_tmem_usage(spr->images[i].fmt, spr->images[i].width, spr->images[i].height);
    }

    if (has_palette)
        tmem_usage += 2048;

    if (out_tmem_usage)
        *out_tmem_usage = tmem_usage;
    return tmem_usage <= 4096;
}

static uint8_t *image_shrink_box(uint8_t *src, int width, int height, bool half_w, bool half_h) {
    int new_width = half_w ? width/2 : width;
    int new_height = half_h ? height/2 : height;
    uint8_t *imgdst = malloc(new_width * new_height * 4);
    int wstep = half_w ? 8 : 4;
    for (int y=0; y<new_height; y++) {
        uint8_t *src1, *src2, *src3, *src4;
        if (half_h) {
            src1 = src + y*2*width*4;
            src2 = src1 + width*4;
        } else {
            src1 = src2 = src + y*width*4;
        }
        if (half_w) {
            src3 = src1 + 4;
            src4 = src2 + 4;
        } else {
            src3 = src4 = src1;
        }
        uint8_t *dst = imgdst + y*new_width*4;
        for (int x=0; x<new_width; x++) {
            dst[0] = (src1[0] + src3[0] + src2[0] + src4[0]) / 4;
            dst[1] = (src1[1] + src3[1] + src2[1] + src4[1]) / 4;
            dst[2] = (src1[2] + src3[2] + src2[2] + src4[2]) / 4;
            dst[3] = (src1[3] + src3[3] + src2[3] + src4[3]) / 4;
            dst += 4; src1 += wstep; src2 += wstep; src3 += wstep; src4 += wstep;
        }
    }
    return imgdst;
}

bool spritemaker_calc_lods(spritemaker_t *spr, int algo) {
    // Calculate mipmap levels
    assert(algo == MIPMAP_ALGO_BOX);

    int tmem_usage;
    if (!spritemaker_fit_tmem(spr, &tmem_usage)) {
        fprintf(stderr, "WARNING: image does not fit in TMEM (%d), no mipmaps will be calculated\n", tmem_usage);
        // Continue execution anyway
        // TODO: maybe abort?
        return true;
    }

    int maxlevels = MAX_IMAGES;
    if (spr->detail.enabled) maxlevels--;
    bool done = false;
    for (int i=1; i<maxlevels && !done; i++) {
        image_t *prev = &spr->images[i-1];
        int mw = prev->width / 2, mh = prev->height / 2;
        if (mw < 4 || mh < 4) break;
        tmem_usage += calc_tmem_usage(spr->images[0].fmt, mw, mh);
        if (tmem_usage > 4096) {
            if (flag_verbose)
                fprintf(stderr, "mipmap: stopping because TMEM full (%d)\n", tmem_usage);
            break;
        }
        uint8_t *mipmap = NULL;
        switch (prev->ct) {
        case LCT_RGBA:
            mipmap = malloc(mw * mh * 4);
            for (int y=0;y<mh;y++) {
                uint8_t *src1 = prev->image + y*prev->width*4*2;
                uint8_t *src2 = src1 + prev->width*4;
                uint8_t *dst = mipmap + y*mw*4;
                for (int x=0;x<mw;x++) {
                    dst[0] = (src1[0] + src1[4] + src2[0] + src2[4]) / 4;
                    dst[1] = (src1[1] + src1[5] + src2[1] + src2[5]) / 4;
                    dst[2] = (src1[2] + src1[6] + src2[2] + src2[6]) / 4;
                    dst[3] = (src1[3] + src1[7] + src2[3] + src2[7]) / 4;
                    dst += 4; src1 += 8; src2 += 8;
                }
            }
            break;
        case LCT_GREY:
            switch(prev->fmt){
                case FMT_I4:
                case FMT_I8:
                {
                    mipmap = malloc(mw * mh);
                    for (int y=0;y<mh;y++) {
                        uint8_t *src1 = prev->image + y*prev->width*2;
                        uint8_t *src2 = src1 + prev->width;
                        uint8_t *dst = mipmap + y*mw;
                        for (int x=0;x<mw;x++) {
                            dst[0] = (src1[0] + src1[1] + src2[0] + src2[1]) / 4;
                            dst += 1; src1 += 2; src2 += 2;
                        }
                    }
                break; }
                default: // should never happen
                	assert(0);
            }
            break;
        case LCT_GREY_ALPHA:{
            switch (prev->fmt)
            {
            case FMT_IA4:
            case FMT_IA8:
            case FMT_IA16:
                {
                    mipmap = malloc(mw * mh * 2);
                    for (int y=0;y<mh;y++) {
                        uint8_t *src1 = prev->image + y*prev->width*2*2;
                        uint8_t *src2 = src1 + prev->width*2;
                        uint8_t *dst = mipmap + y*mw*2;
                        for (int x=0;x<mw;x++) {
                            dst[0] = (src1[0] + src1[2] + src2[0] + src2[2]) / 4;
                            dst[1] = (src1[1] + src1[3] + src2[1] + src2[3]) / 4;
                            dst += 2; src1 += 4; src2 += 4;
                        }
                    }
                break; }
            
            default: // should never happen
                assert(0);
            }
            break;
        }
        default: // all formats are covered now, but for the sake if there's a new format around in the future
            fprintf(stderr, "ERROR: mipmap calculation for format %s/%s not implemented yet\n", tex_format_name(prev->fmt), colortype_to_string(prev->ct));
            return false;
        }
        if(!done) {
            if (flag_verbose)
                fprintf(stderr, "mipmap: generated %dx%d\n", mw, mh);
            spr->images[i] = (image_t){
                .image = mipmap,
                .width = mw,
                .height = mh,
                .ct = prev->ct,
                .fmt = prev->fmt,
            };
        }
    }

    return true;
}

bool spritemaker_expand_rgba(spritemaker_t *spr) {
    for (int i=0; i<MAX_IMAGES; i++) {
        image_t *img = &spr->images[i];
        if (!img->image || img->ct == LCT_RGBA)
            continue;
        if (flag_verbose)
            fprintf(stderr, "expanding image %d to RGBA\n", i);
        uint8_t *rgba = malloc(img->width * img->height * 4);
        switch (img->ct) {
        case LCT_PALETTE:
            for (int y=0; y<img->height; y++) {
                for (int x=0; x<img->width; x++) {
                    uint8_t *src = img->image + y*img->width + x;
                    uint8_t *dst = rgba + (y*img->width + x) * 4;
                    uint8_t *pal = spr->palette.colors[*src];
                    dst[0] = pal[0];
                    dst[1] = pal[1];
                    dst[2] = pal[2];
                    dst[3] = pal[3];
                }
            }
            break;
        default:
            fprintf(stderr, "ERROR: unsupported color type %d\n", img->ct);
            return false;
        }
        free(img->image);
        img->image = rgba;
        img->ct = LCT_RGBA;
    }
    // Clear the palette data as it's not used anymore
    memset(&spr->palette, 0, sizeof(spr->palette));
    return true;
}

bool spritemaker_quantize(spritemaker_t *spr, uint8_t *colors, int num_colors, int dither) {
    if (flag_verbose)
        fprintf(stderr, "quantizing image(s) to %d colors%s\n", num_colors, colors ? " (using existing palette)" : "");

    // Initialize the quantizer engine
    exq_data *exq = exq_init();
    exq->numBitsPerChannel = 5;   // force calculations using rgb555

    // Feed the input images, so that all of them will be quantized at once
    // using the same palette.
    for (int i=0; i<MAX_IMAGES; i++) {
        if (spr->images[i].image == NULL)
            continue;
        if (spr->images[i].ct != LCT_RGBA) {
            fprintf(stderr, "ERROR: image %d is not RGBA\n", i);
            goto error;
        }
        exq_feed(exq, spr->images[i].image, spr->images[i].width * spr->images[i].height);
    }

    if (!colors) {
        // Run quantization (high quality mode)
        exq_quantize_hq(exq, num_colors);

        // Extract the generate palette
        exq_get_palette(exq, spr->palette.colors[0], num_colors);
        spr->palette.num_colors = num_colors;
        spr->palette.used_colors = num_colors;
    } else {
        // Force the input palette
        exq_set_palette(exq, colors, num_colors);
        memcpy(spr->palette.colors[0], colors, num_colors * 4);
        spr->palette.num_colors = num_colors;
        spr->palette.used_colors = num_colors;
    }

    // Remap the images to the new palette
    for (int i=0; i<MAX_IMAGES; i++) {
        image_t *img = &spr->images[i];
        if (spr->images[i].image == NULL)
            continue;
        uint8_t* ci_image = malloc(img->width * img->height);
        switch (dither) {
        case DITHER_ALGO_NONE:
            exq_map_image(exq, img->width * img->height, img->image, ci_image);
            break;
        case DITHER_ALGO_RANDOM:
            exq_map_image_random(exq, img->width * img->height, img->image, ci_image);
            break;
        case DITHER_ALGO_ORDERED:
            exq_map_image_ordered(exq, img->width, img->height, img->image, ci_image);
            break;
        default:
            fprintf(stderr, "ERROR: invalid dithering mode %d\n", dither);
            goto error;
        }
        free(img->image);
        img->image = ci_image;
        img->ct = LCT_PALETTE;
    }

    exq_free(exq);
    return true;

error:
    exq_free(exq);
    return false;
}

static uint8_t ihq_calc_best_i4(float ifactor, uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r, uint8_t g, uint8_t b, float *err) {
    // Compute Y (luma) for r0,g0,b0
    float y0 = 0.299f*r0    + 0.587f*g0   + 0.114f*b0;
    float u0 = -0.14713f*r0 - 0.28886f*g0 + 0.436f*b0;
    float v0 = 0.615f*r0    - 0.51499f*g0 - 0.10001f*b0;

    float rf = r*(1-ifactor);
    float gf = g*(1-ifactor);
    float bf = b*(1-ifactor);

    // Calculate the best I value that, when added to r,g,b, and converted to Y,
    // will give the closest value to (y0, u0, v0).
    uint8_t best_i = 0;
    float best_err = 999999;
    for (int i=0; i<256; i+=16) {
        float ii = i*ifactor;
        int ri = rf+ii;
        int gi = gf+ii;
        int bi = bf+ii;

        float y = 0.299f*ri     + 0.587f*gi   + 0.114f*bi;
        float u = -0.14713f*ri  - 0.28886f*gi + 0.436f*bi;
        float v = 0.615f*ri     - 0.51499f*gi - 0.10001f*bi;

        float ydiff = y-y0;
        float udiff = u-u0;
        float vdiff = v-v0;

        float err = ydiff*ydiff + udiff*udiff + vdiff*vdiff;

        if (err < best_err) {
            best_err = err;
            best_i = i;
        }
    }

    *err = best_err;
    return best_i;
}

uint8_t gamma_correct_value(uint8_t input) {
    return (uint8_t)(((int)input * (int)input) >> 8);
}

bool spritemaker_gamma_correct(spritemaker_t *spr) {
    for (int m=0; m<MAX_IMAGES; m++) {
        image_t *image = &spr->images[m];
        if (image->image == NULL)
            continue;

        if (image->fmt == FMT_CI4 || image->fmt == FMT_CI8) {
            // gamma correct the pallete
            continue;
        } else if (image->fmt == FMT_RGBA32 || image->fmt == FMT_RGBA16) {
            uint8_t *img = image->image;
            for (int i=0;i<image->width*image->height;i++) {
                img[0] = gamma_correct_value(img[0]);
                img[1] = gamma_correct_value(img[1]);
                img[2] = gamma_correct_value(img[2]);
                img += 4;
            }
        } else if (image->fmt == FMT_I4 || image->fmt == FMT_I8) {
            uint8_t *img = image->image;
            for (int i=0;i<image->width*image->height;i++) {
                *img = gamma_correct_value(*img);
                img++;
            }
        } else if (image->fmt == FMT_IA16 || image->fmt == FMT_IA8 || image->fmt == FMT_IA4) {
            uint8_t *img = image->image;
            for (int i=0;i<image->width*image->height;i++) {
                *img = gamma_correct_value(*img);
                img += 2;
            }
        } else {
            fprintf(stderr, "Gamma correction not implemented for format %s\n", tex_format_name(image->fmt));
            return false;
        }
    }

    for (int i=0; i<spr->palette.num_colors; i++) {
        spr->palette.colors[i][0] = gamma_correct_value(spr->palette.colors[i][0]);
        spr->palette.colors[i][1] = gamma_correct_value(spr->palette.colors[i][1]);
        spr->palette.colors[i][2] = gamma_correct_value(spr->palette.colors[i][2]);
    }

    return true;
}

bool spritemaker_convert_ihq(spritemaker_t *spr) {
    if (spr->detail.enabled || spr->detail.texparms.defined) {
        fprintf(stderr, "ERROR: detail textures are not supported in IHQ mode\n");
        return false;
    }

    // A IHQ image fakes doubling the available TMEM. So check whether the TMEM
    // usage as RGBA16 is lower than 8192 bytes, otherwise it doesn't fit.
    if (calc_tmem_usage(FMT_RGBA16, spr->images[0].width, spr->images[0].height) > 8192) {
        fprintf(stderr, "ERROR: image too big for IHQ mode (max is 64x64, or 128x32, or similar)\n");
        return false;
    }

    if (spr->images[0].width % 2 || spr->images[0].height % 2) {
        fprintf(stderr, "ERROR: both width or height must be a multiple of 2 for IHQ mode\n");
        return false;
    }

    // Since we need to shrink the image up to 4, check that it's a multiple of 4 on either W or H
    if (spr->images[0].width % 4 != 0 && spr->images[0].height % 4 != 0) {
        fprintf(stderr, "ERROR: either width or height must be a multiple of 4 for IHQ mode\n");
        return false;
    }

    int width = spr->images[0].width;
    int height = spr->images[0].height;

    // Calculate a first 2x2 mipmap
    uint8_t *img22 = image_shrink_box(spr->images[0].image, width, height, true, true);
    uint8_t *img42 = NULL, *img24 = NULL;

    // Check if the original image has any meaningful alpha info
    bool alphausage = false;
    for(int y = 0; y < height && !alphausage; y++){
        uint8_t* src = spr->images[0].image + y*width*4;
        for(int x = 0; x < width && !alphausage; x++){
            if(src[3] < 128) alphausage = true;
            src += 4;
        }
    } // If there is alpha, use IA4 for the detail texture - halfs the shades count, but adds 1 bit per pixel alpha support
    if(flag_verbose && alphausage)
        fprintf(stderr, "IHQ: found transparent pixels, IA4 is used as detail\n");
	
    uint8_t *best_rgb_img = NULL;
    int best_rgb_w = 0, best_rgb_h = 0;
    float best_err = INT32_MAX;
    float best_ifactor = 0;
    uint8_t *best_i_img = alphausage? malloc(width * height * 2) : malloc(width * height);
    uint8_t *i_img = alphausage? malloc(width * height * 2) : malloc(width * height);

    for (int dir=0; dir<2; dir++) {
        uint8_t *img; int iw, ih;
        if (dir == 0) {
            if (width % 4) continue;
            img42 = image_shrink_box(img22, width/2, height/2, true, false);
            img = img42; iw = width/4; ih = height/2;
        } else {
            if (height % 4) continue;
            img24 = image_shrink_box(img22, width/2, height/2, false, true);
            img = img24; iw = width/2; ih = height/4;
        }

        float wstep = (float)iw / width;
        float hstep = (float)ih / height;

        for (int factor=1; factor<=10; factor++) {
            float ifactor = 0.05f * factor;
            float mse = 0;

            for (int y=0; y<height; y++) {
                float yy = y * hstep;
                int yy0 = (int)yy;
                int yy1 = yy0+1;
                float yyf = yy - yy0;

                for (int x=0; x<width; x++) {
                    uint8_t r0 = spr->images[0].image[(y*width + x)*4 + 0];
                    uint8_t g0 = spr->images[0].image[(y*width + x)*4 + 1];
                    uint8_t b0 = spr->images[0].image[(y*width + x)*4 + 2];
                    uint8_t a0 = spr->images[0].image[(y*width + x)*4 + 3];

                    float xx = x * wstep;
                    int xx0 = (int)xx;
                    int xx1 = xx0+1;
                    float xxf = xx - xx0;

                    uint8_t rm0 = img[(yy0*iw + xx0)*4 + 0];
                    uint8_t gm0 = img[(yy0*iw + xx0)*4 + 1];
                    uint8_t bm0 = img[(yy0*iw + xx0)*4 + 2];

                    uint8_t rm1 = img[(yy0*iw + xx1)*4 + 0];
                    uint8_t gm1 = img[(yy0*iw + xx1)*4 + 1];
                    uint8_t bm1 = img[(yy0*iw + xx1)*4 + 2];

                    uint8_t rm2 = img[(yy1*iw + xx0)*4 + 0];
                    uint8_t gm2 = img[(yy1*iw + xx0)*4 + 1];
                    uint8_t bm2 = img[(yy1*iw + xx0)*4 + 2];

                    uint8_t rm3 = img[(yy1*iw + xx1)*4 + 0];
                    uint8_t gm3 = img[(yy1*iw + xx1)*4 + 1];
                    uint8_t bm3 = img[(yy1*iw + xx1)*4 + 2];

                    // Bilinear interpolate
                    uint8_t r = (uint8_t)(rm0 * (1-xxf) * (1-yyf) + rm1 * xxf * (1-yyf) + rm2 * (1-xxf) * yyf + rm3 * xxf * yyf);
                    uint8_t g = (uint8_t)(gm0 * (1-xxf) * (1-yyf) + gm1 * xxf * (1-yyf) + gm2 * (1-xxf) * yyf + gm3 * xxf * yyf);
                    uint8_t b = (uint8_t)(bm0 * (1-xxf) * (1-yyf) + bm1 * xxf * (1-yyf) + bm2 * (1-xxf) * yyf + bm3 * xxf * yyf);

                    float err;
                    uint8_t i = ihq_calc_best_i4(ifactor, r0, g0, b0, r, g, b, &err);

                    // If there's alpha present, include it in the texture as IA format
                    if(alphausage){
                        i_img[(y*width + x) * 2] = i;
                        i_img[((y*width + x) * 2) + 1] = a0;
                    }
                    else i_img[y*width + x] = i;
                    // if (x==16 && y==0) {
                    //     printf("IHQ: (%d,%d): %d %d %d -> %d %d %d\n", x, y, r0, g0, b0, r, g, b);
                    //     printf("IHQ: i=%d err=%f rgb=(%d,%d,%d)\n", i, err, (int)(r*(1-ifactor)+i*ifactor), (int)(g*(1-ifactor)+i*ifactor), (int)(b*(1-ifactor)+i*ifactor));
                    // }
                    mse += err;
                }
            }

            mse = sqrtf(mse / (width * height));
            if (mse < best_err) {
                best_err = mse;
                best_ifactor = ifactor;
                best_rgb_w = iw;
                best_rgb_h = ih;
                best_rgb_img = img;
                SWAP(best_i_img, i_img);
            }
             if (flag_verbose)
                fprintf(stderr, "IHQ: detail factor=%.1f mse=%f\n", ifactor, mse);
        }
    }

    // We computed the best IHQ image, now copy it as detail texture
    spr->detail.blend_factor = best_ifactor;
    spr->detail.enabled = true;
    spr->detail.use_main_tex = false;
    spr->images[7].fmt = alphausage? FMT_IA4 : FMT_I4;
    spr->images[7].ct = alphausage? LCT_GREY_ALPHA : LCT_GREY;
    spr->images[7].image = best_i_img;
    spr->images[7].width = width;
    spr->images[7].height = height;

    // Put the halved image as first image
    spr->images[0].fmt = FMT_RGBA16;
    spr->images[0].image = best_rgb_img;
    spr->images[0].width = best_rgb_w;
    spr->images[0].height = best_rgb_h;

    // Copy initial texparms into detail texparms, so that detail will follow
    // the same mirror/wrap settings.
    spr->detail.texparms.defined = true;
    spr->detail.texparms.s = spr->texparms.s;
    spr->detail.texparms.t = spr->texparms.t;

    // Enable embedded texparms for main texture. Add 1,1 to scale because
    // it's at least twice smaller, and then another 1 for the direction where
    // it's four times smaller.
    spr->texparms.defined = true;
    spr->texparms.s.scale += 1;
    spr->texparms.t.scale += 1;
    if (best_rgb_w == width/4)
        spr->texparms.s.scale += 1;
    else
        spr->texparms.t.scale += 1;

    if (flag_verbose)
        fprintf(stderr, "computed IHQ planes (rgb:%dx%d, factor=%.1f, rmsd=%.4f)\n", best_rgb_w, best_rgb_h, best_ifactor, best_err);

    if (img22 && img22 != best_rgb_img) free(img22);
    if (img42 && img42 != best_rgb_img) free(img42);
    free(i_img);
    return true;
}

typedef struct {
    int r,g,b;
} shq_rgb;

typedef struct {
    float l,a,b;
} shq_lab;

typedef struct {
    shq_rgb pix[4];
    shq_lab lab[4];

    int i[4];
    shq_rgb avg;
} shq_box;

static float gamma_correction(float value) {
    if (value > 0.04045f) {
        return powf((value + 0.055f) / 1.055f, 2.4f);
    } else {
        return value / 12.92f;
    }
}

static float xyz_to_lab_helper(float value) {
    if (value > 0.008856f) {
        return powf(value, 1.0f / 3.0f);
    } else {
        return (7.787f * value) + (16.0f / 116.0f);
    }
}

static shq_lab rgb_to_lab(shq_rgb in) {
    float r = in.r / 255.0f;
    float g = in.g / 255.0f;
    float b = in.b / 255.0f;

    r = gamma_correction(r);
    g = gamma_correction(g);
    b = gamma_correction(b);

    float x = r * 0.4124564f + g * 0.3575761f + b * 0.1804375f;
    float y = r * 0.2126729f + g * 0.7151522f + b * 0.0721750f;
    float z = r * 0.0193339f + g * 0.1191920f + b * 0.9503041f;

    x *= 100.0f;
    y *= 100.0f;
    z *= 100.0f;

    x = x / 95.047f;
    y = y / 100.000f;
    z = z / 108.883f;

    float fx = xyz_to_lab_helper(x);
    float fy = xyz_to_lab_helper(y);
    float fz = xyz_to_lab_helper(z);

    return (shq_lab){
        (116.0f * fy) - 16.0f,
        500.0f * (fx - fy),
        200.0f * (fy - fz),
    };
}

static float shq_error(shq_box *box)
{
    float error = 0.0f;
    int r8 = (box->avg.r << 3) + (box->avg.r >> 2);
    int g8 = (box->avg.g << 3) + (box->avg.g >> 2);
    int b8 = (box->avg.b << 3) + (box->avg.b >> 2);

    for (int i=0; i<4; i++) {
        assert(box->i[i] >= 0 && box->i[i] <= 15);
        int i8 = box->i[i] * 0x11;
        shq_lab lab = rgb_to_lab((shq_rgb){ MAX(i8-r8,0), MAX(i8-g8,0), MAX(i8-b8,0) });

        error +=
            (lab.l - box->lab[i].l) * (lab.l - box->lab[i].l) +
            (lab.a - box->lab[i].a) * (lab.a - box->lab[i].a) +
            (lab.b - box->lab[i].b) * (lab.b - box->lab[i].b);        
    }
    return error;
}

static float shq_optimize(shq_box *box)
{
    // Convert input pixels into CIELAB space
    for (int i=0; i<4; i++)
        box->lab[i] = rgb_to_lab(box->pix[i]);

    float error = shq_error(box);
    while (1) {
        // Calculate gradient of the error function on each variable
        int *parms[7] = {
            &box->i[0], &box->i[1], &box->i[2], &box->i[3],
            &box->avg.r, &box->avg.g, &box->avg.b,
        };

        // Try increment and decrement each parameter, and find the best move
        float best_err = error;
        int best_idx = -1;
        int best_val = 0;
        for (int i=0; i<7; i++) {
            int old_val = *parms[i];
            for (int j=-3; j<=3; j++) {
                if (j == 0) continue;
                *parms[i] = old_val + j;
                if (*parms[i] >= 0 && *parms[i] <= (i<4 ? 15 : 31)) {
                    float new_err = shq_error(box);
                    if (new_err < best_err) {
                        best_err = new_err;
                        best_idx = i;
                        best_val = *parms[i];
                    }
                }
            }
            *parms[i] = old_val;
        }

        // If we found a better value, apply it
        if (best_idx != -1) {
            *parms[best_idx] = best_val;
            error = best_err;
        } else {
            break;
        }
    }

    return error;
}

bool spritemaker_convert_shq(spritemaker_t *spr)
{
    int width = spr->images[0].width;
    int height = spr->images[0].height;

    if (width % 2 != 0 || height % 2 != 0) {
        fprintf(stderr, "ERROR: SHQ mode requires width and height to be a multiple of 2\n");
        return false;
    }

    // Go though each 2x2 pixel box
    uint8_t *img = spr->images[0].image;
    uint8_t *shq_i = malloc(width * height);
    uint8_t *shq_rgb = malloc((width/2) * (height/2) * 4);
    float error = 0.0f;

    for (int y=0; y<height; y+=2) {
        for (int x=0; x<width; x+=2) {
            shq_box box = {0};

            // Fetch first 4 pixels
            box.pix[0].r = img[(y*width + x)*4 + 0];
            box.pix[0].g = img[(y*width + x)*4 + 1];
            box.pix[0].b = img[(y*width + x)*4 + 2];

            box.pix[1].r = img[(y*width + x+1)*4 + 0];
            box.pix[1].g = img[(y*width + x+1)*4 + 1];
            box.pix[1].b = img[(y*width + x+1)*4 + 2];

            box.pix[2].r = img[((y+1)*width + x)*4 + 0];
            box.pix[2].g = img[((y+1)*width + x)*4 + 1];
            box.pix[2].b = img[((y+1)*width + x)*4 + 2];

            box.pix[3].r = img[((y+1)*width + x+1)*4 + 0];
            box.pix[3].g = img[((y+1)*width + x+1)*4 + 1];
            box.pix[3].b = img[((y+1)*width + x+1)*4 + 2];

            // Start with the I values as the maximum of the RGB values, and convert
            // them to 4 bits, making sure we are sitll above the original value.
            for (int i=0; i<4; i++) {
                int i8 = MAX(box.pix[i].r, MAX(box.pix[i].g, box.pix[i].b));
                int i4 = i8 >> 4;
                if (i4 * 0x11 < i8) i4++;
                box.i[i] = i4;
            }

            // Start with RGB values as the average of the subtractive blending
            // Notice that *0x11 is just the proper conversion from 4 bit to 8 bit.
            box.avg.r = (MAX(box.i[0]*0x11 - box.pix[0].r,0) + MAX(box.i[1]*0x11 - box.pix[1].r,0) + MAX(box.i[2]*0x11 - box.pix[2].r,0) + MAX(box.i[3]*0x11 - box.pix[3].r,0)) / 4;
            box.avg.g = (MAX(box.i[0]*0x11 - box.pix[0].g,0) + MAX(box.i[1]*0x11 - box.pix[1].g,0) + MAX(box.i[2]*0x11 - box.pix[2].g,0) + MAX(box.i[3]*0x11 - box.pix[3].g,0)) / 4;
            box.avg.b = (MAX(box.i[0]*0x11 - box.pix[0].b,0) + MAX(box.i[1]*0x11 - box.pix[1].b,0) + MAX(box.i[2]*0x11 - box.pix[2].b,0) + MAX(box.i[3]*0x11 - box.pix[3].b,0)) / 4;
            box.avg.r >>= 3;
            box.avg.g >>= 3;
            box.avg.b >>= 3;

            // Run optimization step
            error += shq_optimize(&box);

            assert(box.i[0] >= 0 && box.i[0] <= 15);
            assert(box.i[1] >= 0 && box.i[1] <= 15);
            assert(box.i[2] >= 0 && box.i[2] <= 15);
            assert(box.i[3] >= 0 && box.i[3] <= 15);
            assert(box.avg.r >= 0 && box.avg.r <= 31);
            assert(box.avg.g >= 0 && box.avg.g <= 31);
            assert(box.avg.b >= 0 && box.avg.b <= 31);

            // Store the optimized I value, upscaled to 8 bit
            shq_i[(y+0)*width + x+0] = box.i[0] | (box.i[0] << 4);
            shq_i[(y+0)*width + x+1] = box.i[1] | (box.i[1] << 4);
            shq_i[(y+1)*width + x+0] = box.i[2] | (box.i[2] << 4);
            shq_i[(y+1)*width + x+1] = box.i[3] | (box.i[3] << 4);

            // Store the average color, again upscaled to 8 bit
            shq_rgb[(y/2*width/2 + x/2)*4 + 0] = (box.avg.r << 3) | (box.avg.r >> 2);
            shq_rgb[(y/2*width/2 + x/2)*4 + 1] = (box.avg.g << 3) | (box.avg.g >> 2);
            shq_rgb[(y/2*width/2 + x/2)*4 + 2] = (box.avg.b << 3) | (box.avg.b >> 2);
            shq_rgb[(y/2*width/2 + x/2)*4 + 3] = 0xFF;
        }
    }

    if (flag_verbose)
        fprintf(stderr, "computed SHQ planes (rmsd=%.4f)\n", sqrtf(error / (width * height * 3)));

    // Store the SHQ image
    free(spr->images[0].image);
    spr->images[0].fmt = FMT_I4;
    spr->images[0].image = shq_i;
    spr->images[0].width = width;
    spr->images[0].height = height;
    spr->images[0].ct = LCT_GREY;

    // Store the RGBA image as first mipmap
    spr->images[1].fmt = FMT_RGBA16;
    spr->images[1].image = shq_rgb;
    spr->images[1].width = width / 2;
    spr->images[1].height = height / 2;
    spr->images[1].ct = LCT_RGBA;
#if 0
    // Construct resulting image
    uint8_t *output = malloc(width * height * 4);
    for (int y=0; y<height; y++) {
        for (int x=0; x<width; x++) {
            int i = shq_i[y*width + x];
            uint8_t *rgb = &shq_rgb[(y/2*width/2 + x/2)*4];
            output[(y*width + x)*4 + 0] = MAX(i - rgb[0], 0);
            output[(y*width + x)*4 + 1] = MAX(i - rgb[1], 0);
            output[(y*width + x)*4 + 2] = MAX(i - rgb[2], 0);
            output[(y*width + x)*4 + 3] = 0xFF;
        }
    }
    spr->images[2].fmt = FMT_RGBA16;
    spr->images[2].image = output;
    spr->images[2].width = width;
    spr->images[2].height = height;
    spr->images[2].ct = LCT_RGBA;
#endif
    return true;
}

bool spritemaker_write(spritemaker_t *spr) {
    FILE *out = spr->out;

    // Write the sprite header
    // For Z-buffer image, we currently encode them as RGBA16 though that's not really correct.
    tex_format_t img0fmt = spr->images[0].fmt;
    if (img0fmt == FMT_ZBUF) img0fmt = FMT_IA16;
    w16(out, spr->images[0].width);
    w16(out, spr->images[0].height);
    w8(out, 0); // deprecated field
    w8(out, (uint8_t)(img0fmt | SPRITE_FLAGS_EXT));
    w8(out, spr->hslices);
    w8(out, spr->vslices);

    uint32_t w_palpos = 0;
    uint32_t w_lodpos[7] = {0};

    // Process the images (the first always exists)
    for (int m=0; m<MAX_IMAGES; m++) {
        image_t *image = &spr->images[m];
        if (image->image == NULL)
            continue;

        if (m > 0) {
            assert(w_lodpos[m-1] != 0); // we should have left a placeholder for this LOD
            uint32_t xpos = ftell(out) | (image->fmt << 24);
            w32_at(out, w_lodpos[m-1], xpos);
        }

        switch ((int)image->fmt) {
        case FMT_RGBA16: {
            assert(image->ct == LCT_RGBA);
            // Convert to 16-bit RGB5551 format.
            uint8_t *img = image->image;
            for (int i=0;i<image->width*image->height;i++) {
                w16(out, conv_rgb5551_dither(img[0], img[1], img[2], img[3], i % image->width, i / image->height, spr->ditheralgo));
                img += 4;
            }
            break;
        }

        case FMT_CI4: {
            assert(image->ct == LCT_PALETTE);
            assert(spr->palette.used_colors <= 16);
            // Convert image to 4 bit.
            uint8_t *img = image->image;
            for (int j=0; j<image->height; j++) {
                for (int i=0; i<image->width; i+=2) {
                    uint8_t ix0 = *img++;
                    uint8_t ix1 = (i+1 == image->width) ? 0 : *img++;
                    assert(ix0 < 16 && ix1 < 16);
                    w8(out, (uint8_t)((ix0 << 4) | ix1));
                }
            }
            break;
        }

        case FMT_IA8: {
            assert(image->ct == LCT_GREY_ALPHA);
            uint8_t *img = image->image;
            for (int i=0; i<image->width*image->height; i++) {
                uint8_t I = *img++; uint8_t A = *img++;
                w8(out, (uint8_t)((I & 0xF0) | (A >> 4)));
            }
            break;
        }

        case FMT_I4: {
            assert(image->ct == LCT_GREY);
            uint8_t *img = image->image;
            for (int j=0; j<image->height; j++) {
                for (int i=0; i<image->width; i+=2) {
                    uint8_t I0 = *img++;
                    uint8_t I1 = (i+1 == image->width) ? 0 : *img++;
                    w8(out, (uint8_t)((I0 & 0xF0) | (I1 >> 4)));
                }
            }
            break;
        }

        case FMT_IA4: {
            assert(image->ct == LCT_GREY_ALPHA);
            // IA4 is 3 bit intensity and 1 bit alpha. Pack it
            uint8_t *img = image->image;
            for (int j=0; j<image->height; j++) {
                for (int i=0; i<image->width; i+=2) {
                    uint8_t I0 = *img++;
                    uint8_t A0 = *img++;
                    uint8_t I1 = (i+1 == image->width) ? 0 : *img++;
                    uint8_t A1 = (i+1 == image->width) ? 0 : *img++;
                    A0 = A0 >= 128 ? 1 : 0;
                    A1 = A1 >= 128 ? 1 : 0;
                    w8(out, (uint8_t)((I0 & 0xE0) | (A0 << 4) | ((I1 & 0xE0) >> 4) | A1));
                }
            }
            break;
        }

        case FMT_ZBUF: {
            assert(image->ct == LCT_GREY);
            uint8_t *img = image->image;
            for (int j=0; j<image->height; j++) {
                for (int i=0; i<image->width; i++) {
                    uint32_t Z0 = (img[0] << 8) | img[1]; img += 2;
                    Z0 <<= 2; // Convert into 0.15.3
                    uint16_t FZ0 = conv_float14(Z0) << 2;
                    w16(out, FZ0);
                }
            }
            break;
        }

        default: {
            // No further conversion needed. Used for: RGBA32, IA16, CI8, I8.
            int numbytes = TEX_FORMAT_PIX2BYTES(image->fmt, image->width*image->height);
            fwrite(image->image, 1, numbytes, out);
            break;
        }
        }

        // Padding to force alignment of every image
        walign(out, 8);
        
        // Write extended sprite header after first image
        // See sprite_ext_t (sprite_internal.h)
        if (m == 0) { 
            w16(out, 124);  // sizeof(sprite_ext_t)
            w16(out, 4);    // version
            w_palpos = w32_placeholder(out); // placeholder for position of palette
            int numlods = 0;
            for (int i=1; i<8; i++) {
                numlods += (spr->images[i].image != NULL);
                w16(out, spr->images[i].width);
                w16(out, spr->images[i].height);
                w_lodpos[i-1] = w32_placeholder(out); // placeholder for position of LOD
            }
            uint16_t flags = spr->out_flags;
            assert(numlods <= 7); // 3 bits
            flags |= numlods;
            if (spr->texparms.defined) flags |= 0x8;
            if (spr->detail.enabled) flags |= 0x10;
            if (spritemaker_fit_tmem(spr, NULL)) flags |= 0x20;
            w16(out, flags);
            w16(out, 0); // padding
            wf32(out, spr->texparms.s.translate);
            wf32(out, spr->texparms.s.repeats);
            w16(out, spr->texparms.s.scale);
            w8(out, spr->texparms.s.mirror);
            w8(out, 0); // padding
            wf32(out, spr->texparms.t.translate);
            wf32(out, spr->texparms.t.repeats);
            w16(out, spr->texparms.t.scale);
            w8(out, spr->texparms.t.mirror);
            w8(out, 0); // padding

            // detail texture
            wf32(out, spr->detail.texparms.s.translate);
            wf32(out, spr->detail.texparms.s.repeats);
            w16(out, spr->detail.texparms.s.scale);
            w8(out, spr->detail.texparms.s.mirror);
            w8(out, 0); // padding
            wf32(out, spr->detail.texparms.t.translate);
            wf32(out, spr->detail.texparms.t.repeats);
            w16(out, spr->detail.texparms.t.scale);
            w8(out, spr->detail.texparms.t.mirror);
            w8(out, 0); // padding
            wf32(out, spr->detail.blend_factor);
            w8(out, spr->detail.use_main_tex);
            w8(out, 0); // padding
            w8(out, 0); // padding
            w8(out, 0); // padding

            walign(out, 8);
        }
    }

    // Finally, write the palette if needed, stored in the first image
    if (spr->palette.num_colors > 0) {
        assert(spr->images[0].fmt == FMT_CI8 || spr->images[0].fmt == FMT_CI4);
        w32_at(out, w_palpos, ftell(out));

        // Convert the palette into RGB5551 format. The number of colors can differ
        // from the target, for instanc a PNG with LCT_PALETTE of 64 colors but only
        // actually using the first 16. We handle this without quantization, but still
        // saves the full 64 color palette as it might contain useful colors for effects.
        // FIXME: add the palette size to the sprite_ext_format and sprite API.
        for (int i=0; i<spr->palette.num_colors; i++) {
            uint8_t *pal = spr->palette.colors[i];
            w16(out, conv_rgb5551(pal[0], pal[1], pal[2], pal[3]));
        }
        walign(out, 8);
    }

    return true;
}

void spritemaker_write_pngs(spritemaker_t *spr, const char *outfn) {
    for (int i=0; i<MAX_IMAGES; i++) {
        if (spr->images[i].image == NULL)
            continue;
        char lodext[16]; snprintf(lodext, sizeof(lodext), ".%d.png", i);
        char debugfn[2048];
        strcpy(debugfn, outfn);
        strcpy(strrchr(debugfn, '.'), lodext);

        image_t *img = &spr->images[i];
        if (flag_verbose)
            fprintf(stderr, "writing debug file: %s\n", debugfn);

        // Write the PNG file respecting the colortype. Notice that we can't use
        // the simple lodepng_encode_file as it doesn't support a palette, so we need
        // to use the lower level API.
        LodePNGState state;
        lodepng_state_init(&state);
        state.encoder.auto_convert = false; // avoid automatic remapping of palette colors
        state.info_raw = lodepng_color_mode_make(img->ct, 8);
        state.info_png.color = lodepng_color_mode_make(img->ct, 8);
        if (img->ct == LCT_PALETTE) {
            for (int i=0; i<spr->palette.num_colors; i++) {
                lodepng_palette_add(&state.info_raw,       spr->palette.colors[i][0], spr->palette.colors[i][1], spr->palette.colors[i][2], spr->palette.colors[i][3]);
                lodepng_palette_add(&state.info_png.color, spr->palette.colors[i][0], spr->palette.colors[i][1], spr->palette.colors[i][2], spr->palette.colors[i][3]);
            }
        }
        uint8_t *out = NULL; size_t outsize;
        unsigned error = lodepng_encode(&out, &outsize, img->image, img->width, img->height, &state);
        if (!error) error = lodepng_save_file(out, outsize, debugfn);
        lodepng_state_cleanup(&state);
        if (out) lodepng_free(out);
        if (error) {
            fprintf(stderr, "ERROR: writing debug file %s: %s\n", debugfn, lodepng_error_text(error));
        }
    }
}

void spritemaker_free(spritemaker_t *spr) {
    for (int i=0; i<MAX_IMAGES; i++)
        if (spr->images[i].image)
            free(spr->images[i].image);
    memset(spr, 0, sizeof(*spr));
}

int convert(const char *infn, const char *outfn, const parms_t *pm, int compression) {
    FILE *out = tmpfile();
    bool out_is_stdout = (strstr(outfn, "(stdout)") != NULL);

    if (flag_verbose)
        fprintf(stderr, "Converting: %s -> %s [fmt=%s tiles=%d,%d mipmap=%s dither=%s]\n",
            infn, outfn, tex_format_name(pm->outfmt), pm->tilew, pm->tileh, mipmap_algo_name(pm->mipmap_algo), dither_algo_name(pm->dither_algo));

    spritemaker_t spr = {0};

    spr.ditheralgo = pm->dither_algo;
    spr.infn = infn;
    spr.out = out;
    spr.texparms = pm->texparms;
    if (!spr.texparms.defined) {
        spr.texparms.s.translate = 0.0f;
        spr.texparms.s.scale = 0;
        spr.texparms.s.repeats = 1;
        spr.texparms.s.mirror = 0;
        spr.texparms.t = spr.texparms.s;
    }

    spr.detail.enabled = pm->detail.enabled;
    spr.detail.use_main_tex = pm->detail.use_main_tex;
    spr.detail.infn = pm->detail.infn;
    spr.detail.blend_factor = pm->detail.blend_factor;
    spr.detail.texparms = pm->detail.texparms;
    if (!spr.detail.texparms.defined) {
        spr.detail.texparms.s.translate = 0.0f;
        spr.detail.texparms.s.scale = -1;
        spr.detail.texparms.s.repeats = 2048;
        spr.detail.texparms.s.mirror = 0;
        spr.detail.texparms.t = spr.detail.texparms.s;
    }

    int mipmap_algo = pm->mipmap_algo;

    // Load the PNG, passing the desired output format (or FMT_NONE if autodetect).
    if (!spritemaker_load_png(&spr, pm->outfmt))
        goto error;

    if (pm->gamma_correct) {
        if (!spritemaker_gamma_correct(&spr))
            goto error;
    }

    if (spr.images[0].fmt == FMT_IHQ) {
        if (!spritemaker_convert_ihq(&spr))
            goto error;
        // Compute mipmaps for IHQ
        mipmap_algo = MIPMAP_ALGO_BOX;
    } else if (spr.images[0].fmt == FMT_SHQ) {
        spr.out_flags |= 0x40;
        if (!spritemaker_convert_shq(&spr))
            goto error;
        // Mipmaps not supported for SHQ
        if (mipmap_algo != MIPMAP_ALGO_NONE) {
            fprintf(stderr, "WARNiNG: mipmap generation is not supported for SHQ mode\n");
            mipmap_algo = MIPMAP_ALGO_NONE;
        }
    } else if (spr.detail.enabled && !spr.detail.use_main_tex) {
        // Load the detail PNG, passing the desired output format (or FMT_NONE if autodetect).
        if (!spritemaker_load_detail_png(&spr, pm->detail.outfmt))
            goto error;
    }

    // Calculate mipmap levels, if requested
    if (mipmap_algo != MIPMAP_ALGO_NONE) {
        switch (spr.images[0].ct) {
        case LCT_PALETTE: {
            // Mipmap generation of indexed image. In this case, we want to
            // preserve the original palette for all the mipmaps. To reuse
            // existing code, we expand first to RGBA and then quantize again
            // the original palette.
            palette_t orig_palette = spr.palette;
            int fmt_colors = spr.images[0].fmt == FMT_CI8 ? 256 : 16;

            // Expand to RGBA, calc lods, and quantize with the original palette
            if (!spritemaker_expand_rgba(&spr)
                || !spritemaker_calc_lods(&spr, mipmap_algo)
                || !spritemaker_quantize(&spr, orig_palette.colors[0], fmt_colors, pm->dither_algo))
                goto error;

            // Restore palette. Notice that spritemake_quantize has already done that
            // but the palette might contain additional colors (eg: a CI4 sprite
            // might be shipped with a 64 color palette that the user will use
            // at runtime). So we quantized all lods with the first 16 colors
            // (like the first image), but then we restore the other colors.
            spr.palette = orig_palette;
        }   break;

        default:
            if (!spritemaker_calc_lods(&spr, mipmap_algo))
                goto error;
            break;
        }
    }

    // Run quantization if needed
    if (spr.images[0].fmt == FMT_CI8 || spr.images[0].fmt == FMT_CI4) {
        int expected_colors = spr.images[0].fmt == FMT_CI8 ? 256 : 16;

        switch (spr.images[0].ct) {
        case LCT_RGBA:
            if (!spritemaker_quantize(&spr, NULL, expected_colors, pm->dither_algo))
                goto error;
            break;
        case LCT_PALETTE:
            // When the source image is already palettized, we quantize only if
            // the requested number of colors is less than the actually used colors.
            if (expected_colors < spr.palette.used_colors) {
                if (!spritemaker_expand_rgba(&spr) || 
                    !spritemaker_quantize(&spr, NULL, expected_colors, pm->dither_algo))
                    goto error;
            }
            break;
        default:
            assert(0); // should not get here
        }
    }

    // Dump TMEM usage
    if (flag_verbose) {
        int tmem_usage; spritemaker_fit_tmem(&spr, &tmem_usage);
        fprintf(stderr, "TMEM required: %d bytes\n", tmem_usage);
    }

    // Legacy support for old mksprite usage
    if (pm->hslices) spr.hslices = pm->hslices;
    if (pm->vslices) spr.vslices = pm->vslices;
    // Autodetection of optimal slice size. NOTE: we currently don't
    // use this in rdpq. rdpq_tex does its own from-scratch calculation,
    // but we could skip some runtime work by doing the same here.
    if (pm->tilew) spr.hslices = spr.images[0].width / pm->tilew;
    if (pm->tileh) spr.vslices = spr.images[0].height / pm->tileh;
    if (!spr.hslices) {
        spr.hslices = spr.images[0].width / 16;
        if (!spr.hslices) spr.hslices = 1;
    }
    if (!spr.vslices) {
        spr.vslices = spr.images[0].height / 16;
        if (!spr.vslices) spr.vslices = 1;
    }

    // Write the sprite
    if (!spritemaker_write(&spr))
        goto error;

    // Write debug files
    if (flag_debug && !out_is_stdout)
        spritemaker_write_pngs(&spr, outfn);

    spritemaker_free(&spr);

    // Read back the temporary file contents into RAM
    int sz = ftell(out);
    rewind(out);
    uint8_t *data = malloc(sz);
    fread(data, 1, sz, out);
    fclose(out);

    // Compress the data and store it into output file
    // This is a nop if compression is disabled, but at least
    // we don't have two different code paths.
    if (out_is_stdout) {
        out = stdout;
    } else {
        out = fopen(outfn, "wb");
        if (!out) {
            fprintf(stderr, "ERROR: can't open output file %s\n", outfn);
            free(data);
            return 1;
        }
    }

    if (compression == -1) compression = DEFAULT_COMPRESSION;
    int cmp_size = asset_compress_mem(data, sz, out, compression, 256*1024, NULL);
    free(data);

    if (flag_verbose) {
        if (compression > 0) {
            fprintf(stderr, "compressed: %s (%d -> %d, ratio %.1f%%)\n", outfn,
                (int)sz, cmp_size, 100.0 * (float)cmp_size / (float)(sz == 0 ? 1 : sz));
        } else {
            fprintf(stderr, "written: %s (%d bytes)\n", outfn, sz);
        }
    }

    fclose(out);
    return 0;

error:
    spritemaker_free(&spr);
    fclose(out);
    return 1;
}

bool cli_parse_texparms(const char *opt, texparms_t *parms)
{
    char extra;
    if (sscanf(opt, "%f,%f,%d,%d,%f,%f,%d,%d%c", 
            &parms->s.translate, &parms->t.translate,
            &parms->s.scale, &parms->t.scale,
            &parms->s.repeats, &parms->t.repeats,
            &parms->s.mirror, &parms->t.mirror,
            &extra) == 8) {
        // ok, nothing to do
    } else if (sscanf(opt, "%f,%d,%f,%d%c", 
            &parms->s.translate, &parms->s.scale, &parms->s.repeats, &parms->s.mirror, &extra) == 4) {
        parms->t = parms->s;
    } else {
        fprintf(stderr, "invalid texparms: %s\n", opt);
        return false;
    }
    if (parms->s.mirror != 0 && parms->s.mirror != 1) {
        fprintf(stderr, "invalid texparms: mirror must be 0 or 1 (found: %d)\n", parms->s.mirror);
        return false;
    }
    if (parms->t.mirror != 0 && parms->t.mirror != 1) {
        fprintf(stderr, "invalid texparms: mirror must be 0 or 1 (found: %d)\n", parms->t.mirror);
        return false;
    }
    if (parms->s.repeats < 0) {
        fprintf(stderr, "invalid texparms: repeats must be >= 0 (found: %f)\n", parms->s.repeats);
        return false;
    }
    if (parms->t.repeats < 0) {
        fprintf(stderr, "invalid texparms: repeats must be >= 0 (found: %f)\n", parms->t.repeats);
        return false;
    }
    if (parms->s.repeats > 2048) parms->s.repeats = 2048;
    if (parms->t.repeats > 2048) parms->t.repeats = 2048;
    parms->defined = true;
    return true;
}


int main(int argc, char *argv[])
{
    char *infn = NULL, *outdir = ".", *outfn = NULL;
    parms_t pm = {0}; int compression = -1;
    bool at_least_one_file = false;

    if (argc < 2) {
        print_args(argv[0]);
        return 1;
    }

    // We still support (but not document) the old mksprite command line
    // syntax: mksprite <bitdepth> [hslices vslices] input output
    if ((argc == 4 || argc == 6) && (!strcmp(argv[1], "16") || !strcmp(argv[1], "32"))) {
        int i = 1;
        pm.outfmt = !strcmp(argv[i++], "16") ? FMT_RGBA16 : FMT_RGBA32;
        if (argc == 6) {
            pm.hslices = atoi(argv[i++]);
            pm.vslices = atoi(argv[i++]);
        }
        infn = argv[i++];
        outfn = argv[i++];
        printf("WARNING: deprecated command-line syntax was used, please switch to new syntax\n");
        return convert(infn, outfn, &pm, 0);
    }

    bool error = false;
    /* console arguments */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            /* ---------------- HELP  console argument ------------------- */
            /* --help */
            if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
                print_args(argv[0]);
                return 0;
            } 

            /* ---------------- VERBOSE console argument ------------------- */
            /* -v/--verbose   Verbose output             */
            else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
                flag_verbose = true;
            } 

            /* ---------------- DEBUG  console argument ------------------- */
            /* -d/--debug     Dump computed images (eg: mipmaps) as PNG files in output directory             */
            else if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--debug")) {
                flag_debug = true;
            } 

            /* ---------------- OUTPUT FILE console argument ------------------- */
            /* -o/--output <dir>     Specify output directory (default: .)             */
            else if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) {
                if (++i == argc) {
                    fprintf(stderr, "missing argument for %s\n", argv[i-1]);
                    return 1;
                }
                outdir = argv[i];
            } 

            /* ---------------- FORMAT console argument ------------------- */
            /* -f/--format <fmt>     Specify output format (default: AUTO)             */
            else if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--format")) {
                if (++i == argc) {
                    fprintf(stderr, "missing argument for %s\n", argv[i-1]);
                    return 1;
                }
                pm.outfmt = tex_format_from_name(argv[i]);
                if (pm.outfmt == FMT_NONE && strcasecmp(argv[i], "AUTO") != 0) {
                    fprintf(stderr, "invalid argument for %s: %s\n", argv[i-1], argv[i]);
                    print_supported_formats();
                    return 1;
                }
            } 

            /* ---------------- HV TILES console argument ------------------- */
            else if (!strcmp(argv[i], "-t") || !strcmp(argv[i], "--tiles")) {
                if (++i == argc) {
                    fprintf(stderr, "missing argument for %s\n", argv[i-1]);
                    return 1;
                }
                char extra;
                if (sscanf(argv[i], "%d,%d%c", &pm.tilew, &pm.tileh, &extra) != 2) {
                    fprintf(stderr, "invalid argument for %s: %s\n", argv[i-1], argv[i]);
                    return 1;
                }
            }
            
            /* ---------------- MIPMAP console argument ------------------- */
            /* -m/--mipmap <algo>                    Calculate mipmap levels using the specified algorithm (default: NONE)             */
             else if (!strcmp(argv[i], "-m") || !strcmp(argv[i], "--mipmap")) {
                if (++i == argc) {
                    fprintf(stderr, "missing argument for %s\n", argv[i-1]);
                    return 1;
                }
                if (!strcmp(argv[i], "NONE")) pm.mipmap_algo = MIPMAP_ALGO_NONE;
                else if (!strcmp(argv[i], "BOX")) pm.mipmap_algo = MIPMAP_ALGO_BOX;
                else {
                    fprintf(stderr, "invalid mipmap algorithm: %s\n", argv[i]);
                    print_supported_mipmap();
                    return 1;
                }
            } 

            /* ---------------- DITHER console argument ------------------- */
            /* -D/--dither <dither>  Dithering algorithm (default: NONE)             */
            else if (!strcmp(argv[i], "-D") || !strcmp(argv[i], "--dither")) {
                if (++i == argc) {
                    fprintf(stderr, "missing argument for %s\n", argv[i-1]);
                    return 1;
                }
                if (!strcmp(argv[i], "NONE")) pm.dither_algo = DITHER_ALGO_NONE;
                else if (!strcmp(argv[i], "RANDOM")) pm.dither_algo = DITHER_ALGO_RANDOM;
                else if (!strcmp(argv[i], "ORDERED")) pm.dither_algo = DITHER_ALGO_ORDERED;
                else {
                    fprintf(stderr, "invalid dithering algorithm: %s\n", argv[i]);
                    print_supported_dithers();
                    return 1;
                }
            } 
            
            /* ---------------- COMPRESS console argument ------------------- */
            /* -c/--compress         Compress output files (using mksasset)             */
            else if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--compress")) {
                // Optional compression level
                if (i+1 < argc && argv[i+1][1] == 0) {
                    int level = argv[i+1][0] - '0';
                    if (level >= 0 && level <= 3) {
                        compression = level;
                        i++;
                    }
                    else {
                        fprintf(stderr, "invalid compression level: %s\n", argv[i+1]);
                        return 1;
                    }
                }
            }
            
            /* ---------------- GAMMA_CORRECT console argument ------------------- */
            /* -g/--gamma  Adjust colors for when VI gamma correction is enabled on console (convert to linear colors)                          */
            else if (!strcmp(argv[i], "-g") || !strcmp(argv[i], "--gamma")) {
                pm.gamma_correct = 1;
            }

            /* ---------------- TEXTURE PARAMETERS console argument ------------------- */
            /* --texparms <x,s,r,m>          Sampling parameters             */
            /* --texparms <x,x,s,s,r,r,m,m>  Sampling parameters (different for S/T)             */
            else if (!strcmp(argv[i], "--texparms")) {
                if (++i == argc) {
                    fprintf(stderr, "missing argument for %s\n", argv[i-1]);
                    return 1;
                }
                if (!cli_parse_texparms(argv[i], &pm.texparms))
                    return 1;
            } 
            
            /* ---------------- DETAIL console argument ------------------- */
            /* --detail [<image>][,<fmt>][,<factor>] Activate detail texture             */
            else if (!strcmp(argv[i], "--detail")) {
                pm.detail.blend_factor = 0.5;
                pm.detail.use_main_tex = true;
                pm.detail.outfmt = FMT_NONE;
                pm.detail.infn = NULL;
                pm.detail.enabled = true;

                if (++i != argc) {
                    char *fntok = strdup(argv[i]);
                    char *sect = strtok(fntok, ",");

                    // First argument is either the filename or the factor. If
                    // it's the factor, we should be done
                    if (!sscanf(sect, "%f", &pm.detail.blend_factor)) {
                        // Not a floating point number, should be a filename,
                        // but error out if it's a format instead
                        if (tex_format_from_name(sect) != FMT_NONE) {
                            fprintf(stderr, "cannot specify a format without a filename for %s: %s\n", argv[i-1], argv[i]);
                            return 1;
                        }
                        pm.detail.infn = sect;
                        pm.detail.use_main_tex = false;

                        // Next argument is either the format or the factor
                        sect = strtok(NULL, ",");
                        if (sect) {
                            tex_format_t fmt = tex_format_from_name(sect);
                            if (fmt != FMT_NONE) {
                                pm.detail.outfmt = fmt;
                                sect = strtok(NULL, ",");
                            }
                        }
                        // Third argument (or second) must be the blend factor
                        if (sect) {
                            if (!sscanf(sect, "%f", &pm.detail.blend_factor)) {
                                fprintf(stderr, "invalid argument for %s: %s\n", argv[i-1], argv[i]);
                                return 1;
                            }
                        }
                    }
                    // There should be no other arguments
                    sect = strtok(NULL, ",");
                    if (sect) {
                        fprintf(stderr, "too many values for argument %s: %s\n", argv[i-1], argv[i]);
                        return 1;
                    }
                }
            }

            /* ---------------- DETAIL TEXTURE PARAMETERS console argument ------------------- */
            /* --detail-texparms <x,s,r,m>          Sampling parameters             */
            /* --detail-texparms <x,x,s,s,r,r,m,m>  Sampling parameters (different for S/T)             */
            else if (!strcmp(argv[i], "--detail-texparms")) {
                if (++i == argc) {
                    fprintf(stderr, "missing argument for %s\n", argv[i-1]);
                    return 1;
                }
                if (!cli_parse_texparms(argv[i], &pm.detail.texparms))
                    return 1;
            }
            
            else {
                fprintf(stderr, "invalid flag: %s\n", argv[i]);
                return 1;
            }
            continue;
        }

        at_least_one_file = true;
        infn = argv[i];
        char *basename = strrchr(infn, '/');
        if (!basename) basename = infn; else basename += 1;
        char* basename_noext = strdup(basename);
        char* ext = strrchr(basename_noext, '.');
        if (ext) *ext = '\0';

        asprintf(&outfn, "%s/%s.sprite", outdir, basename_noext);

        if (convert(infn, outfn, &pm, compression) != 0)
            error = true;

        free(outfn);
    }

    if (!at_least_one_file) {
        infn = getenv("MKSPRITE_INFN");
        outfn = getenv("MKSPRITE_OUTFN");
        if (infn) 
            asprintf(&infn, "%s (stdin)", infn);
        else 
            infn = "(stdin)";
        if (outfn)
            asprintf(&outfn, "%s (stdout)", outfn);
        else
            outfn = "(stdout)";

        #ifdef _WIN32
        // Switch stdin/stdout to binary mode
        #define _O_BINARY 0x8000
        setmode(0, _O_BINARY);
        setmode(1, _O_BINARY);
        #endif

        if (convert(infn, outfn, &pm, compression) != 0) {
            error = true;
        }
    }

    return error ? 1 : 0;
}
