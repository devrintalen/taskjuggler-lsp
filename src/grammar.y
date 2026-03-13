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
 * %code requires goes into both grammar.tab.c and grammar.tab.h, so the
 * types used in %union are visible wherever grammar.tab.h is included
 * (including lexer.yy.c and tools/lexer_test.c).
 */
%code requires {
#include "parser.h"

/* Dynamic array of Symbols, used for body children. */
typedef struct { DocSymbol *arr; int n, cap; } SymArr;

/* Return type for opt_body and body_items rules. */
typedef struct { SymArr syms; LspPos end; } BodyResult;

/* Return type for item rule: either a DocSymbol or nothing. */
typedef struct { DocSymbol sym; int has_sym; } ItemResult;

/* Return type for dep_path and task_ref rules. */
typedef struct {
    int    bang_count; /* number of leading ! tokens */
    char  *path;       /* dotted path, heap-allocated, e.g. "deliveries.start" */
    LspPos start;
    LspPos end;
} TaskRef;
}

%{
#include "parser.h"        /* Token, DocSymbol, ParseResult, LspRange, etc. */
#include "grammar.tab.h"   /* TK_* / KW_* constants, YYSTYPE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Globals defined in parser.c, shared with lexer.l */
extern ParseResult *g_result;

int  yylex(void);
void yyerror(const char *msg);

/* ── Helpers (declared/defined in parser.c) ─────────────────────────────── */

extern void push_doc_symbol    (ParseResult *r, DocSymbol s);
extern void push_diagnostic(ParseResult *r, LspRange range, int severity,
                             const char *msg);
extern int  symbol_kind_for(const char *kw);
extern void push_dep_ref   (ParseResult *r, int bang_count, const char *path,
                             const char **scope, int scope_n,
                             LspPos start, LspPos end);

/* ── Dep-validation scope stack ──────────────────────────────────────────── *
 * Tracks the current task path so that dep refs can capture a scope snapshot
 * at parse time.  Populated via mid-rule actions on sym_kw opt_id opt_name
 * when the keyword is KW_TASK.                                              */
static char  *g_dep_scope[128];
static int    g_dep_scope_n = 0;

static void dep_scope_push(const char *id) {
    if (g_dep_scope_n < 128 && id && id[0])
        g_dep_scope[g_dep_scope_n++] = strdup(id);
}

static void dep_scope_pop(void) {
    if (g_dep_scope_n > 0)
        free(g_dep_scope[--g_dep_scope_n]);
}

/* ── DocSymbol array helper ─────────────────────────────────────────────────── */

static void symarr_push(SymArr *a, DocSymbol s) {
    if (a->n >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 4;
        a->arr = realloc(a->arr, (size_t)a->cap * sizeof(DocSymbol));
    }
    a->arr[a->n++] = s;
}

/* ── Build a DocSymbol from the components of a symbol_decl rule ───────────── */

static DocSymbol make_doc_symbol(Token kw, Token id, Token name, BodyResult body) {
    DocSymbol s = {0};
    s.kind = symbol_kind_for(kw.text);

    if (id.text) {
        s.detail          = id.text;   /* take ownership */
        s.selection_range = (LspRange){ id.start, id.end };
    } else {
        s.detail          = strdup(kw.text);
        s.selection_range = (LspRange){ kw.start, kw.end };
    }

    s.name = name.text ? name.text : strdup(s.detail); /* take ownership */

    /* Range: from keyword start to closing brace (or last known token). */
    LspPos range_end = body.end;
    if (range_end.line == 0 && range_end.character == 0)
        range_end = kw.end;  /* TODO: use last arg-token end if available */
    s.range = (LspRange){ kw.start, range_end };

    s.children     = body.syms.arr;
    s.num_children = body.syms.n;
    s.children_cap = body.syms.cap;

    return s;
}
%}

/* ── Value union ─────────────────────────────────────────────────────────── */

%union {
    Token      tok;   /* single token (kind / start / end / text) */
    DocSymbol     sym;   /* fully built symbol */
    BodyResult body;  /* body: children + closing-brace position */
    ItemResult item;  /* item: optional symbol */
    TaskRef    tref;  /* dep path + bang count */
    int        ival;  /* integer (bang count) */
}

/* ── Token declarations ──────────────────────────────────────────────────── */

%token <tok> KW_PROJECT KW_TASK KW_RESOURCE KW_ACCOUNT KW_SHIFT
%token <tok> KW_ACCOUNTPREFIX KW_ACCOUNTREPORT KW_ACCOUNTROOT KW_ACTIVE
%token <tok> KW_ADOPT KW_AGGREGATE KW_ALERT KW_ALERTLEVELS KW_ALLOCATE
%token <tok> KW_ALTERNATIVE KW_AUTHOR KW_AUXDIR KW_BALANCE KW_BOOKING
%token <tok> KW_CAPTION KW_CELLCOLOR KW_CELLTEXT KW_CENTER KW_CHARGE
%token <tok> KW_CHARGESET KW_COLUMNS KW_COMPLETE KW_COPYRIGHT
%token <tok> KW_CREDITS KW_CURRENCY KW_CURRENCYFORMAT
%token <tok> KW_DAILYMAX KW_DAILYMIN KW_DAILYWORKINGHOURS
%token <tok> KW_DATE KW_DEFINITIONS KW_DEPENDS KW_DETAILS KW_DISABLED
%token <tok> KW_DURATION KW_EFFICIENCY KW_EFFORT KW_EFFORTDONE KW_EFFORTLEFT
%token <tok> KW_EMAIL KW_ENABLED KW_END KW_ENDCREDIT KW_EPILOG
%token <tok> KW_EXPORT KW_EXTEND KW_FAIL KW_FLAGS KW_FONTCOLOR
%token <tok> KW_FOOTER KW_FORMATS KW_GAPDURATION KW_GAPLENGTH
%token <tok> KW_HALIGN KW_HASALERT KW_HEADER KW_HEADLINE KW_HEIGHT
%token <tok> KW_HIDEACCOUNT KW_HIDEJOURNALENTRY KW_HIDEREPORT
%token <tok> KW_HIDERESOURCE KW_HIDETASK KW_ICALREPORT
%token <tok> KW_INCLUDE KW_INHERIT
%token <tok> KW_ISACTIVE KW_ISCHILDOF KW_ISDEPENDENCYOF KW_ISDUTYOF
%token <tok> KW_ISFEATUREOF KW_ISLEAF KW_ISMILESTONE KW_ISONGOING
%token <tok> KW_ISRESOURCE KW_ISRESPONSIBILITYOF KW_ISTASK KW_ISVALID
%token <tok> KW_JOURNALATTRIBUTES KW_JOURNALENTRY KW_JOURNALMODE
%token <tok> KW_LEAVEALLOWANCES KW_LEAVES KW_LEFT KW_LENGTH
%token <tok> KW_LIMITS KW_LISTITEM KW_LISTTYPE KW_LOADUNIT
%token <tok> KW_MACRO KW_MANAGERS KW_MANDATORY KW_MARKDATE
%token <tok> KW_MAXEND KW_MAXIMUM KW_MAXSTART KW_MILESTONE
%token <tok> KW_MINEND KW_MINIMUM KW_MINSTART
%token <tok> KW_MONTHLYMAX KW_MONTHLYMIN
%token <tok> KW_NAVIGATOR KW_NEWTASK KW_NIKUREPORT KW_NOTE
%token <tok> KW_NOVEVENTS KW_NOW KW_NUMBER KW_NUMBERFORMAT
%token <tok> KW_ONEND KW_ONSTART KW_OPENNODES KW_OUTPUTDIR KW_OVERTIME
%token <tok> KW_PERIOD KW_PERSISTENT KW_PRECEDES KW_PRIORITY
%token <tok> KW_PROJECTID KW_PROJECTIDS KW_PROJECTION KW_PROLOG KW_PURGE
%token <tok> KW_RATE KW_RAWHTMLHEAD KW_REFERENCE KW_REMAINING KW_REPLACE
%token <tok> KW_REPORTPREFIX KW_RESOURCEATTRIBUTES KW_RESOURCEPREFIX
%token <tok> KW_RESOURCEREPORT KW_RESOURCEROOT KW_RESOURCES KW_RESPONSIBLE
%token <tok> KW_RICHTEXT KW_RIGHT
%token <tok> KW_ROLLUPACCOUNT KW_ROLLUPRESOURCE KW_ROLLUPTASK
%token <tok> KW_SCALE KW_SCENARIO KW_SCENARIOS KW_SCENARIOSPECIFIC
%token <tok> KW_SCHEDULED KW_SCHEDULING KW_SCHEDULINGMODE
%token <tok> KW_SELECT KW_SELFCONTAINED KW_SHIFTS KW_SHORTTIMEFORMAT
%token <tok> KW_SLOPPY KW_SORTACCOUNTS KW_SORTJOURNALENTRIES
%token <tok> KW_SORTRESOURCES KW_SORTTASKS
%token <tok> KW_START KW_STARTCREDIT KW_STATUS KW_STATUSSHEET
%token <tok> KW_STATUSSHEETREPORT KW_STRICT KW_SUMMARY KW_SUPPLEMENT
%token <tok> KW_TAGFILE KW_TASKATTRIBUTES KW_TASKPREFIX KW_TASKREPORT
%token <tok> KW_TASKROOT KW_TEXT KW_TEXTREPORT
%token <tok> KW_TIMEFORMAT KW_TIMEFORMAT1 KW_TIMEFORMAT2 KW_TIMEOFF
%token <tok> KW_TIMESHEET KW_TIMESHEETREPORT KW_TIMEZONE KW_TIMINGRESOLUTION
%token <tok> KW_TITLE KW_TOOLTIP KW_TRACEREPORT KW_TRACKINGSCENARIO
%token <tok> KW_TREELEVEL KW_VACATION KW_WARN
%token <tok> KW_WEEKLYMAX KW_WEEKLYMIN KW_WEEKSTARTSMONDAY KW_WEEKSTARTSSUNDAY
%token <tok> KW_WIDTH KW_WORK KW_WORKINGHOURS KW_YEARLYWORKINGDAYS

