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

/* Dynamic array of tj_node pointers, used for body children. */
typedef struct { tj_node **arr; int n, cap; } SymArr;

/* Return type for opt_body and body_items rules. */
typedef struct { SymArr syms; LspPos end; } BodyResult;

/* Return type for item rule: either a tj_node pointer or nothing. */
typedef struct { tj_node *sym; int has_sym; } ItemResult;

/* Return type for dep_path and task_ref rules. */
typedef struct {
    int    bang_count; /* number of leading ! tokens */
    char  *path;       /* dotted path, heap-allocated, e.g. "deliveries.start" */
    LspPos start;
    LspPos end;
} TaskRef;
}

%{
#include "parser.h"           /* Token, tj_node, ParseOutput, LspRange, etc. */
#include "grammar.tab.h"      /* TK_* / KW_* constants, YYSTYPE */
#include "document_symbol.h"  /* symbol_kind_for() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Globals defined in parser.c, shared with lexer.l */
extern ParseOutput *g_output;
/* The in-progress parse's token arena; dep-path strings are built here so they
 * share the tj_node tree's lifetime and need no per-path malloc/free. */
extern str_arena *g_tok_arena;

int  yylex(void);
void yyerror(const char *msg);

/* ── Helpers (declared/defined in parser.c) ─────────────────────────────── */

extern void push_include(ParseOutput *po, const char *quoted_text,
                         const prefix_set *prefixes);

/* ── Include-statement prefix capture ────────────────────────────────────
 *
 * Bumped while parsing the body of an `include` statement so that the
 * KW_TASKPREFIX / KW_RESOURCEPREFIX / KW_ACCOUNTPREFIX / KW_REPORTPREFIX
 * attribute actions know to stash their argument into the matching slot
 * of g_pending_prefixes.  When the enclosing include_stmt action fires,
 * it consumes the pending set and resets it for the next include.
 *
 * The depth counter handles only the direct body — nested include
 * statements are not legal inside an include body, so the counter
 * never legitimately exceeds 1.  We still use a counter rather than a
 * boolean so an accidentally-nested include doesn't corrupt the outer
 * include's pending state. */
static int        g_in_include_depth = 0;
static prefix_set g_pending_prefixes;

static void set_pending_prefix(prefix_kind kind, char *value) {
    if (g_in_include_depth > 0) {
        free(g_pending_prefixes.by_kind[kind]);
        g_pending_prefixes.by_kind[kind] = value;  /* take ownership */
    } else {
        free(value);       /* dropped: appearing outside an include body */
    }
}

/* Called from parser.c at the start of every parse() so a partial
 * include-body parse from a previous run cannot leak pending state. */
void reset_pending_include_state(void) {
    g_in_include_depth = 0;
    prefix_set_clear(&g_pending_prefixes);
}

/* ── Top-level declaration routing ──────────────────────────────────────── *
 *
 * Every top-level declaration that produces a tj_node — task, account,
 * resource/shift, the report family (navigator/scenario/timesheet/
 * statussheet/tagfile/journalentry included), and the project block —
 * is appended to g_output->root in source order.  The node's own
 * `keyword` records its kind, so no per-kind bucketing is needed; later
 * passes (server.c's per-Project rebuild) route by keyword when they
 * need the distinction.
 *
 * At most one project block is kept per file: a second one (malformed
 * input) is dropped rather than admitted as a top-level sibling.
 */
static int output_has_project(void) {
    if (!g_output || !g_output->root) return 0;
    for (int i = 0; i < g_output->root->num_children; i++)
        if (g_output->root->children[i]->keyword == KW_PROJECT)
            return 1;
    return 0;
}

/* Append @p node to the document root, or drop it when it cannot be
 * admitted (no output, or a duplicate project block).  Dropped nodes are
 * simply abandoned in the parse arena. */
static void route_top_level(tj_node *node) {
    if (!node) return;
    if (!g_output || !g_output->root ||
        (node->keyword == KW_PROJECT && output_has_project()))
        return;
    tj_node_append_child(g_output->root, node);
}

/* ── Current dep-ref direction ──────────────────────────────────────────── *
 * Set by mid-rule actions on the KW_DEPENDS / KW_PRECEDES attribute
 * branches before dep_ref_list reduces, and consumed by the dep_ref
 * action to label each captured Dependency.  Safe as a file-scope global
 * because dep_ref_list never nests (it appears only as a leaf inside
 * the depends/precedes attribute productions). */
static DepKind g_pending_dep_kind = DEP_KIND_DEPENDS;

/* ── Current-symbol stack ───────────────────────────────────────────────── *
 * Tracks the current task being parsed so that the KW_START / KW_END
 * date-attribute actions can store the parsed date on the right node.
 * Pushed when entering a task body, popped on exit.                        */
static tj_node *g_sym_stack[128];
static int      g_sym_stack_n = 0;

static void sym_stack_push(tj_node *s) {
    if (g_sym_stack_n < 128)
        g_sym_stack[g_sym_stack_n++] = s;
}

static void sym_stack_pop(void) {
    if (g_sym_stack_n > 0)
        g_sym_stack_n--;
}

static tj_node *sym_stack_top(void) {
    return g_sym_stack_n > 0 ? g_sym_stack[g_sym_stack_n - 1] : NULL;
}

/* ── tj_node array helper ──────────────────────────────────────────────── */

/* Body child arrays live in the parse's token arena like the nodes they
 * hold: growth allocates a doubled arena copy and abandons the old block,
 * so a discarded body needs no cleanup at all. */
static void symarr_push(SymArr *a, tj_node *s) {
    if (a->n >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 4;
        tj_node **grown = arena_alloc(g_tok_arena,
                                      (size_t)a->cap * sizeof(tj_node *));
        memcpy(grown, a->arr, (size_t)a->n * sizeof(tj_node *));
        a->arr = grown;
    }
    a->arr[a->n++] = s;
}

/* ── Build a tj_node from the components of a symbol_decl rule ───────────── */

/* Allocate a tj_node and populate it with header fields (kind, id, name,
 * selection_range).  Body fields (range.end, children) are filled in
 * later by finish_tj_node().                                              */
static tj_node *alloc_tj_node(Token kw, Token id, Token name) {
    tj_node *s = tj_node_new();   /* arena-owned, like the whole tree */
    s->keyword = kw.kind;

    /* id / name borrow the token lexeme straight from the parse's token arena
     * (emit_* interned it there).  The arena travels with this tj_node tree
     * into the doc_snapshot and is freed in bulk only after the tree, so these
     * pointers stay valid for the node's whole life and are never freed
     * individually.  The default name reuses the id pointer. */
    if (id.text) {
        s->id              = id.text;
        s->selection_range = (LspRange){ id.start, id.end };
    } else {
        s->id              = kw.text;
        s->selection_range = (LspRange){ kw.start, kw.end };
    }

    s->name = name.text ? name.text : s->id;

    /* Range start is always the keyword; end is filled after body parse. */
    s->range.start = kw.start;

    return s;
}

/* Finalize a tj_node after its body has been parsed. */
static void finish_tj_node(tj_node *s, Token kw, BodyResult body) {
    LspPos range_end = body.end;
    if (range_end.line == 0 && range_end.character == 0)
        range_end = kw.end;
    s->range.end = range_end;

    /* Adopt the body's arena-backed children array, wiring up parents. */
    s->children     = body.syms.arr;
    s->num_children = body.syms.n;
    s->children_cap = body.syms.cap;
    for (int i = 0; i < s->num_children; i++)
        s->children[i]->parent_node = s;
}

%}

/* ── Value union ─────────────────────────────────────────────────────────── */

%union {
    Token      tok;   /* single token (kind / start / end / text) */
    tj_node   *sym;   /* heap-allocated tj_node */
    BodyResult body;  /* body: children + closing-brace position */
    ItemResult item;  /* item: optional symbol */
    TaskRef    tref;  /* dep path + bang count */
    int        ival;  /* integer (bang count) */
    char      *text;  /* heap-allocated string (e.g. joined dotted id) */
}

/* ── Discarded-symbol destructors ────────────────────────────────────────── *
 *
 * On a syntax error bison pops symbols off its stack during error
 * recovery; a destructor reclaims any heap-allocated value still on the
 * stack.  Almost nothing here needs one: tj_nodes, their arrays, and
 * every token lexeme live in the parse's token arena and are reclaimed in
 * bulk with it, so a discarded <sym> / <item> / <body> / <tok> value is
 * simply abandoned.  The single exception is <text> (prefix_path_id),
 * whose joined dotted string is heap-allocated for the include capture. */
