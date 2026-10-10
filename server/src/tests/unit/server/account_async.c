/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <global.h>
#include <server_main.h>
#include <initialization.h>
#include <server.h>
#include <account.h>
#include <auth_worker.h>
#include <access_server.h>
#include <player.h>
#include <object.h>
#include <commands.h>
#include <server_clock_fake.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <toolkit/packet.h>
#include <toolkit/password.h>
#include <toolkit/string.h>

#define ASYNC_CONNECTIONS (AUTH_WORKER_CAPACITY + 2)

static char fixture_name[MAX_BUF];
static char fixture_character[MAX_BUF];
static const char fixture_password[] = "local-async-7!";
static char registration_name[MAX_BUF];
static socket_struct connections[ASYNC_CONNECTIONS];
static char *account_path;
static char *fixture_player_path;
static char *registration_path;
static player *fixture_actor;
static char saved_default_groups[MAX_BUF];
static bool fixture_groups_saved;
static bool fixture_paths_owned;
static bool saved_access_required;

static char *read_account(void) {
    FILE *fp = fopen(account_path, "rb");
    ck_assert_ptr_nonnull(fp);
    char *contents = xcalloc(HUGE_BUF, 1);
    size_t length = fread(contents, 1, HUGE_BUF - 1, fp);
    ck_assert(!ferror(fp));
    ck_assert(feof(fp));
    contents[length] = '\0';
    ck_assert_int_eq(fclose(fp), 0);
    return contents;
}

/* Model a historical credential or an intervening administrative save in this
 * suite's provisioned private fixture, preserving characters and metrics. */
static void replace_fixture_credential(const char *credential) {
    char *contents = read_account();
    char *rest = strchr(contents, '\n');
    ck_assert_ptr_nonnull(rest);
    FILE *fp = fopen(account_path, "wb");
    ck_assert_ptr_nonnull(fp);
    ck_assert_int_gt(fprintf(fp, "%s%s", credential, rest), 0);
    ck_assert_int_eq(fclose(fp), 0);
    free(contents);
}

static uint64_t successful_logins(void) {
    metric_store_t metrics;
    metrics_store_init(&metrics, METRIC_SCOPE_ACCOUNT, 0);
    ck_assert(account_metrics_load(fixture_name, &metrics));
    uint64_t value = metrics_get(&metrics, METRIC_ACCOUNT_SUCCESSFUL_AUTHENTICATIONS);
    metrics_store_free(&metrics);
    return value;
}

static socket_struct *connection(size_t index) {
    ck_assert_uint_lt(index, arraysize(connections));
    socket_struct *ns = &connections[index];
    if (ns->sc == NULL) {
        ns->sc = socket_create("127.0.0.1", 0, SOCKET_ROLE_CLIENT, false);
        ck_assert_ptr_nonnull(ns->sc);
        ns->state = ST_LOGIN;
        ns->socket_version = SOCKET_VERSION;
        ns->access_policy_sent = true;
        ns->access_transport_authenticated = true;
        ns->access_authenticated = true;
        ns->setup_completed = true;
        socket_login_deadline_refresh(ns);
    }
    return ns;
}

static void login(socket_struct *ns, const char *password) {
    char name[MAX_BUF], secret[MAX_BUF];
    snprintf(VS(name), "%s", fixture_name);
    snprintf(VS(secret), "%s", password);
    account_login(ns, name, secret);
    /* Callers' buffers have no lifetime beyond submission. */
    memset(secret, 0, sizeof(secret));
}

static void register_account(socket_struct *ns, const char *password) {
    char name[MAX_BUF], secret[MAX_BUF], confirmation[MAX_BUF];
    snprintf(VS(name), "%s", registration_name);
    snprintf(VS(secret), "%s", password);
    snprintf(VS(confirmation), "%s", password);
    account_register(ns, name, secret, confirmation);
    memset(secret, 0, sizeof(secret));
    memset(confirmation, 0, sizeof(confirmation));
}

static bool has_message(socket_struct *ns, const char *message) {
    for (packet_struct *packet = ns->packets; packet != NULL; packet = packet->next) {
        if (packet->type != CLIENT_CMD_DRAWINFO || packet->len < 3) {
            continue;
        }
        /* DRAWINFO payload is type, color C string, message C string. */
        uint8_t *end = memchr(packet->data + 1, 0, packet->len - 1);
        if (end != NULL && end + 1 < packet->data + packet->len &&
            strstr((const char *)end + 1, message) != NULL) {
            return true;
        }
    }
    return false;
}

