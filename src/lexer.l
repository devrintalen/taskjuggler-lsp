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

%{
#include "parser.h"
#include "grammar.tab.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Column tracking ─────────────────────────────────────────────────────── *
 *
 * yylineno (managed by %option yylineno) counts newlines and starts at 1.
 * We map to 0-based LSP lines with (yylineno - 1).
 *
 * yycolumn is managed manually: advanced by yyleng in YY_USER_ACTION and
 * reset to 0 in newline-consuming rules.
 *
 * NOTE: yycolumn counts *bytes*, not Unicode codepoints or UTF-16 code units.
 * For ASCII-only files this is correct.  Multi-byte UTF-8 characters will
 * produce wrong character positions.
 *
 * TODO: Replace byte counting with codepoint counting (iterate yytext with
 * utf8_char_len) to match LSP character semantics.
 */
int yycolumn = 0;

/* Start position of the current token, saved before each rule executes. */
static uint32_t tok_sl = 0, tok_sc = 0;

/* Start position of a multi-fragment token (string / block-comment / scissors).
 * Saved once when the opening delimiter is matched; tok_sl/tok_sc would be
 * overwritten by every subsequent fragment before the closing rule fires. */
static uint32_t mfrag_sl = 0, mfrag_sc = 0;

/* Accumulation buffer for DQUOTE and SCISSORS token bodies. */
static char  str_buf[4096];
static int   str_len = 0;

#define STR_RESET()         (str_len = 0)
#define STR_APPEND(s, n)    do { \
    int _n = (int)(n); \
    if (str_len + _n < (int)sizeof(str_buf) - 1) { \
        memcpy(str_buf + str_len, (s), (size_t)_n); \
        str_len += _n; \
    } \
} while (0)
#define STR_FINISH()        (str_buf[str_len] = '\0')

#define YY_USER_ACTION                          \
    tok_sl   = (uint32_t)(yylineno - 1);        \
    tok_sc   = (uint32_t)yycolumn;              \
    yycolumn += yyleng;

/* ── Token emission helpers ──────────────────────────────────────────────── *
 *
 * Every yylex() return goes through one of these helpers so that yylval.tok
 * is always populated with the correct kind, text, and position before bison
 * receives the token.  Grammar actions can then freely access $n.text and
 * call token_free(&$n) without checking for NULL.
 *
 * emit_kw    — semantic token (keyword, date, duration): records in sem-token
 *              array AND populates yylval.tok.
 * emit_plain — non-semantic token (identifier, number, punctuation): only
 *              populates yylval.tok; no sem-token record.
 * emit_kw_mfrag — like emit_kw but uses mfrag_sl/mfrag_sc as the start
 *              position (for multi-fragment tokens: strings, scissors).
 *
 * store_semantic_token / store_semantic_token_mfrag — kept for tokens that
 * are NOT returned to the parser (comments).
 */

extern void g_push_tok_span(int kind,
                             uint32_t sl, uint32_t sc,
                             uint32_t el, uint32_t ec,
                             const char *text);

/* store_semantic_token: used for comments (not returned to parser). */
static void store_semantic_token(int token) {
    uint32_t el = (uint32_t)(yylineno - 1);
    uint32_t ec = (uint32_t)yycolumn;
    g_push_tok_span(token, tok_sl, tok_sc, el, ec, yytext);
}


/* emit_kw: keyword / date / duration — recorded in sem-tokens AND returned. */
static int emit_kw(int code) {
    uint32_t el = (uint32_t)(yylineno - 1);
    uint32_t ec = (uint32_t)yycolumn;
    g_push_tok_span(code, tok_sl, tok_sc, el, ec, yytext);
    yylval.tok = (Token){ code, {tok_sl, tok_sc}, {el, ec}, strdup(yytext) };
    return code;
}

/* emit_plain: identifier / number / punctuation.
 * Tokens needed by hover/completion/signature/dep-validation are also
 * recorded in sem-tokens so that downstream passes can work without a
 * separate full token array. */
