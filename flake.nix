{
  description = "flec, FLac Editor in C";

  inputs = {
    nixpkgs.url     = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        flec = pkgs.stdenv.mkDerivation {
          pname   = "flec";
          version = "1.0.0";
          src     = ./.;

          buildInputs = with pkgs; [ flac ncurses ];
          buildPhase  = ''
            cc -o nob nob.c
            ./nob
          '';

          installPhase = ''
            install -Dm755 flec $out/bin/flec
          '';

          meta = with pkgs.lib; {
            mainProgram = "flec";
            description = "A terminal UI for editing FLAC file metadata";
            homepage    = "https://github.com/tichelmorres/flec";
            license     = licenses.mit;
            platforms   = platforms.linux;
          };
        };
      in
        {
          packages = {
            flec    = flec;
            default = flec;
          };

          apps.default = flake-utils.lib.mkApp {
            drv  = flec;
            name = "flec";
          };

          devShells.default = pkgs.mkShell {
            inputsFrom      = [ flec ];
            PKG_CONFIG_PATH = pkgs.lib.makeSearchPathOutput "dev" "lib/pkgconfig" (
              with pkgs; [ flac ncurses ]
            );
          };
        }
    ) //

    {
      overlays.default = final: prev: {
        flec = self.packages.${prev.system}.default;
      };
    };
}