static void drain(void) {
    ck_assert_msg(auth_worker_wait_idle_for_test(), "authentication failed to finish");
    account_auth_poll();
    ck_assert_uint_eq(auth_worker_pending_for_test(), 0);
    for (size_t i = 0; i < arraysize(connections); i++) {
        ck_assert_uint_eq(connections[i].auth_request, 0);
    }
}

static void fixture_cleanup(void) {
    server_clock_fake_uninstall();
    account_fail_saves_for_test(false);
    auth_worker_pause_for_test(false);
    account_deinit();
    access_server_session_sequence_for_test(NULL, 0);
    settings.access_required = saved_access_required;
    if (fixture_groups_saved) {
        memcpy(settings.default_permission_groups, saved_default_groups,
               sizeof(saved_default_groups));
        fixture_groups_saved = false;
    }
    if (fixture_actor != NULL) {
        free_player(fixture_actor);
        fixture_actor = NULL;
    }
    for (size_t i = 0; i < arraysize(connections); i++) {
        account_auth_connection_clear(&connections[i]);
        socket_buffer_clear(&connections[i]);
        free(connections[i].account);
        if (connections[i].sc != NULL) {
            socket_destroy(connections[i].sc);
        }
    }
    memset(connections, 0, sizeof(connections));
    /* Finish every cleanup operation before reporting an error. Recover only
     * paths reserved by this fixture; a failed forked child has its own names. */
    char **paths[] = {&account_path, &fixture_player_path, &registration_path};
    int cleanup_error = 0;
    for (size_t i = 0; i < arraysize(paths); i++) {
        if (*paths[i] != NULL) {
            if (fixture_paths_owned && unlink(*paths[i]) != 0 && errno != ENOENT) {
                cleanup_error = errno;
            }
            free(*paths[i]);
            *paths[i] = NULL;
        }
    }
    fixture_paths_owned = false;
    ck_assert_msg(cleanup_error == 0, "fixture cleanup failed: %d", cleanup_error);
}

static void fixture_setup(void) {
    /* Check may skip checked teardown after an assertion. In CK_FORK=no,
     * recover this suite's previous owned resources before starting again. */
    if (account_path != NULL) {
        fixture_cleanup();
    }
    check_test_setup();
    saved_access_required = settings.access_required;
    settings.access_required = false;
    memset(connections, 0, sizeof(connections));
    socket_struct *seed = connection(0);
    const char *id = socket_get_id(seed->sc);
    char suffix[9];
    for (size_t i = 0; i < sizeof(suffix) - 1; i++) {
        unsigned char digit = (unsigned char)tolower((unsigned char)id[i]);
        suffix[i] = (char)('a' + (digit <= '9' ? digit - '0' : digit - 'a' + 10));
    }
    suffix[sizeof(suffix) - 1] = '\0';
    snprintf(VS(fixture_name), "async%s", suffix);
    snprintf(VS(registration_name), "reg%s", suffix);
    suffix[0] = (char)toupper((unsigned char)suffix[0]);
    snprintf(VS(fixture_character), "Async %s", suffix);
    account_path = account_make_path(fixture_name);
    fixture_player_path = player_make_path(fixture_character, "player.dat");
    registration_path = account_make_path(registration_name);
    struct stat statbuf;
    ck_assert_int_eq(stat(account_path, &statbuf), -1);
    ck_assert_int_eq(errno, ENOENT);
    ck_assert_int_eq(stat(fixture_player_path, &statbuf), -1);
    ck_assert_int_eq(errno, ENOENT);
    ck_assert_int_eq(stat(registration_path, &statbuf), -1);
    ck_assert_int_eq(errno, ENOENT);
    fixture_paths_owned = true;
    char error[HUGE_BUF];
    ck_assert_msg(account_provision(fixture_name, fixture_password, fixture_character,
                                   "human_male", VS(error)), "%s", error);
    account_init();
    ck_assert(account_auth_start());
}

static void fixture_teardown(void) {
    fixture_cleanup();
    check_test_teardown();
}

