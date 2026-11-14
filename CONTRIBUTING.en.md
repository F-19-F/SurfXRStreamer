# Contributing

English | [简体中文](CONTRIBUTING.md)

Issues, documentation improvements, regression tests and feature changes are welcome. You may use your own coding agent. You do not need to understand every implementation detail before describing a use case, expected behavior and acceptance criteria. Agent assistance does not change review standards: contributors remain responsible for understanding their changes and evidence.

This guide covers collaboration. [AGENTS.md](AGENTS.md) defines repository rules; [architecture](surfxr/docs/ARCHITECTURE.md) and [SurfXR contracts](surfxr/docs/PROTOCOL.md) define interfaces. Maintainers preparing a public repository should complete [open-source preparation](surfxr/docs/OPEN_SOURCE.en.md). Public contributors use ordinary branches and pull requests.

## Choose an entry point

| Task | Start with | Expected result |
|---|---|---|
| Request a feature without writing code | [Requirements and prompts](surfxr/docs/REQUESTS.en.md) | Scenario, actions and observable expectations |
| Report a bug | [Bug template](.github/ISSUE_TEMPLATE/bug_report.md) | Version, reproduction, expected/actual result and sanitized evidence |
| Work with a coding agent | [Agent guide](surfxr/docs/AGENT_GUIDE.en.md) | Ownership, implementation, validation and commit IDs |
| Develop a feature | [Development guide](surfxr/docs/DEVELOPMENT.en.md) | Focused code, relevant tests and documentation |
| Contribute without a headset | [Getting started](surfxr/docs/GETTING_STARTED.en.md) | Documentation, CPU/loopback checks or PC/Linux builds |
| Review a pull request | [Testing and acceptance](surfxr/docs/TESTING.en.md) | Evidence tied to that commit and environment |

## Before submitting a requirement

Every repair report must explain the cause, evidence, concrete repair mechanism and normal-use cost; "fixed" alone is insufficient. Do not cure an intermittent bug by noticeably degrading every normal interaction. Minimize unnecessary gates that assume ideal conditions. Follow the [maintenance policy](surfxr/docs/AGENT_GUIDE.en.md) and [gate inventory](surfxr/docs/GATE_REVIEW.md).

Search existing issues, documentation and source first. Until a hosting platform is selected, the issue templates can also be used in your own requirement records. Chinese and English are both supported.

Describe the actual problem without guessing which function should change. For example, describe how a second controller click moves a window being dragged, rather than asking to rewrite an input module. Include minimal reproducible steps, frequency and observation time for intermittent problems.

Changes to protocols, saved configuration, GPU lifetimes or installation need a design and compatibility statement in the requirement or PR. Routine fixes within the authorized scope do not require another approval step. New behavior outside that scope, breaking migrations and real-device operations must be explained and authorized. Do not assume access to maintainer hardware, remote hosts or private build environments.

## Development workflow

1. Obtain the source from the actual published location and work on your branch or fork, with one coherent objective per branch.
2. Inspect `git status --short` and the current commit. Preserve existing work. Use an isolated worktree if needed, with its own root `build/`.
3. Read `AGENTS.md`, architecture, tools and SurfXR contracts. State the owning module, responsibility and affected interfaces.
4. Define observable behavior, defaults, boundaries, failure/cancellation paths, persistence and compatibility. Identify unresolved assumptions.
5. Implement in the existing owner. Avoid unrelated refactoring, broad imported-code formatting or parallel orchestration tools.
6. Select checks from the [test matrix](surfxr/docs/TESTING.en.md). Extend existing fixtures when useful; tests should detect real regressions rather than copy the implementation.
7. Update affected documentation and both language editions. Stable behavior belongs in contracts; keep historical evidence in its original context.
8. Freeze source edits, run final checks, inspect the diff, stage only intended files and commit. If source changes again, repeat affected checks.
9. Submit a PR using the [template](.github/pull_request_template.md), including untested areas and any missing conditions.

## Commits and pull requests

Each commit should be understandable, verifiable and independently revertible. Keep PC, Frame and shared changes for an SurfXR extension together. Larger work may separate behavior-preserving preparation from feature changes, but every commit needs relevant validation.

Before committing, at least run:

```powershell
python scripts/build/check_structure.py
git diff --check
git diff --stat
git status --short
```

Stage explicit paths, then inspect `git diff --cached` and `git diff --cached --check`. Avoid staging unrelated work. Commit messages should describe concrete behavior. Report the result of `git rev-parse HEAD` when delivering work.

A PR should explain the triggering problem and resulting behavior, owning modules and affected interfaces, actual commands/environment/results, and material limitations. Identify untested device decoding, real XR rendering and wearer experience. Include deployment, migration, permissions, user-data preservation and rollback details when relevant. A full agent conversation is not a substitute for a design and validation summary.

## Review and acceptance

Review ownership, compatibility, failure/shutdown paths, queue bounds, resource lifetimes and configuration migration. Tests should exercise production code. Documentation-only changes need structure, links, commands and diff checks, not unrelated full builds.

Missing hardware may be reported as an explicit validation boundary. Passing CPU models does not establish headset experience. If acceptance requires hardware, leave those checks pending for a contributor with the required environment.

Fix problems within the same objective and update the PR to describe the final implementation. Do not remove assertions, relax hash verification, skip failing tests or expand imported-file exceptions merely to get a passing result.

## Data, licenses and release boundaries

Review the diff, attachments and history for private data. Do not commit pairing records, keys, user settings, raw captures, desktop recordings, dependency caches or build products. Public logs should contain only necessary sanitized excerpts.

Project-owned code and documentation use the root [GNU GPL v3 (GPL-3.0-only)](LICENSE). Preserve imported licenses, attribution and provenance; see [NOTICE.md](NOTICE.md) for third-party terms. Record the source, version, purpose and license location of new dependencies or copied code, and verify that distributions carry the project GPL-3.0-only license and relevant third-party notices.

This guide does not establish a CLA, DCO, maintainer roster, response deadline or automatic merge policy. See [open-source preparation](surfxr/docs/OPEN_SOURCE.en.md) for licensing scope and remaining release work. A contribution does not itself authorize installation, service restarts, headset connections or public release; previously authorized operations retain their agreed scope.
