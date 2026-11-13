{
  runCommand,
  package,
  bash,
  desktop-file-utils,
  weston,
  dbus,
  mesa,
  makeFontsConf,
  dejavu_fonts,
  writeShellScriptBin,
}:
runCommand "rummage-installed-smoke"
  {
    nativeBuildInputs = [
      bash
      desktop-file-utils
      weston
      (writeShellScriptBin "dbus-run-session" ''
        exec ${dbus}/bin/dbus-run-session --dbus-daemon=${dbus}/bin/dbus-daemon --config-file=${dbus}/share/dbus-1/session.conf "$@"
      '')
    ];
    FONTCONFIG_FILE = makeFontsConf { fontDirectories = [ dejavu_fonts ]; };
    __EGL_VENDOR_LIBRARY_FILENAMES = "${mesa}/share/glvnd/egl_vendor.d/50_mesa.json";
    LIBGL_DRIVERS_PATH = "${mesa}/lib/dri";
  }
  ''
    bash ${../scripts/ci/check-installed.sh} ${package} ${package.version}
    bash ${../scripts/ci/check-wayland.sh} ${package}
    touch "$out"
  ''