START_TEST(test_login_is_deferred_and_other_connection_stays_responsive) {
    char *before = read_account();
    auth_worker_pause_for_test(true);
    socket_struct *first = connection(0);
    socket_struct *second = connection(1);
    login(first, fixture_password);
    login(second, fixture_password);
    ck_assert_uint_ne(first->auth_request, 0);
    ck_assert_uint_ne(second->auth_request, 0);
    ck_assert(auth_worker_wait_running_for_test(AUTH_WORKER_THREADS));
    account_auth_poll();
    ck_assert_ptr_null(first->account);
    ck_assert_ptr_null(second->account);
    ck_assert_uint_eq(successful_logins(), 0);
    char *pending = read_account();
    ck_assert_str_eq(pending, before);
    free(pending);

    socket_struct *playing = connection(2);
    playing->state = ST_PLAYING;
    playing->keepalive = 37;
    uint8_t command[] = {SERVER_CMD_KEEPALIVE, 0, 0, 0, 42};
    ck_assert(socket_server_handle_command(playing, NULL, command, sizeof(command)));
    ck_assert_uint_eq(playing->keepalive, 0);
    ck_assert_uint_gt(playing->packet_queue_bytes, 0);
    ck_assert_ptr_null(first->account);
    ck_assert_ptr_null(second->account);

    auth_worker_pause_for_test(false);
    drain();
    ck_assert_str_eq(first->account, fixture_name);
    ck_assert_str_eq(second->account, fixture_name);
    ck_assert_uint_eq(successful_logins(), 2);
    free(before);
}
END_TEST

START_TEST(test_wrong_password_counts_only_completed_failures) {
    socket_struct *ns = connection(0);
    auth_worker_pause_for_test(true);
    login(ns, "wrong-password-8!");
    account_auth_poll();
    ck_assert_uint_eq(ns->password_fails, 0);
    ck_assert_ptr_null(ns->account);
    auth_worker_pause_for_test(false);
    drain();
    ck_assert_uint_eq(ns->password_fails, 1);
    ck_assert_ptr_null(ns->account);
    ck_assert(has_message(ns, "Invalid password"));
    ck_assert_uint_eq(successful_logins(), 0);
    for (size_t i = 1; i < MAX_PASSWORD_FAILURES; i++) {
        login(ns, "wrong-password-8!");
        drain();
    }
    ck_assert_uint_eq(ns->password_fails, MAX_PASSWORD_FAILURES);
    ck_assert_int_eq(ns->state, ST_ZOMBIE);
}
END_TEST

START_TEST(test_busy_and_duplicate_do_not_penalize_password) {
    auth_worker_pause_for_test(true);
    socket_struct *first = connection(0);
    login(first, fixture_password);
    uint64_t original_request = first->auth_request;
    ck_assert_uint_ne(original_request, 0);
    login(first, "wrong-password-8!");
    ck_assert_uint_eq(first->auth_request, original_request);
    ck_assert_uint_eq(first->password_fails, 0);
    ck_assert(has_message(first, "temporarily busy"));
    size_t admitted = 1;
    socket_struct *busy = NULL;
    for (size_t i = 1; i < arraysize(connections); i++) {
        socket_struct *ns = connection(i);
        login(ns, fixture_password);
        if (ns->auth_request == 0) {
            busy = ns;
            break;
        }
        admitted++;
    }
    ck_assert_ptr_nonnull(busy);
    ck_assert_uint_le(admitted, AUTH_WORKER_CAPACITY);
    ck_assert_uint_gt(admitted, 1);
    ck_assert(has_message(busy, "temporarily busy"));
    ck_assert_uint_eq(busy->password_fails, 0);
    ck_assert_int_eq(busy->state, ST_LOGIN);
    auth_worker_pause_for_test(false);
    drain();
    ck_assert_uint_eq(successful_logins(), admitted);
    ck_assert_ptr_null(busy->account);
}
END_TEST

START_TEST(test_disconnected_slot_reuse_discards_stale_work) {
    socket_struct *ns = connection(0);
    auth_worker_pause_for_test(true);
    login(ns, fixture_password);
    uint64_t old_generation = ns->auth_generation;
    ck_assert(auth_worker_wait_running_for_test(1));
    ck_assert_uint_ne(ns->auth_request, 0);
    account_auth_connection_clear(ns);
    ck_assert_uint_eq(ns->auth_request, 0);
    ck_assert_uint_eq(ns->auth_generation, 0);
    socket_buffer_clear(ns);
    socket_destroy(ns->sc);
    memset(ns, 0, sizeof(*ns));
    ns = connection(0);
    login(ns, "wrong-password-8!");
    ck_assert_uint_ne(ns->auth_generation, old_generation);
    auth_worker_pause_for_test(false);
    drain();
    ck_assert_ptr_null(ns->account);
    ck_assert_uint_eq(ns->password_fails, 1);
    ck_assert_uint_eq(successful_logins(), 0);
}
END_TEST