%destructor { free($$); }                               <text>

/* ── Remaining conflicts ─────────────────────────────────────────────────── *
 *
 * Conflict history (all resolved by grammar structure, not %expect/precedence):
 *   ~5200  open-ended `gen_expr` guessing where an expression statement ended
 *          → removed by the synthetic TK_EOL terminator (see lexer.l).
 *      7   nullable `opt_id opt_name` declaration headers
 *          → removed by requiring the name (matching TaskJuggler, whose headers
 *            are all `_keyword !optionalID $STRING`).
 *      1   `shift` declaration vs `shift` attribute
 *          → removed by lifting KW_SHIFT out of sym_kw (see symbol_decl).
 *    204   reduce/reduce in `opt_body` error recovery
 *          → removed by synthesising missing `}` at EOF (see lexer.l).
 *
 * TWO shift/reduce conflicts remain (no reduce/reduce), both in the unknown-
 * identifier fallback `plain_stmt: TK_IDENT opt_args opt_body`: at
 * `TK_IDENT opt_args •` the parser may either extend opt_args with another
 * argument token or reduce the statement.  bison shifts (greedy), which is the
 * intended behaviour — the fallback absorbs its trailing arguments.  These are
 * deliberately left as plain warnings: terminating the rule with TK_EOL would
 * resolve them but reject same-line attributes such as `Phone "x100" rate 350`,
 * which is valid under TaskJuggler's terminator-free grammar.
 *
 * TODO(grammar): context-sensitive refactor (deferred — large, only do this if
 * we want "attribute not valid in this scope" diagnostics as a feature).
 *
 *   Why: TaskJuggler's parser is context-sensitive — each property body has its
 *   own attribute set (taskAttributes, resourceAttributes, projectBodyAttributes
 *   …), and keywords are matched per scope.  Ours is intentionally context-free:
 *   one universal `item` rule is allowed in every body, with semantic scope
 *   checked downstream (see the `item` rule header).  That single decision is
 *   the root of the last two conflicts AND of the keyword-as-id workarounds
 *   (e.g. `opt_id: … | KW_START`, because the flex lexer emits KW_START where
 *   TaskJuggler's scanner would emit a plain ID matched contextually).
 *
 *   What it entails:
 *     - Replace the single `opt_body` / `body_items` with per-scope body rules
 *       (project_body, task_body, resource_body, account_body, shift_body,
 *       report_body, column_body, …), each enumerating exactly the attributes
 *       and nested declarations valid there, mirroring TaskJuggler's *Attributes
 *       rules in lib/taskjuggler/TjpSyntaxRules.rb.
 *     - Split the universal `item` into scope-specific item sets; the unknown-
 *       identifier fallback becomes a per-scope extension point (or is dropped
 *       where an exhaustive attribute set exists), eliminating the 2 conflicts.
 *     - Optionally make the scanner context-aware (keywords as IDs) to retire
 *       the KW_* opt_id / dep_path_seg workarounds.
 *
 *   Benefits: resolves the last 2 conflicts; enables precise "X is not a valid
 *     attribute of a Y" diagnostics; closer fidelity to TaskJuggler.
 *   Costs: large (dozens of body/attribute rules + actions), reverses the
 *     current leniency (would reject attributes used in the "wrong" body, which
 *     today the LSP tolerates and still extracts symbols from), and churns most
 *     golden snapshots.  Treat as a feature project, not conflict cleanup. */

/* ── Token declarations ──────────────────────────────────────────────────── */

/*
 * LSP-documented keywords — MUST be declared first, in one of the two
 * sub-ranges below.  scan_kw_stack() uses the token kind as a fast filter
 * with no string comparisons:
 *
 *   kind < KW_SIG_END   → keyword has signature help (and hover docs)
 *   kind < KW_DOCS_END  → keyword has hover docs only
 *
 * Bison assigns token values in declaration order, so placement here
 * determines which range a keyword falls into.
 *
 * When adding a new keyword:
 *   - With signature help AND hover docs: declare before KW_SIG_END;
 *     add to keyword_docs() in hover.c and build_signature_help_json()
 *     in signature.c.
 *   - With hover docs only: declare between KW_SIG_END and KW_DOCS_END;
 *     add to keyword_docs() in hover.c.
 *   - Neither: declare after KW_DOCS_END (no further action needed).
 * Forgetting this placement means hover/signature will silently return
 * nothing for the keyword.
 */
/* Declarations */
%token <tok> KW_PROJECT KW_TASK KW_RESOURCE KW_ACCOUNT KW_SHIFT
%token <tok> KW_MACRO KW_INCLUDE KW_FLAGS KW_SUPPLEMENT
/* Task attributes */
%token <tok> KW_EFFORT KW_DURATION KW_LENGTH KW_MILESTONE
%token <tok> KW_DEPENDS KW_PRECEDES KW_ALLOCATE
%token <tok> KW_START KW_END KW_MAXSTART KW_MINSTART KW_MAXEND KW_MINEND
%token <tok> KW_PRIORITY KW_COMPLETE KW_NOTE KW_RESPONSIBLE
%token <tok> KW_BOOKING KW_SCHEDULED
/* Resource attributes */
%token <tok> KW_RATE KW_EFFICIENCY KW_VACATION KW_LEAVES
/* Project attributes */
%token <tok> KW_NOW KW_CURRENCY KW_TIMEFORMAT KW_TIMEZONE
%token <tok> KW_WORKINGHOURS KW_TIMINGRESOLUTION KW_SCENARIO

/*
 * KW_SIG_END — sentinel marking the end of the signature-help keyword range.
 *
 * Never returned by the lexer; never appears in grammar rules.
 * scan_kw_stack() checks tok->token_kind < KW_SIG_END to decide whether
 * a token should be pushed when scanning for signature-help context.
 */
%token KW_SIG_END

/*
 * KW_DOCS_END — sentinel marking the end of all LSP-documented keywords.
 *
 * Never returned by the lexer; never appears in grammar rules.
 * scan_kw_stack() checks tok->token_kind < KW_DOCS_END to decide whether
 * a token should be pushed when scanning for hover-docs context.
 * Tokens with kind >= KW_DOCS_END (~85% of all tokens) are skipped with
 * a single integer comparison — no string lookup needed.
 */
%token KW_DOCS_END

