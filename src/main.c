#include <stdio.h>
#include <string.h>

#include "mc/core/base.h"
#include "mc/platform/platform.h"

#include "ports.c"
#include "cli.c"
#include "tui.c"

static int run(Arena *arena, const Options *options)
{
    if (options->command != COMMAND_SCREEN || !terminal_is_terminal(1)) {
        return run_command(arena, options);
    }
    Err err = { 0 };
    return tui_run(options->udp, &err) == ERR_OK ? 0 : fail(&err);
}

int main(int argc, char **argv)
{
    Arena *arena = arena_create(0);
    StringList args = { 0 };
    for (int i = 1; i < argc; i++) {
        strlist_push(arena, &args, S(argv[i]));
    }
    Options options;
    Err err = { 0 };
    int status;
    if (options_parse(args, &options, &err) != ERR_OK) {
        fprintf(stderr, "ov: %s\n\n%s", err.msg, USAGE);
        status = 2;
    } else {
        status = run(arena, &options);
    }
    arena_destroy(arena);
    return status;
}
