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

/* ── store_semantic_token() ──────────────────────────────────────────────── *
 *
 * Records position and type for the current token.  The full semantic-token
 * data structure and helper functions are not yet designed; the body is
 * stubbed out pending that work.
 *
 * TODO create a semantic token struct
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
    /* TODO I'm not happy with these function and variable names, improve readability. */
    /* TODO the el/ec and tok_sl/tok_sc calculations for .start and .end are probably broken. */
    uint32_t el = (uint32_t)(yylineno - 1);
    uint32_t ec = (uint32_t)yycolumn;
    /* TODO semantic_token.type     = tj_token_to_semantic_type(token); */
    /* TODO semantic_token.modifier = tj_token_to_semantic_modifier(token); */
    /* TODO semantic_token.start    = (LspPos){ tok_sl, tok_sc }; */
    /* TODO semantic_token.end      = (LspPos){ el, ec }; */
    (void)token; (void)el; (void)ec;
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

"/*"            { BEGIN(BLOCK_COMMENT); }
<BLOCK_COMMENT>{
    "*/"        { store_semantic_token(TK_BLOCK_COMMENT); BEGIN(INITIAL);
                  /* not returned to parser */ }
    \n          { yycolumn = 0; }
    [^*\n]+     { /* skip */ }
    "*"         { /* not returned to parser */ }
    <<EOF>>     { BEGIN(INITIAL); return TK_ERROR; }
}

    /* Scissors multi-line string: -8<- ... ->8- */
"-8<-"          { BEGIN(SCISSORS); }
<SCISSORS>{
    "->8-"      { store_semantic_token(TK_MULTI_LINE_STR);
                  BEGIN(INITIAL); return TK_MULTI_LINE_STR; }
    \n          { yycolumn = 0; }
    [^-\n]+     { }
    "-"         { }
    <<EOF>>     { BEGIN(INITIAL); return TK_ERROR; }
}

