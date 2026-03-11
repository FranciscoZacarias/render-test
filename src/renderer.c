// ============================================================
// renderer2d.c
// ============================================================

// ----------------------------------------------------------------
// Embedded shader sources
// ----------------------------------------------------------------

// Shared vertex shader for all targets.
// Converts pixel-space (top_left, size) -> NDC, y-down convention.
// The color / ID field is passed through as a flat uint so the
// fragment shader can interpret it however it likes.
static const char *R2D_VS =
  "#version 460 core                                                  \n"
  "                                                                   \n"
  "layout(location = 0) in vec2 a_unit_pos;                           \n"    // per-vertex
  "layout(location = 1) in vec2 a_top_left;                           \n"    // per-instance
  "layout(location = 2) in vec2 a_size;                               \n"        // per-instance
  "layout(location = 3) in uint a_color;                              \n"       // per-instance (packed RGBA or ID)
  "                                                                   \n"
  "uniform vec2 u_screen_size;                                        \n"
  "                                                                   \n"
  "flat out uint v_color;                                             \n"
  "                                                                   \n"
  "out                                                                \n"
  "gl_PerVertex                                                       \n"
  "{                                                                  \n"
  "  vec4 gl_Position;                                                \n"
  "};                                                                 \n"
  "void main()                                                        \n"
  "{                                                                  \n"
  "  vec2 pixel_pos = a_top_left + a_unit_pos * a_size;               \n"
  "  vec2 ndc       = (pixel_pos / u_screen_size) * 2.0 - 1.0;        \n"
  "       ndc.y     = -ndc.y;                                         \n"
  "  gl_Position = vec4(ndc, 0.0, 1.0);                               \n"
  "  v_color     = a_color;                                           \n"
  "}                                                                  \n";

// Default fragment shader: unpack the RGBA u32 and output it.
static const char *R2D_FS_Color =
  "#version 460 core                                                  \n"
  "                                                                   \n"
  "flat in  uint v_color;                                             \n"
  "out      vec4 frag_color;                                          \n"
  "                                                                   \n"
  "void main()                                                        \n"
  "{                                                                  \n"
  "  float r = float((v_color >>  0u) & 0xFFu) / 255.0;               \n"
  "  float g = float((v_color >>  8u) & 0xFFu) / 255.0;               \n"
  "  float b = float((v_color >> 16u) & 0xFFu) / 255.0;               \n"
  "  float a = float((v_color >> 24u) & 0xFFu) / 255.0;               \n"
  "  if (a == 0.0) discard;                                           \n"   // transparent pixels leave the FBO untouched
  "  frag_color = vec4(r, g, b, a);                                   \n"
  "}                                                                  \n";

// Picking fragment shader: write the raw u32 ID to a GL_R32UI attachment.
// Transparent pixels (alpha == 0 in the ID encoding) do not overwrite the
// background so they are not selectable.
static const char *R2D_FS_Picking =
  "#version 460 core                                                  \n"
  "                                                                   \n"
  "flat in  uint  v_color;                                            \n"   // this is the object ID, not a color
  "layout(location = 0) out uint frag_id;                             \n"
  "                                                                   \n"
  "void main()                                                        \n"
  "{                                                                  \n"
  "  if (v_color == 0u) discard;                                      \n"   // ID 0 == "no object"
  "  frag_id = v_color;                                               \n"
  "}                                                                  \n";

