{
  lib,
  stdenv,
  cmake,
  ninja,
  pkg-config,
  makeWrapper,
  wayland,
  wayland-scanner,
  wayland-protocols,
  libglvnd,
  libxkbcommon,
  harfbuzz,
  freetype,
  fontconfig,
  tomlplusplus,
  libwebp,
  ffmpeg,
  mupdf,
  libjxl,
  zlib,
  sdbus-cpp_2,
  shared-mime-info,
  hicolor-icon-theme,
  doctest,
  dbus,
  dejavu_fonts,
  makeFontsConf,
  mesa,
  _7zz,
  unzip,
  writeShellScriptBin,
  withTests ? false,
  withMedia ? false,
  withPdf ? false,
}:

stdenv.mkDerivation {
  pname = "rummage";
  version = "0.5.0";
  outputs = [ "out" ] ++ lib.optional withMedia "media" ++ lib.optional withPdf "pdf";

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions (
      [
        ../CMakeLists.txt
        ../LICENSE
        ../cmake
        ../data
        ../docs/config.example.toml
        ../src
        # The preview helper embeds these images to warm up codecs before sandboxing.
        ../tests/data/image
        # The development SVG renderer shares its comparison helper with the tests.
        ../tests/svg_compare.hpp
        ../third_party
      ]
      ++ lib.optional withTests ../tests
    );
  };

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    makeWrapper
    wayland-scanner
  ]
  ++ lib.optionals withTests [
    doctest
    (writeShellScriptBin "dbus-run-session" ''
      exec ${dbus}/bin/dbus-run-session --dbus-daemon=${dbus}/bin/dbus-daemon --config-file=${dbus}/share/dbus-1/session.conf "$@"
    '')
  ];

  buildInputs = [
    wayland
    wayland-protocols
    libglvnd
    libxkbcommon
    harfbuzz
    freetype
    fontconfig
    tomlplusplus
    libwebp
    libjxl
    zlib
    sdbus-cpp_2
  ]
  ++ lib.optional withMedia ffmpeg
  ++ lib.optional withPdf mupdf;

  cmakeFlags = [
    "-DRUMMAGE_WERROR=OFF"
    "-DRUMMAGE_MEDIA=${if withMedia then "ON" else "OFF"}"
    "-DRUMMAGE_PDF=${if withPdf then "ON" else "OFF"}"
    "-DRUMMAGE_BUILD_TESTS=${if withTests then "ON" else "OFF"}"
  ];

  doCheck = withTests;
  nativeCheckInputs = lib.optionals withTests [
    _7zz
    unzip
    (writeShellScriptBin "7z" ''exec ${_7zz}/bin/7zz "$@"'')
  ];
  # These two fixtures create executable scripts at runtime, after patchShebangs runs.
  postPatch = lib.optionalString withTests ''
    substituteInPlace tests/about_test.cpp tests/platform_test.cpp \
      --replace-fail '#!/bin/sh' '#!${stdenv.shell}'
  '';
  checkPhase = ''
    runHook preCheck
    export FONTCONFIG_FILE=${
      makeFontsConf {
        fontDirectories = [ dejavu_fonts ];
        includes = [ "${fontconfig.out}/share/fontconfig/conf.avail/60-latin.conf" ];
      }
    }
    export XDG_DATA_DIRS=${shared-mime-info}/share:${hicolor-icon-theme}/share
    export LIBGL_ALWAYS_SOFTWARE=1
    export EGL_PLATFORM=surfaceless
    export __EGL_VENDOR_LIBRARY_FILENAMES=${mesa}/share/glvnd/egl_vendor.d/50_mesa.json
    export LIBGL_DRIVERS_PATH=${mesa}/lib/dri
    ctest --show-only | grep -F file-picker-portal
    ctest --show-only | grep -F action-notifications
    ctest --output-on-failure --timeout 180
    runHook postCheck
  '';

  postInstall =
    lib.optionalString withMedia ''
      mkdir -p "$media/libexec/rummage" "$media/share/rummage/engines"
      mv "$out/libexec/rummage/rummage-media" "$media/libexec/rummage/"
      mv "$out/share/rummage/engines/media.toml" "$media/share/rummage/engines/"
    ''
    + lib.optionalString withPdf ''
      mkdir -p "$pdf/libexec/rummage" "$pdf/share/rummage/engines"
      mv "$out/libexec/rummage/rummage-pdf" "$pdf/libexec/rummage/"
      mv "$out/share/rummage/engines/pdf.toml" "$pdf/share/rummage/engines/"
    ''
    + ''
      install -Dm644 "$src/LICENSE" "$out/share/licenses/rummage/LICENSE"
      wrapProgram "$out/bin/rummage" \
        ${lib.optionalString withMedia "--suffix RUMMAGE_ENGINE_PATH : $media/share/rummage/engines"} \
        ${lib.optionalString withPdf "--suffix RUMMAGE_ENGINE_PATH : $pdf/share/rummage/engines"} \
        --set-default XDG_DATA_DIRS /usr/local/share:/usr/share \
        --suffix XDG_DATA_DIRS : "${
          lib.makeSearchPath "share" [
            shared-mime-info
            hicolor-icon-theme
          ]
        }"
    '';

  meta = {
    description = "Native file manager for Wayland";
    homepage = "https://github.com/barrulus/rummage";
    license = lib.licenses.mit;
    mainProgram = "rummage";
    platforms = [
      "x86_64-linux"
      "aarch64-linux"
    ];
  };
}
