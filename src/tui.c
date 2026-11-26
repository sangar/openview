#include "mc/platform/terminal.h"
#include "mc/term/draw.h"
#include "mc/term/input.h"
#include "mc/term/screen.h"
#include "mc/text/utf8.h"

// The live view: a list of listeners in a rounded panel, refreshed every
// second, with a process panel above it on enter, as btop shows a process.

enum {
    REFRESH_MS = 1000,
    DOCKER_REFRESH_MS = 5000,
    KILL_REFRESH_MS = 300,
    STATUS_MS = 4000,
    READ_BUFFER = 256,
    FILTER_MAX = 64,
    NAME_MAX = 64,
    STATUS_MAX = 160,
    WHEEL_ROWS = 3,
    INSPECT_MIN_ROWS = 8,
    MIN_WIDTH = 40,
    MIN_HEIGHT = 8,
    PROCESS_MIN_WIDTH = 8,
};

// Alternate screen, hidden cursor, and mouse clicks and wheel reported in SGR form.
static const char SCREEN_SETUP[] = "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[?1000h\x1b[?1006h";
static const char SCREEN_RESTORE[] = "\x1b[?1006l\x1b[?1000l\x1b[0m\x1b[?25h\x1b[?1049l";

typedef struct Theme {
    Rgb background;
    Rgb border;
    Rgb glow;
    Rgb title;
    Rgb text;
    Rgb muted;
    Rgb key;
    Rgb selection;
    Rgb exposed;
    Rgb local;
    Rgb good;
    Rgb error;
    Rgb container;
} Theme;

static const Theme DARK = {
    .background = { 0x1a, 0x1b, 0x26 },
    .border = { 0x3b, 0x42, 0x61 },
    .glow = { 0x7a, 0xa2, 0xf7 },
    .title = { 0xc0, 0xca, 0xf5 },
    .text = { 0xa9, 0xb1, 0xd6 },
    .muted = { 0x56, 0x5f, 0x89 },
    .key = { 0xff, 0x9e, 0x64 },
    .selection = { 0x28, 0x34, 0x57 },
    .exposed = { 0xe0, 0xaf, 0x68 },
    .local = { 0x7d, 0xcf, 0xff },
    .good = { 0x9e, 0xce, 0x6a },
    .error = { 0xf7, 0x76, 0x8e },
    .container = { 0xbb, 0x9a, 0xf7 },
};

static const Theme LIGHT = {
    .background = { 0xe1, 0xe2, 0xe7 },
    .border = { 0xa8, 0xae, 0xcb },
    .glow = { 0x2e, 0x7d, 0xe9 },
    .title = { 0x34, 0x3b, 0x58 },
    .text = { 0x37, 0x60, 0xbf },
    .muted = { 0x84, 0x8c, 0xb5 },
    .key = { 0xb1, 0x5c, 0x00 },
    .selection = { 0xc4, 0xc8, 0xda },
    .exposed = { 0x8c, 0x6c, 0x3e },
    .local = { 0x00, 0x71, 0x97 },
    .good = { 0x58, 0x75, 0x39 },
    .error = { 0xf5, 0x2a, 0x65 },
    .container = { 0x98, 0x54, 0xf1 },
};

// theme_detect reads the terminal's background from COLORFGBG, "fg;bg" with
// xterm colour numbers, which terminals such as iTerm2 and Konsole set; a
// light background is 7 or 9 to 15. Without it the theme is dark.
static const Theme *theme_detect(Arena *arena)
{
    String value;
    size_t semicolon;
    int64_t background;
    if (!env_get(arena, S("COLORFGBG"), &value) || !str_find_last_char(value, ';', &semicolon) ||
        !str_parse_i64(str_slice(value, semicolon + 1, value.len), &background)) {
        return &DARK;
    }
    return background == 7 || (background >= 9 && background <= 15) ? &LIGHT : &DARK;
}

typedef enum SortKey {
    SORT_PORT,
    SORT_PROTOCOL,
    SORT_ADDRESS,
    SORT_PID,
    SORT_PROCESS,
    SORT_USER,
    SORT_CONNECTIONS,
    SORT_CONTAINER,
    SORT_COUNT,
} SortKey;

typedef struct Column {
    const char *title;
    int width; // 0 takes the width the others leave
    bool right;
} Column;

static const Column COLUMNS[SORT_COUNT] = {
    [SORT_PORT] = { "port", 7, true },     [SORT_PROTOCOL] = { "proto", 7, false },
    [SORT_ADDRESS] = { "address", 24, false }, [SORT_PID] = { "pid", 8, true },
    [SORT_PROCESS] = { "process", 0, false }, [SORT_USER] = { "user", 12, false },
    [SORT_CONNECTIONS] = { "conns", 8, true }, [SORT_CONTAINER] = { "container", 18, false },
};

