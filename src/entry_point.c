#include "Base.h"

#include "OpenGL.h"
#include "Timing.h"
#include "Files.h"
#include "Art.h"

#include "renderer.h"
#include "renderer.c"

global Window *gWindow = NULL;
global Arena *gArena   = NULL;
global b8 gShouldQuit = false;

typedef struct Example_Quad Example_Quad;
struct Example_Quad
{
  V2f32 pos;
  V2f32 size;
  V4f32 color;
  u32   id;      // 0 == no object, so start IDs at 1
  char *name;
};

function void
handle_input()
{
  if (input_is_key_clicked(Keyboard_Key_ESCAPE))
  {
    gShouldQuit = true;
  }
}

function void
example_picking(Window *window, R2D_Render_Target *picking_rt)
{
  local_persist Example_Quad quads[] =
  {
    { { 100, 100 }, { 200, 150 }, { 0.9f, 0.2f, 0.2f, 1.0f }, 1, "Red Box"    },
    { { 400, 200 }, { 180, 180 }, { 0.2f, 0.8f, 0.2f, 1.0f }, 2, "Green Box"  },
    { { 700, 120 }, { 220, 120 }, { 0.2f, 0.4f, 0.9f, 1.0f }, 3, "Blue Box"   },
  };
  u32 quad_count = array_count(quads);

  // -- Submit to screen target (visible colors) --
  r2d_set_target(R2D_RenderContext.screen_target);
  for (u32 i = 0; i < quad_count; i += 1)
  {
    r2d_draw_quad(quads[i].pos, quads[i].size, quads[i].color);
  }

  // -- Submit to picking target (object IDs) --
  // Same positions and sizes as the visual quads. The fragment
  // shader writes the raw u32 ID into the GL_R32UI attachment.
  r2d_set_target(picking_rt);
  for (u32 i = 0; i < quad_count; i += 1)
  {
    r2d_draw_quad_id(quads[i].pos, quads[i].size, quads[i].id);
  }

  // -- After end_frame: read back the pixel under the mouse --
  // (Call this after r2d_end_frame so the picking FBO is fully drawn)
  u32 mx = (u32)WindowContext.input.mouse_current.screen_space.x;
  u32 my = (u32)WindowContext.input.mouse_current.screen_space.y;

  u32 hit_id = r2d_picking_read(picking_rt, mx, my);

  if (hit_id != 0)
  {
    for (u32 i = 0; i < quad_count; i += 1)
    {
      if (quads[i].id == hit_id)
      {
        printf("Hovered: %s (id=%u)\n", quads[i].name, hit_id);
        break;
      }
    }
  }
}

function void
example_render_to_texture(Window *window, R2D_Render_Target *offscreen_rt, R2D_Render_Target *textured_rt)
{
  r2d_set_target(offscreen_rt);
  {
    r2d_draw_quad(v2f32(0, 0), v2f32((f32)offscreen_rt->width, (f32)offscreen_rt->height),
                  v4f32(0.1f, 0.1f, 0.2f, 1.0f));
    r2d_draw_quad(v2f32(20,  20), v2f32(100, 60), v4f32(1.0f, 0.5f, 0.0f, 1.0f));
    r2d_draw_quad(v2f32(140, 80), v2f32(80,  80), v4f32(0.8f, 0.2f, 0.8f, 1.0f));
  }

  u32 texture_unit = 1;
  glBindTextureUnit(texture_unit, offscreen_rt->color_texture);

  r2d_set_target(textured_rt);  // <-- uses textured_pipeline, composites to screen
  {
    r2d_draw_quad_textured(v2f32(50, 400), v2f32(320, 180), texture_unit);
  }
}

function void
entry_point(Command_Line *command_line)
{
  console_attach();
  gWindow = window_create(NULL, S("Renderer test"), 1280, 720, 30, 30);
  opengl_init(gWindow);
  r2d_init(gWindow->width, gWindow->height);

  R2D_Render_Target *picking_rt   = r2d_render_target_create_picking(gWindow->width, gWindow->height);
  R2D_Render_Target *offscreen_rt = r2d_render_target_create(320, 180, NULL, false);
  R2D_Render_Target *textured_rt  = r2d_render_target_create(gWindow->width, gWindow->height, &R2D_RenderContext.textured_pipeline, true);

  // Registration order == flush order.
  // offscreen_rt must be before textured_rt so its texture is ready to sample.
  // picking_rt position doesn't matter since it never composites.
  r2d_render_target_register(offscreen_rt);
  r2d_render_target_register(textured_rt);
  r2d_render_target_register(picking_rt);

  R2D_RenderContext.screen_target->clear_color = v4f32(0.15f, 0.15f, 0.15f, 1.0f);

  while (!gShouldQuit)
  {
    window_update_events();
    handle_input();

    r2d_begin_frame();

    example_picking(gWindow, picking_rt);
    example_render_to_texture(gWindow, offscreen_rt, textured_rt);

    r2d_end_frame(gWindow->width, gWindow->height);

    window_swap_buffers(gWindow);
    clear_temporary_storage();
  }
}