// Self-bootstrapping build program. Bootstrap once with:
//     cc tools/build.c -o nob
// Afterwards ./nob rebuilds itself when this file changes.
//
//     ./nob                 optimised binary              -> ./ov
//     ./nob test            the tests under AddressSanitizer and UBSan
//     ./nob check           modern-c contract check, then the tests
//     ./nob cross           every target with zig cc      -> out/<target>/ov
//     ./nob ci              check, the release build, then cross
//     ./nob install         ./ov to $PREFIX/bin (default ~/.local), under $DESTDIR when set
//     ./nob uninstall       remove the installed binary
//     ./nob clean           remove out/ and ./ov
//     ./nob hash <dir>      print the deps.lock hash of a directory
//
// Every mode but clean, hash and uninstall first fetches the dependencies in
// deps.lock. Host builds compile with $CC, or cc when it is unset.

#include <dirent.h>
#include <errno.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

enum { MAX_ARGS = 256, PATH_MAX_LEN = 1024 };

typedef struct {
    const char *items[MAX_ARGS];
    size_t count;
} Command;

static const char *const WARNING_FLAGS[] = {
    "-std=c23", "-Wall", "-Wextra", "-Werror", "-Wconversion", "-Wshadow", "-Wvla",
    "-Wstrict-prototypes", "-Wimplicit-fallthrough", "-Wno-unused-parameter",
};
// A sanitizer finding stops the tests rather than scrolling past.
static const char *const DEBUG_FLAGS[] = { "-g", "-O0", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                           "-fno-omit-frame-pointer" };
static const char *const RELEASE_FLAGS[] = { "-g", "-O2" };
static const char *const INCLUDE_FLAGS[] = { "-Ideps/libmc/include", "-Isrc" };

typedef struct {
    const char *name;
    const char *zig_target;
    // links says whether zig can link the binary: macOS needs the SDK's
    // frameworks, so that target only compiles.
    bool links;
} Target;

// The glibc version matches libmc's cross builds.
static const Target TARGETS[] = {
    { "linux-x86_64", "x86_64-linux-gnu.2.39", true },
    { "linux-aarch64", "aarch64-linux-gnu.2.39", true },
    { "macos-aarch64", "aarch64-macos", false },
};

static void command_push(Command *command, const char *item)
{
    if (command->count + 1 >= MAX_ARGS) {
        fprintf(stderr, "build: too many arguments\n");
        exit(2);
    }
    command->items[command->count++] = item;
    command->items[command->count] = NULL;
}

static void command_push_many(Command *command, const char *const *items, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        command_push(command, items[i]);
    }
}

static int command_run(const Command *command)
{
    for (size_t i = 0; i < command->count; i++) {
        printf("%s%s", i == 0 ? "" : " ", command->items[i]);
    }
    printf("\n");
    fflush(stdout);
    pid_t pid;
    int status = posix_spawnp(&pid, command->items[0], NULL, NULL, (char *const *)command->items, environ);
    if (status != 0) {
        fprintf(stderr, "build: cannot run %s: %s\n", command->items[0], strerror(status));
        return 1;
    }
    int wait_status = 0;
    if (waitpid(pid, &wait_status, 0) < 0) {
        return 1;
    }
    return WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : 1;
}

static void run_or_die(const Command *command)
{
    int status = command_run(command);
    if (status != 0) {
        fprintf(stderr, "build: command failed with status %d\n", status);
        exit(status);
    }
}

static time_t mtime_of(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

static void mkdir_p(const char *path)
{
    char buffer[PATH_MAX_LEN];
    snprintf(buffer, sizeof buffer, "%s", path);
    for (char *p = buffer + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buffer, 0755);
            *p = '/';
        }
    }
    mkdir(buffer, 0755);
}

// Dependencies are fetched, not committed. deps.lock pins each one by the
// content hash of its directory, and the build fetches deps/<name> when it is
// missing or differs. The hash is `modern-c hash`, computed with a copy of
// libmc's SHA-256, since this program builds libmc and cannot link it.

