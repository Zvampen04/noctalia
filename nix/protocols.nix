{ lib, runCommand }:
runCommand "noctalia-protocols-1.0.0"
  {
    meta = {
      description = "Owned native shell and compositor integration protocols";
      license = lib.licenses.mit;
      platforms = lib.platforms.linux;
    };
  }
  ''
    install -Dm644 ${../protocols/noctalia-foreign-parent-v1.xml} \
      $out/share/noctalia-protocols/noctalia-foreign-parent-v1.xml
  ''
