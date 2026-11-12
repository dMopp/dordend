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
  libjxl,
  zlib,
  sdbus-cpp_2,
  shared-mime-info,
  hicolor-icon-theme,
  # Plugins (plugins/<id>) to leave out of the build, e.g. [ "remotes" ]; all are built otherwise.
  disabledPlugins ? [ ],
}:

stdenv.mkDerivation {
  pname = "nordend";
  version = "0.5.0";

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../CMakeLists.txt
      ../LICENSE
      ../cmake
      ../data
      ../docs/config.example.toml
      ../plugins
      ../src
      # The preview helper embeds these images to warm up codecs before sandboxing.
      ../tests/data/image
      # The development SVG renderer shares its comparison helper with the tests.
      ../tests/svg_compare.hpp
      ../third_party
    ];
  };

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    makeWrapper
    wayland-scanner
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
  ];

  cmakeFlags = [
    "-DNORDEND_WERROR=OFF"
    "-DNORDEND_BUILD_TESTS=OFF"
  ]
  ++ map (id: "-DNORDEND_PLUGIN_${lib.toUpper id}=OFF") disabledPlugins;

  postInstall = ''
    install -Dm644 "$src/LICENSE" "$out/share/licenses/nordend/LICENSE"
    wrapProgram "$out/bin/nordend" \
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
    homepage = "https://github.com/pankaspe/nordend-fm";
    license = lib.licenses.mit;
    mainProgram = "nordend";
    platforms = [
      "x86_64-linux"
      "aarch64-linux"
    ];
  };
}
