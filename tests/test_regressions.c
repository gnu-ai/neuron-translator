/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "commands.h"
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <signal.h>

static int failures;
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%d: %s\n", __LINE__, #x); failures++; } } while (0)

static void corrupt(const char *path, long offset, const void *value, size_t size)
{
    FILE *f = fopen(path, "r+b");
    if (!f) { perror(path); exit(2); }
    CHECK(fseek(f, offset, SEEK_SET) == 0);
    CHECK(fwrite(value, size, 1, f) == 1);
    CHECK(fclose(f) == 0);
}

int main(void)
{
    CompactNeuralNetwork n = {0}, loaded = {0};
    const uint16_t topology[] = {2, 2, 1};
    CHECK(network_init(&n, 3, topology) == 0);
    CHECK(n.input_buffer[0] == 0 && n.output_buffer[0] == 0);
    /* An independently specified two-layer calculation catches indexing
     * errors that a self-round-trip alone cannot detect. */
    const float weights[] = {1, 0, 0, 1, 2, -1};
    memcpy(n.weights, weights, sizeof weights);
    n.biases[0] = 0.5f; n.biases[1] = -0.5f; n.biases[2] = 0.25f;
    CHECK(parse_input_string(&n, "1,2"));
    double h = 1.0 / (1.0 + exp(-1.5));
    CHECK(fabs(n.output_buffer[0] - 1.0 / (1.0 + exp(-(h + 0.25)))) < 1e-6);
    float output = n.output_buffer[0];
    size_t passes = n.forward_pass_count;
    const char *bad[] = {"1", "1,2,3", "1,2garbage", "nan,2", "inf,2", "1e99,2", "1x,2"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        CHECK(!parse_input_string(&n, bad[i]));
        CHECK(n.input_buffer[0] == 1 && n.input_buffer[1] == 2);
        CHECK(n.output_buffer[0] == output && n.forward_pass_count == passes);
    }
    uint16_t sizes[MAX_LAYERS];
    CHECK(parse_config_string("1,2,3,4,5,6,7,8,9", sizes, MAX_LAYERS) < 0);
    CHECK(parse_config_string("1,2junk", sizes, MAX_LAYERS) < 0);
    const uint16_t zero[] = {0, 2};
    void *old = n.memory_block;
    CHECK(network_init(&n, 2, zero) < 0 && n.memory_block == old);
    const uint16_t huge[] = {8192,8192,8192,8192,8192,8192};
    CHECK(network_init(&n, 6, huge) < 0 && errno == EFBIG && n.memory_block == old);
    CHECK(network_command(&n, "input 1,2", 9) == 0);
    CHECK(network_command(&n, " \tinput 1,2\r\n", 13) == 0);
    CHECK(network_command(&n, " \t\r\n", 4) == EINVAL);
    CHECK(n.topology.layer_count == 3 && n.topology.layer_sizes[0] == 2);
    CHECK(network_command(&n, "reset junk", 10) == EINVAL);
    CHECK(network_command(&n, "reset\ninput 1,2", 15) == EINVAL);
    CHECK(network_command(&n, "reset\0junk", 10) == EINVAL);
    CHECK(network_command(&n, "", NETWORK_COMMAND_MAX + 1) == EMSGSIZE);

    char directory[] = "regression-model-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char path[128];
    snprintf(path, sizeof path, "%s/model.nn", directory);
    CHECK(network_save(&n, path));
    CHECK(network_load(&loaded, path));
    /* Force a buffered write failure without touching system devices. The
     * previously saved file must survive the failed replacement. */
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        struct rlimit limit = {64, 64};
        signal(SIGXFSZ, SIG_IGN);
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(2);
        _exit(network_save(&n, path) ? 1 : 0);
    }
    if (child > 0) {
        int status;
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        CHECK(network_load(&loaded, path));
    }
    CHECK(loaded.output_buffer[0] == n.output_buffer[0]);
    CHECK(loaded.input_buffer[1] == n.input_buffer[1]);
    void *previous = loaded.memory_block;
    /* All three tables, including their initial offsets, are validated. */
    long tables = 6 + sizeof(NetworkTopology) + 3 * sizeof(size_t);
    for (unsigned i = 0; i < 8; i++) {
        CHECK(network_save(&n, path));
        size_t forged = 1000000;
        corrupt(path, tables + i * sizeof(size_t), &forged, sizeof forged);
        CHECK(!network_load(&loaded, path));
        CHECK(loaded.memory_block == previous && loaded.output_buffer[0] == n.output_buffer[0]);
    }
    for (unsigned i = 0; i < 3; i++) {
        CHECK(network_save(&n, path));
        size_t forged = SIZE_MAX;
        corrupt(path, 6 + sizeof(NetworkTopology) + i * sizeof(size_t), &forged, sizeof forged);
        CHECK(!network_load(&loaded, path));
    }
    CHECK(network_save(&n, path));
    uint32_t magic = 0;
    corrupt(path, 0, &magic, sizeof magic);
    CHECK(!network_load(&loaded, path));
    CHECK(network_save(&n, path));
    FILE *f = fopen(path, "ab");
    CHECK(f != NULL);
    if (f) { fputc('x', f); fclose(f); }
    CHECK(!network_load(&loaded, path));
    CHECK(network_save(&n, path));
    struct stat st;
    CHECK(stat(path, &st) == 0);
    CHECK(truncate(path, st.st_size - 1) == 0);
    errno = 0;
    CHECK(!network_load(&loaded, path) && errno == EINVAL);
    long payload = tables + 8 * sizeof(size_t);
    long array_offsets[] = {payload, payload + 5 * sizeof(float),
                            payload + 11 * sizeof(float)};
    for (unsigned i = 0; i < 3; i++) {
        CHECK(network_save(&n, path));
        float nan_value = NAN;
        corrupt(path, array_offsets[i], &nan_value, sizeof nan_value);
        CHECK(!network_load(&loaded, path));
    }
    CHECK(network_save(&n, path));
    uint8_t invalid_layers = 9;
    corrupt(path, 6, &invalid_layers, sizeof invalid_layers);
    CHECK(!network_load(&loaded, path));
    CHECK(loaded.memory_block == previous);
    CHECK(!network_save(&n, directory));
    if (access("/dev/full", F_OK) == 0) CHECK(!network_save(&n, "/dev/full"));
    network_reset(&n);
    CHECK(n.output_buffer[0] == 0 && n.input_buffer[0] == 0 && n.forward_pass_count == 0);
    CHECK(memcmp(n.weights, weights, sizeof weights) == 0);
    CHECK(network_save(&n, path));
    CHECK(network_load(&loaded, path) && loaded.output_buffer[0] == 0);
    unlink(path);
    CHECK(mkfifo(path, 0600) == 0);
    CHECK(!network_load(&loaded, path) && errno == EINVAL);
    unlink(path); rmdir(directory);

    CHECK(network_command(&n, "topology 8192,1", 15) == 0);
    char *input = malloc(6 + 2 * 8192 + 1);
    CHECK(input != NULL);
    if (input) {
        memcpy(input, "input ", 6);
        for (size_t i = 0; i < 8192; i++) { input[6 + 2*i] = '1'; input[7 + 2*i] = ' '; }
        CHECK(network_command(&n, input, 6 + 2*8192) == 0);
        CHECK(n.forward_pass_count == 1);
        free(input);
    }
    const uint16_t wide[] = {1, 8192};
    CHECK(network_init(&n, 2, wide) == 0);
    CHECK(parse_input_string(&n, "0"));
    char *text = NULL;
    size_t length = 0;
    CHECK(network_status(&n, &text, &length) == 0);
    if (text) {
        CHECK(length > 4096 && strlen(text) == length);
        CHECK(strstr(text, "[8191]: 0.500000") != NULL);
        free(text);
    }
    const uint16_t extreme[] = {2,1};
    CHECK(network_init(&n, 2, extreme) == 0);
    n.weights[0] = FLT_MAX; n.weights[1] = -FLT_MAX;
    n.input_buffer[0] = FLT_MAX; n.input_buffer[1] = FLT_MAX;
    network_forward(&n);
    CHECK(n.output_buffer[0] == 0.5f);
    network_free(&n); network_free(&loaded);
    printf("regressions: %s\n", failures ? "FAIL" : "OK");
    return failures ? 1 : 0;
}