static int emit_plain(int code) {
    uint32_t el = (uint32_t)(yylineno - 1);
    uint32_t ec = (uint32_t)yycolumn;
    switch (code) {
    case TK_IDENT:
    case TK_LBRACE: case TK_RBRACE:
    case TK_BANG:   case TK_DOT:   case TK_COMMA:
        g_push_tok_span(code, tok_sl, tok_sc, el, ec, yytext);
        break;
    default:
        break;
    }
    yylval.tok = (Token){ code, {tok_sl, tok_sc}, {el, ec}, strdup(yytext) };
    return code;
}

/* emit_kw_mfrag: multi-fragment tokens (strings, scissors). Uses yytext. */
static int emit_kw_mfrag(int code) {
    uint32_t el = (uint32_t)(yylineno - 1);
    uint32_t ec = (uint32_t)yycolumn;
    g_push_tok_span(code, mfrag_sl, mfrag_sc, el, ec, yytext);
    yylval.tok = (Token){ code, {mfrag_sl, mfrag_sc}, {el, ec}, strdup(yytext) };
    return code;
}

/* emit_kw_mfrag_text: like emit_kw_mfrag but uses an explicit text string
 * (needed for tokens whose body is accumulated across multiple lex rules). */
static int emit_kw_mfrag_text(int code, const char *text) {
    uint32_t el = (uint32_t)(yylineno - 1);
    uint32_t ec = (uint32_t)yycolumn;
    g_push_tok_span(code, mfrag_sl, mfrag_sc, el, ec, text);
    yylval.tok = (Token){ code, {mfrag_sl, mfrag_sc}, {el, ec}, strdup(text) };
    return code;
}

%}

%option noyywrap
%option yylineno
%option 8bit

/* Start conditions for tokens whose bodies span multiple lexer fragments. */
%x BLOCK_COMMENT
%x SCISSORS
%x DQUOTE

%%

[ \t\r\f\v]+    { /* skip whitespace */ }
\n              { yycolumn = 0; }

#[^\n]*         { store_semantic_token(TK_LINE_COMMENT); /* not returned to parser */ }
\/\/[^\n]*      { store_semantic_token(TK_LINE_COMMENT); /* not returned to parser */ }

"/*"            { mfrag_sl = tok_sl; mfrag_sc = tok_sc;
                  STR_RESET(); STR_APPEND("/*", 2); BEGIN(BLOCK_COMMENT); }
<BLOCK_COMMENT>{
    "*/"        { STR_APPEND("*/", 2); STR_FINISH();
                  uint32_t el = (uint32_t)(yylineno - 1);
                  uint32_t ec = (uint32_t)yycolumn;
                  g_push_tok_span(TK_BLOCK_COMMENT, mfrag_sl, mfrag_sc, el, ec, str_buf);
                  BEGIN(INITIAL); /* not returned to parser */ }
    \n          { STR_APPEND(yytext, yyleng); yycolumn = 0; }
    [^*\n]+     { STR_APPEND(yytext, yyleng); }
    "*"         { STR_APPEND(yytext, yyleng); }
    <<EOF>>     { BEGIN(INITIAL); return emit_plain(TK_ERROR); }
}

    /* Scissors multi-line string: -8<- ... ->8- */
"-8<-"          { mfrag_sl = tok_sl; mfrag_sc = tok_sc;
                  STR_RESET(); STR_APPEND("-8<-", 4); BEGIN(SCISSORS); }
<SCISSORS>{
    "->8-"      { STR_APPEND("->8-", 4); STR_FINISH();
                  BEGIN(INITIAL); return emit_kw_mfrag_text(TK_MULTI_LINE_STR, str_buf); }
    \n          { STR_APPEND(yytext, yyleng); yycolumn = 0; }
    [^-\n]+     { STR_APPEND(yytext, yyleng); }
    "-"         { STR_APPEND(yytext, yyleng); }
    <<EOF>>     { BEGIN(INITIAL); return emit_plain(TK_ERROR); }
}

\"              { mfrag_sl = tok_sl; mfrag_sc = tok_sc;
                  STR_RESET(); STR_APPEND("\"", 1); BEGIN(DQUOTE); }
