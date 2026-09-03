/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * Fork from Crossfire (Multiplayer game for X-windows).                 *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 *                                                                       *
 * This program is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 * GNU General Public License for more details.                          *
 *                                                                       *
 * You should have received a copy of the GNU General Public License     *
 * along with this program; if not, write to the Free Software           *
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.             *
 *                                                                       *
 * The author can be reached at admin@atrinik.org                        *
 ************************************************************************/

/**
 * @file
 * Handles help files.
 *
 * @author Zoey Rose
 */

#include <SDL3/SDL.h>
#include <book.h>
#include <main.h>
#include <popup.h>
#include <server_files.h>
#include <settings.h>
#include <text.h>
#include <text_input.h>
#include <toolkit/toolkit.h>
#include <widget.h>
#include <toolkit/string.h>

/**
 * Hashtable that contains the help files.
 */
static hfile_struct *hfiles = NULL;
/**
 * Array of command matches in console text input.
 */
static UT_array *command_matches = NULL;
/**
 * Index in ::command_matches to add to text input on the next tabulator
 * key press.
 */
static size_t command_index = 0;
/**
 * Last console string cache.
 */
static char command_buf[HUGE_BUF];

/**
 * Free a help file structure.
 */
static void hfile_free(hfile_struct *hfile) {
    free(hfile->key);

    free(hfile->msg);

    free(hfile);
}

/**
 * Frees the ::hfiles hashtable.
 */
void hfiles_deinit(void) {
    hfile_struct *hfile, *tmp;

    HASH_ITER(hh, hfiles, hfile, tmp) {
        HASH_DEL(hfiles, hfile);
        hfile_free(hfile);
    }

    if (command_matches) {
        utarray_free(command_matches);
        command_matches = NULL;
    }
}

/**
 * Parse help files from an open file.
 */
static void hfiles_load(FILE *fp) {
    char buf[HUGE_BUF], *key, *value;
    hfile_struct *hfile;
    StringBuffer *sb;

    HARD_ASSERT(fp != NULL);

    hfiles_deinit();
    hfile = NULL;

    while (fgets(buf, sizeof(buf), fp)) {
        key = buf;

        while (isspace(*key)) {
            key++;
        }

        string_strip_newline(buf);

        /* Empty line or a comment */
        if (*key == '\0' || *key == '#') {
            continue;
        }

        value = strchr(key, ' ');

        if (value != NULL) {
            *value = '\0';
            value++;

            while (isspace(*value)) {
                value++;
            }
        }

        if (hfile == NULL) {
            if (strcmp(key, "help") == 0 && !string_isempty(value)) {
                hfile = xcalloc(1, sizeof(*hfile));
                hfile->key = xstrdup(value);
            } else {
                LOG(DEVEL, "Unrecognised line: %s %s", buf, value ? value : "");
            }
        } else if (value == NULL) {
            if (strcmp(key, "msg") == 0) {
                sb = stringbuffer_new();

                if (hfile->msg != NULL) {
                    stringbuffer_append_string(sb, hfile->msg);
                    free(hfile->msg);
                }

                while (fgets(buf, sizeof(buf), fp)) {
                    bool has_newline = strchr(buf, '\n') != NULL;
                    string_strip_newline(buf);
                    if (strcmp(buf, "endmsg") == 0) {
                        break;
                    }

                    stringbuffer_append_string(sb, buf);
                    if (has_newline) {
                        stringbuffer_append_char(sb, '\n');
                    }
                }

                hfile->msg = stringbuffer_finish(sb);
            } else if (strcmp(key, "end") == 0) {
                if (hfile->msg != NULL) {
                    hfile->msg_len = strlen(hfile->msg);
                }

                HASH_ADD_KEYPTR(hh, hfiles, hfile->key, strlen(hfile->key), hfile);
                hfile = NULL;
            } else {
                LOG(DEVEL, "Unrecognised line: %s %s", buf, value ? value : "");
            }
        } else if (strcmp(key, "autocomplete") == 0) {
            hfile->autocomplete = atoi(value);
        } else if (strcmp(key, "autocomplete_wiz") == 0) {
            hfile->autocomplete_wiz = atoi(value);
        } else if (strcmp(key, "title") == 0) {
            sb = stringbuffer_new();
            stringbuffer_append_printf(sb, "[book]%s[/book]", value);
            hfile->msg = stringbuffer_finish(sb);
        } else {
            LOG(DEVEL, "Unrecognised line: %s %s", buf, value ? value : "");
        }
    }

    if (hfile != NULL) {
        LOG(BUG, "Help block without end: %s", hfile->key);
        hfile_free(hfile);
    }

    command_buf[0] = '\0';
    utarray_new(command_matches, &ut_str_icd);
}

/**
 * Read help files from file.
 */
void hfiles_init(void) {
    FILE *fp = server_file_open_name(SERVER_FILE_HFILES);

    if (fp == NULL) {
        LOG(BUG, "Could not open help files: %s", strerror(errno));
        return;
    }

    hfiles_load(fp);
    fclose(fp);
}

/**
 * Find a help file by its name.
 * @param name
 * Name of the help file to find.
 * @return
 * Help file if found, NULL otherwise.
 */
hfile_struct *help_find(const char *name) {
    hfile_struct *hfile;

    HASH_FIND_STR(hfiles, name, hfile);

    return hfile;
}

