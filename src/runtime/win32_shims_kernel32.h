// src/runtime/win32_shims_kernel32.h — shims KERNEL32 generiques : aucun
// litteral, chemin ou comportement propre a un jeu donne. Inscrits via
// Bridge::register_shim, le meme mecanisme qu'un consommateur utilise pour
// en redefinir un pour son propre jeu (semantique d'ecrasement sur cle
// dupliquee, cf. bridge.h).
#pragma once
namespace d2rt { class Bridge; }

void win32_shims_kernel32_install(d2rt::Bridge& br);