START_TEST(test_credentials_changed_while_pending_are_not_overwritten) {
    socket_struct *ns = connection(0);
    auth_worker_pause_for_test(true);
    login(ns, fixture_password);
    char record[PASSWORD_RECORD_SIZE];
    ck_assert(password_record_create("new-async-9!", record));
    char credential[PASSWORD_RECORD_SIZE + 16];
    snprintf(VS(credential), "password %s", record);
    replace_fixture_credential(credential);
    char *changed = read_account();
    auth_worker_pause_for_test(false);
    drain();
    ck_assert_ptr_null(ns->account);
    ck_assert_uint_eq(ns->password_fails, 0);
    ck_assert(has_message(ns, "credentials changed"));
    char *after = read_account();
    ck_assert_str_eq(after, changed);
    free(after);
    free(changed);
    login(ns, "new-async-9!");
    drain();
    ck_assert_str_eq(ns->account, fixture_name);
}
END_TEST

START_TEST(test_login_save_failure_never_publishes_account) {
    socket_struct *ns = connection(0);
    char *before = read_account();
    account_fail_saves_for_test(true);
    login(ns, fixture_password);
    drain();
    ck_assert_ptr_null(ns->account);
    ck_assert_uint_eq(ns->password_fails, 0);
    ck_assert(has_message(ns, "authentication was not completed"));
    char *after = read_account();
    ck_assert_str_eq(after, before);
    free(after);
    free(before);
    account_fail_saves_for_test(false);
    login(ns, fixture_password);
    drain();
    ck_assert_str_eq(ns->account, fixture_name);
    ck_assert_uint_eq(successful_logins(), 1);
}
END_TEST

START_TEST(test_pbkdf2_upgrade_is_transactional) {
    const unsigned char salt[32] = "salt";
    const unsigned char hash[32] = {
        0x72, 0xd3, 0xad, 0xf1, 0x1c, 0x90, 0x73, 0x6f, 0xf9, 0x7b, 0xbe,
        0x2c, 0x1f, 0xc0, 0x2a, 0xeb, 0xb9, 0xd1, 0x7b, 0xcd, 0x36, 0xb4,
        0x3e, 0x39, 0x0a, 0x7c, 0x40, 0x8c, 0x17, 0xe5, 0x8d, 0x37,
    };
    char salt_hex[65], hash_hex[65], credential[160];
    ck_assert_uint_eq(string_tohex(salt, sizeof(salt), VS(salt_hex), false), 64);
    ck_assert_uint_eq(string_tohex(hash, sizeof(hash), VS(hash_hex), false), 64);
    snprintf(VS(credential), "pswd %s\nsalt %s", hash_hex, salt_hex);
    replace_fixture_credential(credential);
    char *legacy = read_account();
    socket_struct *ns = connection(0);
    account_fail_saves_for_test(_i == 1);
    login(ns, "password");
    drain();
    char *after = read_account();
    if (_i == 1) {
        ck_assert_ptr_null(ns->account);
        ck_assert_str_eq(after, legacy);
    } else {
        ck_assert_str_eq(ns->account, fixture_name);
        ck_assert_ptr_nonnull(strstr(after, "password $argon2id$"));
        ck_assert_ptr_null(strstr(after, "pswd "));
        ck_assert_ptr_null(strstr(after, "salt "));
        char character_line[MAX_BUF + 32];
        snprintf(VS(character_line), "char human_male:%s::1", fixture_character);
        ck_assert_ptr_nonnull(strstr(after, character_line));
        ck_assert_uint_eq(successful_logins(), 1);
    }
    free(after);
    free(legacy);
}
END_TEST

START_TEST(test_registration_save_failure_rolls_back_reservation) {
    socket_struct *ns = connection(0);
    account_fail_saves_for_test(_i == 1);
    register_account(ns, fixture_password);
    ck_assert_uint_ne(ns->auth_request, 0);
    ck_assert_ptr_null(ns->account);
    drain();
    struct stat statbuf;
    if (_i == 1) {
        ck_assert_ptr_null(ns->account);
        ck_assert_int_eq(stat(registration_path, &statbuf), -1);
        ck_assert_int_eq(errno, ENOENT);
        account_fail_saves_for_test(false);
        register_account(ns, fixture_password);
        drain();
    }
    ck_assert_str_eq(ns->account, registration_name);
    ck_assert_int_eq(stat(registration_path, &statbuf), 0);
#ifndef WIN32
    ck_assert_uint_eq(statbuf.st_mode & 0777, SAVE_MODE);
#endif
}
END_TEST

