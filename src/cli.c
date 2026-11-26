#include <stdio.h>

#include "mc/encoding/json.h"
#include "mc/encoding/node.h"
#include "mc/text/fmt.h"
#include "mc/text/table.h"

static const char USAGE[] = "usage:\n"
                            "  ov [-u]                                   live view of listening ports\n"
                            "  ov :<port>                                who holds port; exits 1 if it is free\n"
                            "  ov ports [-u] [--json]                    list listening ports\n"
                            "  ov kill [-9] [-g] <pid|:port>             SIGTERM, or SIGKILL with -9; -g signals the process group\n"
                            "  ov wait :<port> [--closed] [--timeout 30s] wait until port opens, or closes\n"
                            "  ov free                                   print an unused port\n"
                            "\n"
                            "  -u, --udp   include UDP sockets\n";

enum { WAIT_POLL_MS = 200, PROBE_TIMEOUT_MS = 200 };

typedef enum Command {
    COMMAND_SCREEN,
    COMMAND_PORTS,
    COMMAND_PORT,
    COMMAND_KILL,
    COMMAND_WAIT,
    COMMAND_FREE,
    COMMAND_HELP,
} Command;

typedef struct Options {
    Command command;
    bool json;
    bool udp;
    bool kill_hard;
    bool kill_group;
    bool wait_closed;
    // A target is a port, from ":3000", or a pid; the other is 0.
    int port;
    int pid;
    // timeout_ns is 0 to wait without limit.
    int64_t timeout_ns;
} Options;

static bool parse_target(String text, Options *options)
{
    if (str_starts_with(text, S(":"))) {
        return parse_port(str_slice(text, 1, text.len), &options->port);
    }
    int64_t pid;
    if (!str_is_digits(text) || !str_parse_i64(text, &pid) || pid < 1 || pid > INT32_MAX) {
        return false;
    }
    options->pid = (int)pid;
    return true;
}

static Command command_named(String name)
{
    static const struct {
        const char *name;
        Command command;
    } COMMANDS[] = {
        { "ports", COMMAND_PORTS }, { "kill", COMMAND_KILL },  { "wait", COMMAND_WAIT },
        { "free", COMMAND_FREE },   { "help", COMMAND_HELP },  { "-h", COMMAND_HELP },
        { "--help", COMMAND_HELP },
    };
    for (size_t i = 0; i < countof(COMMANDS); i++) {
        if (str_equal(name, S(COMMANDS[i].name))) {
            return COMMANDS[i].command;
        }
    }
    return COMMAND_SCREEN;
}

[[nodiscard]] static Error parse_flag(StringList args, size_t *at, Options *options, Err *err)
{
    String arg = args.items[*at];
    if (str_equal(arg, S("--json"))) {
        options->json = true;
    } else if (str_equal(arg, S("-u")) || str_equal(arg, S("--udp"))) {
        options->udp = true;
    } else if (str_equal(arg, S("-9"))) {
        options->kill_hard = true;
    } else if (str_equal(arg, S("-g"))) {
        options->kill_group = true;
    } else if (str_equal(arg, S("--closed"))) {
        options->wait_closed = true;
    } else if (str_equal(arg, S("--timeout"))) {
        if (*at + 1 >= args.count || !fmt_parse_duration(args.items[*at + 1], &options->timeout_ns) ||
            options->timeout_ns <= 0) {
            return err_set(err, ERR_INVALID_ARGUMENT, "--timeout takes a duration, such as 30s");
        }
        (*at)++;
    } else {
        return err_set(err, ERR_INVALID_ARGUMENT, "unknown option %.*s", (int)arg.len, arg.data);
    }
    return ERR_OK;
}

