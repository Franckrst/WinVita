# Skills winx86

Skills locaux au dépôt, chargés automatiquement par Claude Code quand on
travaille ici. Ils décrivent le **moteur** : faire tourner du code x86 sur ARM,
l'observer, le mesurer honnêtement.

Ce qui relève d'un **jeu précis** — lire ses archives, reconnaître ses formats,
piloter sa boucle de jeu — n'a rien à faire ici et vit dans le dépôt du portage.
C'est la même frontière que pour le code : si un skill ne sert qu'à un seul
consommateur, il est chez lui.

## Le moteur qui tourne

| Skill | Objet |
|---|---|
| [`runtime-debugging`](runtime-debugging/SKILL.md) | La boucle de base : reproduire sur bureau d'abord, l'échelle de validation à 3 niveaux, l'attribution par compteurs, résoudre un EIP invité, la porte d'identité octet pour octet |
| [`guest-code-hooks`](guest-code-hooks/SKILL.md) | Intercepter / sonder / remplacer du x86 invité à l'exécution — la couture jmp 5 octets, la comptabilité ESP/adresse de retour, le remplacement natif |
| [`intermittent-corruption`](intermittent-corruption/SKILL.md) | Fautes non déterministes : l'échelle d'élimination (entrée → traduction → flot de contrôle → interruption → recouvrement mémoire) et quand la réponse est « c'est un bug latent de l'invité » |
| [`dynarec-internals`](dynarec-internals/SKILL.md) | Fautes dans l'émulateur lui-même : état CPU par fil, préemption par budget de bloc, coût de synchro du cache d'instructions, contrat attente/réveil |

## Mesurer et prouver

| Skill | Objet |
|---|---|
| [`instrument-verification`](instrument-verification/SKILL.md) | Avant de croire un compteur : le **prouver statiquement** avant d'y passer du temps machine. Pièges d'adresse de retour, décalage ELF↔exécution, compteur masqué par un état instantané, sites statiques ≠ appels exécutés, critère de décision écrit AVANT la mesure |
| [`hardware-ab-bench`](hardware-ab-bench/SKILL.md) | Mesurer une perf sur du vrai matériel : **quel banc selon l'ordonnanceur**, la métrique auto-normalisée, le garde-fou de validité, et les pièges qui font mentir un chiffre matériel |

## Plateforme Vita

| Skill | Objet |
|---|---|
| [`vitasdk-build`](vitasdk-build/SKILL.md) | Chaîne VitaSDK, pipeline elf→velf→self→eboot→vpk, erreurs de build courantes |
| [`vita3k-testing`](vita3k-testing/SKILL.md) | Démarrer un VPK en local dans Vita3K sans interaction — recette, pièges, lecture du journal |
| [`vita-perf-budget`](vita-perf-budget/SKILL.md) | Budgets CPU/RAM/VRAM/E-S sur l'appareil — ce qui tient, et pourquoi la console est plus lente que les fiches techniques |

## Chargement

Claude Code découvre seul `.claude/skills/*/SKILL.md` et fait correspondre les
descriptions à la tâche en cours. Aucune invocation manuelle n'est nécessaire.

## Voir aussi

`.claude/agents/` — les agents, qui décrivent des **procédures** complètes
(découper un groupe de shims, mettre la doc à jour) là où les skills décrivent
un **domaine**.
