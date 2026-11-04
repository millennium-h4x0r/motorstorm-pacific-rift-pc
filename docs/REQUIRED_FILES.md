# Required files

Everything comes from **your own** PS3 and copy of the game. Nothing here is
in the repository, and nothing the build makes from it may be shared.

## The game

**MotorStorm: Pacific Rift, BCES-00129 (Europe), version 01.00.** The whole
`PS3_GAME/` folder (~13 GB, mostly `USRDIR/ms2.psarc.666xx`).

## The executable

`EBOOT.BIN` is encrypted (SCE header). Decrypt it to `EBOOT.elf`, e.g. with
RPCS3 (*Utilities → Decrypt PS3 Binaries*). Expected result:

```
sha256 bbb7fe2545e54847483439f59cda54e907db23a111d7afe9a1b655624886b287  EBOOT.elf
size   19047304 bytes
```

## Layout

`disc/` is git-ignored. Link the dump and copy the decrypted ELF in:

```sh
SRC=~/Games/PS3/BCES00129/PS3_GAME
mkdir -p disc/PS3_GAME/USRDIR
for f in "$SRC"/*; do [ "$(basename "$f")" = USRDIR ] || ln -sfn "$f" disc/PS3_GAME/; done
for f in "$SRC"/USRDIR/*; do case "$(basename "$f")" in EBOOT.*|ms2.psarc.*) ;; *) ln -sfn "$f" disc/PS3_GAME/USRDIR/;; esac; done
cp "$SRC/USRDIR/EBOOT.elf" disc/PS3_GAME/USRDIR/
# The game opens one /ms2.psarc. Dumps made for FAT32 split it into 1 GB
# pieces (ms2.psarc.66600 ... .66604): join them, in order.
rm -f disc/PS3_GAME/USRDIR/ms2.psarc.666*
cat "$SRC"/USRDIR/ms2.psarc.666* > disc/PS3_GAME/USRDIR/ms2.psarc   # 4,811,608,561 bytes, starts "PSAR"
```
