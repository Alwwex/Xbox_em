# Architektura xbem

Tento dokument popisuje, jak xbem spouští kód z Xbox Series X|S, proč je
postavený právě takhle a kde jsou hranice současného prototypu.

## Výchozí fakta o konzoli

- **CPU**: vlastní AMD Zen 2 (8 jader / 16 vláken, hry mají k dispozici většinu z nich).
  Instrukční sada x86‑64 s AVX2, FMA3, BMI1/2, SHA, … — žádné AVX‑512.
- **OS**: hypervizor s několika oddíly; hra běží v „herním OS“, což je
  ořezané jádro Windows. Hra je normální x64 **PE** soubor (`.exe` + `.dll`).
- **API**: Win32 podmnožina přes `kernelx.dll` (a API‑set názvy), herní služby
  přes **Microsoft GDK** (`XGameRuntime`: XUser, XGameSave, XTaskQueue, …),
  grafika přes **Direct3D 12.X** – konzolovou variantu D3D12.
- **Balíčky**: instalace jsou kontejnery XVD/XVC (`.xvc`, `.msixvc`), šifrované.
  Vývojář má k dispozici „loose“ build, tj. nezabalenou složku.

Z toho plyne návrh: **neemulovat CPU** (zbytečné a pomalé), ale stejně jako
Wine/Proton **spouštět kód nativně a nahradit operační systém**.

## Model spouštění

### Nativní kód, Microsoft x64 ABI

Herní kód se mapuje do adresního prostoru procesu `xbem` a skáče se přímo na
jeho entry point. Hra očekává volací konvenci Microsoft x64 (argumenty v
`rcx, rdx, r8, r9`, 32 B shadow space, `xmm6–15` callee‑saved), zatímco Linux
používá System V. Všechny funkce, které může zavolat hra, jsou proto
označené `XBEM_GUEST_ABI` = `__attribute__((ms_abi))` a o převod se postará
překladač. Naopak volání herního kódu (entry point, `DllMain`, start vlákna,
TLS callbacky, callbacky XTaskQueue) jde přes ukazatele na funkce se stejným
atributem.

### TEB/PEB přes segment GS

Kód kompilovaný pro Windows čte strukturu vlákna (TEB) přímo přes `gs:`:
`gs:[0x30]` je ukazatel na sebe, `gs:[0x58]` pole TLS, `gs:[0x68]` poslední
chyba atd. xbem pro každé vlákno alokuje vlastní TEB a na Linuxu nastaví
bázi `GS` systémovým voláním `arch_prctl(ARCH_SET_GS)`. glibc používá `FS`,
takže se hostitelský a herní svět nepletou. (Na nativních Windows tohle
nejde, proto spouštění zatím vyžaduje Linux/WSL2.)

Rozložení, na kterém záleží (`src/runtime/process.h`):

| Offset | Pole |
|---|---|
| `0x08 / 0x10` | StackBase / StackLimit (čte např. `__chkstk`) |
| `0x30` | Self |
| `0x40 / 0x48` | ClientId (proces / vlákno) |
| `0x58` | ThreadLocalStoragePointer |
| `0x60` | PEB |
| `0x68` | LastErrorValue |
| `0x1480` | TlsSlots[64], `0x1780` TlsExpansionSlots |

### TLS

- **Implicitní TLS** (`__declspec(thread)`): každý modul s TLS adresářem dostane
  index (exe vždy 0), loader ho zapíše do `_tls_index` a každé vlákno dostane
  vlastní kopii šablony `.tls`. Pole ukazatelů je v `TEB+0x58`.
- **TLS callbacky** a `DllMain` se volají s `DLL_PROCESS_ATTACH`,
  `DLL_THREAD_ATTACH/DETACH` ve správném pořadí (závislosti dřív).
- **`TlsAlloc`** sloty žijí přímo v TEB (stejné offsety jako Windows),
  **FLS** včetně callbacků při konci vlákna.

### Vlákna

Každé herní vlákno je hostitelské pthread vlákno s vlastním TEB a zásobníkem
podle `SizeOfStackReserve` z PE hlavičky (+ rezerva pro HLE kód). `ExitThread`
je implementované přes `longjmp` zpět do startovního rámce vlákna — přes
herní rámce se nic „neodvíjí“, stejně jako na Windows. Afinita a priority se
zaznamenají, ale plánování nechává xbem na hostitelském OS.