/* All remaining keywords — no hover docs or signature help (yet). */
%token <tok> KW_ACCOUNTPREFIX KW_ACCOUNTREPORT KW_ACCOUNTROOT KW_ACTIVE
%token <tok> KW_ADOPT KW_AGGREGATE KW_ALERT KW_ALERTLEVELS
%token <tok> KW_ALTERNATIVE KW_AUTHOR KW_AUXDIR KW_BALANCE
%token <tok> KW_CAPTION KW_CELLCOLOR KW_CELLTEXT KW_CENTER KW_CHARGE
%token <tok> KW_CHARGESET KW_COLUMNS KW_COPYRIGHT
%token <tok> KW_CREDITS KW_CURRENCYFORMAT
%token <tok> KW_DAILYMAX KW_DAILYMIN KW_DAILYWORKINGHOURS
%token <tok> KW_DATE KW_DEFINITIONS KW_DETAILS KW_DISABLED
%token <tok> KW_EFFORTDONE KW_EFFORTLEFT
%token <tok> KW_EMAIL KW_ENABLED KW_ENDCREDIT KW_EPILOG
%token <tok> KW_EXPORT KW_EXTEND KW_FAIL KW_FONTCOLOR
%token <tok> KW_FOOTER KW_FORMATS KW_GAPDURATION KW_GAPLENGTH
%token <tok> KW_HALIGN KW_HASALERT KW_HEADER KW_HEADLINE KW_HEIGHT
%token <tok> KW_HIDEACCOUNT KW_HIDEJOURNALENTRY KW_HIDEREPORT
%token <tok> KW_HIDERESOURCE KW_HIDETASK KW_ICALREPORT
%token <tok> KW_INHERIT
%token <tok> KW_ISACTIVE KW_ISCHILDOF KW_ISDEPENDENCYOF KW_ISDUTYOF
%token <tok> KW_ISFEATUREOF KW_ISLEAF KW_ISMILESTONE KW_ISONGOING
%token <tok> KW_ISRESOURCE KW_ISRESPONSIBILITYOF KW_ISTASK KW_ISVALID
%token <tok> KW_JOURNALATTRIBUTES KW_JOURNALENTRY KW_JOURNALMODE
%token <tok> KW_LEAVEALLOWANCES KW_LEFT
%token <tok> KW_LIMITS KW_LISTITEM KW_LISTTYPE KW_LOADUNIT
%token <tok> KW_MANAGERS KW_MANDATORY KW_MARKDATE
%token <tok> KW_MAXIMUM KW_MINIMUM
%token <tok> KW_MONTHLYMAX KW_MONTHLYMIN
%token <tok> KW_NAVIGATOR KW_NEWTASK KW_NIKUREPORT
%token <tok> KW_NOVEVENTS KW_NUMBER KW_NUMBERFORMAT
%token <tok> KW_ONEND KW_ONSTART KW_OPENNODES KW_OUTPUTDIR KW_OVERTIME
%token <tok> KW_PERIOD KW_PERSISTENT
%token <tok> KW_PROJECTID KW_PROJECTIDS KW_PROJECTION KW_PROLOG KW_PURGE
%token <tok> KW_RAWHTMLHEAD KW_REFERENCE KW_REMAINING KW_REPLACE
%token <tok> KW_REPORTPREFIX KW_RESOURCEATTRIBUTES KW_RESOURCEPREFIX
%token <tok> KW_RESOURCEREPORT KW_RESOURCEROOT KW_RESOURCES
%token <tok> KW_RICHTEXT KW_RIGHT
%token <tok> KW_ROLLUPACCOUNT KW_ROLLUPRESOURCE KW_ROLLUPTASK
%token <tok> KW_SCALE KW_SCENARIOS KW_SCENARIOSPECIFIC
%token <tok> KW_SCHEDULING KW_SCHEDULINGMODE
%token <tok> KW_SELECT KW_SELFCONTAINED KW_SHIFTS KW_SHORTTIMEFORMAT
%token <tok> KW_SLOPPY KW_SORTACCOUNTS KW_SORTJOURNALENTRIES
%token <tok> KW_SORTRESOURCES KW_SORTTASKS
%token <tok> KW_STARTCREDIT KW_STATUS KW_STATUSSHEET
%token <tok> KW_STATUSSHEETREPORT KW_STRICT KW_SUMMARY
%token <tok> KW_TAGFILE KW_TASKATTRIBUTES KW_TASKPREFIX KW_TASKREPORT
%token <tok> KW_TASKROOT KW_TEXT KW_TEXTREPORT
%token <tok> KW_TIMEFORMAT1 KW_TIMEFORMAT2 KW_TIMEOFF
%token <tok> KW_TIMESHEET KW_TIMESHEETREPORT
%token <tok> KW_TITLE KW_TOOLTIP KW_TRACEREPORT KW_TRACKINGSCENARIO
%token <tok> KW_TREELEVEL KW_WARN
%token <tok> KW_WEEKLYMAX KW_WEEKLYMIN KW_WEEKSTARTSMONDAY KW_WEEKSTARTSSUNDAY
%token <tok> KW_WIDTH KW_WORK KW_YEARLYWORKINGDAYS

%token <tok> TK_IDENT
%token <tok> TK_STR TK_INTEGER TK_FLOAT TK_DATE TK_DURATION
%token <tok> TK_LBRACE TK_RBRACE
%token <tok> TK_LBRACKET TK_RBRACKET
%token <tok> TK_BANG TK_PLUS TK_MINUS TK_DOT TK_COLON TK_COMMA TK_PERCENT TK_DOLLAR
%token <tok> TK_MULTI_LINE_STR
%token <tok> TK_ERROR
%token       TK_LINE_COMMENT TK_BLOCK_COMMENT  /* stored in token array only; never returned to parser */

/*
 * TK_EOL — synthetic statement terminator inserted by the yylex wrapper in
 * lexer.l (never produced by a flex rule directly).  It appears only at a
 * genuine statement boundary: it terminates the open-ended `gen_expr` rules
 * (removing the shift/reduce ambiguity over where an expression statement
 * ends) and is otherwise absorbed between statements by `items` / `body_items`
 * and inside macro bodies.  It carries no semantic value.
 */
%token       TK_EOL

/* ── Non-terminal types ──────────────────────────────────────────────────── */

%type <sym>  symbol_decl report_decl navigator_decl scenario_decl
%type <sym>  timesheet_decl statussheet_decl tagfile_decl journalentry_decl
%type <tok>  sym_kw report_kw opt_id opt_name opt_version dep_path_seg
%type <tok>  string_val
%type <body> opt_body body_items
%type <tref> dep_path task_ref
%type <ival> bang_seq
%type <text> prefix_path_id

%type <item> item

/* ── Grammar ─────────────────────────────────────────────────────────────── */

%%

/* ── Top-level file ──────────────────────────────────────────────────────── */

file
    : { g_sym_stack_n = 0; } items
    ;

items
    : /* empty */
    | items item
        {
            if ($2.has_sym)
                route_top_level($2.sym);
        }
    | items TK_EOL   /* absorb the terminator that follows a non-gen_expr item */
    ;

/* ── Universal item rule ─────────────────────────────────────────────────── *
 *
 * The grammar is context-free: all statement types are allowed anywhere, even
 * if some (e.g. 'scenario' inside project body) are semantically restricted.
 * Semantic context is validated by downstream passes.
 *
 * An item is either a tj_node-introducing declaration or a plain_stmt (an
 * attribute or structural statement that produces no symbol).
 *
 * NOTE: gen_expr statements (hidetask, cellcolor, sorttasks, etc.) use a
 * greedy expression rule that consumes all non-brace tokens including most
 * KW_* tokens.  The declaration keywords listed in sym_kw and report_kw are
 * excluded from gen_expr to preserve correct symbol extraction.  All other
 * KW_* keywords included in gen_expr may be accidentally consumed as part
 * of the preceding expression if they appear on the very next line.
 * ────────────────────────────────────────────────────────────────────────── */
item
    : symbol_decl       { $$.sym = $1; $$.has_sym = 1; }
    | report_decl       { $$.sym = $1; $$.has_sym = 1; }
    | navigator_decl    { $$.sym = $1; $$.has_sym = 1; }
    | scenario_decl     { $$.sym = $1; $$.has_sym = 1; }
    | timesheet_decl    { $$.sym = $1; $$.has_sym = 1; }
    | statussheet_decl  { $$.sym = $1; $$.has_sym = 1; }
    | tagfile_decl      { $$.sym = $1; $$.has_sym = 1; }
    | journalentry_decl { $$.sym = $1; $$.has_sym = 1; }
    | plain_stmt        { $$.has_sym = 0; }
    | error             { $$.has_sym = 0; }
    ;

/* ── plain_stmt: every statement that produces no tj_node ────────────────── *
 *
 * Attribute statements are grouped by shape: all keywords sharing an
 * argument pattern are collected into one *_kw class rule below and share
 * a single alternative here.  Only statements with real semantic actions
 * (date capture, prefix capture, dependency capture) keep their own
 * alternative.                                                              */
