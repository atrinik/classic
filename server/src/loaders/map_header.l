%{
/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 The Atrinik Project                         *
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

#include <global.h>
#include <celestial_structure.h>
#include <loader.h>
#include <region.h>
#include <toolkit/string.h>

#define YY_DECL int map_lex_load(mapstruct *m)

static const char *yval(void);
static bool parse_int(const char *value, int minimum, int maximum, int *result);

static char msgbuf[HUGE_BUF];
static int lex_error, msg_flag;

#define IVAL atoi(yval())

#define SET_OR_CLEAR_MAP_FLAG(m, flag, val) \
    { \
        if (val) \
        { \
            m->map_flags |= (flag); \
        } \
        else \
        { \
            m->map_flags &= ~(flag); \
        } \
    }

#define YY_NO_INPUT

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
%}

S   [ \t]+.+
WS  [ \t]*

%x MESSAGE

/* Don't have to link with -lfl with this */
%option noyywrap

/* Disable unput */
%option nounput

/* Disable tt_top_state */
%option noyy_top_state

/* need yy_push_state, yy_pop_state */
%option stack

%%

%{
/* Declare some local variables */
lex_error = 0;
%}

^name{S}             FREE_AND_COPY_HASH(m->name, yval());
^bg_music{S}         FREE_AND_COPY_HASH(m->bg_music, yval());
^weather{S}          FREE_AND_COPY_HASH(m->weather, yval());
^region{S}           {
    if (m->celestial_region_seen) {
        m->celestial_header_invalid = true;
    }
    m->celestial_region_seen = true;
    m->region = region_find_by_name(yval());
    if (m->region == NULL) {
        m->celestial_header_invalid = true;
        return LL_ERROR;
    }
}
^enter_x{S}          m->enter_x = IVAL;
^enter_y{S}          m->enter_y = IVAL;
^width{S}            {
    uint64_t value;
    if (m->celestial_width_seen || !string_parse_uint64(yval(), 10, 1, 64, &value)) {
        m->celestial_header_invalid = true;
        m->width = 0;
    } else {
        m->width = (int)value;
    }
    m->celestial_width_seen = true;
}
^height{S}           {
    uint64_t value;
    if (m->celestial_height_seen || !string_parse_uint64(yval(), 10, 1, 64, &value)) {
        m->celestial_header_invalid = true;
        m->height = 0;
    } else {
        m->height = (int)value;
    }
    m->celestial_height_seen = true;
}
^reset_timeout{S}    m->reset_timeout = IVAL;
^swap_time{S}        m->swap_time = IVAL;
^difficulty{S}       m->difficulty = IVAL;
^journal_run{S}      {
    const char *run_id = yval();
    if (string_is_hex_fixed(run_id, 32, true)) {
        snprintf(VS(m->journal_run_id), "%s", run_id);
    }
}
^journal_sequence{S} {
    const char *value = yval();
    char *end;
    errno = 0;
    uintmax_t sequence = strtoumax(value, &end, 10);
    if (errno == 0 && end != value && *end == '\0' && sequence <= UINT64_MAX) {
        m->journal_sequence = (uint64_t) sequence;
    }
}
^darkness{S}         {
    m->celestial_legacy_darkness_seen = true;
    int v;

    if (!parse_int(yval(), -1, MAX_DARKNESS, &v)) {
        m->celestial_header_invalid = true;
        LOG(BUG, "Illegal map darkness (must be -1 to %d): %s", MAX_DARKNESS, m->path);
    } else {
        set_map_darkness(m, v);
    }
}
^light{S}            {
    uint64_t value;
    if (m->celestial_light_seen) {
        m->celestial_header_invalid = true;
    }
    m->celestial_light_seen = true;
    if (!string_parse_uint64(yval(), 10, 0, 40959, &value)) {
        m->celestial_header_invalid = true;
        m->light_value = 0;
    } else {
        m->light_value = (int)value;
    }
}
^celestial_schema{S} {
    m->celestial_v1_header_seen = true;
    if (m->celestial_schema_seen || strcmp(yval(), "1") != 0) {
        m->celestial_header_invalid = true;
    }
    m->celestial_schema_seen = true;
    m->celestial_schema = strcmp(yval(), "1") == 0 ? 1 : 0;
}
^sky_above{S}        {
    m->celestial_v1_header_seen = true;
    const char *value = yval();
    if (m->celestial_sky_seen) {
        m->celestial_header_invalid = true;
    }
    m->celestial_sky_seen = true;
    if (strcmp(value, "open") == 0) {
        m->celestial_sky_above = CELESTIAL_SKY_OPEN;
    } else if (strcmp(value, "linked") == 0) {
        m->celestial_sky_above = CELESTIAL_SKY_LINKED;
    } else if (strcmp(value, "sealed") == 0) {
        m->celestial_sky_above = CELESTIAL_SKY_SEALED;
    } else {
        m->celestial_header_invalid = true;
    }
}
^celestial_boundary_[0-9]{1,2}{S} {
    m->celestial_v1_header_seen = true;
    int tile = atoi(yytext + 19);
    if (yytext[19] == '0') {
        m->celestial_header_invalid = true;
    }
    map_set_celestial_boundary(m, tile - 1, yval());
}
^no_save{S}          SET_OR_CLEAR_MAP_FLAG(m, MAP_FLAG_NO_SAVE, IVAL);
^no_magic{S}         SET_OR_CLEAR_MAP_FLAG(m, MAP_FLAG_NOMAGIC, IVAL);
^no_harm{S}          SET_OR_CLEAR_MAP_FLAG(m, MAP_FLAG_NOHARM, IVAL);
^height_diff{S}      SET_OR_CLEAR_MAP_FLAG(m, MAP_FLAG_HEIGHT_DIFF, IVAL);
^no_summon{S}        SET_OR_CLEAR_MAP_FLAG(m, MAP_FLAG_NOSUMMON, IVAL);
^fixed_login{S}      SET_OR_CLEAR_MAP_FLAG(m, MAP_FLAG_FIXED_LOGIN, IVAL);
^player_no_save{S}   SET_OR_CLEAR_MAP_FLAG(m, MAP_FLAG_PLAYER_NO_SAVE, IVAL);
^pvp{S}              SET_OR_CLEAR_MAP_FLAG(m, MAP_FLAG_PVP, IVAL);
^outdoor{S}          {
    uint64_t value;
    m->celestial_legacy_outdoor_seen = true;
    if (!string_parse_uint64(yval(), 10, 0, 1, &value)) {
        m->celestial_header_invalid = true;
    } else {
        SET_OR_CLEAR_MAP_FLAG(m, MAP_FLAG_OUTDOOR, value);
    }
}
^unique{S}           SET_OR_CLEAR_MAP_FLAG(m, MAP_FLAG_UNIQUE, IVAL);
^fixed_resettime{S}  SET_OR_CLEAR_MAP_FLAG(m, MAP_FLAG_FIXED_RTIME, IVAL);
^tile_path_[0-9]{1,2}{S}  {
    int tile = atoi(yytext + 10);

    if (yytext[10] == '0') {
        m->celestial_header_invalid = true;
    }
    if (tile < 1 || tile > TILED_NUM) {
        LOG(BUG, "Tile location %d out of bounds (%s)", tile, m->path);
        m->celestial_header_invalid = true;
    } else {
        if (m->celestial_tile_path_seen[tile - 1]) {
            m->celestial_header_invalid = true;
        }
        m->celestial_tile_path_seen[tile - 1] = true;
        const char *value = yval();
        if (!celestial_structure_logical_map_id_valid(value)) {
            m->celestial_tile_path_invalid = true;
        }
        map_set_tile(m, tile - 1, value);
    }
}
^msg{WS}             {
    BEGIN(MESSAGE);
    msgbuf[0] = '\0';
    msg_flag = 0;
}
<MESSAGE>^endmsg{WS} {
    BEGIN(INITIAL);

    m->msg = xstrdup(msgbuf);
}
<MESSAGE>.*          {
    if (msg_flag) {
        strcat(msgbuf, "\n");
    }

    msg_flag = 1;
    strcat(msgbuf, yytext);
}

