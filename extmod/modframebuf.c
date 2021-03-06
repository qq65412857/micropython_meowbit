/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <string.h>

#include "py/runtime.h"

#if MICROPY_PY_FRAMEBUF
#include "py/mphal.h"
// defined for fs operation
#include "lib/oofatfs/ff.h"

#include "extmod/vfs.h"
#include "extmod/vfs_fat.h"

#include "ports/stm32/font_petme128_8x8.h"
#include "bmp.h"
#include "gif.h"

typedef struct _mp_obj_framebuf_t {
    mp_obj_base_t base;
    mp_obj_t buf_obj; // need to store this to prevent GC from reclaiming buf
    void *buf;
    uint16_t width, height, stride;
    uint8_t format;
} mp_obj_framebuf_t;

typedef void (*setpixel_t)(const mp_obj_framebuf_t*, int, int, uint32_t);
typedef uint32_t (*getpixel_t)(const mp_obj_framebuf_t*, int, int);
typedef void (*fill_rect_t)(const mp_obj_framebuf_t *, int, int, int, int, uint32_t);

typedef struct _mp_framebuf_p_t {
    setpixel_t setpixel;
    getpixel_t getpixel;
    fill_rect_t fill_rect;
} mp_framebuf_p_t;

// constants for formats
#define FRAMEBUF_MVLSB    (0)
#define FRAMEBUF_RGB565   (1)
#define FRAMEBUF_GS2_HMSB (5)
#define FRAMEBUF_GS4_HMSB (2)
#define FRAMEBUF_PL8      (6)
#define FRAMEBUF_MHLSB    (3)
#define FRAMEBUF_MHMSB    (4)

// Functions for MHLSB and MHMSB

STATIC void mono_horiz_setpixel(const mp_obj_framebuf_t *fb, int x, int y, uint32_t col) {
    size_t index = (x + y * fb->stride) >> 3;
    int offset = fb->format == FRAMEBUF_MHMSB ? x & 0x07 : 7 - (x & 0x07);
    ((uint8_t*)fb->buf)[index] = (((uint8_t*)fb->buf)[index] & ~(0x01 << offset)) | ((col != 0) << offset);
}

STATIC uint32_t mono_horiz_getpixel(const mp_obj_framebuf_t *fb, int x, int y) {
    size_t index = (x + y * fb->stride) >> 3;
    int offset = fb->format == FRAMEBUF_MHMSB ? x & 0x07 : 7 - (x & 0x07);
    return (((uint8_t*)fb->buf)[index] >> (offset)) & 0x01;
}

STATIC void mono_horiz_fill_rect(const mp_obj_framebuf_t *fb, int x, int y, int w, int h, uint32_t col) {
    int reverse = fb->format == FRAMEBUF_MHMSB;
    int advance = fb->stride >> 3;
    while (w--) {
        uint8_t *b = &((uint8_t*)fb->buf)[(x >> 3) + y * advance];
        int offset = reverse ?  x & 7 : 7 - (x & 7);
        for (int hh = h; hh; --hh) {
            *b = (*b & ~(0x01 << offset)) | ((col != 0) << offset);
            b += advance;
        }
        ++x;
    }
}

// Functions for MVLSB format

STATIC void mvlsb_setpixel(const mp_obj_framebuf_t *fb, int x, int y, uint32_t col) {
    size_t index = (y >> 3) * fb->stride + x;
    uint8_t offset = y & 0x07;
    ((uint8_t*)fb->buf)[index] = (((uint8_t*)fb->buf)[index] & ~(0x01 << offset)) | ((col != 0) << offset);
}

STATIC uint32_t mvlsb_getpixel(const mp_obj_framebuf_t *fb, int x, int y) {
    return (((uint8_t*)fb->buf)[(y >> 3) * fb->stride + x] >> (y & 0x07)) & 0x01;
}

STATIC void mvlsb_fill_rect(const mp_obj_framebuf_t *fb, int x, int y, int w, int h, uint32_t col) {
    while (h--) {
        uint8_t *b = &((uint8_t*)fb->buf)[(y >> 3) * fb->stride + x];
        uint8_t offset = y & 0x07;
        for (int ww = w; ww; --ww) {
            *b = (*b & ~(0x01 << offset)) | ((col != 0) << offset);
            ++b;
        }
        ++y;
    }
}

// Functions for RGB565 format
#define COL0(r, g, b) ((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
#define COL(c) COL0((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff)

STATIC void rgb565_setpixel(const mp_obj_framebuf_t *fb, int x, int y, uint32_t col) {
    col = COL(col);
    uint16_t color = ((col&0xff) << 8) | ((col >> 8) & 0xff);
    ((uint16_t*)fb->buf)[x + y * fb->stride] = color;
}

STATIC uint32_t rgb565_getpixel(const mp_obj_framebuf_t *fb, int x, int y) {
    return ((uint16_t*)fb->buf)[x + y * fb->stride];
}

STATIC void rgb565_fill_rect(const mp_obj_framebuf_t *fb, int x, int y, int w, int h, uint32_t col) {
    col = COL(col);
    uint16_t color = ((col&0xff) << 8) | ((col >> 8) & 0xff);
    uint16_t *b = &((uint16_t*)fb->buf)[x + y * fb->stride];
    while (h--) {
        for (int ww = w; ww; --ww) {
            *b++ = color;
        }
        b += fb->stride - w;
    }
}

// Functions for GS2_HMSB format

STATIC void gs2_hmsb_setpixel(const mp_obj_framebuf_t *fb, int x, int y, uint32_t col) {
    uint8_t *pixel = &((uint8_t*)fb->buf)[(x + y * fb->stride) >> 2];
    uint8_t shift = (x & 0x3) << 1;
    uint8_t mask = 0x3 << shift;
    uint8_t color = (col & 0x3) << shift;
    *pixel = color | (*pixel & (~mask));
}

STATIC uint32_t gs2_hmsb_getpixel(const mp_obj_framebuf_t *fb, int x, int y) {
    uint8_t pixel = ((uint8_t*)fb->buf)[(x + y * fb->stride) >> 2];
    uint8_t shift = (x & 0x3) << 1;
    return (pixel >> shift) & 0x3;
}

STATIC void gs2_hmsb_fill_rect(const mp_obj_framebuf_t *fb, int x, int y, int w, int h, uint32_t col) {
    for (int xx=x; xx < x+w; xx++) {
        for (int yy=y; yy < y+h; yy++) {
            gs2_hmsb_setpixel(fb, xx, yy, col);
        }
    }
}

// Functions for GS4_HMSB format

STATIC void gs4_hmsb_setpixel(const mp_obj_framebuf_t *fb, int x, int y, uint32_t col) {
    uint8_t *pixel = &((uint8_t*)fb->buf)[(x + y * fb->stride) >> 1];

    if (x % 2) {
        *pixel = ((uint8_t)col & 0x0f) | (*pixel & 0xf0);
    } else {
        *pixel = ((uint8_t)col << 4) | (*pixel & 0x0f);
    }
}

STATIC uint32_t gs4_hmsb_getpixel(const mp_obj_framebuf_t *fb, int x, int y) {
    if (x % 2) {
        return ((uint8_t*)fb->buf)[(x + y * fb->stride) >> 1] & 0x0f;
    }

    return ((uint8_t*)fb->buf)[(x + y * fb->stride) >> 1] >> 4;
}

