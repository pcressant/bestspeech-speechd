{
  description = "BeSTspeech / Keynote Gold for Speech Dispatcher, on openbst";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    openbst = {
      url = "github:Mudb0y/openbst/a151e78f66009bc74367974d14476061f3552346";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      openbst,
    }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      # openbst's tables are not free; allowing them is this flake's choice.
      pkgsFor =
        system:
        import nixpkgs {
          inherit system;
          config.allowUnfree = true;
        };
      forAll = f: nixpkgs.lib.genAttrs systems (system: f (pkgsFor system));
    in
    {
      packages = forAll (pkgs: rec {
        sd-bestspeech = pkgs.callPackage ./nix/package.nix {
          # A local build tree is never part of the source.
          src = pkgs.lib.cleanSourceWith {
            src = self;
            filter =
              path: _type:
              !builtins.elem (baseNameOf path) [
                "build"
                "result"
              ];
          };
          inherit openbst;
        };
        default = sd-bestspeech;
      });

      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.default ];
          OPENBST = "${openbst}";
        };
      });
    };
}
