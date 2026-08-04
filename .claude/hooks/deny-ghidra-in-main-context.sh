#!/usr/bin/env bash
#
# PreToolUse gate for the Ghidra MCP tools (see .claude/skills/ghidra-recon).
#
# Decompiler output is enormous, and a main context that reads it directly runs
# out of room for the work the analysis was for. So the Ghidra tools are denied
# here and allowed in a `ghidra-analyst` subagent, which reports back facts
# instead of transcripts.
#
# How it tells the two apart: the hook input carries `agent_type` (and
# `agent_id`) only when the call comes from inside a subagent. Measured by
# logging the raw hook input from both sides:
#
#   main context     no agent_type, no agent_id
#   ghidra-analyst   agent_type = "ghidra-analyst", agent_id = "a3814b17..."
#
# `transcript_path` is deliberately NOT the primary test. This hook used to
# match it against `*subagents*`, on the assumption that a subagent's transcript
# lives at <project>/<parent-session-id>/subagents/agent-<id>.jsonl. On this
# harness it does not — a subagent reports the *parent session's* transcript,
# byte for byte the same path the main context reports:
#
#   C:\Users\franc\.claude\projects\C--Users-franc-GkPlus\<session-id>.jsonl
#
# so the test never matched, and the hook denied the very analysts it exists to
# route work to — from every context, including a synchronous one, which cost a
# whole session of fan-out before anyone logged the input rather than reasoning
# about it. The path test is kept below as a fallback for a harness that does
# use the nested layout.
#
# It fails OPEN — if the input carries neither an `agent_type` nor a readable
# `transcript_path`, the call is allowed. This is a backstop behind the skill,
# and a backstop that guesses "main context" on unknown input would wedge the
# analyst it exists to protect.

set -u

input=$(cat)
agent=$(printf '%s' "$input" | jq -r '.agent_type // ""' 2>/dev/null) || agent=""
path=$(printf '%s' "$input" | jq -r '.transcript_path // ""' 2>/dev/null) || path=""

# Inside a subagent - this is where Ghidra work belongs.
[ -n "$agent" ] && exit 0

case "$path" in
  # Fallback for a harness that does give a subagent its own transcript file.
  # Matched without a separator on either side on purpose: this runs on Windows,
  # where the path arrives with backslashes, and a `*/subagents/*` pattern would
  # miss it and deny the very subagent this exists to route work to.
  *subagents*) exit 0 ;;
  "")          exit 0 ;;  # shape not recognised - fail open
esac

cat <<'JSON'
{
  "hookSpecificOutput": {
    "hookEventName": "PreToolUse",
    "permissionDecision": "deny",
    "permissionDecisionReason": "Ghidra tools are not available in the main context - decompiler output would crowd out the work it informs. Invoke the ghidra-recon skill, then delegate this to a ghidra-analyst subagent (Agent tool, subagent_type: ghidra-analyst, run_in_background: false) with a self-contained brief: the specific question, what is already known and from where, why you are asking, and which fields or signatures the report needs. Its report comes back without the transcript. If the repo already answers this - CLAUDE.md, the *_notes.md files, src/ - read that instead."
  }
}
JSON
exit 0