enum { SHA256_LEN = 32, SHA256_BLOCK_LEN = 64, SHA256_HEX_LEN = 2 * SHA256_LEN, LOCK_HASH_LEN = 72 };

typedef struct {
    uint32_t state[8];
    uint64_t length;
    uint8_t buffer[SHA256_BLOCK_LEN];
    size_t buffered;
} Sha256;

static const uint32_t SHA256_ROUND_CONSTANTS[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotate_right(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_transform(Sha256 *hash, const uint8_t block[SHA256_BLOCK_LEN])
{
    uint32_t w[64];
    for (size_t i = 0; i < 16; i++) {
        w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 | (uint32_t)block[i * 4 + 2] << 8 |
               block[i * 4 + 3];
    }
    for (size_t i = 16; i < 64; i++) {
        uint32_t s0 = rotate_right(w[i - 15], 7) ^ rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotate_right(w[i - 2], 17) ^ rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = hash->state[0];
    uint32_t b = hash->state[1];
    uint32_t c = hash->state[2];
    uint32_t d = hash->state[3];
    uint32_t e = hash->state[4];
    uint32_t f = hash->state[5];
    uint32_t g = hash->state[6];
    uint32_t h = hash->state[7];
    for (size_t i = 0; i < 64; i++) {
        uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + choice + SHA256_ROUND_CONSTANTS[i] + w[i];
        uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    hash->state[0] += a;
    hash->state[1] += b;
    hash->state[2] += c;
    hash->state[3] += d;
    hash->state[4] += e;
    hash->state[5] += f;
    hash->state[6] += g;
    hash->state[7] += h;
}

static void sha256_init(Sha256 *hash)
{
    static const uint32_t initial[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(hash->state, initial, sizeof initial);
    hash->length = 0;
    hash->buffered = 0;
}

static void sha256_update(Sha256 *hash, const void *data, size_t len)
{
    const uint8_t *bytes = data;
    hash->length += len;
    if (hash->buffered > 0) {
        size_t take = SHA256_BLOCK_LEN - hash->buffered < len ? SHA256_BLOCK_LEN - hash->buffered : len;
        memcpy(hash->buffer + hash->buffered, bytes, take);
        hash->buffered += take;
        bytes += take;
        len -= take;
        if (hash->buffered < SHA256_BLOCK_LEN) {
            return;
        }
        sha256_transform(hash, hash->buffer);
        hash->buffered = 0;
    }
    for (; len >= SHA256_BLOCK_LEN; bytes += SHA256_BLOCK_LEN, len -= SHA256_BLOCK_LEN) {
        sha256_transform(hash, bytes);
    }
    if (len > 0) {
        memcpy(hash->buffer, bytes, len);
    }
    hash->buffered = len;
}

static void sha256_final_hex(Sha256 *hash, char hex[SHA256_HEX_LEN + 1])
{
    uint64_t bits = hash->length * 8;
    uint8_t padding[SHA256_BLOCK_LEN + 8] = { 0x80 };
    size_t pad_len = hash->buffered < 56 ? 56 - hash->buffered : SHA256_BLOCK_LEN + 56 - hash->buffered;
    for (size_t i = 0; i < 8; i++) {
        padding[pad_len + i] = (uint8_t)(bits >> (56 - i * 8));
    }
    sha256_update(hash, padding, pad_len + 8);
    for (size_t i = 0; i < 8; i++) {
        snprintf(hex + i * 8, 9, "%08x", hash->state[i]);
    }
}

static void hash_file(const char *path, char hex[SHA256_HEX_LEN + 1])
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "build: cannot read %s\n", path);
        exit(1);
    }
    Sha256 hash;
    sha256_init(&hash);
    static uint8_t chunk[1 << 16];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, file)) > 0) {
        sha256_update(&hash, chunk, n);
    }
    fclose(file);
    sha256_final_hex(&hash, hex);
}

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} PathList;