START_TEST(test_simultaneous_registration_never_overwrites_winner) {
    auth_worker_pause_for_test(true);
    socket_struct *first = connection(0);
    socket_struct *second = connection(1);
    register_account(first, fixture_password);
    register_account(second, "other-async-8!");
    ck_assert_uint_ne(first->auth_request, 0);
    ck_assert_uint_ne(second->auth_request, 0);
    ck_assert(auth_worker_wait_running_for_test(2));
    auth_worker_pause_for_test(false);
    drain();
    ck_assert_int_eq((first->account != NULL) + (second->account != NULL), 1);
    socket_struct *winner = first->account != NULL ? first : second;
    socket_struct *loser = first->account != NULL ? second : first;
    ck_assert_str_eq(winner->account, registration_name);
    ck_assert(has_message(loser, "already registered"));
    FILE *fp = fopen(registration_path, "rb");
    ck_assert_ptr_nonnull(fp);
    char line[HUGE_BUF];
    ck_assert_ptr_nonnull(fgets(VS(line), fp));
    ck_assert_int_eq(fclose(fp), 0);
    ck_assert(strncmp(line, "password ", 9) == 0);
    char *end = strchr(line, '\n');
    ck_assert_ptr_nonnull(end);
    *end = '\0';
    const char *password = winner == first ? fixture_password : "other-async-8!";
    ck_assert_int_eq(password_record_verify(password, line + 9), PASSWORD_VERIFY_MATCH);
}
END_TEST

START_TEST(test_password_change_preserves_session_and_commits_atomically) {
    socket_struct *ns = connection(0);
    login(ns, fixture_password);
    drain();
    ck_assert_str_eq(ns->account, fixture_name);
    char *before = read_account();
    char old_password[MAX_BUF], new_password[] = "new-async-9!";
    char confirmation[] = "new-async-9!";
    snprintf(VS(old_password), "%s", fixture_password);
    account_fail_saves_for_test(_i == 1);
    auth_worker_pause_for_test(true);
    account_password_change(ns, old_password, new_password, confirmation);
    memset(old_password, 0, sizeof(old_password));
    memset(new_password, 0, sizeof(new_password));
    memset(confirmation, 0, sizeof(confirmation));
    ck_assert_uint_ne(ns->auth_request, 0);
    ck_assert_str_eq(ns->account, fixture_name);
    auth_worker_pause_for_test(false);
    drain();
    char *after = read_account();
    if (_i == 1) {
        ck_assert_str_eq(after, before);
        login(connection(1), fixture_password);
    } else {
        ck_assert(has_message(ns, "Password changed successfully"));
        login(connection(1), "new-async-9!");
    }
    account_fail_saves_for_test(false);
    drain();
    ck_assert_str_eq(connection(1)->account, fixture_name);
    ck_assert_str_eq(ns->account, fixture_name);
    free(before);
    free(after);
}
END_TEST

START_TEST(test_concurrent_password_changes_recheck_current_credential) {
    socket_struct *first = connection(0);
    socket_struct *second = connection(1);
    login(first, fixture_password);
    login(second, fixture_password);
    drain();
    char old_first[MAX_BUF], old_second[MAX_BUF];
    char new_first[] = "first-async-9!", confirm_first[] = "first-async-9!";
    char new_second[] = "second-async-9!", confirm_second[] = "second-async-9!";
    snprintf(VS(old_first), "%s", fixture_password);
    snprintf(VS(old_second), "%s", fixture_password);
    auth_worker_pause_for_test(true);
    account_password_change(first, old_first, new_first, confirm_first);
    account_password_change(second, old_second, new_second, confirm_second);
    ck_assert_uint_ne(first->auth_request, 0);
    ck_assert_uint_ne(second->auth_request, 0);
    ck_assert(auth_worker_wait_running_for_test(2));
    auth_worker_pause_for_test(false);
    drain();
    bool first_won = has_message(first, "Password changed successfully");
    bool second_won = has_message(second, "Password changed successfully");
    ck_assert_int_eq(first_won + second_won, 1);
    ck_assert(has_message(first_won ? second : first, "credentials changed"));
    ck_assert_str_eq(first->account, fixture_name);
    ck_assert_str_eq(second->account, fixture_name);
    login(connection(2), first_won ? new_first : new_second);
    drain();
    ck_assert_str_eq(connection(2)->account, fixture_name);
}
END_TEST

