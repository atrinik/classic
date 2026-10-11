# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HTTPS = ROOT / "libatrinik/tests/rendezvous_admission_https.c"


@unittest.skipUnless(shutil.which("cc"), "A synthetic fixture C compiler is required")
class AdmissionPlatformTests(unittest.TestCase):
    """Test instrumentation fixtures; these do not qualify a native platform."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="admission-platform-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for path in ("curl", "toolkit", "openssl"):
            (self.root / path).mkdir()
        (self.root / "curl/curl.h").write_text('''#ifndef CURL_FIXTURE_H
#define CURL_FIXTURE_H
#include <stdlib.h>
typedef struct { int unused; } CURL;
typedef int CURLcode;
#define CURLOPT_CAINFO 1
#define CURLE_OK 0
CURL *curl_easy_init(void);
CURLcode curl_easy_setopt(CURL *, int, const char *);
void curl_easy_cleanup(CURL *);
#endif
''')
        for path in ("toolkit/logger.h", "openssl/crypto.h", "openssl/evp.h"):
            (self.root / path).touch()
        self.guards = ["TOOLKIT_RENDEZVOUS_ADMISSION_H", "TOOLKIT_DATETIME_H",
                       "TOOLKIT_METASERVER_URL_H", "TOOLKIT_STRING_H",
                       "TOOLKIT_SOCKET_PRIVATE_H"]

    def run_checked(self, command):
        result = subprocess.run(command, cwd=self.root, text=True,
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_source_wrap_redirects_actual_admission_and_restores_real_curl(self):
        for source_wrap in (False, True):
            options = ["-DATRINIK_TEST_SOURCE_WRAP"] if source_wrap else []
            output = self.run_checked([
                shutil.which("cc"), "-E", "-P", *options,
                *(f"-D{guard}" for guard in self.guards), "-I", str(self.root),
                str(HTTPS),
            ])
            admission = output.split("static rendezvous_admission_result_t\nadmission_prepare(", 1)[1]
            admission = admission.split("rendezvous_admission_result_t\nrendezvous_admission_prepare(", 1)[0]
            wrapper = output.split("CURL *__wrap_curl_easy_init(void) {", 1)[1].split(
                "static rendezvous_admission_result_t solve_fast", 1)[0]
            with self.subTest(source_wrap=source_wrap):
                called = "__wrap_curl_easy_init" if source_wrap else "curl_easy_init"
                real = "curl_easy_init" if source_wrap else "__real_curl_easy_init"
                self.assertIn(f"CURL *curl = {called}();", admission)
                self.assertIn(f"CURL *handle = {real}();", wrapper)
                self.assertNotIn("CURL *handle = __wrap_curl_easy_init();", wrapper)
                for option in ("CURLOPT_SSL_VERIFYPEER, 1L", "CURLOPT_SSL_VERIFYHOST, 2L",
                               "CURLOPT_FOLLOWLOCATION, 0L", "CURLOPT_MAXREDIRS, 0L"):
                    self.assertIn(option, admission)

    def test_source_wrapper_preserves_ca_selection_and_failure_cleanup(self):
        preamble = HTTPS.read_text().split("static rendezvous_admission_result_t solve_fast", 1)[0]
        # Only this fixture replaces dependencies. The preceding preprocessing
        # test separately exercises the actual included production source.
        preamble = preamble.replace('#include "../rendezvous_admission.c"', '''
#include <curl/curl.h>
CURL *fixture_admission_init(void) { return curl_easy_init(); }
''').replace('#include "../socket_private.h"', "")
        (self.root / "wrapper.c").write_text(preamble + '''
#include <assert.h>
#include <string.h>
static CURL fixture_handle;
static unsigned int init_calls, setopt_calls, cleanup_calls;
static int fail_init, fail_setopt;
static const char *selected_ca;
CURL *curl_easy_init(void) {
    assert(++init_calls < 10);
    return fail_init ? NULL : &fixture_handle;
}
CURLcode curl_easy_setopt(CURL *handle, int option, const char *ca) {
    assert(handle == &fixture_handle && option == CURLOPT_CAINFO);
    setopt_calls++;
    selected_ca = ca;
    return fail_setopt ? 1 : CURLE_OK;
}
void curl_easy_cleanup(CURL *handle) {
    assert(handle == &fixture_handle);
    cleanup_calls++;
}
int main(void) {
    (void)ticket;
    (void)server_id;
    (void)solver_calls;
    unsetenv("ATRINIK_TEST_ADMISSION_CA");
    assert(fixture_admission_init() == &fixture_handle);
    assert(init_calls == 1 && setopt_calls == 0 && cleanup_calls == 0);
    assert(setenv("ATRINIK_TEST_ADMISSION_CA", "fixture-ca.pem", 1) == 0);
    assert(fixture_admission_init() == &fixture_handle);
    assert(init_calls == 2 && setopt_calls == 1 && cleanup_calls == 0);
    assert(strcmp(selected_ca, "fixture-ca.pem") == 0);
    fail_setopt = 1;
    assert(fixture_admission_init() == NULL);
    assert(init_calls == 3 && setopt_calls == 2 && cleanup_calls == 1);
    fail_init = 1;
    assert(fixture_admission_init() == NULL);
    assert(init_calls == 4 && setopt_calls == 2 && cleanup_calls == 1);
    return 0;
}
''')
        binary = self.root / "wrapper"
        self.run_checked([shutil.which("cc"), "-std=c17", "-D_POSIX_C_SOURCE=200809L",
                          "-DATRINIK_TEST_SOURCE_WRAP", "-Wall", "-Wextra", "-Werror",
                          "-I", str(self.root), str(self.root / "wrapper.c"), "-o", str(binary)])
        self.run_checked([str(binary)])

    @unittest.skipUnless(shutil.which("cmake"), "CMake is required")
    def test_platform_configure_keeps_https_test_on_apple_without_gnu_wrap(self):
        text = (ROOT / "libatrinik/CMakeLists.txt").read_text()
        block = text.split('    if (UNIX AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")', 1)[1]
        block = 'if (UNIX AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")' + block.split(
            "    add_executable(libatrinik-rendezvous tests/rendezvous.c)", 1)[0]
        block = block.replace("tests/rendezvous_admission_https.c", str(HTTPS))
        for platform, unix, apple in (("Darwin", "ON", "ON"), ("Linux", "ON", "OFF"),
                                     ("Windows", "OFF", "OFF")):
            (self.root / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.21)
project(AdmissionPlatformFixture LANGUAGES C)
enable_testing()
set(UNIX {unix})
set(APPLE {apple})
set(CMAKE_C_COMPILER_ID Clang)
set(ATRINIK_TEST_OPENSSL "{self.root}/openssl-fixture")
add_library(Atrinik::Core INTERFACE IMPORTED)
function(atrinik_configure_target target)
endfunction()
{block}
if(TARGET libatrinik-rendezvous-admission-https)
    file(GENERATE OUTPUT "${{CMAKE_BINARY_DIR}}/definitions.txt"
        CONTENT "$<TARGET_PROPERTY:libatrinik-rendezvous-admission-https,COMPILE_DEFINITIONS>")
    file(GENERATE OUTPUT "${{CMAKE_BINARY_DIR}}/links.txt"
        CONTENT "$<TARGET_PROPERTY:libatrinik-rendezvous-admission-https,LINK_OPTIONS>")
endif()
''')
            build = self.root / platform
            self.run_checked([shutil.which("cmake"), "-S", str(self.root), "-B", str(build)])
            with self.subTest(platform=platform):
                if platform == "Windows":
                    self.assertFalse((build / "definitions.txt").exists())
                    continue
                tests = (build / "CTestTestfile.cmake").read_text()
                self.assertIn("libatrinik-rendezvous-admission-https", tests)
                self.assertIn("rendezvous_admission_https.py", tests)
                definitions = (build / "definitions.txt").read_text()
                links = (build / "links.txt").read_text()
                self.assertEqual("ATRINIK_TEST_SOURCE_WRAP" in definitions, platform == "Darwin")
                self.assertEqual("--wrap=curl_easy_init" in links, platform == "Linux")


if __name__ == "__main__":
    unittest.main()
