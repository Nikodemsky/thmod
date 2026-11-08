# Ravenholm ruins -- assets

`TH_ns_assets.mor` here holds only the files the **Ravenholm ruins** code needs. Put it next to `TH4.exe`.
The game opens it as an optional archive (priority 8800: above `TH4data.mor`, below the language packs).

| File in the archive | What it is |
|---|---|
| `NLevels\TownData\Ravenholm.CEL` | the town's CEL plus the ruined houses' frames |
| `NLevels\TownData\Ravenholm.MIN` | pillars: the town's + the ruins' (1799 in all) |
| `NLevels\TownData\Ravenholm.TIL` | megatiles: the town's + the ruins' (544 in all) |
| `NLevels\TownData\Ravenholm.SOL` | collision of those pillars (the ruins' walls block walking and missiles) |
| `Levels\TownData\RavenholmRuins.DUN` | Ravenholm's layout with the ruined houses |

No archive, or a file missing from it, and Ravenholm is drawn as before: `RavenholmRuins()` checks all five.
The Dark Campaign's Ravenholm (mission 26) keeps its own layout.

If you already use a `TH_ns_assets.mor` with other files in it, add these five to it under the same names
(any MPQ tool will do; the archive is MPQ v1, uncompressed, like `TH4data.mor`).