STATIC void gs4_hmsb_fill_rect(const mp_obj_framebuf_t *fb, int x, int y, int w, int h, uint32_t col) {
    col &= 0x0f;
    uint8_t *pixel_pair = &((uint8_t*)fb->buf)[(x + y * fb->stride) >> 1];
    uint8_t col_shifted_left = col << 4;
    uint8_t col_pixel_pair = col_shifted_left | col;
    int pixel_count_till_next_line = (fb->stride - w) >> 1;
    bool odd_x = (x % 2 == 1);

    while (h--) {
        int ww = w;

        if (odd_x && ww > 0) {
            *pixel_pair = (*pixel_pair & 0xf0) | col;
            pixel_pair++;
            ww--;
        }

        memset(pixel_pair, col_pixel_pair, ww >> 1);
        pixel_pair += ww >> 1;

        if (ww % 2) {
            *pixel_pair = col_shifted_left | (*pixel_pair & 0x0f);
            if (!odd_x) {
                pixel_pair++;
            }
        }

        pixel_pair += pixel_count_till_next_line;
    }
}

// Functions for GS8 format
//#define COL08(r, g, b) ((((r) >> 5) << 5) | (((g) >> 5) << 2) | ((b) >> 6))
//#define COL8(c) COL08((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff)
STATIC void gs8_setpixel(const mp_obj_framebuf_t *fb, int x, int y, uint32_t col) {
    uint8_t *pixel = &((uint8_t*)fb->buf)[(x + y * fb->stride)];
    *pixel = (col & 0xff);
}

STATIC uint32_t gs8_getpixel(const mp_obj_framebuf_t *fb, int x, int y) {
    return ((uint8_t*)fb->buf)[(x + y * fb->stride)];
}

STATIC void gs8_fill_rect(const mp_obj_framebuf_t *fb, int x, int y, int w, int h, uint32_t col) {
    uint8_t *pixel = &((uint8_t*)fb->buf)[(x + y * fb->stride)];
    while (h--) {
        memset(pixel, col, w);
        pixel += fb->stride;
    }
}

STATIC mp_framebuf_p_t formats[] = {
    [FRAMEBUF_MVLSB] = {mvlsb_setpixel, mvlsb_getpixel, mvlsb_fill_rect},
    [FRAMEBUF_RGB565] = {rgb565_setpixel, rgb565_getpixel, rgb565_fill_rect},
    [FRAMEBUF_GS2_HMSB] = {gs2_hmsb_setpixel, gs2_hmsb_getpixel, gs2_hmsb_fill_rect},
    [FRAMEBUF_GS4_HMSB] = {gs4_hmsb_setpixel, gs4_hmsb_getpixel, gs4_hmsb_fill_rect},
    [FRAMEBUF_PL8] = {gs8_setpixel, gs8_getpixel, gs8_fill_rect},
    [FRAMEBUF_MHLSB] = {mono_horiz_setpixel, mono_horiz_getpixel, mono_horiz_fill_rect},
    [FRAMEBUF_MHMSB] = {mono_horiz_setpixel, mono_horiz_getpixel, mono_horiz_fill_rect},
};

static inline void setpixel(const mp_obj_framebuf_t *fb, int x, int y, uint32_t col) {
    formats[fb->format].setpixel(fb, x, y, col);
}

static inline uint32_t getpixel(const mp_obj_framebuf_t *fb, int x, int y) {
    return formats[fb->format].getpixel(fb, x, y);
}

STATIC void fill_rect(const mp_obj_framebuf_t *fb, int x, int y, int w, int h, uint32_t col) {
    if (h < 1 || w < 1 || x + w <= 0 || y + h <= 0 || y >= fb->height || x >= fb->width) {
        // No operation needed.
        return;
    }

    // clip to the framebuffer
    int xend = MIN(fb->width, x + w);
    int yend = MIN(fb->height, y + h);
    x = MAX(x, 0);
    y = MAX(y, 0);

    formats[fb->format].fill_rect(fb, x, y, xend - x, yend - y, col);
}

STATIC mp_obj_t framebuf_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 4, 5, false);

    mp_obj_framebuf_t *o = m_new_obj(mp_obj_framebuf_t);
    o->base.type = type;
    o->buf_obj = args[0];

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[0], &bufinfo, MP_BUFFER_WRITE);
    o->buf = bufinfo.buf;

    o->width = mp_obj_get_int(args[1]);
    o->height = mp_obj_get_int(args[2]);
    o->format = mp_obj_get_int(args[3]);
    if (n_args >= 5) {
        o->stride = mp_obj_get_int(args[4]);
    } else {
        o->stride = o->width;
    }

    switch (o->format) {
        case FRAMEBUF_MVLSB:
        case FRAMEBUF_RGB565:
            break;
        case FRAMEBUF_MHLSB:
        case FRAMEBUF_MHMSB:
            o->stride = (o->stride + 7) & ~7;
            break;
        case FRAMEBUF_GS2_HMSB:
            o->stride = (o->stride + 3) & ~3;
            break;
        case FRAMEBUF_GS4_HMSB:
            o->stride = (o->stride + 1) & ~1;
            break;
        case FRAMEBUF_PL8:
            break;
        default:
            mp_raise_ValueError("invalid format");
    }

    return MP_OBJ_FROM_PTR(o);
}

STATIC mp_int_t framebuf_get_buffer(mp_obj_t self_in, mp_buffer_info_t *bufinfo, mp_uint_t flags) {
    (void)flags;
    mp_obj_framebuf_t *self = MP_OBJ_TO_PTR(self_in);
    bufinfo->buf = self->buf;
    bufinfo->len = self->stride * self->height * (self->format == FRAMEBUF_RGB565 ? 2 : 1);
    bufinfo->typecode = 'B'; // view framebuf as bytes
    return 0;
}

