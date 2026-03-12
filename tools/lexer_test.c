/*
 * taskjuggler-lsp - Language Server Protocol implementation for TaskJuggler v3
 * Copyright (C) 2026  Devrin Talen <dct23@cornell.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * tools/lexer_test.c - Standalone lexer test driver
 *
 * Feeds a TJP source file to the flex-generated lexer and prints every token
 * with its position and text to stdout.  Useful for verifying lexer output
 * before hooking up the full parser.
 *
 * Usage:
 *   ./lexer-test test/tutorial.tjp
 *   ./lexer-test < test/tutorial.tjp
 *
 * Build:  make lexer-test
 */

#include "../src/parser.h"
#include "../src/grammar.tab.h"  /* token codes + YYSTYPE definition */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── token_kind_name ─────────────────────────────────────────────────────── */

static const char *token_kind_name(int k) {
    switch (k) {
    /* Literals and structural tokens */
    case TK_IDENT:          return "IDENT";
    case TK_STR:            return "STR";
    case TK_INTEGER:        return "INTEGER";
    case TK_FLOAT:          return "FLOAT";
    case TK_DATE:           return "DATE";
    case TK_DURATION:       return "DURATION";
    case TK_LBRACE:         return "LBRACE";
    case TK_RBRACE:         return "RBRACE";
    case TK_LBRACKET:       return "LBRACKET";
    case TK_RBRACKET:       return "RBRACKET";
    case TK_BANG:           return "BANG";
    case TK_PLUS:           return "PLUS";
    case TK_MINUS:          return "MINUS";
    case TK_DOT:            return "DOT";
    case TK_COLON:          return "COLON";
    case TK_COMMA:          return "COMMA";
    case TK_PERCENT:        return "PERCENT";
    case TK_DOLLAR:         return "DOLLAR";
    case TK_LINE_COMMENT:   return "LINE_COMMENT";
    case TK_BLOCK_COMMENT:  return "BLOCK_COMMENT";
    case TK_MULTI_LINE_STR: return "MULTI_LINE_STR";
    case TK_EOF:            return "EOF";
    case TK_ERROR:          return "ERROR";
    /* Keywords */
    case KW_ACCOUNT:             return "KW_ACCOUNT";
    case KW_ACCOUNTPREFIX:       return "KW_ACCOUNTPREFIX";
    case KW_ACCOUNTREPORT:       return "KW_ACCOUNTREPORT";
    case KW_ACCOUNTROOT:         return "KW_ACCOUNTROOT";
    case KW_ACTIVE:              return "KW_ACTIVE";
    case KW_ADOPT:               return "KW_ADOPT";
    case KW_AGGREGATE:           return "KW_AGGREGATE";
    case KW_ALERT:               return "KW_ALERT";
    case KW_ALERTLEVELS:         return "KW_ALERTLEVELS";
    case KW_ALLOCATE:            return "KW_ALLOCATE";
    case KW_ALTERNATIVE:         return "KW_ALTERNATIVE";
    case KW_AUTHOR:              return "KW_AUTHOR";
    case KW_AUXDIR:              return "KW_AUXDIR";
    case KW_BALANCE:             return "KW_BALANCE";
    case KW_BOOKING:             return "KW_BOOKING";
    case KW_CAPTION:             return "KW_CAPTION";
    case KW_CELLCOLOR:           return "KW_CELLCOLOR";
    case KW_CELLTEXT:            return "KW_CELLTEXT";
    case KW_CENTER:              return "KW_CENTER";
    case KW_CHARGE:              return "KW_CHARGE";
    case KW_CHARGESET:           return "KW_CHARGESET";
    case KW_COLUMNS:             return "KW_COLUMNS";
    case KW_COMPLETE:            return "KW_COMPLETE";
    case KW_COPYRIGHT:           return "KW_COPYRIGHT";
    case KW_CREDITS:             return "KW_CREDITS";
    case KW_CURRENCY:            return "KW_CURRENCY";
    case KW_CURRENCYFORMAT:      return "KW_CURRENCYFORMAT";
    case KW_DAILYMAX:            return "KW_DAILYMAX";
    case KW_DAILYMIN:            return "KW_DAILYMIN";
    case KW_DAILYWORKINGHOURS:   return "KW_DAILYWORKINGHOURS";
    case KW_DATE:                return "KW_DATE";
    case KW_DEFINITIONS:         return "KW_DEFINITIONS";
    case KW_DEPENDS:             return "KW_DEPENDS";
    case KW_DETAILS:             return "KW_DETAILS";
    case KW_DISABLED:            return "KW_DISABLED";
    case KW_DURATION:            return "KW_DURATION";
    case KW_EFFICIENCY:          return "KW_EFFICIENCY";
    case KW_EFFORT:              return "KW_EFFORT";
    case KW_EFFORTDONE:          return "KW_EFFORTDONE";
    case KW_EFFORTLEFT:          return "KW_EFFORTLEFT";
    case KW_EMAIL:               return "KW_EMAIL";
    case KW_ENABLED:             return "KW_ENABLED";
    case KW_END:                 return "KW_END";
    case KW_ENDCREDIT:           return "KW_ENDCREDIT";
    case KW_EPILOG:              return "KW_EPILOG";
    case KW_EXPORT:              return "KW_EXPORT";
    case KW_EXTEND:              return "KW_EXTEND";
    case KW_FAIL:                return "KW_FAIL";
    case KW_FLAGS:               return "KW_FLAGS";
    case KW_FONTCOLOR:           return "KW_FONTCOLOR";
    case KW_FOOTER:              return "KW_FOOTER";
    case KW_FORMATS:             return "KW_FORMATS";
    case KW_GAPDURATION:         return "KW_GAPDURATION";
    case KW_GAPLENGTH:           return "KW_GAPLENGTH";
    case KW_HALIGN:              return "KW_HALIGN";
    case KW_HASALERT:            return "KW_HASALERT";
    case KW_HEADER:              return "KW_HEADER";
    case KW_HEADLINE:            return "KW_HEADLINE";
    case KW_HEIGHT:              return "KW_HEIGHT";
    case KW_HIDEACCOUNT:         return "KW_HIDEACCOUNT";
    case KW_HIDEJOURNALENTRY:    return "KW_HIDEJOURNALENTRY";
    case KW_HIDEREPORT:          return "KW_HIDEREPORT";
    case KW_HIDERESOURCE:        return "KW_HIDERESOURCE";
    case KW_HIDETASK:            return "KW_HIDETASK";
    case KW_ICALREPORT:          return "KW_ICALREPORT";
    case KW_INCLUDE:             return "KW_INCLUDE";
    case KW_INHERIT:             return "KW_INHERIT";
    case KW_ISACTIVE:            return "KW_ISACTIVE";
    case KW_ISCHILDOF:           return "KW_ISCHILDOF";
    case KW_ISDEPENDENCYOF:      return "KW_ISDEPENDENCYOF";
    case KW_ISDUTYOF:            return "KW_ISDUTYOF";
    case KW_ISFEATUREOF:         return "KW_ISFEATUREOF";
    case KW_ISLEAF:              return "KW_ISLEAF";
    case KW_ISMILESTONE:         return "KW_ISMILESTONE";
    case KW_ISONGOING:           return "KW_ISONGOING";
    case KW_ISRESOURCE:          return "KW_ISRESOURCE";
    case KW_ISRESPONSIBILITYOF:  return "KW_ISRESPONSIBILITYOF";
    case KW_ISTASK:              return "KW_ISTASK";
    case KW_ISVALID:             return "KW_ISVALID";
    case KW_JOURNALATTRIBUTES:   return "KW_JOURNALATTRIBUTES";
    case KW_JOURNALENTRY:        return "KW_JOURNALENTRY";
    case KW_JOURNALMODE:         return "KW_JOURNALMODE";
    case KW_LEAVEALLOWANCES:     return "KW_LEAVEALLOWANCES";
    case KW_LEAVES:              return "KW_LEAVES";
    case KW_LEFT:                return "KW_LEFT";
    case KW_LENGTH:              return "KW_LENGTH";
    case KW_LIMITS:              return "KW_LIMITS";
    case KW_LISTITEM:            return "KW_LISTITEM";
    case KW_LISTTYPE:            return "KW_LISTTYPE";
    case KW_LOADUNIT:            return "KW_LOADUNIT";
    case KW_MACRO:               return "KW_MACRO";
    case KW_MANAGERS:            return "KW_MANAGERS";
    case KW_MANDATORY:           return "KW_MANDATORY";
    case KW_MARKDATE:            return "KW_MARKDATE";
    case KW_MAXEND:              return "KW_MAXEND";
    case KW_MAXIMUM:             return "KW_MAXIMUM";
    case KW_MAXSTART:            return "KW_MAXSTART";
    case KW_MILESTONE:           return "KW_MILESTONE";
    case KW_MINEND:              return "KW_MINEND";
    case KW_MINIMUM:             return "KW_MINIMUM";
    case KW_MINSTART:            return "KW_MINSTART";
    case KW_MONTHLYMAX:          return "KW_MONTHLYMAX";
    case KW_MONTHLYMIN:          return "KW_MONTHLYMIN";
    case KW_NAVIGATOR:           return "KW_NAVIGATOR";
    case KW_NEWTASK:             return "KW_NEWTASK";
    case KW_NIKUREPORT:          return "KW_NIKUREPORT";
    case KW_NOTE:                return "KW_NOTE";
    case KW_NOVEVENTS:           return "KW_NOVEVENTS";
    case KW_NOW:                 return "KW_NOW";
    case KW_NUMBER:              return "KW_NUMBER";
    case KW_NUMBERFORMAT:        return "KW_NUMBERFORMAT";
    case KW_ONEND:               return "KW_ONEND";
    case KW_ONSTART:             return "KW_ONSTART";
    case KW_OPENNODES:           return "KW_OPENNODES";
    case KW_OUTPUTDIR:           return "KW_OUTPUTDIR";
    case KW_OVERTIME:            return "KW_OVERTIME";
    case KW_PERIOD:              return "KW_PERIOD";
    case KW_PERSISTENT:          return "KW_PERSISTENT";
    case KW_PRECEDES:            return "KW_PRECEDES";
    case KW_PRIORITY:            return "KW_PRIORITY";
    case KW_PROJECT:             return "KW_PROJECT";
    case KW_PROJECTID:           return "KW_PROJECTID";
    case KW_PROJECTIDS:          return "KW_PROJECTIDS";
    case KW_PROJECTION:          return "KW_PROJECTION";
    case KW_PROLOG:              return "KW_PROLOG";
    case KW_PURGE:               return "KW_PURGE";
    case KW_RATE:                return "KW_RATE";
    case KW_RAWHTMLHEAD:         return "KW_RAWHTMLHEAD";
    case KW_REFERENCE:           return "KW_REFERENCE";
    case KW_REMAINING:           return "KW_REMAINING";
    case KW_REPLACE:             return "KW_REPLACE";
    case KW_REPORTPREFIX:        return "KW_REPORTPREFIX";
    case KW_RESOURCE:            return "KW_RESOURCE";
    case KW_RESOURCEATTRIBUTES:  return "KW_RESOURCEATTRIBUTES";
    case KW_RESOURCEPREFIX:      return "KW_RESOURCEPREFIX";
    case KW_RESOURCEREPORT:      return "KW_RESOURCEREPORT";
    case KW_RESOURCEROOT:        return "KW_RESOURCEROOT";
    case KW_RESOURCES:           return "KW_RESOURCES";
    case KW_RESPONSIBLE:         return "KW_RESPONSIBLE";
    case KW_RICHTEXT:            return "KW_RICHTEXT";
    case KW_RIGHT:               return "KW_RIGHT";
    case KW_ROLLUPACCOUNT:       return "KW_ROLLUPACCOUNT";
    case KW_ROLLUPRESOURCE:      return "KW_ROLLUPRESOURCE";
    case KW_ROLLUPTASK:          return "KW_ROLLUPTASK";
    case KW_SCALE:               return "KW_SCALE";
    case KW_SCENARIO:            return "KW_SCENARIO";
    case KW_SCENARIOS:           return "KW_SCENARIOS";
    case KW_SCENARIOSPECIFIC:    return "KW_SCENARIOSPECIFIC";
    case KW_SCHEDULED:           return "KW_SCHEDULED";
    case KW_SCHEDULING:          return "KW_SCHEDULING";
    case KW_SCHEDULINGMODE:      return "KW_SCHEDULINGMODE";
    case KW_SELECT:              return "KW_SELECT";
    case KW_SELFCONTAINED:       return "KW_SELFCONTAINED";
    case KW_SHIFT:               return "KW_SHIFT";
    case KW_SHIFTS:              return "KW_SHIFTS";
    case KW_SHORTTIMEFORMAT:     return "KW_SHORTTIMEFORMAT";
    case KW_SLOPPY:              return "KW_SLOPPY";
    case KW_SORTACCOUNTS:        return "KW_SORTACCOUNTS";
    case KW_SORTJOURNALENTRIES:  return "KW_SORTJOURNALENTRIES";
    case KW_SORTRESOURCES:       return "KW_SORTRESOURCES";
    case KW_SORTTASKS:           return "KW_SORTTASKS";
    case KW_START:               return "KW_START";
    case KW_STARTCREDIT:         return "KW_STARTCREDIT";
    case KW_STATUS:              return "KW_STATUS";
    case KW_STATUSSHEET:         return "KW_STATUSSHEET";
    case KW_STATUSSHEETREPORT:   return "KW_STATUSSHEETREPORT";
    case KW_STRICT:              return "KW_STRICT";
    case KW_SUMMARY:             return "KW_SUMMARY";
    case KW_SUPPLEMENT:          return "KW_SUPPLEMENT";
    case KW_TAGFILE:             return "KW_TAGFILE";
    case KW_TASK:                return "KW_TASK";
    case KW_TASKATTRIBUTES:      return "KW_TASKATTRIBUTES";
    case KW_TASKPREFIX:          return "KW_TASKPREFIX";
    case KW_TASKREPORT:          return "KW_TASKREPORT";
    case KW_TASKROOT:            return "KW_TASKROOT";
    case KW_TEXT:                return "KW_TEXT";
    case KW_TEXTREPORT:          return "KW_TEXTREPORT";
    case KW_TIMEFORMAT:          return "KW_TIMEFORMAT";
    case KW_TIMEFORMAT1:         return "KW_TIMEFORMAT1";
    case KW_TIMEFORMAT2:         return "KW_TIMEFORMAT2";
    case KW_TIMEOFF:             return "KW_TIMEOFF";
    case KW_TIMESHEET:           return "KW_TIMESHEET";
    case KW_TIMESHEETREPORT:     return "KW_TIMESHEETREPORT";
    case KW_TIMEZONE:            return "KW_TIMEZONE";
    case KW_TIMINGRESOLUTION:    return "KW_TIMINGRESOLUTION";
    case KW_TITLE:               return "KW_TITLE";
    case KW_TOOLTIP:             return "KW_TOOLTIP";
    case KW_TRACEREPORT:         return "KW_TRACEREPORT";
    case KW_TRACKINGSCENARIO:    return "KW_TRACKINGSCENARIO";
    case KW_TREELEVEL:           return "KW_TREELEVEL";
    case KW_VACATION:            return "KW_VACATION";
    case KW_WARN:                return "KW_WARN";
    case KW_WEEKLYMAX:           return "KW_WEEKLYMAX";
    case KW_WEEKLYMIN:           return "KW_WEEKLYMIN";
    case KW_WEEKSTARTSMONDAY:    return "KW_WEEKSTARTSMONDAY";
    case KW_WEEKSTARTSSUNDAY:    return "KW_WEEKSTARTSSUNDAY";
    case KW_WIDTH:               return "KW_WIDTH";
    case KW_WORK:                return "KW_WORK";
    case KW_WORKINGHOURS:        return "KW_WORKINGHOURS";
    case KW_YEARLYWORKINGDAYS:   return "KW_YEARLYWORKINGDAYS";
    default:                return "?";
    }
}