<*>(^{WS}$)|\n       {}
#.*\n                {}
^end\n               {
    if (m->region == NULL) {
        m->region = region_world();
        if (m->region == NULL) {
            LOG(BUG, "Cannot resolve world region for map: %s", m->path);
            return LL_ERROR;
        }
    }
    return LL_EOF;
}

<<EOF>>              return LL_MORE;

.*                   {
    LOG(BUG, "Got unknown value in map header: %s", yytext);
    m->celestial_header_invalid = true;
    if (strncmp(yytext, "celestial_", 10) == 0 || strncmp(yytext, "sky_above", 9) == 0) {
        m->celestial_v1_header_seen = true;
    }
    return LL_NORMAL;
}

%%

#pragma GCC diagnostic pop

/**
 * Returns the next token for lex.
 *
 * Our save file syntax is very simple, so we can use a very simple
 * processing mechanism here instead using something like bison.
 *
 * This skips over the space and returns the value, or "" if no value is
 * found. 
 */
static const char *yval(void)
{
    static const char *em = "";
    char *cp, *end;

    /* First skip over start of line, like animation or name */
    for (cp = yytext; *cp != ' '; cp++) {
        if (*cp == '\0') {
            return em;
        }
    }

    /* Skip over whitespace */
    for (; *cp == ' '; cp++) {
        if (*cp == '\0') {
            return em;
        }
    }

    /* Get last character before null and strip
     * off trailing whitespace. */
    for (end = cp + strlen(cp) - 1; *end == ' ' || *end == 0x0A || *end == 0x0D; end--) {
        if (end == cp) {
            return em;
        }

        *end = '\0';
    }

    return cp;
}

