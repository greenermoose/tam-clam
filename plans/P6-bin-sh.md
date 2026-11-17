# P6: /bin/sh (1.0.0)

**Status:** not started.

## Goal

tam-clam is fit to be a system's `/bin/sh`, the shell that runs boot
scripts, package hooks and `system(3)`.

## Versions

- **1.0.0** It runs every shell script on the Tamlinux workstation that
  names `/bin/sh`, including pacman hooks and makepkg, the full spec
  suite passes, and start-up is as fast as dash's.

## Acceptance

- Start-up time and memory measured against dash and bash (`tam-clam -c
  :` run 1,000 times).
- A full system boots and updates with tam-clam as `/bin/sh` on a spare
  machine, with a rollback point recorded first.

## Risks

- **A broken `/bin/sh` breaks the system.** Making tam-clam `/bin/sh`
  is a boot-affecting step: it runs one step at a time with Fred's
  approval and a recorded rollback point, and only on his Test, Run and
  Release words.

## Open points

- Whether Tamlinux ever makes tam-clam `/bin/sh` is Fred's decision.
