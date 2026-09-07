#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <drm/drm_fourcc.h>

#include "bayer2raw.h"

#define EGLGLES_BAYER2RAW_SHADER_PATH "/usr/share/"

#define PRINT_OGL_ERROR() print_opengl_error(__FILE__, __LINE__)

typedef EGLImageKHR (*EGLCREATEIMAGEKHR)(EGLDisplay dpy, EGLContext ctx,
                                         EGLenum target, EGLClientBuffer buffer,
                                         EGLint* attr_list);
typedef EGLBoolean (*EGLDESTROYIMAGEKHR)(EGLDisplay dpy, EGLImageKHR image);
typedef void (*PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)(GLenum target,
                                                              GLeglImageOES image);
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) (GLenum target,
                                                     GLeglImageOES image);
int import_extern_outbuf(struct context_bayer2rgb *ctx,
                         struct extern_buffer *buffer);

static EGLCREATEIMAGEKHR eglCreateImageKHR  = 0;
static EGLDESTROYIMAGEKHR eglDestroyImageKHR = 0;
static PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC
         glEGLImageTargetRenderbufferStorage = 0;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC
         glEGLImageTargetTexture2D = 0;

/* opengl screen_vertices for texture mapping */
static const GLfloat screen_vertices[][2] = {
  {-1.0f, -1.0f},
  {1.0f, -1.0f},
  {-1.0f, 1.0f},
  {1.0f, 1.0f}
};

/* vertical flipping of screen_vertices */
static const GLfloat flip_screen_vertices[][2] = {
  {-1.0f, 1.0f},
  {1.0f, 1.0f},
  {-1.0f, -1.0f},
  {1.0f, -1.0f},
};

static const GLfloat texture_coords[][2] = {
  {0.0f, 1.0f},
  {1.0f, 1.0f},
  {0.0f, 0.0f},
  {1.0f, 0.0f}
};

/* For dynamic vertices data*/
static GLfloat *vertices_data = &screen_vertices[0][0];

static int
compile_shader (const char *FName, GLuint shader_handle)
{
  FILE *fptr = NULL;

  if (!FName) {
    printf ("%s: shader file path is NULL\n", __FUNCTION__);
    return -1;
  }

  fptr = fopen (FName, "rb");

  if (fptr == NULL)
    return -1;

  int length;
  fseek (fptr, 0, SEEK_END);
  length = ftell (fptr);
  fseek (fptr, 0, SEEK_SET);

  char *shaderSource = malloc (sizeof (char) * (unsigned int) length);
  if (shaderSource == NULL)
    return -1;
  if (!fread (shaderSource, (size_t) length, 1, fptr))
    return -1;

  glShaderSource (shader_handle, 1, (const char *const *) &shaderSource,
      &length);
  glCompileShader (shader_handle);

  free (shaderSource);
  fclose (fptr);

  GLint compiled = 0;
  glGetShaderiv (shader_handle, GL_COMPILE_STATUS, &compiled);
  if (!compiled) {
    /* Retrieve error buffer size. */
    GLint error_buf_size;
    GLsizei error_length;
    glGetShaderiv (shader_handle, GL_INFO_LOG_LENGTH, &error_buf_size);

    char *info_log = malloc ((unsigned int) error_buf_size * sizeof (char) + 1);
    if (info_log) {
      /* Retrieve error. */
      glGetShaderInfoLog (shader_handle, error_buf_size,
          &error_length, info_log);
      info_log[error_buf_size + 1] = '\0';
      printf ("%s\n", info_log);
      free (info_log);
    }

    printf ("Error compiling shader '%s'\n", FName);
    return -1;
  }

  return 0;
}

