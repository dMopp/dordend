{
  description = "rummage, a native file manager for Wayland";

  inputs.nixpkgs.url = "https://channels.nixos.org/nixos-unstable/nixexprs.tar.zst";

  outputs =
    { self, nixpkgs }:
    let
      forEachSystem = nixpkgs.lib.genAttrs [
        "x86_64-linux"
        "aarch64-linux"
      ];
    in
    {
      nixosModules = {
        rummage = import ./nix/module.nix self;
        default = self.nixosModules.rummage;
      };

      packages = forEachSystem (system: {
        rummage = nixpkgs.legacyPackages.${system}.callPackage ./nix/package.nix { };
        rummage-with-media = self.packages.${system}.rummage.override { withMedia = true; };
        rummage-media = self.packages.${system}.rummage-with-media.media;
        rummage-with-pdf = self.packages.${system}.rummage.override { withPdf = true; };
        rummage-pdf = self.packages.${system}.rummage-with-pdf.pdf;
        # Both optional preview engines: video stills and PDF pages.
        rummage-with-previews = self.packages.${system}.rummage.override {
          withMedia = true;
          withPdf = true;
        };
        default = self.packages.${system}.rummage;
      });

      checks = forEachSystem (system: {
        tests = self.packages.${system}.rummage.override {
          withTests = true;
          withMedia = true;
          withPdf = true;
        };
        nixos-module = import ./nix/module-check.nix { inherit self nixpkgs system; };
        smoke = nixpkgs.legacyPackages.${system}.callPackage ./nix/smoke-test.nix {
          package = self.packages.${system}.rummage;
        };
      });

      devShells = forEachSystem (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.rummage ];
            packages = with pkgs; [
              clang-tools
              doctest
              gdb
              git
              ffmpeg
              libjpeg_turbo
              nixfmt
            ];
            # Keep CMake's setup hook from configuring a build on shell entry.
            dontUseCmakeConfigure = true;
            shellHook = ''
              export XDG_DATA_DIRS="''${XDG_DATA_DIRS:-/usr/local/share:/usr/share}:${pkgs.shared-mime-info}/share"
            '';
          };
        }
      );

      formatter = forEachSystem (system: nixpkgs.legacyPackages.${system}.nixfmt);
    };
}