%token <tok> TK_IDENT
%token <tok> TK_STR TK_INTEGER TK_FLOAT TK_DATE TK_DURATION
%token <tok> TK_LBRACE TK_RBRACE
%token <tok> TK_LBRACKET TK_RBRACKET
%token <tok> TK_BANG TK_PLUS TK_MINUS TK_DOT TK_COLON TK_COMMA TK_PERCENT TK_DOLLAR
%token <tok> TK_MULTI_LINE_STR
%token <tok> TK_ERROR
%token       TK_LINE_COMMENT TK_BLOCK_COMMENT  /* stored in token array only; never returned to parser */

/* ── Non-terminal types ──────────────────────────────────────────────────── */

%type <sym>  symbol_decl report_decl navigator_decl scenario_decl
%type <sym>  timesheet_decl statussheet_decl tagfile_decl journalentry_decl
%type <tok>  sym_kw report_kw opt_id opt_name opt_version
%type <tok>  num_val dur_unit string_val extend_target supplement_target
%type <body> opt_body body_items
%type <tref> dep_path task_ref
%type <ival> bang_seq

%type <item> item

/* ── Grammar ─────────────────────────────────────────────────────────────── */

%%

/* ── Top-level file ──────────────────────────────────────────────────────── */

file
    : { g_dep_scope_n = 0; } items
    ;

items
    : /* empty */
    | items item
        {
            if ($2.has_sym)
                push_doc_symbol(g_result, $2.sym);
        }
    ;

/* ── Universal item rule ─────────────────────────────────────────────────── *
 *
 * The grammar is context-free: all statement types are allowed anywhere, even
 * if some (e.g. 'scenario' inside project body) are semantically restricted.
 * Semantic context is validated by downstream passes.
 *
 * NOTE: gen_expr statements (hidetask, cellcolor, sorttasks, etc.) use a
 * greedy expression rule that consumes all non-brace tokens including most
 * KW_* tokens.  The declaration keywords listed in sym_kw and report_kw are
 * excluded from gen_expr to preserve correct symbol extraction.  All other
 * KW_* keywords included in gen_expr may be accidentally consumed as part
 * of the preceding expression if they appear on the very next line.
 *
 * TODO: Fix the gen_expr greedy-consumption issue by either:
 *   (a) Adding a newline token from the lexer to act as a statement terminator
 *   (b) Switching to a GLR parser
 *   (c) Hand-writing a recursive-descent parser
 *
 * ────────────────────────────────────────────────────────────────────────── */