// Blit vertex shader: full-screen triangle trick, no VBO needed.
static const char *R2D_VS_Blit =
  "#version 460 core                                                  \n"
  "                                                                   \n"
  "out vec2 v_uv;                                                     \n"
  "                                                                   \n"
  "out                                                                \n"
  "gl_PerVertex                                                       \n"
  "{                                                                  \n"
  "  vec4 gl_Position;                                                \n"
  "};                                                                 \n"
  "void main()                                                        \n"
  "{                                                                  \n"
  "  vec2 positions[4] = vec2[4](                                     \n"
  "    vec2(-1.0,  1.0),                                              \n"
  "    vec2( 1.0,  1.0),                                              \n"
  "    vec2( 1.0, -1.0),                                              \n"
  "    vec2(-1.0, -1.0)                                               \n"
  "  );                                                               \n"
  "  vec2 uvs[4] = vec2[4](                                           \n"
  "    vec2(0.0, 1.0),                                                \n"
  "    vec2(1.0, 1.0),                                                \n"
  "    vec2(1.0, 0.0),                                                \n"
  "    vec2(0.0, 0.0)                                                 \n"
  "  );                                                               \n"
  "  gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);            \n"
  "  v_uv        = uvs[gl_VertexID];                                  \n"
  "}                                                                  \n";

// Blit fragment shader: sample the off-screen texture and apply a
// global alpha multiplier so the caller can fade the entire layer.
static const char *R2D_FS_Blit =
  "#version 460 core                                                  \n"
  "                                                                   \n"
  "in  vec2      v_uv;                                                \n"
  "out vec4      frag_color;                                          \n"
  "                                                                   \n"
  "uniform sampler2D u_texture;                                       \n"
  "uniform float     u_alpha;     // global layer alpha (1.0 = opaque)\n"
  "                                                                   \n"
  "void main()                                                        \n"
  "{                                                                  \n"
  "  vec4 c   = texture(u_texture, v_uv);                             \n"
  "  frag_color = vec4(c.rgb, c.a * u_alpha);                         \n"
  "}                                                                  \n";

static const char *R2D_FS_Textured =
  "#version 460 core                                          \n"
  "                                                           \n"
  "flat in  uint      v_color;                                \n"   // texture unit index, not a color
  "in       vec2      v_uv;                                   \n"      // NEW: we need UVs from the VS
  "out      vec4      frag_color;                             \n"
  "                                                           \n"
  // NOTE: We use a sampler2D array so the unit is dynamically indexable.
  // Declare enough units to cover what you'll bind (16 is plenty).
  "uniform sampler2D u_textures[16];                          \n"
  "                                                           \n"
  "void main()                                                \n"
  "{                                                          \n"
  "  frag_color = texture(u_textures[v_color], v_uv);         \n"
  "}                                                          \n";

// Updated vertex shader that also emits UVs (used by the textured pipeline)
static const char *R2D_VS_UV =
  "#version 460 core                                          \n"
  "                                                           \n"
  "layout(location = 0) in vec2 a_unit_pos;                   \n"
  "layout(location = 1) in vec2 a_top_left;                   \n"
  "layout(location = 2) in vec2 a_size;                       \n"
  "layout(location = 3) in uint a_color;                      \n"   // texture unit index
  "                                                           \n"
  "uniform vec2 u_screen_size;                                \n"
  "                                                           \n"
  "flat out uint v_color;                                     \n"
  "out      vec2 v_uv;                                        \n"
  "                                                           \n"
  "out                                                        \n"
  "gl_PerVertex                                               \n"
  "{                                                          \n"
  "  vec4 gl_Position;                                        \n"
  "};                                                         \n"
  "void main()                                                \n"
  "{                                                          \n"
  "  vec2 pixel_pos = a_top_left + a_unit_pos * a_size;       \n"
  "  vec2 ndc       = (pixel_pos / u_screen_size) * 2.0 - 1.0;\n"
  "       ndc.y     = -ndc.y;                                 \n"
  "  gl_Position = vec4(ndc, 0.0, 1.0);                       \n"
  "  v_color     = a_color;                                   \n"
  "  v_uv        = vec2(a_unit_pos.x, 1.0 - a_unit_pos.y);    \n"   // flip V: GL origin is bottom-left
  "}                                                          \n";

// ----------------------------------------------------------------
// _r2d_compile_shader
// ----------------------------------------------------------------

