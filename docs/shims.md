<!-- PAGE GENEREE PAR tools/gen_shim_list.py — NE PAS EDITER A LA MAIN. -->
<!-- Regenerer : tools/gen_shim_list.py   /   Verifier : tools/gen_shim_list.py --check -->

# Shims Win32 fournis par le moteur

Cette page est **générée depuis les sources** par `tools/gen_shim_list.py`, et la CI échoue si elle diverge de `src/runtime/win32_shims_*.cpp`. Elle dit ce que le moteur couvre **aujourd'hui** — pas ce qu'on aimerait qu'il couvre.

!!! warning "Une DLL listée ici n'est pas une DLL *complète*"
    Le moteur ne fournit que les fonctions dont il a été prouvé qu'elles ne portent **aucun** comportement propre à un jeu. Une même DLL peut donc être servie pour moitié par le moteur et pour moitié par le portage, fonction par fonction. Voir [Point d'extension](extension.md).

| | |
|---|---|
| Inscriptions | 225 |
| Clés distinctes | 225 |
| DLL couvertes | 14 |
| Unités d'installation | 9 |

## ADVAPI32.dll

Registre, ACL et descripteurs de sécurité, jetons, gestionnaire de services — en « succès neutre » : le moteur n'a aucun modèle de sécurité à faire respecter, et un invité qui interroge ces API attend surtout qu'elles n'échouent pas.

37 inscriptions, 37 clés distinctes — `src/runtime/win32_shims_advapi32.cpp`

| | | |
|---|---|---|
| `AddAccessAllowedAce` | `AddAccessDeniedAce` | `AddAce` |
| `AdjustTokenPrivileges` | `CheckTokenMembership` | `CloseServiceHandle` |
| `ControlService` | `CopySid` | `DuplicateToken` |
| `DuplicateTokenEx` | `EqualSid` | `FreeSid` |
| `GetLengthSid` | `GetSecurityDescriptorLength` | `GetSecurityInfo` |
| `GetSidLengthRequired` | `GetTokenInformation` | `GetUserNameA` |
| `ImpersonateLoggedOnUser` | `InitializeAcl` | `InitializeSecurityDescriptor` |
| `InitializeSid` | `IsValidAcl` | `IsValidSecurityDescriptor` |
| `IsValidSid` | `LookupAccountNameA` | `LookupPrivilegeValueA` |
| `OpenProcessToken` | `OpenThreadToken` | `QueryServiceStatus` |
| `RevertToSelf` | `SetSecurityDescriptorDacl` | `SetSecurityDescriptorGroup` |
| `SetSecurityDescriptorOwner` | `SetSecurityDescriptorSacl` | `SetSecurityInfo` |
| `StartServiceA` |  |  |

## binkw32.dll

Lecteur vidéo Bink — répond « pas de vidéo » de façon cohérente plutôt que d'échouer.

9 inscriptions, 9 clés distinctes — `src/runtime/win32_shims_misc.cpp`

| | | |
|---|---|---|
| `_BinkClose@4` | `_BinkCopyToBuffer@28` | `_BinkDDSurfaceType@4` |
| `_BinkDoFrame@4` | `_BinkNextFrame@4` | `_BinkOpen@8` |
| `_BinkOpenDirectSound@4` | `_BinkSetSoundSystem@8` | `_BinkWait@4` |

## DDRAW.dll

DirectDraw réduit à ce qu'un invité doit voir pour ne pas s'arrêter : le moteur ne dessine pas via DirectDraw.

2 inscriptions, 2 clés distinctes — `src/runtime/win32_shims_misc.cpp`

| | | |
|---|---|---|
| `DirectDrawCreate` | `DirectDrawEnumerateA` |  |

## GDI32.dll

Objets GDI simulés par des poignées factices : palettes, polices, pinceaux, contextes de périphérique.

36 inscriptions, 36 clés distinctes — `src/runtime/win32_shims_gdi32.cpp`

