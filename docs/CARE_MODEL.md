# The care model

Where every number came from, how confident the research was, and which choices
are ours rather than the literature's. Section numbers refer to
`HOUSEPLANT_CARE_RESEARCH.md`; confidence tags are that document's.

The rule this file exists to enforce: **a number in the simulation must be
traceable to a source or to a decision.** A number that is neither is a number
somebody guessed, and it will be defended later as though it were measured.

---

## 1. The thing most plant games get wrong

Watering is not a schedule. **Light and temperature drive the drying rate**, so
the interval between waterings is derived — base × pot drainage × light ×
temperature × humidity × airflow × season × root mass — and is never stored
(§2, `[HIGH]`).

Two consequences fall straight out of that, and both are in the sim:

- A dim, cool room **stretches** the interval, which is why low light causes
  *overwatering* rather than merely slow growth. The player's instinct — the
  plant looks sad, water it — is the kill.
- "Water every seven days" is the commonest cause of houseplant death (§13),
  which is why the game has no schedule, no watering day, and no reminder that
  says one.

## 2. Numbers taken from the research

| Quantity | Value | Source | Confidence |
|---|---|---|---|
| Care tick | 900 s | §2.1 quantisation | — (ours, §4 below) |
| Saturation level | soil at or above 0.92 counts as soggy | §8.1 | `[HIGH]` |
| Fungus gnats as the early overwatering tell | breed in permanently moist surface mix | §8.1 | `[HIGH]` |
| Peace lily collapse and recovery | total collapse when thirsty, full turgor back in 2–24 h; repeated collapse scars permanently | §1 row 8 | `[HIGH]` |
| Calathea humidity requirement | 60 %+ genuinely required, not preferred | §1 row 10 | `[HIGH]` |
| Calathea nyctinasty | leaves fold at night, open at dawn | §1 row 10 | `[HIGH]` |
| Snake plant indifference to humidity | no RH term at all | §4 | `[HIGH]` |
| Feeding refusals | dormant, dry soil, recently repotted, visibly sick | §5.4 | `[HIGH]` |
| Repot window after feeding | 4–6 weeks | §5.4 | `[MED]` |
| Overpotting | go up 2.5–5 cm, not more | §13 | `[HIGH]` |
| Gravel "for drainage" | raises the perched water table; makes drainage worse | §13 | `[HIGH]` |
| Misting | raises local RH for minutes, then gone | §13 | `[HIGH]` |
| Dust on leaves | measurably cuts effective light | §3 | `[MED]` |
| Pest egg cycle | one treatment always fails; 3–4 weekly repeats | §8.6 | `[HIGH]` |
| Brown leaf tips | at least four distinct causes that look identical | §8 | `[HIGH]` |
| Yellow lower leaves | more often overwatering than thirst | §8.1, §13 | `[HIGH]` |

Where the research gave a range and the game needed one number, the game takes
the **cautious end** and says so in `content/plants.json`. The feed-after-repot
window is 42 days from a 4–6 week range, because the failure it prevents
(fertiliser into damaged roots) is worse than the cost of waiting.

## 3. The uncertainty register (§12), honoured rather than hidden

The research flagged several numbers as `[MED]` or `[LOW]`. Those are the ones
the game refuses to be confident about **in its own voice**:

- **Brown tips.** Four causes, indistinguishable on the leaf. `pg_advice.c`
  names all four and gives the discriminating check for each — crust at the
  rim, lift the pot, soil at depth — rather than picking one. Naming one would
  be wrong three times in four and would teach the player a myth.
- **Pebble trays.** Marginal and very local. Available, honest, never
  recommended.
- **Droplets as lenses.** Not reproducible on flat leaves. Not modelled.
- **Dust reduction of DLI.** Modelled at the low end of the range, which is the
  honest reading of a `[MED]` ceiling.

## 4. Choices that are ours, each with its reason

These are not in the research. They are modelling decisions, and they are here
so that a future reader does not mistake them for measurements.

| Choice | Value | Why |
|---|---|---|
| Care tick | 900 s | Biology quantises to something; 15 minutes makes a 30-day catch-up 2 880 steps, which is bounded work, and is finer than any real plant process the game models. |
| Growing-season threshold | growth scalar ≥ 0.45 | The season scalar is continuous; "growing" has to be a line somewhere, and 0.45 puts the boundary in early spring and late autumn where the research's month table already changes advice. |
| `solar_noon_local_minutes` | 780 (13:00 local) | D-080. Real solar noon wanders with longitude and DST; one fixed value makes every local-hour rule in the game consistent and reproducible, and 13:00 is the honest average under summer time. |
| Recovery floor | `effective_scalar = max(growth_scalar, 0.30)` | D-085, and the most consequential of these. The raw scalar bottoms out near 0.06 in midwinter. Recovery times are divided by it, so without a floor a three-week repot shock becomes a **year**, and a plant that was merely repotted in November would read as dying until spring. The floor applies only to recoveries, shocks and fair-warning intervals — never to growth, which really does stop. |
| Calathea `dli_maintenance` | 1.5, not 3 | D-093. At 3 she could not survive December at the 2 m spot the design calls her home, failing the reachability assertion. 1.5 is also the more accurate figure: she is a forest-floor plant. |
| Fixed-point everywhere | `_l` 0..10000, `_q12` 4096 = ×1.0 | D-083. No float enters any record, so a 30-day catch-up is byte-identical on every machine. This is why the game can promise that a replayed absence produces the same July it produced the first time. |

## 5. The myth blocklist (§13)

Enforced by `tests/test_content.py` over `content/**` **and over
`src/pg_advice.c`**, not by review — a myth is exactly the sort of plausible
sentence that survives review. 262 authored strings are checked.

Every myth remains *available as an action* and behaves truthfully. You can
mist; it raises local humidity for twenty minutes and then it is gone. You can
add a gravel layer when repotting; it makes drainage worse. What the game will
never do is **recommend** one.
