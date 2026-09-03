#include <toolkit/string.h>

#define require(condition)                                                        \
    do {                                                                          \
        if (!(condition)) {                                                       \
            fprintf(stderr, "%s:%d: requirement failed: %s\n",                  \
                    __FILE__,                                                     \
                    __LINE__,                                                     \
                    #condition);                                                  \
            return 1;                                                             \
        }                                                                         \
    } while (0)

static int test_replace_unprintable(void) {
    static const struct {
        const char *input;
        const char *expected;
    } cases[] = {
        {"caf\xc3\xa9 \xe4\xb8\x96 \xf0\x9f\x8c\x8d", "caf\xc3\xa9 \xe4\xb8\x96 \xf0\x9f\x8c\x8d"},
        {"\xc3\x28", " ("},
        {"\xe2\x82", "  "},
        {"\xc0\xaf", "  "},
        {"\xed\xa0\x80", "   "},
        {"\xf4\x90\x80\x80", "    "},
        {"\xff", " "},
        {"\x01", " "},
        {"\x7f", " "},
        {"\xc2\x80", "  "},
        {"\xe2\x80\x8b", "   "},
        {"\xe2\x80\xaa", "   "},
        {"\xe2\x81\xa0", "   "},
        {"\xef\xbb\xbf", "   "},
        {"\xef\xb7\x90", "   "},
        {"\xef\xbf\xbe", "   "},
    };

    for (size_t i = 0; i < arraysize(cases); i++) {
        char *value = xstrdup(cases[i].input);
        require(value != NULL);
        string_replace_unprintable_chars(value);
        require(strcmp(value, cases[i].expected) == 0);
        free(value);
    }

    return 0;
}

static int test_newline(void) {
    static char cases[][8] = {"line\n", "line\r\n", "\n", "line"};
    static const char *expected[] = {"line", "line", "", "line"};

    for (size_t i = 0; i < arraysize(cases); i++) {
        string_strip_newline(cases[i]);
        if (strcmp(cases[i], expected[i]) != 0) {
            return 1;
        }
    }

    return 0;
}

static int test_whitespace(void) {
    char value[] = "\xc3\xa9   \xe4\xb8\x96\t\xf0\x9f\x8c\x8d";
    string_whitespace_squeeze(value);
    require(strcmp(value, "\xc3\xa9 \xe4\xb8\x96 \xf0\x9f\x8c\x8d") == 0);

    char trimmed[] = "\t\xc3\xa9 \xe4\xb8\x96\t";
    string_whitespace_trim(trimmed);
    require(strcmp(trimmed, "\xc3\xa9 \xe4\xb8\x96") == 0);

    char words[] = "one \xc3\xa9";
    size_t position = 0;
    string_skip_word(words, &position, 1);
    require(position == 3);
    string_skip_word(words, &position, 1);
    require(position == 6);

    require(string_iswhite(" \t") == 1);
    require(string_iswhite("\xc3\xa9") == 0);
    return 0;
}

int main(void) {
    toolkit_import(string);
    int result = test_replace_unprintable();
    if (result == 0) {
        result = test_newline();
    }
    if (result == 0) {
        result = test_whitespace();
    }
    toolkit_deinit();
    return result;
}
