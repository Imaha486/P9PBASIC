# ymfm (vendored subset)

- Origin: https://github.com/aarongiles/ymfm — Aaron Giles' Yamaha FM
  sound cores. This subset was taken from the copy vendored in
  RetroDOS Player (`PC98PLAYER-iOS/third_party/ymfm`), byte-identical
  for the files included here.
- Licence: BSD 3-Clause (see `LICENSE` in this directory).
- Vendored: 2026-11-17, unmodified.
- Subset: only what the YM2608 (OPNA) needs —
  `ymfm.h`, `ymfm_opn.{h,cpp}`, `ymfm_ssg.{h,cpp}`,
  `ymfm_adpcm.{h,cpp}`, `ymfm_fm.h`, `ymfm_fm.ipp`.
  The OPM/OPL/OPZ/etc. families are not included.
- Used by: `src/opna_ymfm.cpp` (host build only), which renders the
  OPNA register stream the Z80 vessel's port model captures.
