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

/* ── Soft statement terminator (TK_EOL) ──────────────────────────────────── *
 *
 * TaskJuggler statements have no explicit terminator, which forced the
 * grammar's open-ended `gen_expr` rule to guess where a statement ends and
 * produced thousands of shift/reduce conflicts.  We give the grammar a soft
 * terminator by post-processing the raw token stream: the flex scanner is
 * generated as raw_yylex() (via YY_DECL) and the real yylex() (defined at the
 * bottom of this file) wraps it, inserting a synthetic TK_EOL between a token
 * that can END a statement and a newline-separated token that can BEGIN one
 * (and before `}` / EOF).  A line break in the middle of an expression — where
 * the next token is an operator that cannot begin a statement, e.g. a continued
 * `!= ...` comparison — inserts nothing, so multi-line expressions and
 * comma-continued lists are preserved.
 *
 * The same wrapper also synthesises the closing `}` of any body left open at
 * EOF, so a file being edited (with unclosed braces) still reduces and shows
 * its partial declarations in the outline.                                    */
#define YY_DECL int raw_yylex(void)
int  yylex(void);
void reset_eol_state(void);

/* Set by the INITIAL newline rule; sampled and cleared by the yylex wrapper. */
int g_pending_newline = 0;

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

extern char *g_push_tok_span(int kind,
                             uint32_t sl, uint32_t sc,
                             uint32_t el, uint32_t ec,
                             const char *text, size_t len);

/* store_semantic_token: used for comments (not returned to parser). */
static void store_semantic_token(int token) {
    uint32_t el = (uint32_t)(yylineno - 1);
    uint32_t ec = (uint32_t)yycolumn;
    g_push_tok_span(token, tok_sl, tok_sc, el, ec, yytext, (size_t)yyleng);
}


/* emit_kw: keyword / date / duration — recorded in sem-tokens AND returned.
 * The value-stack Token.text reuses the arena copy g_push_tok_span interns
 * (no separate strdup); token_free() treats it as a borrowed pointer. */
static int emit_kw(int code) {
    uint32_t el = (uint32_t)(yylineno - 1);
    uint32_t ec = (uint32_t)yycolumn;
    char *text = g_push_tok_span(code, tok_sl, tok_sc, el, ec, yytext, (size_t)yyleng);
    yylval.tok = (Token){ code, {tok_sl, tok_sc}, {el, ec}, text };
    return code;
}

/* emit_plain: identifier / number / punctuation.
 * Tokens needed by hover/completion/signature/dep-validation are also
 * recorded in sem-tokens so that downstream passes can work without a
 * separate full token array. */
static int emit_plain(int code) {
    uint32_t el = (uint32_t)(yylineno - 1);
    uint32_t ec = (uint32_t)yycolumn;
    char *text = NULL;
    switch (code) {
    case TK_IDENT:
    case TK_LBRACE:    case TK_RBRACE:
    case TK_LBRACKET:  case TK_RBRACKET:
    case TK_BANG:      case TK_DOT:   case TK_COMMA:
        text = g_push_tok_span(code, tok_sl, tok_sc, el, ec, yytext, (size_t)yyleng);
        break;
    default:
        break;
    }
    /* Of the tokens routed through emit_plain, only TK_IDENT has its lexeme
     * read by the grammar (task/resource ids and dotted dependency / prefix
     * paths, plus the keyword-id fallback in alloc_tj_node).  The punctuation
     * kinds are recorded as spans but their value-stack Token needs no text;
     * numbers, operators and the error token are not recorded at all.  Reuse
     * the arena copy interned above for TK_IDENT and leave the rest NULL.
     * (Cursor-position queries read the arena-backed TokenSpan, not this
     * value-stack Token, so they are unaffected.) */
    yylval.tok = (Token){ code, {tok_sl, tok_sc}, {el, ec},
                          (code == TK_IDENT) ? text : NULL };
    return code;
}

/* emit_kw_mfrag: multi-fragment tokens (strings, scissors). Uses yytext. */
static int emit_kw_mfrag(int code) {
    uint32_t el = (uint32_t)(yylineno - 1);
    uint32_t ec = (uint32_t)yycolumn;
    char *text = g_push_tok_span(code, mfrag_sl, mfrag_sc, el, ec, yytext, (size_t)yyleng);
    yylval.tok = (Token){ code, {mfrag_sl, mfrag_sc}, {el, ec}, text };
    return code;
}

/* emit_kw_mfrag_text: like emit_kw_mfrag but uses an explicit text string
 * (needed for tokens whose body is accumulated across multiple lex rules).
 * @p len is the accumulated length (str_len) so no strlen is needed. */
static int emit_kw_mfrag_text(int code, const char *text, size_t len) {
    uint32_t el = (uint32_t)(yylineno - 1);
    uint32_t ec = (uint32_t)yycolumn;
    char *interned = g_push_tok_span(code, mfrag_sl, mfrag_sc, el, ec, text, len);
    yylval.tok = (Token){ code, {mfrag_sl, mfrag_sc}, {el, ec}, interned };
    return code;
}

