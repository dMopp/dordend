# PDFium as built by the pdfium-binaries project: one shared library that needs only glibc,
# without V8, XFA or PartitionAlloc. BSD-3-Clause (bundled parts: see share/licenses).
{
  lib,
  stdenvNoCC,
  fetchurl,
}:

let
  version = "8152";
  assets = {
    x86_64-linux = {
      name = "pdfium-linux-x64.tgz";
      hash = "sha256-OEq1xRiCQIoUOiH5wU3NLK8fdSgK5BxVTQSJtGw9up8=";
    };
    aarch64-linux = {
      name = "pdfium-linux-arm64.tgz";
      hash = "sha256-xu0akSNK7eDPr9wJxAG4O9RKxp3q/EWZ/addURw2QD4=";
    };
  };
  asset =
    assets.${stdenvNoCC.hostPlatform.system}
      or (throw "pdfium-binaries: no build for ${stdenvNoCC.hostPlatform.system}");
in
stdenvNoCC.mkDerivation {
  pname = "pdfium-binaries";
  inherit version;

  src = fetchurl {
    url = "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F${version}/${asset.name}";
    inherit (asset) hash;
  };
  sourceRoot = ".";

  installPhase = ''
    runHook preInstall
    mkdir -p "$out/lib/cmake/PDFium" "$out/share/licenses/pdfium"
    cp -r include "$out/"
    rm -f "$out/include/"*.orig
    cp lib/libpdfium.so "$out/lib/"
    cp PDFiumConfig.cmake "$out/lib/cmake/PDFium/"
    # The config looks for include/ and lib/ next to itself.
    substituteInPlace "$out/lib/cmake/PDFium/PDFiumConfig.cmake" \
      --replace-fail '"''${CMAKE_CURRENT_LIST_DIR}"' "\"$out\""
    cp LICENSE licenses/* "$out/share/licenses/pdfium/"
    runHook postInstall
  '';

  meta = {
    description = "PDFium shared library from the pdfium-binaries project";
    homepage = "https://github.com/bblanchon/pdfium-binaries";
    license = lib.licenses.bsd3;
    sourceProvenance = [ lib.sourceTypes.binaryNativeCode ];
    platforms = builtins.attrNames assets;
  };
}