static int
load_shaders (const char *file_vertex, const char *file_frag,
    GLuint * program_handle)
{
  GLuint pixel_shader_handle;
  GLuint vert_shader_handle;

  if (!file_vertex || !file_frag || !program_handle) {
    printf ("%s: invalid shader input parameters\n", __FUNCTION__);
    return -1;
  }

  vert_shader_handle = glCreateShader (GL_VERTEX_SHADER);
  pixel_shader_handle = glCreateShader (GL_FRAGMENT_SHADER);

  if (compile_shader (file_vertex, vert_shader_handle) == -1)
    return -1;

  if (compile_shader (file_frag, pixel_shader_handle) == -1)
    return -1;

  *program_handle = glCreateProgram ();
  glAttachShader (*program_handle, vert_shader_handle);
  glAttachShader (*program_handle, pixel_shader_handle);
  glLinkProgram (*program_handle);

  /* Check if linking succeeded. */
  GLint linked = GL_FALSE;
  glGetProgramiv (*program_handle, GL_LINK_STATUS, &linked);
  if (!linked) {
    /* Retrieve error buffer size. */
    GLint errorBufSize, errorLength;
    glGetShaderiv (*program_handle, GL_INFO_LOG_LENGTH, &errorBufSize);

    char *infoLog = malloc ((unsigned int) errorBufSize * sizeof (char) + 1);
    if (infoLog) {
      /* Retrieve error. */
      glGetProgramInfoLog (*program_handle, errorBufSize,
          &errorLength, infoLog);
      infoLog[errorBufSize + 1] = '\0';

      free (infoLog);
    }
    return -1;
  }

  return 0;
}

void
print_opengl_error (const char *file, int line)
{
  GLenum err;

  if (!file) {
    printf ("%s: invalid log file pointer\n", __FUNCTION__);
    return;
  }

  err = glGetError ();

  while (err != GL_NO_ERROR) {
    char error[128];

    switch (err) {
      case GL_INVALID_OPERATION:
        strcpy (error, "INVALID_OPERATION");
        break;
      case GL_INVALID_ENUM:
        strcpy (error, "INVALID_ENUM");
        break;
      case GL_INVALID_VALUE:
        strcpy (error, "INVALID_VALUE");
        break;
      case GL_OUT_OF_MEMORY:
        strcpy (error, "OUT_OF_MEMORY");
        break;
      case GL_INVALID_FRAMEBUFFER_OPERATION:
        strcpy (error, "INVALID_FRAMEBUFFER_OPERATION");
        break;
    }

    printf ("GL_%s - %s:%d\n", error, file, line);
    err = glGetError ();
  }
}

int
init_bayer_rgb_conversion (struct context_bayer2rgb *ctx)
{
  int ret;
  GLint loc_vertices;
  GLint loc_texcoord;

  if (!ctx) {
    printf ("%s: context is NULL\n", __FUNCTION__);
    return -1;
  }

  if (!eglMakeCurrent (ctx->display, ctx->surface, ctx->surface,
      ctx->context)) {
    printf ("[ERROR] : Could not make the current window current !\n");
    return -1;
  }

  /* Prepare pipeline */
  ret = load_shaders (EGLGLES_BAYER2RAW_SHADER_PATH "bayer.vert",
      EGLGLES_BAYER2RAW_SHADER_PATH "bayer.frag", &ctx->programHandle);
  if (ret == -1) {
    printf ("Can not load shaders\n");
    return -1;
  }
  glUseProgram (ctx->programHandle);
  PRINT_OGL_ERROR ();

  /* Grab location of shader attributes. */
  loc_vertices = glGetAttribLocation (ctx->programHandle, "position");
  loc_texcoord = glGetAttribLocation (ctx->programHandle, "uv");
  PRINT_OGL_ERROR ();

  /* Enable vertex arrays to push the data. */
  glEnableVertexAttribArray ((GLuint) loc_vertices);
  glEnableVertexAttribArray ((GLuint) loc_texcoord);
  PRINT_OGL_ERROR ();

  /* Set data in the arrays. */
  glVertexAttribPointer ((GLuint) loc_vertices, 2, GL_FLOAT, GL_FALSE, 0,
      vertices_data);
  glVertexAttribPointer ((GLuint) loc_texcoord, 2, GL_FLOAT, GL_FALSE, 0,
      &texture_coords[0][0]);
  PRINT_OGL_ERROR ();

  /* Set the size inside fragment shader */
  GLint loc_source_size =
      glGetUniformLocation (ctx->programHandle, "sourceSize");
  glUniform4f (loc_source_size, (float) ctx->width, (float) ctx->height,
      (1.0f / (float)ctx->width), 1.0f / ((float) ctx->height));
  PRINT_OGL_ERROR ();

  /* Define the location of the first red pixel in the bayer matrix */
  GLint loc_first_red = glGetUniformLocation (ctx->programHandle, "firstRed");
  glUniform2f (loc_first_red, 1.0f, 1.0f);
  PRINT_OGL_ERROR ();

  /* Generate the output texture */
  glGenTextures (1, &ctx->rgb_texture);
  PRINT_OGL_ERROR ();

  /* Generate the input texture */
  glGenTextures (1, &ctx->bayer_texture);
  glBindTexture (GL_TEXTURE_2D, ctx->bayer_texture);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  PRINT_OGL_ERROR ();

  ctx->re_paint = 0;

  return 0;
}

