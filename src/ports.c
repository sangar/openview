#include "mc/container/sort.h"
#include "mc/core/arena.h"
#include "mc/platform/platform.h"
#include "mc/platform/procinfo.h"
#include "mc/text/unicode.h"

// A listener is a listening socket and the process holding it: one row of ov.

enum { DOCKER_TIMEOUT_MS = 2000, PORT_MAX = 65535, PORT_RANGE_MAX = 1024 };

typedef struct ContainerPort {
    SocketProtocol protocol;
    String host;
    int port;
    String name;
} ContainerPort;

typedef struct ContainerPorts {
    ContainerPort *items;
    size_t count;
} ContainerPorts;

typedef struct Listener {
    SocketProtocol protocol;
    String host;
    int port;
    // pid is 0 when the process holding the socket cannot be read.
    int pid;
    int group_id;
    String name;
    String user;
    String container;
    size_t connections;
    // exposed is listening on an address other machines can reach.
    bool exposed;
} Listener;

typedef struct Listeners {
    Listener *items;
    size_t count;
} Listeners;

static bool parse_port(String text, int *port)
{
    int64_t value;
    if (!str_parse_i64(str_trim(text), &value) || value < 1 || value > PORT_MAX) {
        return false;
    }
    *port = (int)value;
    return true;
}

// parse_port_range reads "8080" or "8000-8002".
static bool parse_port_range(String text, int *first, int *last)
{
    String low;
    String high;
    if (!str_cut(text, '-', &low, &high)) {
        return parse_port(text, first) && parse_port(text, last);
    }
    return parse_port(low, first) && parse_port(high, last) && *last >= *first && *last - *first < PORT_RANGE_MAX;
}

static bool container_ports_contain(const ContainerPorts *ports, ContainerPort port)
{
    for (size_t i = 0; i < ports->count; i++) {
        const ContainerPort *seen = &ports->items[i];
        if (seen->protocol == port.protocol && seen->port == port.port && str_equal(seen->host, port.host) &&
            str_equal(seen->name, port.name)) {
            return true;
        }
    }
    return false;
}

// docker_parse reads lines of `docker ps --format '{{.Names}}\t{{.Ports}}'`,
// whose published ports read "0.0.0.0:8080->80/tcp, [::]:8080->80/tcp"; ports
// a container exposes without publishing have no "->" and are left out.
static ContainerPorts docker_parse(Arena *arena, String text)
{
    ContainerPorts ports = { 0 };
    size_t capacity = 0;
    StringList lines = strlist_from_lines(arena, text);
    for (size_t i = 0; i < lines.count; i++) {
        String name;
        String published;
        if (!str_cut(lines.items[i], '\t', &name, &published)) {
            continue;
        }
        StringList mappings = str_split(arena, published, ',');
        for (size_t j = 0; j < mappings.count; j++) {
            String mapping = str_trim(mappings.items[j]);
            size_t arrow;
            size_t colon;
            int first;
            int last;
            if (!str_find(mapping, S("->"), &arrow) || !str_find_last_char(str_slice(mapping, 0, arrow), ':', &colon) ||
                !parse_port_range(str_slice(mapping, colon + 1, arrow), &first, &last)) {
                continue;
            }
            SocketProtocol protocol = str_ends_with(mapping, S("/udp")) ? SOCKET_UDP : SOCKET_TCP;
            String host = str_trim_suffix(str_trim_prefix(str_slice(mapping, 0, colon), S("[")), S("]"));
            for (int port = first; port <= last; port++) {
                ContainerPort entry = { .protocol = protocol, .host = host, .port = port, .name = str_trim(name) };
                if (!container_ports_contain(&ports, entry)) {
                    ports.items = arena_grow(arena, ports.items, &capacity, ports.count, sizeof *ports.items);
                    ports.items[ports.count++] = entry;
                }
            }
        }
    }
    return ports;
}

static String run_docker(Arena *arena, StringList argv)
{
    ProcessResult result;
    if (process_run_timeout(arena, argv, (String) { 0 }, DOCKER_TIMEOUT_MS, &result, nullptr) != ERR_OK ||
        result.exit_code != 0) {
        return (String) { 0 };
    }
    return result.stdout_text;
}

// docker_ports lists the ports running containers publish, in every docker
// context, since the listener may be Docker Desktop while the current
// context is another engine; none when docker is missing or stopped.
static ContainerPorts docker_ports(Arena *arena)
{
    String path;
    if (!process_look_path(arena, S("docker"), &path)) {
        return (ContainerPorts) { 0 };
    }
    const char *const list[] = { "docker", "context", "ls", "--format", "{{.Name}}" };
    StringList contexts = strlist_from_lines(arena, run_docker(arena, strlist_of(arena, countof(list), list)));
    String output = { 0 };
    for (size_t i = 0; i < contexts.count; i++) {
        const char *const ps[] = { "docker", "--context", str_cstr(arena, str_trim(contexts.items[i])), "ps",
                                   "--format", "{{.Names}}\t{{.Ports}}" };
        String text = run_docker(arena, strlist_of(arena, countof(ps), ps));
        output = str_format(arena, "%.*s%.*s\n", (int)output.len, output.data, (int)text.len, text.data);
    }
    return docker_parse(arena, output);
}