STATIC mp_obj_t framebuf_fill(mp_obj_t self_in, mp_obj_t col_in) {
    mp_obj_framebuf_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t col = mp_obj_get_int(col_in);
    formats[self->format].fill_rect(self, 0, 0, self->width, self->height, col);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(framebuf_fill_obj, framebuf_fill);

STATIC mp_obj_t framebuf_fill_rect(size_t n_args, const mp_obj_t *args) {
    (void)n_args;

    mp_obj_framebuf_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_int_t x = mp_obj_get_int(args[1]);
    mp_int_t y = mp_obj_get_int(args[2]);
    mp_int_t width = mp_obj_get_int(args[3]);
    mp_int_t height = mp_obj_get_int(args[4]);
    mp_int_t col = mp_obj_get_int(args[5]);

    fill_rect(self, x, y, width, height, col);

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(framebuf_fill_rect_obj, 6, 6, framebuf_fill_rect);

STATIC mp_obj_t framebuf_pixel(size_t n_args, const mp_obj_t *args) {
    mp_obj_framebuf_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_int_t x = mp_obj_get_int(args[1]);
    mp_int_t y = mp_obj_get_int(args[2]);
    if (0 <= x && x < self->width && 0 <= y && y < self->height) {
        if (n_args == 3) {
            // get
            return MP_OBJ_NEW_SMALL_INT(getpixel(self, x, y));
        } else {
            // set
            setpixel(self, x, y, mp_obj_get_int(args[3]));
        }
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(framebuf_pixel_obj, 3, 4, framebuf_pixel);

STATIC mp_obj_t framebuf_hline(size_t n_args, const mp_obj_t *args) {
    (void)n_args;

    mp_obj_framebuf_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_int_t x = mp_obj_get_int(args[1]);
    mp_int_t y = mp_obj_get_int(args[2]);
    mp_int_t w = mp_obj_get_int(args[3]);
    mp_int_t col = mp_obj_get_int(args[4]);

    fill_rect(self, x, y, w, 1, col);

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(framebuf_hline_obj, 5, 5, framebuf_hline);

STATIC mp_obj_t framebuf_vline(size_t n_args, const mp_obj_t *args) {
    (void)n_args;

    mp_obj_framebuf_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_int_t x = mp_obj_get_int(args[1]);
    mp_int_t y = mp_obj_get_int(args[2]);
    mp_int_t h = mp_obj_get_int(args[3]);
    mp_int_t col = mp_obj_get_int(args[4]);

    fill_rect(self, x, y, 1, h, col);

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(framebuf_vline_obj, 5, 5, framebuf_vline);

STATIC mp_obj_t framebuf_rect(size_t n_args, const mp_obj_t *args) {
    (void)n_args;

    mp_obj_framebuf_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_int_t x = mp_obj_get_int(args[1]);
    mp_int_t y = mp_obj_get_int(args[2]);
    mp_int_t w = mp_obj_get_int(args[3]);
    mp_int_t h = mp_obj_get_int(args[4]);
    mp_int_t col = mp_obj_get_int(args[5]);

    fill_rect(self, x, y, w, 1, col);
    fill_rect(self, x, y + h- 1, w, 1, col);
    fill_rect(self, x, y, 1, h, col);
    fill_rect(self, x + w- 1, y, 1, h, col);

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(framebuf_rect_obj, 6, 6, framebuf_rect);

static void drawLine(mp_obj_framebuf_t *self, mp_int_t x1, mp_int_t y1, mp_int_t x2, mp_int_t y2, mp_int_t col){

    mp_int_t dx = x2 - x1;
    mp_int_t sx;
    if (dx > 0) {
        sx = 1;
    } else {
        dx = -dx;
        sx = -1;
    }

    mp_int_t dy = y2 - y1;
    mp_int_t sy;
    if (dy > 0) {
        sy = 1;
    } else {
        dy = -dy;
        sy = -1;
    }

    bool steep;
    if (dy > dx) {
        mp_int_t temp;
        temp = x1; x1 = y1; y1 = temp;
        temp = dx; dx = dy; dy = temp;
        temp = sx; sx = sy; sy = temp;
        steep = true;
    } else {
        steep = false;
    }

    mp_int_t e = 2 * dy - dx;
    for (mp_int_t i = 0; i < dx; ++i) {
        if (steep) {
            if (0 <= y1 && y1 < self->width && 0 <= x1 && x1 < self->height) {
                setpixel(self, y1, x1, col);
            }
        } else {
            if (0 <= x1 && x1 < self->width && 0 <= y1 && y1 < self->height) {
                setpixel(self, x1, y1, col);
            }
        }
        while (e >= 0) {
            y1 += sy;
            e -= 2 * dx;
        }
        x1 += sx;
        e += 2 * dy;
    }

    if (0 <= x2 && x2 < self->width && 0 <= y2 && y2 < self->height) {
        setpixel(self, x2, y2, col);
    }
}

STATIC mp_obj_t framebuf_line(size_t n_args, const mp_obj_t *args) {
    (void)n_args;

    mp_obj_framebuf_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_int_t x1 = mp_obj_get_int(args[1]);
    mp_int_t y1 = mp_obj_get_int(args[2]);
    mp_int_t x2 = mp_obj_get_int(args[3]);
    mp_int_t y2 = mp_obj_get_int(args[4]);
    mp_int_t col = mp_obj_get_int(args[5]);
    drawLine(self, x1, y1, x2, y2, col);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(framebuf_line_obj, 6, 6, framebuf_line);

STATIC mp_obj_t framebuf_blit(size_t n_args, const mp_obj_t *args) {
    mp_obj_framebuf_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_obj_framebuf_t *source = MP_OBJ_TO_PTR(args[1]);
    mp_int_t x = mp_obj_get_int(args[2]);
    mp_int_t y = mp_obj_get_int(args[3]);
    mp_int_t key = -1;
    if (n_args > 4) {
        key = mp_obj_get_int(args[4]);
    }

    if (
        (x >= self->width) ||
        (y >= self->height) ||
        (-x >= source->width) ||
        (-y >= source->height)
    ) {
        // Out of bounds, no-op.
        return mp_const_none;
    }

    // Clip.
    int x0 = MAX(0, x);
    int y0 = MAX(0, y);
    int x1 = MAX(0, -x);
    int y1 = MAX(0, -y);
    int x0end = MIN(self->width, x + source->width);
    int y0end = MIN(self->height, y + source->height);

    for (; y0 < y0end; ++y0) {
        int cx1 = x1;
        for (int cx0 = x0; cx0 < x0end; ++cx0) {
            uint32_t col = getpixel(source, cx1, y1);
            if (col != (uint32_t)key) {
                setpixel(self, cx0, y0, col);
            }
            ++cx1;
        }
        ++y1;
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(framebuf_blit_obj, 4, 5, framebuf_blit);

STATIC mp_obj_t framebuf_scroll(mp_obj_t self_in, mp_obj_t xstep_in, mp_obj_t ystep_in) {
    mp_obj_framebuf_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t xstep = mp_obj_get_int(xstep_in);
    mp_int_t ystep = mp_obj_get_int(ystep_in);
    int sx, y, xend, yend, dx, dy;
    if (xstep < 0) {
        sx = 0;
        xend = self->width + xstep;
        dx = 1;
    } else {
        sx = self->width - 1;
        xend = xstep - 1;
        dx = -1;
    }
    if (ystep < 0) {
        y = 0;
        yend = self->height + ystep;
        dy = 1;
    } else {
        y = self->height - 1;
        yend = ystep - 1;
        dy = -1;
    }
    for (; y != yend; y += dy) {
        for (int x = sx; x != xend; x += dx) {
            setpixel(self, x, y, getpixel(self, x - xstep, y - ystep));
        }
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(framebuf_scroll_obj, framebuf_scroll);

STATIC mp_obj_t framebuf_text(size_t n_args, const mp_obj_t *args) {
    // extract arguments
    mp_obj_framebuf_t *self = MP_OBJ_TO_PTR(args[0]);
    const char *str = mp_obj_str_get_str(args[1]);
    mp_int_t x0 = mp_obj_get_int(args[2]);
    mp_int_t y0 = mp_obj_get_int(args[3]);
    mp_int_t col = 1;
    if (n_args >= 5) {
        col = mp_obj_get_int(args[4]);
    }

    // loop over chars
    for (; *str; ++str) {
        // get char and make sure its in range of font
        int chr = *(uint8_t*)str;
        if (chr < 32 || chr > 127) {
            chr = 127;
        }
        // get char data
        const uint8_t *chr_data = &font_petme128_8x8[(chr - 32) * 8];
        // loop over char data
        for (int j = 0; j < 8; j++, x0++) {
            if (0 <= x0 && x0 < self->width) { // clip x
                uint vline_data = chr_data[j]; // each byte is a column of 8 pixels, LSB at top
                for (int y = y0; vline_data; vline_data >>= 1, y++) { // scan over vertical column
                    if (vline_data & 1) { // only draw if pixel set
                        if (0 <= y && y < self->height) { // clip y
                            setpixel(self, x0, y, col);
                        }
                    }
                }
            }
        }
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(framebuf_text_obj, 4, 5, framebuf_text);

extern fs_user_mount_t fs_user_mount_flash;
STATIC mp_obj_t framebuf_loadbmp(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    mp_obj_framebuf_t *self = MP_OBJ_TO_PTR(args[0]);
    const char *filename = mp_obj_str_get_str(args[1]);
    mp_int_t x0 = 0;
    mp_int_t y0 = 0;
    if (n_args > 2){
        x0 = mp_obj_get_int(args[2]);
        y0 = mp_obj_get_int(args[3]);
    }

    
    fs_user_mount_t *vfs_fat = &fs_user_mount_flash;
    uint8_t res;
    UINT br;
    uint8_t rgb ,color_byte;
    uint32_t color;
    uint16_t x, y;
    uint16_t count;
    FIL fp;
    uint8_t * databuf;
    uint16_t readlen=BMP_DBUF_SIZE;
    uint32_t imgWidth, imgHeight;

    uint16_t countpix=0;//记录像素 
    //x,y的实际坐标	
    //uint16_t  realx=0;
    //uint16_t realy=0;
    //uint8_t  yok=1;			   

    uint8_t *bmpbuf;
    // uint8_t biCompression=0;

    uint16_t rowlen;
    BITMAPINFO *pbmp;

    databuf = (uint8_t*)m_malloc(readlen);
    res = f_open(&vfs_fat->fatfs, &fp, filename, FA_READ);
    if (res == 0){
        res = f_read(&fp, databuf, readlen, &br);
        
        pbmp=(BITMAPINFO*)databuf;
        count=pbmp->bmfHeader.bfOffBits;        	//数据偏移,得到数据段的开始地址
        color_byte=pbmp->bmiHeader.biBitCount/8;	//彩色位 16/24/32  
        // biCompression=pbmp->bmiHeader.biCompression;//压缩方式
        imgWidth = pbmp->bmiHeader.biWidth;
        imgHeight = pbmp->bmiHeader.biHeight;

        // printf("bmp %d %d %ld %ld\n", biCompression, color_byte, pbmp->bmiHeader.biHeight, pbmp->bmiHeader.biWidth);

        //开始解码BMP   
        rowlen = imgWidth * color_byte;
        color=0;//颜色清空
        x=0;
        y=imgHeight;
        rgb=0;
        //对于尺寸小于等于设定尺寸的图片,进行快速解码
        //realy=(y*picinfo.Div_Fac)>>13;
        bmpbuf=databuf;

        if (color_byte!=3 && color_byte!=4){
            printf("only support 24/32 bit bmp\r\n");
        }

        while(1){
            while(count<readlen){  //读取一簇1024扇区 (SectorsPerClust 每簇扇区数)
                if(color_byte==3){   //24位颜色图
                    switch (rgb){
                        case 0:				  
                            color=bmpbuf[count]; //B
                            break ;	   
                        case 1: 	 
                            color+=((uint16_t)bmpbuf[count]<<8);//G
                            break;	  
                        case 2 : 
                            color+=((uint32_t)bmpbuf[count]<<16);//R	  
                            break ;			
                    }
                } else if(color_byte==4){//32位颜色图
					switch (rgb){
						case 0:				  
							color=bmpbuf[count]; //B
							break ;	   
						case 1: 	 
							color+=((uint16_t)bmpbuf[count]<<8);//G
							break;	  
						case 2 : 
							color+=((uint32_t)bmpbuf[count]<<16);//R	  
							break ;			
						case 3 :
							//alphabend=bmpbuf[count];//不读取  ALPHA通道
							break ;  		  	 
					}	
				}
                rgb++;	  
                count++;
                if(rgb==color_byte){ //水平方向读取到1像素数数据后显示	
                    if(x < imgWidth){	
                        setpixel(self, x0 + x, y0 + y, color);
                    }
                    x++;//x轴增加一个像素
                    color=0x00;
                    rgb=0;
                }
                countpix++;//像素累加
                if(countpix>=rowlen){//水平方向像素值到了.换行
                    y--; 
                    if(y==0)break;
                    x=0; 
                    countpix=0;
                    color=0x00;
                    rgb=0;
                }
            }
            res=f_read(&fp, databuf, readlen, &br);//读出readlen个字节
            if(br!=readlen)readlen=br;	//最后一批数据		  
            if(res||br==0)break;		//读取出错
            bmpbuf=databuf;
            count=0;
        }
        f_close(&fp);
    }
    m_free(databuf);

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(framebuf_loadbmp_obj, 2, 4, framebuf_loadbmp);

// gif decoder

const uint16_t _aMaskTbl[16] =
{
    0x0000, 0x0001, 0x0003, 0x0007,
    0x000f, 0x001f, 0x003f, 0x007f,
    0x00ff, 0x01ff, 0x03ff, 0x07ff,
    0x0fff, 0x1fff, 0x3fff, 0x7fff,
};	  
const uint8_t _aInterlaceOffset[]={8,8,4,2};
const uint8_t _aInterlaceYPos[]={0,4,2,1};
 
uint8_t gifdecoding=0;//标记GIF正在解码.

uint8_t gif_check_head(FIL *file)
{
    uint8_t gifversion[6];
    uint32_t readed;
    uint8_t res;
    res=f_read(file,gifversion,6,(UINT*)&readed);
    if(res)return 1;	   
    if((gifversion[0]!='G')||(gifversion[1]!='I')||(gifversion[2]!='F')||
    (gifversion[3]!='8')||((gifversion[4]!='7')&&(gifversion[4]!='9'))||
    (gifversion[5]!='a'))return 2;
    else return 0;	
}

uint16_t gif_getrgb565(uint8_t *ctb) 
{
    uint16_t r,g,b;
    r=(ctb[0]>>3)&0X1F;
    g=(ctb[1]>>2)&0X3F;
    b=(ctb[2]>>3)&0X1F;
    return b+(g<<5)+(r<<11);
}

uint8_t gif_readcolortbl(FIL *file,gif89a * gif,uint16_t num)
{
    uint8_t rgb[3];
    uint16_t t;
    uint8_t res;
    uint32_t readed;
    for(t=0;t<num;t++)
    {
        res=f_read(file,rgb,3,(UINT*)&readed);
        if(res)return 1;//读错误
        // riven, no need to 565 for framebuffer
        // gif->colortbl[t]=gif_getrgb565(rgb);
        gif->colortbl[t] = ((uint32_t)rgb[0]<<16) + ((uint32_t)rgb[1]<<8) + (rgb[2]);
        // printf("color %lx\r\n", gif->colortbl[t]);
    }
    return 0;
}

uint8_t gif_getinfo(FIL *file,gif89a * gif)
{
    uint32_t readed;	 
    uint8_t res;   
    res=f_read(file,(uint8_t*)&gif->gifLSD,7,(UINT*)&readed);
    if(res)return 1;
    if(gif->gifLSD.flag&0x80)//存在全局颜色表
    {
        gif->numcolors=2<<(gif->gifLSD.flag&0x07);//得到颜色表大小
        if(gif_readcolortbl(file,gif,gif->numcolors))return 1;//读错误	
    }	   
    return 0;
}

void gif_savegctbl(gif89a* gif)
{
    uint16_t i=0;
    for(i=0;i<256;i++)gif->bkpcolortbl[i]=gif->colortbl[i];//保存全局颜色.
}

void gif_recovergctbl(gif89a* gif)
{
    uint16_t i=0;
    for(i=0;i<256;i++)gif->colortbl[i]=gif->bkpcolortbl[i];//恢复全局颜色.
}

void gif_initlzw(gif89a* gif,uint8_t codesize) 
{
    memset((uint8_t *)gif->lzw, 0, sizeof(LZW_INFO));
    gif->lzw->SetCodeSize  = codesize;
    gif->lzw->CodeSize     = codesize + 1;
    gif->lzw->ClearCode    = (1 << codesize);
    gif->lzw->EndCode      = (1 << codesize) + 1;
    gif->lzw->MaxCode      = (1 << codesize) + 2;
    gif->lzw->MaxCodeSize  = (1 << codesize) << 1;
    gif->lzw->ReturnClear  = 1;
    gif->lzw->LastByte     = 2;
    gif->lzw->sp           = gif->lzw->aDecompBuffer;
}

uint16_t gif_getdatablock(FIL *gfile,uint8_t *buf,uint16_t maxnum) 
{
    uint8_t cnt;
    uint32_t readed;
    uint32_t fpos;
    f_read(gfile,&cnt,1,(UINT*)&readed);//得到LZW长度			 
    if(cnt) 
    {
        if (buf)//需要读取 
        {
            if(cnt>maxnum)
            {
                fpos=f_tell(gfile);
                f_lseek(gfile,fpos+cnt);//跳过
                return cnt;//直接不读
            }
            f_read(gfile,buf,cnt,(UINT*)&readed);//得到LZW长度	
        }else 	//直接跳过
        {
            fpos=f_tell(gfile);
            f_lseek(gfile,fpos+cnt);//跳过
        }
    }
    return cnt;
}

uint8_t gif_readextension(FIL *gfile,gif89a* gif, int *pTransIndex,uint8_t *pDisposal)
{
    uint8_t temp;
    uint32_t readed;	 
    uint8_t buf[4];  
    f_read(gfile,&temp,1,(UINT*)&readed);//得到长度		 
    switch(temp)
    {
        case GIF_PLAINTEXT:
        case GIF_APPLICATION:
        case GIF_COMMENT:
            while(gif_getdatablock(gfile,0,256)>0);			//获取数据块
            return 0;
        case GIF_GRAPHICCTL://图形控制扩展块
            if(gif_getdatablock(gfile,buf,4)!=4)return 1;	//图形控制扩展块的长度必须为4 
            gif->delay=(buf[2]<<8)|buf[1];					//得到延时 
            *pDisposal=(buf[0]>>2)&0x7; 	    			//得到处理方法
            if((buf[0]&0x1)!=0)*pTransIndex=buf[3];			//透明色表 
            f_read(gfile,&temp,1,(UINT*)&readed);	 		//得到LZW长度	
            if(temp!=0)return 1;							//读取数据块结束符错误.
            return 0;
    }
    return 1;//错误的数据
}

int gif_getnextcode(FIL *gfile,gif89a* gif) 
{
    int i,j,End;
    long Result;
    if(gif->lzw->ReturnClear)
    {
        //The first code should be a clearcode.
        gif->lzw->ReturnClear=0;
        return gif->lzw->ClearCode;
    }
    End=gif->lzw->CurBit+gif->lzw->CodeSize;
    if(End>=gif->lzw->LastBit)
    {
        int Count;
        if(gif->lzw->GetDone)return-1;//Error 
        gif->lzw->aBuffer[0]=gif->lzw->aBuffer[gif->lzw->LastByte-2];
        gif->lzw->aBuffer[1]=gif->lzw->aBuffer[gif->lzw->LastByte-1];
        if((Count=gif_getdatablock(gfile,&gif->lzw->aBuffer[2],300))==0)gif->lzw->GetDone=1;
        if(Count<0)return -1;//Error 
        gif->lzw->LastByte=2+Count;
        gif->lzw->CurBit=(gif->lzw->CurBit-gif->lzw->LastBit)+16;
        gif->lzw->LastBit=(2+Count)*8;
        End=gif->lzw->CurBit+gif->lzw->CodeSize;
    }
    j=End>>3;
    i=gif->lzw->CurBit>>3;
    if(i==j)Result=(long)gif->lzw->aBuffer[i];
    else if(i+1==j)Result=(long)gif->lzw->aBuffer[i]|((long)gif->lzw->aBuffer[i+1]<<8);
    else Result=(long)gif->lzw->aBuffer[i]|((long)gif->lzw->aBuffer[i+1]<<8)|((long)gif->lzw->aBuffer[i+2]<<16);
    Result=(Result>>(gif->lzw->CurBit&0x7))&_aMaskTbl[gif->lzw->CodeSize];
    gif->lzw->CurBit+=gif->lzw->CodeSize;
    return(int)Result;
}

int gif_getnextbyte(FIL *gfile,gif89a* gif) 
{
    int i,Code,Incode;
    while((Code=gif_getnextcode(gfile,gif))>=0)
    {
        if(Code==gif->lzw->ClearCode)
        {
            //Corrupt GIFs can make this happen  
            if(gif->lzw->ClearCode>=(1<<MAX_NUM_LWZ_BITS))return -1;//Error 
            //Clear the tables 
            memset((uint8_t*)gif->lzw->aCode,0,sizeof(gif->lzw->aCode));
            for(i=0;i<gif->lzw->ClearCode;++i)gif->lzw->aPrefix[i]=i;
            //Calculate the'special codes' independence of the initial code size
            //and initialize the stack pointer 
            gif->lzw->CodeSize=gif->lzw->SetCodeSize+1;
            gif->lzw->MaxCodeSize=gif->lzw->ClearCode<<1;
            gif->lzw->MaxCode=gif->lzw->ClearCode+2;
            gif->lzw->sp=gif->lzw->aDecompBuffer;
            //Read the first code from the stack after clear ingand initializing*/
            do
            {
                gif->lzw->FirstCode=gif_getnextcode(gfile,gif);
            }while(gif->lzw->FirstCode==gif->lzw->ClearCode);
            gif->lzw->OldCode=gif->lzw->FirstCode;
            return gif->lzw->FirstCode;
        }
        if(Code==gif->lzw->EndCode)return -2;//End code
        Incode=Code;
        if(Code>=gif->lzw->MaxCode)
        {
            *(gif->lzw->sp)++=gif->lzw->FirstCode;
            Code=gif->lzw->OldCode;
        }
        while(Code>=gif->lzw->ClearCode)
        {
            *(gif->lzw->sp)++=gif->lzw->aPrefix[Code];
            if(Code==gif->lzw->aCode[Code])return Code;
            if((gif->lzw->sp-gif->lzw->aDecompBuffer)>=sizeof(gif->lzw->aDecompBuffer))return Code;
            Code=gif->lzw->aCode[Code];
        }
        *(gif->lzw->sp)++=gif->lzw->FirstCode=gif->lzw->aPrefix[Code];
        if((Code=gif->lzw->MaxCode)<(1<<MAX_NUM_LWZ_BITS))
        {
            gif->lzw->aCode[Code]=gif->lzw->OldCode;
            gif->lzw->aPrefix[Code]=gif->lzw->FirstCode;
            ++gif->lzw->MaxCode;
            if((gif->lzw->MaxCode>=gif->lzw->MaxCodeSize)&&(gif->lzw->MaxCodeSize<(1<<MAX_NUM_LWZ_BITS)))
            {
                gif->lzw->MaxCodeSize<<=1;
                ++gif->lzw->CodeSize;
            }
        }
        gif->lzw->OldCode=Incode;
        if(gif->lzw->sp>gif->lzw->aDecompBuffer)return *--(gif->lzw->sp);
    }
    return Code;
}

mp_obj_framebuf_t * _fb;

uint8_t gif_dispimage(FIL *gfile,gif89a* gif,uint16_t x0,uint16_t y0,int Transparency, uint8_t Disposal) 
{
    uint32_t readed;	   
    uint8_t lzwlen;
    int Index,OldIndex,XPos,YPos,YCnt,Pass,Interlace,XEnd;
    int Width,Height,Cnt,ColorIndex;
    uint32_t bkcolor;
    uint32_t *pTrans;

    Width=gif->gifISD.width;
    Height=gif->gifISD.height;
    XEnd=Width+x0-1;
    bkcolor=gif->colortbl[gif->gifLSD.bkcindex];
    pTrans=(uint32_t*)gif->colortbl;
    f_read(gfile,&lzwlen,1,(UINT*)&readed);//得到LZW长度	 
    gif_initlzw(gif,lzwlen);//Initialize the LZW stack with the LZW code size 
    Interlace=gif->gifISD.flag&0x40;//是否交织编码
    for(YCnt=0,YPos=y0,Pass=0;YCnt<Height;YCnt++)
    {
        Cnt=0;
        OldIndex=-1;
        for(XPos=x0;XPos<=XEnd;XPos++)
        {
            if(gif->lzw->sp>gif->lzw->aDecompBuffer)Index=*--(gif->lzw->sp);
            else Index=gif_getnextbyte(gfile,gif);	   
            if(Index==-2)return 0;//Endcode     
            if((Index<0)||(Index>=gif->numcolors))
            {
                //IfIndex out of legal range stop decompressing
                return 1;//Error
            }
            //If current index equals old index increment counter
            if((Index==OldIndex)&&(XPos<=XEnd))Cnt++;
            else
            {
                if(Cnt)
                {
                    if(OldIndex!=Transparency)
                    {									    
                        // pic_phy.draw_hline(XPos-Cnt-1,YPos,Cnt+1,*(pTrans+OldIndex));
                        fill_rect(_fb, XPos-Cnt-1,YPos,Cnt+1,1,*(pTrans+OldIndex));

                    }else if(Disposal==2)
                    {
                        // pic_phy.draw_hline(XPos-Cnt-1,YPos,Cnt+1,bkcolor);
                        fill_rect(_fb, XPos-Cnt-1,YPos,Cnt+1,1,*(pTrans+OldIndex));
                    }
                    Cnt=0;
                }else
                {
                    if(OldIndex>=0)
                    {
                        // if(OldIndex!=Transparency)pic_phy.draw_point(XPos-1,YPos,*(pTrans+OldIndex));
                        // else if(Disposal==2)pic_phy.draw_point(XPos-1,YPos,bkcolor); 
                        if (OldIndex!=Transparency){
                            setpixel(_fb, XPos-1,YPos,*(pTrans+OldIndex));
                        }else if(Disposal==2){
                            setpixel(_fb, XPos-1,YPos,bkcolor);
                        }
                    }
                }
            }
            OldIndex=Index;
        }
        if((OldIndex!=Transparency)||(Disposal==2))
        {
            if(OldIndex!=Transparency)ColorIndex=*(pTrans+OldIndex);
            else ColorIndex=bkcolor;
            if(Cnt)
            {
                // pic_phy.draw_hline(XPos-Cnt-1,YPos,Cnt+1,ColorIndex);
                fill_rect(_fb, XPos-Cnt-1,YPos,Cnt+1,1,ColorIndex);
            }else{
                // pic_phy.draw_point(XEnd,YPos,ColorIndex);
                setpixel(_fb, XEnd,YPos,ColorIndex);
            }
        }
        //Adjust YPos if image is interlaced 
        if(Interlace)//交织编码
        {
            YPos+=_aInterlaceOffset[Pass];
            if((YPos-y0)>=Height)
            {
                ++Pass;
                YPos=_aInterlaceYPos[Pass]+y0;
            }
        }else YPos++;	    
    }
    return 0;
}

void gif_clear2bkcolor(uint16_t x,uint16_t y,gif89a* gif,ImageScreenDescriptor pimge)
{
    uint16_t x0,y0,x1,y1;
    uint32_t color=gif->colortbl[gif->gifLSD.bkcindex];
    if(pimge.width==0||pimge.height==0)return;//直接不用清除了,原来没有图像!!
    if(gif->gifISD.yoff>pimge.yoff)
    {
        x0=x+pimge.xoff;
        y0=y+pimge.yoff;
        x1=x+pimge.xoff+pimge.width-1;;
        y1=y+gif->gifISD.yoff-1;
        //if(x0<x1&&y0<y1&&x1<320&&y1<320)pic_phy.fill(x0,y0,x1,y1,color); //设定xy,的范围不能太大.
        if(x0<x1&&y0<y1&&x1<320&&y1<320)fill_rect(_fb,x0,y0,x1,y1,color); //设定xy,的范围不能太大.
    }
    if(gif->gifISD.xoff>pimge.xoff)
    {
        x0=x+pimge.xoff;
        y0=y+pimge.yoff;
        x1=x+gif->gifISD.xoff-1;;
        y1=y+pimge.yoff+pimge.height-1;
        //if(x0<x1&&y0<y1&&x1<320&&y1<320)pic_phy.fill(x0,y0,x1,y1,color);
        if(x0<x1&&y0<y1&&x1<320&&y1<320)fill_rect(_fb,x0,y0,x1,y1,color);
    }
    if((gif->gifISD.yoff+gif->gifISD.height)<(pimge.yoff+pimge.height))
    {
        x0=x+pimge.xoff;
        y0=y+gif->gifISD.yoff+gif->gifISD.height-1;
        x1=x+pimge.xoff+pimge.width-1;;
        y1=y+pimge.yoff+pimge.height-1;
        //if(x0<x1&&y0<y1&&x1<320&&y1<320)pic_phy.fill(x0,y0,x1,y1,color);
        if(x0<x1&&y0<y1&&x1<320&&y1<320)fill_rect(_fb,x0,y0,x1,y1,color);
    }
    if((gif->gifISD.xoff+gif->gifISD.width)<(pimge.xoff+pimge.width))
    {
        x0=x+gif->gifISD.xoff+gif->gifISD.width-1;
        y0=y+pimge.yoff;
        x1=x+pimge.xoff+pimge.width-1;;
        y1=y+pimge.yoff+pimge.height-1;
        //if(x0<x1&&y0<y1&&x1<320&&y1<320)pic_phy.fill(x0,y0,x1,y1,color);
        if(x0<x1&&y0<y1&&x1<320&&y1<320)fill_rect(_fb,x0,y0,x1,y1,color);
    }   
}

uint8_t gif_drawimage(FIL *gfile,gif89a* gif,uint16_t x0,uint16_t y0)
{		  
    uint32_t readed;
    uint8_t res,temp;    
    uint16_t numcolors;
    ImageScreenDescriptor previmg;

    uint8_t Disposal;
    int TransIndex;
    uint8_t Introducer;
    TransIndex=-1;				  
    do
    {
        res=f_read(gfile,&Introducer,1,(UINT*)&readed);//读取一个字节
        if(res)return 1;   
        switch(Introducer)
        {		 
            case GIF_INTRO_IMAGE://图像描述
                previmg.xoff=gif->gifISD.xoff;
                previmg.yoff=gif->gifISD.yoff;
                previmg.width=gif->gifISD.width;
                previmg.height=gif->gifISD.height;

                res=f_read(gfile,(uint8_t*)&gif->gifISD,9,(UINT*)&readed);//读取一个字节
                if(res)return 1;			 
                if(gif->gifISD.flag&0x80)//存在局部颜色表
                {							  
                    gif_savegctbl(gif);//保存全局颜色表
                    numcolors=2<<(gif->gifISD.flag&0X07);//得到局部颜色表大小
                    if(gif_readcolortbl(gfile,gif,numcolors))return 1;//读错误	
                }
                if(Disposal==2)gif_clear2bkcolor(x0,y0,gif,previmg); 
                gif_dispimage(gfile,gif,x0+gif->gifISD.xoff,y0+gif->gifISD.yoff,TransIndex,Disposal);
                while(1)
                {
                    f_read(gfile,&temp,1,(UINT*)&readed);//读取一个字节
                    if(temp==0)break;
                    readed=f_tell(gfile);//还存在块.	
                    if(f_lseek(gfile,readed+temp))break;//继续向后偏移	 
                }
                if(temp!=0)return 1;//Error 
                return 0;
            case GIF_INTRO_TERMINATOR://得到结束符了
                return 2;//代表图像解码完成了.
            case GIF_INTRO_EXTENSION:
                //Read image extension*/
                res=gif_readextension(gfile,gif,&TransIndex,&Disposal);//读取图像扩展块消息
                if(res)return 1;
                break;
            default:
                return 1;
        }
    }while(Introducer!=GIF_INTRO_TERMINATOR);//读到结束符了
    return 0;
}

void gif_quit(void)
{
    gifdecoding=0;
}

STATIC mp_obj_t framebuf_loadgif(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    int res = 0;
    FIL gfile;
    fs_user_mount_t *vfs_fat = &fs_user_mount_flash;

    uint16_t dtime=0;//解码延时
    gif89a *mygif89a;

    _fb = MP_OBJ_TO_PTR(args[0]);
    const char *filename = mp_obj_str_get_str(args[1]);
    mp_obj_t callback = MP_OBJ_TO_PTR(args[2]);
    mp_int_t x = 0;
    mp_int_t y = 0;
    if (n_args > 3){
        x = mp_obj_get_int(args[3]);
        y = mp_obj_get_int(args[4]);
    }
    mygif89a=(gif89a*)m_malloc(sizeof(gif89a));
    if (!mygif89a){
        res = -99;
    }
    mygif89a->lzw=(LZW_INFO*)m_malloc(sizeof(LZW_INFO));
    if(mygif89a->lzw==NULL){
        res = -98;
    }
    if (res != 0){
        printf("mem low %d\r\n", res);
        return mp_const_none;
    }

    res = f_open(&vfs_fat->fatfs, &gfile, filename, FA_READ);
    if(res==0)//打开文件ok
    {
        if(gif_check_head(&gfile))res=-3;
        if(gif_getinfo(&gfile,mygif89a))res=-4;
        /*
        if(mygif89a->gifLSD.width>width||mygif89a->gifLSD.height>height)res=-2;//尺寸太大.
        else
        {
            x=(width-mygif89a->gifLSD.width)/2+x;
            y=(height-mygif89a->gifLSD.height)/2+y;
        }
        */
        gifdecoding=1;
        while(gifdecoding&&res==0)//解码循环
        {	 
            res=gif_drawimage(&gfile,mygif89a,x,y);//显示一张图片
            if(callback != mp_const_none){
                mp_call_function_0(callback);
            }
            if(mygif89a->gifISD.flag&0x80)gif_recovergctbl(mygif89a);//恢复全局颜色表
            if(mygif89a->delay)dtime=mygif89a->delay;
            else dtime=10;//默认延时
            while(dtime--&&gifdecoding)mp_hal_delay_ms(10);//延迟
            if(res==2)
            {
                res=0;
                break;
            }
        }
    }
    f_close(&gfile);
    m_free(mygif89a->lzw);
    m_free(mygif89a); 
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(framebuf_loadgif_obj, 3, 5, framebuf_loadgif);

STATIC mp_obj_t framebuf_circle(size_t n_args, const mp_obj_t *args) {
    mp_obj_framebuf_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_int_t x0 = mp_obj_get_int(args[1]);
    mp_int_t y0 = mp_obj_get_int(args[2]);
    mp_int_t r = mp_obj_get_int(args[3]);
    mp_int_t col = mp_obj_get_int(args[4]);
    mp_int_t fill = 0;
    if (n_args == 6){
        fill = mp_obj_get_int(args[5]);
    }
    // fb, x0, 0, r, color
    int f = 1 - r;
    int ddF_x = 1;
    int ddF_y = -2 * r;
    int x = 0;
	int y = r;
    if (fill){
        fill_rect(self, x0, y0 - r, 1, 2*r + 1, col);
    }
    while (x < y){
        if (f >= 0) {
			y--;
			ddF_y += 2;
			f += ddF_y;
		}
        x++;
		ddF_x += 2;
		f += ddF_x;
        if (fill){
            fill_rect(self, x0 + x, y0 - y, 1, 2*y + 1, col);
            fill_rect(self, x0 + y, y0 - x, 1, 2*x + 1, col);
            fill_rect(self, x0 - x, y0 - y, 1, 2*y + 1, col);
            fill_rect(self, x0 - y, y0 - x, 1, 2*x + 1, col);

        } else {
            setpixel(self, x0 + x, y0 + y, col);
            setpixel(self, x0 - x, y0 + y, col);
            setpixel(self, x0 + x, y0 - y, col);
            setpixel(self, x0 - x, y0 - y, col);
            setpixel(self, x0 + y, y0 + x, col);
            setpixel(self, x0 - y, y0 + x, col);
            setpixel(self, x0 + y, y0 - x, col);
            setpixel(self, x0 - y, y0 - x, col);
        }
    }
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(framebuf_circle_obj, 5, 6, framebuf_circle);

#define swap(a, b) { int16_t t = a; a = b; b = t; }

STATIC mp_obj_t framebuf_traingle(size_t n_args, const mp_obj_t *args) {
    // fb, x0, y0, x1, y1, x2, y2, color, fill?
    mp_obj_framebuf_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_int_t x0 = mp_obj_get_int(args[1]);
    mp_int_t y0 = mp_obj_get_int(args[2]);
    mp_int_t x1 = mp_obj_get_int(args[3]);
    mp_int_t y1 = mp_obj_get_int(args[4]);
    mp_int_t x2 = mp_obj_get_int(args[5]);
    mp_int_t y2 = mp_obj_get_int(args[6]);
    mp_int_t col = mp_obj_get_int(args[7]);
    
    mp_int_t fill = 0;
    if (n_args == 9){
        fill = mp_obj_get_int(args[8]);
    }

    if (fill){
        int16_t a, b, y, last;
        if (y0 > y1) {
            swap(y0, y1); swap(x0, x1);
        }
        if (y1 > y2) {
            swap(y2, y1); swap(x2, x1);
        }
        if (y0 > y1) {
            swap(y0, y1); swap(x0, x1);
        }
        if(y0 == y2) { // Handle awkward all-on-same-line case as its own thing
            a = b = x0;
            if(x1 < a)      a = x1;
            else if(x1 > b) b = x1;
            if(x2 < a)      a = x2;
            else if(x2 > b) b = x2;
            fill_rect(self, a, y0, b-a+1, 1, col);
            return mp_const_none;
        }
        int16_t dx01 = x1 - x0;
        int16_t dy01 = y1 - y0;
        int16_t dx02 = x2 - x0;
        int16_t dy02 = y2 - y0;
        int16_t dx12 = x2 - x1;
        int16_t dy12 = y2 - y1;
        if (dy01 == 0) dy01 = 1;
        if (dy02 == 0) dy02 = 1;
        if (dy12 == 0) dy12 = 1;
        int16_t sa = 0, sb = 0;
        if (y1 == y2){
            last = y1;
        } else {
            last = y1 - 1;
        }
        y = y0;
        for (;y<last+1;y++){
            a = x0 + sa / dy01;
            b = x0 + sb / dy02;
            sa += dx01;
            sb += dx02;
            if(a > b){
                swap(a,b);
            }
            fill_rect(self, a, y, b-a+1, 1, col);
        }
        sa = dx12 * (y - y1);
        sb = dx02 * (y - y0);
        while(y <= y2){
            a = x1 + sa / dy12;
            b = x0 + sb / dy02;
            sa += dx12;
            sb += dx02;
            if (a > b) swap(a, b);
            fill_rect(self, a, y, b-a+1, 1, col);
            y += 1;
        }
    } else {
        drawLine(self, x0, y0, x1, y1, col);
        drawLine(self, x1, y1, x2, y2, col);
        drawLine(self, x2, y2, x0, y0, col);
    }
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(framebuf_traingle_obj, 8, 9, framebuf_traingle);

STATIC const mp_rom_map_elem_t framebuf_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_fill), MP_ROM_PTR(&framebuf_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill_rect), MP_ROM_PTR(&framebuf_fill_rect_obj) },
    { MP_ROM_QSTR(MP_QSTR_pixel), MP_ROM_PTR(&framebuf_pixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_hline), MP_ROM_PTR(&framebuf_hline_obj) },
    { MP_ROM_QSTR(MP_QSTR_vline), MP_ROM_PTR(&framebuf_vline_obj) },
    { MP_ROM_QSTR(MP_QSTR_rect), MP_ROM_PTR(&framebuf_rect_obj) },
    { MP_ROM_QSTR(MP_QSTR_line), MP_ROM_PTR(&framebuf_line_obj) },
    { MP_ROM_QSTR(MP_QSTR_blit), MP_ROM_PTR(&framebuf_blit_obj) },
    { MP_ROM_QSTR(MP_QSTR_scroll), MP_ROM_PTR(&framebuf_scroll_obj) },
    { MP_ROM_QSTR(MP_QSTR_text), MP_ROM_PTR(&framebuf_text_obj) },
    { MP_ROM_QSTR(MP_QSTR_loadbmp), MP_ROM_PTR(&framebuf_loadbmp_obj) },
    { MP_ROM_QSTR(MP_QSTR_loadgif), MP_ROM_PTR(&framebuf_loadgif_obj) },
    { MP_ROM_QSTR(MP_QSTR_circle), MP_ROM_PTR(&framebuf_circle_obj) },
    { MP_ROM_QSTR(MP_QSTR_traingle), MP_ROM_PTR(&framebuf_traingle_obj) },
};
STATIC MP_DEFINE_CONST_DICT(framebuf_locals_dict, framebuf_locals_dict_table);

STATIC const mp_obj_type_t mp_type_framebuf = {
    { &mp_type_type },
    .name = MP_QSTR_FrameBuffer,
    .make_new = framebuf_make_new,
    .buffer_p = { .get_buffer = framebuf_get_buffer },
    .locals_dict = (mp_obj_dict_t*)&framebuf_locals_dict,
};

// this factory function is provided for backwards compatibility with old FrameBuffer1 class
STATIC mp_obj_t legacy_framebuffer1(size_t n_args, const mp_obj_t *args) {
    mp_obj_framebuf_t *o = m_new_obj(mp_obj_framebuf_t);
    o->base.type = &mp_type_framebuf;

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[0], &bufinfo, MP_BUFFER_WRITE);
    o->buf = bufinfo.buf;

    o->width = mp_obj_get_int(args[1]);
    o->height = mp_obj_get_int(args[2]);
    o->format = FRAMEBUF_MVLSB;
    if (n_args >= 4) {
        o->stride = mp_obj_get_int(args[3]);
    } else {
        o->stride = o->width;
    }

    return MP_OBJ_FROM_PTR(o);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(legacy_framebuffer1_obj, 3, 4, legacy_framebuffer1);

STATIC const mp_rom_map_elem_t framebuf_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_framebuf) },
    { MP_ROM_QSTR(MP_QSTR_FrameBuffer), MP_ROM_PTR(&mp_type_framebuf) },
    { MP_ROM_QSTR(MP_QSTR_FrameBuffer1), MP_ROM_PTR(&legacy_framebuffer1_obj) },
    { MP_ROM_QSTR(MP_QSTR_MVLSB), MP_ROM_INT(FRAMEBUF_MVLSB) },
    { MP_ROM_QSTR(MP_QSTR_MONO_VLSB), MP_ROM_INT(FRAMEBUF_MVLSB) },
    { MP_ROM_QSTR(MP_QSTR_RGB565), MP_ROM_INT(FRAMEBUF_RGB565) },
    { MP_ROM_QSTR(MP_QSTR_GS2_HMSB), MP_ROM_INT(FRAMEBUF_GS2_HMSB) },
    { MP_ROM_QSTR(MP_QSTR_GS4_HMSB), MP_ROM_INT(FRAMEBUF_GS4_HMSB) },
    { MP_ROM_QSTR(MP_QSTR_PL8), MP_ROM_INT(FRAMEBUF_PL8) },
    { MP_ROM_QSTR(MP_QSTR_MONO_HLSB), MP_ROM_INT(FRAMEBUF_MHLSB) },
    { MP_ROM_QSTR(MP_QSTR_MONO_HMSB), MP_ROM_INT(FRAMEBUF_MHMSB) },
};

STATIC MP_DEFINE_CONST_DICT(framebuf_module_globals, framebuf_module_globals_table);

const mp_obj_module_t mp_module_framebuf = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&framebuf_module_globals,
};

#endif // MICROPY_PY_FRAMEBUF