int
init_bayer_rgb_conversion_extern_output(struct context_bayer2rgb *ctx)
{
  if (!ctx) {
    printf ("%s: context is NULL\n", __FUNCTION__);
    return -1;
  }

  eglCreateImageKHR  = (EGLCREATEIMAGEKHR)
                       eglGetProcAddress("eglCreateImageKHR");
  eglDestroyImageKHR = (EGLDESTROYIMAGEKHR)
                       eglGetProcAddress("eglDestroyImageKHR");
  glEGLImageTargetRenderbufferStorage =
                   (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)
                   eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
  glEGLImageTargetTexture2D = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
                              eglGetProcAddress("glEGLImageTargetTexture2DOES");
  /* When using external output buffer, the read/write memory order is different
   * with internal GLES, so that we need to use the other vertices*/
  vertices_data = &flip_screen_vertices[0][0];
  return init_bayer_rgb_conversion(ctx);
}

static struct extern_buffer *create_eglImage_from_dmabuf(struct context_bayer2rgb *ctx, int dma_fd)
{
  struct extern_buffer *tmp;
  EGLImageKHR pEGLImage;

  if (!ctx) {
    printf ("%s: context is NULL\n", __FUNCTION__);
    return NULL;
  }

  if (dma_fd <= 0) {
    printf("%s: The input FD is invalid \n", __FUNCTION__);
    return NULL;
  }

  for (int i = 0; i < ctx->num_inbuf; i++) {
    if (ctx->in_buf[i].dma_fd == dma_fd)
      return &(ctx->in_buf[i]);
  }

  int attrib[] = {
              EGL_WIDTH, ctx->width,
              EGL_HEIGHT, ctx->height,
              EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8,
              EGL_DMA_BUF_PLANE0_FD_EXT, dma_fd,
              EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
              EGL_DMA_BUF_PLANE0_PITCH_EXT, ctx->width,
              EGL_NONE
    };

  pEGLImage = eglCreateImageKHR(ctx->display, EGL_NO_CONTEXT,
                                          EGL_LINUX_DMA_BUF_EXT, NULL, attrib);
  if (pEGLImage == EGL_NO_IMAGE_KHR) {
    printf("%s: Can not init eglImageKHR. %x\n", __FUNCTION__, eglGetError());
    return NULL;
  }

  tmp = (struct extern_buffer *)realloc(ctx->in_buf,
                                        sizeof(struct extern_buffer) * (ctx->num_inbuf + 1));
  if (!tmp) {
    printf("Out of memory \n");
    eglDestroyImageKHR(ctx->display, pEGLImage);
    return NULL;
  }

  ctx->in_buf = tmp;
  ctx->num_inbuf++;
  ctx->in_buf[ctx->num_inbuf - 1].dma_fd = dma_fd;
  ctx->in_buf[ctx->num_inbuf - 1].pEGLImage = pEGLImage;

  return &(ctx->in_buf[ctx->num_inbuf - 1]);
}

void bayer2raw_free_all_inbuf(struct context_bayer2rgb *ctx)
{
  if (!ctx) {
    printf ("%s: context is NULL\n", __FUNCTION__);
    return;
  }

  for (int i = 0; i < ctx->num_inbuf; i++) {
    if (ctx->in_buf[i].pEGLImage != EGL_NO_IMAGE_KHR)
      eglDestroyImageKHR(ctx->display, ctx->in_buf[i].pEGLImage);
  }

  ctx->num_inbuf = 0;
  if (ctx->in_buf)
    free(ctx->in_buf);
}

