#!/usr/bin/env bash
#
# PreToolUse gate for the Ghidra MCP tools (see .claude/skills/ghidra-recon).
#
# Two rules, both structural rather than advisory:
#
#   1. The main context may not call mcp__ghidra__* at all. Decompiler output is
#      enormous, and a main context that reads it directly runs out of room for
#      the work the analysis was for. Ghidra work belongs in a subagent, which
#      reports back facts instead of transcripts.
#
#   2. A ghidra-explorer may not call create_context - the read-write one. Any
#      number of explorers run in parallel, and the database is shared mutable
#      state with no merge: concurrent renames collide, one agent retypes a
#      global another is reading through, and createLabel deleting a Symbol
#      handle another holds is a race. create_readonly_context binds each one to
#      an immutable snapshot instead, which is what makes the fan-out safe.
#      Writes are the ghidra-consolidator's job, and it runs alone.
#
# Rule 2 is the one that used to be impossible. CLAUDE.md recorded that
# read-only "is not enforced on delegated work - a nested subagent renamed
# functions and globals despite being told not to", so fan-out had to be audited
# after the fact. Now the readonly context refuses the write at the Ghidra end,
# and this hook refuses the request for a writable one at this end.
#
# How it tells the callers apart: the hook input carries `agent_type` (and
# `agent_id`) only when the call comes from inside a subagent. Measured by
# logging the raw hook input from both sides:
#
#   main context     no agent_type, no agent_id
#   a subagent       agent_type = "<agent name>", agent_id = "a3814b17..."
#
# `transcript_path` is deliberately NOT the primary test. This hook used to
# match it against `*subagents*`, on the assumption that a subagent's transcript
# lives at <project>/<parent-session-id>/subagents/agent-<id>.jsonl. On this
# harness it does not - a subagent reports the *parent session's* transcript,
# byte for byte the same path the main context reports:
#
#   C:\Users\franc\.claude\projects\C--Users-franc-GkPlus\<session-id>.jsonl
#
# so the test never matched, and the hook denied the very analysts it exists to
# route work to - from every context, including a synchronous one, which cost a
# whole session of fan-out before anyone logged the input rather than reasoning
# about it. The path test is kept below as a fallback for a harness that does
# use the nested layout.
#
# It fails OPEN - if the input carries neither an `agent_type` nor a readable
# `transcript_path`, the call is allowed, and an unrecognised agent_type is
# allowed anything. This is a backstop behind the skill, and a backstop that
# guesses "main context" on unknown input would wedge the agents it exists to
# protect.

set -u

input=$(cat)
agent=$(printf '%s' "$input" | jq -r '.agent_type // ""' 2>/dev/null) || agent=""
tool=$(printf '%s' "$input" | jq -r '.tool_name // ""' 2>/dev/null) || tool=""
path=$(printf '%s' "$input" | jq -r '.transcript_path // ""' 2>/dev/null) || path=""

deny() {
  # $1 is the reason, and it is the only thing the caller sees - so it says what
  # to do instead, not just what went wrong.
  printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":%s}}\n' \
    "$(printf '%s' "$1" | jq -Rs .)"
  exit 0
}

# Rule 2: an explorer gets the read-only context and nothing else.
if [ "$agent" = "ghidra-explorer" ]; then
  case "$tool" in
    mcp__ghidra__create_context)
      deny "A ghidra-explorer may not open a read-write Ghidra context. Explorers run in parallel over one shared database, so they use mcp__ghidra__create_readonly_context and change nothing - the immutable snapshot is what makes the fan-out safe. Measure the area and report the edits you recommend, marked CONFIRMED or PROPOSED, in your 'Suggested database edits' and 'Suggested repo edits' sections. A ghidra-consolidator runs afterwards, alone, and applies them across the Ghidra DB, src/ and the notes."
      ;;
  esac
  exit 0
fi

# Any other subagent - this is where Ghidra work belongs.
[ -n "$agent" ] && exit 0

case "$path" in
  # Fallback for a harness that does give a subagent its own transcript file.
  # Matched without a separator on either side on purpose: this runs on Windows,
  # where the path arrives with backslashes, and a `*/subagents/*` pattern would
  # miss it and deny the very subagent this exists to route work to.
  *subagents*) exit 0 ;;
  "")          exit 0 ;;  # shape not recognised - fail open
esac

# Rule 1: the main context.
deny "Ghidra tools are not available in the main context - decompiler output would crowd out the work it informs. Invoke the ghidra-recon skill, which runs this as explore-then-consolidate: fan out ghidra-explorer subagents (read-only, any number in parallel) with self-contained briefs - the specific question, the area's boundary, what is already known and from where, why you are asking, and which fields or signatures the report needs. Read their reports, aim another round at any contradiction or gap, then run ghidra-consolidator subagents ONE AT A TIME to apply the findings to the Ghidra DB, src/ and the notes. If the repo already answers this - CLAUDE.md, the *_notes.md files, src/ - read that instead."
