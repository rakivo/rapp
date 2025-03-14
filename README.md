# [rapp](https://github.com/rakivo/rapp/tree/master)

![](https://raw.githubusercontent.com/rakivo/rapp/refs/heads/master/assets/preview.png)

# Quick start
> if you're not on X11, the program won't compile / work for you.
```console
$ rush -t release
```
> if you're not using [rush](https://github.com/rakivo/rush) ... kinda nah but ok do `./build.sh`

> run it
```console
$ ./build/rapp-release
```

# Details
> If the amount of matching apps does not fit into the window, you will see a scrollbar at the right, it's clickable and draggable (who would've thought?).

> [rapp](https://github.com/rakivo/rapp/tree/master) supports basic emacs-motions, specifically:
- `DELETE`
- `yank`
- `backward-kill-word`
- `delete-char`
- `kill-whole-line`
- `kill-line`
- `kill-word`
- `move-beginning-of-line`
- `move-end-of-line`
- `backward-char`
- `backward-word`
- `forward-char`
- `forward-word`
- `next-line`
- `previous-line`

# History
> I was not satisfied with the application finder I had, because it did not rank apps by the amount of times I have already launched them with it.

> so I created [rapp](https://github.com/rakivo/rapp/tree/master) as a weekend project, implemented the frequency ranking thing, made it as simple as possible, and made the [`naysayer`](https://github.com/nickav/naysayer-theme.el) theme default, because it's the theme I use in my editor and am comfortable with.
