# flec

**FL**ac **E**ditor in **C**.

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

Run with no arguments to browse all FLAC files under `$HOME` with fzf:

```console
$ ./flec
```

Show help message:

```console
$ ./flec -h
```

Narrow the fzf search to one or more specific directories:

```console
$ ./flec -f "~/Music"
$ ./flec -f "~/Downloads" "~/Music" "/home/user/My Songs"
```

Also specify where fzf should look for cover images:

```console
$ ./flec -f "~/Music" -c "~/Pictures" "/home/user/CoverArts"
```

Open a file directly, skipping fzf file selection:

```console
$ ./flec -o "~/Music/Pink Floyd/Time.flac"
```

This can be combined with other flags:

```console
$ ./flec -o "~/Music/Pink Floyd/Time.flac" -c "~/Pictures"
```

Disable fzf entirely (enter NO-FZF mode) with `-nf`. This makes typing paths manually mandatory at every prompt:

```console
$ ./flec -nf
```

NO-FZF mode can also be combined with `-o` to skip the initial path prompt:

```console
$ ./flec -nf -o "~/Music/Pink Floyd/Time.flac"
```

flec contains an album editor for shared metadata across FLAC files in a directory tree.

```console
$ ./flec -a -o "~/Music/Pink Floyd/The Wall"
```

The album editor can also be combined with `-nf` for manual directory entry:

```console
$ ./flec -a -nf
```

## Flags

| Flag                       | Description                                                              |
|----------------------------|--------------------------------------------------------------------------|
| `-f`, `--flac` `<dir>...`  | Directories to search for FLAC files (multiple paths allowed)            |
| `-c`, `--cover` `<dir>...` | Directories to search for cover arts (multiple paths allowed)            |
| `-o`, `--open` `<path>`    | Open a FLAC file directly; in album mode, this must be a directory       |
| `-a`, `--album`            | Edit Artist / Album / Date / Cover Art for all FLACs in a directory tree |
| `-nf`, `--no-fzf`          | Use manual path input instead (NO-FZF mode)                              |
| `-h`, `--help`             | Print help information and exit                                          |

In fact, all flags can be combined in any order.

NOTE: If `-f` is omitted, fzf searches `$HOME` by default. The same applies to cover image selection if `-c` is omitted.

## FZF mode vs NO-FZF mode

flec has two operating modes that affect how file and image paths are resolved.

The standard FZF mode requires [fzf](https://github.com/junegunn/fzf) to be installed and uses it as an interactive fuzzy picker in startup for searching for `.flac` files, the `R` keybind (reselecting the `.flac` file or album directory), and the `Cover Art` field, which opens fzf to browse for image files.

The alternative NO-FZF mode (enabled by `-nf` / `--no-fzf`) removes all fzf dependency and replaces every picker with direct path input, including album-directory selection.

## Single-file editor vs Album editor

flec supports two editing scopes that determine how metadata changes are applied.

The standard single-file editor works on one FLAC file at a time. It includes the `Title` field, and every change applies only to the selected file. This is the default behavior when `-a` / `--album` is not used.

The album editor works on a directory instead of a single file. In this mode, `Title` is omitted, since each track keeps its own name, and the saved changes made to shared fields (`Artist`, `Album`, `Date`, and `Cover Art`) are applied to every FLAC file found in the selected folder tree. The search includes the root folder and up to two subdirectory levels below it.

When album mode is enabled, `-o` / `--open` must point to a directory rather than a file. The same rule applies to NO-FZF mode, flec asks for a directory path manually instead of a FLAC file path.

## Keybinds

| Key                 | Action                                         |
|---------------------|------------------------------------------------|
| `j` / `k` / arrows  | Navigate fields                                |
| `Enter` / `e` / `a` | Edit field                                     |
| `Escape`            | Cancel edit                                    |
| `Ctrl+S`            | Save                                           |
| `Ctrl+X`            | Discard pending cover path                     |
| `r`                 | Search another file or album directory to edit |
| `q`                 | Quit                                           |

Keybinds are case insensitive.

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

Then pass it through `specialArgs` in your outputs and add it to `environment.systemPackages`:

```nix
outputs = { nixpkgs, flec, ... }:
  let system = "x86_64-linux"; in {
    nixosConfigurations.hostname = nixpkgs.lib.nixosSystem {
      inherit system;
      specialArgs = { inherit flec; };
      modules = [
        ({ pkgs, ... }: {
          environment.systemPackages = [
            flec.packages.${pkgs.stdenv.hostPlatform.system}.default
          ];
        })
      ];
    };
  };
```