typedef enum Confirm { CONFIRM_NONE, CONFIRM_TERM, CONFIRM_KILL, CONFIRM_GROUP } Confirm;

// Target is the listener a confirmation is about, copied so that a refresh
// while the question is open cannot change it.
typedef struct Target {
    int pid;
    int group_id;
    int port;
    char name[NAME_MAX];
} Target;

typedef struct App {
    const Theme *theme;
    Terminal *terminal;
    // data holds listeners and rows, and is replaced on each refresh.
    Arena *data;
    Listeners listeners;
    size_t *rows;
    size_t row_count;
    size_t cursor;
    size_t offset;
    Arena *docker;
    ContainerPorts containers;
    bool include_udp;
    SortKey sort;
    bool reverse;
    char filter[FILTER_MAX];
    size_t filter_len;
    bool filtering;
    bool help;
    Confirm confirm;
    Target target;
    // inspect holds the process panel's contents while it is open.
    Arena *inspect;
    bool inspecting;
    int inspect_pid;
    bool inspect_found;
    ProcessInfo info;
    StringList files;
    char status[STATUS_MAX];
    bool status_error;
    int64_t status_until;
    int64_t next_refresh;
    int64_t next_docker;
    int group_id;
    bool quit;
    // Where the last frame put the header and rows, for the mouse.
    int header_y;
    int rows_y;
    int rows_height;
    int column_x[SORT_COUNT];
    int column_width[SORT_COUNT];
} App;

static void set_status(App *app, bool error, String text)
{
    size_t len = text.len < sizeof app->status - 1 ? text.len : sizeof app->status - 1;
    memcpy(app->status, text.data, len);
    app->status[len] = '\0';
    app->status_error = error;
    app->status_until = clock_monotonic_ns() + (int64_t)STATUS_MS * NS_PER_MILLISECOND;
}

static bool has_containers(const App *app)
{
    for (size_t i = 0; i < app->listeners.count; i++) {
        if (app->listeners.items[i].container.len > 0) {
            return true;
        }
    }
    return false;
}

static int compare_numbers(int64_t a, int64_t b)
{
    return (a > b) - (a < b);
}

static int compare_by(SortKey key, const Listener *a, const Listener *b)
{
    switch (key) {
    case SORT_PORT:
        return compare_numbers(a->port, b->port);
    case SORT_PROTOCOL:
        return compare_numbers(a->protocol, b->protocol);
    case SORT_ADDRESS:
        return str_compare(a->host, b->host);
    case SORT_PID:
        return compare_numbers(a->pid, b->pid);
    case SORT_PROCESS:
        return unicode_compare_norwegian_ignore_case(a->name, b->name);
    case SORT_USER:
        return unicode_compare_norwegian_ignore_case(a->user, b->user);
    case SORT_CONNECTIONS:
        return compare_numbers((int64_t)a->connections, (int64_t)b->connections);
    case SORT_CONTAINER:
        return unicode_compare_norwegian_ignore_case(a->container, b->container);
    case SORT_COUNT:
        break;
    }
    return 0;
}

static int compare_rows(const void *context, const void *a, const void *b)
{
    const App *app = context;
    const Listener *left = &app->listeners.items[*(const size_t *)a];
    const Listener *right = &app->listeners.items[*(const size_t *)b];
    int order = compare_by(app->sort, left, right);
    if (order == 0) {
        order = compare_listeners(nullptr, left, right);
    }
    return app->reverse ? -order : order;
}

static const Listener *selected(const App *app)
{
    return app->cursor < app->row_count ? &app->listeners.items[app->rows[app->cursor]] : nullptr;
}

// apply_view filters and sorts the rows, keeping the cursor on the listener
// it was on when that one is still shown.
static void apply_view(App *app, const Listener *keep)
{
    Listener kept = keep != nullptr ? *keep : (Listener) { .pid = -1 };
    app->rows = arena_push(app->data, arena_size_mul(app->listeners.count + 1, sizeof *app->rows));
    app->row_count = 0;
    ArenaMark mark = arena_mark(app->data);
    String filter = unicode_lower(app->data, (String) { app->filter, app->filter_len });
    for (size_t i = 0; i < app->listeners.count; i++) {
        if (listener_matches(app->data, &app->listeners.items[i], filter)) {
            app->rows[app->row_count++] = i;
        }
    }
    arena_release(mark);
    sort_stable(app->data, app->rows, app->row_count, sizeof *app->rows, compare_rows, app);
    for (size_t i = 0; i < app->row_count; i++) {
        const Listener *listener = &app->listeners.items[app->rows[i]];
        if (listener->pid == kept.pid && listener->port == kept.port && listener->protocol == kept.protocol) {
            app->cursor = i;
            return;
        }
    }
    if (app->cursor >= app->row_count) {
        app->cursor = app->row_count > 0 ? app->row_count - 1 : 0;
    }
}