/* ── Stubs required by lexer.yy.c ───────────────────────────────────────── */

/* g_push_tok_span: normally accumulates tokens into a ParseResult; here we just
 * print each token as it arrives. */
void g_push_tok_span(int kind,
                     uint32_t sl, uint32_t sc,
                     uint32_t el, uint32_t ec,
                     const char *text) {
    /* Collapse newlines in text so each token fits on one line. */
    char display[48];
    int j = 0;
    for (int i = 0; text[i] && j < (int)sizeof(display) - 4; i++) {
        if (text[i] == '\n') {
            display[j++] = '\\';
            display[j++] = 'n';
        } else {
            display[j++] = text[i];
        }
    }
    if (text[j] != '\0') { display[j++] = '.'; display[j++] = '.'; display[j++] = '.'; }
    display[j] = '\0';

    printf("%3u:%-3u  %3u:%-3u  %-18s  %s\n",
           sl, sc, el, ec, token_kind_name(kind), display);
}

/* yylval: normally defined by bison's grammar.tab.c; provide it here so the
 * lexer can populate it even though we never call yyparse(). */
YYSTYPE yylval;

/* ── flex interface ──────────────────────────────────────────────────────── */

typedef void *YY_BUFFER_STATE;
extern YY_BUFFER_STATE yy_scan_string(const char *str);
extern void            yy_delete_buffer(YY_BUFFER_STATE buf);
extern int             yylex(void);

/* ── File reading ────────────────────────────────────────────────────────── */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { perror("fseek"); fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { perror("ftell"); fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fputs("out of memory\n", stderr); fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        perror("fread"); free(buf); fclose(f); return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

static char *read_stdin(void) {
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fputs("out of memory\n", stderr); return NULL; }
    int c;
    while ((c = getchar()) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) { fputs("out of memory\n", stderr); return NULL; }
        }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    return buf;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    char *src;

    if (argc >= 2) {
        src = read_file(argv[1]);
    } else {
        src = read_stdin();
    }
    if (!src) return 1;

    printf("  line:col   line:col   %-18s  text\n", "kind");
    printf("  ----:---   ----:---   %-18s  ----\n", "----");

    YY_BUFFER_STATE buf = yy_scan_string(src);

    /* Drive the lexer: g_push_token() prints each token as a side effect. */
    while (yylex() != 0)
        ;

    yy_delete_buffer(buf);
    free(src);
    return 0;
}