// list_tree appends the path of every file under root/relative, relative to
// root, skipping what `modern-c hash` skips: .DS_Store, and at the top .git
// and what a dependency's own build creates, deps/, out/ and nob.
static void list_tree(PathList *list, const char *root, const char *relative)
{
    static const char *const SKIPPED_AT_TOP[] = { ".git", "deps", "out", "nob" };
    char directory[PATH_MAX_LEN];
    snprintf(directory, sizeof directory, "%s/%s", root, relative);
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        bool skipped = strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strcmp(name, ".DS_Store") == 0;
        for (size_t i = 0; relative[0] == '\0' && i < sizeof SKIPPED_AT_TOP / sizeof *SKIPPED_AT_TOP; i++) {
            skipped = skipped || strcmp(name, SKIPPED_AT_TOP[i]) == 0;
        }
        if (skipped) {
            continue;
        }
        char child[PATH_MAX_LEN];
        snprintf(child, sizeof child, "%s%s%s", relative, relative[0] == '\0' ? "" : "/", name);
        char path[PATH_MAX_LEN];
        snprintf(path, sizeof path, "%s/%s", root, child);
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            list_tree(list, root, child);
            continue;
        }
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (list->count == list->capacity) {
            list->capacity = list->capacity == 0 ? 256 : list->capacity * 2;
            list->items = realloc(list->items, list->capacity * sizeof *list->items);
        }
        list->items[list->count] = strdup(child);
        if (list->items == NULL || list->items[list->count] == NULL) {
            fprintf(stderr, "build: out of memory\n");
            exit(1);
        }
        list->count++;
    }
    closedir(dir);
}

static int compare_paths(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

// tree_hash computes the hash deps.lock records for root: SHA-256 over each
// file's relative path, a NUL, the hex SHA-256 of its content and a newline,
// in byte order of path. A missing root hashes as an empty one.
static void tree_hash(const char *root, char out[LOCK_HASH_LEN])
{
    PathList files = { 0 };
    list_tree(&files, root, "");
    qsort(files.items, files.count, sizeof *files.items, compare_paths);
    Sha256 hash;
    sha256_init(&hash);
    for (size_t i = 0; i < files.count; i++) {
        char path[PATH_MAX_LEN];
        snprintf(path, sizeof path, "%s/%s", root, files.items[i]);
        char file_hex[SHA256_HEX_LEN + 1];
        hash_file(path, file_hex);
        sha256_update(&hash, files.items[i], strlen(files.items[i]) + 1);
        sha256_update(&hash, file_hex, SHA256_HEX_LEN);
        sha256_update(&hash, "\n", 1);
        free(files.items[i]);
    }
    free(files.items);
    char hex[SHA256_HEX_LEN + 1];
    sha256_final_hex(&hash, hex);
    snprintf(out, LOCK_HASH_LEN, "sha256:%s", hex);
}

typedef struct {
    char name[128];
    char version[128];
    char url[PATH_MAX_LEN];
    char hash[LOCK_HASH_LEN];
} LockedDependency;

static void remove_tree(const char *path)
{
    Command command = { 0 };
    command_push(&command, "rm");
    command_push(&command, "-rf");
    command_push(&command, path);
    run_or_die(&command);
}

// unpacked_root finds the one top-level directory an archive unpacked into
// staging: <name>/ in a release asset, <repository>-<commit>/ in GitHub's
// archive of a commit.
static void unpacked_root(const char *staging, const char *url, char out[PATH_MAX_LEN])
{
    DIR *dir = opendir(staging);
    size_t count = 0;
    struct dirent *entry;
    while (dir != NULL && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            snprintf(out, PATH_MAX_LEN, "%s/%s", staging, entry->d_name);
            count++;
        }
    }
    if (dir != NULL) {
        closedir(dir);
    }
    struct stat st;
    if (count != 1 || stat(out, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "build: %s must hold one top-level directory\n", url);
        exit(1);
    }
}