static void load_inspect(App *app)
{
    arena_reset(app->inspect);
    app->info = (ProcessInfo) { 0 };
    app->files = (StringList) { 0 };
    app->inspect_found = app->inspect_pid != 0 &&
                         process_info(app->inspect, app->inspect_pid, &app->info, nullptr) == ERR_OK &&
                         process_open_files(app->inspect, app->inspect_pid, &app->files, nullptr) == ERR_OK;
}

static void refresh(App *app)
{
    int64_t now = clock_monotonic_ns();
    Arena *previous_docker = nullptr;
    if (now >= app->next_docker) {
        previous_docker = app->docker;
        app->docker = arena_create(0);
        app->containers = docker_ports(app->docker);
        app->next_docker = now + (int64_t)DOCKER_REFRESH_MS * NS_PER_MILLISECOND;
    }
    Arena *next = arena_create(0);
    Listeners listeners;
    Err err = { 0 };
    bool collected = listeners_collect(next, app->include_udp, &app->containers, &listeners, &err) == ERR_OK;
    if (!collected) {
        set_status(app, true, S(err.msg));
        arena_destroy(next);
    } else {
        const Listener *current = selected(app);
        Listener keep = current != nullptr ? *current : (Listener) { .pid = -1 };
        arena_destroy(app->data);
        app->data = next;
        app->listeners = listeners;
        apply_view(app, &keep);
    }
    // The listeners on screen name containers from the previous lookup until a collect succeeds.
    if (previous_docker != nullptr && !collected) {
        arena_destroy(app->docker);
        app->docker = previous_docker;
    } else if (previous_docker != nullptr) {
        arena_destroy(previous_docker);
    }
    if (app->inspecting) {
        load_inspect(app);
    }
    app->next_refresh = clock_monotonic_ns() + (int64_t)REFRESH_MS * NS_PER_MILLISECOND;
}

// ---- drawing ----

static void draw_cell(Screen *screen, int x, int y, int width, const Column *column, String text, Rgb fg,
                      uint8_t attrs)
{
    if (width <= 1) {
        return;
    }
    int room = width - 1;
    int used = (int)utf8_width(text);
    int start = column->right && used < room ? x + room - used : x;
    screen_text(screen, start, y, room, text, fg, attrs);
}

static int fixed_width(const bool shown[SORT_COUNT])
{
    int width = 0;
    for (int key = 0; key < SORT_COUNT; key++) {
        width += shown[key] ? COLUMNS[key].width : 0;
    }
    return width;
}

// layout_columns places the columns across width, the process column taking
// what the others leave. The container column shows only when a listener
// has a container; it, the user and the protocol columns give way on narrow
// screens.
static void layout_columns(App *app, int x, int width)
{
    bool shown[SORT_COUNT];
    for (int key = 0; key < SORT_COUNT; key++) {
        shown[key] = true;
    }
    shown[SORT_CONTAINER] = has_containers(app);
    static const SortKey GIVE_WAY[] = { SORT_CONTAINER, SORT_USER, SORT_PROTOCOL };
    for (size_t i = 0; i < countof(GIVE_WAY) && width - fixed_width(shown) < PROCESS_MIN_WIDTH; i++) {
        shown[GIVE_WAY[i]] = false;
    }
    int flexible = width - fixed_width(shown);
    for (int key = 0; key < SORT_COUNT; key++) {
        int column_width = COLUMNS[key].width == 0 ? flexible : COLUMNS[key].width;
        app->column_x[key] = x;
        app->column_width[key] = shown[key] && column_width > 0 ? column_width : 0;
        x += app->column_width[key];
    }
}

static void draw_header(App *app, Screen *screen, int y)
{
    const Theme *theme = app->theme;
    for (int key = 0; key < SORT_COUNT; key++) {
        bool sorted = (SortKey)key == app->sort;
        String title = S(COLUMNS[key].title);
        if (sorted) {
            title = str_format(app->data, "%s%s", COLUMNS[key].title, app->reverse ? "↓" : "↑");
        }
        draw_cell(screen, app->column_x[key], y, app->column_width[key], &COLUMNS[key], title,
                  sorted ? theme->title : theme->muted, sorted ? ATTR_BOLD : 0);
    }
}