item
    /* ── DocSymbol-introducing declarations ── */
    : symbol_decl
        { $$.sym = $1; $$.has_sym = 1; }
    | report_decl
        { $$.sym = $1; $$.has_sym = 1; }
    | navigator_decl
        { $$.sym = $1; $$.has_sym = 1; }
    | scenario_decl
        { $$.sym = $1; $$.has_sym = 1; }
    | timesheet_decl
        { $$.sym = $1; $$.has_sym = 1; }
    | statussheet_decl
        { $$.sym = $1; $$.has_sym = 1; }
    | tagfile_decl
        { $$.sym = $1; $$.has_sym = 1; }
    | journalentry_decl
        { $$.sym = $1; $$.has_sym = 1; }

    /* ── Structural statements ── */
    | extend_stmt        { $$.has_sym = 0; }
    | supplement_stmt    { $$.has_sym = 0; }
    | macro_stmt         { $$.has_sym = 0; }
    | include_stmt       { $$.has_sym = 0; }

    /* ── Date attributes ── */
    /* Syntax: start <date>                                                   */
    | KW_START TK_DATE
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: end <date>                                                     */
    | KW_END TK_DATE
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: now <date>                                                     */
    | KW_NOW TK_DATE
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: maxend <date>                                                  */
    | KW_MAXEND TK_DATE
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: minend <date>                                                  */
    | KW_MINEND TK_DATE
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: maxstart <date>                                                */
    | KW_MAXSTART TK_DATE
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: minstart <date>                                                */
    | KW_MINSTART TK_DATE
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: markdate <date>                                                */
    | KW_MARKDATE TK_DATE
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }

    /* ── Duration attributes ── */
    /* Syntax: effort <value> (min | h | d | w | m | y)                      */
    | KW_EFFORT dur_val
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: duration <value> (min | h | d | w | m | y)                   */
    | KW_DURATION dur_val
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: length <value> (min | h | d | w | m | y)                     */
    | KW_LENGTH dur_val
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: effortdone <value> (min | h | d | w | m | y)                 */
    | KW_EFFORTDONE dur_val
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: effortleft <value> (min | h | d | w | m | y)                 */
    | KW_EFFORTLEFT dur_val
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: remaining <value> (min | h | d | w | m | y)                  */
    | KW_REMAINING dur_val
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: work <value> (% | min | h | d)                                */
    | KW_WORK num_val dur_unit
        { token_free(&$1); token_free(&$2); token_free(&$3); $$.has_sym = 0; }
    /* Syntax: gaplength <value> (min | h | d | w | m | y)                  */
    | KW_GAPLENGTH dur_val
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: gapduration <value> (min | h | d | w | m | y)                */
    | KW_GAPDURATION dur_val
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: overtime <value>                                               */
    | KW_OVERTIME num_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: sloppy <sloppyness>                                            */
    | KW_SLOPPY num_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }

    /* ── Duration attributes with optional sub-body ──
     *
     * Syntax: dailymax <value> (min | h | d | w | m | y) [{ <attributes> }]
     * The optional body accepts: resources <id> [, <id>...]
     * (and scenario-specific attributes for dailymax/dailymin/etc.)
     */
    | KW_DAILYMAX dur_val opt_body
        { token_free(&$1);
          for (int i = 0; i < $3.syms.n; i++) doc_symbol_free(&$3.syms.arr[i]);
          free($3.syms.arr); $$.has_sym = 0; }
    /* Syntax: dailymin <value> (min | h | d | w | m | y) [{ <attributes> }] */
    | KW_DAILYMIN dur_val opt_body
        { token_free(&$1);
          for (int i = 0; i < $3.syms.n; i++) doc_symbol_free(&$3.syms.arr[i]);
          free($3.syms.arr); $$.has_sym = 0; }
    /* Syntax: weeklymax <value> (min | h | d | w | m | y) [{ <attributes> }] */
    | KW_WEEKLYMAX dur_val opt_body
        { token_free(&$1);
          for (int i = 0; i < $3.syms.n; i++) doc_symbol_free(&$3.syms.arr[i]);
          free($3.syms.arr); $$.has_sym = 0; }
    /* Syntax: weeklymin <value> (min | h | d | w | m | y) [{ <attributes> }] */
    | KW_WEEKLYMIN dur_val opt_body
        { token_free(&$1);
          for (int i = 0; i < $3.syms.n; i++) doc_symbol_free(&$3.syms.arr[i]);
          free($3.syms.arr); $$.has_sym = 0; }
    /* Syntax: monthlymax <value> (min | h | d | w | m | y) [{ <attributes> }] */
    | KW_MONTHLYMAX dur_val opt_body
        { token_free(&$1);
          for (int i = 0; i < $3.syms.n; i++) doc_symbol_free(&$3.syms.arr[i]);
          free($3.syms.arr); $$.has_sym = 0; }
    /* Syntax: monthlymin <value> (min | h | d | w | m | y) [{ <attributes> }] */
    | KW_MONTHLYMIN dur_val opt_body
        { token_free(&$1);
          for (int i = 0; i < $3.syms.n; i++) doc_symbol_free(&$3.syms.arr[i]);
          free($3.syms.arr); $$.has_sym = 0; }
    /* Syntax: maximum <value> (min | h | d | w | m | y) [{ <attributes> }]  */
    | KW_MAXIMUM dur_val opt_body
        { token_free(&$1);
          for (int i = 0; i < $3.syms.n; i++) doc_symbol_free(&$3.syms.arr[i]);
          free($3.syms.arr); $$.has_sym = 0; }
    /* Syntax: minimum <value> (min | h | d | w | m | y) [{ <attributes> }]  */
    | KW_MINIMUM dur_val opt_body
        { token_free(&$1);
          for (int i = 0; i < $3.syms.n; i++) doc_symbol_free(&$3.syms.arr[i]);
          free($3.syms.arr); $$.has_sym = 0; }

    /* ── Numeric attributes ── */
    /* Syntax: complete <percent>                                             */
    | KW_COMPLETE num_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: priority <value>                                               */
    | KW_PRIORITY TK_INTEGER
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: height <INTEGER>                                               */
    | KW_HEIGHT TK_INTEGER
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: width <INTEGER>                                                */
    | KW_WIDTH TK_INTEGER
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: dailyworkinghours <hours>                                      */
    | KW_DAILYWORKINGHOURS num_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: yearlyworkingdays <days>                                       */
    | KW_YEARLYWORKINGDAYS num_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: efficiency (<INTEGER> | <FLOAT>)                               */
    | KW_EFFICIENCY num_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: rate (<INTEGER> | <FLOAT>)                                     */
    | KW_RATE num_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: startcredit (<INTEGER> | <FLOAT>)                              */
    | KW_STARTCREDIT num_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: endcredit (<INTEGER> | <FLOAT>)                                */
    | KW_ENDCREDIT num_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: timingresolution <INTEGER> min                                 */
    | KW_TIMINGRESOLUTION TK_INTEGER TK_IDENT
        { token_free(&$1); token_free(&$2); token_free(&$3); $$.has_sym = 0; }

    /* ── String attributes ── */
    /* Syntax: note <STRING>                                                  */
    | KW_NOTE string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: email <STRING>                                                 */
    | KW_EMAIL string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: headline <text>  (text may be a string literal)               */
    | KW_HEADLINE string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: title <STRING>                                                 */
    | KW_TITLE string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: caption <text>                                                 */
    | KW_CAPTION string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: epilog <STRING>                                                */
    | KW_EPILOG string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: prolog <STRING>                                                */
    | KW_PROLOG string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: left <STRING>                                                  */
    | KW_LEFT string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: right <STRING>                                                 */
    | KW_RIGHT string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: center <text>                                                  */
    | KW_CENTER string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: footer <STRING>                                                */
    | KW_FOOTER string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: header <STRING>                                                */
    | KW_HEADER string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: rawhtmlhead <STRING>                                           */
    | KW_RAWHTMLHEAD string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: listitem <STRING>                                              */
    | KW_LISTITEM string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: currency <symbol>                                              */
    | KW_CURRENCY string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: timezone <zone>  (a string or identifier)                     */
    | KW_TIMEZONE string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    | KW_TIMEZONE TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: timeformat <format>                                            */
    | KW_TIMEFORMAT string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: shorttimeformat <format>                                       */
    | KW_SHORTTIMEFORMAT string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: timeformat1 <format>                                           */
    | KW_TIMEFORMAT1 string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: timeformat2 <format>                                           */
    | KW_TIMEFORMAT2 string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: outputdir <directory>                                          */
    | KW_OUTPUTDIR string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: auxdir <STRING>                                                */
    | KW_AUXDIR string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: copyright <STRING>                                             */
    | KW_COPYRIGHT string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: details <text>  (journalentry attribute)                      */
    | KW_DETAILS string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: summary <text>  (journalentry attribute)                      */
    | KW_SUMMARY string_val
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }

    /* ── ID/identifier attributes ── */
    /* Syntax: projectid <ID>                                                 */
    | KW_PROJECTID TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: trackingscenario <scenario>                                    */
    | KW_TRACKINGSCENARIO TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: alert <alert level>  (journalentry / report column attribute)  */
    | KW_ALERT TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: author <resource>  (journalentry attribute)                    */
    | KW_AUTHOR TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: taskprefix <task ID>                                           */
    | KW_TASKPREFIX dotted_id
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: resourceprefix <resource ID>                                   */
    | KW_RESOURCEPREFIX TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: accountprefix <account ID>                                     */
    | KW_ACCOUNTPREFIX TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: reportprefix <report ID>                                       */
    | KW_REPORTPREFIX TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: taskroot (<ABSOLUTE_ID> | <ID>)                               */
    | KW_TASKROOT dotted_id
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: resourceroot <resource>                                        */
    | KW_RESOURCEROOT TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: accountroot <ID>                                               */
    | KW_ACCOUNTROOT TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: purge <attribute> <ID>                                         */
    | KW_PURGE TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }

    /* ── balance ──
     * Syntax: balance (<cost account> <ID> | -)
     */
    | KW_BALANCE TK_IDENT TK_IDENT
        { token_free(&$1); token_free(&$2); token_free(&$3); $$.has_sym = 0; }
    | KW_BALANCE TK_MINUS
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }

    /* ── ID-list attributes ── */
    /* Syntax: responsible <resource> [, <resource>...]                       */
    | KW_RESPONSIBLE id_list
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: managers <resource> [, <resource>...]                          */
    | KW_MANAGERS id_list
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: scenarios <ID> [, <ID>...]                                     */
    | KW_SCENARIOS id_list
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: flags <ID> [, <ID>...]                                         */
    | KW_FLAGS id_list
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: adopt (<ABSOLUTE_ID> | <ID>) [, ...]                           */
    | KW_ADOPT dotted_id_list
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: alternative <resource> [, <resource>...]                       */
    | KW_ALTERNATIVE id_list
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: projectids <ID> [, <ID>...]                                    */
    | KW_PROJECTIDS id_list
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: resources <resource> [, <resource>...]
     * (inside dailymax/dailymin/etc. body)                                   */
    | KW_RESOURCES id_list
        { token_free(&$1); $$.has_sym = 0; }

    /* ── No-argument keyword statements ── */
    /* Syntax: milestone                                                      */
    | KW_MILESTONE
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: scheduled                                                      */
    | KW_SCHEDULED
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: mandatory                                                      */
    | KW_MANDATORY
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: persistent                                                     */
    | KW_PERSISTENT
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: weekstartsmonday                                               */
    | KW_WEEKSTARTSMONDAY
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: weekstartssunday                                               */
    | KW_WEEKSTARTSSUNDAY
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: disabled                                                       */
    | KW_DISABLED
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: enabled                                                        */
    | KW_ENABLED
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: replace  (shift attribute: clear inherited shifts)             */
    | KW_REPLACE
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: novevents  (navigation events)                                 */
    | KW_NOVEVENTS
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: onstart  (charge timing)                                       */
    | KW_ONSTART
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: onend    (charge timing)                                       */
    | KW_ONEND
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: strict   (scheduling strictness)                               */
    | KW_STRICT
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: timeoff  (used in workinghours to indicate no work)
     * TODO: verify exact tj3man syntax for timeoff                          */
    | KW_TIMEOFF
        { token_free(&$1); $$.has_sym = 0; }

    /* ── Enum-keyword attributes ── */
    /* Syntax: scheduling (alap | asap)                                       */
    | KW_SCHEDULING TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: schedulingmode (planning | projection)                         */
    | KW_SCHEDULINGMODE TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: aggregate (resources | tasks)                                  */
    | KW_AGGREGATE TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: selfcontained (yes | no)                                       */
    | KW_SELFCONTAINED TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: active (yes | no)                                              */
    | KW_ACTIVE TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: journalmode (journal | journal_sub | status_dep | ...)         */
    | KW_JOURNALMODE TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: loadunit (days | hours | longauto | minutes | ...)             */
    | KW_LOADUNIT TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: select (maxloaded | minloaded | minallocated | order | random) */
    | KW_SELECT TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: scale (hour | day | week | month | quarter | year)
     * (column attribute)                                                      */
    | KW_SCALE TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }
    /* Syntax: listtype (bullets | comma | numbered)
     * (column attribute)                                                      */
    | KW_LISTTYPE TK_IDENT
        { token_free(&$1); token_free(&$2); $$.has_sym = 0; }

    /* ── Interval attributes ── */
    /* Syntax: period <interval1>  (date - date | date + N unit)             */
    | KW_PERIOD interval2
        { token_free(&$1); $$.has_sym = 0; }

    /* ── Dependency and allocation statements ── */
    /* Syntax: depends (<ABSOLUTE ID> | <ID> | <RELATIVE ID>) [{ <attrs> }]
     *                 [, ... ]
     * The optional body accepts: gaplength, gapduration                     */
    | KW_DEPENDS dep_ref_list
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: precedes (<ABSOLUTE ID> | <ID> | <RELATIVE ID>) [{ <attrs> }]
     *                  [, ... ]                                              */
    | KW_PRECEDES dep_ref_list
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: allocate <resource> [{ <attributes> }] [, <resource> ...]
     * Body attributes: alternative, mandatory, persistent, select,
     *   shift.allocate, shifts.allocate, limits.allocate                    */
    | KW_ALLOCATE alloc_ref_list
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: booking <resource> <interval4> [, <interval4>...] [{ <attrs> }]
     * Body attributes: overtime.booking, sloppy.booking                     */
    | KW_BOOKING TK_IDENT booking_interval_list opt_body
        { token_free(&$1); token_free(&$2);
          for (int i = 0; i < $4.syms.n; i++) doc_symbol_free(&$4.syms.arr[i]);
          free($4.syms.arr); $$.has_sym = 0; }
    /* Syntax: shift <shift> [<interval2>] [, <shift> [<interval2>] ...]
     * (attribute form inside resource/task; differs from the shift declaration) */
    | KW_SHIFT shift_attr_list
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: shifts <shift> [<interval2>] [, <shift> [<interval2>] ...]
     * (same semantics as shift attribute but explicit plural form)          */
    | KW_SHIFTS shift_attr_list
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: limits [{ <attributes> }]                                     */
    | KW_LIMITS opt_body
        { token_free(&$1);
          for (int i = 0; i < $2.syms.n; i++) doc_symbol_free(&$2.syms.arr[i]);
          free($2.syms.arr); $$.has_sym = 0; }
    /* Syntax: projection [{ <attributes> }]                                  */
    | KW_PROJECTION opt_body
        { token_free(&$1);
          for (int i = 0; i < $2.syms.n; i++) doc_symbol_free(&$2.syms.arr[i]);
          free($2.syms.arr); $$.has_sym = 0; }

    /* ── Account/charge attributes ── */
    /* Syntax: chargeset <account> <share> [, <account> <share> ...]
     * where share is a percentage value (number followed by %)              */
    | KW_CHARGESET chargeset_list
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: charge <amount> (onstart | onend | perhour | perday | perweek) */
    | KW_CHARGE num_val TK_IDENT
        { token_free(&$1); token_free(&$2); token_free(&$3); $$.has_sym = 0; }
    /* Syntax: credits <date> <description> <amount> [, ...]                 */
    | KW_CREDITS credits_list
        { token_free(&$1); $$.has_sym = 0; }

    /* ── Leave management ── */
    /* Syntax: leaveallowances annual <date> [-] <value> (min | h | d | w | m | y)
     *         [, annual <date> [-] <value> ...]                             */
    | KW_LEAVEALLOWANCES gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: leaves (project | annual | special | sick | unpaid | holiday |
     *          unemployed) [<name>] <interval3> [, ...]                     */
    | KW_LEAVES gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: vacation [<name>] <interval3> [, <interval3>...]              */
    | KW_VACATION gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: workinghours <weekday> [-<end weekday>] [, ...] (off | TIME-TIME ...)
     * TODO: write precise rule; using gen_expr as approximation             */
    | KW_WORKINGHOURS gen_expr
        { token_free(&$1); $$.has_sym = 0; }

    /* ── Report layout ── */
    /* Syntax: columns <columnid> [{ <attributes> }] [, <columnid> ...]      */
    | KW_COLUMNS column_list
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: formats (csv | html | niku) [, ...]                           */
    | KW_FORMATS formats_list
        { token_free(&$1); $$.has_sym = 0; }

    /* ── Logical-expression statements ─────────────────────────────────────
     * These all take a logical expression argument (operands + operators).
     * gen_expr accepts any sequence of non-brace tokens including most KW_*
     * (e.g. plan.effort, delayed.end, ~isleaf()).
     * See NOTE in item rule about greedy consumption.                       */
    /* Syntax: hidetask (<operand> [...] | @ (all | none))                   */
    | KW_HIDETASK gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: hideresource (<operand> [...] | @ (all | none))               */
    | KW_HIDERESOURCE gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: hideaccount (<operand> [...] | @ (all | none))                */
    | KW_HIDEACCOUNT gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: hidereport (<operand> [...] | @ (all | none))                 */
    | KW_HIDEREPORT gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: hidejournalentry <logicalflagexpression>                      */
    | KW_HIDEJOURNALENTRY gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: rolluptask (<operand> [...] | @ (all | none))                 */
    | KW_ROLLUPTASK gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: rollupresource (<operand> [...] | @ (all | none))             */
    | KW_ROLLUPRESOURCE gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: rollupaccount (<operand> [...] | @ (all | none))              */
    | KW_ROLLUPACCOUNT gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: warn (<operand> [...])                                         */
    | KW_WARN gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: fail (<operand> [...])                                         */
    | KW_FAIL gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: cellcolor (<operand> [...])  (column attribute)               */
    | KW_CELLCOLOR gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: celltext (<operand> [...])   (column attribute)               */
    | KW_CELLTEXT gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: tooltip (<operand> [...])    (column attribute)               */
    | KW_TOOLTIP gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: fontcolor (<operand> [...])  (column attribute)               */
    | KW_FONTCOLOR gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: halign (<operand> [...])     (column attribute)               */
    | KW_HALIGN gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: sorttasks (<tree> | <criteria>) [, <criteria>...]             */
    | KW_SORTTASKS gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: sortresources (<tree> | <criteria>) [, <criteria>...]         */
    | KW_SORTRESOURCES gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: sortaccounts (<tree> | <criteria>) [, <criteria>...]          */
    | KW_SORTACCOUNTS gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: sortjournalentries <ABSOLUTE_ID> [, <ABSOLUTE_ID>...]         */
    | KW_SORTJOURNALENTRIES gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: journalattributes (* | - | (alert | author | date | ...) [...]) */
    | KW_JOURNALATTRIBUTES gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: taskattributes (* | - | (booking | complete | ...) [...])     */
    | KW_TASKATTRIBUTES gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: resourceattributes (* | - | (booking | leaves | ...) [...])   */
    | KW_RESOURCEATTRIBUTES gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: definitions (* | - | (flags | project | ...) [...])           */
    | KW_DEFINITIONS gen_expr
        { token_free(&$1); $$.has_sym = 0; }
    /* Syntax: opennodes ((<ID> | <ABSOLUTE_ID>) [: ...] [, ...])            */
    | KW_OPENNODES gen_expr
        { token_free(&$1); $$.has_sym = 0; }

    /* ── Status / timesheet body attributes ── */
    /* Syntax: status <alert level> <STRING> [{ <attributes> }]
     * (inside timesheet task or statussheet)                                */
    | KW_STATUS TK_IDENT string_val opt_body
        { token_free(&$1); token_free(&$2); token_free(&$3);
          for (int i = 0; i < $4.syms.n; i++) doc_symbol_free(&$4.syms.arr[i]);
          free($4.syms.arr); $$.has_sym = 0; }
    /* Syntax: newtask <task> <STRING> { <attributes> }
     * (inside timesheet)                                                    */
    | KW_NEWTASK TK_IDENT string_val opt_body
        { token_free(&$1); token_free(&$2); token_free(&$3);
          for (int i = 0; i < $4.syms.n; i++) doc_symbol_free(&$4.syms.arr[i]);
          free($4.syms.arr); $$.has_sym = 0; }

    /* ── Format specifiers ── */
    /* Syntax: numberformat <negpfx> <negsfx> <thousandsep> <decimsep> <fracdigits>
     * All five arguments are strings.                                        */
    | KW_NUMBERFORMAT string_val string_val string_val string_val num_val
        { token_free(&$1); token_free(&$2); token_free(&$3);
          token_free(&$4); token_free(&$5); token_free(&$6);
          $$.has_sym = 0; }
    /* Syntax: currencyformat <negpfx> <negsfx> <thousandsep> <decimsep> <fracdigits>
     * All five arguments are strings.                                        */
    | KW_CURRENCYFORMAT string_val string_val string_val string_val num_val
        { token_free(&$1); token_free(&$2); token_free(&$3);
          token_free(&$4); token_free(&$5); token_free(&$6);
          $$.has_sym = 0; }
    /* Syntax: alertlevels <ID> <color name> <color> [, <ID> <color name> <color>...]
     * TODO: write precise rule; using gen_expr as approximation             */
    | KW_ALERTLEVELS gen_expr
        { token_free(&$1); $$.has_sym = 0; }

    /* ── Extend sub-attributes ── */
    /* Syntax: date <id> <name> [{ <attributes> }]
     * (inside 'extend (task|resource) { }' body)                            */
    | KW_DATE TK_IDENT string_val opt_body
        { token_free(&$1); token_free(&$2); token_free(&$3);
          for (int i = 0; i < $4.syms.n; i++) doc_symbol_free(&$4.syms.arr[i]);
          free($4.syms.arr); $$.has_sym = 0; }
    /* Syntax: number <id> <name> [{ <attributes> }]                          */
    | KW_NUMBER TK_IDENT string_val opt_body
        { token_free(&$1); token_free(&$2); token_free(&$3);
          for (int i = 0; i < $4.syms.n; i++) doc_symbol_free(&$4.syms.arr[i]);
          free($4.syms.arr); $$.has_sym = 0; }
    /* Syntax: reference <id> <name> [{ <attributes> }]                       */
    | KW_REFERENCE TK_IDENT string_val opt_body
        { token_free(&$1); token_free(&$2); token_free(&$3);
          for (int i = 0; i < $4.syms.n; i++) doc_symbol_free(&$4.syms.arr[i]);
          free($4.syms.arr); $$.has_sym = 0; }
    /* Syntax: richtext <id> <name> [{ <attributes> }]                        */
    | KW_RICHTEXT TK_IDENT string_val opt_body
        { token_free(&$1); token_free(&$2); token_free(&$3);
          for (int i = 0; i < $4.syms.n; i++) doc_symbol_free(&$4.syms.arr[i]);
          free($4.syms.arr); $$.has_sym = 0; }
    /* Syntax: text <id> <name> [{ <attributes> }]                            */
    | KW_TEXT TK_IDENT string_val opt_body
        { token_free(&$1); token_free(&$2); token_free(&$3);
          for (int i = 0; i < $4.syms.n; i++) doc_symbol_free(&$4.syms.arr[i]);
          free($4.syms.arr); $$.has_sym = 0; }

    /* ── Tokens used only in logical expressions ─────────────────────────── *
     * The isXxx/hasXxx keywords and treelevel appear ONLY in logical expression
     * operands (never as standalone statements).  They are included here as
     * item alternatives purely as error-recovery stubs; in normal parsing
     * they will be consumed by gen_expr in their enclosing statement.       */
    | KW_ISACTIVE opt_args    { $$.has_sym = 0; }
    | KW_ISCHILDOF opt_args   { $$.has_sym = 0; }
    | KW_ISDEPENDENCYOF opt_args { $$.has_sym = 0; }
    | KW_ISDUTYOF opt_args    { $$.has_sym = 0; }
    | KW_ISFEATUREOF opt_args { $$.has_sym = 0; }
    | KW_ISLEAF opt_args      { $$.has_sym = 0; }
    | KW_ISMILESTONE opt_args { $$.has_sym = 0; }
    | KW_ISONGOING opt_args   { $$.has_sym = 0; }
    | KW_ISRESOURCE opt_args  { $$.has_sym = 0; }
    | KW_ISRESPONSIBILITYOF opt_args { $$.has_sym = 0; }
    | KW_ISTASK opt_args      { $$.has_sym = 0; }
    | KW_ISVALID opt_args     { $$.has_sym = 0; }
    | KW_HASALERT opt_args    { $$.has_sym = 0; }
    | KW_TREELEVEL opt_args   { $$.has_sym = 0; }

    /* ── Tokens with unknown/unverified standalone syntax ────────────────── *
     * TODO: verify exact syntax for these keywords with tj3man and write
     *       precise rules.  Using opt_args as a safe fallback for now.      */
    | KW_INHERIT opt_args     { $$.has_sym = 0; }
    | KW_SCENARIOSPECIFIC opt_args { $$.has_sym = 0; }

    /* ── Macro invocation: ${macroname [args...]} ───────────────────────── *
     * Macro invocations use ${...} which tokenises as TK_DOLLAR TK_LBRACE
     * TK_IDENT opt_args TK_RBRACE.  Without this rule, TK_DOLLAR would
     * cause an error, and the TK_RBRACE from the invocation would
     * prematurely close the enclosing task/resource body.                  */
    | TK_DOLLAR TK_LBRACE opt_args TK_RBRACE
        { token_free(&$1); token_free(&$2); token_free(&$4); $$.has_sym = 0; }

    /* ── Fallback: unrecognised TK_IDENT statement ──
     *
     * Handles unknown identifiers and scenario-specific syntax like
     * "delayed:effort 40d" (which the lexer returns as a single TK_IDENT
     * due to ':' being allowed in identifier characters).
     * Also handles any future keywords not yet in the KW_* token set.      */
    | TK_IDENT opt_args opt_body
        {
            for (int i = 0; i < $3.syms.n; i++)
                doc_symbol_free(&$3.syms.arr[i]);
            free($3.syms.arr);
            token_free(&$1);
            $$.has_sym = 0;
        }
    | error
        { $$.has_sym = 0; }
    ;

