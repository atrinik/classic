#include <toolkit/clioptions.h>
#include <toolkit/logger.h>

#include <openssl/crypto.h>

#define require(condition)                                                        \
    do {                                                                          \
        if (!(condition)) {                                                       \
            fprintf(stderr, "%s:%d: requirement failed: %s\n",                   \
                    __FILE__,                                                     \
                    __LINE__,                                                     \
                    #condition);                                                  \
            return 1;                                                             \
        }                                                                          \
    } while (0)

static char observed[64];
static char captured[HUGE_BUF];

static void capture_log(const char *str) {
    snprintf(captured, sizeof(captured), "%s", str);
}

static bool sensitive_handler(const char *arg, char **errmsg) {
    if (strcmp(arg, "do-not-log") == 0) {
        size_t size = strlen(arg) + 32U;
        *errmsg = malloc(size);
        HARD_ASSERT(*errmsg != NULL);
        snprintf(*errmsg, size, "Rejected sensitive value: %s", arg);
        return false;
    }

    snprintf(observed, sizeof(observed), "%s", arg);
    return true;
}

static int test_crlf_category(void) {
    char path[] = "/tmp/atrinik-clioptions-test.XXXXXX";
    int fd = mkstemp(path);
    require(fd != -1);

    FILE *fp = fdopen(fd, "wb");
    require(fp != NULL);

    static const char config[] = "[general]\r\n"
                                  "secret = general-value\r\n"
                                  "[meta]\r\n"
                                  "secret = meta-value\r\n";
    require(fwrite(config, 1, sizeof(config) - 1, fp) == sizeof(config) - 1);
    require(fclose(fp) == 0);
    require(clioptions_load(path, "[general]"));
    require(strcmp(observed, "general-value") == 0);
    require(unlink(path) == 0);
    return 0;
}

static int test_startup_failure_latch(void) {
    toolkit_import(clioptions);
    clioption_t *option = clioptions_create("policy", sensitive_handler);
    clioptions_enable_argument(option);
    require(!clioptions_had_startup_errors());
    char *error = NULL;
    require(!clioptions_load_str("policy = <missing-atrinik-startup-option", &error));
    free(error);
    require(clioptions_had_startup_errors());
    error = NULL;
    require(clioptions_load_str("policy = valid", &error));
    require(clioptions_had_startup_errors());
    toolkit_deinit();

    toolkit_import(clioptions);
    require(!clioptions_had_startup_errors());
    option = clioptions_create("policy", sensitive_handler);
    clioptions_enable_argument(option);
    char executable[] = "test";
    char missing[] = "--policy=<missing-atrinik-startup-option";
    char valid[] = "--policy=valid";
    char *arguments[] = {executable, missing, valid};
    clioptions_parse(3, arguments);
    require(clioptions_had_startup_errors());
    toolkit_deinit();
#ifndef WIN32
    toolkit_import(clioptions);
    require(!clioptions_had_startup_errors());
    require(!clioptions_load(".", NULL));
    require(clioptions_had_startup_errors());
    toolkit_deinit();
#endif
    return 0;
}

static int test_command_line_only(void) {
    char config[] = "/tmp/atrinik-cli-capability-config.XXXXXX";
    int fd = mkstemp(config);
    require(fd >= 0);
    FILE *file = fdopen(fd, "w");
    require(file != NULL);
    require(fputs("capability = 17\n", file) > 0);
    require(fclose(file) == 0);
    char outer[] = "/tmp/atrinik-cli-capability-outer.XXXXXX";
    fd = mkstemp(outer);
    require(fd >= 0);
    file = fdopen(fd, "w");
    require(file != NULL);
    require(fprintf(file, "config = %s\n", config) > 0);
    require(fclose(file) == 0);
    char value[] = "/tmp/atrinik-cli-capability-value.XXXXXX";
    fd = mkstemp(value);
    require(fd >= 0);
    require(write(fd, "17", 2) == 2);
    require(close(fd) == 0);
    for (unsigned mode = 0; mode < 5; mode++) {
        toolkit_import(clioptions);
        clioption_t *option = clioptions_create("capability", sensitive_handler);
        clioptions_enable_argument(option);
        clioptions_enable_command_line_only(option);
        observed[0] = '\0';
        if (mode == 0) {
            char *error = NULL;
            require(!clioptions_load_str("capability = 17", &error));
            free(error);
        } else {
            char executable[] = "test";
            char argument[HUGE_BUF];
            if (mode == 1)
                snprintf(argument, sizeof(argument), "--capability=<%s", value);
            else if (mode == 2)
                snprintf(argument, sizeof(argument), "--config=%s", config);
            else if (mode == 3)
                snprintf(argument, sizeof(argument), "--config=%s", outer);
            else
                snprintf(argument, sizeof(argument), "--capability=17");
            char *arguments[] = {executable, argument};
            clioptions_parse(2, arguments);
        }
        require(clioptions_had_startup_errors() == (mode != 4));
        require(strcmp(observed, mode == 4 ? "17" : "") == 0);
        toolkit_deinit();
    }
    require(unlink(config) == 0);
    require(unlink(outer) == 0);
    require(unlink(value) == 0);
    return 0;
}

int main(void) {
    require(test_startup_failure_latch() == 0);
    require(test_command_line_only() == 0);
    toolkit_import(clioptions);

    clioption_t *option = clioptions_create("secret", sensitive_handler);
    clioptions_enable_argument(option);
    clioptions_enable_sensitive(option);
    clioptions_enable_changeable(option);

    char executable[] = "test";
    char argument[] = "--secret=correct horse battery staple";
    char *argv[] = {executable, argument};
    clioptions_parse(2, argv);

    const char *display = clioptions_get("secret");
    int failed = strcmp(observed, "correct horse battery staple") != 0 || display == NULL ||
                 strcmp(display, "<redacted>") != 0 || strstr(display, "horse") != NULL;

    char rejected_argument[] = "--secret=do-not-log";
    char *rejected_argv[] = {executable, rejected_argument};
    logger_set_print_func(capture_log);
    clioptions_parse(2, rejected_argv);
    failed |=
        strstr(captured, "do-not-log") != NULL || strstr(captured, "sensitive option") == NULL;

    char config_line[] = "secret = do-not-log";
    char *errmsg = NULL;
    failed |= clioptions_load_str(config_line, &errmsg);
    failed |= errmsg == NULL || strstr(errmsg, "do-not-log") != NULL ||
              strcmp(errmsg, "Failed to parse sensitive option") != 0;
    failed |= strcmp(clioptions_get("secret"), "<redacted>") != 0;

    logger_set_print_func(logger_do_print);
    failed |= test_crlf_category();
    if (errmsg != NULL) {
        OPENSSL_cleanse(errmsg, strlen(errmsg) + 1U);
    }
    free(errmsg);
    OPENSSL_cleanse(observed, sizeof(observed));
    OPENSSL_cleanse(captured, sizeof(captured));
    OPENSSL_cleanse(argument, sizeof(argument));
    OPENSSL_cleanse(rejected_argument, sizeof(rejected_argument));
    OPENSSL_cleanse(config_line, sizeof(config_line));
    toolkit_deinit();
    return failed;
}