static void draw_row(App *app, Screen *screen, int y, const Listener *listener)
{
    const Theme *theme = app->theme;
    Arena *arena = app->data;
    String cells[SORT_COUNT] = {
        [SORT_PORT] = str_format(arena, "%d", listener->port),
        [SORT_PROTOCOL] = S(protocol_name(listener->protocol)),
        [SORT_ADDRESS] = address_text(arena, listener->host, listener->port),
        [SORT_PID] = listener->pid == 0 ? S("-") : str_format(arena, "%d", listener->pid),
        [SORT_PROCESS] = process_text(listener),
        [SORT_USER] = listener->user,
        [SORT_CONNECTIONS] = str_format(arena, "%zu", listener->connections),
        [SORT_CONTAINER] = listener->container,
    };
    Rgb colors[SORT_COUNT] = {
        [SORT_PORT] = theme->title,
        [SORT_PROTOCOL] = theme->muted,
        [SORT_ADDRESS] = listener->exposed ? theme->exposed : theme->local,
        [SORT_PID] = theme->muted,
        [SORT_PROCESS] = listener->pid == 0 ? theme->muted : theme->text,
        [SORT_USER] = theme->muted,
        [SORT_CONNECTIONS] = listener->connections > 0 ? theme->good : theme->muted,
        [SORT_CONTAINER] = theme->container,
    };
    for (int key = 0; key < SORT_COUNT; key++) {
        uint8_t attrs = key == SORT_PORT || key == SORT_PROCESS ? ATTR_BOLD : 0;
        draw_cell(screen, app->column_x[key], y, app->column_width[key], &COLUMNS[key], cells[key], colors[key],
                  attrs);
    }
}

// draw_hints puts "key label" pairs on the bottom edge of a panel, btop style.
static void draw_hints(App *app, Screen *screen, Rect rect)
{
    static const char *const HINTS[][2] = {
        { "k", "term" },  { "K", "kill" }, { "g", "group" }, { "enter", "inspect" }, { "/", "filter" },
        { "u", "udp" },   { "←→", "sort" }, { "?", "help" },  { "q", "quit" },
    };
    const Theme *theme = app->theme;
    int y = rect.y + rect.height - 1;
    int x = rect.x + 2;
    int end = rect.x + rect.width - 2;
    if (app->status[0] != '\0' && clock_monotonic_ns() < app->status_until) {
        screen_text(screen, x, y, end - x, str_format(app->data, " %s ", app->status),
                    app->status_error ? theme->error : theme->good, ATTR_BOLD);
        return;
    }
    for (size_t i = 0; i < countof(HINTS); i++) {
        String key = S(HINTS[i][0]);
        String label = S(HINTS[i][1]);
        int needed = (int)(utf8_width(key) + utf8_width(label)) + 3;
        if (x + needed > end) {
            break;
        }
        bool on = str_equal(key, S("u")) && app->include_udp;
        x += screen_text(screen, x, y, end - x, S(" "), theme->muted, 0);
        x += screen_text(screen, x, y, end - x, key, theme->key, ATTR_BOLD);
        x += screen_text(screen, x, y, end - x, S(" "), theme->muted, 0);
        x += screen_text(screen, x, y, end - x, label, on ? theme->good : theme->muted, on ? ATTR_BOLD : 0);
        x += screen_text(screen, x, y, end - x, S(" "), theme->muted, 0);
    }
}

static Panel panel_of(const Theme *theme, String title, String info)
{
    return (Panel) {
        .title = title,
        .title_color = theme->title,
        .info = info,
        .info_color = theme->muted,
        .border = theme->border,
        .glow = theme->glow,
    };
}

static void draw_empty(App *app, Screen *screen, Rect rows)
{
    String text = app->filter_len > 0 ? str_format(app->data, "nothing matches “%.*s”", (int)app->filter_len, app->filter)
                  : process_user_id() == 0 ? S("nothing is listening")
                                           : S("nothing ov can read is listening; sudo ov shows every process");
    int width = (int)utf8_width(text);
    int x = rows.x + (rows.width > width ? (rows.width - width) / 2 : 0);
    screen_text(screen, x, rows.y + rows.height / 2, rows.width, text, app->theme->muted, 0);
}

static void draw_ports(App *app, Screen *screen, Rect rect)
{
    const Theme *theme = app->theme;
    String info = app->filtering || app->filter_len > 0
                      ? str_format(app->data, "filter: %.*s%s", (int)app->filter_len, app->filter, app->filtering ? "▏" : "")
                      : str_format(app->data, "%zu listening", app->row_count);
    draw_panel(screen, rect, panel_of(theme, app->include_udp ? S("ports · tcp+udp") : S("ports"), info));
    Rect inner = rect_inner(rect);
    inner.x += 1;
    inner.width -= 2;
    layout_columns(app, inner.x, inner.width);
    app->header_y = inner.y;
    app->rows_y = inner.y + 1;
    app->rows_height = inner.height - 1;
    draw_header(app, screen, app->header_y);
    Rect rows = { inner.x, app->rows_y, inner.width, app->rows_height };
    if (app->row_count == 0) {
        draw_empty(app, screen, rows);
    }
    size_t visible = rows.height > 0 ? (size_t)rows.height : 0;
    if (app->cursor < app->offset) {
        app->offset = app->cursor;
    }
    if (visible > 0 && app->cursor >= app->offset + visible) {
        app->offset = app->cursor - visible + 1;
    }
    for (size_t i = 0; i < visible && app->offset + i < app->row_count; i++) {
        int y = rows.y + (int)i;
        if (app->offset + i == app->cursor) {
            screen_paint(screen, (Rect) { rect.x + 1, y, rect.width - 2, 1 }, theme->selection);
        }
        draw_row(app, screen, y, &app->listeners.items[app->rows[app->offset + i]]);
    }
    draw_hints(app, screen, rect);
}