Synchronizační objekty jádra (události, semafory, mutexy, vlákna) sdílejí
jeden zámek a condition variable. Je to pomalejší, ale triviálně správné i
pro `WaitForMultipleObjects(waitAll)`; optimalizace přijde, až bude co měřit.

## Loader

`src/loader/pe_image.*` je čistý parser (bez mapování) s kontrolou mezí všech
čtení — poškozený soubor vyvolá chybu, ne pád. Poškozené volitelné části
(importy, TLS, debug) se nahlásí jako varování a zbytek jde dál zkoumat.

`src/runtime/module_loader.*` pak:

1. namapuje obraz na preferovanou bázi (`MAP_FIXED_NOREPLACE`), jinak kamkoliv
   zarovnaně na 64 KiB a aplikuje relokace,
2. zaregistruje modul (kvůli cyklickým importům) a rekurzivně načte závislosti:
   **HLE modul** → **DLL přibalená ke hře** (case‑insensitive hledání) → nic,
3. vyplní IAT; co nejde vyřešit, dostane **stub**,
4. nastaví TLS a ochrany stránek podle sekcí,
5. zařadí modul k inicializaci (post‑order = závislosti první).

Delay‑load importy řeší herní CRT sám přes `LoadLibraryExA`/`GetProcAddress`,
které jsou implementované.

### Stuby neimplementovaných funkcí

Pro každou chybějící funkci se vygeneruje 32bajtový kus strojového kódu:

```
sub  rsp, 0x28        ; shadow space + zarovnání
mov  rcx, <info>      ; struktura s názvem DLL a funkce
mov  rax, <handler>
call rax
add  rsp, 0x28
ret
```

Handler zvýší počítadlo, prvních pár volání zaloguje, nastaví
`ERROR_CALL_NOT_IMPLEMENTED` a vrátí 0. V režimu `--strict` proces ukončí
(kód 125). Na konci běhu se vypíše seznam volaných neimplementovaných funkcí
seřazený podle počtu volání — to je pracovní seznam pro další vývoj.

## HLE moduly

Registr (`src/hle/registry.*`) mapuje název DLL → modul → funkce.
Porovnávání je case‑insensitive, `kernel32.dll`/`kernelbase.dll` jsou aliasy
`kernelx.dll` a všechny `api-ms-win-core-*` (kromě CRT) se přesměrují tam.

Funkce mají dva druhy:

- **Implemented** — chová se jako originál (případně zjednodušeně),
- **Stub** — úmyslně hlásí „nepodporováno“ způsobem, se kterým hra počítá
  (např. `E_NOTIMPL`, `DXGI_ERROR_UNSUPPORTED`). To je lepší než obecný stub,
  který vrací 0 (= `S_OK`) a hru tím může nechat čekat na něco, co nepřijde.

> **Pozor:** názvy konzolových DLL a způsob, jakým se titul váže na GDK
> runtime, je potřeba ověřit na skutečném titulu (`xbem inspect`). Současné
> názvy vycházejí z veřejné dokumentace GDK a z PC varianty GDK.

### Jak přidat HLE funkci

1. Najdi signaturu ve veřejné dokumentaci (Win32 / GDK na Microsoft Learn).
2. Napiš ji v odpovídajícím souboru s prefixem `hle_` a `XBEM_GUEST_ABI`:

   ```cpp
   DWORD XBEM_GUEST_ABI hle_GetActiveProcessorCount(WORD group) {
     return platform::hardware_threads();
   }
   ```

   Typy jsou ve `src/hle/guest_types.h` (pozor: `wchar_t` hosta ≠ `WCHAR`
   hosta hry, používej `char16_t`; `long` na Windows je 32bitový).
3. Přidej ji do tabulky exportů: `XBEM_HLE(GetActiveProcessorCount)`.
4. Nikdy nenech uniknout C++ výjimku do herního kódu — herní rámce nemají
   unwind informace pro hostitelský runtime.
5. Přidej test: ideálně guest program v `tests/guest/` (viz `hello.c`).

## Výjimky (SEH)

MSVC C++ výjimky, `__try/__except/__finally` i unwinder z libgcc stojí na
stejných primitivech jádra, která xbem implementuje nad unwind daty hry
(`src/runtime/seh.*`):

- `RtlVirtualUnwind` interpretuje `UNWIND_INFO` (všechny unwind kódy,
  řetězené záznamy, detekce epilogu podle pravidel x64 ABI).