/* ══════════════════════════════════════════════════════════════════════════ *
 * Declaration rules
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── symbol_decl: project / task / resource / account / shift ───────────── *
 *
 * project Syntax: project [<id>] <name> [<version>] <interval2> [{ <attrs> }]
 * task    Syntax: task [<id>] <name> [{ <attributes> }]
 * resource/account/shift share the same shape as task.
 *
 * All id and name fields are optional for leniency (the LSP should still
 * extract the symbol even if the file is syntactically incomplete).        */
symbol_decl
    : KW_PROJECT opt_id opt_name opt_version interval2 opt_body
        {
            $$ = make_doc_symbol($1, $2, $3, $6);
            token_free(&$1);
            if ($4.text) token_free(&$4); /* discard version string */
            /* TODO: store interval $5 as the project time range */
        }
    | sym_kw opt_id opt_name
      { /* Push task ID onto dep scope before parsing the body. */
        if ($1.kind == KW_TASK) dep_scope_push($2.text); }
      opt_body
        {
            if ($1.kind == KW_TASK) dep_scope_pop();
            $$ = make_doc_symbol($1, $2, $3, $5);
            token_free(&$1);
        }
    ;

sym_kw
    : KW_TASK     { $$ = $1; }
    | KW_RESOURCE { $$ = $1; }
    | KW_ACCOUNT  { $$ = $1; }
    | KW_SHIFT    { $$ = $1; }
    ;

