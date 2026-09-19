{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
}:
stdenv.mkDerivation {
  pname = "noctalia-material-core";
  version = "7.0.0";
  src = lib.cleanSource ../src/material;
  nativeBuildInputs = [
    meson
    ninja
    pkg-config
  ];
  doCheck = true;
  meta = {
    description = "Shared parameter-driven desktop material rendering and scene protocol";
    license = [
      lib.licenses.mit
      lib.licenses.bsd3
    ];
    platforms = lib.platforms.linux;
  };
}