// fetch downloads the archive deps.lock points at and moves its top-level
// directory to deps/<name> once its content matches the lock.
static void fetch(const LockedDependency *dependency)
{
    char staging[PATH_MAX_LEN];
    char archive[PATH_MAX_LEN];
    char unpacked[PATH_MAX_LEN];
    char directory[PATH_MAX_LEN];
    snprintf(staging, sizeof staging, "out/fetch/%s", dependency->name);
    snprintf(archive, sizeof archive, "%s.tar.gz", staging);
    snprintf(directory, sizeof directory, "deps/%s", dependency->name);
    printf("build: fetching %s %s\n", dependency->name, dependency->version);
    remove_tree(staging);
    mkdir_p(staging);

    Command download = { 0 };
    command_push(&download, "curl");
    command_push(&download, "-fsSL");
    command_push(&download, "-o");
    command_push(&download, archive);
    command_push(&download, dependency->url);
    if (command_run(&download) != 0) {
        fprintf(stderr, "build: cannot fetch %s from %s; fetching dependencies needs the network and curl\n",
                dependency->name, dependency->url);
        exit(1);
    }
    Command unpack = { 0 };
    command_push(&unpack, "tar");
    command_push(&unpack, "-xzf");
    command_push(&unpack, archive);
    command_push(&unpack, "-C");
    command_push(&unpack, staging);
    run_or_die(&unpack);
    unpacked_root(staging, dependency->url, unpacked);

    char actual[LOCK_HASH_LEN];
    tree_hash(unpacked, actual);
    if (strcmp(actual, dependency->hash) != 0) {
        fprintf(stderr, "build: %s holds %s, deps.lock says %s\n", dependency->url, actual, dependency->hash);
        exit(1);
    }
    mkdir_p("deps");
    remove_tree(directory);
    if (rename(unpacked, directory) != 0) {
        fprintf(stderr, "build: cannot move %s to %s: %s\n", unpacked, directory, strerror(errno));
        exit(1);
    }
    remove_tree(staging);
    remove(archive);
}

// fetch_dependencies fetches each dependency whose directory is missing or
// does not match deps.lock. Lines with a fifth field, prebuilt archives, are
// not fetched here.
static void fetch_dependencies(void)
{
    FILE *lock = fopen("deps.lock", "r");
    if (lock == NULL) {
        fprintf(stderr, "build: cannot read deps.lock\n");
        exit(1);
    }
    char line[2 * PATH_MAX_LEN];
    while (fgets(line, sizeof line, lock) != NULL) {
        LockedDependency dependency;
        char fifth[2];
        if (line[0] == '#' || sscanf(line, "%127s %127s %1023s %71s %1s", dependency.name, dependency.version,
                                     dependency.url, dependency.hash, fifth) != 4) {
            continue;
        }
        char directory[PATH_MAX_LEN];
        snprintf(directory, sizeof directory, "deps/%s", dependency.name);
        char actual[LOCK_HASH_LEN];
        tree_hash(directory, actual);
        if (strcmp(actual, dependency.hash) != 0) {
            fetch(&dependency);
        }
    }
    fclose(lock);
}

static const char *host_compiler(void)
{
    const char *compiler = getenv("CC");
    return compiler != NULL && compiler[0] != '\0' ? compiler : "cc";
}

// build_libmc builds deps/libmc with libmc's own build program, which fetches
// its dependencies into deps/libmc/deps and writes deps/libmc/out; the
// deps.lock hash skips both.
static void build_libmc(bool release)
{
    Command command = { 0 };
    command_push(&command, "sh");
    command_push(&command, "-c");
    command_push(&command, release ? "cd deps/libmc && cc tools/build.c -o nob && ./nob release"
                                   : "cd deps/libmc && cc tools/build.c -o nob && ./nob build");
    run_or_die(&command);
}

