---
name: doc-update
description: Use after a change that moves the engine/port boundary, adds or removes an extension point, or closes a known limitation — to bring docs/ back in line with the code. Verifies facts against the code and git log rather than restating a summary, and knows which pages are generated and must not be hand-edited.
---

# Mettre la doc à jour après un changement

La doc de ce dépôt décrit une **frontière** (ce qui est du moteur, ce qui est du
portage) et des **points d'extension**. Ce sont exactement les deux choses qu'un
chantier déplace. Une doc qui dit « le moteur ne fournit aucun shim » alors que
le moteur en fournit deux cents n'est pas incomplète, elle est **fausse** — et
elle induit en erreur quelqu'un qui prendrait une décision dessus.

## La règle : vérifier, jamais recopier

Ne rédige **jamais** depuis le résumé de quelqu'un — pas même un message de
commit. Va lire :

- le code concerné, pour ce qui existe réellement aujourd'hui ;
- `git log` / `git show`, pour ce qui a changé et quand ;
- `docs/shims.md`, qui est **généré** et donc toujours exact.

Un chiffre (nombre de lignes, d'inscriptions, de fichiers) se **re-mesure** avant
d'être écrit. Un agent de ce projet s'est fait prendre à annoncer un décompte de
lignes repris d'un état intermédiaire : le fichier avait en réalité grossi.

## Les pages générées : ne pas éditer à la main

`docs/shims.md` est produit par `tools/gen_shim_list.py` et vérifié par la CI
(`--check`). Si son contenu est faux, le défaut est dans le **générateur** ou
dans les sources, pas dans la page. La corriger à la main casse la CI au commit
suivant, ce qui est la preuve que le garde-fou marche.

Après tout déplacement de shims : `tools/gen_shim_list.py`, et commiter la page
avec le code.

## Ce qu'il faut relire après quoi

| le changement | les pages à revérifier |
|---|---|
| des shims changent de dépôt | `docs/shims.md` (régénérer), `extension.md`, `architecture.md`, `dette.md` |
| un nouveau point d'extension | `extension.md` en premier, puis `architecture.md` |
| une limitation connue est levée | `dette.md` — **la retirer**, pas l'annoter |
| un outil ajouté ou modifié | `outils-ia.md` |
| un renommage de variables d'env | `dette.md`, et vérifier qu'aucune page ne cite l'ancien nom |

## Les pièges propres à cette doc

**`dette.md` ment par accumulation.** C'est la page qui pourrit le plus vite :
elle liste ce qui n'est pas fait, donc chaque chantier terminé devrait en retirer
une entrée — et personne n'y pense. Avant d'y ajouter quoi que ce soit, **vérifie
que chaque entrée existante est encore vraie.** Une dette déjà payée qui traîne
fait croire à un chantier restant.

**Une affirmation absolue vieillit mal.** « winx86 ne contient aucun shim » était
vrai, et l'est resté écrit un jour de trop. Préfère une formulation qui reste
juste quand le nombre bouge, ou un renvoi vers la page générée.

**Un lien vers une ancre disparue passe `--strict` sans broncher.** MkDocs
vérifie les fichiers, pas les fragments. Si tu renommes une section vers laquelle
une autre page pointe par `#ancre`, corrige les deux côtés à la main.

## Validation

- `mkdocs build --strict` doit passer (installe `mkdocs-material` dans un venv si
  besoin) ;
- `tools/gen_shim_list.py --check` doit sortir 0 ;
- aucun lien mort ; vérifie aussi les liens **externes** que tu ajoutes, une URL
  inventée est un mensonge silencieux — une page de ce dépôt a pointé pendant des
  semaines vers un domaine qui n'a jamais existé.

## Ce qu'on ne met pas ici

Pas de journal daté, pas de récit de session, pas de « ce qui a été fait ce
soir ». La doc dit ce que le moteur **est** ; l'historique est dans `git log`, et
dans `historique.md` pour ce qui relève de la provenance. Un journal dans une doc
publique se périme, et personne ne le retire.
