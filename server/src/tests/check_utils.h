#ifndef CHECK_UTILS_H
#define CHECK_UTILS_H

/* src/tests/check.c */
extern void check_setup(void);
extern void check_teardown(void);
extern void check_test_setup(void);
extern void check_test_teardown(void);
extern void check_setup_env_pl(mapstruct **map, object **pl);
extern void check_run_suite(Suite *suite, const char *file);
extern int check_main(int argc, char **argv);

/* src/server/attack.c: deterministic seam for block-path unit tests. */
extern int attack_block_test_override;

#endif
