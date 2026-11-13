self:
{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.programs.rummage;
in
{
  options.programs.rummage = {
    enable = lib.mkEnableOption "the rummage file manager";

    package = lib.mkOption {
      type = lib.types.package;
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      defaultText = lib.literalExpression "inputs.rummage.packages.\${pkgs.stdenv.hostPlatform.system}.default";
      description = "rummage package to install and use for the file picker.";
    };

    filePicker = {
      enable = lib.mkEnableOption "rummage as the XDG FileChooser portal backend";

      desktops = lib.mkOption {
        type = lib.types.listOf lib.types.str;
        default = [ "umbriel" ];
        example = [
          "umbriel"
          "niri"
        ];
        description = ''
          Lowercase desktop names whose portal configuration should select rummage
          for file selection. Use "common" for the desktop-independent portals.conf.
          Keep the other portal preferences in xdg.portal.config for these desktops:
          desktop-specific configurations take precedence over common and package
          configurations.
        '';
      };
    };
  };

  config = lib.mkMerge [
    {
      assertions = [
        {
          assertion = cfg.filePicker.enable -> cfg.enable;
          message = "programs.rummage.filePicker.enable requires programs.rummage.enable.";
        }
        {
          assertion = cfg.filePicker.enable -> cfg.filePicker.desktops != [ ];
          message = "programs.rummage.filePicker.desktops must select at least one desktop (or common).";
        }
      ];
    }
    (lib.mkIf cfg.enable {
      environment.systemPackages = [ cfg.package ];
    })
    (lib.mkIf (cfg.enable && cfg.filePicker.enable) {
      xdg.portal = {
        enable = true;
        extraPortals = [ cfg.package ];
        config = lib.genAttrs cfg.filePicker.desktops (_: {
          "org.freedesktop.impl.portal.FileChooser" = [ "rummage" ];
        });
      };
    })
  ];
}
