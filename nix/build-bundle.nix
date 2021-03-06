let defaultPkgs = import <nixpkgs> {};
    defaultSystems = builtins.listToAttrs [ { name = defaultPkgs.hostPlatform.config;
                                              value = import <nixpkgs> {}; } ];
in { kite-app-module
   , systems ? defaultSystems
   , pure-build ? false }:

let evalInPlatform = pkgs: import <nixpkgs/nixos/lib/eval-config.nix> {
      inherit pkgs;
      system = pkgs.stdenv.targetPlatform.system;
      modules = [ ./modules/top-level.nix (builtins.toPath kite-app-module) ];
      extraArgs = { kite-lib = (import ./lib/kite.nix) pkgs; inherit pure-build; };
    };

    platforms = map (name: rec { inherit name; config = (evalInPlatform (builtins.getAttr name systems)).config; package = config.kite.toplevel; })
                    (builtins.attrNames systems);

    config = (builtins.head platforms).config;
    closures = builtins.listToAttrs (map ({name, package, ...}: { inherit name; value = package; }) platforms);

in ((import <nixpkgs> {}).writeText "${config.kite.meta.slug}-manifest"
    (builtins.toJSON {
        name = config.kite.meta.name;
        app-url = config.kite.meta.app-url;
        icon = config.kite.meta.icon;
#        authors = config.kite.meta.authors;
        domain = config.kite.identifier;
        nix-closure = closures;
        run-as-admin = config.kite.runAsAdmin;
        singleton = config.kite.singleton;

        version = "${builtins.toString config.kite.version.major}.${builtins.toString config.kite.version.minor}.${builtins.toString config.kite.version.revision}";

        bind-mounts = config.kite.bindMounts;
      })) // { toplevels = closures; }


