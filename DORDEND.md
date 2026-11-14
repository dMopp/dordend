# dordend

dordend is [barrulus/bordend-fm](https://github.com/barrulus/bordend-fm) plus a few changes of my
own, all directly on `main`:

- text rendered like Pango: fontconfig hinting, subpixel positioning, trak
- `smb://` and `sftp://` bookmarks mounted through gvfs, Ctrl+L "Go to Folder or Server"
- German translation, picked from the locale
- optional PDF previews: `rummage-pdf`, PDFium (pdfium-binaries) in the helper cage (`-DRUMMAGE_PDF=ON`,
  `.#rummage-with-previews`)

## Updating from bordend

```sh
git fetch upstream
git merge upstream/main
```

Solve any conflicts, run the tests, then `git push origin main`. `git rerere` is switched on, so a
conflict solved once is solved by itself the next time.

## Remotes

- `origin`: dMopp/dordend (this repo)
- `upstream`: barrulus/bordend-fm
- `pankaspe`: pankaspe/nordend-fm (the original Nordend)