static int draw_field(App *app, Screen *screen, Rect area, int y, const char *label, String value, Rgb color)
{
    if (y >= area.y + area.height) {
        return y;
    }
    screen_text(screen, area.x, y, area.width, S(label), app->theme->muted, 0);
    screen_text(screen, area.x + 10, y, area.width - 10, value, color, 0);
    return y + 1;
}

static void draw_inspect(App *app, Screen *screen, Rect rect)
{
    const Theme *theme = app->theme;
    Arena *arena = app->data;
    String title = app->inspect_found ? str_format(arena, "%.*s · %d", (int)app->info.name.len, app->info.name.data,
                                                   app->inspect_pid)
                                      : S("process");
    draw_panel(screen, rect, panel_of(theme, title, S("esc close")));
    Rect area = rect_inner(rect);
    area.x += 1;
    area.width -= 2;
    int y = area.y;
    if (!app->inspect_found) {
        String text = app->inspect_pid == 0 ? S("ov cannot read this process; sudo ov shows every process")
                                            : str_format(arena, "process %d has exited", app->inspect_pid);
        screen_text(screen, area.x, y, area.width, text, theme->muted, 0);
        return;
    }
    const ProcessInfo *info = &app->info;
    String command = str_join(arena, info->arguments, S(" "));
    StringList ports = { 0 };
    for (size_t i = 0; i < app->listeners.count; i++) {
        const Listener *listener = &app->listeners.items[i];
        if (listener->pid == info->pid) {
            strlist_push(arena, &ports, address_text(arena, listener->host, listener->port));
        }
    }
    y = draw_field(app, screen, area, y, "command", command.len > 0 ? command : info->name, theme->title);
    y = draw_field(app, screen, area, y, "directory", info->working_directory, theme->text);
    y = draw_field(app, screen, area, y, "process",
                   str_format(arena, "pid %d · parent %d · group %d · user %.*s", info->pid, info->parent_pid,
                              info->group_id, (int)user_name(arena, info->user_id).len,
                              user_name(arena, info->user_id).data),
                   theme->text);
    y = draw_field(app, screen, area, y, "ports", str_join(arena, ports, S("  ")), theme->local);
    y = draw_field(app, screen, area, y, "files", str_format(arena, "%zu open", app->files.count), theme->text);
    for (size_t i = 0; i < app->files.count && y < area.y + area.height; i++, y++) {
        screen_text(screen, area.x + 10, y, area.width - 10, app->files.items[i], theme->muted, 0);
    }
}

static Rect centered(Screen *screen, int width, int height)
{
    int screen_w = screen_width(screen);
    int screen_h = screen_height(screen);
    width = width < screen_w - 2 ? width : screen_w - 2;
    height = height < screen_h - 2 ? height : screen_h - 2;
    return (Rect) { (screen_w - width) / 2, (screen_h - height) / 2, width, height };
}

static void draw_dialog(App *app, Screen *screen, Rect rect, String title, String info)
{
    screen_shade(screen, (Rect) { 0, 0, screen_width(screen), screen_height(screen) }, 0.5);
    screen_paint(screen, rect, app->theme->background);
    draw_panel(screen, rect, panel_of(app->theme, title, info));
}

static String confirm_question(const App *app, Arena *arena)
{
    const Target *target = &app->target;
    switch (app->confirm) {
    case CONFIRM_TERM:
        return str_format(arena, "Send SIGTERM to %s (%d)?", target->name, target->pid);
    case CONFIRM_KILL:
        return str_format(arena, "Send SIGKILL to %s (%d)?", target->name, target->pid);
    case CONFIRM_GROUP:
        return str_format(arena, "Send SIGTERM to the group of %s (%d)?", target->name, target->group_id);
    case CONFIRM_NONE:
        break;
    }
    return (String) { 0 };
}

