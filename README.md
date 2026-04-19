# flec

**FL**ac **E**ditor in **C**. A terminal UI for editing FLAC file metadata.

## Dependencies

- [libFLAC](https://xiph.org/flac/)
- [ncurses](https://invisible-island.net/ncurses/)
- [fzf](https://github.com/junegunn/fzf) *(optional)*

## Build

```console
$ cc -o nob nob.c
$ ./nob
```

## Usage

```console
$ ./flec [file.flac]
$ ./flec -m            # restrict fzf search to ~/Music
```

If no file is given, fzf is launched to pick one interactively.

## Keybinds

| Key                | Action                     |
|--------------------|----------------------------|
| `j` / `k` / arrows | Navigate fields            |
| `Enter` / `e`      | Edit field                 |
| `Escape`           | Cancel edit                |
| `Ctrl+S`           | Save                       |
| `Ctrl+X`           | Discard pending cover path |
| `q`                | Quit                       |

## Nix

To automatically run it:

```console
$ nix run github:tichelmorres/flec
```

If using a flake.nix, add flec as an input as follows.

```nix
flec = {
  url = "github:tichelmorres/flec";
  inputs.nixpkgs.follows = "nixpkgs";
};
```
