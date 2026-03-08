use crate::parser::{Lexer, Token, TokenKind};
use tower_lsp::lsp_types::Position;

/// Find the token whose range contains `pos`, if any.
pub fn token_at(src: &str, pos: Position) -> Option<Token> {
    let mut lexer = Lexer::new(src);
    loop {
        let tok = lexer.next_token();
        if tok.kind == TokenKind::Eof {
            break;
        }
        // Stop scanning once we've passed the requested position.
        if tok.start.line > pos.line
            || (tok.start.line == pos.line && tok.start.character > pos.character)
        {
            break;
        }
        if pos_in(pos, tok.start, tok.end) {
            return Some(tok);
        }
    }
    None
}

fn pos_in(pos: Position, start: Position, end: Position) -> bool {
    let after = pos.line > start.line
        || (pos.line == start.line && pos.character >= start.character);
    let before = pos.line < end.line
        || (pos.line == end.line && pos.character <= end.character);
    after && before
}

/// Return Markdown documentation for a TaskJuggler v3 keyword, if known.
pub fn keyword_docs(kw: &str) -> Option<&'static str> {
    Some(match kw {
        // ── Declarations ────────────────────────────────────────────────────
        "project" => "\
**`project`** *(declaration)*

Defines the project header. Required exactly once per TJP file.

**Syntax:** `project <id> \"<name>\" <start> <end|duration> { … }`

**Example:**
```
project acso \"Accounting Software\" 2024-01-01 +6m { }
```",

        "task" => "\
**`task`** *(declaration)*

Defines a work package, summary task, or milestone. Tasks may be nested \
to form a work-breakdown structure.

**Syntax:** `task <id> \"<name>\" { … }`

**Common body attributes:** `effort`, `duration`, `depends`, `allocate`, \
`milestone`, `start`, `end`, `priority`, `complete`",

        "resource" => "\
**`resource`** *(declaration)*

Defines a resource (person, team, or piece of equipment) that can be \
allocated to tasks.

**Syntax:** `resource <id> \"<name>\" { … }`

**Common body attributes:** `rate`, `workinghours`, `vacation`, `leaves`, \
`efficiency`, `limits`",

        "account" => "\
**`account`** *(declaration)*

Defines a cost/revenue account used for project accounting.

**Syntax:** `account <id> \"<name>\" { … }`",

        "shift" => "\
**`shift`** *(declaration)*

Defines a named set of working hours that can be assigned to resources or tasks.

**Syntax:** `shift <id> \"<name>\" { workinghours … }`",

        "macro" => "\
**`macro`** *(declaration)*

Defines a reusable text macro. The body is enclosed in `[ … ]` and may \
contain `${1}`, `${2}`, … placeholders for arguments.

**Syntax:** `macro <id> [ <body> ]`",

        "include" => "\
**`include`** *(directive)*

Includes another TaskJuggler file at this point in the project.

**Syntax:** `include \"<filename>\"`",

        "flags" => "\
**`flags`** *(declaration)*

Declares one or more flag identifiers that can later be set on tasks, \
resources, or accounts and used in reports.

**Syntax:** `flags <id> [, <id> …]`",

        "supplement" => "\
**`supplement`** *(declaration)*

Adds attributes to a previously-defined task or resource without \
redefining it. Useful when splitting definitions across included files.

**Syntax:** `supplement task|resource <id> { … }`",

        // ── Task attributes ─────────────────────────────────────────────────
        "effort" => "\
**`effort`** *(task attribute)*

The amount of work required to complete the task, expressed in time units. \
The scheduler distributes this across allocated resources.

**Syntax:** `effort <value><unit>`

**Units:** `min`, `h`, `d` (days), `w` (weeks), `m` (months)

**Example:** `effort 5d`",

        "duration" => "\
**`duration`** *(task attribute)*

The elapsed (wall-clock) duration of the task, regardless of resource \
availability or working hours.

**Syntax:** `duration <value><unit>`

**Units:** `min`, `h`, `d`, `w`, `m`",

        "length" => "\
**`length`** *(task attribute)*

The duration of the task measured in working days/hours, as opposed to \
calendar time (`duration`).

**Syntax:** `length <value><unit>`",

        "milestone" => "\
**`milestone`** *(task attribute)*

Marks the task as a milestone (zero duration). A milestone represents a \
point-in-time event rather than a work package.

**Syntax:** `milestone`",

        "depends" => "\
**`depends`** *(task attribute)*

Specifies finish-to-start dependencies on other tasks. The current task \
cannot start until all listed tasks have finished.

**Syntax:** `depends <task-id> [{ gap <duration> }] [, …]`

**Example:** `depends !design, !prototype`",

        "precedes" => "\
**`precedes`** *(task attribute)*

The reverse of `depends`: lists tasks that must not start before this \
task finishes.

**Syntax:** `precedes <task-id> [, …]`",

        "allocate" => "\
**`allocate`** *(task attribute)*

Assigns one or more resources to the task for scheduling purposes.

**Syntax:** `allocate <resource-id> [{ … }] [, …]`

**Example:** `allocate alice, bob`",

        "start" => "\
**`start`** *(task / project attribute)*

Pins the start date of a task or defines the project start date.

**Syntax:** `start <YYYY-MM-DD[-HH:MM]>`",

        "end" => "\
**`end`** *(task / project attribute)*

Pins the end date of a task or defines the project end date.

**Syntax:** `end <YYYY-MM-DD[-HH:MM]>`",

        "maxstart" => "\
**`maxstart`** *(task attribute)*

Sets a latest-allowed start date. The scheduler reports an error if the \
task would start later than this date.

**Syntax:** `maxstart <YYYY-MM-DD>`",

        "minstart" => "\
**`minstart`** *(task attribute)*

Sets an earliest-allowed start date.

**Syntax:** `minstart <YYYY-MM-DD>`",

        "maxend" => "\
**`maxend`** *(task attribute)*

Sets a latest-allowed end date.

**Syntax:** `maxend <YYYY-MM-DD>`",

        "minend" => "\
**`minend`** *(task attribute)*

Sets an earliest-allowed end date.

**Syntax:** `minend <YYYY-MM-DD>`",

        "priority" => "\
**`priority`** *(task attribute)*

Sets the scheduling priority. Higher values (0–1000, default 500) cause \
the task to be scheduled before lower-priority tasks when resources are \
contested.

**Syntax:** `priority <0–1000>`",

        "complete" => "\
**`complete`** *(task attribute)*

The percentage of the task that is already done (0–100). Used in progress \
tracking and reports.

**Syntax:** `complete <0–100>`",

        "note" => "\
**`note`** *(task / resource attribute)*

Attaches a free-text note to the task or resource.

**Syntax:** `note \"<text>\"`",

        "responsible" => "\
**`responsible`** *(task attribute)*

Names the resource(s) accountable for completing the task. Informational \
only; does not affect scheduling.

**Syntax:** `responsible <resource-id> [, …]`",

        "booking" => "\
**`booking`** *(task attribute)*

Records actual work already performed on a task by a resource on a \
specific date, used for progress tracking.

**Syntax:** `booking <resource-id> <date> <duration> { … }`",

        "scheduled" => "\
**`scheduled`** *(task attribute)*

Marks a task as having fixed, pre-computed start and end dates that the \
scheduler should not change.

**Syntax:** `scheduled`",

        // ── Resource attributes ──────────────────────────────────────────────
        "rate" => "\
**`rate`** *(resource attribute)*

The daily cost rate of the resource in the project currency, used for \
cost calculations.

**Syntax:** `rate <number>`",

        "efficiency" => "\
**`efficiency`** *(resource attribute)*

A multiplier (default 1.0) applied to the resource's work output. \
`efficiency 0.5` means the resource works at half speed.

**Syntax:** `efficiency <number>`",

        "vacation" => "\
**`vacation`** *(resource / project attribute)*

Defines a vacation period during which the resource is unavailable (or \
a project-wide holiday).

**Syntax:** `vacation \"<name>\" <date> [+<duration> | - <date>]`",

        "leaves" => "\
**`leaves`** *(resource / project attribute)*

Defines a leave block (holiday, sick leave, etc.) similar to `vacation` \
but with a typed category.

**Syntax:** `leaves <type> <date> [+<duration>]`

**Types:** `annual`, `sick`, `special`, `unpaid`",

        // ── Project attributes ────────────────────────────────────────────────
        "now" => "\
**`now`** *(project attribute)*

Sets the reference date used as \"today\" in reports and progress \
calculations. Defaults to the current date.

**Syntax:** `now <YYYY-MM-DD>`",

        "currency" => "\
**`currency`** *(project attribute)*

Sets the currency symbol displayed in cost/revenue reports.

**Syntax:** `currency \"<symbol>\"`

**Example:** `currency \"EUR\"`",

        "timeformat" => "\
**`timeformat`** *(project attribute)*

Sets the `strftime`-style format string used when displaying dates.

**Syntax:** `timeformat \"<format>\"`

**Example:** `timeformat \"%Y-%m-%d\"`",

        "timezone" => "\
**`timezone`** *(project attribute)*

Sets the timezone for all date calculations. Uses IANA tz database names.

**Syntax:** `timezone \"<tz>\"`

**Example:** `timezone \"America/New_York\"`",

        "workinghours" => "\
**`workinghours`** *(project / resource / shift attribute)*

Defines which hours of the day and days of the week are working time.

**Syntax:** `workinghours <days> <HH:MM> - <HH:MM> [, …]`

**Days:** `mon`, `tue`, `wed`, `thu`, `fri`, `sat`, `sun`, `mon-fri`, etc.

**Example:** `workinghours mon-fri 9:00 - 17:00`",

        "timingresolution" => "\
**`timingresolution`** *(project attribute)*

Sets the scheduling granularity in minutes (default 60). Smaller values \
give finer-grained schedules at the cost of longer computation.

**Syntax:** `timingresolution <minutes>`",

        "scenario" => "\
**`scenario`** *(project attribute)*

Defines a scheduling scenario for what-if analysis. The default scenario \
is always `plan`.

**Syntax:** `scenario <id> \"<name>\" { … }`",

        _ => return None,
    })
}
