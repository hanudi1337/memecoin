# ps5view – PS5-Bild und -Ton als Fenster für Discord (nur Zuschauen)

`ps5view` ist ein schlankes Remote-Play-Programm für Windows 10/11. Es zeigt das Bild
und den Ton deiner PS5 in einem frei skalierbaren Fenster. Dieses Fenster kannst du in
Discord per „Bildschirm teilen“ streamen, während du selbst ganz normal direkt an der
PS5 spielst (Controller an der PS5, Bild per HDMI auf deinem Monitor).

* **Nur Zuschauen:** ps5view sendet keine Controller-, Tastatur-, Maus- oder
  Touch-Eingaben an die PS5. Im Fenster bewirken Tasten nur F11/Alt+Enter (Vollbild) und Esc.
* **Ton auf ein beliebiges Gerät:** z. B. „CABLE Input (VB-Audio Virtual Cable)“, damit
  du den Ton selbst nicht doppelt hörst, Discord ihn aber mitüberträgt. Die Lautstärke wird
  nie verändert (100 %).
* **Robust und gesprächig bei Fehlern:** ausführliches Log (Datei + Konsole), verständliche
  deutsche Meldungen mit konkreten Hinweisen, begrenzte Neuversuche statt Endlosschleife,
  Diagnosebefehl `ps5view check`.