function u32
_r2d_compile_shader(const char *name, const char *source, GLenum type)
{
  u32 program = glCreateShaderProgramv(type, 1, &source);

  GLint ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok)
  {
    GLchar log[1024];
    glGetProgramInfoLog(program, sizeof(log), 0, log);
    Scratch scratch = scratch_begin(0, 0);
    r2d_error(Sf(scratch.arena, "Shader '%s' failed:\n%s", name, log));
    scratch_end(&scratch);
  }

  return program;
}

// ----------------------------------------------------------------
// _r2d_build_pipeline
// ----------------------------------------------------------------

function void
_r2d_build_pipeline(R2D_Pipeline *p, const char *vs_src, const char *fs_src)
{
  p->vertex_program_handle   = _r2d_compile_shader("VS", vs_src, GL_VERTEX_SHADER);
  p->fragment_program_handle = _r2d_compile_shader("FS", fs_src, GL_FRAGMENT_SHADER);
  p->uniforms.screen_size    = glGetUniformLocation(p->vertex_program_handle, "u_screen_size");

  glCreateProgramPipelines(1, &p->pipeline_handle);
  glUseProgramStages(p->pipeline_handle, GL_VERTEX_SHADER_BIT,   p->vertex_program_handle);
  glUseProgramStages(p->pipeline_handle, GL_FRAGMENT_SHADER_BIT, p->fragment_program_handle);
}

// ----------------------------------------------------------------
// r2d_init
// ----------------------------------------------------------------