static void draw_confirm(App *app, Screen *screen)
{
    String question = confirm_question(app, app->data);
    Rect rect = centered(screen, (int)utf8_width(question) + 6, 5);
    draw_dialog(app, screen, rect, app->confirm == CONFIRM_KILL ? S("kill") : S("terminate"), S(""));
    Rect inner = rect_inner(rect);
    screen_text(screen, inner.x + 2, inner.y, inner.width - 2, question, app->theme->title, ATTR_BOLD);
    int x = inner.x + 2;
    x += screen_text(screen, x, inner.y + 2, inner.width, S("y"), app->theme->key, ATTR_BOLD);
    x += screen_text(screen, x, inner.y + 2, inner.width, S(" yes   "), app->theme->muted, 0);
    x += screen_text(screen, x, inner.y + 2, inner.width, S("n"), app->theme->key, ATTR_BOLD);
    screen_text(screen, x, inner.y + 2, inner.width, S(" no"), app->theme->muted, 0);
}

static void draw_help(App *app, Screen *screen)
{
    static const char *const KEYS[][2] = {
        { "↑ ↓ pgup pgdn", "move" },
        { "home end", "first, last" },
        { "click, wheel", "select, scroll" },
        { "enter", "inspect the process" },
        { "esc", "close, clear the filter" },
        { "k", "send SIGTERM" },
        { "K", "send SIGKILL" },
        { "g", "send SIGTERM to the process group" },
        { "/", "filter" },
        { "← →  click header", "sort by column" },
        { "r", "reverse the sort" },
        { "u", "show UDP too" },
        { "q  ctrl-c", "quit" },
    };
    Rect rect = centered(screen, 64, (int)countof(KEYS) + 4);
    draw_dialog(app, screen, rect, S("help"), S("any key closes"));
    Rect inner = rect_inner(rect);
    for (size_t i = 0; i < countof(KEYS) && (int)i < inner.height - 1; i++) {
        int y = inner.y + 1 + (int)i;
        screen_text(screen, inner.x + 2, y, 20, S(KEYS[i][0]), app->theme->key, ATTR_BOLD);
        screen_text(screen, inner.x + 22, y, inner.width - 22, S(KEYS[i][1]), app->theme->text, 0);
    }
}

static void draw(App *app, Screen *screen)
{
    int width = screen_width(screen);
    int height = screen_height(screen);
    screen_paint(screen, (Rect) { 0, 0, width, height }, app->theme->background);
    if (width < MIN_WIDTH || height < MIN_HEIGHT) {
        screen_text(screen, 0, 0, width, S("ov needs a larger terminal"), app->theme->muted, 0);
        return;
    }
    int top = 0;
    if (app->inspecting) {
        int inspect_height = height * 2 / 5;
        inspect_height = inspect_height < INSPECT_MIN_ROWS ? INSPECT_MIN_ROWS : inspect_height;
        inspect_height = inspect_height > height - 5 ? height - 5 : inspect_height;
        draw_inspect(app, screen, (Rect) { 0, 0, width, inspect_height });
        top = inspect_height;
    }
    draw_ports(app, screen, (Rect) { 0, top, width, height - top });
    if (app->confirm != CONFIRM_NONE) {
        draw_confirm(app, screen);
    } else if (app->help) {
        draw_help(app, screen);
    }
}

// ---- input ----

static void move_cursor(App *app, int64_t delta)
{
    if (app->row_count == 0) {
        return;
    }
    int64_t cursor = (int64_t)app->cursor + delta;
    int64_t last = (int64_t)app->row_count - 1;
    app->cursor = (size_t)(cursor < 0 ? 0 : cursor > last ? last : cursor);
}

static void sort_by(App *app, SortKey key)
{
    const Listener *current = selected(app);
    Listener keep = current != nullptr ? *current : (Listener) { .pid = -1 };
    if (app->sort == key) {
        app->reverse = !app->reverse;
    } else {
        app->sort = key;
        app->reverse = false;
    }
    apply_view(app, &keep);
}

static void sort_step(App *app, int step)
{
    int key = (int)app->sort;
    for (int tries = 0; tries < SORT_COUNT; tries++) {
        key = (key + step + SORT_COUNT) % SORT_COUNT;
        if (app->column_width[key] > 0) {
            break;
        }
    }
    const Listener *current = selected(app);
    Listener keep = current != nullptr ? *current : (Listener) { .pid = -1 };
    app->sort = (SortKey)key;
    app->reverse = false;
    apply_view(app, &keep);
}

static void ask(App *app, Confirm confirm)
{
    const Listener *listener = selected(app);
    if (listener == nullptr) {
        return;
    }
    if (listener->pid == 0) {
        set_status(app, true,
                   str_format(app->data, "ov cannot read the process on port %d; try sudo ov", listener->port));
        return;
    }
    if (confirm == CONFIRM_GROUP && listener->group_id == app->group_id) {
        set_status(app, true, str_format(app->data, "%.*s is in ov's own process group", (int)listener->name.len,
                                         listener->name.data));
        return;
    }
    app->target = (Target) { .pid = listener->pid, .group_id = listener->group_id, .port = listener->port };
    size_t len = listener->name.len < NAME_MAX - 1 ? listener->name.len : NAME_MAX - 1;
    memcpy(app->target.name, listener->name.data, len);
    app->target.name[len] = '\0';
    app->confirm = confirm;
}

