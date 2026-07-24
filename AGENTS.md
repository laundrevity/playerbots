# Shared Project Protocol

This repository is one Git history within the laundrevity WoW local project.
Before editing, read:

1. `~/dev/wow-tbc-local/CLAUDE.md`
2. `~/dev/wow-tbc-local/AGENTS.md`
3. `~/dev/wow-tbc-local/CLAUDE_TO_CODEX.md`
4. `~/dev/wow-tbc-local/CODEX_TO_CLAUDE.md`

The central `AGENTS.md` defines the repository registry, single-writer
mailboxes, claims, handoffs, and verification requirements. It applies to
this repository even though the coordination files live in another checkout.

Write claims and handoffs to the central mailbox for your agent before
substantial work. Always name this repository, branch, base commit, and files.
Do not overlap another agent's active claim or commit unrelated dirty changes.
