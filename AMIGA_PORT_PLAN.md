# ppengine → Apollo V4 (SAGA): analyse en takenlijst

Antwoord op `Projects/task_1.md`. Datum: 2026-11-14.

## 1. Analyse van de code

### Wat het is
Een 3D-remake van de Pole Position-arcadekast. De spellogica is
reverse-engineered uit de arcade-ROMs (68000-code van de arcadekast: de
bestandsnamen `1ff0.c`, `bd0.c` enz. zijn de ROM-adressen). De arcadekast
draait op Zilog Z8002-CPU's (16-bit); het commentaar in de code verwijst
naar hun registers (`rr1`).

**Let op: het project gebruikt geen SDL**, ook al noemt de README SDL2.
De code gebruikt:

| Laag | Bibliotheek | Bestanden | Regels |
|---|---|---|---|
| Spellogica | geen (pure C, alleen integers) | `src/ppengine/**` (89 bestanden) | ~4500 |
| Baangeometrie | libm (`double`, `atan2`, `sqrt`) | `src/track_geometry/`, `src/geometry/` | ~330 |
| Rendering | **OpenSceneGraph** (OpenGL, C++98) | `src/scene/osg/*` | ~700 |
| Scene-koppeling | C++ + `cos`/`sin` | `src/scene/scene.c` | 237 |
| Invoer | osgGA event handler | `src/input/*` | ~150 |
| Geluid | **Gorilla Audio** (OpenAL), OGG/WAV | `src/audio/*` | ~180 |
| Hoofdlus | `src/client.c` | | 35 |

### Architectuur (goed nieuws voor een port)
De scheiding is netjes. `client.c` doet per frame:

```
update_engine_state_from_input();   // invoer -> globale variabelen
engine_execute_frame();             // arcade-logica, 1 tick
scene_update_*(...);                // leest moving_objects[], hud_content[], gantry
audio_update(...);                  // leest moving_objects[], engine pitch, skid volume
scene_generate_frame();
```

