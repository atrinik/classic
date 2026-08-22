/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                 *
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

#include <toolkit/path.h>

#include <user_data.h>

#define TEST_CHECK(condition) \
    do {                      \
        if (!(condition)) {   \
            abort();          \
        }                     \
    } while (0)

static bool remove_tree(const char *path) {
    struct stat metadata;
#ifdef WIN32
    if (stat(path, &metadata) != 0) {
#else
    if (lstat(path, &metadata) != 0) {
#endif
        return errno == ENOENT;
    }

    if (S_ISDIR(metadata.st_mode)) {
        DIR *directory = opendir(path);
        if (directory == NULL) {
            return false;
        }

        bool complete = true;
        struct dirent *entry;
        bool read_error = false;
        for (;;) {
            errno = 0;
            entry = readdir(directory);
            if (entry == NULL) {
                read_error = errno != 0;
                break;
            }
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                char child[HUGE_BUF];
                if (snprintf(VS(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child) ||
                    !remove_tree(child)) {
                    complete = false;
                }
            }
        }
        complete = complete && !read_error && closedir(directory) == 0;
        return complete && rmdir(path) == 0;
    }

    return unlink(path) == 0;
}

static void write_file(const char *path, const char *contents) {
    path_ensure_directories(path);
    FILE *file = fopen(path, "wb");
    TEST_CHECK(file != NULL);
    TEST_CHECK(fwrite(contents, 1, strlen(contents), file) == strlen(contents));
    TEST_CHECK(fclose(file) == 0);
}

static void read_file(const char *path, char *contents, size_t contents_size) {
    FILE *file = fopen(path, "rb");
    TEST_CHECK(file != NULL);
    size_t length = fread(contents, 1, contents_size - 1U, file);
    TEST_CHECK(ferror(file) == 0);
    TEST_CHECK(fclose(file) == 0);
    contents[length] = '\0';
}

static bool is_directory(const char *path) {
    struct stat metadata;
    return stat(path, &metadata) == 0 && S_ISDIR(metadata.st_mode);
}

static void make_temp_root(char *root, size_t root_size) {
#ifdef WIN32
    char temporary[HUGE_BUF];
    DWORD length = GetTempPathA(sizeof(temporary), temporary);
    TEST_CHECK(length > 0 && length < sizeof(temporary));
    TEST_CHECK(snprintf(root,
                        root_size,
                        "%satrinik-user-data-%lu",
                        temporary,
                        (unsigned long)GetCurrentProcessId()) < (int)root_size);
    TEST_CHECK(CreateDirectoryA(root, NULL) != 0);
#else
    TEST_CHECK(snprintf(root, root_size, "/tmp/atrinik-user-data-XXXXXX") < (int)root_size);
    TEST_CHECK(mkdtemp(root) != NULL);
#endif
}

static void test_numeric_migration(const char *root) {
    char path[HUGE_BUF];
    char contents[128];

    snprintf(VS(path), "%s/.atrinik/5.10.9/settings/keys.dat", root);
    write_file(path, "older\n");
    snprintf(VS(path), "%s/.atrinik/5.9.999/settings/keys.dat", root);
    write_file(path, "older-minor\n");
    snprintf(VS(path), "%s/.atrinik/5.10.10/cache/compatible.dat", root);
    write_file(path, "cache\n");
    snprintf(VS(path), "%s/.atrinik/5.10.10/settings/keys.dat", root);
    write_file(path, "selected\n");
    snprintf(VS(path), "%s/.atrinik/5.10.10~/.ignored", root);
    write_file(path, "suffix\n");
    snprintf(VS(path), "%s/.atrinik/6.99/settings/keys.dat", root);
    write_file(path, "other-major\n");
    snprintf(VS(path), "%s/.atrinik/5.10.invalid/settings/keys.dat", root);
    write_file(path, "invalid\n");
    snprintf(VS(path), "%s/.atrinik/5.10~~/settings/keys.dat", root);
    write_file(path, "invalid-suffix\n");

    TEST_CHECK(user_data_prepare(root, 5, VS(path)));
    TEST_CHECK(strcmp(path, "/tmp/never") != 0);

    char expected[HUGE_BUF];
    snprintf(VS(expected), "%s/.atrinik/5.x", root);
    TEST_CHECK(strcmp(path, expected) == 0);

    snprintf(VS(path), "%s/.atrinik/5.x/settings/keys.dat", root);
    read_file(path, contents, sizeof(contents));
    TEST_CHECK(strcmp(contents, "selected\n") == 0);
    snprintf(VS(path), "%s/.atrinik/5.x/cache/compatible.dat", root);
    read_file(path, contents, sizeof(contents));
    TEST_CHECK(strcmp(contents, "cache\n") == 0);
    snprintf(VS(path), "%s/.atrinik/5.10.10", root);
    TEST_CHECK(!is_directory(path));
    snprintf(VS(path), "%s/.atrinik/5.10.10~", root);
    TEST_CHECK(is_directory(path));
    snprintf(VS(path), "%s/.atrinik/6.99", root);
    TEST_CHECK(is_directory(path));

    TEST_CHECK(user_data_prepare(root, 5, VS(path)));
    TEST_CHECK(strcmp(path, expected) == 0);
}

static void test_suffixed_candidate(const char *root) {
    char path[HUGE_BUF];
    char contents[128];

    snprintf(VS(path), "%s/.atrinik/5.13~/settings/keys.dat", root);
    write_file(path, "suffix-only\n");

    TEST_CHECK(user_data_prepare(root, 5, VS(path)));
    snprintf(VS(path), "%s/.atrinik/5.x/settings/keys.dat", root);
    read_file(path, contents, sizeof(contents));
    TEST_CHECK(strcmp(contents, "suffix-only\n") == 0);
    snprintf(VS(path), "%s/.atrinik/5.13~", root);
    TEST_CHECK(!is_directory(path));
    snprintf(VS(path), "%s/.atrinik/.5.x.migration", root);
    TEST_CHECK(access(path, F_OK) != 0);
}

static void test_existing_target_is_preserved(const char *root) {
    char path[HUGE_BUF];
    char contents[128];

    snprintf(VS(path), "%s/.atrinik/5.x/settings/keys.dat", root);
    write_file(path, "current\n");
    snprintf(VS(path), "%s/.atrinik/5.11/settings/keys.dat", root);
    write_file(path, "legacy\n");

    TEST_CHECK(user_data_prepare(root, 5, VS(path)));
    snprintf(VS(path), "%s/.atrinik/5.x/settings/keys.dat", root);
    read_file(path, contents, sizeof(contents));
    TEST_CHECK(strcmp(contents, "current\n") == 0);
    snprintf(VS(path), "%s/.atrinik/5.11/settings/keys.dat", root);
    read_file(path, contents, sizeof(contents));
    TEST_CHECK(strcmp(contents, "legacy\n") == 0);
}

static void test_no_candidate_creates_stable_directory(const char *root) {
    char path[HUGE_BUF];

    snprintf(VS(path), "%s/.atrinik/5.1.2.invalid", root);
    write_file(path, "not a directory\n");
    snprintf(VS(path), "%s/.atrinik/7.99", root);
    TEST_CHECK(path_ensure_real_directory(path, 0700) == PATH_DIRECTORY_OK);

    TEST_CHECK(user_data_prepare(root, 5, VS(path)));
    snprintf(VS(path), "%s/.atrinik/5.x", root);
    TEST_CHECK(is_directory(path));
}

static void test_conflicting_migration_is_recoverable(const char *root) {
    char path[HUGE_BUF];
    char contents[128];

    snprintf(VS(path), "%s/.atrinik/5.x/settings/keys.dat", root);
    write_file(path, "current\n");
    snprintf(VS(path), "%s/.atrinik/5.12/settings/keys.dat", root);
    write_file(path, "legacy\n");
    snprintf(VS(path), "%s/.atrinik/.5.x.migration", root);
    TEST_CHECK(path_secret_create_atomic(path, "5.12", strlen("5.12")) == PATH_SECRET_CREATE_OK);

    TEST_CHECK(user_data_prepare(root, 5, VS(path)));
    snprintf(VS(path), "%s/.atrinik/5.x/settings/keys.dat", root);
    read_file(path, contents, sizeof(contents));
    TEST_CHECK(strcmp(contents, "current\n") == 0);
    snprintf(VS(path), "%s/.atrinik/5.12/settings/keys.dat", root);
    read_file(path, contents, sizeof(contents));
    TEST_CHECK(strcmp(contents, "legacy\n") == 0);

    snprintf(VS(path), "%s/.atrinik/5.x/settings/keys.dat", root);
    TEST_CHECK(unlink(path) == 0);
    TEST_CHECK(user_data_prepare(root, 5, VS(path)));
    snprintf(VS(path), "%s/.atrinik/5.x/settings/keys.dat", root);
    read_file(path, contents, sizeof(contents));
    TEST_CHECK(strcmp(contents, "legacy\n") == 0);
    snprintf(VS(path), "%s/.atrinik/5.12", root);
    TEST_CHECK(!is_directory(path));
    snprintf(VS(path), "%s/.atrinik/.5.x.migration", root);
    TEST_CHECK(!is_directory(path));
    TEST_CHECK(access(path, F_OK) != 0);
}

int main(void) {
    char root[HUGE_BUF];

    toolkit_import(path);
    make_temp_root(root, sizeof(root));
    test_numeric_migration(root);

    char scenario[HUGE_BUF];
    TEST_CHECK(snprintf(VS(scenario), "%s/existing-target", root) < (int)sizeof(scenario));
    TEST_CHECK(path_ensure_real_directory(scenario, 0700) == PATH_DIRECTORY_OK);
    test_existing_target_is_preserved(scenario);

    TEST_CHECK(snprintf(VS(scenario), "%s/suffixed", root) < (int)sizeof(scenario));
    TEST_CHECK(path_ensure_real_directory(scenario, 0700) == PATH_DIRECTORY_OK);
    test_suffixed_candidate(scenario);

    TEST_CHECK(snprintf(VS(scenario), "%s/no-candidate", root) < (int)sizeof(scenario));
    TEST_CHECK(path_ensure_real_directory(scenario, 0700) == PATH_DIRECTORY_OK);
    test_no_candidate_creates_stable_directory(scenario);

    TEST_CHECK(snprintf(VS(scenario), "%s/recoverable", root) < (int)sizeof(scenario));
    TEST_CHECK(path_ensure_real_directory(scenario, 0700) == PATH_DIRECTORY_OK);
    test_conflicting_migration_is_recoverable(scenario);

    TEST_CHECK(remove_tree(root));
    toolkit_deinit();
    return 0;
}