static player *reset_actor(void) {
    memcpy(saved_default_groups, settings.default_permission_groups,
           sizeof(saved_default_groups));
    fixture_groups_saved = true;
    settings.default_permission_groups[0] = '\0';
    player *pl = CONTR(player_get_dummy(NULL, NULL));
    fixture_actor = pl;
    pl->cmd_permissions = xcalloc(1, sizeof(*pl->cmd_permissions));
    pl->cmd_permissions[0] = xstrdup("password");
    pl->num_cmd_permissions = 1;
    ck_assert(commands_check_permission(pl, "password"));
    pl->cs->socket_version = SOCKET_VERSION;
    pl->cs->setup_completed = true;
    pl->cs->access_policy_sent = true;
    pl->cs->access_transport_authenticated = true;
    pl->cs->access_authenticated = true;
    pl->cs->state = ST_PLAYING;
    return pl;
}

START_TEST(test_password_reset_rechecks_actor_permission) {
    char *before = read_account();
    player *pl = reset_actor();
    object *actor = pl->ob;
    auth_worker_pause_for_test(true);
    char name[MAX_BUF];
    snprintf(VS(name), "%s", fixture_name);
    account_password_force(actor, name, "reset-async-8!");
    ck_assert_uint_ne(pl->cs->auth_request, 0);
    ck_assert(auth_worker_wait_running_for_test(1));
    free(pl->cmd_permissions[0]);
    pl->cmd_permissions[0] = NULL;
    ck_assert(!commands_check_permission(pl, "password"));
    account_auth_poll();
    ck_assert_uint_eq(pl->cs->auth_request, 0);
    auth_worker_pause_for_test(false);
    drain();
    char *after = read_account();
    ck_assert_str_eq(after, before);
    free(before);
    free(after);
    memcpy(settings.default_permission_groups, saved_default_groups,
           sizeof(saved_default_groups));
    fixture_groups_saved = false;
    free_player(pl);
    fixture_actor = NULL;
}
END_TEST

START_TEST(test_pending_request_timeout_discards_running_result) {
    server_clock_fake_install(UINT64_C(125000), server_tick_now(),
                              server_monotonic_now(), server_wall_utc_now());
    socket_struct *ns = connection(0);
    auth_worker_pause_for_test(true);
    login(ns, fixture_password);
    ck_assert(auth_worker_wait_running_for_test(1));
    server_clock_fake_advance_monotonic(server_duration_from_seconds(30));
    account_auth_poll();
    ck_assert_uint_eq(ns->auth_request, 0);
    ck_assert_ptr_null(ns->account);
    ck_assert(has_message(ns, "timed out"));
    auth_worker_pause_for_test(false);
    drain();
    ck_assert_ptr_null(ns->account);
    ck_assert_uint_eq(ns->password_fails, 0);
    ck_assert_uint_eq(successful_logins(), 0);
}
END_TEST

START_TEST(test_shutdown_clears_pending_work_and_can_restart) {
    socket_struct *ns = connection(0);
    auth_worker_pause_for_test(true);
    login(ns, fixture_password);
    ck_assert_uint_ne(ns->auth_request, 0);
    ck_assert(auth_worker_wait_running_for_test(1));
    account_deinit();
    ck_assert_uint_eq(ns->auth_request, 0);
    ck_assert_uint_eq(auth_worker_pending_for_test(), 0);
    ck_assert_ptr_null(ns->account);
    ck_assert_uint_eq(successful_logins(), 0);
    account_init();
    ck_assert(account_auth_start());
    login(ns, fixture_password);
    drain();
    ck_assert_str_eq(ns->account, fixture_name);
}
END_TEST

/* Exercise every async mutation through the same completion admission fence. */
static socket_struct *submit_admission_operation(int operation) {
    socket_struct *ns = connection(0);
    if (operation == 0) {
        login(ns, fixture_password);
    } else if (operation == 1) {
        register_account(ns, fixture_password);
    } else if (operation == 2) {
        ns->account = xstrdup(fixture_name);
        char old_password[MAX_BUF], password[] = "changed-async-9!";
        char confirmation[] = "changed-async-9!";
        snprintf(VS(old_password), "%s", fixture_password);
        account_password_change(ns, old_password, password, confirmation);
    } else {
        player *pl = reset_actor();
        char name[MAX_BUF];
        snprintf(VS(name), "%s", fixture_name);
        account_password_force(pl->ob, name, "changed-async-9!");
        ns = pl->cs;
    }
    ck_assert_uint_ne(ns->auth_request, 0);
    ck_assert(auth_worker_wait_idle_for_test());
    return ns;
}