plain_stmt
    /* ── Structural statements ── */
    : extend_stmt
    | supplement_stmt
    | macro_stmt
    | include_stmt

    /* ── Date attributes ── */
    /* Syntax: start <date> — captured onto the enclosing task              */
    | KW_START TK_DATE
        {
            tj_node *task = sym_stack_top();
            if (task && task->keyword == KW_TASK) {
                time_t parsed;
                if (parse_tjp_date($2.text, &parsed)) {
                    task->start_date = parsed;
                    task->has_start  = 1;
                }
            }
        }
    /* Syntax: end <date> — captured onto the enclosing task                */
    | KW_END TK_DATE
        {
            tj_node *task = sym_stack_top();
            if (task && task->keyword == KW_TASK) {
                time_t parsed;
                if (parse_tjp_date($2.text, &parsed)) {
                    task->end_date = parsed;
                    task->has_end  = 1;
                }
            }
        }
    /* Syntax: (now | maxend | minend | maxstart | minstart | markdate) <date> */
    | date_kw TK_DATE

    /* ── Duration / numeric attributes ── */
    /* Syntax: (effort | duration | length | …) <value> (min|h|d|w|m|y)     */
    | durval_kw dur_val
    /* Syntax: work <value> (% | min | h | d)                               */
    | KW_WORK num_val dur_unit
    /* Syntax: (complete | rate | efficiency | …) (<INTEGER> | <FLOAT>)     */
    | numval_kw num_val
    /* Syntax: (priority | height | width) <INTEGER>                        */
    | int_kw TK_INTEGER
    /* Syntax: timingresolution <INTEGER> min                               */
    | KW_TIMINGRESOLUTION TK_INTEGER TK_IDENT
    /* Syntax: (dailymax | weeklymin | maximum | …) <value> <unit> [{ … }]
     * The optional body accepts: resources <id> [, <id>...] and
     * scenario-specific attributes.                                        */
    | durlimit_kw dur_val opt_body

    /* ── String attributes ── */
    /* Syntax: (note | email | title | header | …) <STRING>                 */
    | string_kw string_val
    /* Syntax: timezone <zone>  (a string or identifier)                    */
    | KW_TIMEZONE string_val
    | KW_TIMEZONE TK_IDENT

    /* ── ID/identifier attributes ── */
    /* Syntax: (projectid | author | scheduling | loadunit | …) <ID>        */
    | ident_kw TK_IDENT
    /* Syntax: taskprefix <task ID> — captured for the enclosing include    */
    | KW_TASKPREFIX prefix_path_id
        { set_pending_prefix(PREFIX_TASK, $2); }
    /* Syntax: resourceprefix <resource ID>                                 */
    | KW_RESOURCEPREFIX prefix_path_id
        { set_pending_prefix(PREFIX_RESOURCE, $2); }
    /* Syntax: accountprefix <account ID>                                   */
    | KW_ACCOUNTPREFIX prefix_path_id
        { set_pending_prefix(PREFIX_ACCOUNT, $2); }
    /* Syntax: reportprefix <report ID>                                     */
    | KW_REPORTPREFIX prefix_path_id
        { set_pending_prefix(PREFIX_REPORT, $2); }
    /* Syntax: taskroot (<ABSOLUTE_ID> | <ID>)                              */
    | KW_TASKROOT dotted_id
    /* Syntax: balance (<cost account> <ID> | -)                            */
    | KW_BALANCE TK_IDENT TK_IDENT
    | KW_BALANCE TK_MINUS

    /* ── ID-list attributes ── */
    /* Syntax: (responsible | flags | scenarios | …) <ID> [, <ID>...]       */
    | idlist_kw id_list
    /* Syntax: adopt (<ABSOLUTE_ID> | <ID>) [, ...]                         */
    | KW_ADOPT dotted_id_list

    /* ── No-argument keyword statements ── */
    /* Syntax: milestone | scheduled | mandatory | strict | …               */
    | noarg_kw

    /* ── Interval attributes ── */
    /* Syntax: period <interval1>  (date - date | date + N unit)            */
    | KW_PERIOD interval2

    /* ── Dependency and allocation statements ── */
    /* Syntax: depends (<ABSOLUTE ID> | <ID> | <RELATIVE ID>) [{ <attrs> }]
     *                 [, ... ]
     * The optional body accepts: gaplength, gapduration                    */
    | KW_DEPENDS { g_pending_dep_kind = DEP_KIND_DEPENDS; } dep_ref_list
    /* Syntax: precedes (<ABSOLUTE ID> | <ID> | <RELATIVE ID>) [{ <attrs> }]
     *                  [, ... ]                                            */
    | KW_PRECEDES { g_pending_dep_kind = DEP_KIND_PRECEDES; } dep_ref_list
    /* Syntax: allocate <resource> [{ <attributes> }] [, <resource> ...]    */
    | KW_ALLOCATE alloc_ref_list
    /* Syntax: booking <resource> <interval4> [, <interval4>...] [{ <attrs> }] */
    | KW_BOOKING TK_IDENT booking_interval_list opt_body
    /* Syntax: shift <shift> [<interval2>] [, <shift> [<interval2>] ...]
     * (attribute form inside resource/task; differs from the shift declaration) */
    | KW_SHIFT shift_attr_list
    /* Syntax: shifts <shift> [<interval2>] [, ...] (explicit plural form)  */
    | KW_SHIFTS shift_attr_list
    /* Syntax: limits [{ <attributes> }]                                    */
    | KW_LIMITS opt_body
    /* Syntax: projection [{ <attributes> }]                                */
    | KW_PROJECTION opt_body

    /* ── Account/charge attributes ── */
    /* Syntax: chargeset <account> <share%> [, <account> <share%> ...]      */
    | KW_CHARGESET chargeset_list
    /* Syntax: charge <amount> (onstart | onend | perhour | perday | perweek) */
    | KW_CHARGE num_val TK_IDENT
    /* Syntax: credits <date> <description> <amount> [, ...]                */
    | KW_CREDITS credits_list

    /* ── Free-form expression statements ── *
     * Logical expressions (hidetask, fail, warn, cellcolor, sorttasks, …)
     * and complex list arguments (leaves, workinghours, alertlevels, …).
     * gen_expr accepts any sequence of non-brace tokens; the synthetic
     * TK_EOL terminates the statement.  See NOTE in the item rule about
     * greedy consumption.                                                   */
    | genexpr_kw gen_expr TK_EOL

    /* ── Report layout ── */
    /* Syntax: columns <columnid> [{ <attributes> }] [, <columnid> ...]     */
    | KW_COLUMNS column_list
    /* Syntax: formats (csv | html | niku) [, ...]                          */
    | KW_FORMATS formats_list

    /* ── <keyword> <id> <string> [{ … }] statements ── *
     * status/newtask (timesheet bodies) and the extend-field declarations
     * (date/number/reference/richtext/text) all share this shape.          */
    | ident_string_body_kw TK_IDENT string_val opt_body

    /* ── Format specifiers ── */
    /* Syntax: (numberformat | currencyformat)
     *         <negpfx> <negsfx> <thousandsep> <decimsep> <fracdigits>      */
    | format_kw string_val string_val string_val string_val num_val

    /* ── Expression-only / unverified keywords ── *
     * The isXxx/hasXxx keywords and treelevel appear only inside logical
     * expressions and are normally consumed by gen_expr; these stubs keep
     * them parseable standalone during error recovery.  inherit and
     * scenariospecific have unverified standalone syntax (TODO: tj3man).   */
    | stub_kw opt_args TK_EOL

    /* ── Macro invocation: ${macroname [args...]} ───────────────────────── *
     * Without this rule, TK_DOLLAR would cause an error, and the TK_RBRACE
     * from the invocation would prematurely close the enclosing body.      */
    | TK_DOLLAR TK_LBRACE opt_args TK_RBRACE

    /* ── Fallback: unrecognised TK_IDENT statement ──
     *
     * Handles unknown identifiers and scenario-specific syntax like
     * "delayed:effort 40d" (which the lexer returns as a single TK_IDENT
     * due to ':' being allowed in identifier characters).
     * Also handles any future keywords not yet in the KW_* token set.      */
    | TK_IDENT opt_args opt_body
    ;

/* ── Attribute keyword classes ───────────────────────────────────────────── *
 *
 * Each class collects the keywords sharing one argument shape; plain_stmt
 * has a single alternative per class.  A keyword belongs to exactly one
 * class (or to a dedicated plain_stmt alternative when it carries a
 * semantic action).                                                         */

/* <kw> <date> */
date_kw
    : KW_NOW | KW_MAXEND | KW_MINEND | KW_MAXSTART | KW_MINSTART | KW_MARKDATE
    ;

/* <kw> <value> <unit> */
durval_kw
    : KW_EFFORT | KW_DURATION | KW_LENGTH | KW_EFFORTDONE | KW_EFFORTLEFT
    | KW_REMAINING | KW_GAPLENGTH | KW_GAPDURATION
    ;

/* <kw> <number> */
numval_kw
    : KW_COMPLETE | KW_OVERTIME | KW_SLOPPY | KW_DAILYWORKINGHOURS
    | KW_YEARLYWORKINGDAYS | KW_EFFICIENCY | KW_RATE | KW_STARTCREDIT
    | KW_ENDCREDIT
    ;

/* <kw> <integer> */
int_kw
    : KW_PRIORITY | KW_HEIGHT | KW_WIDTH
    ;

