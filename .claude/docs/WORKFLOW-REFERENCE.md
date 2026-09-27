# Agent workflow

A feature goes through three stages, each in its own conversation so that
context stays clean. Michael writes the specification with an assistant,
a planning coordinator turns it into a phased plan, and an implementation
orchestrator implements the plan through implementation and review
subagents, committing each accepted phase.

```text
PLANS/<feature>.md                 the specification (behaviour, boundaries,
                                   tests, documentation, Commit Policy)
        |
        v  planning-coordinator.md  -> spawns phase-documenter.md per phase
PLANS/implementation-plan/
    00-overview.md                 spec verbatim, phases, references,
                                   decisions, Commit Policy
    NN-<phase>.md                  one per phase
        |
        v  implementation-orchestrator.md
           -> implementation-agent.md, review-agent.md per phase,
              final reviews, one commit and tag per accepted phase
        |
        v  Michael reviews and pushes; the spec and plan move to PLANS/done/
           and are committed on their own
```

## Files

```text
.claude/prompts/planning-coordinator.md       stage 2 coordinator
.claude/prompts/phase-documenter.md           one phase document
.claude/prompts/implementation-orchestrator.md stage 3 coordinator
.claude/prompts/implementation-agent.md       writes the code of one phase
.claude/prompts/review-agent.md               reviews one phase, or the whole
CLAUDE.md                                     project facts every agent loads
CLAUDE.local.md                               this machine's facts (untracked)
```

## Starting each stage

Planning, in a fresh conversation:

```text
Follow .claude/prompts/planning-coordinator.md
Specification: PLANS/<feature>.md
```

Implementation, in a fresh conversation:

```text
Follow .claude/prompts/implementation-orchestrator.md
Plan: PLANS/implementation-plan/
```

Resuming an interrupted implementation:

```text
Continue as implementation orchestrator per .claude/prompts/implementation-orchestrator.md
Plan: PLANS/implementation-plan/
Done so far: <phases committed, with tags>; resume from <phase>.
```

## Conventions

- **The specification** says what the change must do and the boundaries it
  must respect, at the level of behaviour and architecture. It names a
  function or class only when the name is part of the observable contract.
  It carries no planner notes, build-machine details or status history. It
  ends with a Commit Policy section, which is Michael's standing
  authorization to commit for that plan and names the branch, the commit
  subject format and the tag format. Specifications and archived plans
  are committed on their own, never in a phase commit.
- **The plan** keeps the same altitude inside a phase and is explicit only
  at the interfaces between phases, where two agents working from different
  documents must agree. The overview carries the specification verbatim and
  the Commit Policy verbatim.
- **Coordinators do not write code**, and only the implementation
  orchestrator changes repository state. Subagents get everything they need
  by path and read it in full; a coordinator passes paths, not pasted
  contents, and keeps its own context for coordination.
- **A review builds and runs.** A review that did not build the tree and run
  the relevant tests is not a review.
- **Every phase leaves the tree green**: build, tests, the audit and the
  acceptance map. The documentation that describes changed behaviour changes
  with it.
- **Iteration is bounded**: three review cycles per phase, then escalation
  to Michael with the phase uncommitted.

## Models

The stages are run with Opus 5.5 at high effort; subagents inherit the
session's model unless a spawn says otherwise. The prompts assume an agent
that reads files reliably, keeps working until the task is done, and
reports what it did without being handed a template.