static void link_program(bool release, const char *main_source, const char *binary)
{
    build_libmc(release);
    Command command = { 0 };
    command_push(&command, host_compiler());
    command_push_many(&command, WARNING_FLAGS, sizeof WARNING_FLAGS / sizeof *WARNING_FLAGS);
    if (release) {
        command_push_many(&command, RELEASE_FLAGS, sizeof RELEASE_FLAGS / sizeof *RELEASE_FLAGS);
    } else {
        command_push_many(&command, DEBUG_FLAGS, sizeof DEBUG_FLAGS / sizeof *DEBUG_FLAGS);
    }
    command_push_many(&command, INCLUDE_FLAGS, sizeof INCLUDE_FLAGS / sizeof *INCLUDE_FLAGS);
    command_push(&command, main_source);
    command_push(&command, release ? "deps/libmc/out/release/libmc.a" : "deps/libmc/out/debug/libmc.a");
    command_push(&command, "-lpthread");
    command_push(&command, "-lm");
#ifdef __APPLE__
    static const char *const FRAMEWORKS[] = { "CoreServices", "SystemConfiguration", "CoreFoundation" };
    for (size_t i = 0; i < sizeof FRAMEWORKS / sizeof *FRAMEWORKS; i++) {
        command_push(&command, "-framework");
        command_push(&command, FRAMEWORKS[i]);
    }
#endif
    command_push(&command, "-o");
    command_push(&command, binary);
    run_or_die(&command);
}

static int run_tests(void)
{
    mkdir_p("out");
    link_program(false, "tests/test_ov.c", "out/test_ov");
    Command run = { 0 };
    command_push(&run, "out/test_ov");
    return command_run(&run);
}

// run_cross builds libmc for every target, then compiles ov for each, and
// links it where zig can.
static int run_cross(void)
{
    Command libmc = { 0 };
    command_push(&libmc, "sh");
    command_push(&libmc, "-c");
    command_push(&libmc, "cd deps/libmc && cc tools/build.c -o nob && ./nob cross");
    run_or_die(&libmc);
    for (size_t i = 0; i < sizeof TARGETS / sizeof *TARGETS; i++) {
        const Target *target = &TARGETS[i];
        char directory[PATH_MAX_LEN];
        char output[PATH_MAX_LEN];
        char archive[PATH_MAX_LEN];
        snprintf(directory, sizeof directory, "out/%s", target->name);
        snprintf(output, sizeof output, "out/%s/%s", target->name, target->links ? "ov" : "ov.o");
        snprintf(archive, sizeof archive, "deps/libmc/out/%s/release/libmc.a", target->name);
        mkdir_p(directory);
        Command command = { 0 };
        command_push(&command, "zig");
        command_push(&command, "cc");
        command_push(&command, "-target");
        command_push(&command, target->zig_target);
        command_push_many(&command, WARNING_FLAGS, sizeof WARNING_FLAGS / sizeof *WARNING_FLAGS);
        command_push_many(&command, RELEASE_FLAGS, sizeof RELEASE_FLAGS / sizeof *RELEASE_FLAGS);
        command_push_many(&command, INCLUDE_FLAGS, sizeof INCLUDE_FLAGS / sizeof *INCLUDE_FLAGS);
        if (!target->links) {
            command_push(&command, "-c");
        }
        command_push(&command, "src/main.c");
        if (target->links) {
            command_push(&command, archive);
            command_push(&command, "-lpthread");
            command_push(&command, "-lm");
        }
        command_push(&command, "-o");
        command_push(&command, output);
        run_or_die(&command);
        printf("%s %s\n", target->links ? "built" : "compiled; linking it needs the macOS SDK:", output);
    }
    return 0;
}