De engine praat met de rest alleen via globale structs (`moving_objects[32]`,
`scenery_objects[24]`, `hud_content[32*28]`, een paar `uint16_t`'s) en een
sound-effect-callback. **Alleen de platformlagen (scene, input, audio) moeten
vervangen worden; de engine blijft ongewijzigd.**

### Wat het platform-specifieke deel vraagt
- **3D-scène**: 5 automodellen, 10 bordmodellen, startportaal met lampen,
  bergen-panorama, baan van 2048 getextureerde quads. Modellen in OSG ASCII
  (`.osgt`, totaal ~68 000 regels), textures in PNG (niet macht van 2,
  `mountains...png` is 100 KB), achtergrond `bg.jpg`.
- **Effecten**: shadow mapping (2048x2048 shadow map), particle-explosies
  (`osgParticle::ExplosionEffect`), alpha-blending voor het Game Over-plaatje.
- **HUD**: 32x28 tekenrooster (arcade tilemap) met TrueType-font
  `prstartk.ttf`, 6 kleuren.
- **Geluid**: 8 motorgeluiden (7 CPU-auto's + speler) met per stem pitch, pan
  en volume, plus een slipgeluid in een lus. Explosie en overige effecten zijn
  nog niet geïmplementeerd (alleen `printf`).
- **Baangeometrie**: bij het opstarten 65 536 segmenten berekend in `double`
  (Bézier-interpolatie + `atan2`), ±3,5 MB segmenten + 1 MB subpunten.
- **Timing**: de engine draait één arcade-tick per gerenderd frame, met
  OSG op max. 30 fps. Spelsnelheid hangt dus aan de framerate.

### Test: compileert de spellogica al voor de 68080?
Ja. Alle 89 bestanden in `src/ppengine/` compileren met
`m68k-amigaos-gcc -noixemul -O2 -m68080 -m68881` (GCC-6.50-Dev) **zonder
fouten**; er zijn 2 `-Wstrict-overflow` waarschuwingen in
`src/ppengine/sub/common/1572.c:29`. Geen 64-bit types, geen
endian-gevoelige casts gevonden. De Z8002 is net als de 68080 big-endian
("most significant data element accessed at lowest address" in `1572.c`).
De engine gebruikt `rand()` of tijd niet: hij is deterministisch.

## 2. Beoordeling: is een port naar de Vampire (SAGA) mogelijk?

**Ja, haalbaar.** De spellogica is direct bruikbaar; het werk zit in het
vervangen van OpenSceneGraph, Gorilla Audio en de OSG-invoer.

| Onderdeel | Apollo V4-tegenhanger | Haalbaarheid |
|---|---|---|
| Spellogica | ongewijzigd, C met ApolloCrossDev GCC | ✅ getest |
| Baangeometrie | `float`/FPU of vooraf berekende tabel | ✅ |
| 3D-rendering | **Maggie** via `maggie.library` op SAGA chunky 16-bit | ⚠️ grootste klus, zie risico's |
| Schaduwen, particles, alpha-blend | niet beschikbaar in Maggie → vervangen | ⚠️ versimpelen |
| HUD-tekst | eigen bitmapfont (8x8) naar chunky buffer | ✅ eenvoudig |
| Geluid | **Arne**: 16 kanalen, per kanaal periode (= pitch) en L/R-volume (= pan) | ✅ past 1-op-1 |
| Invoer | ApolloLib `ApolloKeyboard` / `ApolloJoypad` | ✅ |
| Timing | `ApolloWaitVBL`, vaste tick | ✅ |

### Risico's en beperkingen uit de ApolloKnowledgeBase
- **maggie.library** (`os/libraries/maggie-library.md`, status reviewed):
  - gedocumenteerd voor **AmigaOS 3.9**; niet voor ApolloOS of 3.2;
  - textures: vierkant, macht van 2 (32–1024), altijd DXT1, 1-bit alpha,
    **geen alpha-blending**, kleur = intensiteit + één RGB-tint per draw;
  - depth buffer is een 16-bit w-buffer;
  - zet FPCR op round-to-zero en vernietigt E-registers (D8–D31)
    (`os/pitfalls/maggie-library-side-effects.md`): FPCR zelf herstellen;
  - gebruikt BANK-prefixed FPU-instructies die toekomstige cores niet meer
    ondersteunen: controleren bij elke nieuwe core.
- **Core 12847 depth-test bug** (`core/pitfalls/maggie-depth-test-colour-write.md`):
  de kleur wordt altijd geschreven, ook als de depth-test faalt. Workaround:
  polygonen van achter naar voor sorteren (painter's algorithm). Bij deze
  scène (baan, ~8 auto's, borden) is dat goed te doen. Controleren of het
  op jouw Core 12856 al opgelost is.
- **Span-tail / span-lengte 0** (`core/pitfalls/maggie-span-tail.md`,
  `maggie-span-usage.md`): renderbuffers niet vrijgeven vlak na de laatste
  span; Maggie snoopt de datacache niet.
- **Saga-video en -audio** zijn in de KB maar deels gedocumenteerd; gebruik
  ApolloLib (`toolchain/apollolib/`) i.p.v. eigen registerwerk waar mogelijk.
- **Floats**: altijd `-m68080 -m68881` (anders soft-float, ~8x trager,
  `toolchain/pitfalls/float-settings-mismatch.md`). `sinf`+`cosf` van
  dezelfde waarde met `-ffast-math` kan een ICE geven, alleen opgelost in
  GCC-6.50-Dev (`toolchain/pitfalls/gcc-fsincos-ice.md`).

### Alternatief als Maggie tegenvalt
Het origineel was een **pseudo-3D** spel (baan per scanline + geschaalde
sprites). Een eigen CPU/AMMX-renderer in die stijl op de SAGA chunky plane
is volledig onder eigen controle en heeft geen last van de Maggie-bugs, maar
verlaat de 3D-look van deze remake. Advies: begin met Maggie, en houd de
renderlaag achter een eigen interface (`scene.h` bestaat al) zodat
omschakelen mogelijk blijft.

## 3. Takenlijst

Voorgestelde projectkeuzes (graag bevestigen):
- **Target OS**: AmigaOS 3.9 (Coffin) — vanwege maggie.library.
- **Execution mode**: `os-takeover` (spel; Maggie-samples doen dit ook).
- **Taal**: C (C++/OSG verdwijnt geheel).
- **Scherm**: 320x240 16-bit (GFXMODE `$0202`, 60 Hz) voor snelheid;
  later eventueel 640x360 (`$0B02`, 50 Hz).

### Fase 0 – Projectopzet
- [x] T01 `Projects/AGENTS.md`-template invullen voor ppengine (kind: game,
      mode: os-takeover, OS: AmigaOS 3.9, core 12856+).
- [x] T02 Nieuwe Makefile op basis van het ApolloCrossDev v1.2-template
      (`toolchain/project-makefile.md`), met ApolloLib, flags
      `-noixemul -O2 -m68080 -m68881`, link `libmaggie.a` vóór `-lamiga -lm`.
- [ ] T03 Bronbestanden die als C++ gecompileerd werden (`client.c`,
      `scene.c`, `track_geometry/*`, `vertex2d.c`, `audio.c`) geschikt maken
      voor C: `extern "C"`, `<cstdio>`, `bool`, `float(x)`-casts weg.
- [x] T04 Waarschuwingen in `src/ppengine/sub/common/1572.c:29` oplossen.
- [x] T05 Host-build (Linux, zonder graphics) behouden als referentie: een
      "headless" client die engine-state per frame dumpt, om later met de
      Amiga-versie te vergelijken (demo-modus is deterministisch).

Status fase 0 (2026-11-14):
- T01: `AGENTS.md` aangemaakt.
- T02: nieuwe `Makefile` (template v1.2 + submappen, `make compile`,
  header-afhankelijkheden, ApolloLib); de Linux-build heet nu
  `Makefile.linux`. Build: geen fouten, geen waarschuwingen.
- T03 (deels): `track_geometry/` en `geometry/` compileren nu als C.
  Gevonden: `abs()` op een `double` (in C++ een overload, in C een
  afronding naar `int`) → `fabs()` in `track_geometry.c`; `M_PI` met
  `#ifndef`. `client.c`, `scene.c` en `audio.c` worden in fase 1, 4 en 5
  vervangen en blijven tot dan C++.
- T04: de waarschuwing in `1572.c` was een echte bug: door int-promotie
  was `safe_to_change_gantry_appearance` altijd 0, dus het portaal wisselde
  nooit (laatste ronde / finish) en functie `0x1a` werd nooit in de wachtrij
  gezet. Nu 16-bit gerekend zoals de Z8002: het wisselen gebeurt in het
  frame waarin de speler positie `$C000` (3/4 ronde) passeert. **Dit
  verandert het spelgedrag t.o.v. de Linux-versie**; graag controleren in
  een volledige race.
- T05: `src/platform/headless/headless_main.c` → `make check` (host) en
  `build/ppengine_headless` (V4). Script: demo, munt bij frame 900,
  daarna rijden met sturen/schakelen/remmen. Host-resultaat na 7200 frames:
  `checksum 061ed1ef`, 9 geluidseffecten. **Op de V4 gedraaid
  (2026-11-14, AmigaOS 3.9, via amigent): uitvoer identiek aan de host**,
  inclusief de floating-point baangeometrie (3 decimalen). De engine en de
  baanberekening werken dus correct op de 68080.

### Fase 1 – Platformlaag en hoofdlus
- [x] T06 `main` met `ApolloTakeOver`/`ApolloRelease` (of het
      `main-os-takeover.c` skelet uit `skills/apollo-new-project/assets/`):
      multitasking en Workbench-scherm netjes herstellen bij afsluiten.
- [x] T07 Assets laden **vóór** de takeover (alleen `Open`/`Close`/
      `AllocMem` daarna, zie `os/os-takeover.md`).
- [x] T08 Vaste speltick: engine-tick koppelen aan VBlank
      (`ApolloWaitVBL`), los van de renderframerate; arcade-tickrate
      uitzoeken (60 Hz) en zo nodig ticks inhalen.
- [x] T09 SAGA-scherm: GFXMODE + chunky-pointer opslaan/zetten/herstellen
      (`$DFE1F4`/`$DFE1EC` lezen, `$DFF1F4`/`$DFF1EC` schrijven),
      triple buffering, flippen na VBlank.

Status fase 1 (2026-11-14, getest op de V4):
- Spel: `build/ppengine` uit `src/platform/amiga/`
  (`main_amiga.c`, `saga_display.c`, `keyboard.c`, `game_input.c`,
  `debug_view.c`).
- Test op de V4 (AmigaOS 3.9): spel gespeeld en met Esc afgesloten.
  Uitvoer: `5262 frames, 5261 engine ticks, 0 ticks dropped, 5261 VBlanks`
  (≈ 88 s op 60 Hz): elke VBlank precies één tick, geen haperingen.
  Workbench en Shell kwamen terug, geen herstart of guru.
- T06: `ApolloTimer` → `ApolloTakeOver` → `ApolloForbid`; bij afsluiten in
  omgekeerde volgorde `ApolloPermit` → `ApolloRelease` → `ApolloTimerStop`.
  Controle op 68080 en FPU vóór alles.
- T07: baangeometrie, debugweergave en schermbuffers worden vóór de
  takeover aangemaakt.
- T08: één arcade-tick per VBlank (60 Hz in 320x240), geteld via de
  interruptserver van ApolloLib (`GlobalTimer`) en niet via INTREQR:
  de interrupthandlers van het OS lopen onder `Forbid()` door en wissen
  die vlag ook. Gemiste ticks worden ingehaald (max. 4 per frame).
- T09: GFXMODE `$0202` (320x240 R5G6B5), dubbele buffer, wisselen direct na
  de VBlank. Mode, pointer en modulo worden vooraf uit `$DFE1xx` gelezen
  en bij afsluiten teruggezet; de muispointer wordt buiten beeld gezet en
  ook teruggezet. Dubbel i.p.v. drievoudig bufferen volstaat zolang er pas
  na de VBlank gewisseld wordt.
- T10 (vervroegd): eigen toetsenbordmatrix via `$BFEC01`, omdat
  `ApolloKeyboard` alleen de laatste toets geeft en het spel meerdere
  toetsen tegelijk nodig heeft.
- Tijdelijke debugweergave: baan van bovenaf met alle auto's, en balkjes
  voor snelheid, versnelling, spelstatus en credits. Wordt in fase 4
  vervangen door de Maggie-renderer.

### Fase 2 – Invoer
- [x] T10 `src/input/` herschrijven met `ApolloKeyboard`: cursortoetsen,
      `1` (credit), spatie (versnelling), Esc (stoppen). Edge-detectie
      (down/up) zelf doen, zoals de huidige event-queue.
- [~] T11 Optioneel: `ApolloJoypad` (USB-pad) en analoog sturen.

Status T11 (2026-11-14, joypad getest; DB9-joystick nog niet):
toetsenbord, USB-joypad (`$DFF220`) en DB9-joystick in poort 2 (`$DFF20C`,
vuurknop CIA-A `$BFE001` bit 7) werken tegelijk (`game_input.c`).
Joypad: kruis links/rechts sturen, A gas, B rem, X/Y versnelling, Start
munt. Joystick: links/rechts sturen, omhoog gas, omlaag rem, vuur
versnelling. De DB9-decodering en de vuurknop zijn klassieke Amiga-kennis
die de KB nog niet voor de V4 beschrijft. `INPUTTEST` toont 10 s lang de
registers en de herkende bediening; in rust: pad `$0001` (aangesloten),
DB9 `$0000`, CIA `$F6` (vuur los).
Test gebruiker: joypad werkt (A `$0003`, B `$0005`, X `$0009`, Y `$0011`).
Gevonden: knop A van de USB-joypad trekt ook de vuurlijn van poort 2 laag
(CIA `$76`), zodat gas geven ook van versnelling zou wisselen; nu telt de
DB9-vuurknop niet zolang A is ingedrukt. DB9-joystick zelf nog niet getest
(niet bij de hand).

### Fase 3 – Assets converteren (host-tools, Python)
- [x] T12 `.osgt` → eigen binair modelformaat (big-endian floats: posities,
      UV, indices, materiaal/texture-verwijzing, benoemde subnodes voor de
      portaallampen `Light1On`…). Polygonaantal per model meten en zo
      nodig reduceren.
- [x] T13 Textures naar vierkant macht-van-2 (≤ 256 aanbevolen) en
      voorgecomprimeerd **DXT1** (`.dds`); transparante pixels via 1-bit alpha.
- [x] T14 Lucht/bergen: `bg.jpg` en `mountains...png` als 2D-achtergrond
      (scrollende strip in chunky buffer) i.p.v. 3D-cilinder.
- [x] T15 Font `prstartk.ttf` → 8x8 bitmapfont (of rechtstreeks de
      arcade-tiles).
- [x] T16 Geluid: `f1sound1.ogg` en `skidding.wav` → 16-bit mono PCM WAV
      (big-endian na laden; ApolloLib swapt WAV sinds PR #25). Geen 8-bit
      unsigned WAV gebruiken (ApolloLib converteert die niet).
- [x] T17 Baangeometrie: optie A – runtime in `float` (65 536× `atan2` op
      de FPU, meten); optie B – tabel offline berekenen en als binair
      bestand meeleveren (≈ 1,8 MB in float). `double` → `float` in
      `vertex2d`/`segments`.

Status fase 3 (2026-11-14): `make assets` → `tools/convert_assets.py`
(alleen python3 + ffmpeg) schrijft `build/data/`; de bestandsformaten staan
bovenin het script.
- T12: 15 modellen → `.ppm`, transformaties ingebakken, onderdelen per
  (naam, texture, kleur) samengevoegd, polygonen als indexlijst met `$FFFF`
  (formaat van `magDrawIndexedPolygons`). Auto: 12 onderdelen,
  602 polygonen; bord: 154–294; startportaal: 879, met de lampen als
  aparte onderdelen `Light1On`…`Light4Off`. Bijna alle materialen hebben
  `GL_LIGHTING OFF`: Maggie-belichting is niet nodig.
- T13: 10 modeltextures + weg, startstreep, explosie → DXT1 `.dds` met
  volledige mip-keten (256x256, explosie 64x64), plus `white.dds` voor
  onderdelen zonder texture (maggie.library tekent niets zonder gebonden
  texture). Terug-gedecodeerd en visueel vergeleken met de bron: gelijk.
  **Nog te checken in fase 4:** of `magUploadTexture` DXT1 in de
  DDS-bytevolgorde (little-endian) verwacht, zoals de samples die `.dds`
  laden.
- T14: bergen als 2D-strook 2048x64 R5G6B5 (`gfx/mountains.565`).
  `images/bg.jpg` wordt nergens in de code gebruikt en is overgeslagen.
- T15: `font8x8.bin`, ASCII 32–127 uit `prstartk.ttf`, 1 bit per pixel.
- T16: `sounds/engine.wav` (12,9 s) en `sounds/skidding.wav`, 16-bit mono
  22050 Hz. WAV is little-endian; ApolloLoadSound zet dat om (PR #25).
- T17: baan blijft runtime-berekend: de headless test met 1 frame (baan
  bouwen + afsluiten) duurt op de V4 < 0,8 s inclusief netwerk. Geen
  tabel nodig; `double` blijft tot de renderer `float` nodig heeft.

### Fase 4 – Renderer met Maggie (vervangt `src/scene/osg/`)
- [x] T18 Nieuwe `src/scene/maggie/` die dezelfde functies levert als
      `scene/osg/scene.h` (`osg_scene_open`, `..._set_moving_object_attributes`,
      `..._set_camera_position`, `..._set_gantry_light_illumination`, …).
- [x] T19 maggie.library initialiseren (statisch: `magCreateContext`),
      `magSetScreenMemory`, 32-byte uitgelijnde buffers, FPCR
      opslaan/herstellen rond elke frame, geen E-registers in eigen code
      over `mag*`-aanroepen heen.
- [x] T20 Camera/projectie met `mat4_perspective` (w-buffer); camera
      22 eenheden achter de speler op hoogte 4,5 zoals nu.
- [x] T21 Baan: alleen de zichtbare segmenten rond de speler als quads
      (`magDrawIndexedPolygons`), met wegtextuur en rand.
- [x] T22 Auto's (4 lakvarianten + puddle), 24 borden en het startportaal
      met lampen; per object één world-matrix.
- [x] T23 Painter's sort (achter→voor) als workaround voor de depth-test
      bug; eerst testen of Core 12856 de bug nog heeft.
- [x] T24 HUD-overlay: 32x28 tekenrooster na de 3D-pass direct in de
      chunky buffer tekenen (kleur per attribuut, 6 kleuren uit
      `text_matrix.c`).
- [x] T25 Titelscherm en Game Over als 2D-bitmaps (geen alpha-blend:
      colour-key).
- [x] T26 Vervangers voor wat Maggie niet kan: schaduw = donkere quad
      onder de auto; explosie = geanimeerde sprite (`magDrawSprites`) of
      2D-overlay.

Status fase 4 (2026-11-14): Maggie-renderer `src/scene/maggie/`
(`maggie_backend.c`, `assets.c`), `src/scene/scene.c` nu in C met een
backend-interface (`src/scene/backend.h`). Getest op de V4 via de
`SNAPSHOT`-modus (rendert zonder takeover naar `RAM:ppsnap_<tick>.565`):
weg, auto's, borden, startportaal met lampen, bergen, HUD en GAME OVER
kloppen. Het spel zelf (takeover) moet nog door de gebruiker getest worden.
- T18/T19: maggie.library 4.0 gedeeld geopend; textures per mip-niveau
  (`magUploadTexture` kopieert DXT1 byte voor byte, DDS-layout klopt);
  FPCR na elke frame hersteld; Apollo-bit gezet; bij afsluiten een
  "flush"-span naar een nooit vrijgegeven scratchbuffer (span-tail pitfall).
- T20: eigen view-matrix (x rechts, y omlaag, z vooruit); de standaard
  wikkelrichting van Maggie klopt met de OSG-modellen. FOV 39,3°
  horizontaal (= OSG's 30° verticaal bij 4:3).
- T21: weg als vaste vertexbuffer, per frame een venster: 6 quads achter,
  40 vooruit, dan 16 grove quads (4x langer).
- T22/T26: auto's, borden, portaal (lampen als aparte onderdelen),
  plas als donkere quad, explosie als sprite.
- T23: **Core 12856 heeft de dieptebug niet meer**: test `DEPTHTEST`
  (nabije auto eerst, verre erna) toont de nabije auto ongeschonden. De
  objecten worden toch van ver naar dichtbij getekend.
- T24/T25: HUD 32x28 tekens met 8x8-font op (32,8); titelscherm grijs;
  GAME OVER-plaatje met kleursleutel.
- Snelheid (T30/T31 vervroegd): maggie.library kost ~10 µs CPU per polygoon
  + ~0,6 ms per model (meting `BENCH`); pixelinstellingen (bilinear,
  diepte, affine) maken geen verschil. Daarom: gedeelde vertices
  (auto 2126 → 751), onzichtbare onderkanten weg, 4 detailniveaus
  (auto 589/289/165/106 polygonen, grens 28/70/150 eenheden), grove weg
  in de verte, snellere achtergrond. Resultaat per frame: 15–33 ms
  (startopstelling met 8 auto's ≈ 33 ms), was 29–95 ms.
- Testopties: `CCW`, `NOBILINEAR`, `NODEPTH`, `AFFINE`, `NEARFIRST`,
  `BENCH`, `DEPTHTEST`, `SNAPSHOT t1 t2 ...`.
- Eerste speltest door de gebruiker (2026-11-14): toetsenbord reageerde
  slecht en het beeld flikkerde. Oorzaak: toetsen lezen en buffers wisselen
  zaten in de wachtlus vóór de VBlank, die wordt overgeslagen als een frame
  langer duurt dan 16,7 ms (gemiddeld 18 ms, slechtste 67 ms); het spel
  tekende dan in de zichtbare buffer. Opgelost met interruptservers
  (`irq_servers.s`): toetsenbordbytes uit `$BFEC01` via PORTS + VBlank in
  een ringbuffer, en drie buffers waarvan de VBlank-server de klaargezette
  buffer op het scherm zet. Hertest door de gebruiker: toetsen reageren
  goed, flikkeren is weg, geen haperingen.
- Tweede opmerking van de gebruiker: startportaal (palen) en reclameborden
  verloren te veel detail. Oorzaak: ze bestaan uit staafjes van ~0,1
  eenheid, die bij cellen van 0,3+ samenvallen. Opgelost: het decor heeft
  eigen, fijnere detailniveaus (cellen 0,05/0,1/0,2, grens 25/50/150
  eenheden); auto's houden 0,3/0,5/0,7. Vergeleken met `FULLDETAIL`:
  bij de start visueel gelijk. Kosten: start ≈ 36 ms per beeld (was 31),
  tijdens het racen 14–20 ms.
- Derde opmerking: de palen bij de start werden nog deels getekend. Ook
  het volledige model doet dat: de vakwerkstaafjes zijn op afstand dunner
  dan een pixel en Maggie tekent zulke polygonen maar gedeeltelijk.
  Opgelost in de converter (`solidify_lattices`): in de detailniveaus 1–3
  wordt elk langwerpig, plat vakwerkpaneel (palen van het portaal, poten
  van de borden) één massief vlak in dezelfde kleur; vierkante panelen
  (de rode X-vakken) blijven. Portaal 879 → 186/174/65 polygonen, bord
  153 → 17. Start nu ≈ 31 ms per beeld, decor 3,5 ms.
- De Linux-build compileert `scene.c` niet meer zonder een kleine
  OSG-adapter voor `scene/backend.h` (niet gemaakt).

- Opmerking gebruiker: in de demo stond de eerste rij voorbij de startlijn.
  Klopt (berekend met de engine-toestand op tick 301): de eerste rij staat
  op segment 0 (daar telt de engine de rondes), de startlijn-quad en het
  portaal stonden op segment 65504–65535 (zo ook in de Linux-versie), en het
  automodel reikt 3,7 eenheden (≈ 24 segmenten) voor zijn oorsprong. Nu:
  startlijn-quad 1 (segment 32–63) en portaal op segment 32, net voorbij de
  neuzen van de eerste rij (`START_LINE_QUAD` in `maggie_backend.c`,
  `scene.c`). De engine telt de ronde nog steeds op segment 0.

- Hogere resolutie (verzoek gebruiker, kartels aan de wielen): standaard
  nu 640x480 R5G6B5 op 60 Hz (GFXMODE `$0502`, zelfde 28 MHz-pixelklok als
  `$0202`; speltik, beeldverhouding en beeldveld blijven gelijk);
  `LOWRES` geeft 320x240. HUD wordt 2x geschaald. Kosten eerst 33–58 ms
  per beeld; daarna:
  - achtergrond 14 → 2 ms: bergenstrook één keer op schermschaal
    voorbereid (≈ 5850 x 175, 2 MB), per rij één `ApolloCopyBlock`; gras
    met `ApolloFillBlock` (AMMX);
  - HUD 3 → 0,5 ms: 2x2 per fontpixel met 32-bit stores; GAME OVER met
    kolomtabel;
  - weg zonder dieptebuffer: met bilinear én dieptebuffer tekende Maggie
    rood-witte streepjes op de naden tussen de wegquads (ook in 320x240,
    kleiner); elk van de twee alleen gaf een schoon beeld.
  Resultaat 640x480: racen 17–22 ms, start ≈ 39 ms (was in 320x240
  14–20 en 31 ms).
  Speeltest gebruiker in 640x480: 89 % van de beelden binnen 2 VBlanks
  (≈ 30 fps), 1 tick verloren. De kartels aan de banden bleven: ingezoomd
  zijn het inkepingen langs de rand van de zwarte cilinders (16 smalle
  vlakjes per band), geen resolutietrapjes. Poging: banden in de converter
  vervangen door achthoekige prisma's; resultaat niet beter (blauwe velgen
  verdwenen, nieuwe inkeping) en teruggedraaid.
- Draaiende wielen (verzoek gebruiker): de converter verdeelt in het
  volledige automodel de 16 loopvlakken per band in vier groepen
  (`Tread0`–`Tread3`, sector rond de as modulo 4); de renderer kleurt één
  groep donkergrijs en schuift die door met de afgelegde afstand (0,19
  eenheid per vlakje, maximaal 1,5 vlakje per beeld tegen het
  wagenwiel-effect). Alleen zichtbaar bij auto's binnen 28 eenheden
  (volledig model), dus de eigen auto en wie vlakbij rijdt. Getest door de
  gebruiker: werkt, maar ziet er niet goed uit; **teruggedraaid**.
  640x480 blijft standaard, `LOWRES` voor 320x240.
- Gaten in de auto's (gebruiker: middellijn zichtbaar door de achterkant):
  (1) het weglaten van naar beneden gerichte vlakken staat uit, alle
  polygonen blijven; (2) de grootste oorzaak was het grove raster van de
  detailniveaus (0,3/0,5/0,7), dat dunne delen liet samenvallen. Gemeten bij
  de start (640x480, tick 302), vergeleken met `FULLDETAIL`:
  - 0,3/0,5/0,7 (grens 28/70/150): 39 ms, gaten;
  - 0,1/0,2/0,4 (grens 40/90/180): 72 ms, gelijk aan volledig model;
  - **0,15/0,25/0,4 (grens 30/70/150): 51 ms, geen zichtbare gaten** (nu
    actief). Racen blijft 19–22 ms.
  Speeltest gebruiker: gaten weg, en de banden zien er een stuk beter uit:
  de kartels aan de banden kwamen dus grotendeels van het grove raster,
  niet (alleen) van de 16 dunne loopvlakken zoals eerder gedacht.
- Witte balk in de lucht (gebruiker, bij bochten naar links): de lucht boven
  de bergenstrook was de bovenste rij van de strook, omhoog doorgetrokken;
  op kolom 1746–1747 raakt een wolk die bovenrand, wat opgeschaald een
  witte balk van ±6 pixels gaf tot aan de wolk. Nu: de lucht is één kleur,
  de meest voorkomende van de bovenste rij (`$2B9F`). Gecontroleerd op 10
  beelden verspreid over de baan: de lucht naast de HUD is overal één kleur.
- Origineel vakwerk van portaal en borden in 640x480 (vraag gebruiker): in
  640x480 tekent Maggie de staafjes van 0,1 eenheid wel heel (in 320x240
  niet: daar zijn ze voorbij ±45 eenheden dunner dan een pixel). Het
  originele model blijft in 640x480 nu tot 90 eenheden staan (decorgrenzen
  90/150/250; `LOWRES` houdt 25/50/150). Kosten: start 47 → 55 ms, portaal
  naderen 22 → 32 ms, elders 16–21 ms; de X-vakken naast de START-banner
  zijn weer heel (op afstand werden ze massief).
- Workbench-pictogram (verzoek gebruiker): `tools/make_icon.py` (via `make
  assets`) schrijft `build/ppengine.info`: klassiek 4-kleurenpictogram
  (raceauto van achteren), stack 128 KB, tooltype `(LOWRES)`. Bij een start
  vanaf de Workbench leest `main_amiga.c` de tooltypes (icon.library) en gaan
  meldingen naar een `CON:`-venster dat pas opengaat als er iets te melden
  is. Getest door de gebruiker: start vanaf de Workbench werkt.

### Fase 5 – Geluid op Arne (vervangt `src/audio/gorilla/`)
- [x] T27 `src/audio/arne/` met dezelfde functies als `gorilla/audio.h`.
      Kanalen 0–7 motorgeluiden, 8 slipgeluid, rest effecten.
- [x] T28 Pitch → periodewaarde (`3546895 / samplerate`, geschaald met
      `0.5 + pitch/65536/128`), pan → L/R-volume, gain → volume
      (`ApolloVolumeSound`), loop aan.
- [x] T29 Ontbrekende effecten (explosie, aftelpiepjes) toevoegen via
      `audio_play_effect` (optioneel, nu ook in het origineel niet af).

Status fase 5 (2026-11-14, getest op de V4):
- `src/audio/arne/arne_audio.c` achter `src/audio/backend.h`;
  `src/audio/audio.c` (spellogica van het geluid) ongewijzigd op de
  functienamen na. Eigen WAV-lader (little → big-endian), samples 32-byte
  uitgelijnd in Fast RAM (Arne doet DMA uit elk RAM).
- Kanalen 6–13: motorgeluiden (7 CPU-auto's + speler), 14: slipgeluid;
  16-bit mono in een lus (CTRL `$0001`), gestart met DMACON2. 0–3 (Paula)
  en 4–5 blijven vrij voor het OS/AHI.
- Toonhoogte: zelfde formule als Gorilla, factor 0,5 + (pitch >> 16)/128,
  als periode 3546895 / (22050 × factor), minimaal 59 (60 kHz).
  Pan: zijdelingse afstand tot de speler, ±1800 = helemaal links/rechts
  (de Gorilla-versie gaf negatieve waarden door als uint16: altijd rechts).
- `SOUNDTEST`-modus (zonder takeover): DMACON2R `$0200` (kanaal 13) bij
  motor aan, `$0400` (kanaal 14) bij slippen, `$0000` na afloop.
- T29 (explosie/aftelgeluiden): ook in de Linux-versie niet gemaakt; open.
- Luistertest gebruiker: in het spel klinkt het juiste geluid. In de demo
  is het stil: zo bedoeld door de spellogica (`engine_sound_active` is 0
  buiten een spel; de Linux-versie was ook stil).
- `SOUNDTEST` gaf de eerste keer geen geluid. Het vermoeden (de AHI-driver
  `arne.audio` maakt Arne stil zolang het OS draait) bleek **onjuist**: de
  hertest met multitasking aan (`SOUNDTEST NOFORBID`) klinkt goed; de eerste
  keer lag het aan de testopstelling. `SOUNDTEST` werkt met en zonder
  `Forbid()`.

### Fase 6 – Prestaties en testen op de V4
- [ ] T30 Meten met `make static PROFILE=1` (maggie.library) en
      `ApolloCPUTick`; doel ≥ 30 fps renderen bij 60 Hz logica.
- [ ] T31 Optimaliseren: `MAG_DRAWMODE_AFFINE_MAPPING` voor kleine/verre
      objecten, uploaded buffers i.p.v. user pointers, LOD voor verre auto's
      en borden, kleinere textures.
- [ ] T32 Vergelijkingstest: demo-modus op host en V4 per frame de
      engine-state laten dumpen (seriële debug, `toolchain/apollolib/debug.md`)
      en vergelijken.
- [ ] T33 Afsluiten testen: scherm, multitasking, Maggie-buffers (span-tail
      pitfall) en audio-DMA netjes terug; daarna nog een Maggie-programma
      starten om geheugenbeschadiging uit te sluiten.
- [~] T34 Lessons voor de ApolloKnowledgeBase opstellen (o.a. Maggie-bug op
      Core 12856, Arne-pitch voor motorgeluid).

Status fase 6 (2026-11-14, deels; speltest door de gebruiker nodig):
- T29: effecten zonder samples, bij het opstarten gegenereerd en eenmalig
  op kanaal 15: startlicht (440 Hz, callback 12), groen (880 Hz, 13),
  extended play (3 piepjes, 7), bonus-tik (`sample_player_2` bit 7/1,
  daarna gewist), explosie van de speler (gefilterde ruis, de al
  voorbereide maar uitgecommentarieerde plek in `audio.c`). Alleen tijdens
  een spel (`jump_table_function_number == 3`), zoals de motorgeluiden.
  Stemsamples: de gebruiker leverde "Prepare to qualify" (SOUND 19) en het
  crashgeluid (SOUND 17) aan; nu `sounds/prepare_to_qualify.wav` en
  `sounds/crash.wav`. De stem klinkt op kanaal 5 wanneer de engine
  `sample_player_1 = 2` zet (begin van een spel, frame handler 66a, toestand
  3/1, gecontroleerd met de host-build); de crash vervangt de gegenereerde
  explosie (die blijft als reserve zonder het bestand). Andere waarden van
  `sample_player_1` (3, 4, bit 5) vragen samples die er niet zijn.
  Te horen via `SOUNDTEST`.
- T30: het spel meldt bij Esc ook welk deel van de beelden binnen 1, 2, 3
  of meer VBlanks klaar was.
- T32: gedaan in fase 0 (headless checksum host = V4); de engine is sinds
  dien niet gewijzigd (checksum `061ed1ef`).
- T33: ppengine zelf laat per run 544 bytes achter: de bewust nooit
  vrijgegeven scratchbuffer voor de Maggie-span-flush. Twee
  Maggie-runs na elkaar: geen problemen. **Gevonden: maggie.library 4.0
  lekt 128 KB per laad/verwijder-cyclus** (`Avail FLUSH`, of geheugentekort):
  `sizeof(MaggieBase)` = 158 354 bytes past niet in het 16-bit
  `lib_PosSize`, en `maggieExpunge` geeft `lib_NegSize + lib_PosSize` vrij
  (gemeten: precies 131 072 bytes per cyclus; zonder FLUSH blijft de
  library geladen en lekt er niets). Fout in maggie.library, niet in
  ppengine; melden bij RMortenB/MaggieLibrary (met akkoord gebruiker).

## 4. Gebruikte KB-bestanden
`AGENTS.md`, `os/os-takeover.md`, `os/libraries/maggie-library.md`,
`os/pitfalls/maggie-library-side-effects.md`,
`core/saga-3d-maggie/index.md`, `core/saga-3d-maggie/registers.md`,
`core/pitfalls/maggie-*.md`, `core/saga-registers/chunky-plane.md`,
`core/saga-audio-arne/index.md`, `core/saga-video-isabelle/index.md`,
`toolchain/apollolib/{index,sound,input}.md`,
`toolchain/pitfalls/float-settings-mismatch.md`,
`toolchain/pitfalls/gcc-fsincos-ice.md`,
`core/pitfalls/fpu-broken-instructions.md`.

Leemtes in de KB: de SAGA-video en Arne-mappen zijn grotendeels nog leeg,
en maggie.library is alleen voor AmigaOS 3.9 beschreven.