| | | |
|---|---|---|
| `AnimatePalette` | `CreateBitmap` | `CreateCompatibleBitmap` |
| `CreateCompatibleDC` | `CreateDCA` | `CreateFontA` |
| `CreatePalette` | `CreatePen` | `CreateSolidBrush` |
| `DeleteDC` | `DeleteObject` | `GdiFlush` |
| `GdiSetBatchLimit` | `GetCharWidthA` | `GetDeviceCaps` |
| `GetDeviceGammaRamp` | `GetDIBColorTable` | `GetPixel` |
| `GetStockObject` | `GetSystemPaletteEntries` | `GetTextExtentPoint32A` |
| `GetTextExtentPointA` | `LineTo` | `MoveToEx` |
| `PatBlt` | `RealizePalette` | `SelectObject` |
| `SelectPalette` | `SetBkColor` | `SetBkMode` |
| `SetDeviceGammaRamp` | `SetPaletteEntries` | `SetStretchBltMode` |
| `SetTextColor` | `StretchDIBits` | `UnrealizeObject` |

## ijl11.dll

Décodeur JPEG Intel.

3 inscriptions, 3 clés distinctes — `src/runtime/win32_shims_misc.cpp`

| | | |
|---|---|---|
| `#2` | `#3` | `#5` |

## IMM32.dll

Éditeur de méthode d'entrée (IME) — toujours rapporté absent, ce qui est la vérité sur les plateformes visées.

11 inscriptions, 11 clés distinctes — `src/runtime/win32_shims_misc.cpp`

| | | |
|---|---|---|
| `ImmGetCandidateListA` | `ImmGetCandidateListCountA` | `ImmGetCompositionStringA` |
| `ImmGetContext` | `ImmGetConversionStatus` | `ImmGetOpenStatus` |
| `ImmIsIME` | `ImmReleaseContext` | `ImmSetConversionStatus` |
| `ImmSetOpenStatus` | `ImmSimulateHotKey` |  |

## KERNEL32.dll

Les quelques entrées d'énumération de modules que KERNEL32 expose en doublon de PSAPI (`K32*`).

4 inscriptions, 4 clés distinctes — `src/runtime/win32_shims_psapi.cpp`

| | | |
|---|---|---|
| `EnumProcessModules` | `GetModuleInformation` | `K32EnumProcessModules` |
| `K32GetModuleInformation` |  |  |

## PSAPI.DLL

Énumération des modules du processus.

1 inscription, 1 clé distincte — `src/runtime/win32_shims_psapi.cpp`

| | | |
|---|---|---|
| `GetModuleInformation` |  |  |

## SHELL32.dll

Deux appels shell sans effet observable pour l'invité.

2 inscriptions, 2 clés distinctes — `src/runtime/win32_shims_shell32.cpp`

| | | |
|---|---|---|
| `SHAppBarMessage` | `ShellExecuteA` |  |

## smackw32.dll

Lecteur vidéo Smacker, même principe que Bink.

6 inscriptions, 6 clés distinctes — `src/runtime/win32_shims_misc.cpp`

| | | |
|---|---|---|
| `_SmackClose@4` | `_SmackDoFrame@4` | `_SmackNextFrame@4` |
| `_SmackOpen@12` | `_SmackToBuffer@28` | `_SmackWait@4` |

## USER32.dll

Géométrie de rectangles, chaînes, métriques d'écran, sémantique de fenêtre générique (peinture, contexte de périphérique, curseur) — la part de USER32 qui ne dépend ni d'une vraie fenêtre système ni de l'état clavier.

56 inscriptions, 56 clés distinctes — `src/runtime/win32_shims_user32.cpp`, `src/runtime/win32_shims_window.cpp`

| | | |
|---|---|---|
| `AdjustWindowRect` | `AdjustWindowRectEx` | `BeginPaint` |
| `BringWindowToTop` | `ChangeDisplaySettingsA` | `CharLowerBuffA` |
| `CharNextA` | `ClientToScreen` | `ClipCursor` |
| `CopyRect` | `DefWindowProcA` | `DestroyWindow` |
| `EnableWindow` | `EndPaint` | `EnumDisplaySettingsA` |
| `EqualRect` | `FindWindowA` | `GetClientRect` |
| `GetCursorPos` | `GetDC` | `GetDesktopWindow` |
| `GetSystemMetrics` | `GetWindowRect` | `GetWindowThreadProcessId` |
| `InflateRect` | `IntersectRect` | `IsRectEmpty` |
| `IsWindow` | `LoadAcceleratorsA` | `LoadCursorA` |
| `LoadIconA` | `LoadImageA` | `LoadStringA` |
| `LoadStringW` | `MapVirtualKeyA` | `MessageBeep` |
| `OffsetRect` | `PostQuitMessage` | `PtInRect` |
| `ReleaseDC` | `ScreenToClient` | `SetCursor` |
| `SetCursorPos` | `SetForegroundWindow` | `SetRect` |
| `SetRectEmpty` | `SetWindowPos` | `SetWindowTextA` |
| `ShowCursor` | `SystemParametersInfoA` | `TrackMouseEvent` |
| `TranslateAccelerator` | `TranslateAcceleratorA` | `TranslateMessage` |
| `UnionRect` | `UpdateWindow` |  |

