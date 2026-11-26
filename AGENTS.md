# idrive-apps

Tools and apps for the iDrive 7 head unit (MGU) of a BMW, installed from the
mgu Store. Versions and CHANGELOG rules: `CONTRIBUTING.md`.

## Git workflow: follow `CONTRIBUTING.md`

Read `CONTRIBUTING.md` before touching the tree, and follow its **Git
workflow** rules exactly: they define the branches (`main`,
`stable/carplay-working`, `feature/<short-task-name>`), what may never be
done to them (force-push, `reset --hard`, history rewrites, automatic
releases, merges without an explicit request from the user), how a change is
tested and reviewed, and how a broken update is recovered. They are part of
this file's instructions for agents.

In short, and never waived by a task:

- Work on a dedicated `feature/<short-task-name>` branch cut from
  `origin/main`; never develop on `main` or on `stable/carplay-working`.
- `stable/carplay-working` is the last version the user personally verified
  in the car. It moves only after the user says so, and is never
  force-pushed.
- CI passing is not vehicle validation: an untested version is never called
  known-good.
- Never stage unrelated untracked files (`.idea/`, `_audit/`), never
  `git add -A`, never `git clean`.
- Never run the Release workflow, publish to the Store, merge a pull
  request or change a component version unless the user asked for exactly
  that.

## Pull requests by AI agents

A pull request opened, or mostly written, by an AI agent says in its
description (`.github/pull_request_template.md` has the fields):

- **AI:** the product **and** the exact model name and version, for example
  "Claude Code, Claude Opus 5.5" or "Codex, GPT-6.1". "An AI agent", "an AI
  coding agent" or "AI-assisted" alone is not enough: name both. When several
  models worked on it, list each, with what it did if that helps.
- **Tested:** exactly how: which test suites ran and passed; whether it ran
  on a real car (which iDrive and firmware) or on a test unit; and what was
  not tested.
- **Human review:** whether a person reviewed it before it was opened.

A pull request without AI involvement says `AI: none`.

Every pull request that changes anything shown on screen (a page, screen,
dialog, tile, icon or text of the Store or an app) includes screenshots or a
screen recording of each changed screen, before and after, under
**Screenshots:**. A change to how something moves or reacts to input needs a
recording.
