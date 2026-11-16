# sd_bestspeech: BeSTspeech / Keynote Gold for Speech Dispatcher, on openbst.
# Shared by this repository's flake and by a NixOS configuration, which only
# has to say where the two sources come from.
{
  lib,
  stdenv,
  pkg-config,
  python3,
  speechd,
  glib,
  sonic,
  src,
  openbst,
}:
stdenv.mkDerivation {
  pname = "sd-bestspeech";
  version = "0.1.0";
  inherit src;

  nativeBuildInputs = [
    pkg-config
    python3
  ];
  # speech-dispatcher.pc requires glib, so pkg-config needs glib.dev too.
  buildInputs = [
    speechd
    glib
    sonic
  ];

  enableParallelBuilding = true;
  makeFlags = [
    "OPENBST=${openbst}"
    "PREFIX=${placeholder "out"}"
  ];

  # openbst's self-test on the five builds linked, then the module driven
  # over its protocol and compared sample for sample with bstspeak.
  doCheck = true;
  checkTarget = "test";

  meta = {
    description = "BeSTspeech / Keynote Gold (openbst) output module for Speech Dispatcher, French and English";
    homepage = "https://github.com/Mudb0y/openbst";
    # Our code and openbst's are MIT; openbst's tables are Berkeley Speech
    # Technologies' and HumanWare's, which nobody here can license.
    license = with lib.licenses; [
      mit
      unfree
    ];
    platforms = lib.platforms.linux;
  };
}
