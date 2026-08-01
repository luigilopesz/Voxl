# Editing verbs — before / after

One pair per verb, captured from the same camera position, with the HUD visible.
Left is before the edit, right is after.

The HUD strip along the bottom is the tool selector: `-T` remove terrain, `+T` add
terrain, `+G` add grass, `-G` remove grass, `Fl` flowers, `Lb` light ball,
`La` lantern, `Fi` fire, `To` torch, `Mp` maple, `Sp` spruce. Number keys or the
wheel select; `Alt`+wheel or `[` `]` sizes the brush; the current radius is shown
in metres at the right.

| | Verb | Notes |
|---|---|---|
| `01` | Remove terrain | The original verb — was hardcoded to a mouse button before this. |
| `02` | Add terrain | **The only genuinely new brush.** Everything else already existed in the shader and was commented out. |
| `03` | Add grass | |
| `04` | Remove grass | |
| `05` | Add flowers | |
| `06` | Light ball | Emissive sphere — the light feeds the path-traced GI, so it bounces. |
| `07` | Lantern | |
| `08` | Fire | |
| `09` | Torch | |
| `10` | Maple tree | |
| `11` | Spruce tree | |
| `12` | Remove at maximum radius | Worst-case brush cost — many chunks regenerate at once. |
| `13` | Add at maximum radius | Same, for the add path. |

## Honest read

These prove the plumbing works: every verb applies, the chunk update fires, and
the result persists. They do **not** yet prove the verbs look *good* — `02` and
`13` add a plain grey sphere, which is correct behaviour for a first pass and
crude as level design. Material choice for added terrain is the obvious next
refinement.

Also unverified: these are the implementing agents' own captures. The independent
verification pass and the three adversarial reviews were cut off by a session
limit before they ran, so nothing here has been checked by anything other than the
code that produced it.