function void
r2d_init(u32 window_width, u32 window_height)
{
  assert_no_reentry();

  memory_zero_struct(&R2D_RenderContext);
  R2D_RenderContext.arena = arena_alloc();

  // Quad pool
  R2D_RenderContext.quads_capacity = R2D_MAX_QUADS;
  R2D_RenderContext.quads_count    = 0;
  R2D_RenderContext.quads = push_array(R2D_RenderContext.arena, R2D_Quad, R2D_RenderContext.quads_capacity);

  // Pipelines
  _r2d_build_pipeline(&R2D_RenderContext.default_pipeline, R2D_VS, R2D_FS_Color);
  _r2d_build_pipeline(&R2D_RenderContext.picking_pipeline, R2D_VS, R2D_FS_Picking);
  _r2d_build_pipeline(&R2D_RenderContext.textured_pipeline, R2D_VS_UV, R2D_FS_Textured);

  // Blit pipeline stored separately on the context isn't needed –
  // _r2d_blit_to_screen uses its own local program handles (see below).

  // OpenGL state
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  // Geometry buffers
  {
    glCreateBuffers(1, &R2D_RenderContext.unit_vbo);
    glNamedBufferStorage(R2D_RenderContext.unit_vbo, sizeof(R2D_UnitQuadVertices), R2D_UnitQuadVertices, 0);

    glCreateBuffers(1, &R2D_RenderContext.instance_vbo);
    glNamedBufferStorage(R2D_RenderContext.instance_vbo, sizeof(R2D_Quad) * R2D_MAX_QUADS, NULL, GL_DYNAMIC_STORAGE_BIT);

    glCreateBuffers(1, &R2D_RenderContext.ebo);
    glNamedBufferStorage(R2D_RenderContext.ebo, sizeof(R2D_UnitQuadIndices), R2D_UnitQuadIndices, 0);

    glCreateVertexArrays(1, &R2D_RenderContext.vao);
    glVertexArrayElementBuffer(R2D_RenderContext.vao, R2D_RenderContext.ebo);

    // Binding 0 – unit quad positions (per-vertex)
    glVertexArrayVertexBuffer(R2D_RenderContext.vao, 0, R2D_RenderContext.unit_vbo, 0, sizeof(V2f32));

    // Binding 1 – instance data (per-instance)
    glVertexArrayVertexBuffer(R2D_RenderContext.vao, 1, R2D_RenderContext.instance_vbo, 0, sizeof(R2D_Quad));
    glVertexArrayBindingDivisor(R2D_RenderContext.vao, 1, 1);

    // Attrib 0 – a_unit_pos
    glEnableVertexArrayAttrib(R2D_RenderContext.vao, 0);
    glVertexArrayAttribFormat(R2D_RenderContext.vao, 0, 2, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(R2D_RenderContext.vao, 0, 0);

    // Attrib 1 – a_top_left
    glEnableVertexArrayAttrib(R2D_RenderContext.vao, 1);
    glVertexArrayAttribFormat(R2D_RenderContext.vao, 1, 2, GL_FLOAT, GL_FALSE, offsetof(R2D_Quad, top_left));
    glVertexArrayAttribBinding(R2D_RenderContext.vao, 1, 1);

    // Attrib 2 – a_size
    glEnableVertexArrayAttrib(R2D_RenderContext.vao, 2);
    glVertexArrayAttribFormat(R2D_RenderContext.vao, 2, 2, GL_FLOAT, GL_FALSE, offsetof(R2D_Quad, size));
    glVertexArrayAttribBinding(R2D_RenderContext.vao, 2, 1);

    // Attrib 3 – a_color (integer, not normalised)
    glEnableVertexArrayAttrib(R2D_RenderContext.vao, 3);
    glVertexArrayAttribIFormat(R2D_RenderContext.vao, 3, 1, GL_UNSIGNED_INT, offsetof(R2D_Quad, color));
    glVertexArrayAttribBinding(R2D_RenderContext.vao, 3, 1);
  }

  // Screen target (wraps the default framebuffer, never composited)
  {
    R2D_Render_Target *screen = push_struct(R2D_RenderContext.arena, R2D_Render_Target);
    screen->fbo                = 0;
    screen->color_texture      = 0;
    screen->width              = window_width;
    screen->height             = window_height;
    screen->pipeline_override  = NULL;
    screen->clear_flags        = R2D_Clear_Color;
    screen->clear_color        = v4f32(0.0f, 0.0f, 0.0f, 1.0f);
    screen->composite_to_screen = false;

    R2D_RenderContext.screen_target = screen;
    r2d_render_target_register(screen);
    r2d_set_target(screen);
  }
}

// ----------------------------------------------------------------
// r2d_render_target_create
// ----------------------------------------------------------------

function R2D_Render_Target*
r2d_render_target_create(u32 width, u32 height, R2D_Pipeline *pipeline_override, b8 composite_to_screen)
{
  R2D_Render_Target *rt = push_struct(R2D_RenderContext.arena, R2D_Render_Target);
  rt->width               = width;
  rt->height              = height;
  rt->pipeline_override   = pipeline_override;
  rt->clear_flags         = R2D_Clear_Color;
  rt->clear_color         = v4f32(0.0f, 0.0f, 0.0f, 0.0f);
  rt->composite_to_screen = composite_to_screen;

  glGenFramebuffers(1, &rt->fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);

  glGenTextures(1, &rt->color_texture);
  glBindTexture(GL_TEXTURE_2D, rt->color_texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (s32)width, (s32)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt->color_texture, 0);

  assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return rt;
}

// ----------------------------------------------------------------
// r2d_render_target_create_picking
// Uses GL_R32UI so object IDs survive the round-trip without precision loss.
// ----------------------------------------------------------------

function R2D_Render_Target*
r2d_render_target_create_picking(u32 width, u32 height)
{
  R2D_Render_Target *rt = push_struct(R2D_RenderContext.arena, R2D_Render_Target);
  rt->width               = width;
  rt->height              = height;
  rt->pipeline_override   = &R2D_RenderContext.picking_pipeline;
  rt->clear_flags         = R2D_Clear_Color;
  rt->clear_color         = v4f32(0.0f, 0.0f, 0.0f, 0.0f);  // 0 == no object
  rt->composite_to_screen = false;

  glGenFramebuffers(1, &rt->fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);

  glGenTextures(1, &rt->color_texture);
  glBindTexture(GL_TEXTURE_2D, rt->color_texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, (s32)width, (s32)height, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt->color_texture, 0);

  assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return rt;
}

// ----------------------------------------------------------------
// r2d_render_target_register
// Registers a target so end_frame flushes it. Call once after creation.
// ----------------------------------------------------------------

function void
r2d_render_target_register(R2D_Render_Target *rt)
{
  if (R2D_RenderContext.target_count >= R2D_MAX_TARGETS)
  {
    r2d_error(S("Too many render targets. Increase R2D_MAX_TARGETS."));
    return;
  }
  R2D_RenderContext.targets[R2D_RenderContext.target_count++] = rt;
}

// ----------------------------------------------------------------
// r2d_render_target_destroy
// ----------------------------------------------------------------

function void
r2d_render_target_destroy(R2D_Render_Target *rt)
{
  if (rt->fbo == 0)
  {
    r2d_error(S("Cannot destroy the screen target."));
    return;
  }

  if (rt->color_texture) glDeleteTextures(1,     &rt->color_texture);
  if (rt->fbo)           glDeleteFramebuffers(1, &rt->fbo);

  // Remove from the registered list
  for (u32 i = 0; i < R2D_RenderContext.target_count; i += 1)
  {
    if (R2D_RenderContext.targets[i] == rt)
    {
      R2D_RenderContext.targets[i] = R2D_RenderContext.targets[--R2D_RenderContext.target_count];
      break;
    }
  }

  // If this was the active target, fall back to the screen target
  if (R2D_RenderContext.active_target == rt)
  {
    R2D_RenderContext.active_target = R2D_RenderContext.screen_target;
  }

  memory_zero_struct(rt);
}

// ----------------------------------------------------------------
// r2d_render_target_resize
// ----------------------------------------------------------------

function void
r2d_render_target_resize(R2D_Render_Target *rt, u32 new_width, u32 new_height)
{
  if (rt->fbo == 0)
  {
    // Screen target: just update the stored dimensions
    rt->width  = new_width;
    rt->height = new_height;
    return;
  }

  rt->width  = new_width;
  rt->height = new_height;

  glBindTexture(GL_TEXTURE_2D, rt->color_texture);

  // Check whether this is an integer (picking) or RGBA target
  GLint internal_format = GL_RGBA8;
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_format);

  if (internal_format == GL_R32UI)
  {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, (s32)new_width, (s32)new_height, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
  }
  else
  {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (s32)new_width, (s32)new_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  }

  glBindTexture(GL_TEXTURE_2D, 0);
}