static String container_of(const ContainerPorts *containers, SocketProtocol protocol, String host, int port)
{
    for (size_t i = 0; i < containers->count; i++) {
        const ContainerPort *published = &containers->items[i];
        if (published->protocol == protocol && published->port == port && str_equal(published->host, host)) {
            return published->name;
        }
    }
    return (String) { 0 };
}

static bool host_is_exposed(String host)
{
    return !str_starts_with(host, S("127.")) && !str_equal(host, S("::1")) && !str_starts_with(host, S("::ffff:127."));
}

// address_text is host:port, with an IPv6 host in brackets.
static String address_text(Arena *arena, String host, int port)
{
    bool ipv6 = str_contains(host, S(":"));
    return str_format(arena, "%s%.*s%s:%d", ipv6 ? "[" : "", (int)host.len, host.data, ipv6 ? "]" : "", port);
}

static const char *protocol_name(SocketProtocol protocol)
{
    return protocol == SOCKET_UDP ? "udp" : "tcp";
}

static int compare_listeners(const void *context, const void *a, const void *b)
{
    const Listener *left = a;
    const Listener *right = b;
    if (left->port != right->port) {
        return left->port < right->port ? -1 : 1;
    }
    if (left->protocol != right->protocol) {
        return left->protocol < right->protocol ? -1 : 1;
    }
    if (left->pid != right->pid) {
        return left->pid < right->pid ? -1 : 1;
    }
    return str_compare(left->host, right->host);
}

// describe_process fills in what the process of listener is, from an earlier
// listener of the same process when there is one.
static void describe_process(Arena *arena, Listener *listeners, size_t index, int user_id)
{
    Listener *listener = &listeners[index];
    for (size_t i = 0; i < index; i++) {
        if (listeners[i].pid == listener->pid && listener->pid != 0) {
            listener->name = listeners[i].name;
            listener->user = listeners[i].user;
            listener->group_id = listeners[i].group_id;
            return;
        }
    }
    ProcessInfo info;
    if (listener->pid != 0 && process_info(arena, listener->pid, &info, nullptr) == ERR_OK) {
        listener->name = info.name;
        listener->group_id = info.group_id;
    }
    listener->user = user_name(arena, user_id);
}

// listeners_from picks the listening sockets out of sockets, TCP only unless
// include_udp, and counts the established connections each has accepted.
static Listeners listeners_from(Arena *arena, SocketList sockets, bool include_udp, const ContainerPorts *containers)
{
    Listeners listeners = { 0 };
    size_t capacity = 0;
    for (size_t i = 0; i < sockets.count; i++) {
        const SocketEntry *entry = &sockets.items[i];
        if (entry->state != SOCKET_LISTEN || (entry->protocol == SOCKET_UDP && !include_udp)) {
            continue;
        }
        listeners.items = arena_grow(arena, listeners.items, &capacity, listeners.count, sizeof *listeners.items);
        Listener *listener = &listeners.items[listeners.count++];
        *listener = (Listener) {
            .protocol = entry->protocol,
            .host = entry->local_host,
            .port = entry->local_port,
            .pid = entry->pid,
            .container = container_of(containers, entry->protocol, entry->local_host, entry->local_port),
            .exposed = host_is_exposed(entry->local_host),
        };
        describe_process(arena, listeners.items, listeners.count - 1, entry->user_id);
    }
    for (size_t i = 0; i < sockets.count; i++) {
        const SocketEntry *entry = &sockets.items[i];
        if (entry->protocol != SOCKET_TCP || entry->state != SOCKET_ESTABLISHED) {
            continue;
        }
        for (size_t j = 0; j < listeners.count; j++) {
            Listener *listener = &listeners.items[j];
            if (listener->protocol == SOCKET_TCP && listener->port == entry->local_port && listener->pid == entry->pid) {
                listener->connections++;
                break;
            }
        }
    }
    sort_stable(arena, listeners.items, listeners.count, sizeof *listeners.items, compare_listeners, nullptr);
    return listeners;
}

[[nodiscard]] static Error listeners_collect(Arena *arena, bool include_udp, const ContainerPorts *containers,
                                             Listeners *listeners, Err *err)
{
    SocketList sockets;
    Error error = sockets_list(arena, &sockets, err);
    if (error != ERR_OK) {
        return error;
    }
    *listeners = listeners_from(arena, sockets, include_udp, containers);
    return ERR_OK;
}

// listener_matches reports whether filter, lower case, is in any column of listener.
static bool listener_matches(Arena *scratch, const Listener *listener, String filter)
{
    if (filter.len == 0) {
        return true;
    }
    String address = address_text(scratch, listener->host, listener->port);
    String text = str_format(scratch, "%s %.*s %d %.*s %.*s %.*s", protocol_name(listener->protocol),
                             (int)address.len, address.data, listener->pid, (int)listener->name.len,
                             listener->name.data, (int)listener->user.len, listener->user.data,
                             (int)listener->container.len, listener->container.data);
    return str_contains(unicode_lower(scratch, text), filter);
}
