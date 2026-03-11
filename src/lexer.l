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

#define YY_USER_ACTION                          \
    tok_sl   = (uint32_t)(yylineno - 1);        \
    tok_sc   = (uint32_t)yycolumn;              \
    yycolumn += yyleng;

/* ── emit(): record token in array and populate yylval ──────────────────── */


/* TODO create a semantic token struct
 *
 * Fields: start    (line and column)
 *         end      (line and column)
 *         type     (based on LSP SemanticTokenTypes)
 *         modifier (based on LSP SemanticTokenModifiers)
 *
 * These objects will be used by the SemanticToken code to return
 * text/Document/semanticTokens/* responses.
 */

/* TODO create a data structure to hold semantic token objects
 *
 * This data structure will need to be efficiently processed by the
 * SemanticToken code. Something that can be traversed linearly efficiently.
 * Possibly just a variable-length array.
 */

static void store_semantic_token(int token) {
    // TODO I'm not happy with these function and variable names, improve readability.
    // TODO the el/ec and tok_sl/tok_sc calculations for .start and .end are probably broken.
    uint32_t el = (uint32_t)(yylineno - 1);
    uint32_t ec = (uint32_t)yycolumn;
    semantic_token.type     = tj_token_to_semantic_type(token);
    semantic_token.modifier = tj_token_to_semantic_modifier(token);
    semantic_token.start    = (LspPos){ tok_sl, tok_sc };
    semantic_token.end      = (LspPos){ el, ec };
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

#[^\n]*         { store_semantic_token(TJ3_LINE_COMMENT); /* not returned to parser */ }
\/\/[^\n]*      { store_semantic_token(TJ3_LINE_COMMENT); /* not returned to parser */ }

"/*"            { BEGIN(BLOCK_COMMENT); }
<BLOCK_COMMENT>{
    "*/"        { store_semantic_token(TJ3_MULTI_LINE_COMMENT); BEGIN(INITIAL);
                  /* not returned to parser */ }
    \n          { yycolumn = 0; }
    [^*\n]+     { /* skip whitespace */ }
    "*"         { /* not returned to parser */ }
    <<EOF>>     { BEGIN(INITIAL); return TJ3_ERROR; }
}

    /* Scissors multi-line string: -8<- ... ->8- */
"-8<-"          { BEGIN(SCISSORS); }
<SCISSORS>{
    "->8-"      { store_semantic_token(TJ3_MULTI_LINE_STR);
                  BEGIN(INITIAL); return TJ3_MULTI_LINE_STR; }
    \n          { yycolumn = 0; }
    [^-\n]+     { }
    "-"         { }
    <<EOF>>     { BEGIN(INITIAL); return TJ3_ERROR; }
}

\"              { BEGIN(DQUOTE); }
<DQUOTE>{
    \"          { store_semantic_token(TJ3_STR); BEGIN(INITIAL); return TJ3_STR; }
    \n          { yycolumn = 0;
                  BEGIN(INITIAL); return TJ3_ERROR; }
    \\[^\n]     { }
    [^\"\\\n]+  { }
    <<EOF>>     { BEGIN(INITIAL); return TJ3_ERROR; }
}

    /* Date YYYY-MM-DD[-hh:mm[:ss]][-{+|-}HHMM]: must precede INT so longest match wins.
     * Time, seconds, and timezone suffix are each independently optional per the TJP spec.
     * Timezone is always a leading '-' separator followed by an explicit '+' or '-' sign. */
[0-9]{4}-[0-9]{2}-[0-9]{2}(-[0-9]{2}:[0-9]{2}(:[0-9]{2})?)?(-[+-][0-9]{4})?  { store_semantic_token(TJ3_DATE); return TJ3_DATE; }

[0-9]+"."[0-9]+  { emit(TJ3_FLOAT, yytext); return TJ3_FLOAT; }

[0-9]+           { emit(TJ3_INTEGER, yytext); return TJ3_INTEGER; }

    /* Identifier / keyword: letters, digits, '_', ':', high UTF-8 bytes.
     * '.' is NOT included; it is emitted as TJ3_DOT so that dotted dependency
     * paths (e.g. deliveries.start) can be resolved component-wise by the LSP. */
[a-zA-Z_\x80-\xFF][a-zA-Z0-9_:\x80-\xFF]* {
    int tok = keyword_token(yytext);
    emit(TJ3_IDENT, yytext); /* always TJ3_IDENT in token array */
    yylval.tok.kind = tok;  /* KW_* for symbol keywords        */
    return tok;
}

    /* Keywords */
"account"    { store_semantic_token(TJ3_ACCOUNT); return TJ3_ACCOUNT }

/*  Full list of TJ3 keywords (reported by tj3man)
account
accountprefix
accountreport
accountroot
active
adopt
aggregate
alert
alertlevels
allocate
alternative
author
auxdir
balance
booking
caption
cellcolor
celltext
center
charge
chargeset
columnid
columns
complete
copyright
credits
currency
currencyformat
dailymax
dailymin
dailyworkinghours
date
definitions
depends
details
disabled
duration
efficiency
effort
effortdone
effortleft
email
enabled
end
endcredit
epilog
export
extend
fail
flags
fontcolor
footer
formats
functions
gapduration
gaplength
center
halign
left
right  <-- checked until here
hasalert
header
headline
height
hideaccount
hidejournalentry
hidereport
hideresource
hidetask
icalreport
include.macro
include.project
include.properties
inherit.extend
interval1
interval2
interval3
interval4
isactive
ischildof
isdependencyof
isdutyof
isfeatureof
isleaf
ismilestone
isongoing
isresource
isresponsibilityof
istask
isvalid
journalattributes
journalentry
journalmode
leaveallowance
leaves
left
length
limits
limits.allocate
limits.resource
limits.task
listitem.column
listtype.column
loadunit
logicalexpression
logicalflagexpression
macro
managers
mandatory
markdate
maxend
maximum
maxstart
milestone
minend
minimum
minstart
monthlymax
monthlymin
navigator
newtask
nikureport
note.task
novevents
now
number.extend
numberformat
onend
onstart
opennodes
outputdir
overtime.booking
period.column
period.limit
period.report
period.task
persistent
precedes
priority
priority.timesheet
project
projectid
projectid.task
projectids
projection
prolog
properties
purge
rate
rate.resource
rawhtmlhead
reference.extend
remaining
replace
reportprefix
resource
resourceattributes
resourceprefix
resourcereport
resourceroot
resources.limit
responsible
richtext.extend
right
rollupaccount
rollupresource
rolluptask
scale.column
scenario
scenario.ical
scenarios
scenarios.export
scenariospecific.extend
scheduled
scheduling
schedulingmode
select
selfcontained
shift
shift.allocate
shift.resource
shift.task
shift.timesheet
shifts.allocate
shifts.resource
shifts.task
shorttimeformat
sloppy.booking
sloppy.projection
sortaccounts
sortjournalentries
sortresources
sorttasks
start
start.column
start.limit
start.report
startcredit
status.statussheet
status.timesheet
statussheet
statussheetreport
strict.projection
summary
supplement
supplement.resource
supplement.task
tagfile
task
task.statussheet
task.timesheet
taskattributes
taskprefix
taskreport
taskroot
taskroot.export
text.extend
textreport
timeformat
timeformat1
timeformat2
timeoff.nikureport
timesheet
timesheetreport
timezone
timezone.export
timezone.report
timezone.shift
timingresolution
title
title.column
tooltip.column
tracereport
trackingscenario
treelevel
vacation
vacation.resource
vacation.shift
warn
weeklymax
weeklymin
weekstartsmonday
weekstartssunday
width
width.column
work
workinghours.project
workinghours.resource
workinghours.shift
yearlyworkingdays
*/

"{"     { emit(TJ3_LBRACE,   yytext); return TJ3_LBRACE;   }
"}"     { emit(TJ3_RBRACE,   yytext); return TJ3_RBRACE;   }
"["     { emit(TJ3_LBRACKET, yytext); return TJ3_LBRACKET; }
"]"     { emit(TJ3_RBRACKET, yytext); return TJ3_RBRACKET; }
"!"     { emit(TJ3_BANG,     yytext); return TJ3_BANG;     }
"+"     { emit(TJ3_PLUS,     yytext); return TJ3_PLUS;     }
"-"     { emit(TJ3_MINUS,    yytext); return TJ3_MINUS;    }
"."     { emit(TJ3_DOT,      yytext); return TJ3_DOT;      }
":"     { emit(TJ3_COLON,    yytext); return TJ3_COLON;    }
","     { emit(TJ3_COMMA,    yytext); return TJ3_COMMA;    }
"%"     { emit(TJ3_PERCENT,  yytext); return TJ3_PERCENT;  }
"$"     { emit(TJ3_DOLLAR,   yytext); return TJ3_DOLLAR;   }

.       { char msg[48];
          snprintf(msg, sizeof(msg), "unexpected character: '%s'", yytext);
          emit(TJ3_ERROR, msg); return TJ3_ERROR; }

%%