- `RaiseException` zachytí registry volajícího (assemblerový thunk v
  `seh_asm.cpp`), zavolá vektorové handlery a pak prochází rámce a volá jejich
  jazykové handlery (`__CxxFrameHandler*`, `__C_specific_handler`, …).
- `RtlUnwindEx` rozbaluje k cílovému rámci, volá unwind handlery
  (destruktory) a nakonec obnoví kontext. U `STATUS_UNWIND_CONSOLIDATE`
  (tak MSVC spouští `catch` blok) zavolá callback přes trampolínu, která si
  uloží kopii cílového kontextu – výjimka vyhozená uvnitř `catch` se pak
  rozbalí správně ven.

Mezi handlerem a rámci, které zpracovává, leží na zásobníku hostitelské
rámce xbem bez Windows unwind dat. Každý dispatch/unwind si proto zapisuje
per-vláknový záznam; procházení, které narazí na hostitelský rámec, najde jeho
vlastníka a pokračuje stejně, jako by to zařídily dispatcher rámce Windows
(od místa vyhození, kolidující unwind, vnořená výjimka).

## C knihovna (UCRT)

Konzole má C runtime jako součást systému (`ucrtbase.dll` za API‑sety
`api-ms-win-crt-*`), hra ho tedy importuje, ale nepřibaluje. `src/hle/ucrt_*.cpp`
ho reimplementuje s Windows sémantikou: `long` je 32bitový, `wchar_t`
16bitový, `printf` má vlastní engine (Microsoft va_list jsou prostě ukazatele
na 8bajtové sloty), soubory jdou přes VFS a `malloc` sdílí alokátor s
`HeapAlloc`. `vcruntime140.dll` a `msvcp140.dll` hra přibaluje sama a ty se
načtou jako běžné DLL hry.

## Virtuální souborový systém

`src/runtime/vfs.*`: písmena disků → hostitelské složky. Výchozí:
`G:\` = balíček hry (**jen pro čtení**, jako na konzoli), `T:\` = dočasná
zapisovatelná složka. Relativní cesty a `\cesta` bez disku míří do kořene
balíčku (pracovní adresář titulu). Každá existující komponenta cesty se
hledá case‑insensitive, `..` je ořezané na kořen připojení. Zda konzole
používá pro jednotlivé úložiště přesně tato písmena, je třeba ověřit na
reálném titulu — mapování je konfigurovatelné (`--mount`).

## GPU

Tady leží většina zbývající práce. Konzolová hra:

- vytváří zařízení přes **D3D12.X** (`D3D12XboxCreateDevice`) — rozšířené
  D3D12 s přímějším přístupem k paměti GPU, vlastními frontami a dalšími
  konzolovými rozšířeními,
- **posílá předkompilované shadery v nativním strojovém kódu RDNA2**, nikoliv
  DXIL jako na PC.

Možné cesty (od nejschůdnější):

1. **Shadery od vývojáře.** Vývojář má zdrojové kódy shaderů a může dodat
   DXIL/PC verze. Ty jde přes [vkd3d‑proton]‑like vrstvu převést na Vulkan.
   Zbývá D3D12.X → D3D12/Vulkan překlad API.
2. **Rekompilátor RDNA2 ISA → SPIR‑V.** ISA RDNA2 je veřejně zdokumentovaná
   (AMD), ale jde o velký projekt srovnatelný s tím, co dělají emulátory jiných
   konzolí se shadery.
3. **PC build hry** — viz README; obchází celý problém.

Rozhraní, kam se backend zapojí, je `src/gpu/backend.h`; `hle/d3d12.cpp`
dnes vrací `DXGI_ERROR_UNSUPPORTED`, takže titul dojde k inicializaci
grafiky a „čistě“ selže tam.

[vkd3d‑proton]: https://github.com/HansKristian-Work/vkd3d-proton

## Známá omezení

- Hardwarové výjimky (neplatný přístup do paměti, dělení nulou) se zatím
  nepřevádějí na SEH: místo doručení do `__try/__except` skončí crash reportem.
- C knihovna běží v „C“ locale; `setlocale` jiné locale nepřepne.
- `SuspendThread`, APC a alertable waity nejsou podporované.
- Pojmenované kernel objekty se chovají jako nepojmenované.
- `VirtualProtect` nevrací skutečnou původní ochranu.
- Paměť GPU/ESRAM a speciální typy alokací konzole jsou obyčejná paměť hostitele.
- Spouštění jen na Linuxu x86‑64 / WSL2.
