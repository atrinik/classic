#include <toolkit/clioptions.h>
#include <toolkit/logger.h>

#include <openssl/crypto.h>

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
    if (fd == -1) {
        return 1;
    }

    FILE *fp = fdopen(fd, "wb");
    if (fp == NULL) {
        close(fd);
        unlink(path);
        return 1;
    }

    static const char config[] = "[general]\r\n"
                                  "secret = general-value\r\n"
                                  "[meta]\r\n"
                                  "secret = meta-value\r\n";
    int failed = fwrite(config, 1, sizeof(config) - 1, fp) != sizeof(config) - 1;
    failed |= fclose(fp) != 0;
    if (!failed) {
        failed = !clioptions_load(path, "[general]") ||
                 strcmp(observed, "general-value") != 0;
    }
    failed |= unlink(path) != 0;
    return failed;
}

int main(void) {
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
