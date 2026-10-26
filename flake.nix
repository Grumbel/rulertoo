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
        versionBase = nixpkgs.lib.removeSuffix "\n" (builtins.readFile ./VERSION);
        # Optional: append short rev when building from a dirty/git tree.
        version =
          if (self ? shortRev)
          then "${versionBase}+g${self.shortRev}"
          else if (self ? dirtyShortRev)
          then "${versionBase}+g${self.dirtyShortRev}"
          else versionBase;
      in {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "rulertoo";
          inherit version;
          src = self;

          nativeBuildInputs = with pkgs; [ cmake pkg-config ];
          # libxcb, libxau, libxdmcp provide .pc files for libX11 Requires.private
          buildInputs = with pkgs; [
            libx11
            libxrandr
            libxrender
            libxcb
            libxau
            libxdmcp
            xorgproto
            libxext
          ];

          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
            "-DPROJECT_VERSION_FULL=${version}"
          ];

          meta = with pkgs.lib; {
            description = "Borderless X11 ruler with real physical mm/cm via XRandR";
            longDescription = ''
              Borderless on-screen ruler for X11 that uses real physical
              millimetre sizes reported by XRandR rather than a global DPI
              assumption. Supports horizontal/vertical orientation, free-end
              resize, unit cycling, and per-monitor calibration when EDID
              sizes are wrong.
            '';
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
            gdb
            libx11
            libxrandr
            libxrender
            libxcb
            libxau
            libxdmcp
            xorgproto
            (writeShellScriptBin "rulertoo-configure" ''
              set -euo pipefail
              if [ -z "''${PROJECT_SOURCE:-}" ]; then
                echo "rulertoo-configure: PROJECT_SOURCE is not set (enter the shell with: nix develop)" >&2
                exit 1
              fi
              PROJECT_BUILD_DIR="''${PROJECT_BUILD_DIR:-/tmp/rulertoo-build}"
              cmake -S "$PROJECT_SOURCE" -B "$PROJECT_BUILD_DIR" -G Ninja \
                -DCMAKE_BUILD_TYPE="''${CMAKE_BUILD_TYPE:-Debug}" \
                -DPROJECT_VERSION_FULL="''${PROJECT_VERSION_FULL:-$(tr -d '\n' < "$PROJECT_SOURCE/VERSION")}"
            '')
            (writeShellScriptBin "rulertoo-build" ''
              set -euo pipefail
              if [ -z "''${PROJECT_SOURCE:-}" ]; then
                echo "rulertoo-build: PROJECT_SOURCE is not set (enter the shell with: nix develop)" >&2
                exit 1
              fi
              PROJECT_BUILD_DIR="''${PROJECT_BUILD_DIR:-/tmp/rulertoo-build}"
              if [ ! -f "$PROJECT_BUILD_DIR/CMakeCache.txt" ]; then
                rulertoo-configure
              else
                # Reconfigure if the source tree path in the cache no longer matches.
                cached=$(grep -E '^CMAKE_HOME_DIRECTORY:' "$PROJECT_BUILD_DIR/CMakeCache.txt" | cut -d= -f2- || true)
                if [ "$cached" != "$PROJECT_SOURCE" ]; then
                  rulertoo-configure
                fi
              fi
              cmake --build "$PROJECT_BUILD_DIR"
            '')
            (writeShellScriptBin "rulertoo-run" ''
              set -euo pipefail
              rulertoo-build
              PROJECT_BUILD_DIR="''${PROJECT_BUILD_DIR:-/tmp/rulertoo-build}"
              exec "$PROJECT_BUILD_DIR/rulertoo" "$@"
            '')
            (writeShellScriptBin "rulertoo-run-gdb" ''
              set -euo pipefail
              rulertoo-build
              PROJECT_BUILD_DIR="''${PROJECT_BUILD_DIR:-/tmp/rulertoo-build}"
              exec gdb --args "$PROJECT_BUILD_DIR/rulertoo" "$@"
            '')
          ];
          CMAKE_BUILD_TYPE = "Debug";
          shellHook = ''
            export PROJECT_SOURCE="$PWD"
            export PROJECT_BUILD_DIR="''${PROJECT_BUILD_DIR:-/tmp/rulertoo-build}"
            echo "rulertoo: rulertoo-configure | rulertoo-build | rulertoo-run | rulertoo-run-gdb"
          '';
        };
      });
}