/* ── report_decl: all report types ─────────────────────────────────────── *
 *
 * Standard reports share the pattern: [id] <name> [{ <attributes> }]
 * Syntax: taskreport [<id>] <name> [{ <attributes> }]
 * Syntax: resourcereport [<id>] <name> [{ <attributes> }]
 * Syntax: accountreport [<id>] <name> [{ <attributes> }]
 * Syntax: textreport [<id>] <name> [{ <attributes> }]
 * Syntax: tracereport [<id>] <name> [{ <attributes> }]
 * Syntax: export [<id>] <file name> [{ <attributes> }]
 * Syntax: statussheetreport [<id>] <file name> [{ <attributes> }]
 * Syntax: timesheetreport [<id>] <file name> [{ <attributes> }]
 *
 * Non-standard (no opt_id, file name is positional string):
 * Syntax: icalreport <file name> <STRING> [{ <attributes> }]
 * Syntax: nikureport <file name> <STRING> { <attributes> }  (body required)
 *
 * TODO: Update symbol_kind_for() in parser.c to return appropriate kinds
 *       for report keywords (currently only handles task/resource/etc.).   */
report_decl
    : report_kw opt_id opt_name opt_body
        {
            $$ = make_doc_symbol($1, $2, $3, $4);
            token_free(&$1);
        }
    | KW_ICALREPORT string_val opt_name opt_body
        {
            Token no_id = {0};
            $$ = make_doc_symbol($1, no_id, $2, $4);
            token_free(&$1);
            if ($3.text) token_free(&$3); /* second string (unused as display name) */
        }
    | KW_NIKUREPORT string_val opt_name opt_body
        {
            Token no_id = {0};
            $$ = make_doc_symbol($1, no_id, $2, $4);
            token_free(&$1);
            if ($3.text) token_free(&$3);
        }
    ;

report_kw
    : KW_TASKREPORT        { $$ = $1; }
    | KW_RESOURCEREPORT    { $$ = $1; }
    | KW_ACCOUNTREPORT     { $$ = $1; }
    | KW_TEXTREPORT        { $$ = $1; }
    | KW_TRACEREPORT       { $$ = $1; }
    | KW_EXPORT            { $$ = $1; }
    | KW_STATUSSHEETREPORT { $$ = $1; }
    | KW_TIMESHEETREPORT   { $$ = $1; }
    ;

/* ── navigator_decl ─────────────────────────────────────────────────────── *
 * Syntax: navigator <ID> [{ <attributes> }]
 * Body attribute: hidereport                                                */
navigator_decl
    : KW_NAVIGATOR TK_IDENT opt_body
        {
            Token no_name = {0};
            $$ = make_doc_symbol($1, $2, no_name, $3);
            token_free(&$1);
        }
    ;

/* ── scenario_decl ──────────────────────────────────────────────────────── *
 * Syntax: scenario <id> <name> [{ <attributes> }]
 * Context: project, scenario (recursive)
 * Body attributes: active, disabled, enabled, projection, scenario         */
scenario_decl
    : KW_SCENARIO TK_IDENT opt_name opt_body
        {
            $$ = make_doc_symbol($1, $2, $3, $4);
            token_free(&$1);
        }
    ;

/* ── timesheet_decl ─────────────────────────────────────────────────────── *
 * Syntax: timesheet <resource> <interval4> { <attributes> }
 * Body attributes: newtask, shift.timesheet, status.timesheet, task.timesheet
 * Note: body is required by spec but we use opt_body for leniency.         */
timesheet_decl
    : KW_TIMESHEET TK_IDENT interval3 opt_body
        {
            Token no_name = {0};
            $$ = make_doc_symbol($1, $2, no_name, $4);
            token_free(&$1);
        }
    ;

/* ── statussheet_decl ───────────────────────────────────────────────────── *
 * Syntax: statussheet <reporter> <interval4> [{ <attributes> }]
 * reporter is a resource ID.                                                */
statussheet_decl
    : KW_STATUSSHEET TK_IDENT interval3 opt_body
        {
            Token no_name = {0};
            $$ = make_doc_symbol($1, $2, no_name, $4);
            token_free(&$1);
        }
    ;

/* ── tagfile_decl ───────────────────────────────────────────────────────── *
 * Syntax: tagfile [<id>] <file name> [{ <attributes> }]
 * TODO: verify tj3man syntax for tagfile                                   */
tagfile_decl
    : KW_TAGFILE opt_id opt_name opt_body
        {
            $$ = make_doc_symbol($1, $2, $3, $4);
            token_free(&$1);
        }
    ;

/* ── journalentry_decl ──────────────────────────────────────────────────── *
 * Syntax: journalentry <date> <headline> [{ <attributes> }]
 * Body attributes: alert, author, details, flags.journalentry, summary
 * Note: journalentry is treated as a DocSymbol-producing declaration so that
 *       it appears in the document outline.
 * TODO: decide whether journalentry should be a DocSymbol or just an attribute. */
journalentry_decl
    : KW_JOURNALENTRY TK_DATE string_val opt_body
        {
            Token no_id = {0};
            /* Use the date as the detail and the headline as the name */
            $$ = make_doc_symbol($1, no_id, $3, $4);
            token_free(&$1);
            token_free(&$2); /* date token */
        }
    ;

/* ── extend_stmt ────────────────────────────────────────────────────────── *
 * Syntax: extend (task | resource) [{ <attributes> }]
 * Body attributes: date.extend, number.extend, reference.extend,
 *   richtext.extend, text.extend                                            */
extend_stmt
    : KW_EXTEND extend_target opt_body
        {
            token_free(&$1); token_free(&$2);
            for (int i = 0; i < $3.syms.n; i++) doc_symbol_free(&$3.syms.arr[i]);
            free($3.syms.arr);
        }
    ;

extend_target
    : KW_TASK     { $$ = $1; }
    | KW_RESOURCE { $$ = $1; }
    ;

/* ── supplement_stmt ────────────────────────────────────────────────────── *
 * Syntax: supplement (account <account ID> [{ <attributes> }] |
 *                     report  <report ID>  [{ <attributes> }] |
 *                     resource <resource ID> [{ <attributes> }] |
 *                     task <task ID> [{ <attributes> }])
 * Note: 'report' is not a keyword token, so it is handled as TK_IDENT.    */
supplement_stmt
    : KW_SUPPLEMENT supplement_target dotted_id opt_body
        {
            token_free(&$1); token_free(&$2);
            for (int i = 0; i < $4.syms.n; i++) doc_symbol_free(&$4.syms.arr[i]);
            free($4.syms.arr);
        }
    ;

supplement_target
    : KW_ACCOUNT  { $$ = $1; }
    | KW_TASK     { $$ = $1; }
    | KW_RESOURCE { $$ = $1; }
    | TK_IDENT    { $$ = $1; }  /* 'report' keyword (not in KW_* set) */
    ;

/* ── macro_stmt ─────────────────────────────────────────────────────────── *
 * Syntax: macro <ID> <MACRO>
 * where <MACRO> is the content wrapped in square brackets: [ ... ]
 * The macro body may contain any tokens; we accept a sequence of
 * macro_body_tok (anything except TK_RBRACKET) wrapped in [ ].             */
macro_stmt
    : KW_MACRO TK_IDENT TK_LBRACKET macro_body TK_RBRACKET
        { token_free(&$1); token_free(&$2);
          token_free(&$3); token_free(&$5); }
    ;

/* ── include_stmt ───────────────────────────────────────────────────────── *
 * Syntax (in properties context): include <filename> [{ <attributes> }]
 * Syntax (in project context):    include <filename>
 * We use opt_body for leniency.                                             */
include_stmt
    : KW_INCLUDE string_val opt_body
        {
            token_free(&$1); token_free(&$2);
            for (int i = 0; i < $3.syms.n; i++) doc_symbol_free(&$3.syms.arr[i]);
            free($3.syms.arr);
        }
    ;

/* ══════════════════════════════════════════════════════════════════════════ *
 * Helper non-terminal rules
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── num_val: integer or floating-point number ──────────────────────────── */
num_val
    : TK_INTEGER { $$ = $1; }
    | TK_FLOAT   { $$ = $1; }
    ;

/* ── dur_unit: duration unit identifier (min h d w m y %) ──────────────── *
 * These are plain identifiers in the TJP language; any TK_IDENT is accepted
 * here.  Semantic validation (ensuring the unit is one of min/h/d/w/m/y)
 * is left to downstream passes.                                             */
dur_unit
    : TK_IDENT   { $$ = $1; }
    | TK_PERCENT { $$ = $1; }  /* for 'work' which uses % as a unit */
    ;

/* ── dur_val: duration value = number + unit ────────────────────────────── *
 * Syntax: <value> (min | h | d | w | m | y)
 * Token sequence: TK_INTEGER TK_IDENT  or  TK_FLOAT TK_IDENT
 * Note: The lexer also produces TK_DURATION for signed compact durations
 * like +4m or -2h.  Those appear only in interval syntax (as part of a
 * project date range), not in effort/duration/etc. statements.             */
dur_val
    : num_val dur_unit
        { token_free(&$1); token_free(&$2); }
    ;

/* ── string_val: a quoted or multi-line string ──────────────────────────── */
string_val
    : TK_STR           { $$ = $1; }
    | TK_MULTI_LINE_STR { $$ = $1; }
    ;

/* ── opt_id: optional identifier (the TJP property ID) ─────────────────── */
opt_id
    : /* empty */  { $$ = (Token){0}; }
    | TK_IDENT     { $$ = $1; }
    ;