\"              { BEGIN(DQUOTE); }
<DQUOTE>{
    \"          { store_semantic_token(TK_STR); BEGIN(INITIAL); return TK_STR; }
    \n          { yycolumn = 0;
                  BEGIN(INITIAL); return TK_ERROR; }
    \\[^\n]     { }
    [^\"\\\n]+  { }
    <<EOF>>     { BEGIN(INITIAL); return TK_ERROR; }
}

    /* Date YYYY-MM-DD[-hh:mm[:ss]][-{+|-}HHMM]: must precede INT so longest match wins.
     * Time, seconds, and timezone suffix are each independently optional per the TJP spec.
     * Timezone is always a leading '-' separator followed by an explicit '+' or '-' sign. */
[0-9]{4}-[0-9]{2}-[0-9]{2}(-[0-9]{2}:[0-9]{2}(:[0-9]{2})?)?(-[+-][0-9]{4})?  { store_semantic_token(TK_DATE); return TK_DATE; }

[0-9]+"."[0-9]+  { return TK_FLOAT; }

[0-9]+           { return TK_INTEGER; }

    /* Keywords — listed alphabetically; must precede the identifier catch-all
     * so that an exact match wins over the longer-pattern identifier rule. */
"account"            { store_semantic_token(KW_ACCOUNT);            return KW_ACCOUNT;            }
"accountprefix"      { store_semantic_token(KW_ACCOUNTPREFIX);      return KW_ACCOUNTPREFIX;      }
"accountreport"      { store_semantic_token(KW_ACCOUNTREPORT);      return KW_ACCOUNTREPORT;      }
"accountroot"        { store_semantic_token(KW_ACCOUNTROOT);        return KW_ACCOUNTROOT;        }
"active"             { store_semantic_token(KW_ACTIVE);             return KW_ACTIVE;             }
"adopt"              { store_semantic_token(KW_ADOPT);              return KW_ADOPT;              }
"aggregate"          { store_semantic_token(KW_AGGREGATE);          return KW_AGGREGATE;          }
"alert"              { store_semantic_token(KW_ALERT);              return KW_ALERT;              }
"alertlevels"        { store_semantic_token(KW_ALERTLEVELS);        return KW_ALERTLEVELS;        }
"allocate"           { store_semantic_token(KW_ALLOCATE);           return KW_ALLOCATE;           }
"alternative"        { store_semantic_token(KW_ALTERNATIVE);        return KW_ALTERNATIVE;        }
"author"             { store_semantic_token(KW_AUTHOR);             return KW_AUTHOR;             }
"auxdir"             { store_semantic_token(KW_AUXDIR);             return KW_AUXDIR;             }
"balance"            { store_semantic_token(KW_BALANCE);            return KW_BALANCE;            }
"booking"            { store_semantic_token(KW_BOOKING);            return KW_BOOKING;            }
"caption"            { store_semantic_token(KW_CAPTION);            return KW_CAPTION;            }
"cellcolor"          { store_semantic_token(KW_CELLCOLOR);          return KW_CELLCOLOR;          }
"celltext"           { store_semantic_token(KW_CELLTEXT);           return KW_CELLTEXT;           }
"center"             { store_semantic_token(KW_CENTER);             return KW_CENTER;             }
"charge"             { store_semantic_token(KW_CHARGE);             return KW_CHARGE;             }
"chargeset"          { store_semantic_token(KW_CHARGESET);          return KW_CHARGESET;          }
"columns"            { store_semantic_token(KW_COLUMNS);            return KW_COLUMNS;            }
"complete"           { store_semantic_token(KW_COMPLETE);           return KW_COMPLETE;           }
"copyright"          { store_semantic_token(KW_COPYRIGHT);          return KW_COPYRIGHT;          }
"credits"            { store_semantic_token(KW_CREDITS);            return KW_CREDITS;            }
"currency"           { store_semantic_token(KW_CURRENCY);           return KW_CURRENCY;           }
"currencyformat"     { store_semantic_token(KW_CURRENCYFORMAT);     return KW_CURRENCYFORMAT;     }
"dailymax"           { store_semantic_token(KW_DAILYMAX);           return KW_DAILYMAX;           }
"dailymin"           { store_semantic_token(KW_DAILYMIN);           return KW_DAILYMIN;           }
"dailyworkinghours"  { store_semantic_token(KW_DAILYWORKINGHOURS);  return KW_DAILYWORKINGHOURS;  }
"date"               { store_semantic_token(KW_DATE);               return KW_DATE;               }
"definitions"        { store_semantic_token(KW_DEFINITIONS);        return KW_DEFINITIONS;        }
"depends"            { store_semantic_token(KW_DEPENDS);            return KW_DEPENDS;            }
"details"            { store_semantic_token(KW_DETAILS);            return KW_DETAILS;            }
"disabled"           { store_semantic_token(KW_DISABLED);           return KW_DISABLED;           }
"duration"           { store_semantic_token(KW_DURATION);           return KW_DURATION;           }
"efficiency"         { store_semantic_token(KW_EFFICIENCY);         return KW_EFFICIENCY;         }
"effort"             { store_semantic_token(KW_EFFORT);             return KW_EFFORT;             }
"effortdone"         { store_semantic_token(KW_EFFORTDONE);         return KW_EFFORTDONE;         }
"effortleft"         { store_semantic_token(KW_EFFORTLEFT);         return KW_EFFORTLEFT;         }
"email"              { store_semantic_token(KW_EMAIL);              return KW_EMAIL;              }
"enabled"            { store_semantic_token(KW_ENABLED);            return KW_ENABLED;            }
"end"                { store_semantic_token(KW_END);                return KW_END;                }
"endcredit"          { store_semantic_token(KW_ENDCREDIT);          return KW_ENDCREDIT;          }
"epilog"             { store_semantic_token(KW_EPILOG);             return KW_EPILOG;             }
"export"             { store_semantic_token(KW_EXPORT);             return KW_EXPORT;             }
"extend"             { store_semantic_token(KW_EXTEND);             return KW_EXTEND;             }
"fail"               { store_semantic_token(KW_FAIL);               return KW_FAIL;               }
"flags"              { store_semantic_token(KW_FLAGS);              return KW_FLAGS;              }
"fontcolor"          { store_semantic_token(KW_FONTCOLOR);          return KW_FONTCOLOR;          }
"footer"             { store_semantic_token(KW_FOOTER);             return KW_FOOTER;             }
"formats"            { store_semantic_token(KW_FORMATS);            return KW_FORMATS;            }
"gapduration"        { store_semantic_token(KW_GAPDURATION);        return KW_GAPDURATION;        }
"gaplength"          { store_semantic_token(KW_GAPLENGTH);          return KW_GAPLENGTH;          }
"halign"             { store_semantic_token(KW_HALIGN);             return KW_HALIGN;             }
"hasalert"           { store_semantic_token(KW_HASALERT);           return KW_HASALERT;           }
"header"             { store_semantic_token(KW_HEADER);             return KW_HEADER;             }
"headline"           { store_semantic_token(KW_HEADLINE);           return KW_HEADLINE;           }
"height"             { store_semantic_token(KW_HEIGHT);             return KW_HEIGHT;             }
"hideaccount"        { store_semantic_token(KW_HIDEACCOUNT);        return KW_HIDEACCOUNT;        }
"hidejournalentry"   { store_semantic_token(KW_HIDEJOURNALENTRY);   return KW_HIDEJOURNALENTRY;   }
"hidereport"         { store_semantic_token(KW_HIDEREPORT);         return KW_HIDEREPORT;         }
"hideresource"       { store_semantic_token(KW_HIDERESOURCE);       return KW_HIDERESOURCE;       }
"hidetask"           { store_semantic_token(KW_HIDETASK);           return KW_HIDETASK;           }
"icalreport"         { store_semantic_token(KW_ICALREPORT);         return KW_ICALREPORT;         }
"include"            { store_semantic_token(KW_INCLUDE);            return KW_INCLUDE;            }
"inherit"            { store_semantic_token(KW_INHERIT);            return KW_INHERIT;            }
"isactive"           { store_semantic_token(KW_ISACTIVE);           return KW_ISACTIVE;           }
"ischildof"          { store_semantic_token(KW_ISCHILDOF);          return KW_ISCHILDOF;          }
"isdependencyof"     { store_semantic_token(KW_ISDEPENDENCYOF);     return KW_ISDEPENDENCYOF;     }
"isdutyof"           { store_semantic_token(KW_ISDUTYOF);           return KW_ISDUTYOF;           }
"isfeatureof"        { store_semantic_token(KW_ISFEATUREOF);        return KW_ISFEATUREOF;        }
"isleaf"             { store_semantic_token(KW_ISLEAF);             return KW_ISLEAF;             }
"ismilestone"        { store_semantic_token(KW_ISMILESTONE);        return KW_ISMILESTONE;        }
"isongoing"          { store_semantic_token(KW_ISONGOING);          return KW_ISONGOING;          }
"isresource"         { store_semantic_token(KW_ISRESOURCE);         return KW_ISRESOURCE;         }
"isresponsibilityof" { store_semantic_token(KW_ISRESPONSIBILITYOF); return KW_ISRESPONSIBILITYOF; }
"istask"             { store_semantic_token(KW_ISTASK);             return KW_ISTASK;             }
"isvalid"            { store_semantic_token(KW_ISVALID);            return KW_ISVALID;            }
"journalattributes"  { store_semantic_token(KW_JOURNALATTRIBUTES);  return KW_JOURNALATTRIBUTES;  }
"journalentry"       { store_semantic_token(KW_JOURNALENTRY);       return KW_JOURNALENTRY;       }
"journalmode"        { store_semantic_token(KW_JOURNALMODE);        return KW_JOURNALMODE;        }
"leaveallowances"    { store_semantic_token(KW_LEAVEALLOWANCES);    return KW_LEAVEALLOWANCES;    }
"leaves"             { store_semantic_token(KW_LEAVES);             return KW_LEAVES;             }
"left"               { store_semantic_token(KW_LEFT);               return KW_LEFT;               }
"length"             { store_semantic_token(KW_LENGTH);             return KW_LENGTH;             }
"limits"             { store_semantic_token(KW_LIMITS);             return KW_LIMITS;             }
"listitem"           { store_semantic_token(KW_LISTITEM);           return KW_LISTITEM;           }
"listtype"           { store_semantic_token(KW_LISTTYPE);           return KW_LISTTYPE;           }
"loadunit"           { store_semantic_token(KW_LOADUNIT);           return KW_LOADUNIT;           }
"macro"              { store_semantic_token(KW_MACRO);              return KW_MACRO;              }
"managers"           { store_semantic_token(KW_MANAGERS);           return KW_MANAGERS;           }
"mandatory"          { store_semantic_token(KW_MANDATORY);          return KW_MANDATORY;          }
"markdate"           { store_semantic_token(KW_MARKDATE);           return KW_MARKDATE;           }
"maxend"             { store_semantic_token(KW_MAXEND);             return KW_MAXEND;             }
"maximum"            { store_semantic_token(KW_MAXIMUM);            return KW_MAXIMUM;            }
"maxstart"           { store_semantic_token(KW_MAXSTART);           return KW_MAXSTART;           }
"milestone"          { store_semantic_token(KW_MILESTONE);          return KW_MILESTONE;          }
"minend"             { store_semantic_token(KW_MINEND);             return KW_MINEND;             }
"minimum"            { store_semantic_token(KW_MINIMUM);            return KW_MINIMUM;             }
"minstart"           { store_semantic_token(KW_MINSTART);           return KW_MINSTART;           }
"monthlymax"         { store_semantic_token(KW_MONTHLYMAX);         return KW_MONTHLYMAX;         }
"monthlymin"         { store_semantic_token(KW_MONTHLYMIN);         return KW_MONTHLYMIN;         }
"navigator"          { store_semantic_token(KW_NAVIGATOR);          return KW_NAVIGATOR;          }
"newtask"            { store_semantic_token(KW_NEWTASK);            return KW_NEWTASK;            }
"nikureport"         { store_semantic_token(KW_NIKUREPORT);         return KW_NIKUREPORT;         }
"note"               { store_semantic_token(KW_NOTE);               return KW_NOTE;               }
"novevents"          { store_semantic_token(KW_NOVEVENTS);          return KW_NOVEVENTS;          }
"now"                { store_semantic_token(KW_NOW);                return KW_NOW;                }
"number"             { store_semantic_token(KW_NUMBER);             return KW_NUMBER;             }
"numberformat"       { store_semantic_token(KW_NUMBERFORMAT);       return KW_NUMBERFORMAT;       }
"onend"              { store_semantic_token(KW_ONEND);              return KW_ONEND;              }
"onstart"            { store_semantic_token(KW_ONSTART);            return KW_ONSTART;            }
"opennodes"          { store_semantic_token(KW_OPENNODES);          return KW_OPENNODES;          }
"outputdir"          { store_semantic_token(KW_OUTPUTDIR);          return KW_OUTPUTDIR;          }
"overtime"           { store_semantic_token(KW_OVERTIME);           return KW_OVERTIME;           }
"period"             { store_semantic_token(KW_PERIOD);             return KW_PERIOD;             }
"persistent"         { store_semantic_token(KW_PERSISTENT);         return KW_PERSISTENT;         }
"precedes"           { store_semantic_token(KW_PRECEDES);           return KW_PRECEDES;           }
"priority"           { store_semantic_token(KW_PRIORITY);           return KW_PRIORITY;           }
"project"            { store_semantic_token(KW_PROJECT);            return KW_PROJECT;            }
"projectid"          { store_semantic_token(KW_PROJECTID);          return KW_PROJECTID;          }
"projectids"         { store_semantic_token(KW_PROJECTIDS);         return KW_PROJECTIDS;         }
"projection"         { store_semantic_token(KW_PROJECTION);         return KW_PROJECTION;         }
"prolog"             { store_semantic_token(KW_PROLOG);             return KW_PROLOG;             }
"purge"              { store_semantic_token(KW_PURGE);              return KW_PURGE;              }
"rate"               { store_semantic_token(KW_RATE);               return KW_RATE;               }
"rawhtmlhead"        { store_semantic_token(KW_RAWHTMLHEAD);        return KW_RAWHTMLHEAD;        }
"reference"          { store_semantic_token(KW_REFERENCE);          return KW_REFERENCE;          }
"remaining"          { store_semantic_token(KW_REMAINING);          return KW_REMAINING;          }
"replace"            { store_semantic_token(KW_REPLACE);            return KW_REPLACE;            }
"reportprefix"       { store_semantic_token(KW_REPORTPREFIX);       return KW_REPORTPREFIX;       }
"resource"           { store_semantic_token(KW_RESOURCE);           return KW_RESOURCE;           }
"resourceattributes" { store_semantic_token(KW_RESOURCEATTRIBUTES); return KW_RESOURCEATTRIBUTES; }
"resourceprefix"     { store_semantic_token(KW_RESOURCEPREFIX);     return KW_RESOURCEPREFIX;     }
"resourcereport"     { store_semantic_token(KW_RESOURCEREPORT);     return KW_RESOURCEREPORT;     }
"resourceroot"       { store_semantic_token(KW_RESOURCEROOT);       return KW_RESOURCEROOT;       }
"resources"          { store_semantic_token(KW_RESOURCES);          return KW_RESOURCES;          }
"responsible"        { store_semantic_token(KW_RESPONSIBLE);        return KW_RESPONSIBLE;        }
"richtext"           { store_semantic_token(KW_RICHTEXT);           return KW_RICHTEXT;           }
"right"              { store_semantic_token(KW_RIGHT);              return KW_RIGHT;              }
"rollupaccount"      { store_semantic_token(KW_ROLLUPACCOUNT);      return KW_ROLLUPACCOUNT;      }
"rollupresource"     { store_semantic_token(KW_ROLLUPRESOURCE);     return KW_ROLLUPRESOURCE;     }
"rolluptask"         { store_semantic_token(KW_ROLLUPTASK);         return KW_ROLLUPTASK;         }
"scale"              { store_semantic_token(KW_SCALE);              return KW_SCALE;              }
"scenario"           { store_semantic_token(KW_SCENARIO);           return KW_SCENARIO;           }
"scenarios"          { store_semantic_token(KW_SCENARIOS);          return KW_SCENARIOS;          }
"scenariospecific"   { store_semantic_token(KW_SCENARIOSPECIFIC);   return KW_SCENARIOSPECIFIC;   }
"scheduled"          { store_semantic_token(KW_SCHEDULED);          return KW_SCHEDULED;          }
"scheduling"         { store_semantic_token(KW_SCHEDULING);         return KW_SCHEDULING;         }
"schedulingmode"     { store_semantic_token(KW_SCHEDULINGMODE);     return KW_SCHEDULINGMODE;     }
"select"             { store_semantic_token(KW_SELECT);             return KW_SELECT;             }
"selfcontained"      { store_semantic_token(KW_SELFCONTAINED);      return KW_SELFCONTAINED;      }
"shift"              { store_semantic_token(KW_SHIFT);              return KW_SHIFT;              }
"shifts"             { store_semantic_token(KW_SHIFTS);             return KW_SHIFTS;             }
"shorttimeformat"    { store_semantic_token(KW_SHORTTIMEFORMAT);    return KW_SHORTTIMEFORMAT;    }
"sloppy"             { store_semantic_token(KW_SLOPPY);             return KW_SLOPPY;             }
"sortaccounts"       { store_semantic_token(KW_SORTACCOUNTS);       return KW_SORTACCOUNTS;       }
"sortjournalentries" { store_semantic_token(KW_SORTJOURNALENTRIES); return KW_SORTJOURNALENTRIES; }
"sortresources"      { store_semantic_token(KW_SORTRESOURCES);      return KW_SORTRESOURCES;      }
"sorttasks"          { store_semantic_token(KW_SORTTASKS);          return KW_SORTTASKS;          }
"start"              { store_semantic_token(KW_START);              return KW_START;              }
"startcredit"        { store_semantic_token(KW_STARTCREDIT);        return KW_STARTCREDIT;        }
"status"             { store_semantic_token(KW_STATUS);             return KW_STATUS;             }
"statussheet"        { store_semantic_token(KW_STATUSSHEET);        return KW_STATUSSHEET;        }
"statussheetreport"  { store_semantic_token(KW_STATUSSHEETREPORT);  return KW_STATUSSHEETREPORT;  }
"strict"             { store_semantic_token(KW_STRICT);             return KW_STRICT;             }
"summary"            { store_semantic_token(KW_SUMMARY);            return KW_SUMMARY;            }
"supplement"         { store_semantic_token(KW_SUPPLEMENT);         return KW_SUPPLEMENT;         }
"tagfile"            { store_semantic_token(KW_TAGFILE);            return KW_TAGFILE;            }
"task"               { store_semantic_token(KW_TASK);               return KW_TASK;               }
"taskattributes"     { store_semantic_token(KW_TASKATTRIBUTES);     return KW_TASKATTRIBUTES;     }
"taskprefix"         { store_semantic_token(KW_TASKPREFIX);         return KW_TASKPREFIX;         }
"taskreport"         { store_semantic_token(KW_TASKREPORT);         return KW_TASKREPORT;         }
"taskroot"           { store_semantic_token(KW_TASKROOT);           return KW_TASKROOT;           }
"text"               { store_semantic_token(KW_TEXT);               return KW_TEXT;               }
"textreport"         { store_semantic_token(KW_TEXTREPORT);         return KW_TEXTREPORT;         }
"timeformat"         { store_semantic_token(KW_TIMEFORMAT);         return KW_TIMEFORMAT;         }
"timeformat1"        { store_semantic_token(KW_TIMEFORMAT1);        return KW_TIMEFORMAT1;        }
"timeformat2"        { store_semantic_token(KW_TIMEFORMAT2);        return KW_TIMEFORMAT2;        }
"timeoff"            { store_semantic_token(KW_TIMEOFF);            return KW_TIMEOFF;            }
"timesheet"          { store_semantic_token(KW_TIMESHEET);          return KW_TIMESHEET;          }
"timesheetreport"    { store_semantic_token(KW_TIMESHEETREPORT);    return KW_TIMESHEETREPORT;    }
"timezone"           { store_semantic_token(KW_TIMEZONE);           return KW_TIMEZONE;           }
"timingresolution"   { store_semantic_token(KW_TIMINGRESOLUTION);   return KW_TIMINGRESOLUTION;   }
"title"              { store_semantic_token(KW_TITLE);              return KW_TITLE;              }
"tooltip"            { store_semantic_token(KW_TOOLTIP);            return KW_TOOLTIP;            }
"tracereport"        { store_semantic_token(KW_TRACEREPORT);        return KW_TRACEREPORT;        }
"trackingscenario"   { store_semantic_token(KW_TRACKINGSCENARIO);   return KW_TRACKINGSCENARIO;   }
"treelevel"          { store_semantic_token(KW_TREELEVEL);          return KW_TREELEVEL;          }
"vacation"           { store_semantic_token(KW_VACATION);           return KW_VACATION;           }
"warn"               { store_semantic_token(KW_WARN);               return KW_WARN;               }
"weeklymax"          { store_semantic_token(KW_WEEKLYMAX);          return KW_WEEKLYMAX;          }
"weeklymin"          { store_semantic_token(KW_WEEKLYMIN);          return KW_WEEKLYMIN;          }
"weekstartsmonday"   { store_semantic_token(KW_WEEKSTARTSMONDAY);   return KW_WEEKSTARTSMONDAY;   }
"weekstartssunday"   { store_semantic_token(KW_WEEKSTARTSSUNDAY);   return KW_WEEKSTARTSSUNDAY;   }
"width"              { store_semantic_token(KW_WIDTH);              return KW_WIDTH;              }
"work"               { store_semantic_token(KW_WORK);               return KW_WORK;               }
"workinghours"       { store_semantic_token(KW_WORKINGHOURS);       return KW_WORKINGHOURS;       }
"yearlyworkingdays"  { store_semantic_token(KW_YEARLYWORKINGDAYS);  return KW_YEARLYWORKINGDAYS;  }

    /* Identifier / keyword: letters, digits, '_', ':', high UTF-8 bytes.
     * '.' is NOT included; it is emitted as TK_DOT so that dotted dependency
     * paths (e.g. deliveries.start) can be resolved component-wise by the LSP. */
[a-zA-Z_\x80-\xFF][a-zA-Z0-9_:\x80-\xFF]* { return TK_IDENT; }

"{"     { return TK_LBRACE;   }
"}"     { return TK_RBRACE;   }
"["     { return TK_LBRACKET; }
"]"     { return TK_RBRACKET; }
"!"     { return TK_BANG;     }
"+"     { return TK_PLUS;     }
"-"     { return TK_MINUS;    }
"."     { return TK_DOT;      }
":"     { return TK_COLON;    }
","     { return TK_COMMA;    }
"%"     { return TK_PERCENT;  }
"$"     { return TK_DOLLAR;   }

.       { char msg[48];
          snprintf(msg, sizeof(msg), "unexpected character: '%s'", yytext);
          return TK_ERROR; }

%%
