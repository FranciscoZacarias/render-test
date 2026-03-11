#include "Introspection.h"
#include "Code_Generation.h"

#define SRC_PATH "../src"

function void
entry_point(Command_Line* command_line)
{
  console_attach();

  Arena *arena = arena_alloc();
  String path = full_path_from_relative_path(arena, S(SRC_PATH));

  if (command_line->args_count > 0)
  {
    b8 run_introspection = false;
    b8 report_functions  = false;
    b8 report_enums      = false;
    b8 report_scratch    = false;
    b8 run_cgen          = false;
    b8 report_todos      = false;

    for (u32 i = 0; i < command_line->args_count; i += 1)
    {
      Command_Line_Arg arg = command_line->args[i];

      if (string_equals(arg.value, S("functions"), false))
      {
        run_introspection = true;
        report_functions  = true;
      }
      else if (string_equals(arg.value, S("enums"), false))
      {
        run_introspection = true;
        report_enums      = true;
      }
      else if (string_equals(arg.value, S("scratch"), false))
      {
        run_introspection = true;
        report_scratch    = true;
      }
      else if (string_equals(arg.value, S("cgen"), false))
      {
        run_cgen = true;
      }
      else if (string_equals(arg.value, S("todos"), false))
      {
        report_todos = true;
      }
    }

    if (run_introspection)
    {
      intsp_run(path, true);
    }

    if (run_cgen)
    {
      CGen_Context cgen = cgen_run(path);
      cgen_execute_commands(&cgen);
    }

    if (report_todos)
    {

    }
  }
  else
  {
    printf("Help:\n"
      "\nIntrospection:\n"
      "  > '-functions': reports functions\n"
      "  > '-enums': reports enums\n"
      "  > '-scratch': reports scratch arenas\n"
      "\nCode Generation:\n"
      "  > '-cgen': runs code generation module for all .cgen files\n"
    );
  }
}