// options_parse reads the arguments after the program name.
[[nodiscard]] static Error options_parse(StringList args, Options *options, Err *err)
{
    *options = (Options) { .command = COMMAND_SCREEN };
    size_t at = 0;
    if (args.count > 0 && str_starts_with(args.items[0], S(":"))) {
        options->command = COMMAND_PORT;
    } else if (args.count > 0 && !str_starts_with(args.items[0], S("-"))) {
        options->command = command_named(args.items[0]);
        if (options->command == COMMAND_SCREEN) {
            return err_set(err, ERR_INVALID_ARGUMENT, "unknown command %.*s", (int)args.items[0].len,
                           args.items[0].data);
        }
        at = 1;
    } else if (args.count > 0 && command_named(args.items[0]) == COMMAND_HELP) {
        options->command = COMMAND_HELP;
        return ERR_OK;
    }
    bool has_target = false;
    for (; at < args.count; at++) {
        String arg = args.items[at];
        if (str_starts_with(arg, S("-"))) {
            Error error = parse_flag(args, &at, options, err);
            if (error != ERR_OK) {
                return error;
            }
            continue;
        }
        if (has_target || !parse_target(arg, options)) {
            return err_set(err, ERR_INVALID_ARGUMENT, "unexpected %.*s", (int)arg.len, arg.data);
        }
        has_target = true;
    }
    bool needs_target = options->command == COMMAND_PORT || options->command == COMMAND_KILL ||
                        options->command == COMMAND_WAIT;
    if (needs_target != has_target) {
        return err_set(err, ERR_INVALID_ARGUMENT, needs_target ? "missing <pid|:port>" : "this command takes no target");
    }
    if ((options->command == COMMAND_PORT || options->command == COMMAND_WAIT) && options->port == 0) {
        return err_set(err, ERR_INVALID_ARGUMENT, "the target is a port, such as :3000");
    }
    return ERR_OK;
}

static void print(String text)
{
    fwrite(text.data, 1, text.len, stdout);
}

static String process_text(const Listener *listener)
{
    return listener->pid == 0 ? S("?") : listener->name;
}

static Node *int_node(Arena *arena, int64_t value)
{
    Node *node = node_create(arena, NODE_INT);
    node->integer = value;
    return node;
}

static Node *bool_node(Arena *arena, bool value)
{
    Node *node = node_create(arena, NODE_BOOL);
    node->boolean = value;
    return node;
}

static Node *listener_node(Arena *arena, const Listener *listener)
{
    Node *node = node_create(arena, NODE_MAPPING);
    node_put(arena, node, S("protocol"), node_scalar(arena, NODE_STRING, S(protocol_name(listener->protocol))));
    node_put(arena, node, S("host"), node_scalar(arena, NODE_STRING, listener->host));
    node_put(arena, node, S("port"), int_node(arena, listener->port));
    node_put(arena, node, S("pid"), listener->pid == 0 ? node_create(arena, NODE_NULL)
                                                       : int_node(arena, listener->pid));
    node_put(arena, node, S("process"),
             listener->pid == 0 ? node_create(arena, NODE_NULL) : node_scalar(arena, NODE_STRING, listener->name));
    node_put(arena, node, S("user"), node_scalar(arena, NODE_STRING, listener->user));
    node_put(arena, node, S("connections"), int_node(arena, (int64_t)listener->connections));
    node_put(arena, node, S("exposed"), bool_node(arena, listener->exposed));
    node_put(arena, node, S("container"), listener->container.len == 0
                                              ? node_create(arena, NODE_NULL)
                                              : node_scalar(arena, NODE_STRING, listener->container));
    return node;
}

static void print_listeners(Arena *arena, Listeners listeners, bool json)
{
    if (json) {
        Node *list = node_create(arena, NODE_SEQUENCE);
        for (size_t i = 0; i < listeners.count; i++) {
            node_append(arena, list, listener_node(arena, &listeners.items[i]));
        }
        print(json_encode(arena, list));
        print(S("\n"));
        return;
    }
    bool containers = false;
    for (size_t i = 0; i < listeners.count; i++) {
        containers = containers || listeners.items[i].container.len > 0;
    }
    Table table = table_create(arena);
    String header[] = { S("PROTO"), S("ADDRESS"), S("PID"), S("PROCESS"), S("USER"), S("CONNS"), S("CONTAINER") };
    table_row(&table, containers ? countof(header) : countof(header) - 1, header);
    for (size_t i = 0; i < listeners.count; i++) {
        const Listener *listener = &listeners.items[i];
        String row[] = {
            S(protocol_name(listener->protocol)),
            address_text(arena, listener->host, listener->port),
            listener->pid == 0 ? S("-") : str_format(arena, "%d", listener->pid),
            process_text(listener),
            listener->user,
            str_format(arena, "%zu", listener->connections),
            listener->container,
        };
        table_row(&table, containers ? countof(row) : countof(row) - 1, row);
    }
    print(table_format(&table));
}