<DQUOTE>{
    \"          { STR_APPEND("\"", 1); STR_FINISH();
                  BEGIN(INITIAL); return emit_kw_mfrag_text(TK_STR, str_buf); }
    \n          { yycolumn = 0;
                  BEGIN(INITIAL); return emit_plain(TK_ERROR); }
    \\[^\n]     { STR_APPEND(yytext, yyleng); }
    [^\"\\\n]+  { STR_APPEND(yytext, yyleng); }
    <<EOF>>     { BEGIN(INITIAL); return emit_plain(TK_ERROR); }
}

    /* Date: YYYY-MM-DD[-hh:mm[:ss]][-{+|-}HHMM]: must precede INT so longest match wins.
     * Time, seconds, and timezone suffix are each independently optional per the TJP spec.
     * Timezone is always a leading '-' separator followed by an explicit '+' or '-' sign. */
[0-9]{4}-[0-9]{2}-[0-9]{2}(-[0-9]{2}:[0-9]{2}(:[0-9]{2})?)?(-[+-][0-9]{4})?  { return emit_kw(TK_DATE); }

    /* Duration: (+|-) <duration> (min | h | d | w | m | y) */
[+-][0-9]+(min|h|d|w|m|y) { return emit_kw(TK_DURATION); }

[0-9]+"."[0-9]+  { return emit_plain(TK_FLOAT); }

[0-9]+           { return emit_plain(TK_INTEGER); }

    /* Keywords — listed alphabetically; must precede the identifier catch-all
     * so that an exact match wins over the longer-pattern identifier rule. */