int map_set_variable(mapstruct *m, char *buf)
{
    YY_BUFFER_STATE yybufstate, yycurbuf = YY_CURRENT_BUFFER;
    int retval;

    yy_push_state(INITIAL);
    yybufstate = yy_scan_string(buf);

    retval = map_lex_load(m);

    if (yycurbuf) {
        yy_switch_to_buffer(yycurbuf);
    }

    yy_delete_buffer(yybufstate);
    yy_pop_state();

    return retval;
}

/**
 * Frees all memory allocated by the map header loader. 
 */
void free_map_header_loader(void)
{
#ifndef WIN32
    yylex_destroy();
#endif
}

/**
 * This loads the header information of the map. The header contains
 * things like difficulty, size, timeout, etc.
 * @param m
 * Map being read.
 * @param fp
 * File to read from.
 * @return
 * 1 on success, 0 on failure. 
 */
int load_map_header(mapstruct *m, FILE *fp)
{
    char inbuf[HUGE_BUF];
    YY_BUFFER_STATE yybufstate;
    int retval;

    if (!fgets(inbuf, sizeof(inbuf) - 1, fp) || strcmp(inbuf, "arch map\n")) {
        LOG(BUG, "First line should always be 'arch map', but it is not (%s)", m->path);
        return 0;
    }

    while (fgets(inbuf, sizeof(inbuf) - 1, fp)) {
        yybufstate = yy_scan_string(inbuf);
        retval = map_lex_load(m);
        yy_delete_buffer(yybufstate);

        if (retval == LL_ERROR) {
            return 0;
        }
        else if (retval == LL_EOF) {
            return 1;
        }
    }

    return m->celestial_v1_header_seen ? 0 : 1;
}

static bool parse_int(const char *value, int minimum, int maximum, int *result)
{
    HARD_ASSERT(value != NULL);
    HARD_ASSERT(result != NULL);
    errno = 0;
    char *end;
    long parsed = strtol(value, &end, 10);
    if (errno == ERANGE || *value == '\0' || *end != '\0' || parsed < minimum ||
        parsed > maximum) {
        return false;
    }
    *result = (int)parsed;
    return true;
}

void map_set_celestial_boundary(mapstruct *m, int tile, const char *value)
{
    if (tile < 0 || tile >= TILED_NUM || m->celestial_boundary[tile] != CELESTIAL_BOUNDARY_UNSET) {
        m->celestial_header_invalid = true;
        return;
    }
    if (strcmp(value, "continuous") == 0) {
        m->celestial_boundary[tile] = CELESTIAL_BOUNDARY_CONTINUOUS;
    } else if (strcmp(value, "discontinuous") == 0) {
        m->celestial_boundary[tile] = CELESTIAL_BOUNDARY_DISCONTINUOUS;
    } else {
        m->celestial_header_invalid = true;
    }
}

