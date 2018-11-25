let defaultSystems = builtins.listToAttrs [ { name = builtins.currentSystem;
                                              value = import <nixpkgs> {}; } ];
in { kite-app-module
   , systems ? defaultSystems
   , pure-build ? false }:

let kite-config = { config, pkgs, lib, ... }: {
      imports = [ ./kite.nix (builtins.toPath kite-app-module) ];

      config = with lib; {
        boot.isContainer = true;
        networking.firewall.enable = false;
        services.openssh.startWhenNeeded = false;
        environment.noXlibs = mkDefault true;

        environment.systemPackages = [ config.kite.app ];
      };

      options = with lib; {
        kite.app = lib.mkOption {
          type = lib.types.package;
          description = ''
          A nix derivation providing the package that is used to launch the kite application
          '';
        };
      };
    };

    evalInPlatform = system: pkgs: import <nixpkgs/nixos/lib/eval-config.nix> {
      inherit system pkgs;
      modules = [ ./modules/top-level.nix (builtins.toPath kite-app-module) ];
      extraArgs = { kite-lib = (import ./lib/kite.nix) pkgs; inherit pure-build; };
    };

    platforms = map (name: rec { inherit name; config = (evalInPlatform name (builtins.getAttr name systems)).config; package = config.kite.toplevel; })
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
        runAsAdmin = config.kite.runAsAdmin;
        singleton = config.kite.singleton;

        bind-mounts = config.kite.bindMounts;
      })) // { toplevels = closures; }


