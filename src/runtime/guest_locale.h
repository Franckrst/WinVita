// src/runtime/guest_locale.h — la locale que le programme invite croit voir.
//
// DECOUPE. La *correspondance* LCID -> page de codes, nom de langue, separateur
// decimal... est de la connaissance Windows : elle ne depend d'aucun jeu, et les
// deux portages en portaient la meme copie, table pour table. Elle vit donc ici.
// *Quelle* locale est active depend de la machine et du consommateur : il la
// pose avec wx86_locale_set_lcid(). Par defaut le moteur repond en-US (0x0409),
// jamais une valeur choisie pour un jeu particulier.
//
// wx86_locale_detect_posix() est offert comme defaut raisonnable pour un hote
// POSIX ; une console qui expose sa propre langue systeme n'a qu'a poser le
// LCID elle-meme.
#pragma once
#include <cstdint>

// LCID actif. Defaut 0x0409 (en-US) tant que le consommateur n'a rien pose.
uint32_t wx86_locale_lcid();
void     wx86_locale_set_lcid(uint32_t lcid);

// Locale de l'hote POSIX, deduite de LC_ALL / LC_MESSAGES / LANG.
// Rend 0x0409 pour « C »/« POSIX »/absent.
uint32_t wx86_locale_detect_posix();

// Pages de codes ANSI et OEM de la locale active.
uint32_t wx86_cp_ansi();
uint32_t wx86_cp_oem();

// Valeur texte d'un LCTYPE (GetLocaleInfoA/W). Jamais nul.
const char* wx86_locale_info(uint32_t lctype);

// Classification de caractere facon GetStringType (CT_CTYPE1).
uint16_t wx86_ctype1(uint32_t ch);