// ----------------------------------------------------------------
// r2d_set_target
// Closes the previous target's slice and opens a new one.
// ----------------------------------------------------------------

function void
r2d_set_target(R2D_Render_Target *rt)
{
  // Close the previous target's slice
  if (R2D_RenderContext.active_target != NULL)
  {
    R2D_Render_Target *prev = R2D_RenderContext.active_target;
    prev->quad_count = R2D_RenderContext.quads_count - prev->quad_start;
  }

  rt->quad_start  = R2D_RenderContext.quads_count;
  rt->quad_count  = 0;
  R2D_RenderContext.active_target = rt;
}

// ----------------------------------------------------------------
// r2d_draw_quad
// ----------------------------------------------------------------

function void
r2d_draw_quad(V2f32 top_left, V2f32 size, V4f32 color)
{
  if (R2D_RenderContext.quads_count >= R2D_RenderContext.quads_capacity)
  {
    r2d_error(S("Quad buffer full. Increase R2D_MAX_QUADS."));
    return;
  }

  R2D_Quad *q = &R2D_RenderContext.quads[R2D_RenderContext.quads_count++];
  q->top_left = top_left;
  q->size     = size;
  q->color    = color_pack(color);
}

// ----------------------------------------------------------------
// r2d_draw_quad_id  (for picking targets)
// ----------------------------------------------------------------