static void assert_admission_unchanged(socket_struct *ns, const char *before, int operation) {
    char *after = read_account();
    ck_assert_str_eq(after, before);
    free(after);
    struct stat statbuf;
    ck_assert_int_eq(stat(registration_path, &statbuf), -1);
    ck_assert_int_eq(errno, ENOENT);
    if (operation < 2) {
        ck_assert_ptr_null(ns->account);
    } else {
        ck_assert_str_eq(ns->account, operation == 2 ? fixture_name : ACCOUNT_TESTING_NAME);
    }
    ck_assert_uint_eq(ns->password_fails, 0);
    ck_assert(!has_message(ns, "Password changed successfully"));
}

START_TEST(test_completed_operation_requires_current_access) {
    char *before = read_account();
    socket_struct *ns = submit_admission_operation(_i);
    settings.access_required = true;
    /* Revoked and expired tokens both return DENIED, before the later socket
     * poll has had a chance to mark this still-live connection dead. */
    const access_session_state_t sequence[] = {ACCESS_SESSION_DENIED};
    access_server_session_sequence_for_test(sequence, arraysize(sequence));
    account_auth_poll();
    ck_assert_uint_eq(ns->auth_request, 0);
    ck_assert_int_ne(ns->state, ST_DEAD);
    assert_admission_unchanged(ns, before, _i);
    free(before);
}
END_TEST

START_TEST(test_busy_completion_retries_current_access) {
    char *before = read_account();
    socket_struct *ns = submit_admission_operation(_i);
    uint64_t request = ns->auth_request;
    settings.access_required = true;
    const access_session_state_t sequence[] = {ACCESS_SESSION_BUSY, ACCESS_SESSION_BUSY,
                                                ACCESS_SESSION_VALID};
    access_server_session_sequence_for_test(sequence, arraysize(sequence));
    for (size_t i = 0; i < 2; i++) {
        account_auth_poll();
        ck_assert_uint_eq(ns->auth_request, request);
        ck_assert_uint_eq(auth_worker_pending_for_test(), 0);
        assert_admission_unchanged(ns, before, _i);
    }
    account_auth_poll();
    ck_assert_uint_eq(ns->auth_request, 0);
    if (_i < 2) {
        ck_assert_str_eq(ns->account, _i == 0 ? fixture_name : registration_name);
    } else {
        ck_assert(has_message(ns, "Password changed successfully"));
        ck_assert_str_eq(ns->account, _i == 2 ? fixture_name : ACCOUNT_TESTING_NAME);
    }
    char *after = read_account();
    if (_i != 1) {
        ck_assert_str_ne(after, before);
    }
    free(after);
    free(before);
}
END_TEST

START_TEST(test_busy_result_cannot_outlive_session_or_deadline) {
    server_clock_fake_install(UINT64_C(125000), server_tick_now(),
                              server_monotonic_now(), server_wall_utc_now());
    char *before = read_account();
    socket_struct *ns = submit_admission_operation(0);
    settings.access_required = true;
    const access_session_state_t sequence[] = {ACCESS_SESSION_BUSY, ACCESS_SESSION_VALID};
    access_server_session_sequence_for_test(sequence, arraysize(sequence));
    account_auth_poll();
    ck_assert_uint_ne(ns->auth_request, 0);
    if (_i == 0) {
        server_clock_fake_advance_monotonic(server_duration_from_seconds(30));
    } else if (_i == 1) {
        account_auth_connection_clear(ns);
    } else if (_i == 2) {
        ns->access_authenticated = false;
    } else {
        account_deinit();
    }
    account_auth_poll();
    ck_assert_uint_eq(ns->auth_request, 0);
    assert_admission_unchanged(ns, before, 0);
    free(before);
}
END_TEST

START_TEST(test_busy_result_rechecks_revocation_and_reset_permission) {
    char *before = read_account();
    socket_struct *ns = submit_admission_operation(_i == 0 ? 0 : 3);
    settings.access_required = true;
    const access_session_state_t sequence[] = {ACCESS_SESSION_BUSY, ACCESS_SESSION_DENIED};
    access_server_session_sequence_for_test(sequence, arraysize(sequence));
    account_auth_poll();
    ck_assert_uint_ne(ns->auth_request, 0);
    if (_i == 1) {
        free(fixture_actor->cmd_permissions[0]);
        fixture_actor->cmd_permissions[0] = NULL;
        /* Permission loss must reject even if the token is still valid. */
        const access_session_state_t valid[] = {ACCESS_SESSION_VALID};
        access_server_session_sequence_for_test(valid, arraysize(valid));
    }
    account_auth_poll();
    ck_assert_uint_eq(ns->auth_request, 0);
    assert_admission_unchanged(ns, before, _i == 0 ? 0 : 3);
    free(before);
}
END_TEST