// still_listening reports whether target still holds its port, so that a
// question left open while the process exits cannot signal a reused pid.
static bool still_listening(const App *app, const Target *target)
{
    for (size_t i = 0; i < app->listeners.count; i++) {
        const Listener *listener = &app->listeners.items[i];
        if (listener->pid == target->pid && listener->port == target->port &&
            listener->group_id == target->group_id && str_starts_with(listener->name, S(target->name))) {
            return true;
        }
    }
    return false;
}

static void send_confirmed(App *app)
{
    const Target *target = &app->target;
    refresh(app);
    if (!still_listening(app, target)) {
        set_status(app, true, str_format(app->data, "%s (%d) no longer holds port %d; nothing sent", target->name,
                                         target->pid, target->port));
        app->confirm = CONFIRM_NONE;
        return;
    }
    Signal signal = app->confirm == CONFIRM_KILL ? SIGNAL_KILL : SIGNAL_STOP;
    bool group = app->confirm == CONFIRM_GROUP;
    Listener listener = { .pid = target->pid, .group_id = target->group_id, .name = S(target->name) };
    Err err = { 0 };
    if (signal_listener(app->data, &listener, signal, group, &err) != ERR_OK) {
        set_status(app, true, S(err.msg));
    } else {
        set_status(app, false, signalled_text(app->data, &listener, signal, group));
    }
    app->confirm = CONFIRM_NONE;
    app->next_refresh = clock_monotonic_ns() + (int64_t)KILL_REFRESH_MS * NS_PER_MILLISECOND;
}

static void toggle_inspect(App *app)
{
    const Listener *listener = selected(app);
    if (app->inspecting && (listener == nullptr || listener->pid == app->inspect_pid)) {
        app->inspecting = false;
        return;
    }
    if (listener == nullptr) {
        return;
    }
    app->inspecting = true;
    app->inspect_pid = listener->pid;
    load_inspect(app);
}

static void edit_filter(App *app, Event event)
{
    if (event.key == KEY_ENTER) {
        app->filtering = false;
        return;
    }
    if (event.key == KEY_ESCAPE) {
        app->filtering = false;
        app->filter_len = 0;
    } else if (event.key == KEY_BACKSPACE) {
        while (app->filter_len > 0 && ((unsigned char)app->filter[app->filter_len - 1] & 0xC0) == 0x80) {
            app->filter_len--;
        }
        app->filter_len -= app->filter_len > 0;
    } else if (event.key == KEY_CHAR && !event.ctrl && !event.alt) {
        char bytes[UTF8_MAX_BYTES];
        size_t len = utf8_encode(event.codepoint, bytes);
        if (app->filter_len + len < sizeof app->filter) {
            memcpy(app->filter + app->filter_len, bytes, len);
            app->filter_len += len;
        }
    } else if (event.key == KEY_PASTE) {
        String text = input_paste_text(event.raw);
        size_t room = sizeof app->filter - 1 - app->filter_len;
        size_t len = text.len < room ? text.len : room;
        memcpy(app->filter + app->filter_len, text.data, len);
        app->filter_len += len;
    } else {
        return;
    }
    const Listener *current = selected(app);
    Listener keep = current != nullptr ? *current : (Listener) { .pid = -1 };
    apply_view(app, &keep);
}

static void click(App *app, Event event)
{
    if (event.mouse == MOUSE_WHEEL_UP || event.mouse == MOUSE_WHEEL_DOWN) {
        move_cursor(app, event.mouse == MOUSE_WHEEL_UP ? -WHEEL_ROWS : WHEEL_ROWS);
        return;
    }
    if (event.mouse != MOUSE_PRESS || event.mouse_button != 0) {
        return;
    }
    if (event.y == app->header_y) {
        for (int key = 0; key < SORT_COUNT; key++) {
            if (app->column_width[key] > 0 && event.x >= app->column_x[key] &&
                event.x < app->column_x[key] + app->column_width[key]) {
                sort_by(app, (SortKey)key);
            }
        }
        return;
    }
    if (event.y >= app->rows_y && event.y < app->rows_y + app->rows_height) {
        size_t row = app->offset + (size_t)(event.y - app->rows_y);
        if (row < app->row_count) {
            app->cursor = row;
        }
    }
}