int bayer2raw_create_outbuf(struct context_bayer2rgb *ctx,
                            unsigned long address)
{
  if (!ctx) {
    printf ("%s: context is NULL\n", __FUNCTION__);
    return -1;
  }
#if defined(EGLEXT_REL) && EGLEXT_REL
  struct extern_buffer buffer;
  EGLNativePixmapTypeREL  sNativePixmap;
  NativePixmapType eglPixmap;

  sNativePixmap.width    = ctx->width;
  sNativePixmap.height   = ctx->height;
  sNativePixmap.stride   = ctx->width;
  sNativePixmap.usage    = 0;
  sNativePixmap.pixelData    = (void*)address;
  sNativePixmap.format   = EGL_NATIVE_PIXFORMAT_ARGB8888_REL;
  eglPixmap = (NativeWindowType)&sNativePixmap;
  buffer.pEGLImage = eglCreateImageKHR(ctx->display,
                                      EGL_NO_CONTEXT,
                                      EGL_NATIVE_PIXMAP_KHR,
                                      eglPixmap, NULL);


  if (buffer.pEGLImage == EGL_NO_IMAGE_KHR){
    printf("%s: Can not init eglImageKHR. %x\n", __FUNCTION__, eglGetError());
    return -1;
  }

  return import_extern_outbuf(ctx, &buffer);
#else /* Not support */
  return -1;
#endif
}

int bayer2raw_create_outbuf_dmabuf(struct context_bayer2rgb *ctx, int dma_fd)
{
  struct extern_buffer buffer;

  if (!ctx) {
    printf ("%s: context is NULL\n", __FUNCTION__);
    return -1;
  }

  if (dma_fd <= 0) {
    printf("%s: The input FD is invalid \n", __FUNCTION__);
    return -1;
  }

  int attrib[] = {
              EGL_WIDTH, ctx->width,
              EGL_HEIGHT, ctx->height,
              EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
              EGL_DMA_BUF_PLANE0_FD_EXT, dma_fd,
              EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
              EGL_DMA_BUF_PLANE0_PITCH_EXT, ctx->width * 4,
              EGL_NONE
    };

  buffer.pEGLImage = eglCreateImageKHR(ctx->display, EGL_NO_CONTEXT,
                                        EGL_LINUX_DMA_BUF_EXT, NULL, attrib);
  if (buffer.pEGLImage == EGL_NO_IMAGE_KHR){
    printf("%s: Can not init eglImageKHR. %x\n", __FUNCTION__, eglGetError());
    return -1;
  }

  return import_extern_outbuf(ctx, &buffer);
}

int import_extern_outbuf(struct context_bayer2rgb *ctx,
                         struct extern_buffer *buffer)
{
  int index = -1;
  void *tmp;

  if (!ctx || !buffer) {
    printf ("%s: invalid buffer arguments\n", __FUNCTION__);
    return -1;
  }

  glGenFramebuffers(1, (GLuint *)&buffer->fb);
  glGenRenderbuffers(1, (GLuint *)&buffer->rb);

  if (buffer->fb >= 0)
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)buffer->fb);
  else
    goto invalid_fb;
  if (buffer->rb >= 0)
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)buffer->rb);
  else
    goto invalid_rb;

  glEGLImageTargetRenderbufferStorage(GL_RENDERBUFFER, buffer->pEGLImage);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_RENDERBUFFER, (GLuint)buffer->rb);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    printf("Can not init the new framebuffer for external image. %x\n",
             glGetError());
    goto invalid_rb;
  }
  glBindRenderbuffer(GL_RENDERBUFFER, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  index = ctx->num_outbuf;
  /* Extend the list one more member*/
  tmp = (struct extern_buffer *)realloc(ctx->out_buf,
                                        sizeof(struct extern_buffer) *
                                        (long unsigned int)(ctx->num_outbuf + 1));
  if (!tmp) {
    /* If we can't allocate for importing more, deny and use the old list*/
    printf("Out of Memory \n");
    index = -1;
    goto invalid_rb;
  }

  ctx->out_buf = tmp;

  buffer->repaint = 0;

  ctx->out_buf[index] = *buffer;
  ctx->num_outbuf++;
  goto done;

invalid_rb:
  if (buffer->rb > 0) {
    glDeleteRenderbuffers(1, (GLuint *)&buffer->rb);
    buffer->rb = -1;
  }
invalid_fb:
  if (buffer->fb > 0) {
    glDeleteFramebuffers(1, (GLuint *)&buffer->fb);
    buffer->fb = -1;
  }
done:
  return index;
}

