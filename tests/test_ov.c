#include <stdio.h>
#include <string.h>

#include "mc/core/base.h"
#include "mc/platform/platform.h"

#include "ports.c"
#include "cli.c"

typedef struct Test {
    Arena *arena;
    unsigned checks;
    unsigned failures;
} Test;

static void check(Test *test, bool ok, const char *what)
{
    test->checks++;
    if (!ok) {
        test->failures++;
        fprintf(stderr, "FAIL %s\n", what);
    }
}

static StringList args_of(Arena *arena, const char *line)
{
    return str_fields(arena, S(line));
}

static void options_parse_reads_each_command(Test *test)
{
    Arena *a = test->arena;
    Options options;
    check(test, options_parse(args_of(a, ""), &options, nullptr) == ERR_OK && options.command == COMMAND_SCREEN,
          "no arguments is the screen");
    check(test, options_parse(args_of(a, ":3000"), &options, nullptr) == ERR_OK && options.command == COMMAND_PORT &&
                    options.port == 3000,
          ":3000 asks who holds port 3000");
    check(test, options_parse(args_of(a, "kill -9 -g :80"), &options, nullptr) == ERR_OK &&
                    options.command == COMMAND_KILL && options.kill_hard && options.kill_group && options.port == 80,
          "kill with flags and a port");
    check(test, options_parse(args_of(a, "kill 1234"), &options, nullptr) == ERR_OK && options.pid == 1234,
          "kill a pid");
    check(test, options_parse(args_of(a, "wait :5432 --closed --timeout 30s"), &options, nullptr) == ERR_OK &&
                    options.wait_closed && options.timeout_ns == 30 * (int64_t)NS_PER_SECOND,
          "wait for a port to close");
    check(test, options_parse(args_of(a, "ports -u --json"), &options, nullptr) == ERR_OK && options.udp && options.json,
          "ports with UDP as JSON");
    const char *const wrong[] = { "kill", ":70000", "wait 1234", "free :80", "ports --bogus", "bogus", "kill 0" };
    for (size_t i = 0; i < countof(wrong); i++) {
        check(test, options_parse(args_of(a, wrong[i]), &options, nullptr) == ERR_INVALID_ARGUMENT, wrong[i]);
    }
}

static void docker_parse_reads_published_ports(Test *test)
{
    String text = S("web\t0.0.0.0:8080->80/tcp, [::]:8080->80/tcp, 443/tcp\n"
                    "db\t127.0.0.1:5432->5432/tcp\n"
                    "dns\t0.0.0.0:8000-8001->8000-8001/udp\n"
                    "quiet\t\n");
    ContainerPorts ports = docker_parse(test->arena, text);
    check(test, ports.count == 5, "two web addresses, db, and two dns ports; unpublished ports left out");
    check(test, str_equal(container_of(&ports, SOCKET_TCP, S("0.0.0.0"), 8080), S("web")), "web on IPv4");
    check(test, str_equal(container_of(&ports, SOCKET_TCP, S("::"), 8080), S("web")), "web on IPv6");
    check(test, container_of(&ports, SOCKET_TCP, S("127.0.0.1"), 8080).len == 0, "not another process on loopback");
    check(test, str_equal(container_of(&ports, SOCKET_UDP, S("0.0.0.0"), 8001), S("dns")), "a port in a range");
    check(test, container_of(&ports, SOCKET_TCP, S("0.0.0.0"), 8001).len == 0, "UDP is not TCP");
}

static void addresses_tell_loopback_from_exposed(Test *test)
{
    check(test, !host_is_exposed(S("127.0.0.1")) && !host_is_exposed(S("::1")), "loopback stays on this machine");
    check(test, host_is_exposed(S("0.0.0.0")) && host_is_exposed(S("::")) && host_is_exposed(S("192.168.1.5")),
          "every interface or a LAN address is reachable");
    check(test, str_equal(address_text(test->arena, S("::"), 80), S("[::]:80")), "IPv6 in brackets");
    check(test, str_equal(address_text(test->arena, S("127.0.0.1"), 80), S("127.0.0.1:80")), "IPv4 as it is");
}

static void filters_match_any_column_ignoring_case(Test *test)
{
    Arena *a = test->arena;
    Listener listener = {
        .host = S("0.0.0.0"), .port = 3000, .pid = 42, .name = S("Bjørn-Server"), .user = S("Åse"),
    };
    check(test, listener_matches(a, &listener, unicode_lower(a, S("BJØRN"))), "a process name, any case");
    check(test, listener_matches(a, &listener, S("åse")), "a user with å");
    check(test, listener_matches(a, &listener, S(":3000")), "the port");
    check(test, !listener_matches(a, &listener, S("postgres")), "something else");
}

