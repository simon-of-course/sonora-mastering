## SONORA VST3 v0.2.0 – Rap Overhaul

Drei Windows-x64-VST3-Plugins für Rap, Trap, Drill und Boom Bap in FL Studio – komplett neu gebaut.

### SONORA Master
- Alle drei Bus-Modi verarbeiten jetzt wirklich: **Vocal** (De-Esser, Nasalität, Resonanzen, Präsenz, Vocal-Kompressor), **Beat** (Vocal-Sidechain-Ducking, 808-Punch, Glue, Bass-Mono, Stereo-Breite), **Master** (komplette Kette + Lautheit, oversampelter Clipper, Look-ahead-True-Peak-Limiter mit Ceiling).
- 12 Presets, neu: **Trap 808, Drill, Boom Bap, Cloud Rap, Vocal Upfront, Club Loud** – laut (auf einem −14-LUFS-Testmix ≈ −8 bis −10 LUFS bei −1 dBTP), ohne die Stimme zu verlieren (1–4 kHz bleibt mindestens gleich, Vocal Upfront +1–2 dB).
- LUFS momentan/kurzzeitig/integriert, True Peak, Pegel- und Gain-Reduction-Anzeigen, echte EQ-Kurve, 8-s-Analyse.
- Konstante Latenz (135 Samples @ 48 kHz) – FL Studio gleicht sie automatisch aus.
- Projekte aus v0.1 laden weiter (Reglerwerte werden auf die neue Skalierung umgerechnet).

### SONORA Shutter
- 12 Notenwerte (inkl. Triolen/punktiert), Pulsbreite, Swing, Versatz, Form Hart/Weich/Pumpe, Ping-Pong-Stereo, Gate-Klang (Stumm/Telefon/Dumpf). Sample-genau im Takt.

### SONORA Drowner
- Steiler 24-dB/Okt.-Unterwasserfilter mit Resonanz, Wobble, Wasserhall, Körper, Abtauch-/Auftauchzeit. Bei 0 % bitgenau clean.

### Alle Plugins
- Neues dunkles Interface mit Live-Werten und Einheiten an jedem Regler, Host-Bypass für FL Studio.
- Geprüft: Steinberg-Validator 47/47 je Plugin, Unit-Tests, Host-Check (State-Migration, Latenz, Ceiling, sample-genaue Automation), Windows-MSVC-Build ohne zusätzliche Runtime.

**Installation:** ZIP entpacken, den kompletten Ordner `SONORA_….vst3` nach `C:\Program Files\Common Files\VST3` kopieren, in FL Studio *Options → Manage plugins → Find installed plugins*.

---
*English:* Complete v0.2 rewrite of SONORA Master, Shutter and Drowner for rap production: real per-mode processing, six new rap presets, true-peak limiter with LUFS metering, tempo-synced gate with swing/pulse width, steep underwater filter, new dark UI. v0.1 projects load and are migrated.
