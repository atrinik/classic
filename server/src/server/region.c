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
 * Region management and immutable inherited celestial environments.
 */

#include <global.h>
#include <server_main.h>
#include <region.h>
#include <initialization.h>
#include <toolkit/string.h>
#include <object.h>
#include <light.h>
#include <openssl/evp.h>
#include <stdarg.h>

enum celestial_field {
    CELESTIAL_SCHEMA,
    CELESTIAL_SOLAR_MODE,
    CELESTIAL_SOLAR_RATE,
    CELESTIAL_SOLAR_EPOCH,
    CELESTIAL_SOLAR_PHASE,
    CELESTIAL_SEASON_MODE,
    CELESTIAL_SEASON_RATE,
    CELESTIAL_SEASON_EPOCH,
    CELESTIAL_SEASON_PHASE,
    CELESTIAL_LUNAR_MODE,
    CELESTIAL_LUNAR_RATE,
    CELESTIAL_LUNAR_EPOCH,
    CELESTIAL_LUNAR_PHASE,
    CELESTIAL_LUNAR_PERIOD,
    CELESTIAL_DAY_COLOR,
    CELESTIAL_NIGHT_COLOR,
    CELESTIAL_DAY_BRIGHTNESS,
    CELESTIAL_NIGHT_BRIGHTNESS,
    CELESTIAL_MOON_COLOR,
    CELESTIAL_MOON_MAX,
    CELESTIAL_STARLIGHT_COLOR,
    CELESTIAL_STARLIGHT_STRENGTH,
    CELESTIAL_FIELD_COUNT,
};

#define CELESTIAL_ALL_FIELDS ((UINT32_C(1) << CELESTIAL_FIELD_COUNT) - 1)

static const char *const celestial_keys[CELESTIAL_FIELD_COUNT] = {
    [CELESTIAL_SCHEMA] = "celestial_schema",
    [CELESTIAL_SOLAR_MODE] = "celestial_solar_mode",
    [CELESTIAL_SOLAR_RATE] = "celestial_solar_rate",
    [CELESTIAL_SOLAR_EPOCH] = "celestial_solar_epoch",
    [CELESTIAL_SOLAR_PHASE] = "celestial_solar_phase",
    [CELESTIAL_SEASON_MODE] = "celestial_season_mode",
    [CELESTIAL_SEASON_RATE] = "celestial_season_rate",
    [CELESTIAL_SEASON_EPOCH] = "celestial_season_epoch",
    [CELESTIAL_SEASON_PHASE] = "celestial_season_phase",
    [CELESTIAL_LUNAR_MODE] = "celestial_lunar_mode",
    [CELESTIAL_LUNAR_RATE] = "celestial_lunar_rate",
    [CELESTIAL_LUNAR_EPOCH] = "celestial_lunar_epoch",
    [CELESTIAL_LUNAR_PHASE] = "celestial_lunar_phase",
    [CELESTIAL_LUNAR_PERIOD] = "celestial_lunar_period",
    [CELESTIAL_DAY_COLOR] = "celestial_day_color",
    [CELESTIAL_NIGHT_COLOR] = "celestial_night_color",
    [CELESTIAL_DAY_BRIGHTNESS] = "celestial_day_brightness",
    [CELESTIAL_NIGHT_BRIGHTNESS] = "celestial_night_brightness",
    [CELESTIAL_MOON_COLOR] = "celestial_moon_color",
    [CELESTIAL_MOON_MAX] = "celestial_moon_max",
    [CELESTIAL_STARLIGHT_COLOR] = "celestial_starlight_color",
    [CELESTIAL_STARLIGHT_STRENGTH] = "celestial_starlight_strength",
};

/** First region. */
region_struct *first_region = NULL;

static region_struct *region_get(void);
static void region_free(region_struct *region);
static void region_list_free(region_struct *regions);
static region_struct *region_find_in_list(region_struct *regions, const char *name);
static bool region_resolve(region_struct *region, char *error, size_t error_size);

static bool region_error(char *error, size_t error_size, const char *format, ...) {
    if (error != NULL && error_size != 0) {
        va_list ap;
        va_start(ap, format);
        vsnprintf(error, error_size, format, ap);
        va_end(ap);
    }
    return false;
}