/* <kw> <value> <unit> [{ … }] — working-time limit attributes */
durlimit_kw
    : KW_DAILYMAX | KW_DAILYMIN | KW_WEEKLYMAX | KW_WEEKLYMIN
    | KW_MONTHLYMAX | KW_MONTHLYMIN | KW_MAXIMUM | KW_MINIMUM
    ;

/* <kw> <string> */
string_kw
    : KW_NOTE | KW_EMAIL | KW_HEADLINE | KW_TITLE | KW_CAPTION | KW_EPILOG
    | KW_PROLOG | KW_LEFT | KW_RIGHT | KW_CENTER | KW_FOOTER | KW_HEADER
    | KW_RAWHTMLHEAD | KW_LISTITEM | KW_CURRENCY | KW_TIMEFORMAT
    | KW_SHORTTIMEFORMAT | KW_TIMEFORMAT1 | KW_TIMEFORMAT2 | KW_OUTPUTDIR
    | KW_AUXDIR | KW_COPYRIGHT | KW_DETAILS | KW_SUMMARY
    ;

/* <kw> <id> — single identifier argument (IDs and enum values alike) */
ident_kw
    : KW_PROJECTID | KW_TRACKINGSCENARIO | KW_ALERT | KW_AUTHOR
    | KW_RESOURCEROOT | KW_ACCOUNTROOT | KW_PURGE
    | KW_SCHEDULING | KW_SCHEDULINGMODE | KW_AGGREGATE | KW_SELFCONTAINED
    | KW_ACTIVE | KW_JOURNALMODE | KW_LOADUNIT | KW_SELECT | KW_SCALE
    | KW_LISTTYPE
    ;

/* <kw> <id> [, <id>...] */
idlist_kw
    : KW_RESPONSIBLE | KW_MANAGERS | KW_SCENARIOS | KW_FLAGS
    | KW_ALTERNATIVE | KW_PROJECTIDS | KW_RESOURCES
    ;

/* <kw> — no arguments */
noarg_kw
    : KW_MILESTONE | KW_SCHEDULED | KW_MANDATORY | KW_PERSISTENT
    | KW_WEEKSTARTSMONDAY | KW_WEEKSTARTSSUNDAY | KW_DISABLED | KW_ENABLED
    | KW_REPLACE | KW_NOVEVENTS | KW_ONSTART | KW_ONEND | KW_STRICT
    | KW_TIMEOFF
    ;

/* <kw> <gen_expr> — free-form logical-expression / list statements */
genexpr_kw
    : KW_HIDETASK | KW_HIDERESOURCE | KW_HIDEACCOUNT | KW_HIDEREPORT
    | KW_HIDEJOURNALENTRY | KW_ROLLUPTASK | KW_ROLLUPRESOURCE
    | KW_ROLLUPACCOUNT | KW_WARN | KW_FAIL | KW_CELLCOLOR | KW_CELLTEXT
    | KW_TOOLTIP | KW_FONTCOLOR | KW_HALIGN | KW_SORTTASKS | KW_SORTRESOURCES
    | KW_SORTACCOUNTS | KW_SORTJOURNALENTRIES | KW_JOURNALATTRIBUTES
    | KW_TASKATTRIBUTES | KW_RESOURCEATTRIBUTES | KW_DEFINITIONS
    | KW_OPENNODES | KW_LEAVEALLOWANCES | KW_LEAVES | KW_VACATION
    | KW_WORKINGHOURS | KW_ALERTLEVELS
    ;

/* <kw> <id> <string> [{ … }] */
ident_string_body_kw
    : KW_STATUS | KW_NEWTASK
    | KW_DATE | KW_NUMBER | KW_REFERENCE | KW_RICHTEXT | KW_TEXT
    ;

/* <kw> <string> ×4 <number> */
format_kw
    : KW_NUMBERFORMAT | KW_CURRENCYFORMAT
    ;

/* <kw> [args] — error-recovery / unverified-syntax stubs */
stub_kw
    : KW_ISACTIVE | KW_ISCHILDOF | KW_ISDEPENDENCYOF | KW_ISDUTYOF
    | KW_ISFEATUREOF | KW_ISLEAF | KW_ISMILESTONE | KW_ISONGOING
    | KW_ISRESOURCE | KW_ISRESPONSIBILITYOF | KW_ISTASK | KW_ISVALID
    | KW_HASALERT | KW_TREELEVEL | KW_INHERIT | KW_SCENARIOSPECIFIC
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
 * The id is optional, but the name is required — matching TaskJuggler, whose
 * declaration headers are all `_keyword !optionalID $STRING`.  (Making the name
 * mandatory is also what lets `opt_id` reduce unambiguously: a leading TK_IDENT
 * is the id, a leading string is the name, so there is no nullable-nullable
 * guess.)                                                                   */
symbol_decl
    : KW_PROJECT opt_id string_val opt_version interval2 opt_body
        {
            /* The node is allocated here, in the final action, rather than
             * in a mid-rule action: nothing between the header and the body
             * needs it, and a mid-rule allocation would be orphaned (and
             * leaked, since bison runs no destructor for mid-rule values)
             * if `interval2` failed to parse. */
            $$ = alloc_tj_node($1, $2, $3);
            /* A project block is metadata only — its body's task /
             * resource / account / report children are conceptually
             * top-level declarations of the file, so we hoist each body
             * child to g_output->root as a sibling of the project node
             * (the same destination as items declared outside the
             * project block).  The project tj_node itself ends up with
             * no children; document_symbol rendering composes the
             * outline shape on demand. */
            LspPos range_end = $6.end;
            if (range_end.line == 0 && range_end.character == 0)
                range_end = $1.end;
            $$->range.end = range_end;
            for (int i = 0; i < $6.syms.n; i++) {
                tj_node *child = $6.syms.arr[i];
                if (child->keyword == KW_PROJECT) {
                    /* Nested `project` inside a project block is malformed;
                     * drop it (abandoned in the arena) rather than corrupt
                     * the tree. */
                    continue;
                }
                route_top_level(child);
            }
            /* TODO: store interval $5 as the project time range */
        }
    | sym_kw opt_id string_val
      { $<sym>$ = alloc_tj_node($1, $2, $3);
        if ($1.kind == KW_TASK) sym_stack_push($<sym>$); }
      opt_body
        {
            if ($1.kind == KW_TASK) sym_stack_pop();
            $$ = $<sym>4;
            finish_tj_node($$, $1, $5);
        }
    /* ── shift declaration: `shift [id] <name> [{ … }]` ──
     *
     * KW_SHIFT is deliberately kept OUT of sym_kw and given its own
     * alternative.  Were it in sym_kw it would reduce to sym_kw the moment the
     * keyword is seen, which collides with the `shift` *attribute*
     * (`plain_stmt: KW_SHIFT shift_attr_list`, e.g. `shift early [interval], …`)
     * and produces a shift/reduce conflict.  As a distinct alternative the
     * decision defers one token: after `shift <id>` a following name string
     * selects this declaration, while an interval / comma / `}` / end-of-line
     * selects the attribute.  TaskJuggler distinguishes the two by scope; we
     * stay context-free and let the trailing token decide.                  */
    | KW_SHIFT opt_id string_val opt_body
        {
            $$ = alloc_tj_node($1, $2, $3);
            finish_tj_node($$, $1, $4);
        }
    ;

sym_kw
    : KW_TASK
    | KW_RESOURCE
    | KW_ACCOUNT
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
    : report_kw opt_id string_val opt_body
        {
            $$ = alloc_tj_node($1, $2, $3);
            finish_tj_node($$, $1, $4);
        }
    | KW_ICALREPORT string_val opt_name opt_body
        {
            Token no_id = {0};
            $$ = alloc_tj_node($1, no_id, $2);
            finish_tj_node($$, $1, $4);
            /* $3: second string (unused as display name) */
        }
    | KW_NIKUREPORT string_val opt_name opt_body
        {
            Token no_id = {0};
            $$ = alloc_tj_node($1, no_id, $2);
            finish_tj_node($$, $1, $4);
        }
    ;

report_kw
    : KW_TASKREPORT
    | KW_RESOURCEREPORT
    | KW_ACCOUNTREPORT
    | KW_TEXTREPORT
    | KW_TRACEREPORT
    | KW_EXPORT
    | KW_STATUSSHEETREPORT
    | KW_TIMESHEETREPORT
    ;

