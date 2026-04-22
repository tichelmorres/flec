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

Narrow the fzf search to one or more specific directories:

```console
$ ./flec -f "~/Music"
$ ./flec -f "~/Downloads" "~/Music" "/home/user/My Songs"
```

Also specify where fzf should look for cover images:

```console
$ ./flec -f "~/Music" -c "~/Pictures" "/home/user/CoverArts"
```

Skip fzf entirely and open a file directly:

```console
$ ./flec -nf "~/Music/Pink Floyd/Time.flac"
```

This can be combined with the other flags too:

```console
$ ./flec -nf "~/Music/Pink Floyd/Time.flac" -c "~/Pictures"
```

## Flags

| Flag                       | Description                                                   |
|----------------------------|---------------------------------------------------------------|
| `-f`, `--flac` `<dir>...`  | Directories to search for FLAC files (multiple paths allowed) |
| `-c`, `--cover` `<dir>...` | Directories to search for cover arts (multiple paths allowed) |
| `-nf`, `--no-fzf` `<path>` | Skip fzf at start and open a specific FLAC file directly      |

In fact, all flags can be combined in any order. If `-f` is omitted, fzf searches `$HOME` by default. The same happens to cover image selection if `-c` is omitted.

## Keybinds

| Key                 | Action                      |
|---------------------|-----------------------------|
| `j` / `k` / arrows  | Navigate fields             |
| `Enter` / `e` / `a` | Edit field                  |
| `Escape`            | Cancel edit                 |
| `Ctrl+S`            | Save                        |
| `Ctrl+X`            | Discard pending cover path  |
| `r`                 | Search another file to edit |
| `q`                 | Quit                        |

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