static bool parse_u64(const char *value, uint64_t maximum, uint64_t *result) {
    if (string_isempty(value) || !isdigit((unsigned char)value[0])) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    uintmax_t parsed = strtoumax(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' || parsed > maximum) {
        return false;
    }
    *result = (uint64_t)parsed;
    return true;
}

static uint8_t gcd_u8(uint8_t left, uint8_t right) {
    while (right != 0) {
        uint8_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

static bool parse_rate(const char *value, uint8_t *numerator, uint8_t *denominator) {
    const char *separator = strchr(value, '/');
    if (separator == NULL || separator == value || strchr(separator + 1, '/') != NULL) {
        return false;
    }
    char left[4], right[4];
    size_t left_length = (size_t)(separator - value);
    size_t right_length = strlen(separator + 1);
    if (left_length >= sizeof(left) || right_length == 0 || right_length >= sizeof(right)) {
        return false;
    }
    memcpy(left, value, left_length);
    left[left_length] = '\0';
    memcpy(right, separator + 1, right_length + 1);
    uint64_t n, d;
    if (!parse_u64(left, UINT8_MAX, &n) || !parse_u64(right, UINT8_MAX, &d)) {
        return false;
    }
    *numerator = (uint8_t)n;
    *denominator = (uint8_t)d;
    return true;
}

static bool parse_mode(const char *value, region_celestial_mode_t *mode) {
    if (strcmp(value, "global") == 0) {
        *mode = REGION_CELESTIAL_MODE_GLOBAL;
    } else if (strcmp(value, "scaled") == 0) {
        *mode = REGION_CELESTIAL_MODE_SCALED;
    } else if (strcmp(value, "fixed") == 0) {
        *mode = REGION_CELESTIAL_MODE_FIXED;
    } else {
        return false;
    }
    return true;
}

static const char *mode_name(region_celestial_mode_t mode) {
    switch (mode) {
        case REGION_CELESTIAL_MODE_GLOBAL:
            return "global";
        case REGION_CELESTIAL_MODE_SCALED:
            return "scaled";
        case REGION_CELESTIAL_MODE_FIXED:
            return "fixed";
        default:
            return "unset";
    }
}

static int celestial_field(const char *key) {
    for (size_t i = 0; i < arraysize(celestial_keys); i++) {
        if (strcmp(key, celestial_keys[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static region_celestial_cycle_t *field_cycle(region_celestial_profile_t *profile,
                                             enum celestial_field field) {
    if (field >= CELESTIAL_SOLAR_MODE && field <= CELESTIAL_SOLAR_PHASE) {
        return &profile->solar;
    }
    if (field >= CELESTIAL_SEASON_MODE && field <= CELESTIAL_SEASON_PHASE) {
        return &profile->season;
    }
    if (field >= CELESTIAL_LUNAR_MODE && field <= CELESTIAL_LUNAR_PHASE) {
        return &profile->lunar;
    }
    return NULL;
}

static const region_celestial_cycle_t *field_cycle_const(const region_celestial_profile_t *profile,
                                                         enum celestial_field field) {
    if (field >= CELESTIAL_SOLAR_MODE && field <= CELESTIAL_SOLAR_PHASE) {
        return &profile->solar;
    }
    if (field >= CELESTIAL_SEASON_MODE && field <= CELESTIAL_SEASON_PHASE) {
        return &profile->season;
    }
    if (field >= CELESTIAL_LUNAR_MODE && field <= CELESTIAL_LUNAR_PHASE) {
        return &profile->lunar;
    }
    return NULL;
}

static bool parse_celestial(region_struct *region,
                            enum celestial_field field,
                            const char *value,
                            char *error,
                            size_t error_size) {
    uint32_t bit = UINT32_C(1) << field;
    if ((region->celestial_fields & bit) != 0) {
        return region_error(error,
                            error_size,
                            "region %s duplicates %s",
                            region->name,
                            celestial_keys[field]);
    }

    region_celestial_profile_t *profile = &region->celestial_authored;
    region_celestial_cycle_t *cycle = field_cycle(profile, field);
    uint64_t parsed;
    bool valid = false;
    switch (field) {
        case CELESTIAL_SCHEMA:
            valid = parse_u64(value, UINT8_MAX, &parsed);
            if (valid) {
                profile->schema = (uint8_t)parsed;
            }
            break;
        case CELESTIAL_SOLAR_MODE:
        case CELESTIAL_SEASON_MODE:
        case CELESTIAL_LUNAR_MODE:
            valid = parse_mode(value, &cycle->mode);
            break;
        case CELESTIAL_SOLAR_RATE:
        case CELESTIAL_SEASON_RATE:
        case CELESTIAL_LUNAR_RATE:
            valid = parse_rate(value, &cycle->rate_numerator, &cycle->rate_denominator);
            break;
        case CELESTIAL_SOLAR_EPOCH:
        case CELESTIAL_SEASON_EPOCH:
        case CELESTIAL_LUNAR_EPOCH:
            valid = parse_u64(value, UINT64_MAX, &cycle->epoch);
            break;
        case CELESTIAL_SOLAR_PHASE:
        case CELESTIAL_SEASON_PHASE:
        case CELESTIAL_LUNAR_PHASE:
            valid = parse_u64(value, UINT16_MAX, &parsed);
            if (valid) {
                cycle->phase = (uint16_t)parsed;
            }
            break;
        case CELESTIAL_LUNAR_PERIOD:
            valid = parse_u64(value, UINT16_MAX, &parsed);
            if (valid) {
                profile->lunar_period = (uint16_t)parsed;
            }
            break;
        case CELESTIAL_DAY_COLOR:
            valid = light_color_parse(value, &profile->day_color);
            break;
        case CELESTIAL_NIGHT_COLOR:
            valid = light_color_parse(value, &profile->night_color);
            break;
        case CELESTIAL_DAY_BRIGHTNESS:
            valid = parse_u64(value, UINT16_MAX, &parsed);
            if (valid) {
                profile->day_brightness = (uint16_t)parsed;
            }
            break;
        case CELESTIAL_NIGHT_BRIGHTNESS:
            valid = parse_u64(value, UINT16_MAX, &parsed);
            if (valid) {
                profile->night_brightness = (uint16_t)parsed;
            }
            break;
        case CELESTIAL_MOON_COLOR:
            valid = light_color_parse(value, &profile->moon_color);
            break;
        case CELESTIAL_MOON_MAX:
            valid = parse_u64(value, UINT8_MAX, &parsed);
            if (valid) {
                profile->moon_max = (uint8_t)parsed;
            }
            break;
        case CELESTIAL_STARLIGHT_COLOR:
            valid = light_color_parse(value, &profile->starlight_color);
            break;
        case CELESTIAL_STARLIGHT_STRENGTH:
            valid = parse_u64(value, UINT8_MAX, &parsed);
            if (valid) {
                profile->starlight_strength = (uint8_t)parsed;
            }
            break;
        default:
            break;
    }
    if (!valid) {
        return region_error(error,
                            error_size,
                            "region %s has invalid %s value '%s'",
                            region->name,
                            celestial_keys[field],
                            value);
    }
    region->celestial_fields |= bit;
    return true;
}

static void inherit_field(region_celestial_profile_t *effective,
                          const region_celestial_profile_t *authored,
                          enum celestial_field field) {
    region_celestial_cycle_t *destination = field_cycle(effective, field);
    const region_celestial_cycle_t *source = field_cycle_const(authored, field);
    switch (field) {
        case CELESTIAL_SCHEMA:
            effective->schema = authored->schema;
            break;
        case CELESTIAL_SOLAR_MODE:
        case CELESTIAL_SEASON_MODE:
        case CELESTIAL_LUNAR_MODE:
            destination->mode = source->mode;
            break;
        case CELESTIAL_SOLAR_RATE:
        case CELESTIAL_SEASON_RATE:
        case CELESTIAL_LUNAR_RATE:
            destination->rate_numerator = source->rate_numerator;
            destination->rate_denominator = source->rate_denominator;
            break;
        case CELESTIAL_SOLAR_EPOCH:
        case CELESTIAL_SEASON_EPOCH:
        case CELESTIAL_LUNAR_EPOCH:
            destination->epoch = source->epoch;
            break;
        case CELESTIAL_SOLAR_PHASE:
        case CELESTIAL_SEASON_PHASE:
        case CELESTIAL_LUNAR_PHASE:
            destination->phase = source->phase;
            break;
        case CELESTIAL_LUNAR_PERIOD:
            effective->lunar_period = authored->lunar_period;
            break;
        case CELESTIAL_DAY_COLOR:
            effective->day_color = authored->day_color;
            break;
        case CELESTIAL_NIGHT_COLOR:
            effective->night_color = authored->night_color;
            break;
        case CELESTIAL_DAY_BRIGHTNESS:
            effective->day_brightness = authored->day_brightness;
            break;
        case CELESTIAL_NIGHT_BRIGHTNESS:
            effective->night_brightness = authored->night_brightness;
            break;
        case CELESTIAL_MOON_COLOR:
            effective->moon_color = authored->moon_color;
            break;
        case CELESTIAL_MOON_MAX:
            effective->moon_max = authored->moon_max;
            break;
        case CELESTIAL_STARLIGHT_COLOR:
            effective->starlight_color = authored->starlight_color;
            break;
        case CELESTIAL_STARLIGHT_STRENGTH:
            effective->starlight_strength = authored->starlight_strength;
            break;
        default:
            break;
    }
}

static bool validate_cycle(const char *name,
                           const region_celestial_cycle_t *cycle,
                           uint16_t period,
                           char *error,
                           size_t error_size) {
    bool valid = cycle->phase < period;
    switch (cycle->mode) {
        case REGION_CELESTIAL_MODE_GLOBAL:
            valid = valid && cycle->rate_numerator == 1 && cycle->rate_denominator == 1 &&
                    cycle->epoch == 0 && cycle->phase == 0;
            break;
        case REGION_CELESTIAL_MODE_SCALED:
            valid = valid && cycle->rate_numerator >= 1 && cycle->rate_numerator <= 16 &&
                    cycle->rate_denominator >= 1 && cycle->rate_denominator <= 16 &&
                    gcd_u8(cycle->rate_numerator, cycle->rate_denominator) == 1;
            break;
        case REGION_CELESTIAL_MODE_FIXED:
            valid = valid && cycle->rate_numerator == 0 && cycle->rate_denominator == 1 &&
                    cycle->epoch == 0;
            break;
        default:
            valid = false;
            break;
    }
    return valid || region_error(error, error_size, "invalid effective %s cycle", name);
}

static bool profile_digest(region_celestial_profile_t *profile, char *error, size_t error_size) {
    char serialized[2048];
    int length = snprintf(serialized,
                          sizeof(serialized),
                          "atrinik-celestial-profile-v1\n"
                          "celestial_schema=%u\n"
                          "celestial_solar_mode=%s\n"
                          "celestial_solar_rate=%u/%u\n"
                          "celestial_solar_epoch=%" PRIu64 "\n"
                          "celestial_solar_phase=%u\n"
                          "celestial_season_mode=%s\n"
                          "celestial_season_rate=%u/%u\n"
                          "celestial_season_epoch=%" PRIu64 "\n"
                          "celestial_season_phase=%u\n"
                          "celestial_lunar_mode=%s\n"
                          "celestial_lunar_rate=%u/%u\n"
                          "celestial_lunar_epoch=%" PRIu64 "\n"
                          "celestial_lunar_phase=%u\n"
                          "celestial_lunar_period=%u\n"
                          "celestial_day_color=%06" PRIx32 "\n"
                          "celestial_night_color=%06" PRIx32 "\n"
                          "celestial_day_brightness=%u\n"
                          "celestial_night_brightness=%u\n"
                          "celestial_moon_color=%06" PRIx32 "\n"
                          "celestial_moon_max=%u\n"
                          "celestial_starlight_color=%06" PRIx32 "\n"
                          "celestial_starlight_strength=%u\n",
                          profile->schema,
                          mode_name(profile->solar.mode),
                          profile->solar.rate_numerator,
                          profile->solar.rate_denominator,
                          profile->solar.epoch,
                          profile->solar.phase,
                          mode_name(profile->season.mode),
                          profile->season.rate_numerator,
                          profile->season.rate_denominator,
                          profile->season.epoch,
                          profile->season.phase,
                          mode_name(profile->lunar.mode),
                          profile->lunar.rate_numerator,
                          profile->lunar.rate_denominator,
                          profile->lunar.epoch,
                          profile->lunar.phase,
                          profile->lunar_period,
                          profile->day_color,
                          profile->night_color,
                          profile->day_brightness,
                          profile->night_brightness,
                          profile->moon_color,
                          profile->moon_max,
                          profile->starlight_color,
                          profile->starlight_strength);
    if (length < 0 || (size_t)length >= sizeof(serialized)) {
        return region_error(error, error_size, "celestial profile serialization overflow");
    }

    unsigned int digest_length = 0;
    if (EVP_Digest(serialized,
                   (size_t)length,
                   profile->digest,
                   &digest_length,
                   EVP_sha256(),
                   NULL) != 1 ||
        digest_length != sizeof(profile->digest) ||
        string_tohex(profile->digest,
                     sizeof(profile->digest),
                     profile->digest_hex,
                     sizeof(profile->digest_hex),
                     false) != sizeof(profile->digest) * 2U) {
        return region_error(error, error_size, "failed to digest celestial profile");
    }
    for (size_t i = 0; i < sizeof(profile->digest_hex) - 1; i++) {
        profile->digest_hex[i] = (char)tolower((unsigned char)profile->digest_hex[i]);
    }
    profile->revision = 0;
    for (size_t i = 0; i < sizeof(profile->revision); i++) {
        profile->revision = (profile->revision << 8) | profile->digest[i];
    }
    return true;
}

static void profile_lunar_input(const region_celestial_profile_t *profile,
                                const region_celestial_phases_t *phases,
                                celestial_lunar_input *input) {
    memset(input, 0, sizeof(*input));
    input->solar_hour = phases->solar;
    input->season_phase = phases->season;
    input->lunar_age = phases->lunar;
    input->lunar_period = profile->lunar_period;
    memcpy(input->moon_color, profile->moon_linear, sizeof(input->moon_color));
    memcpy(input->starlight_color, profile->starlight_linear, sizeof(input->starlight_color));
    input->moon_max = profile->moon_max;
    input->starlight_strength = profile->starlight_strength;
}

static bool profile_finalize(region_celestial_profile_t *profile, char *error, size_t error_size) {
    if (profile->schema != 1) {
        return region_error(error, error_size, "celestial schema must be 1");
    }
    if (profile->lunar_period < 168 || profile->lunar_period > 8064 ||
        profile->lunar_period % 24 != 0 || profile->lunar_period % 8 != 0) {
        return region_error(error, error_size, "invalid effective lunar period");
    }
    if (!validate_cycle("solar", &profile->solar, 24, error, error_size) ||
        !validate_cycle("season", &profile->season, 8064, error, error_size) ||
        !validate_cycle("lunar", &profile->lunar, profile->lunar_period, error, error_size)) {
        return false;
    }
    if (profile->day_brightness > 1024 || profile->night_brightness > 1024 ||
        profile->moon_max > 20 || profile->starlight_strength > 2) {
        return region_error(error, error_size, "effective celestial radiance is out of range");
    }

    light_color_linearize(profile->day_color, profile->day_linear);
    light_color_linearize(profile->night_color, profile->night_linear);
    light_color_linearize(profile->moon_color, profile->moon_linear);
    light_color_linearize(profile->starlight_color, profile->starlight_linear);
    celestial_lunar_input lunar_input;
    celestial_lunar_sample lunar_sample;
    const region_celestial_phases_t phases = {0};
    profile_lunar_input(profile, &phases, &lunar_input);
    if (!celestial_lunar_evaluate(&lunar_input, &lunar_sample)) {
        return region_error(error, error_size, "invalid effective lunar environment");
    }
    return profile_digest(profile, error, error_size);
}

static bool region_resolve(region_struct *region, char *error, size_t error_size) {
    if (region->celestial_state == 2) {
        return true;
    }
    if (region->celestial_state == 1) {
        return region_error(error, error_size, "parent cycle includes region %s", region->name);
    }
    region->celestial_state = 1;
    if (region->parent != NULL) {
        if (!region_resolve(region->parent, error, error_size)) {
            return false;
        }
        region->celestial = region->parent->celestial;
    } else if (region->celestial_fields != CELESTIAL_ALL_FIELDS) {
        return region_error(error,
                            error_size,
                            "root region world must define every celestial field");
    }

    for (size_t field = 0; field < CELESTIAL_FIELD_COUNT; field++) {
        if ((region->celestial_fields & (UINT32_C(1) << field)) != 0) {
            inherit_field(&region->celestial,
                          &region->celestial_authored,
                          (enum celestial_field)field);
        }
    }
    char detail[HUGE_BUF];
    if (!profile_finalize(&region->celestial, VS(detail))) {
        return region_error(error, error_size, "region %s: %s", region->name, detail);
    }
    region->celestial_state = 2;
    return true;
}

static bool validate_graph(region_struct *regions, char *error, size_t error_size) {
    region_struct *root = NULL;
    size_t roots = 0;
    for (region_struct *region = regions; region != NULL; region = region->next) {
        if (region->parent_name == NULL) {
            root = region;
            roots++;
        } else {
            region->parent = region_find_in_list(regions, region->parent_name);
            if (region->parent == NULL) {
                return region_error(error,
                                    error_size,
                                    "region %s has unknown parent %s",
                                    region->name,
                                    region->parent_name);
            }
            if (region->parent == region) {
                return region_error(error, error_size, "region %s is its own parent", region->name);
            }
        }
    }
    if (roots != 1 || root == NULL || strcmp(root->name, "world") != 0) {
        return region_error(error,
                            error_size,
                            "registry must have exactly one parentless world region");
    }
    for (region_struct *region = regions; region != NULL; region = region->next) {
        if (!region_resolve(region, error, error_size)) {
            return false;
        }
    }
    return true;
}

bool regions_load(const char *filename, char *error, size_t error_size) {
    if (first_region != NULL) {
        return region_error(error, error_size, "regions are already initialized");
    }
    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {
        return region_error(error, error_size, "cannot open regions file: %s", strerror(errno));
    }

    region_struct *regions = NULL;
    region_struct *tail = NULL;
    region_struct *region = NULL;
    char buf[HUGE_BUF * 4], msgbuf[HUGE_BUF];
    size_t line = 0;
    bool ok = true;
    while (ok && fgets(buf, sizeof(buf), fp) != NULL) {
        line++;
        char *key = buf;
        while (isspace((unsigned char)*key)) {
            key++;
        }
        char *end = key + strlen(key);
        while (end > key && (end[-1] == '\n' || end[-1] == '\r')) {
            *--end = '\0';
        }
        if (*key == '\0' || *key == '#') {
            continue;
        }

        char *value = key;
        while (*value != '\0' && !isspace((unsigned char)*value)) {
            value++;
        }
        if (*value != '\0') {
            *value++ = '\0';
            while (isspace((unsigned char)*value)) {
                value++;
            }
        } else {
            value = NULL;
        }

        if (region == NULL) {
            if (strcmp(key, "region") != 0 || string_isempty(value)) {
                ok = region_error(error, error_size, "line %zu: expected region name", line);
                continue;
            }
            if (region_find_in_list(regions, value) != NULL) {
                ok = region_error(error, error_size, "line %zu: duplicate region %s", line, value);
                continue;
            }
            region = region_get();
            region->name = xstrdup(value);
            continue;
        }

        if (value == NULL) {
            if (strcmp(key, "msg") == 0) {
                msgbuf[0] = '\0';
                bool terminated = false;
                while (fgets(buf, sizeof(buf), fp) != NULL) {
                    line++;
                    if (strcmp(buf, "endmsg\n") == 0 || strcmp(buf, "endmsg\r\n") == 0) {
                        terminated = true;
                        break;
                    }
                    snprintfcat(VS(msgbuf), "%s", buf);
                }
                if (!terminated) {
                    ok = region_error(error,
                                      error_size,
                                      "line %zu: unterminated region message",
                                      line);
                } else if (msgbuf[0] != '\0') {
                    region->msg = xstrdup(msgbuf);
                }
            } else if (strcmp(key, "end") == 0) {
                if (tail == NULL) {
                    regions = region;
                } else {
                    tail->next = region;
                }
                tail = region;
                region = NULL;
            } else {
                ok = region_error(error,
                                  error_size,
                                  "line %zu: invalid region record %s",
                                  line,
                                  key);
            }
            continue;
        }

        int field = celestial_field(key);
        if (field >= 0) {
            ok = parse_celestial(region, (enum celestial_field)field, value, error, error_size);
        } else if (strncmp(key, "celestial_", strlen("celestial_")) == 0) {
            ok = region_error(error, error_size, "line %zu: unknown celestial field %s", line, key);
        } else if (strcmp(key, "parent") == 0) {
            if (region->parent_name != NULL) {
                ok = region_error(error, error_size, "line %zu: duplicate parent", line);
            } else {
                region->parent_name = xstrdup(value);
            }
        } else if (strcmp(key, "longname") == 0) {
            region->longname = xstrdup(value);
        } else if (strcmp(key, "map_first") == 0) {
            region->map_first = xstrdup(value);
        } else if (strcmp(key, "map_bg") == 0) {
            region->map_bg = xstrdup(value);
        } else if (strcmp(key, "jail") == 0) {
            char path[MAX_BUF], trailing;
            int x, y;
            if (sscanf(value, "%255s %d %d %c", path, &x, &y, &trailing) != 3 || x < INT16_MIN ||
                x > INT16_MAX || y < INT16_MIN || y > INT16_MAX) {
                ok = region_error(error, error_size, "line %zu: invalid jail", line);
            } else {
                region->jailmap = xstrdup(path);
                region->jailx = (int16_t)x;
                region->jaily = (int16_t)y;
            }
        } else if (strcmp(key, "child_maps") == 0) {
            region->child_maps = KEYWORD_IS_TRUE(value);
        } else {
            ok = region_error(error, error_size, "line %zu: unknown region field %s", line, key);
        }
    }
    if (ferror(fp)) {
        ok = region_error(error, error_size, "failed reading regions file: %s", strerror(errno));
    } else if (ok && region != NULL) {
        ok = region_error(error, error_size, "region block without end: %s", region->name);
    } else if (ok) {
        ok = validate_graph(regions, error, error_size);
    }
    fclose(fp);

    if (!ok) {
        region_free(region);
        region_list_free(regions);
        return false;
    }
    first_region = regions;
    return true;
}

void regions_init(void) {
    if (first_region != NULL) {
        return;
    }
    char filename[HUGE_BUF], error[HUGE_BUF];
    snprintf(filename, sizeof(filename), "%s/regions.reg", settings.mapspath);
    if (!regions_load(filename, VS(error))) {
        LOG(ERROR, "Cannot initialize regions from %s: %s", filename, error);
        exit(EXIT_FAILURE);
    }
}

void regions_free(void) {
    region_list_free(first_region);
    first_region = NULL;
}

static region_struct *region_get(void) {
    return xcalloc(1, sizeof(region_struct));
}

static void region_free(region_struct *region) {
    if (region == NULL) {
        return;
    }
    FREE_AND_NULL_PTR(region->name);
    FREE_AND_NULL_PTR(region->parent_name);
    FREE_AND_NULL_PTR(region->longname);
    FREE_AND_NULL_PTR(region->map_first);
    FREE_AND_NULL_PTR(region->map_bg);
    FREE_AND_NULL_PTR(region->msg);
    FREE_AND_NULL_PTR(region->jailmap);
    free(region);
}

static void region_list_free(region_struct *regions) {
    while (regions != NULL) {
        region_struct *next = regions->next;
        region_free(regions);
        regions = next;
    }
}

static region_struct *region_find_in_list(region_struct *regions, const char *name) {
    for (region_struct *region = regions; region != NULL; region = region->next) {
        if (strcmp(region->name, name) == 0) {
            return region;
        }
    }
    return NULL;
}

region_struct *region_find_by_name(const char *region_name) {
    region_struct *region = region_find_in_list(first_region, region_name);
    if (region == NULL) {
        LOG(BUG, "Got no region for region %s.", region_name);
    }
    return region;
}

region_struct *region_world(void) {
    return region_find_in_list(first_region, "world");
}

const region_celestial_profile_t *region_celestial_for_map(const mapstruct *map) {
    if (map == NULL) {
        return NULL;
    }
    const region_struct *region = map->region != NULL ? map->region : region_world();
    return region != NULL ? &region->celestial : NULL;
}

static uint16_t
cycle_phase(const region_celestial_cycle_t *cycle, uint64_t absolute_tick, uint16_t period) {
    if (cycle->mode == REGION_CELESTIAL_MODE_GLOBAL) {
        return (uint16_t)(absolute_tick % period);
    }
    if (cycle->mode == REGION_CELESTIAL_MODE_FIXED) {
        return cycle->phase;
    }
    uint64_t modulus = (uint64_t)period * cycle->rate_denominator;
    uint64_t tick = absolute_tick % modulus;
    uint64_t epoch = cycle->epoch % modulus;
    uint64_t delta = tick >= epoch ? tick - epoch : modulus - (epoch - tick);
    uint64_t advance = delta * cycle->rate_numerator / cycle->rate_denominator;
    return (uint16_t)((cycle->phase + advance) % period);
}

void region_celestial_phases(const region_celestial_profile_t *profile,
                             uint64_t absolute_tick,
                             region_celestial_phases_t *phases) {
    HARD_ASSERT(profile != NULL);
    HARD_ASSERT(phases != NULL);
    phases->solar = cycle_phase(&profile->solar, absolute_tick, 24);
    phases->season = cycle_phase(&profile->season, absolute_tick, 8064);
    phases->lunar = cycle_phase(&profile->lunar, absolute_tick, profile->lunar_period);
}

void region_celestial_lunar_input(const region_celestial_profile_t *profile,
                                  uint64_t absolute_tick,
                                  celestial_lunar_input *input) {
    HARD_ASSERT(profile != NULL);
    HARD_ASSERT(input != NULL);
    region_celestial_phases_t phases;
    region_celestial_phases(profile, absolute_tick, &phases);
    profile_lunar_input(profile, &phases, input);
}

bool region_celestial_diagnostic(const region_struct *region,
                                 uint64_t absolute_tick,
                                 char *buffer,
                                 size_t buffer_size) {
    if (region == NULL || buffer == NULL || buffer_size == 0 || region->celestial_state != 2) {
        return false;
    }
    region_celestial_phases_t phases;
    region_celestial_phases(&region->celestial, absolute_tick, &phases);
    int length = snprintf(buffer,
                          buffer_size,
                          "region=%s digest=%s revision=%016" PRIx64
                          " solar=%u season=%u lunar=%u overrides=",
                          region->name,
                          region->celestial.digest_hex,
                          region->celestial.revision,
                          phases.solar,
                          phases.season,
                          phases.lunar);
    if (length < 0 || (size_t)length >= buffer_size) {
        return false;
    }
    size_t used = (size_t)length;
    bool first = true;
    for (size_t field = 0; field < CELESTIAL_FIELD_COUNT; field++) {
        if ((region->celestial_fields & (UINT32_C(1) << field)) == 0) {
            continue;
        }
        length = snprintf(buffer + used,
                          buffer_size - used,
                          "%s%s",
                          first ? "" : ",",
                          celestial_keys[field]);
        if (length < 0 || (size_t)length >= buffer_size - used) {
            return false;
        }
        used += (size_t)length;
        first = false;
    }
    if (first) {
        length = snprintf(buffer + used, buffer_size - used, "none");
        if (length < 0 || (size_t)length >= buffer_size - used) {
            return false;
        }
    }
    return true;
}

const region_struct *region_find_with_map(const region_struct *region) {
    HARD_ASSERT(region != NULL);
    for (; region != NULL; region = region->parent) {
        if (region->map_first != NULL) {
            break;
        }
    }
    return region;
}

const char *region_get_longname(const region_struct *region) {
    if (region->longname) {
        return region->longname;
    } else if (region->parent) {
        return region_get_longname(region->parent);
    }
    LOG(BUG, "Region %s has no parent and no longname.", region->name);
    return "no region name";
}

const char *region_get_msg(const region_struct *region) {
    if (region->msg) {
        return region->msg;
    } else if (region->parent) {
        return region_get_msg(region->parent);
    }
    LOG(BUG, "Region %s has no parent and no msg.", region->name);
    return "no region message";
}

int region_enter_jail(object *op) {
    region_struct *region;
    mapstruct *m;
    if (op->map->region == NULL) {
        return 0;
    }
    for (region = op->map->region; region != NULL; region = region->parent) {
        if (region->jailmap == NULL) {
            continue;
        }
        m = ready_map_name(region->jailmap, NULL, 0);
        if (m == NULL) {
            LOG(BUG,
                "Could not load map '%s' (%d,%d).",
                region->jailmap,
                region->jailx,
                region->jaily);
            return 0;
        }
        return object_enter_map(op, NULL, m, region->jailx, region->jaily, true);
    }
    LOG(BUG, "No suitable jailmap for region %s was found.", op->map->region->name);
    return 0;
}