%}

%option noyywrap
%option yylineno
%option 8bit
%option full

/* Start conditions for tokens whose bodies span multiple lexer fragments. */
%x BLOCK_COMMENT
%x SCISSORS
%x DQUOTE
%x SQUOTE

%%

[ \t\r\f\v]+    { /* skip whitespace */ }
\n              { yycolumn = 0; g_pending_newline = 1; }

#[^\n]*         { store_semantic_token(TK_LINE_COMMENT); /* not returned to parser */ }
\/\/[^\n]*      { store_semantic_token(TK_LINE_COMMENT); /* not returned to parser */ }

"/*"            { mfrag_sl = tok_sl; mfrag_sc = tok_sc;
                  STR_RESET(); STR_APPEND("/*", 2); BEGIN(BLOCK_COMMENT); }
<BLOCK_COMMENT>{
    "*/"        { STR_APPEND("*/", 2); STR_FINISH();
                  uint32_t el = (uint32_t)(yylineno - 1);
                  uint32_t ec = (uint32_t)yycolumn;
                  g_push_tok_span(TK_BLOCK_COMMENT, mfrag_sl, mfrag_sc, el, ec, str_buf, (size_t)str_len);
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
                  BEGIN(INITIAL); return emit_kw_mfrag_text(TK_MULTI_LINE_STR, str_buf, (size_t)str_len); }
    \n          { STR_APPEND(yytext, yyleng); yycolumn = 0; }
    [^-\n]+     { STR_APPEND(yytext, yyleng); }
    "-"         { STR_APPEND(yytext, yyleng); }
    <<EOF>>     { BEGIN(INITIAL); return emit_plain(TK_ERROR); }
}

\"              { mfrag_sl = tok_sl; mfrag_sc = tok_sc;
                  STR_RESET(); STR_APPEND("\"", 1); BEGIN(DQUOTE); }
<DQUOTE>{
    \"          { STR_APPEND("\"", 1); STR_FINISH();
                  BEGIN(INITIAL); return emit_kw_mfrag_text(TK_STR, str_buf, (size_t)str_len); }
    \n          { yycolumn = 0;
                  BEGIN(INITIAL); return emit_plain(TK_ERROR); }
    \\[^\n]     { STR_APPEND(yytext, yyleng); }
    [^\"\\\n]+  { STR_APPEND(yytext, yyleng); }
    <<EOF>>     { BEGIN(INITIAL); return emit_plain(TK_ERROR); }
}

    /* Single-quoted string: '...' — valid in TJP as an alternative to "..." */
\'              { mfrag_sl = tok_sl; mfrag_sc = tok_sc;
                  STR_RESET(); STR_APPEND("'", 1); BEGIN(SQUOTE); }
<SQUOTE>{
    \'          { STR_APPEND("'", 1); STR_FINISH();
                  BEGIN(INITIAL); return emit_kw_mfrag_text(TK_STR, str_buf, (size_t)str_len); }
    \n          { yycolumn = 0;
                  BEGIN(INITIAL); return emit_plain(TK_ERROR); }
    \\[^\n]     { STR_APPEND(yytext, yyleng); }
    [^\'\\\n]+  { STR_APPEND(yytext, yyleng); }
    <<EOF>>     { BEGIN(INITIAL); return emit_plain(TK_ERROR); }
}

    /* Date: YYYY-MM-DD[-hh:mm[:ss]][-{+|-}HHMM]: must precede INT so longest match wins.
     * Time, seconds, and timezone suffix are each independently optional per the TJP spec.
     * Timezone is always a leading '-' separator followed by an explicit '+' or '-' sign. */
[0-9]{4}-[0-9]{2}-[0-9]{2}(-[0-9]{2}:[0-9]{2}(:[0-9]{2})?)?(-[+-][0-9]{4})?  { return emit_kw(TK_DATE); }

    /* Duration: (+|-) <duration> (min | h | d | w | m | y) */
[+-]?[0-9]+(min|h|d|w|m|y) { return emit_kw(TK_DURATION); }

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

/* ── yylex wrapper: synthesise TK_EOL statement terminators ──────────────── *
 *
 * See the note at the top of this file.  raw_yylex() is the flex scanner; this
 * wrapper inserts a TK_EOL ahead of a token when the previous token could end
 * a statement and the current one opens a new one (newline-separated), or when
 * the current token closes a body (`}`) or the input ends.  A single-slot
 * lookahead holds the real token back while the parser consumes the TK_EOL.   */

/* A token that can be the LAST token of a statement.  Everything is allowed
 * except punctuation that clearly continues onto more input. */