int
bayer2raw_attach_render_outbuf(struct context_bayer2rgb *ctx, int index)
{
  if (!ctx) {
    printf ("%s: context is NULL\n", __FUNCTION__);
    return -1;
  }

  if (index < 0)
    return -1;

  glBindFramebuffer( GL_FRAMEBUFFER, (GLuint)ctx->out_buf[index].fb);
  ctx->re_paint = ctx->out_buf[index].repaint;
  if (!ctx->out_buf[index].repaint)
    ctx->out_buf[index].repaint = 1;

  return 0;
}

void
bayer2raw_free_all_outbuf(struct context_bayer2rgb *ctx)
{
  int i;
  struct extern_buffer *buffer;

  if (!ctx) {
    printf ("%s: context is NULL\n", __FUNCTION__);
    return;
  }

  for (i = 0; i < ctx->num_outbuf; i++) {
    buffer = &ctx->out_buf[i];
    if (buffer->pEGLImage != EGL_NO_IMAGE_KHR)
      eglDestroyImageKHR(ctx->display, buffer->pEGLImage);
    if (buffer->rb > 0)
      glDeleteRenderbuffers(1, (GLuint *)&buffer->rb);
    if (buffer->fb > 0)
      glDeleteFramebuffers(1, (GLuint *)&buffer->fb);
  }
  ctx->num_outbuf = 0;
  if (ctx->out_buf)
    free(ctx->out_buf);
}

void
bayer_rgb_conversion(struct context_bayer2rgb *ctx, void *in_buf)
{
  if (!ctx || !in_buf) {
    printf ("%s: invalid conversion input\n", __FUNCTION__);
    return;
  }

  /* Map the bayer buffer */
  glBindTexture (GL_TEXTURE_2D, ctx->bayer_texture);

  if (!ctx->re_paint) {
    glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE, ctx->width, ctx->height,
        0, GL_LUMINANCE, GL_UNSIGNED_BYTE, in_buf);
    /* This step for cover the case that we won't use external output buffer*/
    ctx->re_paint++;
  } else {
    glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, ctx->width, ctx->height,
      GL_LUMINANCE, GL_UNSIGNED_BYTE, in_buf);
  }

  /* Draw on framebufer */
  glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
}

void
do_bayer_rgb_conversion (struct context_bayer2rgb *ctx, void *in_buf,
    void *out_buf)
{
  if (!ctx || !in_buf) {
    printf ("%s: invalid conversion input\n", __FUNCTION__);
    return;
  }

  bayer_rgb_conversion (ctx, in_buf);

  if (out_buf) {
    /* Get back the buffer */
    glReadPixels (0, 0, ctx->width, ctx->height, GL_RGBA,
        GL_UNSIGNED_BYTE, out_buf);
  }
  PRINT_OGL_ERROR ();
}

void
do_bayer_rgb_conversion_texture(struct context_bayer2rgb *ctx, void *in_buf,
    GLuint *texture)
{
  if (!ctx || !in_buf) {
    printf ("%s: invalid conversion input\n", __FUNCTION__);
    return;
  }

  bayer_rgb_conversion (ctx, in_buf);

  if (texture)
    *texture = ctx->rgb_texture;
}

int do_bayer_rgb_conversion_texture_dmabuf(struct context_bayer2rgb *ctx, int dma_fd,
    GLuint *texture)
{
  struct extern_buffer *in_buf = create_eglImage_from_dmabuf(ctx, dma_fd);

  if (!ctx) {
    printf ("%s: context is NULL\n", __FUNCTION__);
    return -1;
  }

  if (!in_buf || in_buf->pEGLImage == EGL_NO_IMAGE_KHR)
    return -1;

  /* Attach EGLImage to texture */
  glBindTexture(GL_TEXTURE_2D, ctx->bayer_texture);
  glEGLImageTargetTexture2D(GL_TEXTURE_2D, in_buf->pEGLImage);

  /* Draw on framebuffer */
  glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);

  if (texture)
    *texture = ctx->rgb_texture;

  return 0;
}