#ifdef ATRINIK_WIDGET_TESTS
#define HFILES_TEST_REQUIRE(condition)                                           \
    do {                                                                         \
        if (!(condition)) {                                                      \
            fprintf(stderr, "%s:%d: requirement failed: %s\n",                  \
                    __FILE__,                                                    \
                    __LINE__,                                                    \
                    #condition);                                                 \
            return false;                                                        \
        }                                                                        \
    } while (0)
static bool hfiles_parser_test_case(const char *input, const char *expected) {
    FILE *fp = tmpfile();
    HFILES_TEST_REQUIRE(fp != NULL);

    size_t input_len = strlen(input);
    HFILES_TEST_REQUIRE(fwrite(input, 1, input_len, fp) == input_len);
    HFILES_TEST_REQUIRE(fseek(fp, 0, SEEK_SET) == 0);

    hfiles_load(fp);
    hfile_struct *hfile = help_find("parser-test");
    hfile_struct *empty = help_find("empty");
    bool success = hfile != NULL && hfile->msg != NULL &&
                   strcmp(hfile->msg, expected) == 0 &&
                   hfile->msg_len == strlen(expected) &&
                   hfile->autocomplete == 1 &&
                   hfile->autocomplete_wiz == 1 &&
                   empty != NULL && empty->msg == NULL && empty->msg_len == 0;

    hfiles_deinit();
    HARD_ASSERT(fclose(fp) == 0);
    return success;
}

static bool hfiles_parser_incomplete_test(void) {
    static const char input[] = "help incomplete\n"
                                "msg\n"
                                "last line";
    FILE *fp = tmpfile();
    HFILES_TEST_REQUIRE(fp != NULL);
    size_t input_len = strlen(input);
    HFILES_TEST_REQUIRE(fwrite(input, 1, input_len, fp) == input_len);
    HFILES_TEST_REQUIRE(fseek(fp, 0, SEEK_SET) == 0);

    hfiles_load(fp);
    bool success = help_find("incomplete") == NULL;
    hfiles_deinit();
    HARD_ASSERT(fclose(fp) == 0);
    return success;
}

bool hfiles_parser_test(void) {
    static const char lf[] =
        " \t# ignored\n"
        "\n"
        "help\n"
        "unknown\n"
        "help parser-test\n"
        "autocomplete 1\n"
        "autocomplete_wiz 1\n"
        "title Title\n"
        "msg\n"
        "first line\n"
        "second line\n"
        "endmsg\n"
        "msg\n"
        "third line\n"
        "endmsg\n"
        "unknown value\n"
        "end\n"
        "help empty\n"
        "end\n";
    static const char crlf[] =
        " \t# ignored\r\n"
        "\r\n"
        "help\r\n"
        "unknown\r\n"
        "help parser-test\r\n"
        "autocomplete 1\r\n"
        "autocomplete_wiz 1\r\n"
        "title Title\r\n"
        "msg\r\n"
        "first line\r\n"
        "second line\r\n"
        "endmsg\r\n"
        "msg\r\n"
        "third line\r\n"
        "endmsg\r\n"
        "unknown value\r\n"
        "end\r\n"
        "help empty\r\n"
        "end\r\n";
    static const char expected[] = "[book]Title[/book]first line\n"
                                   "second line\n"
                                   "third line\n";

    return hfiles_parser_test_case(lf, expected) &&
           hfiles_parser_test_case(crlf, expected) &&
           hfiles_parser_incomplete_test();
}
#endif


/**
 * Show a help GUI.
 * @param name
 * Name of the help file entry to show.
 */
void help_show(const char *name) {
    hfile_struct *hfile;

    hfile = help_find(name);
    book_add_help_history(name);

    if (hfile == NULL) {
        char buf[HUGE_BUF];

        snprintf(VS(buf),
                 "[book]Help not found[/book][title]\n[center]The "
                 "specified help file could not be found.[/center]"
                 "[/title]");
        book_load(buf, strlen(buf));
    } else {
        book_load(hfile->msg, hfile->msg_len);
    }
}

/**
 * Comparison function used in help_handle_tabulator().
 */
static int command_match_cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/**
 * Handle tabulator key in console text input.
 */
void help_handle_tabulator(text_input_struct *text_input) {
    size_t len;
    char buf[sizeof(text_input->str)], *space;

    /* No help files or text input doesn't start with a forward slash. */
    if (command_matches == NULL || *text_input->str != '/') {
        return;
    }

    space = strrchr(text_input->str, ' ');

    /* Cannot have anything after a space (if any). */
    if (space != NULL && *(space + 1) != '\0') {
        return;
    }

    /* Does not match the previous command buffer, so rebuild the array. */
    if (strcmp(command_buf, text_input->str) != 0) {
        hfile_struct *hfile, *tmp;

        utarray_clear(command_matches);

        HASH_ITER(hh, hfiles, hfile, tmp) {
            if ((hfile->autocomplete ||
                 (setting_get_int(OPT_CAT_DEVEL, OPT_OPERATOR) && hfile->autocomplete_wiz)) &&
                strncasecmp(hfile->key, text_input->str + 1, text_input->num - 1) == 0) {

                utarray_push_back(command_matches, &hfile->key);
            }
        }

        utarray_sort(command_matches, command_match_cmp);
        command_index = 0;

        snprintf(VS(command_buf), "%s", text_input->str);
    }

    len = utarray_len(command_matches);

    if (len == 0) {
        return;
    }

    snprintf(VS(buf), "/%s ", *((char **)utarray_eltptr(command_matches, command_index)));
    text_input_set(text_input, buf);
    snprintf(VS(command_buf), "%s", buf);

    command_index++;

    if (command_index >= len) {
        command_index = 0;
    }
}