static int tok_can_end_stmt(int k) {
    switch (k) {
    case TK_COMMA: case TK_DOT:   case TK_COLON:
    case TK_PLUS:  case TK_MINUS: case TK_BANG:
    case TK_LBRACE: case TK_LBRACKET: case TK_DOLLAR:
    case TK_EOL:   case 0:
        return 0;
    default:
        return 1;
    }
}

/* A token that can be the FIRST token of a statement: a keyword, an
 * identifier, or a macro call (`${`).  Literals, operators and closing
 * punctuation never start one, so a line break before them is a
 * continuation, not a boundary. */
static int tok_can_begin_stmt(int k) {
    switch (k) {
    case TK_COMMA: case TK_DOT:   case TK_COLON:
    case TK_PLUS:  case TK_MINUS: case TK_BANG:
    case TK_LBRACE: case TK_RBRACE:
    case TK_LBRACKET: case TK_RBRACKET:
    case TK_PERCENT:
    case TK_INTEGER: case TK_FLOAT: case TK_DATE: case TK_DURATION:
    case TK_STR:   case TK_MULTI_LINE_STR:
    case TK_ERROR: case TK_EOL: case 0:
        return 0;
    default:
        return 1;   /* all KW_* keywords, plus TK_IDENT and TK_DOLLAR */
    }
}

static int   g_prev_can_end  = 0;   /* previous real token could end a stmt */
static int   g_prev_kind      = 0;   /* previous real token kind (for `${`)  */
static int   g_have_buffered  = 0;   /* a real token is held behind a TK_EOL */
static int   g_buffered_kind  = 0;
static Token g_buffered_val;

/* Brace-nesting stack: each open `{` records whether it begins a `${…}` macro
 * invocation (true) or a normal statement body (false).  A `}` is a statement
 * terminator only when it closes a body — never when it closes a macro call,
 * whose `}` is part of the invocation syntax rather than a statement end. */
#define EOL_BRACE_MAX 256
static int   g_brace_is_macro[EOL_BRACE_MAX];
static int   g_brace_depth     = 0;

void reset_eol_state(void) {
    g_pending_newline = 0;
    g_prev_can_end    = 0;
    g_prev_kind       = 0;
    g_have_buffered   = 0;
    g_buffered_kind   = 0;
    g_buffered_val    = (Token){0};
    g_brace_depth     = 0;
}

int yylex(void) {
    int kind;
    int newline_before;

    if (g_have_buffered) {
        kind            = g_buffered_kind;
        yylval.tok      = g_buffered_val;
        g_have_buffered = 0;
        newline_before  = 0;   /* the break was already consumed by the TK_EOL */
    } else {
        g_pending_newline = 0;
        kind              = raw_yylex();
        newline_before    = g_pending_newline;

        /* Mid-edit input often has unclosed bodies (`task t {` with no `}`
         * yet).  Rather than rely on a grammar error production — which would
         * collide with `item: error` and produce reduce/reduce conflicts — we
         * synthesise the missing `}` for each still-open body at EOF so the
         * partial declarations still reduce and appear in the outline.  flex
         * keeps returning 0 at EOF, so each call closes one more level until
         * the brace stack is empty, then the real EOF is delivered. */
        if (kind == 0 && g_brace_depth > 0) {
            uint32_t el = (uint32_t)(yylineno - 1);
            uint32_t ec = (uint32_t)yycolumn;
            kind       = TK_RBRACE;
            yylval.tok = (Token){ TK_RBRACE, {el, ec}, {el, ec}, strdup("}") };
        }
    }

    /* A `}` terminates a statement only when it closes a body, not a `${…}`.
     * Nesting beyond the stack is assumed to be a body (the common case). */
    int macro_close = (kind == TK_RBRACE) &&
                      g_brace_depth > 0 && g_brace_depth <= EOL_BRACE_MAX &&
                      g_brace_is_macro[g_brace_depth - 1];
    int body_close  = (kind == TK_RBRACE) && !macro_close;

    /* Emit a TK_EOL terminator ahead of this token? */
    if (g_prev_can_end &&
        ((newline_before && tok_can_begin_stmt(kind)) ||  /* statement boundary */
         body_close ||                                    /* end of a body      */
         kind == 0)) {                                    /* end of input       */
        g_buffered_kind = kind;
        g_buffered_val  = yylval.tok;
        g_have_buffered = 1;
        g_prev_can_end  = 0;            /* the TK_EOL is now the previous token */
        yylval.tok = (Token){ TK_EOL, {0,0}, {0,0}, NULL };
        return TK_EOL;
    }

    /* Track brace nesting as tokens are actually handed to the parser. */
    if (kind == TK_LBRACE) {
        if (g_brace_depth < EOL_BRACE_MAX)
            g_brace_is_macro[g_brace_depth] = (g_prev_kind == TK_DOLLAR);
        g_brace_depth++;
    } else if (kind == TK_RBRACE && g_brace_depth > 0) {
        g_brace_depth--;
    }

    g_prev_kind    = kind;
    g_prev_can_end = tok_can_end_stmt(kind);
    return kind;
}
