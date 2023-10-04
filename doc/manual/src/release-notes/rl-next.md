# Release X.Y (202?-??-??)

- [URL flake references](@docroot@/command-ref/new-cli/nix3-flake.md#flake-references) now support [percent-encoded](https://datatracker.ietf.org/doc/html/rfc3986#section-2.1) characters.

- [Path-like flake references](@docroot@/command-ref/new-cli/nix3-flake.md#path-like-syntax) now accept arbitrary unicode characters (except `#` and `?`).

- The experimental feature `repl-flake` is no longer needed, as its functionality is now part of the `flakes` experimental feature. To get the previous behavior, use the `--file/--expr` flags accordingly.

- Introduce new flake installable syntax `flakeref#.attrPath` where the "." prefix denotes no searching of default attribute prefixes like `packages.<SYSTEM>` or `legacyPackages.<SYSTEM>`.

- The `nixosConfigurations` flake output attribute has been renamed to
  `configurations.nixos.<system>` with the system name that would be used to
  build the configuration. It is advised to keep configurations identical when
  defined under the same name for multiple systems, e.g.

  * `configurations.nixos.aarch64-linux.webserver` defines a webserver
    configuration built on `aarch64-linux` system.
  * `configurations.nixos.x86_64-linux.webserver` describes the same
    configuration, but cross-compiled from `x86_64-linux` system.

  Note though that bit-wise identical cross-compilation is currently not
  possible without [content-addressed derivations] (and even then a lot of
  effort is necessary on the Nixpkgs side to make that work). In addition to
  that, NixOS configurations currently support only a single host platform. That
  is, the resulting system contains executables and bootloader only for a single
  CPU architecture.

  The old output will continue to work, but `nix flake check` will issue a
  deprecation warning about it.

  Other configuration systems are encouraged to use `configurations.<class>`
  instead of top-level attributes, e.g. `configurations.home` instead of
  `homeConfigurations`.

[content-addressed derivations]: @docroot@/contributing/experimental-features.md#xp-feature-ca-derivations
