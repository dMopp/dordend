# dordend

dordend is [barrulus/bordend-fm](https://github.com/barrulus/bordend-fm) plus a few topic
branches of my own. `main` is never edited by hand: `scripts/dordend-rebuild` builds it again
from bordend's current `main` and the topics, so every topic always sits on bordend's latest
state.

## Topics

Each topic is a branch based directly on `upstream/main` (bordend), so it builds and can be
offered upstream on its own:

| Branch | What |
|---|---|
| `stack/dordend-tools` | this file and the rebuild script |
| `stack/text-rendering` | text rendered like Pango: fontconfig hinting, subpixel positioning, trak |
| `stack/remote-bookmarks` | `smb://` and `sftp://` bookmarks mounted through gvfs, Ctrl+L "Go to Folder or Server" |
| `stack/german` | German translation, picked from the locale |

Glue commits that only make sense once several topics are merged live on their own branch,
rebased onto the merged topics at every rebuild; `<branch>-base` marks where they start:

| Branch | Needs | What |
|---|---|---|
| `stack/german-remotes` | remote-bookmarks + german | German for the network folder texts |

## Rebuilding

```sh
scripts/dordend-rebuild           # fetch, rebase the topics, rebuild main, run the tests
scripts/dordend-rebuild --push    # the same, then force-push main and the topics to origin
```

The script stops at the first conflict it cannot solve. Fix it the usual git way (`git add`,
`git rebase --continue` or `git commit`), then run it again. `git rerere` is switched on, so a
conflict solved once is solved by itself the next time.

## Remotes

- `origin`: dMopp/dordend (this repo)
- `upstream`: barrulus/bordend-fm
- `pankaspe`: pankaspe/nordend-fm (the original Nordend)