void save_map_header(mapstruct *m, FILE *fp, int flag)
{
    int i;

    fprintf(fp, "arch map\n");

    if (m->name) {
        fprintf(fp, "name %s\n", m->name);
    }

    if (m->bg_music) {
        fprintf(fp, "bg_music %s\n", m->bg_music);
    }

    if (m->weather) {
        fprintf(fp, "weather %s\n", m->weather);
    }

    if (m->region) {
        fprintf(fp, "region %s\n", m->region->name);
    }

    if (!flag) {
        fprintf(fp, "swap_time %d\n", m->swap_time);
    }

    if (m->reset_timeout) {
        fprintf(fp, "reset_timeout %d\n", m->reset_timeout);
    }

    if (MAP_FIXED_RESETTIME(m)) {
        fprintf(fp, "fixed_resettime 1\n");
    }

    if (m->difficulty) {
        fprintf(fp, "difficulty %d\n", m->difficulty);
    }

    if (!flag && m->journal_run_id[0] != '\0') {
        fprintf(fp, "journal_run %s\n", m->journal_run_id);
        fprintf(fp, "journal_sequence %" PRIu64 "\n", m->journal_sequence);
    }

    if (m->celestial_schema == 1) {
        fprintf(fp, "celestial_schema 1\n");
        fprintf(fp,
                "sky_above %s\n",
                m->celestial_sky_above == CELESTIAL_SKY_OPEN
                    ? "open"
                    : m->celestial_sky_above == CELESTIAL_SKY_LINKED ? "linked" : "sealed");
        if (m->light_value != 0) {
            fprintf(fp, "light %d\n", m->light_value);
        }
    } else {
        fprintf(fp, "darkness %d\n", m->darkness);
        fprintf(fp, "light %d\n", m->light_value);
    }

    if (m->width) {
        fprintf(fp, "width %d\n", m->width);
    }

    if (m->height) {
        fprintf(fp, "height %d\n", m->height);
    }

    if (m->enter_x) {
        fprintf(fp, "enter_x %d\n", m->enter_x);
    }

    if (m->enter_y) {
        fprintf(fp, "enter_y %d\n", m->enter_y);
    }

    if (m->msg) {
        fprintf(fp, "msg\n%s\nendmsg\n", m->msg);
    }

    if (MAP_UNIQUE(m)) {
        fprintf(fp, "unique 1\n");
    }

    if (m->celestial_schema != 1 && MAP_OUTDOORS(m)) {
        fprintf(fp, "outdoor 1\n");
    }

    if (MAP_NOSAVE(m)) {
        fprintf(fp, "no_save 1\n");
    }

    if (MAP_NOMAGIC(m)) {
        fprintf(fp, "no_magic 1\n");
    }

    if (MAP_HEIGHT_DIFF(m)) {
        fprintf(fp, "height_diff 1\n");
    }

    if (MAP_NOHARM(m)) {
        fprintf(fp, "no_harm 1\n");
    }

    if (MAP_NOSUMMON(m)) {
        fprintf(fp, "no_summon 1\n");
    }

    if (MAP_FIXEDLOGIN(m)) {
        fprintf(fp, "fixed_login 1\n");
    }

    if (MAP_PVP(m)) {
        fprintf(fp, "pvp 1\n");
    }

    /* Save any tiling information */
    for (i = 0; i < TILED_NUM; i++) {
        if (m->tile_path[i]) {
            fprintf(fp, "tile_path_%d %s\n", i + 1, m->tile_path[i]);
            if (m->celestial_schema == 1) {
                fprintf(fp,
                        "celestial_boundary_%d %s\n",
                        i + 1,
                        m->celestial_boundary[i] == CELESTIAL_BOUNDARY_CONTINUOUS
                            ? "continuous"
                            : "discontinuous");
            }
        }
    }

    fprintf(fp, "end\n");
}
