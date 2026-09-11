// src/runtime/win32_shims_sync.h — shims KERNEL32 de synchronisation
// (evenement, mutex, semaphore) et lecture du code de sortie d'un fil. Aucun
// litteral ni comportement propre a un jeu : de la semantique Win32.
// L'instrumentation d'un portage se branche par wx86_sync_set_observer()
// (guest_sync.h), elle n'a pas a vivre dans ces corps.
#pragma once
namespace d2rt { class Bridge; }

void win32_shims_sync_install(d2rt::Bridge& br);
