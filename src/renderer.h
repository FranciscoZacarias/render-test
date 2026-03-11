#ifndef RENDERER2D_H
#define RENDERER2D_H

// ============================================================
// renderer2d.h
//
// Instanced OpenGL 4.6 quad renderer with multiple render targets.
//
// Core design:
//   - All quads live in one flat CPU array.
//   - Each render target records a (start, count) slice of that array.
//   - r2d_set_target() switches which target receives new quads.
//   - r2d_end_frame() iterates every registered target in registration
//     order and flushes its slice.
//   - Each target can override the pipeline (e.g. picking uses a
//     different fragment shader and a GL_R32UI fbo).
//   - Targets flagged composite_to_screen are blitted onto the
//     default framebuffer at the very end of end_frame.
// ============================================================

#define R2D_MAX_QUADS   kilobytes(32)
#define R2D_MAX_TARGETS 16

// ----------------------------------------------------------------
// Quad  (GPU instance data)
// ----------------------------------------------------------------

typedef struct R2D_Quad R2D_Quad;
struct R2D_Quad
{
  V2f32 top_left;  //  8 bytes
  V2f32 size;      //  8 bytes
  u32   color;     //  4 bytes  packed RGBA  (or object-ID for picking)
  u32   _pad[3];   // 12 bytes  keeps struct at 32 bytes
};

// ----------------------------------------------------------------
// Pipeline  (separable program pipeline)
// ----------------------------------------------------------------

typedef struct R2D_Pipeline R2D_Pipeline;
struct R2D_Pipeline
{
  u32 pipeline_handle;
  u32 vertex_program_handle;
  u32 fragment_program_handle;

  struct
  {
    s32 screen_size;  // vec2 – pixel dimensions of the active target
  } uniforms;
};

// ----------------------------------------------------------------
// Render Target
// ----------------------------------------------------------------

typedef enum R2D_Clear_Flags R2D_Clear_Flags;
enum R2D_Clear_Flags
{
  R2D_Clear_None  = 0,
  R2D_Clear_Color = (1 << 0),
};

typedef struct R2D_Render_Target R2D_Render_Target;
struct R2D_Render_Target
{
  // GPU objects
  GLuint fbo;
  GLuint color_texture;   // 0 for the screen target
  u32    width;
  u32    height;

  // Per-frame quad slice (filled during submission, consumed at end_frame)
  u32 quad_start;         // index into R2D_Context.quads[]
  u32 quad_count;

  // Optional pipeline override. NULL = use R2D_Context.default_pipeline.
  R2D_Pipeline *pipeline_override;

  // Clear behaviour
  R2D_Clear_Flags clear_flags;
  V4f32           clear_color;

  // When true, this target's texture is composited onto the screen at
  // end_frame. Ignored for the screen target itself (fbo == 0).
  b8 composite_to_screen;
};

// ----------------------------------------------------------------
// Context
// ----------------------------------------------------------------

typedef struct R2D_Context R2D_Context;
struct R2D_Context
{
  Arena *arena;

  // Built-in pipelines
  R2D_Pipeline default_pipeline;  // RGBA color quad
  R2D_Pipeline picking_pipeline;  // writes object-ID to GL_R32UI fbo
  R2D_Pipeline textured_pipeline; 

  // GPU geometry (shared by all targets)
  u32 vao;
  u32 ebo;
  u32 unit_vbo;      // static: 4 unit-quad positions
  u32 instance_vbo;  // dynamic: per-instance data, re-uploaded each frame

  // Single CPU quad pool – targets slice into this
  R2D_Quad *quads;
  u32       quads_count;
  u32       quads_capacity;

  // Registered targets (flushed in registration order)
  R2D_Render_Target *targets[R2D_MAX_TARGETS];
  u32                target_count;

  // Currently active target – receives r2d_draw_quad() calls
  R2D_Render_Target *active_target;

  // The screen target (fbo == 0). Registered automatically by r2d_init.
  R2D_Render_Target *screen_target;

  struct
  {
    u32 draw_calls;
    u32 quads_drawn;
  } per_frame_debug;
};

// ----------------------------------------------------------------
// Globals
// ----------------------------------------------------------------

global R2D_Context R2D_RenderContext;

global V2f32 R2D_UnitQuadVertices[] =
{
  { 0.0f, 0.0f },  // top-left
  { 1.0f, 0.0f },  // top-right
  { 1.0f, 1.0f },  // bottom-right
  { 0.0f, 1.0f },  // bottom-left
};
global u16 R2D_UnitQuadIndices[] = { 0, 1, 2, 2, 3, 0 };

// ----------------------------------------------------------------
// Public API
// ----------------------------------------------------------------

// Lifecycle
function void r2d_init(u32 window_width, u32 window_height);
function void r2d_begin_frame();
function void r2d_end_frame(u32 window_width, u32 window_height);

// Target management
function R2D_Render_Target* r2d_render_target_create(u32 width, u32 height, R2D_Pipeline *pipeline_override, b8 composite_to_screen);
function R2D_Render_Target* r2d_render_target_create_picking(u32 width, u32 height);
function void               r2d_render_target_register(R2D_Render_Target *rt);
function void               r2d_render_target_destroy(R2D_Render_Target *rt);
function void               r2d_render_target_resize(R2D_Render_Target *rt, u32 new_width, u32 new_height);

// Submission
function void r2d_set_target(R2D_Render_Target *rt);
function void r2d_draw_quad(V2f32 top_left, V2f32 size, V4f32 color);
function void r2d_draw_quad_id(V2f32 top_left, V2f32 size, u32 id);
function void r2d_draw_quad_textured(V2f32 top_left, V2f32 size, u32 texture_unit);

// Picking readback
function u32  r2d_picking_read(R2D_Render_Target *picking_rt, u32 x, u32 y);

// Internal
function void _r2d_flush_target(R2D_Render_Target *rt);
function void _r2d_blit_to_screen(R2D_Render_Target *rt, u32 window_width, u32 window_height);
function u32  _r2d_compile_shader(const char *name, const char *source, GLenum type);
function void _r2d_build_pipeline(R2D_Pipeline *p, const char *vs_src, const char *fs_src);

#if DEBUG
#  define r2d_error(msg) _r2d_error((msg), S(__FILE__), __LINE__)
function void inline _r2d_error(String message, String file, u32 line);
#else
#  define r2d_error(msg)
#endif

#endif // RENDERER2D_H