static void handle_key(App *app, Event event)
{
    if (event.key == KEY_CHAR && event.ctrl && event.codepoint == 'c') {
        app->quit = true;
        return;
    }
    if (app->confirm != CONFIRM_NONE) {
        bool yes = event.key == KEY_ENTER || (event.key == KEY_CHAR && (event.codepoint == 'y' || event.codepoint == 'Y'));
        if (yes) {
            send_confirmed(app);
        } else if (event.key != KEY_MOUSE) {
            app->confirm = CONFIRM_NONE;
        }
        return;
    }
    if (app->help) {
        app->help = event.key == KEY_MOUSE;
        return;
    }
    if (app->filtering) {
        edit_filter(app, event);
        return;
    }
    switch (event.key) {
    case KEY_MOUSE:
        click(app, event);
        return;
    case KEY_UP:
        move_cursor(app, -1);
        return;
    case KEY_DOWN:
        move_cursor(app, 1);
        return;
    case KEY_PAGE_UP:
        move_cursor(app, -(int64_t)(app->rows_height > 1 ? app->rows_height - 1 : 1));
        return;
    case KEY_PAGE_DOWN:
        move_cursor(app, app->rows_height > 1 ? app->rows_height - 1 : 1);
        return;
    case KEY_HOME:
        app->cursor = 0;
        return;
    case KEY_END:
        move_cursor(app, (int64_t)app->row_count);
        return;
    case KEY_LEFT:
    case KEY_RIGHT:
        sort_step(app, event.key == KEY_LEFT ? -1 : 1);
        return;
    case KEY_ENTER:
        toggle_inspect(app);
        return;
    case KEY_ESCAPE:
        if (app->inspecting) {
            app->inspecting = false;
        } else if (app->filter_len > 0) {
            edit_filter(app, event);
        }
        return;
    case KEY_CHAR:
        break;
    default:
        return;
    }
    switch (event.codepoint) {
    case 'q':
        app->quit = true;
        return;
    case '/':
        app->filtering = true;
        return;
    case '?':
        app->help = true;
        return;
    case 'r':
        sort_by(app, app->sort);
        return;
    case 'u':
        app->include_udp = !app->include_udp;
        refresh(app);
        return;
    case 'k':
        ask(app, CONFIRM_TERM);
        return;
    case 'K':
        ask(app, CONFIRM_KILL);
        return;
    case 'g':
        ask(app, CONFIRM_GROUP);
        return;
    default:
        return;
    }
}

static void read_events(App *app)
{
    char bytes[READ_BUFFER];
    size_t len = 0;
    unsigned char byte;
    while (len < sizeof bytes && terminal_read_byte(app->terminal, len == 0 ? 0 : 5, &byte)) {
        bytes[len++] = (char)byte;
    }
    size_t at = 0;
    while (at < len && !app->quit) {
        Event event;
        size_t used;
        input_decode((String) { bytes + at, len - at }, true, &event, &used);
        at += used == 0 ? 1 : used;
        handle_key(app, event);
    }
}

[[nodiscard]] static Error run_view(App *app, Screen *screen, Err *err)
{
    Arena *frame = arena_create(0);
    Error error = ERR_OK;
    while (!app->quit) {
        if (clock_monotonic_ns() >= app->next_refresh) {
            refresh(app);
        }
        arena_reset(frame);
        ArenaMark mark = arena_mark(app->data);
        draw(app, screen);
        arena_release(mark);
        StringBuilder out = str_builder_create(frame, 1 << 16);
        screen_render(screen, &out);
        error = terminal_write(app->terminal, str_builder_finish(&out), err);
        if (error != ERR_OK) {
            break;
        }
        int64_t wait_ns = app->next_refresh - clock_monotonic_ns();
        TerminalReady ready = terminal_wait(app->terminal, wait_ns > 0 ? (int)(wait_ns / NS_PER_MILLISECOND) + 1 : 0);
        if (ready.resized) {
            TerminalSize size = terminal_size(app->terminal);
            screen_resize(screen, size.columns, size.rows);
        }
        if (ready.input) {
            read_events(app);
        }
    }
    arena_destroy(frame);
    return error;
}

[[nodiscard]] static Error tui_run(bool include_udp, Err *err)
{
    App app = {
        .data = arena_create(0),
        .docker = arena_create(0),
        .inspect = arena_create(0),
        .include_udp = include_udp,
    };
    app.theme = theme_detect(app.docker);
    ProcessInfo self;
    if (process_info(app.docker, process_id(), &self, nullptr) == ERR_OK) {
        app.group_id = self.group_id;
    }
    Error error = terminal_open(&app.terminal, err);
    if (error == ERR_OK) {
        Screen *screen = screen_create(color_depth_detect(app.docker, true));
        TerminalSize size = terminal_size(app.terminal);
        screen_resize(screen, size.columns, size.rows);
        error = terminal_write(app.terminal, S(SCREEN_SETUP), err);
        if (error == ERR_OK) {
            error = run_view(&app, screen, err);
        }
        (void)terminal_write(app.terminal, S(SCREEN_RESTORE), nullptr);
        screen_destroy(screen);
        terminal_close(app.terminal);
    }
    arena_destroy(app.inspect);
    arena_destroy(app.docker);
    arena_destroy(app.data);
    return error;
}
