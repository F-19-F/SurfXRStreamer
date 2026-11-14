# Asking an agent to change the project

English | [简体中文](REQUESTS.md)

You describe the problem and the conditions for accepting a result. The agent reads the repository, locates ownership, implements, tests and explains the evidence. You do not need to know function names, design the protocol first or use a particular agent service.

Give the agent access to the checkout. A chat-only agent can help draft an issue, but a proposal in chat does not mean code was changed or tested.

## Seven parts of a useful requirement

| Part | What to include | Example |
|---|---|---|
| Scenario | Mode, devices and interaction | Desktop mode, two controllers, window dragging |
| Current behavior | What actually happens | A second-hand click moves the target being dragged |
| Expected behavior | Observable result | Drag remains with its original hand/panel until release |
| Scope | Work included | Fix input arbitration without adding shortcuts |
| Boundaries | Cancel, disconnect, tracking loss, restart | Release held input on focus loss; do not replay old clicks |
| Acceptance | How to judge success | Repeat the reproduction ten times without jumps or stuck buttons |
| Environment/authorization | Versions, tools, hardware and allowed actions | Local builds/offline tests allowed; user operates the headset |

These are examples of writing requirements, not claims about existing defects. Choose thresholds for the actual task; ask for a measurement plan when the baseline is unknown.

## Full requirement template

Fill in known facts and write "investigate first" for unknowns:

```text
Title:
Target user and scenario:
Current behavior or difficulty:
Expected behavior:
Entry point or display location:
Defaults, bounds and persistence:
Failure, cancellation, restart and disconnect behavior:
Compatibility with old settings/devices/versions:
Scope and explicit exclusions:

Environment:
- Source commit/release:
- PC OS, GPU and driver:
- Frame OS/runtime, or no headset available:
- Desktop/VR, link, codec, resolution, FPS and relevant settings:
- Build environment and SDK:

Acceptance:
1. Under ... conditions, performing ... should produce ... .
2. When ... fails, the result should be ... .
3. Existing ... behavior should remain ... .

Allowed actions: local edits/builds/offline tests; list other actions explicitly.
Deployment, service, network-device and publication constraints:
Evidence: sanitized steps, short logs and necessary screenshots, or none.
```

Separate unrelated goals into independently acceptable changes. Necessary changes at both ends of one protocol extension still belong to the same feature.

## Prompt A: implement a clear requirement

```text
Implement the following requirement in the SurfXR / SurfXR repository:
[Paste the completed template.]

Read AGENTS.md and surfxr/docs/ARCHITECTURE.md, TOOLS.md, PROTOCOL.md and
AGENT_GUIDE.en.md. Inspect the working tree and preserve unrelated changes.
State ownership, responsibility, affected interfaces and acceptance plan, then implement.
Follow DEVELOPMENT.en.md and TESTING.en.md, maintaining relevant consumers.
Reuse existing build/install/shader owners and keep generated output under root build/.
Maintain both endpoints of cross-PC/Frame changes and test configuration migrations.
Complete the authorized work, commit coherent validated changes, and report commit IDs,
actual results, untested areas and manual acceptance steps.
```

State the intended action scope. For local work only, say so. For deployment, identify the target, version, permitted service changes and recovery requirements. Existing explicit authorization remains applicable.

## Prompt B: investigate an unclear problem

```text
I observe this problem: [symptom, actions, frequency and environment].
Read repository rules and maintenance documents, then trace the production path.
Do not yet change runtime code, install, restart services or connect to a device.
Separate confirmed facts, hypotheses and missing evidence. Identify the owner and interfaces.
Use existing offline checks to narrow the cause. Propose the smallest repair and validation.
Ask only for missing information that affects the diagnosis.
```

An investigation is not an implementation. Once a repair is chosen, use prompt A and retain the findings.

## Prompt C: fix a reproducible bug

```text
Fix this reproducible issue:
Version/commit:
Preconditions:
Steps: 1. ... 2. ... 3. ...
Expected:
Actual:
Frequency:
Sanitized evidence:

Read AGENTS.md, architecture, tools, SurfXR and the agent guide, then locate the owner.
Extend an existing fixture with a regression that exercises the root cause.
Fix normal and relevant cancellation/disconnect/exit paths while preserving protocol,
configuration and resource contracts. Run required builds/tests and commit the change.
Report before/after evidence and outstanding device acceptance.
Authorized actions: ...
```

## Prompt D: performance or image quality

```text
Improve: [specific bottleneck or visible issue].
Fixed scenario: [resolution/FPS/bitrate/codec, hardware, driver, route and input].
Baseline: [measurements, method and duration, or measure first].
Goal and allowed variation:
Allowed tradeoffs: [quality, CPU/GPU, memory, power].
Scope limits: [for example color interpretation without changing spatial filtering].

Check metric definitions in existing telemetry before diagnosing the bottleneck.
Reuse timing, encoder/decoder and GPU fixtures. Compare identical conditions and preserve
queue bounds and GPU/decoder lifetimes. Report sample duration, method and limitations.
Do not claim a speed/quality improvement without evidence. Follow the agent guide,
complete the implementation and validation, then commit.
Authorized actions: ...
```

## Prompt E: review and prepare acceptance

```text
Review whether [commit/branch] satisfies [requirement].
Do not edit or deploy yet. Read the rules/contracts and inspect the diff and evidence.
Mark each criterion proven, pending, failed or not applicable, with a reason.
Separate builds, CPU/loopback, offscreen GPU, actual headset operation and wearer experience.
Check both endpoints, migration, cleanup and queue bounds.
Give executable manual acceptance steps: preconditions, action, expectation and failure evidence.
```

## Write observable acceptance criteria

Use "precondition → action → observation" instead of "works well". For a hypothetical local HUD setting:

| Scenario | Action | Expected observation |
|---|---|---|
| Default | Start with an isolated test configuration | Requested default, without invented remote acknowledgement |
| Edit | Change in Dashboard and open in-stream settings | Consistent value and actual behavior |
| Persistence | Exit normally and restart | Saved value remains effective |
| Bounds | Try minimum, maximum and out-of-range values | Defined bounds or explicit rejection, no crash |
| Invalid data | Inject invalid state in a fixture; perform agreed device checks | Defined hiding/error behavior, no stale result |
| Compatibility | Load a historical-format fixture | Old fields retained, new fields defaulted |
| Regression | Use existing HUD and related switches | Previously agreed behavior remains correct |

Do not delete user configuration to simulate first run. Use isolated fixtures or agree on backup/recovery first. Avoid disrupting a working device to simulate failures.

## Evaluate the delivery

Look for the owning module and rationale, affected endpoints/formats, commit ID, preservation of unrelated work, actual commands/environment/results/warnings, automated versus pending acceptance, and any installation/service/device actions. Build success or offscreen Vulkan success does not establish headset experience; historical validation does not replace current evidence.

For final acceptance, confirm the actual deployed PC/Frame versions, follow the agreed steps with fixed settings, record visuals/audio/input/comfort and duration as relevant, and capture the first failure with sanitized evidence. Identify which criteria passed, failed or remain untested.

```text
Accepted commit/package: ...; PC/Frame environment: ...; duration: ... .
Passed: A1, A2, A4. A3 untested because ... .
Failed: A5; steps: ...; actual: ...; expected: ...; sanitized evidence: ... .
Continue fixing A5 while preserving accepted behavior. Authorized actions remain: ... .
```

Remove credentials, pairing material, account/host identifiers, private desktop content and unrelated stream data from logs and screenshots. Retain relevant error codes, relative time, state and counters. Do not provide account passwords to simplify reproduction.