START_TEST(test_busy_results_retain_capacity_reservations) {
    settings.access_required = true;
    access_session_state_t busy[AUTH_WORKER_QUEUE];
    for (size_t i = 0; i < arraysize(busy); i++) {
        login(connection(i), fixture_password);
        ck_assert_uint_ne(connection(i)->auth_request, 0);
        /* Avoid making this a race against the independent worker queue cap. */
        ck_assert(auth_worker_wait_idle_for_test());
        busy[i] = ACCESS_SESSION_BUSY;
    }
    access_server_session_sequence_for_test(busy, arraysize(busy));
    account_auth_poll();
    ck_assert_uint_eq(auth_worker_pending_for_test(), 0);
    for (size_t i = 0; i < arraysize(busy); i++) {
        ck_assert_uint_ne(connection(i)->auth_request, 0);
        ck_assert_ptr_null(connection(i)->account);
    }
    /* Reset only the rate budget so capacity is the limit under test. */
    account_init();
    auth_worker_pause_for_test(true);
    for (size_t i = arraysize(busy); i < AUTH_WORKER_CAPACITY; i++) {
        login(connection(i), fixture_password);
        ck_assert_uint_ne(connection(i)->auth_request, 0);
    }
    socket_struct *overflow = connection(AUTH_WORKER_CAPACITY);
    login(overflow, fixture_password);
    ck_assert_uint_eq(overflow->auth_request, 0);
    ck_assert(has_message(overflow, "temporarily busy"));
    ck_assert_uint_eq(overflow->password_fails, 0);
    for (size_t i = 0; i < AUTH_WORKER_CAPACITY; i++) {
        account_auth_connection_clear(connection(i));
    }
    auth_worker_pause_for_test(false);
    ck_assert(auth_worker_wait_idle_for_test());
    const access_session_state_t valid[] = {ACCESS_SESSION_VALID};
    access_server_session_sequence_for_test(valid, arraysize(valid));
    login(overflow, fixture_password);
    drain();
    ck_assert_str_eq(overflow->account, fixture_name);
    ck_assert_uint_eq(successful_logins(), 1);
}
END_TEST

START_TEST(test_public_completion_requires_transport_admission) {
    char *before = read_account();
    socket_struct *ns = submit_admission_operation(0);
    ck_assert(!settings.access_required);
    ns->access_transport_authenticated = false;
    account_auth_poll();
    ck_assert_uint_eq(ns->auth_request, 0);
    assert_admission_unchanged(ns, before, 0);
    free(before);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("account_async");
    TCase *tc = tcase_create("integration");
    tcase_add_unchecked_fixture(tc, check_setup, check_teardown);
    tcase_add_checked_fixture(tc, fixture_setup, fixture_teardown);
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_login_is_deferred_and_other_connection_stays_responsive);
    tcase_add_test(tc, test_wrong_password_counts_only_completed_failures);
    tcase_add_test(tc, test_busy_and_duplicate_do_not_penalize_password);
    tcase_add_test(tc, test_disconnected_slot_reuse_discards_stale_work);
    tcase_add_test(tc, test_credentials_changed_while_pending_are_not_overwritten);
    tcase_add_test(tc, test_login_save_failure_never_publishes_account);
    tcase_add_loop_test(tc, test_pbkdf2_upgrade_is_transactional, 0, 2);
    tcase_add_loop_test(tc, test_registration_save_failure_rolls_back_reservation, 0, 2);
    tcase_add_test(tc, test_simultaneous_registration_never_overwrites_winner);
    tcase_add_loop_test(tc, test_password_change_preserves_session_and_commits_atomically, 0, 2);
    tcase_add_test(tc, test_concurrent_password_changes_recheck_current_credential);
    tcase_add_test(tc, test_password_reset_rechecks_actor_permission);
    tcase_add_test(tc, test_pending_request_timeout_discards_running_result);
    tcase_add_test(tc, test_shutdown_clears_pending_work_and_can_restart);
    tcase_add_loop_test(tc, test_completed_operation_requires_current_access, 0, 4);
    tcase_add_loop_test(tc, test_busy_completion_retries_current_access, 0, 4);
    tcase_add_loop_test(tc, test_busy_result_cannot_outlive_session_or_deadline, 0, 4);
    tcase_add_test(tc, test_public_completion_requires_transport_admission);
    tcase_add_loop_test(tc, test_busy_result_rechecks_revocation_and_reset_permission, 0, 2);
    tcase_add_test(tc, test_busy_results_retain_capacity_reservations);
    suite_add_tcase(s, tc);
    return s;
}

void check_server_account_async(void) {
    check_run_suite(suite(), __FILE__);
}