/* ── opt_name: optional display-name string ─────────────────────────────── */
opt_name
    : /* empty */       { $$ = (Token){0}; }
    | TK_STR            { $$ = $1; }
    | TK_MULTI_LINE_STR { $$ = $1; }
    ;

/* ── opt_version: optional version string (project declaration only) ─────── */
opt_version
    : /* empty */  { $$ = (Token){0}; }
    | TK_STR       { $$ = $1; }
    ;

/* ── interval2: required interval ──────────────────────────────────────── *
 * Syntax: <date> (- <date> | + <duration> (min | h | d | w | m | y))
 * TK_DURATION captures the compact form +Nunit (e.g. +4m) as a single token.
 * The separate-token form (+ N unit) is also accepted.                     */
interval2
    : TK_DATE TK_MINUS TK_DATE
        { token_free(&$1); token_free(&$2); token_free(&$3); }
    | TK_DATE TK_DURATION
        { token_free(&$1); token_free(&$2); }
    | TK_DATE TK_PLUS num_val dur_unit
        { token_free(&$1); token_free(&$2); token_free(&$3); token_free(&$4); }
    ;

/* ── interval3: optional interval ──────────────────────────────────────── *
 * Syntax: <date> [(- <date> | + <duration> unit)]
 * Used in: vacation, leaves, statussheet, timesheet                        */
interval3
    : TK_DATE
        { token_free(&$1); }
    | TK_DATE TK_MINUS TK_DATE
        { token_free(&$1); token_free(&$2); token_free(&$3); }
    | TK_DATE TK_DURATION
        { token_free(&$1); token_free(&$2); }
    | TK_DATE TK_PLUS num_val dur_unit
        { token_free(&$1); token_free(&$2); token_free(&$3); token_free(&$4); }
    ;

/* ── dotted_id: dot-separated identifier path ───────────────────────────── *
 * Syntax: <id> [. <id> [. <id> ...]]
 * Used for: taskroot, taskprefix, adopt targets, supplement target IDs     */
dotted_id
    : TK_IDENT
        { token_free(&$1); }
    | dotted_id TK_DOT TK_IDENT
        { token_free(&$2); token_free(&$3); }
    ;

/* ── dotted_id_list: comma-separated list of dotted IDs ────────────────── */
dotted_id_list
    : dotted_id
    | dotted_id_list TK_COMMA dotted_id
        { token_free(&$2); }
    ;

/* ── id_list: comma-separated list of plain identifiers ─────────────────── */
id_list
    : TK_IDENT
        { token_free(&$1); }
    | id_list TK_COMMA TK_IDENT
        { token_free(&$2); token_free(&$3); }
    ;

/* ── bang_seq: zero or more leading ! tokens ────────────────────────────── *
 * Returns the count of bangs seen (int ival).                               */
bang_seq
    : /* empty */
        { $$ = 0; }
    | bang_seq TK_BANG
        { token_free(&$2); $$ = $1 + 1; }
    ;

/* ── dep_path: dotted identifier path for dep references ────────────────── *
 * Distinct from dotted_id so taskprefix/taskroot are unaffected.           */
dep_path
    : TK_IDENT
        { $$.bang_count = 0; $$.path = strdup($1.text);
          $$.start = $1.start; $$.end = $1.end;
          token_free(&$1); }
    | dep_path TK_DOT TK_IDENT
        { char buf[512];
          snprintf(buf, sizeof(buf), "%s.%s", $1.path, $3.text);
          free($1.path);
          $$.bang_count = 0;
          $$.path  = strdup(buf);
          $$.start = $1.start;
          $$.end   = $3.end;
          token_free(&$2); token_free(&$3); }
    ;

/* ── task_ref: [!...]dep_path  ──────────────────────────────────────────── */
task_ref
    : bang_seq dep_path
        { $$ = $2; $$.bang_count = $1; }
    ;

/* ── dep_ref: task reference with optional body ─────────────────────────── *
 * Used in depends/precedes lists.
 * Body may contain: gaplength, gapduration                                 */
dep_ref
    : task_ref opt_body
        {
            push_dep_ref(g_result, $1.bang_count, $1.path,
                         (const char **)g_dep_scope, g_dep_scope_n,
                         $1.start, $1.end);
            free($1.path);
            for (int i = 0; i < $2.syms.n; i++) doc_symbol_free(&$2.syms.arr[i]);
            free($2.syms.arr);
        }
    ;

/* ── dep_ref_list: comma-separated dependency references ────────────────── */
dep_ref_list
    : dep_ref
    | dep_ref_list TK_COMMA dep_ref
        { token_free(&$2); }
    ;

/* ── alloc_ref: resource ID with optional allocation body ───────────────── *
 * Used in allocate statements.
 * Body may contain: alternative, mandatory, persistent, select,
 *   shift.allocate, shifts.allocate                                        */
alloc_ref
    : TK_IDENT opt_body
        {
            token_free(&$1);
            for (int i = 0; i < $2.syms.n; i++) doc_symbol_free(&$2.syms.arr[i]);
            free($2.syms.arr);
        }
    ;

/* ── alloc_ref_list: comma-separated allocation references ─────────────── */
alloc_ref_list
    : alloc_ref
    | alloc_ref_list TK_COMMA alloc_ref
        { token_free(&$2); }
    ;

/* ── shift_attr_ref: shift ID with optional interval ────────────────────── *
 * Syntax: <shift> [<interval2>]
 * Used in shift/shifts attribute statements.                               */
shift_attr_ref
    : TK_IDENT
        { token_free(&$1); }
    | TK_IDENT interval2
        { token_free(&$1); }
    ;

/* ── shift_attr_list: comma-separated shift references ─────────────────── */
shift_attr_list
    : shift_attr_ref
    | shift_attr_list TK_COMMA shift_attr_ref
        { token_free(&$2); }
    ;

/* ── booking_interval_list: one or more intervals for booking ───────────── *
 * Syntax: <interval4> [, <interval4>...]                                   */
booking_interval_list
    : interval3
    | booking_interval_list TK_COMMA interval3
        { token_free(&$2); }
    ;

/* ── chargeset_list: account + percentage pairs ─────────────────────────── *
 * Syntax: <account> <share> [, <account> <share>...]
 * <share> is a number followed by %, e.g. 50%
 * In the lexer, '50%' is tokenized as TK_INTEGER TK_PERCENT (two tokens).  */
chargeset_list
    : TK_IDENT num_val TK_PERCENT
        { token_free(&$1); token_free(&$2); token_free(&$3); }
    | chargeset_list TK_COMMA TK_IDENT num_val TK_PERCENT
        { token_free(&$2); token_free(&$3); token_free(&$4); token_free(&$5); }
    ;

/* ── credits_list: date + description + amount triples ──────────────────── *
 * Syntax: <date> <description> <amount> [, <date> <description> <amount>...]
 */
credits_list
    : TK_DATE string_val num_val
        { token_free(&$1); token_free(&$2); token_free(&$3); }
    | credits_list TK_COMMA TK_DATE string_val num_val
        { token_free(&$2); token_free(&$3); token_free(&$4); token_free(&$5); }
    ;

/* ── column_id: a column name token ─────────────────────────────────────── *
 * Column IDs include both plain identifiers and many keyword tokens that
 * happen to share names with column IDs (e.g. start, end, effort, alert).
 * Rather than listing all ~60+ valid column IDs, we accept any single
 * gen_expr_tok here.  The columns_list rule uses commas as delimiters so
 * the column ID consumes only one token.                                    */
column_id
    : TK_IDENT { token_free(&$1); }
    /* Column IDs that are also keyword tokens: */
    | KW_ALERT    { token_free(&$1); }
    | KW_COMPLETE { token_free(&$1); }
    | KW_DATE     { token_free(&$1); }
    | KW_DURATION { token_free(&$1); }
    | KW_EFFORT   { token_free(&$1); }
    | KW_EMAIL    { token_free(&$1); }
    | KW_END      { token_free(&$1); }
    | KW_FLAGS    { token_free(&$1); }
    | KW_LEAVES   { token_free(&$1); }
    | KW_LENGTH   { token_free(&$1); }
    | KW_NOTE     { token_free(&$1); }
    | KW_PRIORITY { token_free(&$1); }
    | KW_RATE     { token_free(&$1); }
    | KW_REMAINING { token_free(&$1); }
    | KW_START    { token_free(&$1); }
    | KW_STATUS   { token_free(&$1); }
    | KW_WORK     { token_free(&$1); }
    /* TODO: add all other column IDs as seen in tj3man columnid listing:
     * activetasks, annualleave, annualleavebalance, annualleavelist, bsi,
     * chart, children, closedtasks, competitorcount, competitors, cost,
     * criticalness, daily, directreports, duties, effortdone, effortleft,
     * followers, id, journal, managers, monthly, name, no, overtime, ...
     * Most of these are plain TK_IDENT so they are already covered.        */
    ;

/* ── column_entry: single column specification ──────────────────────────── *
 * Syntax: <columnid> [{ <attributes> }]
 * Body attributes: cellcolor, celltext, end, fontcolor, halign, listitem,
 *   listtype, period, scale, start, timeformat1, timeformat2, title,
 *   tooltip, width                                                          */
column_entry
    : column_id opt_body
        {
            for (int i = 0; i < $2.syms.n; i++) doc_symbol_free(&$2.syms.arr[i]);
            free($2.syms.arr);
        }
    ;

/* ── column_list: comma-separated column specifications ─────────────────── */
column_list
    : column_entry
    | column_list TK_COMMA column_entry
        { token_free(&$2); }
    ;

/* ── formats_list: comma-separated format identifiers ───────────────────── *
 * Syntax: (csv | html | niku) [, ...]
 * These are TK_IDENT tokens (not keywords).                                */
formats_list
    : TK_IDENT
        { token_free(&$1); }
    | formats_list TK_COMMA TK_IDENT
        { token_free(&$2); token_free(&$3); }
    ;