static int port_of_address(String address)
{
    size_t colon;
    int port = 0;
    return str_find_last_char(address, ':', &colon) && parse_port(str_slice(address, colon + 1, address.len), &port)
               ? port
               : 0;
}

static void listeners_collect_finds_our_listener(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    int listener_fd;
    String address;
    check(test, net_listen(S("127.0.0.1:0"), &listener_fd, &err) == ERR_OK, "listen");
    check(test, net_local_address(a, listener_fd, &address, &err) == ERR_OK, "name the listener");
    int port = port_of_address(address);
    int client;
    int accepted;
    check(test, net_connect(S("127.0.0.1"), str_format(a, "%d", port), 1000, &client, &err) == ERR_OK, "connect");
    check(test, net_accept(listener_fd, 1000, &accepted), "accept");

    Listeners listeners;
    check(test, listeners_collect(a, false, &(ContainerPorts) { 0 }, &listeners, &err) == ERR_OK, "collect");
    Listeners ours = listeners_on_port(a, listeners, port);
    check(test, ours.count == 1, "our listener, once");
    if (ours.count == 1) {
        const Listener *listener = &ours.items[0];
        check(test, listener->pid == process_id() && str_equal(listener->name, S("test_ov")), "held by this test");
        check(test, !listener->exposed && listener->connections == 1, "on loopback, with one connection");
        check(test, str_equal(listener->user, user_name(a, process_user_id())), "as us");
    }
    Options who = { .command = COMMAND_PORT, .port = port };
    check(test, run_command(a, &who) == 0, "ov :port finds who holds it");
    Options wait = { .command = COMMAND_WAIT, .port = port, .timeout_ns = 100 * (int64_t)NS_PER_MILLISECOND };
    check(test, run_wait(a, &wait) == 0, "an open port is already open");
    net_close(client);
    net_close(accepted);
    net_close(listener_fd);
    wait.wait_closed = true;
    check(test, run_wait(a, &wait) == 0, "a closed port is already closed");
    wait.wait_closed = false;
    check(test, run_wait(a, &wait) == 1, "waiting for a closed port to open times out");
    check(test, run_command(a, &who) == 1, "ov :port on a free port");
}

static void kill_and_free_do_what_they_say(Test *test)
{
    Arena *a = test->arena;
    const char *const argv[] = { "sleep", "30" };
    int pid;
    check(test, process_spawn_detached(a, strlist_of(a, countof(argv), argv), S(""), &pid, nullptr) == ERR_OK,
          "start a sleep");
    Options kill = { .command = COMMAND_KILL, .pid = pid };
    check(test, run_command(a, &kill) == 0, "kill it");
    int64_t deadline = clock_monotonic_ns() + 5 * (int64_t)NS_PER_SECOND;
    while (process_alive(pid) && clock_monotonic_ns() < deadline) {
        clock_sleep_ms(10);
    }
    check(test, !process_alive(pid), "it is gone");
    check(test, run_command(a, &kill) == 1, "a gone process cannot be killed");
    Listener init = { .pid = 1, .group_id = 1, .name = S("init") };
    check(test, signal_listener(a, &init, SIGNAL_STOP, false, nullptr) == ERR_INVALID_ARGUMENT, "never pid 1");
    check(test, signal_listener(a, &init, SIGNAL_STOP, true, nullptr) == ERR_INVALID_ARGUMENT, "never group 1");
    Options free_port = { .command = COMMAND_FREE };
    check(test, run_command(a, &free_port) == 0, "a free port");
}

int main(void)
{
    void (*const tests[])(Test *) = {
        options_parse_reads_each_command,    docker_parse_reads_published_ports,
        addresses_tell_loopback_from_exposed, filters_match_any_column_ignoring_case,
        listeners_collect_finds_our_listener, kill_and_free_do_what_they_say,
    };
    Test test = { 0 };
    for (size_t i = 0; i < countof(tests); i++) {
        test.arena = arena_create(0);
        tests[i](&test);
        arena_destroy(test.arena);
    }
    printf("%s, %u checks\n", test.failures == 0 ? "all tests passed" : "tests failed", test.checks);
    return test.failures == 0 ? 0 : 1;
}