/* ── navigator_decl ─────────────────────────────────────────────────────── *
 * Syntax: navigator <ID> [{ <attributes> }]
 * Body attribute: hidereport                                                */
navigator_decl
    : KW_NAVIGATOR TK_IDENT opt_body
        {
            Token no_name = {0};
            $$ = alloc_tj_node($1, $2, no_name);
            finish_tj_node($$, $1, $3);
        }
    ;

/* ── scenario_decl ──────────────────────────────────────────────────────── *
 * Syntax: scenario <id> <name> [{ <attributes> }]
 * Context: project, scenario (recursive)
 * Body attributes: active, disabled, enabled, projection, scenario         */
scenario_decl
    : KW_SCENARIO TK_IDENT opt_name opt_body
        {
            $$ = alloc_tj_node($1, $2, $3);
            finish_tj_node($$, $1, $4);
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
            $$ = alloc_tj_node($1, $2, no_name);
            finish_tj_node($$, $1, $4);
        }
    ;

/* ── statussheet_decl ───────────────────────────────────────────────────── *
 * Syntax: statussheet <reporter> <interval4> [{ <attributes> }]
 * reporter is a resource ID.                                                */
statussheet_decl
    : KW_STATUSSHEET TK_IDENT interval3 opt_body
        {
            Token no_name = {0};
            $$ = alloc_tj_node($1, $2, no_name);
            finish_tj_node($$, $1, $4);
        }
    ;

/* ── tagfile_decl ───────────────────────────────────────────────────────── *
 * Syntax: tagfile [<id>] <file name> [{ <attributes> }]
 * TODO: verify tj3man syntax for tagfile                                   */
tagfile_decl
    : KW_TAGFILE opt_id string_val opt_body
        {
            $$ = alloc_tj_node($1, $2, $3);
            finish_tj_node($$, $1, $4);
        }
    ;

/* ── journalentry_decl ──────────────────────────────────────────────────── *
 * Syntax: journalentry <date> <headline> [{ <attributes> }]
 * Body attributes: alert, author, details, flags.journalentry, summary
 * Note: journalentry is treated as a tj_node-producing declaration so that
 *       it appears in the document outline.
 * TODO: decide whether journalentry should be a tj_node or just an attribute. */
journalentry_decl
    : KW_JOURNALENTRY TK_DATE string_val opt_body
        {
            Token no_id = {0};
            /* Use the date as the detail and the headline as the name */
            $$ = alloc_tj_node($1, no_id, $3);
            finish_tj_node($$, $1, $4);
        }
    ;

/* ── extend_stmt ────────────────────────────────────────────────────────── *
 * Syntax: extend (task | resource) [{ <attributes> }]
 * Body attributes: date.extend, number.extend, reference.extend,
 *   richtext.extend, text.extend                                            */
extend_stmt
    : KW_EXTEND extend_target opt_body
    ;

extend_target
    : KW_TASK
    | KW_RESOURCE
    ;

/* ── supplement_stmt ────────────────────────────────────────────────────── *
 * Syntax: supplement (account <account ID> [{ <attributes> }] |
 *                     report  <report ID>  [{ <attributes> }] |
 *                     resource <resource ID> [{ <attributes> }] |
 *                     task <task ID> [{ <attributes> }])
 * Note: 'report' is not a keyword token, so it is handled as TK_IDENT.    */
supplement_stmt
    : KW_SUPPLEMENT supplement_target dotted_id opt_body
    ;

supplement_target
    : KW_ACCOUNT
    | KW_TASK
    | KW_RESOURCE
    | TK_IDENT     /* 'report' keyword (not in KW_* set) */
    ;

/* ── macro_stmt ─────────────────────────────────────────────────────────── *
 * Syntax: macro <ID> <MACRO>
 * where <MACRO> is the content wrapped in square brackets: [ ... ]
 * The macro body may contain any tokens; we accept a sequence of
 * macro_body_tok (anything except TK_RBRACKET) wrapped in [ ].             */
macro_stmt
    : KW_MACRO TK_IDENT TK_LBRACKET macro_body TK_RBRACKET
    ;

/* ── include_stmt ───────────────────────────────────────────────────────── *
 * Syntax (in properties context): include <filename> [{ <attributes> }]
 * Syntax (in project context):    include <filename>
 * We use opt_body for leniency.
 *
 * The mid-rule action bumps g_in_include_depth before opt_body fires so
 * that any KW_TASKPREFIX / KW_RESOURCEPREFIX / KW_ACCOUNTPREFIX /
 * KW_REPORTPREFIX attribute inside the body stashes its value into the
 * matching slot of g_pending_prefixes.  The trailing action then
 * consumes the set into an IncludeRef and resets it. */
include_stmt
    : KW_INCLUDE string_val
        { g_in_include_depth++; }
      opt_body
        {
            g_in_include_depth--;
            push_include(g_output, $2.text, &g_pending_prefixes);
            prefix_set_clear(&g_pending_prefixes);
        }
    ;

/* ══════════════════════════════════════════════════════════════════════════ *
 * Helper non-terminal rules
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── num_val: integer or floating-point number ──────────────────────────── */
num_val
    : TK_INTEGER
    | TK_FLOAT
    ;

/* ── dur_unit: duration unit identifier (min h d w m y %) ──────────────── *
 * These are plain identifiers in the TJP language; any TK_IDENT is accepted
 * here.  Semantic validation (ensuring the unit is one of min/h/d/w/m/y)
 * is left to downstream passes.                                             */
dur_unit
    : TK_IDENT
    | TK_PERCENT   /* for 'work' which uses % as a unit */
    ;

/* ── dur_val: duration value = number + unit ────────────────────────────── *
 * Syntax: <value> (min | h | d | w | m | y)
 * Token sequence: TK_INTEGER TK_IDENT  or  TK_FLOAT TK_IDENT
 * The lexer also produces TK_DURATION for compact durations like 4d, +4m,
 * or -2h.  Both signed and unsigned compact forms are accepted here.       */
dur_val
    : num_val dur_unit
    | TK_DURATION
    ;

/* ── string_val: a quoted or multi-line string ──────────────────────────── */
string_val
    : TK_STR
    | TK_MULTI_LINE_STR
    ;

/* ── opt_id: optional identifier (the TJP property ID) ─────────────────── *
 * TJP allows any identifier—including reserved words—as a task/resource ID.
 * Keywords that are commonly used as IDs must be listed here explicitly so
 * that the lexer's keyword token does not prevent them from being recognised
 * as an identifier in declaration position.                                 */
opt_id
    : /* empty */  { $$ = (Token){0}; }
    | TK_IDENT
    | KW_START
    ;

/* ── opt_name: optional display-name string ─────────────────────────────── */
opt_name
    : /* empty */       { $$ = (Token){0}; }
    | TK_STR
    | TK_MULTI_LINE_STR
    ;

/* ── opt_version: optional version string (project declaration only) ─────── */
opt_version
    : /* empty */  { $$ = (Token){0}; }
    | TK_STR
    ;

/* ── interval2: required interval ──────────────────────────────────────── *
 * Syntax: <date> (- <date> | + <duration> (min | h | d | w | m | y))
 * TK_DURATION captures compact forms like 4d, +4m, -2h as a single token.
 * The separate-token form (+ N unit) is also accepted.                     */
interval2
    : TK_DATE TK_MINUS TK_DATE
    | TK_DATE TK_DURATION
    | TK_DATE TK_PLUS num_val dur_unit
    ;

/* ── interval3: optional interval ──────────────────────────────────────── *
 * Syntax: <date> [(- <date> | + <duration> unit | <duration>)]
 * Used in: vacation, leaves, statussheet, timesheet                        */
interval3
    : TK_DATE
    | TK_DATE TK_MINUS TK_DATE
    | TK_DATE TK_DURATION
    | TK_DATE TK_PLUS num_val dur_unit
    ;

/* ── dotted_id: dot-separated identifier path ───────────────────────────── *
 * Syntax: <id> [. <id> [. <id> ...]]
 * Used for: taskroot, adopt targets, supplement target IDs                 */
dotted_id
    : TK_IDENT
    | dotted_id TK_DOT TK_IDENT
    ;

/* ── prefix_path_id: dotted identifier returned as a heap-allocated string
 *
 * Same shape as dotted_id but the action joins the segments and yields
 * the result string for the caller to consume.  Used by the *prefix
 * attribute rules so the include_stmt action can stash the value into
 * an IncludeRef. */