function void
r2d_draw_quad_id(V2f32 top_left, V2f32 size, u32 id)
{
  if (R2D_RenderContext.quads_count >= R2D_RenderContext.quads_capacity)
  {
    r2d_error(S("Quad buffer full. Increase R2D_MAX_QUADS."));
    return;
  }

  R2D_Quad *q = &R2D_RenderContext.quads[R2D_RenderContext.quads_count++];
  q->top_left = top_left;
  q->size     = size;
  q->color    = id;  // raw u32, not color-packed
}

function void
r2d_draw_quad_textured(V2f32 top_left, V2f32 size, u32 texture_unit)
{
  if (R2D_RenderContext.quads_count >= R2D_RenderContext.quads_capacity)
  {
    r2d_error(S("Quad buffer full."));
    return;
  }

  R2D_Quad *q = &R2D_RenderContext.quads[R2D_RenderContext.quads_count++];
  q->top_left = top_left;
  q->size     = size;
  q->color    = texture_unit;   // reused as texture unit index
}

// ----------------------------------------------------------------
// r2d_begin_frame
// Resets the quad pool and all target slices.
// ----------------------------------------------------------------

function void
r2d_begin_frame()
{
  R2D_RenderContext.quads_count = 0;

  R2D_RenderContext.per_frame_debug.draw_calls  = 0;
  R2D_RenderContext.per_frame_debug.quads_drawn = 0;

  for (u32 i = 0; i < R2D_RenderContext.target_count; i += 1)
  {
    R2D_Render_Target *rt = R2D_RenderContext.targets[i];
    rt->quad_start = 0;
    rt->quad_count = 0;
  }

  // Re-open the active target's slice from position 0
  if (R2D_RenderContext.active_target != NULL)
  {
    R2D_RenderContext.active_target->quad_start = 0;
  }
}

// ----------------------------------------------------------------
// _r2d_flush_target
// Uploads the target's quad slice and issues one instanced draw.
// ----------------------------------------------------------------

function void
_r2d_flush_target(R2D_Render_Target *rt)
{
  if (rt->quad_count == 0) return;

  R2D_Pipeline *pipeline = (rt->pipeline_override != NULL)
                         ? rt->pipeline_override
                         : &R2D_RenderContext.default_pipeline;

  glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);


  glViewport(0, 0, (s32)rt->width, (s32)rt->height);

  if (rt->clear_flags & R2D_Clear_Color)
  {
    glClearColor(rt->clear_color.x, rt->clear_color.y, rt->clear_color.z, rt->clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT);
  }

  glBindVertexArray(R2D_RenderContext.vao);
  glBindProgramPipeline(pipeline->pipeline_handle);

  glProgramUniform2f(
    pipeline->vertex_program_handle,
    pipeline->uniforms.screen_size,
    (f32)rt->width,
    (f32)rt->height);

  R2D_Quad *slice  = R2D_RenderContext.quads + rt->quad_start;
  u32       count  = rt->quad_count;

  glNamedBufferSubData(R2D_RenderContext.instance_vbo, 0, count * sizeof(R2D_Quad), slice);
  glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0, (s32)count);

  R2D_RenderContext.per_frame_debug.draw_calls  += 1;
  R2D_RenderContext.per_frame_debug.quads_drawn += count;
}

// ----------------------------------------------------------------
// _r2d_blit_to_screen
// Composites one off-screen RGBA target onto the default framebuffer.
// Uses a full-screen quad drawn via gl_VertexID, no extra VBO needed.
// The blit pipeline is compiled once and cached statically.
// ----------------------------------------------------------------