static int run_check(void)
{
    const char *home = getenv("HOME");
    char checker[PATH_MAX_LEN];
    snprintf(checker, sizeof checker, "%s/.agents/tools/modern-c/modern-c", home == NULL ? "" : home);
    Command command = { 0 };
    command_push(&command, "ruby");
    command_push(&command, checker);
    command_push(&command, "check");
    command_push(&command, ".");
    int status = command_run(&command);
    return status != 0 ? status : run_tests();
}

// installed_path is $DESTDIR$PREFIX/bin/ov, PREFIX defaulting to ~/.local.
static void installed_path(char *directory, char *binary)
{
    const char *destdir = getenv("DESTDIR");
    const char *prefix = getenv("PREFIX");
    const char *home = getenv("HOME");
    char base[PATH_MAX_LEN];
    if (prefix != NULL && prefix[0] != '\0') {
        snprintf(base, sizeof base, "%s", prefix);
    } else {
        snprintf(base, sizeof base, "%s/.local", home == NULL ? "" : home);
    }
    snprintf(directory, PATH_MAX_LEN, "%s%s/bin", destdir == NULL ? "" : destdir, base);
    snprintf(binary, PATH_MAX_LEN, "%s/ov", directory);
}

static int run_install(void)
{
    char directory[PATH_MAX_LEN];
    char installed[PATH_MAX_LEN];
    installed_path(directory, installed);
    mkdir_p(directory);
    Command copy = { 0 };
    command_push(&copy, "install");
    command_push(&copy, "-m");
    command_push(&copy, "755");
    command_push(&copy, "ov");
    command_push(&copy, installed);
    return command_run(&copy);
}

static int run_uninstall(void)
{
    char directory[PATH_MAX_LEN];
    char installed[PATH_MAX_LEN];
    installed_path(directory, installed);
    Command remove_binary = { 0 };
    command_push(&remove_binary, "rm");
    command_push(&remove_binary, "-f");
    command_push(&remove_binary, installed);
    return command_run(&remove_binary);
}

static void rebuild_self_if_needed(char **argv)
{
    const char *source = "tools/build.c";
    if (mtime_of(source) <= mtime_of(argv[0])) {
        return;
    }
    printf("build: rebuilding self\n");
    Command command = { 0 };
    command_push(&command, "cc");
    command_push(&command, source);
    command_push(&command, "-o");
    command_push(&command, argv[0]);
    run_or_die(&command);
    execv(argv[0], argv);
    fprintf(stderr, "build: exec failed: %s\n", strerror(errno));
    exit(1);
}

int main(int argc, char **argv)
{
    rebuild_self_if_needed(argv);
    const char *mode = argc > 1 ? argv[1] : "build";

    if (strcmp(mode, "hash") == 0 && argc == 3) {
        char hash[LOCK_HASH_LEN];
        tree_hash(argv[2], hash);
        printf("%s\n", hash);
        return 0;
    }
    if (strcmp(mode, "uninstall") == 0) {
        return run_uninstall();
    }
    if (strcmp(mode, "clean") == 0) {
        Command command = { 0 };
        command_push(&command, "rm");
        command_push(&command, "-rf");
        command_push(&command, "out");
        command_push(&command, "ov");
        return command_run(&command);
    }

    fetch_dependencies();
    if (strcmp(mode, "build") == 0 || strcmp(mode, "install") == 0) {
        link_program(true, "src/main.c", "ov");
        return strcmp(mode, "install") == 0 ? run_install() : 0;
    }
    if (strcmp(mode, "test") == 0) {
        return run_tests();
    }
    if (strcmp(mode, "check") == 0) {
        return run_check();
    }
    if (strcmp(mode, "ci") == 0) {
        int status = run_check();
        if (status != 0) {
            return status;
        }
        link_program(true, "src/main.c", "ov");
        return run_cross();
    }
    if (strcmp(mode, "cross") == 0) {
        return run_cross();
    }

    fprintf(stderr, "build: unknown mode '%s'\nmodes: build, test, check, cross, ci, install, uninstall, clean, hash\n", mode);
    return 2;
}
