# SPDX-License-Identifier: GPL-3.0-or-later
{
  description = "X11 on-screen ruler with real physical dimensions via XRandR";

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
          pname = "x11-ruler";
          version = "0.1.0";
          src = self;

          nativeBuildInputs = with pkgs; [ cmake pkg-config ];
          buildInputs = with pkgs; [
            xorg.libX11
            xorg.libXrandr
            xorg.libXext
            xorg.libXrender
          ];

          cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release" ];

          meta = with pkgs.lib; {
            description = "Borderless X11 ruler showing real physical mm/cm via XRandR";
            license = licenses.gpl3Plus;
            platforms = platforms.linux;
            mainProgram = "x11-ruler";
          };
        };

        apps.default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/x11-ruler";
        };

        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            cmake
            pkg-config
            ninja
            xorg.libX11
            xorg.libXrandr
            xorg.libXext
            xorg.libXrender
            xorg.libX11.dev
            xorg.libXrandr.dev
            xorg.libXext.dev
            xorg.libXrender.dev
          ];
        };
      });
}