"account"            { return emit_kw(KW_ACCOUNT); }
"accountprefix"      { return emit_kw(KW_ACCOUNTPREFIX); }
"accountreport"      { return emit_kw(KW_ACCOUNTREPORT); }
"accountroot"        { return emit_kw(KW_ACCOUNTROOT); }
"active"             { return emit_kw(KW_ACTIVE); }
"adopt"              { return emit_kw(KW_ADOPT); }
"aggregate"          { return emit_kw(KW_AGGREGATE); }
"alert"              { return emit_kw(KW_ALERT); }
"alertlevels"        { return emit_kw(KW_ALERTLEVELS); }
"allocate"           { return emit_kw(KW_ALLOCATE); }
"alternative"        { return emit_kw(KW_ALTERNATIVE); }
"author"             { return emit_kw(KW_AUTHOR); }
"auxdir"             { return emit_kw(KW_AUXDIR); }
"balance"            { return emit_kw(KW_BALANCE); }
"booking"            { return emit_kw(KW_BOOKING); }
"caption"            { return emit_kw(KW_CAPTION); }
"cellcolor"          { return emit_kw(KW_CELLCOLOR); }
"celltext"           { return emit_kw(KW_CELLTEXT); }
"center"             { return emit_kw(KW_CENTER); }
"charge"             { return emit_kw(KW_CHARGE); }
"chargeset"          { return emit_kw(KW_CHARGESET); }
"columns"            { return emit_kw(KW_COLUMNS); }
"complete"           { return emit_kw(KW_COMPLETE); }
"copyright"          { return emit_kw(KW_COPYRIGHT); }
"credits"            { return emit_kw(KW_CREDITS); }
"currency"           { return emit_kw(KW_CURRENCY); }
"currencyformat"     { return emit_kw(KW_CURRENCYFORMAT); }
"dailymax"           { return emit_kw(KW_DAILYMAX); }
"dailymin"           { return emit_kw(KW_DAILYMIN); }
"dailyworkinghours"  { return emit_kw(KW_DAILYWORKINGHOURS); }
"date"               { return emit_kw(KW_DATE); }
"definitions"        { return emit_kw(KW_DEFINITIONS); }
"depends"            { return emit_kw(KW_DEPENDS); }
"details"            { return emit_kw(KW_DETAILS); }
"disabled"           { return emit_kw(KW_DISABLED); }
"duration"           { return emit_kw(KW_DURATION); }
"efficiency"         { return emit_kw(KW_EFFICIENCY); }
"effort"             { return emit_kw(KW_EFFORT); }
"effortdone"         { return emit_kw(KW_EFFORTDONE); }
"effortleft"         { return emit_kw(KW_EFFORTLEFT); }
"email"              { return emit_kw(KW_EMAIL); }
"enabled"            { return emit_kw(KW_ENABLED); }
"end"                { return emit_kw(KW_END); }
"endcredit"          { return emit_kw(KW_ENDCREDIT); }
"epilog"             { return emit_kw(KW_EPILOG); }
"export"             { return emit_kw(KW_EXPORT); }
"extend"             { return emit_kw(KW_EXTEND); }
"fail"               { return emit_kw(KW_FAIL); }
"flags"              { return emit_kw(KW_FLAGS); }
"fontcolor"          { return emit_kw(KW_FONTCOLOR); }
"footer"             { return emit_kw(KW_FOOTER); }
"formats"            { return emit_kw(KW_FORMATS); }
"gapduration"        { return emit_kw(KW_GAPDURATION); }
"gaplength"          { return emit_kw(KW_GAPLENGTH); }
"halign"             { return emit_kw(KW_HALIGN); }
"hasalert"           { return emit_kw(KW_HASALERT); }
"header"             { return emit_kw(KW_HEADER); }
"headline"           { return emit_kw(KW_HEADLINE); }
"height"             { return emit_kw(KW_HEIGHT); }
"hideaccount"        { return emit_kw(KW_HIDEACCOUNT); }
"hidejournalentry"   { return emit_kw(KW_HIDEJOURNALENTRY); }
"hidereport"         { return emit_kw(KW_HIDEREPORT); }
"hideresource"       { return emit_kw(KW_HIDERESOURCE); }
"hidetask"           { return emit_kw(KW_HIDETASK); }
"icalreport"         { return emit_kw(KW_ICALREPORT); }
"include"            { return emit_kw(KW_INCLUDE); }
"inherit"            { return emit_kw(KW_INHERIT); }
"isactive"           { return emit_kw(KW_ISACTIVE); }
"ischildof"          { return emit_kw(KW_ISCHILDOF); }
"isdependencyof"     { return emit_kw(KW_ISDEPENDENCYOF); }
"isdutyof"           { return emit_kw(KW_ISDUTYOF); }
"isfeatureof"        { return emit_kw(KW_ISFEATUREOF); }
"isleaf"             { return emit_kw(KW_ISLEAF); }
"ismilestone"        { return emit_kw(KW_ISMILESTONE); }
"isongoing"          { return emit_kw(KW_ISONGOING); }
"isresource"         { return emit_kw(KW_ISRESOURCE); }
"isresponsibilityof" { return emit_kw(KW_ISRESPONSIBILITYOF); }
"istask"             { return emit_kw(KW_ISTASK); }
"isvalid"            { return emit_kw(KW_ISVALID); }
"journalattributes"  { return emit_kw(KW_JOURNALATTRIBUTES); }
"journalentry"       { return emit_kw(KW_JOURNALENTRY); }
"journalmode"        { return emit_kw(KW_JOURNALMODE); }
"leaveallowances"    { return emit_kw(KW_LEAVEALLOWANCES); }
"leaves"             { return emit_kw(KW_LEAVES); }
"left"               { return emit_kw(KW_LEFT); }
"length"             { return emit_kw(KW_LENGTH); }
"limits"             { return emit_kw(KW_LIMITS); }
"listitem"           { return emit_kw(KW_LISTITEM); }
"listtype"           { return emit_kw(KW_LISTTYPE); }
"loadunit"           { return emit_kw(KW_LOADUNIT); }
"macro"              { return emit_kw(KW_MACRO); }
"managers"           { return emit_kw(KW_MANAGERS); }
"mandatory"          { return emit_kw(KW_MANDATORY); }
"markdate"           { return emit_kw(KW_MARKDATE); }
"maxend"             { return emit_kw(KW_MAXEND); }
"maximum"            { return emit_kw(KW_MAXIMUM); }
"maxstart"           { return emit_kw(KW_MAXSTART); }
"milestone"          { return emit_kw(KW_MILESTONE); }
"minend"             { return emit_kw(KW_MINEND); }
"minimum"            { return emit_kw(KW_MINIMUM); }
"minstart"           { return emit_kw(KW_MINSTART); }
"monthlymax"         { return emit_kw(KW_MONTHLYMAX); }
"monthlymin"         { return emit_kw(KW_MONTHLYMIN); }
"navigator"          { return emit_kw(KW_NAVIGATOR); }
"newtask"            { return emit_kw(KW_NEWTASK); }
"nikureport"         { return emit_kw(KW_NIKUREPORT); }
"note"               { return emit_kw(KW_NOTE); }
"novevents"          { return emit_kw(KW_NOVEVENTS); }
"now"                { return emit_kw(KW_NOW); }
"number"             { return emit_kw(KW_NUMBER); }
"numberformat"       { return emit_kw(KW_NUMBERFORMAT); }
"onend"              { return emit_kw(KW_ONEND); }
"onstart"            { return emit_kw(KW_ONSTART); }
"opennodes"          { return emit_kw(KW_OPENNODES); }
"outputdir"          { return emit_kw(KW_OUTPUTDIR); }
"overtime"           { return emit_kw(KW_OVERTIME); }
"period"             { return emit_kw(KW_PERIOD); }
"persistent"         { return emit_kw(KW_PERSISTENT); }
"precedes"           { return emit_kw(KW_PRECEDES); }
"priority"           { return emit_kw(KW_PRIORITY); }
"project"            { return emit_kw(KW_PROJECT); }
"projectid"          { return emit_kw(KW_PROJECTID); }
"projectids"         { return emit_kw(KW_PROJECTIDS); }
"projection"         { return emit_kw(KW_PROJECTION); }
"prolog"             { return emit_kw(KW_PROLOG); }
"purge"              { return emit_kw(KW_PURGE); }
"rate"               { return emit_kw(KW_RATE); }
"rawhtmlhead"        { return emit_kw(KW_RAWHTMLHEAD); }
"reference"          { return emit_kw(KW_REFERENCE); }
"remaining"          { return emit_kw(KW_REMAINING); }
"replace"            { return emit_kw(KW_REPLACE); }
"reportprefix"       { return emit_kw(KW_REPORTPREFIX); }
"resource"           { return emit_kw(KW_RESOURCE); }
"resourceattributes" { return emit_kw(KW_RESOURCEATTRIBUTES); }
"resourceprefix"     { return emit_kw(KW_RESOURCEPREFIX); }
"resourcereport"     { return emit_kw(KW_RESOURCEREPORT); }
"resourceroot"       { return emit_kw(KW_RESOURCEROOT); }
"resources"          { return emit_kw(KW_RESOURCES); }
"responsible"        { return emit_kw(KW_RESPONSIBLE); }
"richtext"           { return emit_kw(KW_RICHTEXT); }
"right"              { return emit_kw(KW_RIGHT); }
"rollupaccount"      { return emit_kw(KW_ROLLUPACCOUNT); }
"rollupresource"     { return emit_kw(KW_ROLLUPRESOURCE); }
"rolluptask"         { return emit_kw(KW_ROLLUPTASK); }
"scale"              { return emit_kw(KW_SCALE); }
"scenario"           { return emit_kw(KW_SCENARIO); }
"scenarios"          { return emit_kw(KW_SCENARIOS); }
"scenariospecific"   { return emit_kw(KW_SCENARIOSPECIFIC); }
"scheduled"          { return emit_kw(KW_SCHEDULED); }
"scheduling"         { return emit_kw(KW_SCHEDULING); }
"schedulingmode"     { return emit_kw(KW_SCHEDULINGMODE); }
"select"             { return emit_kw(KW_SELECT); }
"selfcontained"      { return emit_kw(KW_SELFCONTAINED); }
"shift"              { return emit_kw(KW_SHIFT); }
"shifts"             { return emit_kw(KW_SHIFTS); }
"shorttimeformat"    { return emit_kw(KW_SHORTTIMEFORMAT); }
"sloppy"             { return emit_kw(KW_SLOPPY); }
"sortaccounts"       { return emit_kw(KW_SORTACCOUNTS); }
"sortjournalentries" { return emit_kw(KW_SORTJOURNALENTRIES); }
"sortresources"      { return emit_kw(KW_SORTRESOURCES); }
"sorttasks"          { return emit_kw(KW_SORTTASKS); }
"start"              { return emit_kw(KW_START); }
"startcredit"        { return emit_kw(KW_STARTCREDIT); }
"status"             { return emit_kw(KW_STATUS); }
"statussheet"        { return emit_kw(KW_STATUSSHEET); }
"statussheetreport"  { return emit_kw(KW_STATUSSHEETREPORT); }
"strict"             { return emit_kw(KW_STRICT); }
"summary"            { return emit_kw(KW_SUMMARY); }
"supplement"         { return emit_kw(KW_SUPPLEMENT); }
"tagfile"            { return emit_kw(KW_TAGFILE); }
"task"               { return emit_kw(KW_TASK); }
"taskattributes"     { return emit_kw(KW_TASKATTRIBUTES); }
"taskprefix"         { return emit_kw(KW_TASKPREFIX); }
"taskreport"         { return emit_kw(KW_TASKREPORT); }
"taskroot"           { return emit_kw(KW_TASKROOT); }
"text"               { return emit_kw(KW_TEXT); }
"textreport"         { return emit_kw(KW_TEXTREPORT); }
"timeformat"         { return emit_kw(KW_TIMEFORMAT); }
"timeformat1"        { return emit_kw(KW_TIMEFORMAT1); }
"timeformat2"        { return emit_kw(KW_TIMEFORMAT2); }
"timeoff"            { return emit_kw(KW_TIMEOFF); }
"timesheet"          { return emit_kw(KW_TIMESHEET); }
"timesheetreport"    { return emit_kw(KW_TIMESHEETREPORT); }
"timezone"           { return emit_kw(KW_TIMEZONE); }
"timingresolution"   { return emit_kw(KW_TIMINGRESOLUTION); }
"title"              { return emit_kw(KW_TITLE); }
"tooltip"            { return emit_kw(KW_TOOLTIP); }
"tracereport"        { return emit_kw(KW_TRACEREPORT); }
"trackingscenario"   { return emit_kw(KW_TRACKINGSCENARIO); }
"treelevel"          { return emit_kw(KW_TREELEVEL); }
"vacation"           { return emit_kw(KW_VACATION); }
"warn"               { return emit_kw(KW_WARN); }
"weeklymax"          { return emit_kw(KW_WEEKLYMAX); }
"weeklymin"          { return emit_kw(KW_WEEKLYMIN); }
"weekstartsmonday"   { return emit_kw(KW_WEEKSTARTSMONDAY); }
"weekstartssunday"   { return emit_kw(KW_WEEKSTARTSSUNDAY); }
"width"              { return emit_kw(KW_WIDTH); }
"work"               { return emit_kw(KW_WORK); }
"workinghours"       { return emit_kw(KW_WORKINGHOURS); }
"yearlyworkingdays"  { return emit_kw(KW_YEARLYWORKINGDAYS); }

    /* Identifier / keyword: letters, digits, '_', ':', high UTF-8 bytes.
     * '.' is NOT included; it is emitted as TK_DOT so that dotted dependency
     * paths (e.g. deliveries.start) can be resolved component-wise by the LSP. */
[a-zA-Z_\x80-\xFF][a-zA-Z0-9_:\x80-\xFF]* { return emit_plain(TK_IDENT); }

"{"     { return emit_plain(TK_LBRACE);   }
"}"     { return emit_plain(TK_RBRACE);   }
"["     { return emit_plain(TK_LBRACKET); }
"]"     { return emit_plain(TK_RBRACKET); }
"!"     { return emit_plain(TK_BANG);     }
"+"     { return emit_plain(TK_PLUS);     }
"-"     { return emit_plain(TK_MINUS);    }
"."     { return emit_plain(TK_DOT);      }
":"     { return emit_plain(TK_COLON);    }
","     { return emit_plain(TK_COMMA);    }
"%"     { return emit_plain(TK_PERCENT);  }
"$"     { return emit_plain(TK_DOLLAR);   }

.       { return emit_plain(TK_ERROR); }

%%
