# Contributing

A pull request description has `AI:` and `Tested:` fields, and
`Human review:` when an AI agent wrote it (AGENTS.md, "Pull requests by
AI agents").

## Git workflow

These rules are permanent: they hold for people and for agents, and a task
never waives them implicitly. An agent follows them unless the user's
request says otherwise for that one task, and even then never the safety
rules below.

### `main`

- The integration and release branch. Releases are cut from it by hand, by
  the maintainer, from the release workflow.
- Nobody develops on `main`, and an agent never commits to it or pushes to
  it directly: changes arrive through a reviewed pull request.
- A release is manual. It is never started by a pull request, a merge or an
  agent.

### `stable/carplay-working`

- The last version the user personally verified in the actual BMW. It is the
  known-good fallback, and the annotated tag `carplay-working-YYYY-MM-DD`
  records the same commit.
- Never updated automatically, never by a merge, a rebase or a scheduled
  job. It moves only after the user explicitly approves it, after testing the
  recovered or new version in the car.
- Never force-pushed, never deleted, never recreated. Its tag is never moved
  or removed.

### Feature branches

- One dedicated branch per development task, always cut from `origin/main`:
  `feature/<short-task-name>` (`fix/<short-task-name>` for a fix). Do not
  reuse another task's branch.
- Never develop on `main` or on `stable/carplay-working`, and never mix two
  unrelated tasks in one branch.
- Before the first edit: `git status --short --branch`, `git rev-parse HEAD`
  and the remote's `stable/carplay-working` and tag, to know the baseline you
  start from. If the working tree has tracked modifications, staged changes
  or an unclear state, stop and report instead of repairing it.

### Testing and integration

- Review the whole diff before integration (`git diff`, `git diff --check`),
  run the task's tests and builds, and check GitHub Actions CI for the pull
  request.
- Successful CI is **not** vehicle validation. Only the user, in the car,
  turns a version into a known-good one. Never describe an untested version
  as verified, and never as known-good.
- A change that is shown on screen needs its screenshots or recording
  (AGENTS.md); never fabricate them, and say plainly what was not tested.

### Recovery

- A broken update is recovered with a new corrective commit or a revert
  commit on a new branch, through a pull request.
- Never rewrite history, never force-push, and never downgrade or reuse a
  published component version: the release gives the next version.
- `stable/carplay-working` does not move until the recovered version is
  physically verified in the car.

### Git safety

- No `git push --force`, no `git reset --hard`, no `git clean`, no deleting
  branches or tags that others may have, no destructive cleanup.
- Never stage unrelated untracked files, and never `git add -A`; stage the
  files of the task explicitly. Local IDE and diagnostic directories such as
  `.idea/` and `_audit/` are not part of the tree.
- Never run the Release workflow, publish to the mgu Store, merge a pull
  request or update `stable/carplay-working` without the user explicitly
  asking for it.

### Working efficiently

- Read the existing documentation (`AGENTS.md`, this file,
  `PROJECT_CONTEXT.md`, a component's `README.md`) before researching, and
  search the code for the path that matters instead of reading everything.
- Do not repeat an investigation that is already answered in the tree or in
  the logs without new evidence; do not refactor unrelated code; do not fix
  cosmetic issues merely because they were noticed.
- Spend the effort on implementation and tests, not on speculative analysis
  or duplicated documents: extend an existing document instead of adding a
  second one that says the same.

## Versions

Leave version numbers alone. The release gives each component it publishes
its next version and writes it into the tree. A pull request does not change
`"version"` in `component.json`, `mgu-app.json` or `package.json`, and an
app's package tree stays `apps/<id>/package/`. CI fails a pull request that
changes a version.

- **Patch** is the default.
- **Minor or major:** say so in the pull request; the maintainer adds the
  label `release:minor` or `release:major`. The label covers every component
  the pull request changes.

## CHANGELOG

A component with a CHANGELOG (an app's `package/CHANGELOG.md`) lists what
your pull request changes under `## Unreleased` at the top:

```md
# Changelog

## Unreleased

What changes, in a sentence or two for the people who use it.

## 0.6.2
...
```

The release moves those lines under the new version's heading and leaves an
empty `## Unreleased` for the next pull requests. CI fails a pull request
that changes such a component, its `release.json` included, without an
Unreleased entry.

When another pull request added its lines first, GitHub reports a conflict
in the CHANGELOG. Merge `main` into your branch locally
(`git merge origin/main`): the repository's `.gitattributes` keeps both
sets of lines, so there is nothing to resolve by hand.

## Requiring another component's next release

When your change needs a change to another component of this repository in
the same or an earlier pull request, require that component's next release
with `"next"` in `"requires"` (an app's `release.json`, a tool's
`component.json`):

```json
"requires": { "waze-on-cluster": "next" }
```

The release publishes both and writes the real version in. Keep a component
that is already released at its number (`"cluster-render": "0.4.0"`).

## New components

A new component starts at the version you give it, usually `0.1.0`; the
release publishes it as that.