static Listeners listeners_on_port(Arena *arena, Listeners all, int port)
{
    Listeners found = { .items = arena_push(arena, arena_size_mul(all.count + 1, sizeof *all.items)) };
    for (size_t i = 0; i < all.count; i++) {
        if (all.items[i].port == port) {
            found.items[found.count++] = all.items[i];
        }
    }
    return found;
}

// port_accepts reports whether a connection to port on this machine's
// loopback is accepted, which finds listeners ov cannot read, such as
// root's on macOS, without ever holding the port.
static bool port_accepts(Arena *arena, int port)
{
    const char *const hosts[] = { "127.0.0.1", "::1" };
    for (size_t i = 0; i < countof(hosts); i++) {
        int fd;
        if (net_connect(S(hosts[i]), str_format(arena, "%d", port), PROBE_TIMEOUT_MS, &fd, nullptr) == ERR_OK) {
            net_close(fd);
            return true;
        }
    }
    return false;
}

static int fail(const Err *err)
{
    fprintf(stderr, "ov: %s\n", err->msg);
    return 1;
}

[[nodiscard]] static Error collect(Arena *arena, bool include_udp, Listeners *listeners, Err *err)
{
    ContainerPorts containers = docker_ports(arena);
    return listeners_collect(arena, include_udp, &containers, listeners, err);
}

static int run_ports(Arena *arena, const Options *options)
{
    Listeners listeners;
    Err err = { 0 };
    if (collect(arena, options->udp, &listeners, &err) != ERR_OK) {
        return fail(&err);
    }
    print_listeners(arena, listeners, options->json);
    return 0;
}

static int run_port(Arena *arena, const Options *options)
{
    Listeners listeners;
    Err err = { 0 };
    if (collect(arena, true, &listeners, &err) != ERR_OK) {
        return fail(&err);
    }
    Listeners found = listeners_on_port(arena, listeners, options->port);
    if (found.count > 0) {
        print_listeners(arena, found, options->json);
        return 0;
    }
    if (port_accepts(arena, options->port)) {
        fprintf(stderr, "ov: port %d is in use by a process ov cannot read; try sudo\n", options->port);
        return 0;
    }
    fprintf(stderr, "ov: port %d is free\n", options->port);
    return 1;
}

static const char *signal_name(Signal signal)
{
    return signal == SIGNAL_KILL ? "SIGKILL" : "SIGTERM";
}

// signal_listener sends signal to the process of listener, or to its group,
// but never to the group ov itself runs in.
[[nodiscard]] static Error signal_listener(Arena *arena, const Listener *listener, Signal signal, bool group,
                                           Err *err)
{
    // pid 1 is launchd or init, which holds the sockets of services it starts on demand.
    if (listener->pid == 1 || (group && listener->group_id == 1)) {
        return err_set(err, ERR_INVALID_ARGUMENT, "refusing to signal %s 1, the system's init",
                       group ? "group" : "pid");
    }
    if (!group) {
        return process_signal(listener->pid, signal, err);
    }
    ProcessInfo self;
    Error error = process_info(arena, process_id(), &self, err);
    if (error != ERR_OK) {
        return error;
    }
    if (listener->group_id == self.group_id) {
        return err_set(err, ERR_INVALID_ARGUMENT, "%.*s (%d) is in ov's own process group",
                       (int)listener->name.len, listener->name.data, listener->pid);
    }
    return process_signal_group(listener->group_id, signal, err);
}

