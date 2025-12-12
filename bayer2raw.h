#ifndef _BAYER2RAW_H
#define _BAYER2RAW_H

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_REL.h>
#include <EGL/eglplatform.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>

struct extern_buffer {
  int repaint;
  int fb, rb;
  int dma_fd;
  EGLImageKHR pEGLImage;
};

struct context_bayer2rgb
{
  /* Public */
  int width;
  int height;
  EGLDisplay  display;
  EGLContext  context;
  EGLSurface  surface;

  int re_paint;

  /* Intern */
  /* Colorspace conversion specific attributes */
  GLuint programHandle;
  GLuint bayer_texture;
  GLuint rgb_texture;

  int num_outbuf;
  struct extern_buffer *out_buf;
  int num_inbuf;
  struct extern_buffer *in_buf;
};

/* Required: all public members */
int init_bayer_rgb_conversion(struct context_bayer2rgb *ctx);
int init_bayer_rgb_conversion_extern_output(struct context_bayer2rgb *ctx);

/* Give memory address to create framebuffer store RGB surface data */
int bayer2raw_create_outbuf(struct context_bayer2rgb *ctx,
                            unsigned long address);
/* Give DMABUF FD to create framebuffer store RGB surface data */
int bayer2raw_create_outbuf_dmabuf(struct context_bayer2rgb *ctx,
                                   int dma_fd);
/* Select framebuffer in out_buf list that GLES will render to */
int bayer2raw_attach_render_outbuf(struct context_bayer2rgb *ctx, int index);
/* Release all DMA-BUF input buffer */
void bayer2raw_free_all_inbuf(struct context_bayer2rgb *ctx);
/* Free/release all framebuffer that created by external memory */
void bayer2raw_free_all_outbuf(struct context_bayer2rgb *ctx);
/* Get the bayer to RGB conversion done */
void do_bayer_rgb_conversion(struct context_bayer2rgb *ctx, void *in_buf,
    void *out_buf);
void do_bayer_rgb_conversion_texture(struct context_bayer2rgb *ctx, void *in_buf,
    GLuint *texture);
int do_bayer_rgb_conversion_texture_dmabuf(struct context_bayer2rgb *ctx, int dma_fd,
    GLuint *texture);

#endif // _BAYER2RAW_H
