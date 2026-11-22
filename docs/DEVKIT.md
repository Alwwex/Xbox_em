# Komunikace PC ↔ Xbox (devkit / Dev Mode)

Tato část repozitáře řeší „infrastrukturu“ mezi konzolí a počítačem:
jak se s vývojovou konzolí spojit, dostat z ní/na ni soubory a ovládat
aplikace. Nástroj je [`tools/xbdp/xbdp.py`](../tools/xbdp/xbdp.py) —
jeden soubor, jen standardní knihovna Pythonu 3.9+, funguje na Windows,
Linuxu i macOS.

## Dvě úrovně přístupu

| | Retail konzole v **Dev Mode** | **Devkit** (vývojářský kit, GDK) |
|---|---|---|
| Kdo ji má | kdokoliv s vývojářským účtem Microsoftu | vývojáři v programu ID@Xbox / partneři |
| Co spustí | hlavně UWP aplikace | GDK tituly — tj. běžné hry |
| Nástroje | Xbox Device Portal (webové rozhraní + REST API) | Device Portal + nástroje z GDK (`xbconnect`, `xbapp`, `xbcp`, …) |

Hru vytvořenou přes GDK typicky **na retail konzoli v Dev Mode nespustíš** —
k tomu je potřeba devkit, který má vývojář. `xbdp` funguje s obojím, protože
oba typy konzole poskytují Device Portal.

## Zapnutí Device Portalu

1. Na konzoli v Dev Mode otevři **Dev Home**.
2. V části pro vzdálený přístup (*Remote Access Settings*) zapni
   **Xbox Device Portal** a nastav uživatelské jméno a heslo.
3. Dev Home ukáže adresu, typicky `https://<IP-konzole>:11443`.

Konzole používá certifikát podepsaný sama sebou. Bezpečnější než vypnout
ověřování je certifikát jednou stáhnout (z webového rozhraní Device Portalu
nebo `https://<IP>:11443/config/rootcertificate`) a předat ho přes
`--cafile`. Volba `--insecure` ověřování vypne — používej ji jen v síti,
které věříš.

## Použití `xbdp`

Připojení lze zadat parametry, nebo proměnnými prostředí:

```bash
export XBDP_HOST=192.168.1.50 XBDP_USER=dev XBDP_PASSWORD=heslo XBDP_CAFILE=~/xbox.cer
```

```bash
python3 tools/xbdp/xbdp.py info
```

```bash
python3 tools/xbdp/xbdp.py packages
```

```bash
python3 tools/xbdp/xbdp.py processes
```

```bash
python3 tools/xbdp/xbdp.py launch "Contoso.Game_8wekyb3d8bbwe!Game" Contoso.Game_1.0.0.0_x64__8wekyb3d8bbwe
```

```bash
python3 tools/xbdp/xbdp.py folders
```

```bash
python3 tools/xbdp/xbdp.py ls DevelopmentFiles LooseApps
```

```bash
python3 tools/xbdp/xbdp.py pull-tree DevelopmentFiles LooseApps/MojeHra ./MojeHra
```

```bash
python3 tools/xbdp/xbdp.py screenshot obrazovka.png
```

Všechny příkazy: `python3 tools/xbdp/xbdp.py --help`. Volba `--json`
vypíše surová data z API.

## Propojení s emulátorem

```
 devkit / Dev Mode konzole              PC (Linux / WSL2)
 ┌──────────────────────┐   HTTPS     ┌───────────────────────────────┐
 │ Xbox Device Portal   │◀──11443────▶│ xbdp  (info, ls, pull-tree …) │
 │  DevelopmentFiles/   │             │        │                      │
 │    LooseApps/MojeHra │──soubory───▶│   ./MojeHra (loose build)     │
 └──────────────────────┘             │        ▼                      │
                                      │ xbem coverage / inspect / run │
                                      └───────────────────────────────┘
```

1. Vývojář nasadí loose build na devkit (nebo ti ho pošle přímo).
2. `xbdp pull-tree` ho zkopíruje na PC.
3. `xbem coverage` / `xbem run` s ním pracuje.

Kde přesně na devkitu leží nasazené GDK tituly a zda jsou přes Device Portal
dostupné, záleží na verzi OS a způsobu nasazení — `xbdp folders` a `xbdp ls`
ukážou, co konzole zpřístupňuje.

## Poznámky k API

- Ověřování je HTTP Basic (jméno/heslo z Dev Home).
- Požadavky, které něco mění (POST/DELETE), musí nést token proti CSRF:
  portal ho posílá jako cookie `CSRF-Token` a čeká ho zpět v hlavičce
  `X-CSRF-Token`. `xbdp` si ho vyžádá automaticky.
- Identifikátory aplikací a balíčků se v URL předávají v base64
  (dokumentace Device Portalu tomu říká „hex64“).
- Endpointy specifické pro Xbox jsou pod `/ext/…` (např. screenshot) a mezi
  verzemi OS se mohou lišit.

## Bezpečnost

Device Portal dává plnou kontrolu nad konzolí. Nezpřístupňuj ho mimo
domácí síť, používej silné heslo a hesla neukládej do repozitáře
(proměnné prostředí nebo správce hesel).