/* ── gen_expr: general expression ───────────────────────────────────────── *
 *
 * Accepts any sequence of one or more non-brace tokens.  Used for logical
 * expressions (hidetask, fail, warn, etc.) and complex list arguments
 * (leaves, workinghours, alertlevels, etc.).
 *
 * IMPORTANT: gen_expr includes most KW_* tokens so that expressions like
 *   "plan.effort = 0" (where 'effort' is KW_EFFORT) parse correctly.
 * EXCLUDED from gen_expr (declaration keywords):
 *   KW_PROJECT, KW_TASK, KW_RESOURCE, KW_ACCOUNT, KW_SHIFT,
 *   KW_TASKREPORT, KW_RESOURCEREPORT, KW_ACCOUNTREPORT, KW_TEXTREPORT,
 *   KW_TRACEREPORT, KW_ICALREPORT, KW_NIKUREPORT, KW_EXPORT,
 *   KW_STATUSSHEETREPORT, KW_TIMESHEETREPORT, KW_NAVIGATOR, KW_TAGFILE,
 *   KW_MACRO, KW_INCLUDE, KW_SUPPLEMENT, KW_SCENARIO, KW_EXTEND,
 *   KW_TIMESHEET, KW_STATUSSHEET, KW_JOURNALENTRY
 *
 * NOTE: Because gen_expr uses greedy matching (bison's default shift
 * resolution), a gen_expr statement may accidentally consume the keyword
 * that begins the NEXT statement on the following line.  This is a known
 * limitation.  See NOTE in the item rule above.                            */
gen_expr
    : gen_expr_tok
    | gen_expr gen_expr_tok
    ;

gen_expr_tok
    : TK_IDENT          { token_free(&$1); }
    | TK_INTEGER        { token_free(&$1); }
    | TK_FLOAT          { token_free(&$1); }
    | TK_STR            { token_free(&$1); }
    | TK_DATE           { token_free(&$1); }
    | TK_DURATION       { token_free(&$1); }
    | TK_BANG           { token_free(&$1); }
    | TK_PLUS           { token_free(&$1); }
    | TK_MINUS          { token_free(&$1); }
    | TK_DOT            { token_free(&$1); }
    | TK_COLON          { token_free(&$1); }
    | TK_COMMA          { token_free(&$1); }
    | TK_PERCENT        { token_free(&$1); }
    | TK_DOLLAR         { token_free(&$1); }
    | TK_LBRACKET       { token_free(&$1); }
    | TK_RBRACKET       { token_free(&$1); }
    | TK_MULTI_LINE_STR { token_free(&$1); }
    | TK_ERROR          { token_free(&$1); }
    /* Attribute/expression keywords (included so gen_expr can parse
     * expressions like "plan.effort = 0" or "~isleaf()"):                  */
    | KW_ACCOUNTPREFIX    { token_free(&$1); }
    | KW_ACCOUNTROOT      { token_free(&$1); }
    | KW_ACTIVE           { token_free(&$1); }
    | KW_ADOPT            { token_free(&$1); }
    | KW_AGGREGATE        { token_free(&$1); }
    | KW_ALERT            { token_free(&$1); }
    | KW_ALERTLEVELS      { token_free(&$1); }
    | KW_ALLOCATE         { token_free(&$1); }
    | KW_ALTERNATIVE      { token_free(&$1); }
    | KW_AUTHOR           { token_free(&$1); }
    | KW_AUXDIR           { token_free(&$1); }
    | KW_BALANCE          { token_free(&$1); }
    | KW_BOOKING          { token_free(&$1); }
    | KW_CAPTION          { token_free(&$1); }
    | KW_CELLCOLOR        { token_free(&$1); }
    | KW_CELLTEXT         { token_free(&$1); }
    | KW_CENTER           { token_free(&$1); }
    | KW_CHARGE           { token_free(&$1); }
    | KW_CHARGESET        { token_free(&$1); }
    | KW_COLUMNS          { token_free(&$1); }
    | KW_COMPLETE         { token_free(&$1); }
    | KW_COPYRIGHT        { token_free(&$1); }
    | KW_CREDITS          { token_free(&$1); }
    | KW_CURRENCY         { token_free(&$1); }
    | KW_CURRENCYFORMAT   { token_free(&$1); }
    | KW_DAILYMAX         { token_free(&$1); }
    | KW_DAILYMIN         { token_free(&$1); }
    | KW_DAILYWORKINGHOURS { token_free(&$1); }
    | KW_DATE             { token_free(&$1); }
    | KW_DEFINITIONS      { token_free(&$1); }
    | KW_DEPENDS          { token_free(&$1); }
    | KW_DETAILS          { token_free(&$1); }
    | KW_DISABLED         { token_free(&$1); }
    | KW_DURATION         { token_free(&$1); }
    | KW_EFFICIENCY       { token_free(&$1); }
    | KW_EFFORT           { token_free(&$1); }
    | KW_EFFORTDONE       { token_free(&$1); }
    | KW_EFFORTLEFT       { token_free(&$1); }
    | KW_EMAIL            { token_free(&$1); }
    | KW_ENABLED          { token_free(&$1); }
    | KW_END              { token_free(&$1); }
    | KW_ENDCREDIT        { token_free(&$1); }
    | KW_EPILOG           { token_free(&$1); }
    | KW_FAIL             { token_free(&$1); }
    | KW_FLAGS            { token_free(&$1); }
    | KW_FONTCOLOR        { token_free(&$1); }
    | KW_FOOTER           { token_free(&$1); }
    | KW_FORMATS          { token_free(&$1); }
    | KW_GAPDURATION      { token_free(&$1); }
    | KW_GAPLENGTH        { token_free(&$1); }
    | KW_HALIGN           { token_free(&$1); }
    | KW_HASALERT         { token_free(&$1); }
    | KW_HEADER           { token_free(&$1); }
    | KW_HEADLINE         { token_free(&$1); }
    | KW_HEIGHT           { token_free(&$1); }
    | KW_HIDEACCOUNT      { token_free(&$1); }
    | KW_HIDEJOURNALENTRY { token_free(&$1); }
    | KW_HIDEREPORT       { token_free(&$1); }
    | KW_HIDERESOURCE     { token_free(&$1); }
    | KW_HIDETASK         { token_free(&$1); }
    | KW_INHERIT          { token_free(&$1); }
    | KW_ISACTIVE         { token_free(&$1); }
    | KW_ISCHILDOF        { token_free(&$1); }
    | KW_ISDEPENDENCYOF   { token_free(&$1); }
    | KW_ISDUTYOF         { token_free(&$1); }
    | KW_ISFEATUREOF      { token_free(&$1); }
    | KW_ISLEAF           { token_free(&$1); }
    | KW_ISMILESTONE      { token_free(&$1); }
    | KW_ISONGOING        { token_free(&$1); }
    | KW_ISRESOURCE       { token_free(&$1); }
    | KW_ISRESPONSIBILITYOF { token_free(&$1); }
    | KW_ISTASK           { token_free(&$1); }
    | KW_ISVALID          { token_free(&$1); }
    | KW_JOURNALATTRIBUTES { token_free(&$1); }
    | KW_JOURNALMODE      { token_free(&$1); }
    | KW_LEAVEALLOWANCES  { token_free(&$1); }
    | KW_LEAVES           { token_free(&$1); }
    | KW_LEFT             { token_free(&$1); }
    | KW_LENGTH           { token_free(&$1); }
    | KW_LIMITS           { token_free(&$1); }
    | KW_LISTITEM         { token_free(&$1); }
    | KW_LISTTYPE         { token_free(&$1); }
    | KW_LOADUNIT         { token_free(&$1); }
    | KW_MANAGERS         { token_free(&$1); }
    | KW_MANDATORY        { token_free(&$1); }
    | KW_MARKDATE         { token_free(&$1); }
    | KW_MAXEND           { token_free(&$1); }
    | KW_MAXIMUM          { token_free(&$1); }
    | KW_MAXSTART         { token_free(&$1); }
    | KW_MILESTONE        { token_free(&$1); }
    | KW_MINEND           { token_free(&$1); }
    | KW_MINIMUM          { token_free(&$1); }
    | KW_MINSTART         { token_free(&$1); }
    | KW_MONTHLYMAX       { token_free(&$1); }
    | KW_MONTHLYMIN       { token_free(&$1); }
    | KW_NEWTASK          { token_free(&$1); }
    | KW_NOTE             { token_free(&$1); }
    | KW_NOVEVENTS        { token_free(&$1); }
    | KW_NOW              { token_free(&$1); }
    | KW_NUMBER           { token_free(&$1); }
    | KW_NUMBERFORMAT     { token_free(&$1); }
    | KW_ONEND            { token_free(&$1); }
    | KW_ONSTART          { token_free(&$1); }
    | KW_OPENNODES        { token_free(&$1); }
    | KW_OUTPUTDIR        { token_free(&$1); }
    | KW_OVERTIME         { token_free(&$1); }
    | KW_PERIOD           { token_free(&$1); }
    | KW_PERSISTENT       { token_free(&$1); }
    | KW_PRECEDES         { token_free(&$1); }
    | KW_PRIORITY         { token_free(&$1); }
    | KW_PROJECTID        { token_free(&$1); }
    | KW_PROJECTIDS       { token_free(&$1); }
    | KW_PROJECTION       { token_free(&$1); }
    | KW_PROLOG           { token_free(&$1); }
    | KW_PURGE            { token_free(&$1); }
    | KW_RATE             { token_free(&$1); }
    | KW_RAWHTMLHEAD      { token_free(&$1); }
    | KW_REFERENCE        { token_free(&$1); }
    | KW_REMAINING        { token_free(&$1); }
    | KW_REPLACE          { token_free(&$1); }
    | KW_REPORTPREFIX     { token_free(&$1); }
    | KW_RESOURCEATTRIBUTES { token_free(&$1); }
    | KW_RESOURCEPREFIX   { token_free(&$1); }
    | KW_RESOURCEROOT     { token_free(&$1); }
    | KW_RESOURCES        { token_free(&$1); }
    | KW_RESPONSIBLE      { token_free(&$1); }
    | KW_RICHTEXT         { token_free(&$1); }
    | KW_RIGHT            { token_free(&$1); }
    | KW_ROLLUPACCOUNT    { token_free(&$1); }
    | KW_ROLLUPRESOURCE   { token_free(&$1); }
    | KW_ROLLUPTASK       { token_free(&$1); }
    | KW_SCALE            { token_free(&$1); }
    | KW_SCENARIOS        { token_free(&$1); }
    | KW_SCENARIOSPECIFIC { token_free(&$1); }
    | KW_SCHEDULED        { token_free(&$1); }
    | KW_SCHEDULING       { token_free(&$1); }
    | KW_SCHEDULINGMODE   { token_free(&$1); }
    | KW_SELECT           { token_free(&$1); }
    | KW_SELFCONTAINED    { token_free(&$1); }
    | KW_SHIFTS           { token_free(&$1); }
    | KW_SHORTTIMEFORMAT  { token_free(&$1); }
    | KW_SLOPPY           { token_free(&$1); }
    | KW_SORTACCOUNTS     { token_free(&$1); }
    | KW_SORTJOURNALENTRIES { token_free(&$1); }
    | KW_SORTRESOURCES    { token_free(&$1); }
    | KW_SORTTASKS        { token_free(&$1); }
    | KW_START            { token_free(&$1); }
    | KW_STARTCREDIT      { token_free(&$1); }
    | KW_STATUS           { token_free(&$1); }
    | KW_STRICT           { token_free(&$1); }
    | KW_SUMMARY          { token_free(&$1); }
    | KW_TAGFILE          { token_free(&$1); }
    | KW_TASKATTRIBUTES   { token_free(&$1); }
    | KW_TASKPREFIX       { token_free(&$1); }
    | KW_TASKROOT         { token_free(&$1); }
    | KW_TEXT             { token_free(&$1); }
    | KW_TIMEFORMAT       { token_free(&$1); }
    | KW_TIMEFORMAT1      { token_free(&$1); }
    | KW_TIMEFORMAT2      { token_free(&$1); }
    | KW_TIMEOFF          { token_free(&$1); }
    | KW_TIMEZONE         { token_free(&$1); }
    | KW_TIMINGRESOLUTION { token_free(&$1); }
    | KW_TITLE            { token_free(&$1); }
    | KW_TOOLTIP          { token_free(&$1); }
    | KW_TRACKINGSCENARIO { token_free(&$1); }
    | KW_TREELEVEL        { token_free(&$1); }
    | KW_VACATION         { token_free(&$1); }
    | KW_WARN             { token_free(&$1); }
    | KW_WEEKLYMAX        { token_free(&$1); }
    | KW_WEEKLYMIN        { token_free(&$1); }
    | KW_WEEKSTARTSMONDAY { token_free(&$1); }
    | KW_WEEKSTARTSSUNDAY { token_free(&$1); }
    | KW_WIDTH            { token_free(&$1); }
    | KW_WORK             { token_free(&$1); }
    | KW_WORKINGHOURS     { token_free(&$1); }
    | KW_YEARLYWORKINGDAYS { token_free(&$1); }
    /* is* / has* / treelevel: expression-only functions                     */
    /* (already listed above under KW_IS* and KW_HASALERT/KW_TREELEVEL)     */
    ;