prefix_path_id
    : TK_IDENT
        { $$ = strdup($1.text); /* tok text is arena-borrowed; copy out */ }
    | prefix_path_id TK_DOT TK_IDENT
        {
            size_t plen = strlen($1);
            size_t slen = strlen($3.text);
            char *buf = malloc(plen + 1 + slen + 1);
            if (!buf) { fprintf(stderr, "taskjuggler-lsp: out of memory\n"); exit(1); }
            memcpy(buf, $1, plen);
            buf[plen] = '.';
            memcpy(buf + plen + 1, $3.text, slen + 1);
            free($1);
            $$ = buf;
        }
    ;

/* ── dotted_id_list: comma-separated list of dotted IDs ────────────────── */
dotted_id_list
    : dotted_id
    | dotted_id_list TK_COMMA dotted_id
    ;

/* ── id_list: comma-separated list of plain identifiers ─────────────────── */
id_list
    : TK_IDENT
    | id_list TK_COMMA TK_IDENT
    ;

/* ── bang_seq: zero or more leading ! tokens ────────────────────────────── *
 * Returns the count of bangs seen (int ival).                               */
bang_seq
    : /* empty */
        { $$ = 0; }
    | bang_seq TK_BANG
        { $$ = $1 + 1; }
    ;

/* ── dep_path_seg: one segment of a dotted task path ────────────────────── *
 * TaskJuggler allows any identifier—including reserved words—as a task ID.
 * Keywords that are commonly used as task IDs must be listed here so that
 * the lexer's keyword token does not prevent them from being recognised as
 * a path segment in dependency references.                                  */
dep_path_seg
    : TK_IDENT
    | KW_START
    ;

/* ── dep_path: dotted identifier path for dep references ────────────────── *
 * Distinct from dotted_id so taskprefix/taskroot are unaffected.           */
dep_path
    : dep_path_seg
        /* A single segment's lexeme already lives in the token arena, so the
         * path just borrows it — no copy.  Multi-segment paths (below) are
         * built into the same arena, which reclaims every dep path with the
         * rest of the tree. */
        { $$.bang_count = 0; $$.path = $1.text;
          $$.start = $1.start; $$.end = $1.end; }
    | dep_path TK_DOT dep_path_seg
        { size_t plen = strlen($1.path);
          size_t slen = strlen($3.text);
          char *buf = arena_alloc(g_tok_arena, plen + 1 + slen + 1);
          memcpy(buf, $1.path, plen);
          buf[plen] = '.';
          memcpy(buf + plen + 1, $3.text, slen + 1);
          $$.bang_count = 0;
          $$.path  = buf;
          $$.start = $1.start;
          $$.end   = $3.end; }
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
            /* Body attributes (gaplength / gapduration) are not captured —
             * the body's nodes are simply left behind in the arena. */
            tj_node *task = sym_stack_top();
            if (task) {
                Dependency dep = {
                    .kind            = g_pending_dep_kind,
                    .bang_count      = $1.bang_count,
                    .path            = $1.path,   /* transfer ownership */
                    .source_range    = { $1.start, $1.end },
                };
                tj_node_push_dependency(task, dep);
            } else {
                /* `depends`/`precedes` outside a task body — syntactically
                 * possible during error recovery; nothing to attach to.  The
                 * path is arena-owned, so there is nothing to free here. */
            }
        }
    ;

/* ── dep_ref_list: comma-separated dependency references ────────────────── */
dep_ref_list
    : dep_ref
    | dep_ref_list TK_COMMA dep_ref
    ;

/* ── alloc_ref: resource ID with optional allocation body ───────────────── *
 * Used in allocate statements.
 * Body may contain: alternative, mandatory, persistent, select,
 *   shift.allocate, shifts.allocate                                        */
alloc_ref
    : TK_IDENT opt_body
    ;

/* ── alloc_ref_list: comma-separated allocation references ─────────────── */
alloc_ref_list
    : alloc_ref
    | alloc_ref_list TK_COMMA alloc_ref
    ;

/* ── shift_attr_ref: shift ID with optional interval ────────────────────── *
 * Syntax: <shift> [<interval2>]
 * Used in shift/shifts attribute statements.                               */
shift_attr_ref
    : TK_IDENT
    | TK_IDENT interval2
    ;

/* ── shift_attr_list: comma-separated shift references ─────────────────── */
shift_attr_list
    : shift_attr_ref
    | shift_attr_list TK_COMMA shift_attr_ref
    ;

/* ── booking_interval_list: one or more intervals for booking ───────────── *
 * Syntax: <interval4> [, <interval4>...]                                   */
booking_interval_list
    : interval3
    | booking_interval_list TK_COMMA interval3
    ;

/* ── chargeset_list: account + percentage pairs ─────────────────────────── *
 * Syntax: <account> <share> [, <account> <share>...]
 * <share> is a number followed by %, e.g. 50%
 * In the lexer, '50%' is tokenized as TK_INTEGER TK_PERCENT (two tokens).  */
chargeset_list
    : TK_IDENT num_val TK_PERCENT
    | chargeset_list TK_COMMA TK_IDENT num_val TK_PERCENT
    ;

/* ── credits_list: date + description + amount triples ──────────────────── *
 * Syntax: <date> <description> <amount> [, <date> <description> <amount>...]
 */
credits_list
    : TK_DATE string_val num_val
    | credits_list TK_COMMA TK_DATE string_val num_val
    ;

/* ── column_id: a column name token ─────────────────────────────────────── *
 * Column IDs include both plain identifiers and many keyword tokens that
 * happen to share names with column IDs (e.g. start, end, effort, alert).
 * Rather than listing all ~60+ valid column IDs, we accept any single
 * gen_expr_tok here.  The columns_list rule uses commas as delimiters so
 * the column ID consumes only one token.
 *
 * TODO: add all other column IDs as seen in tj3man columnid listing:
 * activetasks, annualleave, annualleavebalance, annualleavelist, bsi,
 * chart, children, closedtasks, competitorcount, competitors, cost,
 * criticalness, daily, directreports, duties, effortdone, effortleft,
 * followers, id, journal, managers, monthly, name, no, overtime, ...
 * Most of these are plain TK_IDENT so they are already covered.            */
column_id
    : TK_IDENT
    /* Column IDs that are also keyword tokens: */
    | KW_ALERT | KW_COMPLETE | KW_DATE | KW_DURATION | KW_EFFORT | KW_EMAIL
    | KW_END | KW_FLAGS | KW_LEAVES | KW_LENGTH | KW_NOTE | KW_PRIORITY
    | KW_RATE | KW_REMAINING | KW_START | KW_STATUS | KW_WORK
    ;

/* ── column_entry: single column specification ──────────────────────────── *
 * Syntax: <columnid> [{ <attributes> }]
 * Body attributes: cellcolor, celltext, end, fontcolor, halign, listitem,
 *   listtype, period, scale, start, timeformat1, timeformat2, title,
 *   tooltip, width                                                          */
column_entry
    : column_id opt_body
    ;

/* ── column_list: comma-separated column specifications ─────────────────── */
column_list
    : column_entry
    | column_list TK_COMMA column_entry
    ;

/* ── formats_list: comma-separated format identifiers ───────────────────── *
 * Syntax: (csv | html | niku) [, ...]
 * These are TK_IDENT tokens (not keywords).                                */
