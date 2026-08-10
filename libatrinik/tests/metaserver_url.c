#include <toolkit/metaserver_url.h>

#define REQUIRE(expression)                    \
    do {                                       \
        if (!(expression)) {                   \
            fprintf(stderr, "%d\n", __LINE__); \
            return 1;                          \
        }                                      \
    } while (0)

static const char server_id[] = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

static int test_directory(void) {
    static const char *const accepted[] = {
        "https://classic.meta.atrinik.org/index.xml",
        "http://localhost:8787/test/index.xml",
        "http://127.0.0.1:8787/index.xml",
        "http://[::1]:8787/index.xml",
        "https://xn--bcher-kva.example.org/index.xml",
    };
    static const char *const rejected[] = {
        "",
        "ftp://classic.meta.atrinik.org/index.xml",
        "https://user@classic.meta.atrinik.org/index.xml",
        "https://classic.meta.atrinik.org/",
        "https://classic.meta.atrinik.org/index.xml?x=1",
        "https://classic.meta.atrinik.org/index.xml#x",
        "https://classic.meta.atrinik.org//index.xml",
        "https://classic.meta.atrinik.org/a/../index.xml",
        "https://classic.meta.atrinik.org/%69ndex.xml",
        "https://bad_host.example/index.xml",
        "https://xn--a.example.org/index.xml",
        "http://127.1:8787/index.xml",
        "http://2130706433:8787/index.xml",
        "http://[0:0::1]:8787/index.xml",
        "HTTPS://classic.meta.atrinik.org/index.xml",
        "https://rendezvous.meta.atrinik.org/v1/classic",
        "https://publish.meta.atrinik.org/index.xml",
    };
    for (size_t i = 0; i < arraysize(accepted); i++) {
        REQUIRE(metaserver_url_directory_valid(accepted[i]));
    }
    for (size_t i = 0; i < arraysize(rejected); i++) {
        REQUIRE(!metaserver_url_directory_valid(rejected[i]));
    }
    return 0;
}

static int test_hostname(void) {
    static const char *const accepted[] = {
        "play.example.net",
        "xn--bcher-kva.example.org",
        "server.999",
    };
    static const char *const rejected[] = {
        "example",
        "EXAMPLE.ORG",
        "example.org.",
        "127.1",
        "2130706433",
        "0x7f.0.0.1",
        "xn--a.example.org",
        "xn--0.example.org",
        "xn--0ca24w.example.org",
        "-bad.example",
        "bad-.example",
        "bad..example",
    };
    for (size_t i = 0; i < arraysize(accepted); i++) {
        if (!metaserver_hostname_valid(accepted[i])) {
            fprintf(stderr, "rejected hostname: %s\n", accepted[i]);
            return 1;
        }
    }
    for (size_t i = 0; i < arraysize(rejected); i++) {
        if (metaserver_hostname_valid(rejected[i])) {
            fprintf(stderr, "accepted hostname: %s\n", rejected[i]);
            return 1;
        }
    }
    return 0;
}

static int test_publish(void) {
    char url[MAX_BUF];
    char authority[MAX_BUF];
    REQUIRE(metaserver_url_publish("https://publish.meta.atrinik.org",
                                   "/v1/classic/servers/id/publish",
                                   VS(url),
                                   VS(authority)));
    REQUIRE(strcmp(url, "https://publish.meta.atrinik.org/v1/classic/servers/id/publish") == 0);
    REQUIRE(strcmp(authority, "publish.meta.atrinik.org") == 0);
    REQUIRE(metaserver_url_publish("http://127.0.0.1:8787/",
                                   "/v1/classic/servers/id/publish",
                                   VS(url),
                                   VS(authority)));
    REQUIRE(strcmp(authority, "127.0.0.1:8787") == 0);
    static const char *const rejected[] = {
        "https://publish.meta.atrinik.org/base",
        "https://publish.meta.atrinik.org?x=1",
        "https://user@publish.meta.atrinik.org",
        "wss://publish.meta.atrinik.org",
        "https://rendezvous.meta.atrinik.org",
        "https://classic.meta.atrinik.org",
    };
    for (size_t i = 0; i < arraysize(rejected); i++) {
        REQUIRE(!metaserver_url_publish(rejected[i], "/publish", VS(url), VS(authority)));
        REQUIRE(*url == '\0');
        REQUIRE(*authority == '\0');
    }
    static const char *const rejected_paths[] = {
        "publish",
        "/publish?x=1",
        "/publish#fragment",
        "/publish%2fother",
        "/a/../publish",
    };
    for (size_t i = 0; i < arraysize(rejected_paths); i++) {
        REQUIRE(!metaserver_url_publish("https://publish.meta.atrinik.org",
                                        rejected_paths[i],
                                        VS(url),
                                        VS(authority)));
        REQUIRE(*url == '\0');
        REQUIRE(*authority == '\0');
    }
    return 0;
}

static int test_rendezvous(void) {
    char url[MAX_BUF];
    REQUIRE(metaserver_url_rendezvous("https://rendezvous.meta.atrinik.org/v1/classic",
                                      server_id,
                                      "client",
                                      VS(url)));
    REQUIRE(strcmp(url,
                   "wss://rendezvous.meta.atrinik.org/v1/classic/servers/"
                   "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
                   "?role=client") == 0);
    REQUIRE(metaserver_url_rendezvous("http://127.0.0.1:8787/base/", server_id, "server", VS(url)));
    REQUIRE(strcmp(url,
                   "ws://127.0.0.1:8787/base/servers/"
                   "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
                   "?role=server") == 0);
    REQUIRE(metaserver_url_rendezvous("http://[::1]:8787/base", server_id, "client", VS(url)));
    REQUIRE(strcmp(url,
                   "ws://[::1]:8787/base/servers/"
                   "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
                   "?role=client") == 0);
    char tiny[4] = "bad";
    REQUIRE(!metaserver_url_rendezvous("https://rendezvous.meta.atrinik.org/v1/classic",
                                       server_id,
                                       "client",
                                       VS(tiny)));
    REQUIRE(*tiny == '\0');
    REQUIRE(!metaserver_url_rendezvous("https://rendezvous.meta.atrinik.org/v1/classic",
                                       "ABC",
                                       "client",
                                       VS(url)));
    REQUIRE(*url == '\0');
    REQUIRE(!metaserver_url_rendezvous("https://rendezvous.meta.atrinik.org/v1/classic",
                                       server_id,
                                       "admin",
                                       VS(url)));
    REQUIRE(!metaserver_url_rendezvous("https://classic.meta.atrinik.org/index.xml",
                                       server_id,
                                       "client",
                                       VS(url)));
    REQUIRE(!metaserver_url_rendezvous("https://publish.meta.atrinik.org",
                                       server_id,
                                       "server",
                                       VS(url)));
    return 0;
}

int main(void) {
    return test_directory() || test_hostname() || test_publish() || test_rendezvous();
}