* Basiert auf **libchiaki** aus [chiaki-ng](https://github.com/streetpea/chiaki-ng)
  (AGPL-3.0). Das Sony-Protokoll wird nicht nachgebaut.

> **Hinweis zum Controller:** Jede Remote-Play-Verbindung meldet an der PS5 einen
> virtuellen Controller an – das verlangt das Protokoll. ps5view sendet für diesen
> Controller ausschließlich den Ruhezustand (keine Taste gedrückt, Sticks in der Mitte)
> und meldet ihn standardmäßig als DualShock 4 an (kein Haptik-/Trigger-Kanal). Dein echter
> Controller funktioniert ganz normal weiter. Zeigt die PS5 deshalb einen Hinweis zum
> DUALSHOCK 4 an, kannst du mit `--controller dualsense` (wie bei chiaki-ng üblich) umschalten.

---

## Inhalt

1. [Einrichtung Schritt für Schritt](#1-einrichtung-schritt-für-schritt)
2. [In Discord streamen](#2-in-discord-streamen)
3. [Befehle und Optionen](#3-befehle-und-optionen)
4. [Konfiguration, Profile, Logs](#4-konfiguration-profile-logs)
5. [Troubleshooting](#5-troubleshooting)
6. [Bauen](#6-bauen)
7. [Lizenz und Quellcode](#7-lizenz-und-quellcode)
8. [Technischer Hintergrund](#8-technischer-hintergrund)

---

## 1. Einrichtung Schritt für Schritt

### 1.1 PS5 vorbereiten (einmalig)

* **Remote Play aktivieren:** Einstellungen > System > Remote Play > „Remote Play aktivieren“.
* **Aufwecken aus dem Ruhemodus erlauben** (für `--wake`):
  Einstellungen > System > Energiesparen > Im Ruhemodus verfügbare Funktionen >
  „Mit dem Internet verbunden bleiben“ und „Einschalten der PS5 über das Netzwerk ermöglichen“.
* **Feste IP-Adresse** für die PS5 im Router vergeben (z. B. 192.168.178.88 in der FRITZ!Box:
  Heimnetz > Netzwerk > Gerät bearbeiten > „Diesem Netzwerkgerät immer die gleiche IPv4-Adresse zuweisen“).
  Sonst kann sich die Adresse nach einem Neustart ändern.
* PC und PS5 müssen im **selben Heimnetz** sein (kein Gast-WLAN, keine WLAN-Client-Isolation).

### 1.2 Programm besorgen

* Fertige Windows-Version: In GitHub unter *Actions* > Workflow „ps5view“ > letzter
  erfolgreicher Lauf > Artefakt **ps5view-windows-x64** herunterladen und entpacken
  (enthält `ps5view.exe`, alle DLLs, Lizenzen).
* Oder selbst bauen, siehe [Abschnitt 6](#6-bauen).

ps5view ist ein Kommandozeilenprogramm. Öffne im entpackten Ordner eine **PowerShell**
(Shift + Rechtsklick > „PowerShell-Fenster hier öffnen“) oder eine Eingabeaufforderung.

Beim ersten Start fragt die Windows-Firewall, ob ps5view kommunizieren darf:
**„Private Netzwerke“ erlauben.** Da die exe nicht signiert ist, kann Windows SmartScreen
„Der Computer wurde durch Windows geschützt“ anzeigen: „Weitere Informationen“ > „Trotzdem ausführen“.

### 1.3 PS5 finden: `discover`

```powershell
.\ps5view.exe discover
```

Beispielausgabe:

```
1 Konsole(n) gefunden:

  Adresse:       192.168.178.88
  Name:          PS5-123
  Typ:           PS5
  Status:        bereit (ready)
  Systemversion: 8.05 (08050001)
```

Antwortet nichts, kannst du die IP direkt abfragen: `.\ps5view.exe discover --host 192.168.178.88`.

### 1.4 PSN-Account-ID ermitteln: `account-id`

```powershell
.\ps5view.exe account-id
```

1. Der Browser öffnet die Sony-Anmeldung. Melde dich mit **dem PSN-Konto an, das an der
   PS5 angemeldet ist**.
2. Danach landet der Browser auf einer fast leeren Seite (oft nur „redirect“). Kopiere die
   **komplette Adresse** aus der Adresszeile (beginnt mit
   `https://remoteplay.dl.playstation.net/remoteplay/redirect?code=...`).
3. In ps5view einfügen und Enter drücken – **zügig**, der Code ist nur kurz und nur einmal gültig.

Ergebnis: `Account-ID:  xxxxxxxxxxx=` (8 Byte, base64). Diese ID ist kein Geheimnis wie
ein Passwort, aber sie identifiziert dein Konto.

### 1.5 Konsole registrieren: `register`

An der PS5: **Einstellungen > System > Remote Play > Gerät verknüpfen**. Es erscheint eine
8-stellige PIN. Den Bildschirm offen lassen und sofort:

```powershell
.\ps5view.exe register --host 192.168.178.88 --account-id xxxxxxxxxxx= --pin 12345678
```

Die Schlüssel werden als **Profil** gespeichert (Name = Konsolenname, z. B. `PS5-123`).
Für ein zweites PSN-Konto einfach nochmal registrieren (mit dessen Account-ID) –
es entsteht ein zweites Profil (`PS5-123-2` oder per `--name zweitkonto`).

### 1.6 Audiogerät wählen: `list-audio`

Für „ich höre den Ton nicht doppelt, Discord aber schon“ eignet sich
[VB-Audio Virtual Cable](https://vb-audio.com/Cable/) (kostenlos). Nach der Installation:

```powershell
.\ps5view.exe list-audio
```

```
Audio-Ausgabegeräte (SDL-Audiotreiber: wasapi):
  "Lautsprecher (Realtek(R) Audio)"   [Standardgerät]
  "CABLE Input (VB-Audio Virtual Cable)"
```

Optional prüfen, ob der Ton dort ankommt (Pegel bei „CABLE Output“ unter
Windows-Soundeinstellungen > Aufnahme, und nicht aus deinen Lautsprechern):

```powershell
.\ps5view.exe list-audio --test-tone "CABLE Input"
```

### 1.7 Streamen: `stream`

```powershell
.\ps5view.exe stream --audio-device "CABLE Input" --wake --save
```

* `--audio-device` akzeptiert den vollen Namen oder einen eindeutigen Teil davon.
* `--wake` weckt die PS5 aus dem Ruhemodus und wartet, bis sie bereit ist.
* `--save` merkt sich die angegebenen Optionen im Profil – danach genügt `.\ps5view.exe stream`.

Fenster: frei skalierbar (Bild bleibt im richtigen Seitenverhältnis), **F11**, **Alt+Enter**
oder **Doppelklick** = Vollbild, **Esc** = Vollbild verlassen, **Fenster schließen** oder
**Strg+C** in der Konsole = Verbindung sauber beenden.

---

## 2. In Discord streamen

1. ps5view-Fenster auf Monitor 2 offen lassen (darf von anderen Fenstern verdeckt sein,
   aber **nicht minimieren** – minimierte Fenster liefern kein Bild).
2. In Discord: Sprachkanal > **Bildschirm teilen** > Reiter **Anwendungen** > Fenster
   „ps5view – <Profil> – …“ auswählen > **Ton** einschalten > Live gehen.
3. Discord überträgt beim Teilen eines Anwendungsfensters den Ton dieser Anwendung.
   Ausgegeben wird er auf dem gewählten Gerät (z. B. CABLE Input) – du hörst ihn also nicht,
   die Zuschauer schon.

Kein Ton bei den Zuschauern? Siehe [Kein Ton in Discord](#kein-ton-in-discord--ton-trotzdem-selbst-hörbar).

---

## 3. Befehle und Optionen

Globale Optionen (an beliebiger Stelle): `-v`/`--verbose` (mehr Details inkl. libchiaki),
`--trace` (zusätzlich Hexdumps – **enthalten Schlüssel**, nicht weitergeben),
`--config <datei>`, `--log-file <datei>`, `--no-log-file`. Hilfe zu jedem Befehl: `ps5view <befehl> --help`.

| Befehl | Zweck |
|---|---|
| `discover [--host <ip>]... [--timeout <s>]` | PS5/PS4 im Netz suchen: IP, Name, Status (bereit/Ruhemodus), Systemversion, laufendes Spiel |
| `account-id [--no-browser] [--redirect-url <url>]` | PSN-Account-ID per Sony-Login ermitteln |
| `register --host <ip> --account-id <id> --pin <8 Ziffern> [--name <profil>] [--default] [--ps4]` | Konsole registrieren, Profil speichern |
| `profiles [--default <profil>]` | Profile anzeigen (ohne Schlüssel), Standardprofil setzen |
| `remove-profile --profile <profil>` | Profil löschen |
| `stream [--profile <profil>] [optionen]` | Verbinden und Fenster anzeigen |
| `list-audio [--test-tone <gerät>] [--seconds <n>]` | Audio-Ausgabegeräte auflisten / Testton |
| `check [--profile <profil>] [--host <ip>]` | Diagnose aller Voraussetzungen |
| `version`, `license` | Version / Lizenz und Bezugsquelle des Quellcodes |

Optionen von `stream`:

| Option | Standard | Bedeutung |
|---|---|---|
| `--resolution 360p\|540p\|720p\|1080p` | 1080p (PS4: 720p) | Auflösung des Streams |
| `--fps 30\|60` | 60 | Bildrate |
| `--bitrate <kbit/s>` | 720p: 10000, 1080p: 15000 | Videobitrate (1000–100000) |
| `--codec h264\|h265` | h264 | H.264 ist am kompatibelsten; H.265 spart Bandbreite |
| `--audio-device <name>` | Windows-Standardgerät | exakter Name oder eindeutiger Teil, `default` = Standardgerät |
| `--no-audio` | – | ohne Ton |
| `--wake` / `--no-wake` | aus | PS5 aus dem Ruhemodus aufwecken |
| `--wake-timeout <s>` | 60 | so lange auf das Hochfahren warten |
| `--retries <n>` | 5 | Verbindungsversuche (1 = keine Wiederholung) |
| `--login-pin <ziffern>` | – | Anmelde-Passcode des PS5-Benutzers, falls gesetzt |
| `--hwdec auto\|none\|d3d11va\|dxva2` | auto | Hardware-Decoder (auto: d3d11va, dann dxva2, sonst Software) |
| `--renderer direct3d11\|direct3d\|opengl\|software` | automatisch | SDL-Renderer (bei Darstellungsproblemen wechseln) |
| `--window-size <BxH>` | 1280x720 | Startgröße des Fensters |
| `--display <n>` | – | Fenster auf Bildschirm n öffnen; `ps5view check` listet die Bildschirme mit ihrem Index |
| `--fullscreen` | – | im Vollbild starten |
| `--controller ds4\|dualsense` | ds4 | Typ des (immer unbetätigten) Controllers, den Remote Play an der PS5 anmeldet |
| `--skip-check` | – | Konsolensuche vor dem Verbinden überspringen |
| `--save` | – | angegebene Optionen im Profil speichern |

Beispiele:

```powershell
# erstes Mal: alles festlegen und speichern
.\ps5view.exe stream --audio-device "CABLE Input" --wake --display 1 --save

# danach reicht
.\ps5view.exe stream

# schwaches WLAN: weniger Bitrate, 720p
.\ps5view.exe stream --resolution 720p --bitrate 6000

# zweites PSN-Konto
.\ps5view.exe stream --profile PS5-123-2
```

### Exit-Codes

| Code | Bedeutung |
|---|---|
| 0 | OK (auch: Verbindung normal beendet, PS5 in den Ruhemodus gewechselt) |
| 1 | Aufruf fehlerhaft (Option/Wert) |
| 2 | Konfiguration/Profil fehlerhaft |
| 3 | Netzwerk: Konsole nicht erreichbar, Firewall, Zeitüberschreitung |
| 4 | Authentifizierung: PIN, Account-ID, Registrierung, PSN-Login |
| 5 | Konsolenzustand: Ruhemodus, Remote Play deaktiviert/belegt, Version |
| 6 | Audio-/Grafikgerät oder Decoder |
| 10 | interner Fehler |
| 130 | abgebrochen (Strg+C) |

---

## 4. Konfiguration, Profile, Logs

* **Konfiguration:** `%APPDATA%\ps5view\config.json` (JSON, mehrere Profile). Beim Speichern
  wird atomar geschrieben und die vorherige Version als `config.json.bak` behalten. Die Datei
  enthält die Remote-Play-Schlüssel – **nicht weitergeben**.
* **Logs:** `%LOCALAPPDATA%\ps5view\logs\ps5view-<datum>-<uhrzeit>-<pid>.log` – jede
  Zeile mit Zeitstempel (ms), Level (T/D/I/W/E), Thread und Bereich. Es werden die 20 neuesten
  Logs behalten. Bei Fehlern nennt ps5view den Pfad der Logdatei. PIN und Account-ID werden
  im Log maskiert; Schlüssel landen nur mit `--trace` (libchiaki-Hexdumps) im Log.
* **Konsole:** zeigt kurze deutsche Meldungen; mit `--verbose` zusätzlich alle libchiaki-Meldungen.

Beispiel eines Profils:

```json
{
  "name": "PS5-123",
  "host": "192.168.178.88",
  "console": "ps5",
  "nickname": "PS5-123",
  "account_id": "xxxxxxxxxxx=",
  "regist_key": "…",
  "rp_key": "…",
  "stream": { "audio_device": "CABLE Input (VB-Audio Virtual Cable)", "wake": true }
}
```

---

## 5. Troubleshooting

Erster Schritt bei jedem Problem:

```powershell
.\ps5view.exe check
```

`check` prüft Konfiguration, Netzwerkadapter, ob die PS5 im lokalen Netz liegt, ob sie
antwortet (UDP 9302), ob der Remote-Play-Port TCP 9295 erreichbar ist, die Decoder
(inkl. Hardware), Renderer und Audiogerät – mit `[ OK ]`/`[WARN]`/`[FAIL]` pro Punkt.

### Fehlercodes der PS5 („RP-Application-Reason“)

Die PS5 begründet abgelehnte Anfragen mit einem Code. ps5view übersetzt ihn:

| Code | Bedeutung | Was tun | Neuversuch? |
|---|---|---|---|
| `0x80108b02` | PSN-Account-ID wird nicht akzeptiert | `account-id` neu ermitteln; muss das an der PS5 angemeldete Konto sein | nein |
| `0x80108b09` | Registrierung abgelehnt – PIN falsch/abgelaufen | „Gerät verknüpfen“ neu öffnen, neue PIN sofort verwenden | nein |
| `0x80108b10` | Remote Play schon belegt | andere Sitzung beenden (PS App, PS Remote Play, chiaki-ng, PS Portal) | ja, mit langen Pausen |
| `0x80108b11` | Remote-Play-Version passt nicht | PS5-Systemsoftware/ps5view aktualisieren | nein |
| **`0x80108b12`** | **Remote Play an der PS5 deaktiviert** (siehe unten) | Remote Play aktivieren, PSN-Anmeldung prüfen, PS5 neu starten | nein |
| `0x80108b13` | Zugriff verweigert („not allowed“) | Kontoeinschränkungen prüfen, neu verknüpfen | nein |
| `0x80108b15` | Remote Play auf der PS5 abgestürzt | PS5 komplett neu starten | ja, mit langen Pausen |
| `0x80108bff` | Zugangsdaten nicht akzeptiert | neue PIN/Account-ID prüfen bzw. neu registrieren | nein |

Die ersten sechs Codes (b02, b09, b10, b11, b15, bff) sind in chiaki-ng definiert
(`lib/include/chiaki/session.h`). chiaki-ng kennt **0x80108b12** nicht (meldet „unknown“,
siehe [thestr4ng3r/chiaki#106](https://github.com/thestr4ng3r/chiaki/issues/106), bis heute
unbeantwortet).

#### 0x80108b12 im Detail

Die PS5 beantwortet die Sitzungsanfrage (`/sie/ps5/rp/sess/init`) mit HTTP 403 und diesem
Code, obwohl die Registrierung funktioniert hat. Zwei unabhängige Open-Source-Clients ordnen
ihn gleich zu: Remote Play ist an der Konsole ausgeschaltet
([openps-rs `regist.rs`](https://github.com/Gourav1100/openps-rs/blob/4b02a8e4a9d872f9fe95a139392680c18664a9b9/crates/openps-rp/src/regist.rs):
„Remote Play is disabled on the console“;
[ps5upload `rp_stream_api.rs`](https://github.com/phantomptr/ps5upload/blob/a6934412bcd055a57396eb25568a3d0437731533/engine/crates/ps5upload-engine/src/rp_stream_api.rs):
„Session init answers 80108b12 while the console's own Remote Play switch is off“).
Ein weiteres Projekt nennt als Ursache ein PS5-Benutzerkonto ohne PSN-Anmeldung (reines Offline-Konto).
Sony dokumentiert den Code nicht. ps5view gibt deshalb aus:

1. Einstellungen > System > Remote Play > „Remote Play aktivieren“ einschalten.
2. Das registrierte PSN-Konto muss an der PS5 angemeldet sein (kein Offline-Konto).
3. Für Start aus dem Ruhemodus: Energiesparen > Im Ruhemodus verfügbare Funktionen >
   „Mit dem Internet verbunden bleiben“ + „Einschalten der PS5 über das Netzwerk ermöglichen“.
4. PS5 einmal komplett neu starten.

Ein Neuversuch wird bei diesem Code nicht gemacht – er würde nichts ändern.

### Konsole im Ruhemodus / Aufwecken klappt nicht

* Ohne `--wake` bricht ps5view mit „Die PS5 ist im Ruhemodus“ ab.
* Mit `--wake` sendet ps5view das Weckpaket (alle 10 s erneut) und wartet bis zu
  `--wake-timeout` Sekunden (Standard 60), bis die PS5 „bereit“ meldet.
* Klappt es nicht: die beiden Ruhemodus-Optionen unter Energiesparen aktivieren (siehe 1.1).
  Aus dem **komplett ausgeschalteten** Zustand kann die PS5 nicht über das Netz geweckt werden.
* Antwortet die PS5 im Ruhemodus gar nicht auf `discover`, ist „Mit dem Internet verbunden
  bleiben“ aus.

### PIN falsch oder abgelaufen

* Die PIN gilt nur, solange „Gerät verknüpfen“ an der PS5 offen ist (wenige Minuten).
* Meldung „Der Bildschirm ‚Gerät verknüpfen‘ ist nicht geöffnet“: die PS5 hat die Suche
  abgelehnt – Bildschirm öffnen und offen lassen, bis ps5view „Registrierung erfolgreich“ meldet.
* `0x80108b09` = PIN falsch/abgelaufen, `0x80108b02` = Account-ID passt nicht.

### PSN-Login (`account-id`) schlägt fehl

* **„Code ist abgelaufen oder wurde schon benutzt“** (`invalid_grant`): ps5view neu starten,
  neu anmelden und die Redirect-URL **sofort** einfügen; dieselbe URL nicht zweimal verwenden.
* **„too_many_requests“ / Fehler 4177:** Sony hat zu viele Versuche in kurzer Zeit gezählt.
  15–60 Minuten warten, dann **einmal** neu versuchen. Wiederholte Versuche während der Sperre
  verlängern sie eventuell. Nicht parallel in chiaki-ng oder Skripten abfragen.
* „In der Eingabe ist kein code= enthalten“: Es wurde die Login-Adresse statt der
  Redirect-Adresse kopiert.
* Proxy/TLS-Fehler: Uhrzeit des PCs, Virenscanner mit HTTPS-Prüfung, Proxy-Variablen prüfen.

### Falsche Account-ID

Die Account-ID ist **nicht** die Online-ID (dein sichtbarer PSN-Name). Sie ist ein
12-stelliger base64-Wert mit `=` am Ende. Bei mehreren Benutzern an der PS5 muss es die ID
des Benutzers sein, dessen Spiel du streamen willst.

### Firewall blockiert

ps5view nutzt im Heimnetz diese Ports der PS5:

| Port | Zweck |
|---|---|
| UDP 9302 (PS4: 987) | Konsolensuche und Aufwecken |
| UDP 9295 / TCP 9295 | Registrierung, Sitzungsanfrage, Steuerverbindung |
| UDP 9296 | Video-/Audiostream |
| UDP 9297 | Netzwerktest vor dem Stream |
| UDP 9303–9319 (lokal) | Antworten auf die Konsolensuche |

Typische Meldungen: „Keine Antwort … (UDP 9302)“, „Die Video-/Audioverbindung (UDP 9296) kam
nicht zustande“ oder „Seit … s kommen keine Videodaten an“.

* Windows-Sicherheit > Firewall- & Netzwerkschutz > „App durch die Firewall zulassen“ >
  ps5view für **Privat** erlauben. Oder als Administrator:

  ```powershell
  netsh advfirewall firewall add rule name="ps5view" dir=in action=allow program="C:\Pfad\zu\ps5view.exe" enable=yes profile=private
  ```

* Netzwerkprofil muss **Privat** sein: `Get-NetConnectionProfile` zeigt es an,
  `Set-NetConnectionProfile -InterfaceAlias "Ethernet" -NetworkCategory Private` (Administrator) ändert es.
* VPN trennen; Drittanbieter-Firewalls/Virenscanner prüfen.
* `ps5view check` meldet zusätzlich, wenn die PS5-Adresse in keinem lokalen Netz des PCs liegt
  (Gast-WLAN, anderes Subnetz, VPN).

### Konsole schon in einer anderen Remote-Play-Sitzung

Meldung `0x80108b10`. Andere Verbindung beenden (Handy-App, PS Remote Play, chiaki-ng,
PlayStation Portal). Nach einem Absturz von ps5view hält die PS5 die alte Sitzung noch kurz
fest – ps5view versucht es deshalb automatisch mit 10, 20, 30 … Sekunden Pause erneut
(höchstens `--retries` Mal). ps5view beendet Sitzungen beim Schließen des Fensters, bei Strg+C
und beim Schließen des Konsolenfensters sauber, damit das selten passiert.

### Hinweis der PS5 zum Controller

Remote Play meldet immer einen virtuellen Controller an (siehe oben). Blendet die PS5 dazu
einen Hinweis ein (z. B. dass ein DUALSHOCK 4 für PS5-Spiele nicht geeignet ist), mit
`--controller dualsense --save` den Typ umstellen. ps5view sendet in beiden Fällen keine Eingaben.

### Anmelde-Passcode

Hat dein PS5-Benutzer einen Anmelde-Passcode, fragt die PS5 danach. Dann
`--login-pin <ziffern>` angeben (das ist nicht die Registrierungs-PIN).

### Kein Bild / schwarzes Fenster

* Konsole meldet „Seit … s kommen keine Videodaten an“ → Firewall/UDP (siehe oben).
* „… kein neues Bild, obwohl Daten ankommen“ → Decoder: `--hwdec none` oder `--codec h264` versuchen.
  ps5view wechselt bei einem Hardware-Decoder, der keine Bilder liefert, automatisch auf Software.
* Schwarzes Bild nur in Discord: Fenster nicht minimieren; anderen Renderer versuchen
  (`--renderer direct3d` oder `--renderer opengl`).

### Kein Ton in Discord / Ton trotzdem selbst hörbar

* In Discord beim Teilen **„Ton“** einschalten und das **ps5view-Fenster** (nicht den ganzen
  Bildschirm) teilen.
* `ps5view list-audio` zeigt die exakten Gerätenamen; `--audio-device` muss eindeutig sein.
  Ist das Gerät nicht vorhanden, bricht ps5view ab bzw. spielt **keinen** Ton – es weicht nie
  still auf die Lautsprecher aus.
* Windows-Lautstärkemixer: „ps5view“ darf nicht stummgeschaltet sein. ps5view selbst
  verändert die Lautstärke nie.
* Du hörst den Ton trotzdem? Unter Soundeinstellungen > Aufnahme > „CABLE Output“ >
  Eigenschaften > Abhören darf „Dieses Gerät als Wiedergabequelle verwenden“ nicht aktiv sein.
* Falls Discord den Ton von „CABLE Input“ bei dir nicht mitnimmt: als Ausgabegerät den
  HDMI-/DisplayPort-Ausgang von Monitor 2 wählen und den Monitor stumm schalten.

### Ruckeln, Artefakte, Aussetzer

* LAN-Kabel statt WLAN (vor allem an der PS5).
* Bitrate senken (`--bitrate 8000`) oder `--resolution 720p`.
* `--codec h265` spart Bandbreite (braucht aber einen HEVC-fähigen Decoder).
* Im Log (Bereich `stats`, alle 30 s) stehen verlorene Pakete, Decoderfehler und der Audiopuffer.

### Neuversuche

ps5view versucht eine Verbindung höchstens `--retries` Mal (Standard 5) mit steigender Pause:
2, 4, 8, 16, 30 s – bei „Konsole belegt“/„abgestürzt“ 10, 20, 30, 45, 60 s. Fehler, die ein
Neuversuch nicht beheben kann (falsche PIN, Remote Play deaktiviert, ungültige Registrierung,
Ruhemodus ohne `--wake`), beenden sofort mit einer klaren Meldung. Läuft ein Stream mindestens
60 s stabil und bricht dann ab, beginnt die Zählung neu (automatisches Wiederverbinden).

---

## 6. Bauen

ps5view wird wie chiaki-ng unter Windows mit **MSYS2/MinGW** gebaut (die chiaki-ng-Builds für
Windows nutzen dieselbe Toolchain, siehe `.github/workflows/build-msys2.yml` dort). Visual Studio
wird nicht unterstützt: libchiaki zieht eine Reihe von Bibliotheken (json-c, miniupnpc,
libevent, nanopb/protobuf, OpenSSL, Opus, FFmpeg) über pkg-config heran, die unter MSYS2 fertig
vorliegen.

### Windows (MSYS2 MINGW64)

1. [MSYS2](https://www.msys2.org) installieren und die Shell **„MSYS2 MINGW64“** öffnen.
2. Pakete installieren:

   ```bash
   pacman -Syu
   ```

   ```bash
   pacman -S --needed git mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-pkgconf mingw-w64-x86_64-openssl mingw-w64-x86_64-opus mingw-w64-x86_64-json-c mingw-w64-x86_64-miniupnpc mingw-w64-x86_64-libevent mingw-w64-x86_64-protobuf mingw-w64-x86_64-python mingw-w64-x86_64-python-protobuf mingw-w64-x86_64-ffmpeg mingw-w64-x86_64-SDL2
   ```

3. Quellcode holen (Zeilenenden nicht umwandeln, sonst schlägt ein Patch beim curl-Build fehl):

   ```bash
   git config --global core.autocrlf false
   ```

   ```bash
   git clone https://github.com/hanudi1337/memecoin.git && cd memecoin
   ```

   ```bash
   git submodule update --init ps5view/third-party/chiaki-ng
   ```

   ```bash
   git -C ps5view/third-party/chiaki-ng submodule update --init third-party/nanopb third-party/jerasure third-party/gf-complete third-party/curl
   ```

4. Bauen:

   ```bash
   cmake -S ps5view -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
   ```

   ```bash
   cmake --build build
   ```

5. Starten: in der MINGW64-Shell direkt `./build/ps5view.exe discover`. Für den Start außerhalb
   von MSYS2 die benötigten DLLs neben die exe kopieren:

   ```bash
   ldd build/ps5view.exe | awk '{print $3}' | grep -i '^/mingw64/' | xargs -I{} cp {} build/
   ```

### Linux (Entwicklung)

```bash
sudo apt-get install cmake ninja-build pkg-config libssl-dev libopus-dev libjson-c-dev libminiupnpc-dev libevent-dev libavcodec-dev libavutil-dev libavformat-dev libswscale-dev libsdl2-dev protobuf-compiler python3-protobuf
```

```bash
cmake -S ps5view -B build -G Ninja && cmake --build build
```

### Tests

```bash
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ctest --test-dir build --output-on-failure
```

* `ps5view-tests`: Unit-Tests (Hilfsfunktionen, OAuth, Konfiguration, Fehlertabelle,
  Diagnose-Parser, Audio-Geräteauswahl, Audio-Pipeline Opus → Gerät, Video-Pipeline
  H.264 → Decoder → Fenster). Ein Test prüft, dass alle ausgewerteten libchiaki-Logzeilen
  im eingebundenen chiaki-ng-Stand noch existieren (Schutz bei Updates des Submodules).
* `tests/integration.py`: startet das echte `ps5view` gegen eine **Fake-PS5**
  (`tests/fake_ps5.py`: Discovery, Aufwecken, Registrierung, Sitzungsanfrage mit beliebigem
  Application-Reason-Code) und einen **Fake-Sony-Login** (`tests/fake_psn_auth.py`).
  Die Fake-PS5 lässt sich auch einzeln starten: `python3 ps5view/tests/fake_ps5.py --session-reason 80108b12`.
* Die GitHub-Actions-Workflow-Datei `.github/workflows/ps5view.yml` baut und testet unter
  Windows (MSYS2) und Linux und erzeugt das Windows-Paket.

Umgebungsvariablen für Tests/Sonderfälle: `PS5VIEW_PSN_TOKEN_URL` (Token-Endpunkt umlenken,
nur für Tests), `CURL_CA_BUNDLE` (eigenes CA-Bündel für den PSN-Login, z. B. hinter einem Proxy).

---

## 7. Lizenz und Quellcode

ps5view ist freie Software unter der **GNU Affero General Public License v3.0 (nur diese
Version)** mit der zusätzlichen Erlaubnis zum Linken mit OpenSSL – dieselbe Lizenz wie
chiaki-ng (`LicenseRef-AGPL-3.0-only-OpenSSL`, Text in [`COPYING`](COPYING)).

* libchiaki stammt aus [chiaki-ng](https://github.com/streetpea/chiaki-ng) und ist als
  Git-Submodule in `third-party/chiaki-ng` auf einen festen Commit gepinnt (unverändert).
  Lizenzen der darin enthaltenen Fremdbibliotheken: `third-party/chiaki-ng/LICENSES/`.
* `ps5view version` und `ps5view license` nennen den chiaki-ng-Commit und die Bezugsquelle
  des vollständigen Quellcodes.
* Wer ps5view (auch verändert) weitergibt, muss den vollständigen Quellcode dieser Version
  mitliefern oder anbieten (AGPL §§ 4–6). Das Windows-Paket aus der CI enthält dazu `COPYING`,
  die Lizenzen und eine `SOURCE.txt` mit dem exakten Quellcode-Stand.
* Die mitgelieferten DLLs (FFmpeg, SDL2, Opus, OpenSSL, …) stammen aus MSYS2 und stehen unter
  ihren eigenen Lizenzen; Quellen unter <https://packages.msys2.org>.
* PlayStation, PS5 und Remote Play sind Marken von Sony Interactive Entertainment. ps5view
  steht in keiner Verbindung zu Sony.

---

## 8. Technischer Hintergrund

```
            ┌──────────── ps5view.exe ───────────────────────────────────────────┐
 PS5 ◄─────►│ libchiaki: Discovery, Wakeup, Regist, Session (Ctrl, Senkusha,     │
 UDP/TCP    │ Takion), FEC, Opus-Decoder, FFmpeg-Decoder                          │
            │      │ Video-Samples            │ Opus-PCM            │ Log-Zeilen   │
            │      ▼                          ▼                     ▼              │
            │  video.c (HW/SW-Decoding,   audio.c (SDL-Queue,   diag.c (Codes,   │
            │  SDL2-Fenster, Letterbox)   gewähltes Gerät)      Ursachen)        │
            │      └────────── cmd_stream.c (Ablauf, Neuversuche) ◄── errors.c   │
            └─────────────────────────────────────────────────────────────────────┘
```

| Datei | Aufgabe |
|---|---|
| `src/main.c` | Befehlsverteilung, globale Optionen, Log-Start, UTF-8-Argumente unter Windows |
| `src/cmd_*.c` | die Befehle |
| `src/discover.c`, `src/netif.c` | Konsolensuche über alle Netzwerkadapter, Aufwecken + Warten |
| `src/video.c` | FFmpeg-Decoder (d3d11va/dxva2 → Software, auch Laufzeit-Fallback), SDL2-Darstellung |
| `src/audio.c` | Opus → SDL2 auf dem gewählten Gerät, Latenzbegrenzung, Wiederöffnen |
| `src/diag.c` | wertet libchiaki-Logzeilen aus (z. B. „Reported Application Reason“), weil libchiaki diese Details nicht über die API liefert |
| `src/errors.c` | Fehlertabelle: Meldung, Hinweis, Exit-Code, Neuversuch ja/nein |
| `src/config.c` | JSON-Profile, strikte Prüfung, atomares Speichern |
| `src/log.c` | Datei-/Konsolenlog, libchiaki- und FFmpeg-Meldungen eingebunden |
| `src/util.c` | Pfade (UTF-16 unter Windows), Zeit, Strg+C/Konsole schließen, Hilfsfunktionen |

Wichtige Entwurfsentscheidungen:

* **Kein Fork von chiaki-ng:** libchiaki wird unverändert genutzt. Was die API nicht hergibt
  (Ablehnungscodes, Ursachen von Registrierungsfehlern, UDP-Probleme), wird aus den
  libchiaki-Logzeilen erkannt; ein Unit-Test schlägt Alarm, wenn sich diese Zeilen in einer
  neuen chiaki-ng-Version ändern.
* **View-only:** `chiaki_session_set_controller_state()` wird nie aufgerufen, Tastatur und
  Mikrofon sind aus, Controller-/Tastatur-Ereignisse des Fensters werden nicht weitergegeben.
* **Threads:** libchiaki ruft Video-/Audio-/Ereignis-Callbacks auf eigenen Threads auf; diese
  füttern nur Decoder bzw. Audio-Warteschlange und wecken den Hauptthread über SDL-Ereignisse.
  Fenster, Texturen und das Öffnen von Audiogeräten passieren ausschließlich im Hauptthread.