static String signalled_text(Arena *arena, const Listener *listener, Signal signal, bool group)
{
    return str_format(arena, "sent %s to %s%.*s (%d)", signal_name(signal), group ? "the group of " : "",
                      (int)listener->name.len, listener->name.data, group ? listener->group_id : listener->pid);
}

static int run_kill(Arena *arena, const Options *options)
{
    Listeners targets = { 0 };
    Err err = { 0 };
    if (options->port != 0) {
        Listeners listeners;
        if (collect(arena, true, &listeners, &err) != ERR_OK) {
            return fail(&err);
        }
        targets = listeners_on_port(arena, listeners, options->port);
    } else {
        Listener *listener = arena_push(arena, sizeof *listener);
        *listener = (Listener) { .pid = options->pid };
        describe_process(arena, listener, 0, 0);
        targets = (Listeners) { .items = listener, .count = 1 };
    }
    Signal signal = options->kill_hard ? SIGNAL_KILL : SIGNAL_STOP;
    size_t signalled = 0;
    int status = 0;
    for (size_t i = 0; i < targets.count; i++) {
        const Listener *target = &targets.items[i];
        bool repeat = false;
        for (size_t j = 0; j < i; j++) {
            repeat = repeat || (options->kill_group ? targets.items[j].group_id == target->group_id
                                                    : targets.items[j].pid == target->pid);
        }
        if (target->pid == 0 || repeat) {
            continue;
        }
        if (signal_listener(arena, target, signal, options->kill_group, &err) != ERR_OK) {
            status = fail(&err);
            continue;
        }
        print(signalled_text(arena, target, signal, options->kill_group));
        print(S("\n"));
        signalled++;
    }
    if (signalled == 0 && status == 0) {
        fprintf(stderr, "ov: no process ov can read holds port %d%s\n", options->port,
                targets.count > 0 ? "; try sudo" : "");
        return 1;
    }
    return status;
}

static bool port_open(Arena *arena, int port)
{
    Listeners listeners;
    if (listeners_collect(arena, false, &(ContainerPorts) { 0 }, &listeners, nullptr) == ERR_OK &&
        listeners_on_port(arena, listeners, port).count > 0) {
        return true;
    }
    return port_accepts(arena, port);
}

static int run_wait(Arena *arena, const Options *options)
{
    int64_t deadline = options->timeout_ns > 0 ? clock_monotonic_ns() + options->timeout_ns : 0;
    for (;;) {
        ArenaMark mark = arena_mark(arena);
        bool open = port_open(arena, options->port);
        arena_release(mark);
        if (open != options->wait_closed) {
            return 0;
        }
        if (deadline != 0 && clock_monotonic_ns() >= deadline) {
            fprintf(stderr, "ov: port %d did not %s in time\n", options->port, options->wait_closed ? "close" : "open");
            return 1;
        }
        clock_sleep_ms(WAIT_POLL_MS);
    }
}

static int run_free(Arena *arena)
{
    int fd;
    String address;
    Err err = { 0 };
    if (net_listen(S(":0"), &fd, &err) != ERR_OK) {
        return fail(&err);
    }
    Error error = net_local_address(arena, fd, &address, &err);
    net_close(fd);
    size_t colon;
    if (error != ERR_OK || !str_find_last_char(address, ':', &colon)) {
        return error != ERR_OK ? fail(&err) : 1;
    }
    print(str_slice(address, colon + 1, address.len));
    print(S("\n"));
    return 0;
}

// run_command runs every command but the screen, which needs tui.c.
static int run_command(Arena *arena, const Options *options)
{
    switch (options->command) {
    case COMMAND_SCREEN:
    case COMMAND_PORTS:
        return run_ports(arena, options);
    case COMMAND_PORT:
        return run_port(arena, options);
    case COMMAND_KILL:
        return run_kill(arena, options);
    case COMMAND_WAIT:
        return run_wait(arena, options);
    case COMMAND_FREE:
        return run_free(arena);
    case COMMAND_HELP:
        fputs(USAGE, stdout);
        return 0;
    }
    return 2;
}