formats_list
    : TK_IDENT
    | formats_list TK_COMMA TK_IDENT
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
    : TK_IDENT | TK_INTEGER | TK_FLOAT | TK_STR | TK_DATE | TK_DURATION
    | TK_BANG | TK_PLUS | TK_MINUS | TK_DOT | TK_COLON | TK_COMMA
    | TK_PERCENT | TK_DOLLAR | TK_LBRACKET | TK_RBRACKET
    | TK_MULTI_LINE_STR | TK_ERROR
    /* Attribute/expression keywords (included so gen_expr can parse
     * expressions like "plan.effort = 0" or "~isleaf()"):                  */
    | KW_ACCOUNTPREFIX | KW_ACCOUNTROOT | KW_ACTIVE | KW_ADOPT
    | KW_AGGREGATE | KW_ALERT | KW_ALERTLEVELS | KW_ALLOCATE
    | KW_ALTERNATIVE | KW_AUTHOR | KW_AUXDIR | KW_BALANCE | KW_BOOKING
    | KW_CAPTION | KW_CELLCOLOR | KW_CELLTEXT | KW_CENTER | KW_CHARGE
    | KW_CHARGESET | KW_COLUMNS | KW_COMPLETE | KW_COPYRIGHT | KW_CREDITS
    | KW_CURRENCY | KW_CURRENCYFORMAT | KW_DAILYMAX | KW_DAILYMIN
    | KW_DAILYWORKINGHOURS | KW_DATE | KW_DEFINITIONS | KW_DEPENDS
    | KW_DETAILS | KW_DISABLED | KW_DURATION | KW_EFFICIENCY | KW_EFFORT
    | KW_EFFORTDONE | KW_EFFORTLEFT | KW_EMAIL | KW_ENABLED | KW_END
    | KW_ENDCREDIT | KW_EPILOG | KW_FAIL | KW_FLAGS | KW_FONTCOLOR
    | KW_FOOTER | KW_FORMATS | KW_GAPDURATION | KW_GAPLENGTH | KW_HALIGN
    | KW_HASALERT | KW_HEADER | KW_HEADLINE | KW_HEIGHT | KW_HIDEACCOUNT
    | KW_HIDEJOURNALENTRY | KW_HIDEREPORT | KW_HIDERESOURCE | KW_HIDETASK
    | KW_INHERIT | KW_ISACTIVE | KW_ISCHILDOF | KW_ISDEPENDENCYOF
    | KW_ISDUTYOF | KW_ISFEATUREOF | KW_ISLEAF | KW_ISMILESTONE
    | KW_ISONGOING | KW_ISRESOURCE | KW_ISRESPONSIBILITYOF | KW_ISTASK
    | KW_ISVALID | KW_JOURNALATTRIBUTES | KW_JOURNALMODE
    | KW_LEAVEALLOWANCES | KW_LEAVES | KW_LEFT | KW_LENGTH | KW_LIMITS
    | KW_LISTITEM | KW_LISTTYPE | KW_LOADUNIT | KW_MANAGERS | KW_MANDATORY
    | KW_MARKDATE | KW_MAXEND | KW_MAXIMUM | KW_MAXSTART | KW_MILESTONE
    | KW_MINEND | KW_MINIMUM | KW_MINSTART | KW_MONTHLYMAX | KW_MONTHLYMIN
    | KW_NEWTASK | KW_NOTE | KW_NOVEVENTS | KW_NOW | KW_NUMBER
    | KW_NUMBERFORMAT | KW_ONEND | KW_ONSTART | KW_OPENNODES | KW_OUTPUTDIR
    | KW_OVERTIME | KW_PERIOD | KW_PERSISTENT | KW_PRECEDES | KW_PRIORITY
    | KW_PROJECTID | KW_PROJECTIDS | KW_PROJECTION | KW_PROLOG | KW_PURGE
    | KW_RATE | KW_RAWHTMLHEAD | KW_REFERENCE | KW_REMAINING | KW_REPLACE
    | KW_REPORTPREFIX | KW_RESOURCEATTRIBUTES | KW_RESOURCEPREFIX
    | KW_RESOURCEROOT | KW_RESOURCES | KW_RESPONSIBLE | KW_RICHTEXT
    | KW_RIGHT | KW_ROLLUPACCOUNT | KW_ROLLUPRESOURCE | KW_ROLLUPTASK
    | KW_SCALE | KW_SCENARIOS | KW_SCENARIOSPECIFIC | KW_SCHEDULED
    | KW_SCHEDULING | KW_SCHEDULINGMODE | KW_SELECT | KW_SELFCONTAINED
    | KW_SHIFTS | KW_SHORTTIMEFORMAT | KW_SLOPPY | KW_SORTACCOUNTS
    | KW_SORTJOURNALENTRIES | KW_SORTRESOURCES | KW_SORTTASKS | KW_START
    | KW_STARTCREDIT | KW_STATUS | KW_STRICT | KW_SUMMARY | KW_TAGFILE
    | KW_TASKATTRIBUTES | KW_TASKPREFIX | KW_TASKROOT | KW_TEXT
    | KW_TIMEFORMAT | KW_TIMEFORMAT1 | KW_TIMEFORMAT2 | KW_TIMEOFF
    | KW_TIMEZONE | KW_TIMINGRESOLUTION | KW_TITLE | KW_TOOLTIP
    | KW_TRACKINGSCENARIO | KW_TREELEVEL | KW_VACATION | KW_WARN
    | KW_WEEKLYMAX | KW_WEEKLYMIN | KW_WEEKSTARTSMONDAY
    | KW_WEEKSTARTSSUNDAY | KW_WIDTH | KW_WORK | KW_WORKINGHOURS
    | KW_YEARLYWORKINGDAYS
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
    : TK_IDENT | TK_INTEGER | TK_FLOAT | TK_STR | TK_DATE | TK_DURATION
    | TK_LBRACE | TK_RBRACE | TK_LBRACKET | TK_BANG | TK_PLUS | TK_MINUS
    | TK_DOT | TK_COLON | TK_COMMA | TK_PERCENT | TK_DOLLAR
    | TK_MULTI_LINE_STR | TK_ERROR
    | TK_EOL   /* synthetic terminator inside the macro body */
    /* All KW_* tokens (a macro body can contain any keywords):             */
    | KW_PROJECT | KW_TASK | KW_RESOURCE | KW_ACCOUNT | KW_SHIFT
    | KW_TASKREPORT | KW_RESOURCEREPORT | KW_ACCOUNTREPORT | KW_TEXTREPORT
    | KW_TRACEREPORT | KW_ICALREPORT | KW_NIKUREPORT | KW_EXPORT
    | KW_STATUSSHEETREPORT | KW_TIMESHEETREPORT | KW_NAVIGATOR | KW_TAGFILE
    | KW_MACRO | KW_INCLUDE | KW_SUPPLEMENT | KW_SCENARIO | KW_EXTEND
    | KW_TIMESHEET | KW_STATUSSHEET | KW_JOURNALENTRY
    /* Plus all attribute keywords via gen_expr_tok re-use (or list them):  */
    | KW_ALLOCATE | KW_DEPENDS | KW_EFFORT | KW_START | KW_END
    | KW_COMPLETE | KW_NOTE | KW_TOOLTIP | KW_CELLCOLOR | KW_HIDETASK
    | KW_HIDERESOURCE | KW_FORMATS | KW_COLUMNS | KW_LOADUNIT
    | KW_HEADLINE | KW_SORTTASKS | KW_SORTRESOURCES | KW_TITLE
    /* TODO: list all remaining KW_* here, or factor into a shared rule.    */
    ;

/* ── opt_args: zero or more argument tokens (fallback rule only) ─────────── *
 *
 * Used ONLY in the TK_IDENT fallback alternative and the stub_kw
 * alternatives in the plain_stmt rule.  KW_* tokens are intentionally
 * excluded so that statement boundaries are clean: when a KW_* token
 * appears while consuming opt_args, bison will reduce and the outer items
 * loop will start a new item with that keyword.                             */
opt_args
    : /* empty */
    | opt_args arg_token
    ;

arg_token
    : TK_IDENT | TK_STR | TK_INTEGER | TK_FLOAT | TK_DATE | TK_DURATION
    | TK_BANG | TK_PLUS | TK_MINUS | TK_DOT | TK_COLON | TK_COMMA
    | TK_PERCENT | TK_DOLLAR | TK_LBRACKET | TK_RBRACKET
    | TK_MULTI_LINE_STR | TK_ERROR
    ;

/* ── opt_body: optional { items } block ─────────────────────────────────── *
 *
 * Returns a BodyResult with the collected child symbols and the end
 * position of the closing `}`.
 *
 * There is no error-recovery alternative for a missing `}`: the yylex wrapper
 * in lexer.l synthesises the closing braces for any still-open bodies at EOF,
 * so a mid-edit file with unclosed bodies reduces normally instead of needing
 * a `'{' body_items error` production (which would reduce/reduce-conflict with
 * `item: error`).                                                            */
opt_body
    : /* empty */
        { $$.syms = (SymArr){0}; $$.end = (LspPos){0}; }
    | TK_LBRACE body_items TK_RBRACE
        {
            $$.syms = $2.syms;
            $$.end  = $3.end;
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
    | body_items TK_EOL   /* absorb the terminator that follows a non-gen_expr item */
        { $$ = $1; }
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