function void
_r2d_blit_to_screen(R2D_Render_Target *rt, u32 window_width, u32 window_height)
{
  // One-time setup via a local static
  local_persist u32 blit_vs      = 0;
  local_persist u32 blit_fs      = 0;
  local_persist u32 blit_pipeline = 0;
  local_persist s32 loc_texture   = -1;
  local_persist s32 loc_alpha     = -1;

  if (blit_pipeline == 0)
  {
    blit_vs = _r2d_compile_shader("Blit VS", R2D_VS_Blit, GL_VERTEX_SHADER);
    blit_fs = _r2d_compile_shader("Blit FS", R2D_FS_Blit, GL_FRAGMENT_SHADER);
    loc_texture = glGetUniformLocation(blit_fs, "u_texture");
    loc_alpha   = glGetUniformLocation(blit_fs, "u_alpha");

    glCreateProgramPipelines(1, &blit_pipeline);
    glUseProgramStages(blit_pipeline, GL_VERTEX_SHADER_BIT,   blit_vs);
    glUseProgramStages(blit_pipeline, GL_FRAGMENT_SHADER_BIT, blit_fs);
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, (s32)window_width, (s32)window_height);

  glBindProgramPipeline(blit_pipeline);

  // Bind the source texture to unit 0
  glBindTextureUnit(0, rt->color_texture);
  glProgramUniform1i(blit_fs, loc_texture, 0);
  glProgramUniform1f(blit_fs, loc_alpha, 1.0f);  // caller can change this

  // No VAO needed – gl_VertexID drives the full-screen quad
  glBindVertexArray(0);
  glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

  R2D_RenderContext.per_frame_debug.draw_calls += 1;
}

// ----------------------------------------------------------------
// r2d_end_frame
// 1. Closes the active target's slice.
// 2. Flushes every registered target in registration order.
// 3. Composites targets flagged composite_to_screen onto fbo 0.
// ----------------------------------------------------------------

function void
r2d_end_frame(u32 window_width, u32 window_height)
{
  // Close the currently active slice
  if (R2D_RenderContext.active_target != NULL)
  {
    R2D_Render_Target *a = R2D_RenderContext.active_target;
    a->quad_count = R2D_RenderContext.quads_count - a->quad_start;
  }

  // Clear the screen target first
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, (s32)window_width, (s32)window_height);
  R2D_Render_Target *screen = R2D_RenderContext.screen_target;
  if (screen->clear_flags & R2D_Clear_Color)
  {
    glClearColor(screen->clear_color.x, screen->clear_color.y, screen->clear_color.z, screen->clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT);
  }

  // Flush each target
  for (u32 i = 0; i < R2D_RenderContext.target_count; i += 1)
  {
    R2D_Render_Target *rt = R2D_RenderContext.targets[i];

    if (rt->fbo == 0)
    {
      // Screen target: flush directly to the default framebuffer
      _r2d_flush_target(rt);
    }
    else
    {
      // Off-screen target: flush to its FBO, then optionally composite
      _r2d_flush_target(rt);

      if (rt->composite_to_screen)
      {
        _r2d_blit_to_screen(rt, window_width, window_height);
      }
    }
  }
}

// ----------------------------------------------------------------
// r2d_picking_read
// Reads back the object ID at (x, y) from a picking render target.
// Call after r2d_end_frame. Returns 0 if nothing was hit.
// ----------------------------------------------------------------

function u32
r2d_picking_read(R2D_Render_Target *picking_rt, u32 x, u32 y)
{
  // Flip y: OpenGL origin is bottom-left, screen coords are top-left
  u32 gl_y = picking_rt->height - 1 - y;

  glBindFramebuffer(GL_READ_FRAMEBUFFER, picking_rt->fbo);
  glReadBuffer(GL_COLOR_ATTACHMENT0);

  u32 id = 0;
  glReadPixels((s32)x, (s32)gl_y, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &id);

  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  return id;
}

// ----------------------------------------------------------------
// Debug error helper
// ----------------------------------------------------------------

#if DEBUG
function void inline
_r2d_error(String message, String file, u32 line)
{
  Scratch scratch = scratch_begin(0, 0);
  String body = Sf(scratch.arena, "R2D Error\n"S_FMT"\n\nat "S_FMT":%u", S_ARG(message), S_ARG(file), line);
  message_box(S("R2D Error"), body, file, line);
  raddbg_break();
  scratch_end(&scratch);
  assert(false);
}
#endif
