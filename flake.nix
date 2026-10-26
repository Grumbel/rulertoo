# SPDX-License-Identifier: GPL-3.0-or-later
{
  description = "On-screen X11 ruler with real physical dimensions via XRandR";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "rulertoo";
          version = "0.2.1";
          src = self;

          nativeBuildInputs = with pkgs; [ cmake pkg-config ];
          # libxcb provides xcb.pc so pkg-config can resolve libX11's Requires
          buildInputs = with pkgs; [
            libx11
            libxrandr
            libxrender
            libxcb
            xorg.xorgproto
          ];

          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
          ];

          meta = with pkgs.lib; {
            description = "Borderless X11 ruler with real physical mm/cm via XRandR";
            homepage = "https://github.com/Grumbel/rulertoo";
            license = licenses.gpl3Plus;
            platforms = platforms.linux;
            mainProgram = "rulertoo";
          };
        };

        apps.default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/rulertoo";
        };

        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            cmake
            pkg-config
            ninja
            libx11
            libxrandr
            libxrender
            libxcb
            xorg.xorgproto
          ];
        };
      });
}