## VERSION.dll

Ressources de version d'un fichier (`VS_FIXEDFILEINFO`).

4 inscriptions, 4 clés distinctes — `src/runtime/win32_shims_version.cpp`

| | | |
|---|---|---|
| `GetFileVersionInfoSizeW` | `GetFileVersionInfoW` | `VerQueryValueA` |
| `VerQueryValueW` |  |  |

## WS2_32.dll

Les mêmes fonctions que WSOCK32.dll, sous des ordinaux différents — les deux DLL ne s'accordent pas sur la numérotation, d'où deux entrées par fonction.

27 inscriptions, 27 clés distinctes — `src/runtime/win32_shims_wsock32.cpp`

| | | |
|---|---|---|
| `#1` accept | `#2` bind | `#3` closesocket |
| `#4` connect | `#5` getpeername | `#6` getsockname |
| `#7` getsockopt | `#8` htonl | `#9` htons |
| `#10` ioctlsocket | `#11` inet_addr | `#12` inet_ntoa |
| `#13` listen | `#14` ntohl | `#15` ntohs |
| `#16` recv | `#19` send | `#21` setsockopt |
| `#22` shutdown | `#23` socket | `#52` gethostbyname |
| `#57` gethostname | `#101` wsaasyncselect | `#111` wsagetlasterror |
| `#112` wsaisblocking | `#115` wsastartup | `#116` wsacleanup |

## WSOCK32.dll

Couche socket complète, traduite vers POSIX : table de poignées, primitives BSD, résolution de noms, ordre des octets. Aucune logique propre à un jeu — ce qu'un portage veut observer ou dévier passe par l'observateur et la route (`wx86_net_set_observer`, `wx86_net_set_redirect`).

27 inscriptions, 27 clés distinctes — `src/runtime/win32_shims_wsock32.cpp`

| | | |
|---|---|---|
| `#1` accept | `#2` bind | `#3` closesocket |
| `#4` connect | `#5` getpeername | `#6` getsockname |
| `#7` getsockopt | `#8` htonl | `#9` htons |
| `#10` inet_addr | `#11` inet_ntoa | `#12` ioctlsocket |
| `#13` listen | `#14` ntohl | `#15` ntohs |
| `#16` recv | `#19` send | `#21` setsockopt |
| `#22` shutdown | `#23` socket | `#52` gethostbyname |
| `#57` gethostname | `#101` wsaasyncselect | `#111` wsagetlasterror |
| `#112` wsaisblocking | `#115` wsastartup | `#116` wsacleanup |

## Unités d'installation

Chaque groupe expose une fonction `install` que le portage appelle depuis son propre binaire de boot. Aucune n'est appelée automatiquement : le moteur ne décide pas à la place du portage quels shims celui-ci veut.

| Fonction | Fichier | Inscriptions |
|---|---|---|
| `win32_shims_advapi32_install` | `src/runtime/win32_shims_advapi32.cpp` | 37 |
| `win32_shims_gdi32_install` | `src/runtime/win32_shims_gdi32.cpp` | 36 |
| `win32_shims_misc_install` | `src/runtime/win32_shims_misc.cpp` | 31 |
| `win32_shims_psapi_install` | `src/runtime/win32_shims_psapi.cpp` | 5 |
| `win32_shims_shell32_install` | `src/runtime/win32_shims_shell32.cpp` | 2 |
| `win32_shims_user32_install` | `src/runtime/win32_shims_user32.cpp` | 23 |
| `win32_shims_version_install` | `src/runtime/win32_shims_version.cpp` | 4 |
| `win32_shims_window_install` | `src/runtime/win32_shims_window.cpp` | 33 |
| `win32_shims_wsock32_install` | `src/runtime/win32_shims_wsock32.cpp` | 54 |

