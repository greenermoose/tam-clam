# P5: Dogfood (0.4.x)

**Status:** not started.

## Goal

Fred uses tam-clam as his everyday interactive shell, and its bugs get
found and fixed in daily use.

## Versions

- **0.4.0** Packaged in `tamlinux-packages`, delivered only through a
  recorded assembly and `tam-deploy`, like every Tamlinux component.
  Started from the terminal (for example as the terminal's command);
  Fred's login shell stays bash until he says otherwise.
- **0.4.x** Fixes from daily use, recorded in `bugs/`.

## Acceptance

- A week of daily use with no crash and no lost history.
- Every bug found has a regression test.

## Open points

- Which Tamlinux version first carries tam-clam is Fred's decision.
- Moving Fred's bash configuration (aliases, prompt, functions) is a
  migration step to plan with him.