/* ── macro_body: tokens inside a macro definition ───────────────────────── *
 * A macro body is everything between [ and ].  We accept any token
 * except TK_RBRACKET (which terminates the macro).
 * Note: this means macro bodies cannot contain unbalanced ']'.             */
macro_body
    : /* empty */
    | macro_body macro_body_tok
    ;

macro_body_tok
    : TK_IDENT          { token_free(&$1); }
    | TK_INTEGER        { token_free(&$1); }
    | TK_FLOAT          { token_free(&$1); }
    | TK_STR            { token_free(&$1); }
    | TK_DATE           { token_free(&$1); }
    | TK_DURATION       { token_free(&$1); }
    | TK_LBRACE         { token_free(&$1); }
    | TK_RBRACE         { token_free(&$1); }
    | TK_LBRACKET       { token_free(&$1); }
    | TK_BANG           { token_free(&$1); }
    | TK_PLUS           { token_free(&$1); }
    | TK_MINUS          { token_free(&$1); }
    | TK_DOT            { token_free(&$1); }
    | TK_COLON          { token_free(&$1); }
    | TK_COMMA          { token_free(&$1); }
    | TK_PERCENT        { token_free(&$1); }
    | TK_DOLLAR         { token_free(&$1); }
    | TK_MULTI_LINE_STR { token_free(&$1); }
    | TK_ERROR          { token_free(&$1); }
    /* All KW_* tokens (a macro body can contain any keywords):             */
    | KW_PROJECT        { token_free(&$1); }
    | KW_TASK           { token_free(&$1); }
    | KW_RESOURCE       { token_free(&$1); }
    | KW_ACCOUNT        { token_free(&$1); }
    | KW_SHIFT          { token_free(&$1); }
    | KW_TASKREPORT     { token_free(&$1); }
    | KW_RESOURCEREPORT { token_free(&$1); }
    | KW_ACCOUNTREPORT  { token_free(&$1); }
    | KW_TEXTREPORT     { token_free(&$1); }
    | KW_TRACEREPORT    { token_free(&$1); }
    | KW_ICALREPORT     { token_free(&$1); }
    | KW_NIKUREPORT     { token_free(&$1); }
    | KW_EXPORT         { token_free(&$1); }
    | KW_STATUSSHEETREPORT { token_free(&$1); }
    | KW_TIMESHEETREPORT { token_free(&$1); }
    | KW_NAVIGATOR      { token_free(&$1); }
    | KW_TAGFILE        { token_free(&$1); }
    | KW_MACRO          { token_free(&$1); }
    | KW_INCLUDE        { token_free(&$1); }
    | KW_SUPPLEMENT     { token_free(&$1); }
    | KW_SCENARIO       { token_free(&$1); }
    | KW_EXTEND         { token_free(&$1); }
    | KW_TIMESHEET      { token_free(&$1); }
    | KW_STATUSSHEET    { token_free(&$1); }
    | KW_JOURNALENTRY   { token_free(&$1); }
    /* Plus all attribute keywords via gen_expr_tok re-use (or list them):  */
    | KW_ALLOCATE       { token_free(&$1); }
    | KW_DEPENDS        { token_free(&$1); }
    | KW_EFFORT         { token_free(&$1); }
    | KW_START          { token_free(&$1); }
    | KW_END            { token_free(&$1); }
    | KW_COMPLETE       { token_free(&$1); }
    | KW_NOTE           { token_free(&$1); }
    | KW_TOOLTIP        { token_free(&$1); }
    | KW_CELLCOLOR      { token_free(&$1); }
    | KW_HIDETASK       { token_free(&$1); }
    | KW_HIDERESOURCE   { token_free(&$1); }
    | KW_FORMATS        { token_free(&$1); }
    | KW_COLUMNS        { token_free(&$1); }
    | KW_LOADUNIT       { token_free(&$1); }
    | KW_HEADLINE       { token_free(&$1); }
    | KW_SORTTASKS      { token_free(&$1); }
    | KW_SORTRESOURCES  { token_free(&$1); }
    | KW_TITLE          { token_free(&$1); }
    /* TODO: list all remaining KW_* here, or factor into a shared rule.    */
    ;

/* ── opt_args: zero or more argument tokens (fallback rule only) ─────────── *
 *
 * Used ONLY in the TK_IDENT fallback alternative and the isXxx/hasXxx/unknown
 * keyword stubs in the item rule.  KW_* tokens are intentionally excluded
 * so that statement boundaries are clean: when a KW_* token appears while
 * consuming opt_args, bison will reduce and the outer items loop will start
 * a new item with that keyword.                                             */
opt_args
    : /* empty */
    | opt_args arg_token
    ;

arg_token
    : TK_IDENT     { token_free(&$1); }
    | TK_STR       { token_free(&$1); }
    | TK_INTEGER   { token_free(&$1); }
    | TK_FLOAT     { token_free(&$1); }
    | TK_DATE      { token_free(&$1); }
    | TK_DURATION  { token_free(&$1); }
    | TK_BANG      { token_free(&$1); }
    | TK_PLUS      { token_free(&$1); }
    | TK_MINUS     { token_free(&$1); }
    | TK_DOT       { token_free(&$1); }
    | TK_COLON     { token_free(&$1); }
    | TK_COMMA     { token_free(&$1); }
    | TK_PERCENT   { token_free(&$1); }
    | TK_DOLLAR    { token_free(&$1); }
    | TK_LBRACKET  { token_free(&$1); }
    | TK_RBRACKET  { token_free(&$1); }
    | TK_MULTI_LINE_STR { token_free(&$1); }
    | TK_ERROR     { token_free(&$1); }
    ;

/* ── opt_body: optional { items } block ─────────────────────────────────── *
 *
 * Returns a BodyResult with the collected child symbols and the end
 * position of the closing `}`.
 *
 * The second alternative uses bison's built-in `error` token for error
 * recovery when the closing brace is missing.                              */
opt_body
    : /* empty */
        { $$.syms = (SymArr){0}; $$.end = (LspPos){0}; }
    | TK_LBRACE body_items TK_RBRACE
        {
            $$.syms = $2.syms;
            $$.end  = $3.end;
            token_free(&$1);
            token_free(&$3);
        }
    | TK_LBRACE body_items error
        {
            $$.syms = $2.syms;
            $$.end  = (LspPos){0};
            push_diagnostic(g_result,
                            (LspRange){ $1.start, $1.end },
                            DIAG_ERROR, "unclosed `{`");
            token_free(&$1);
        }
    ;

/* ── body_items: items collected inside a { … } block ───────────────────── *
 *
 * Returns a BodyResult (reusing the <body> union slot; .end is always zero
 * here and gets filled in by the enclosing opt_body rule from the `}`).    */
body_items
    : /* empty */
        { $$.syms = (SymArr){0}; $$.end = (LspPos){0}; }
    | body_items item
        {
            $$ = $1;
            if ($2.has_sym)
                symarr_push(&$$.syms, $2.sym);
        }
    ;

%%

void yyerror(const char *msg) {
    /*
     * TODO: capture the position of the offending token (e.g. via a
     * `g_last_token` global updated by the lexer) and emit a proper
     * LSP diagnostic instead of printing to stderr.
     */
    (void)msg;
}
